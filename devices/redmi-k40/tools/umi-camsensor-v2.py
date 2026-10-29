#!/usr/bin/env python3
"""Inspect UMI Qualcomm CamX Parameter Parser V2 camera modules.

The upstream camsensor.py handles Parser V3. UMI's vendor modules use V2,
whose symbol records and register-setting entries are 64-bit. This device-local
reader reports power sequences, stream dimensions, and mode register tables.
For the main S5KHMX sensor it can export RWCTAB02 tables for an explicitly
selected sensor version. Keep generated tables outside git, like the module.

Usage: umi-camsensor-v2.py MODULE.bin
       umi-camsensor-v2.py MODULE.bin --sensor-version 3 --output OUT.tab
"""
import argparse
import struct
import sys
from pathlib import Path


HEADER = b"QTI Chromatix Header"
SYMBOL_SIZE = 72
SYMBOL_NAME_SIZE = 40
RESOLUTION_SIZE = 0x130
REGSETTING_SIZE = 80


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def load_module(path):
    data = Path(path).read_bytes()
    if not data.startswith(HEADER) or b"Parameter Parser V2.0.0" not in data[:0x80]:
        raise ValueError("not a Qualcomm CamX Parameter Parser V2.0.0 module")
    directory, count = struct.unpack_from("<II", data, 0x98)
    if directory != 0xA0 or count != 3:
        raise ValueError("unsupported V2 section directory")
    sections = [struct.unpack_from("<QQQ", data, directory + i * 24) for i in range(count)]
    if [section[0] for section in sections] != [0, 1, 2]:
        raise ValueError("unexpected V2 section types")
    symbols_offset, symbols_size = sections[0][1:]
    pool_offset, pool_size = sections[1][1:]
    if symbols_size % SYMBOL_SIZE or symbols_offset + symbols_size > len(data) or pool_offset + pool_size > len(data):
        raise ValueError("invalid V2 section bounds")

    symbols = {}
    count = symbols_size // SYMBOL_SIZE
    for i in range(count):
        offset = symbols_offset + i * SYMBOL_SIZE
        sid = u64(data, offset)
        name = data[offset + 8:offset + 8 + SYMBOL_NAME_SIZE].split(b"\0", 1)[0].decode("latin1")
        kind, ref, size = struct.unpack_from("<QQQ", data, offset + 48)
        if sid in symbols:
            raise ValueError(f"duplicate symbol id {sid}")
        if ref > pool_size or size > pool_size - ref:
            raise ValueError(f"symbol {sid} ({name}) lies outside the data section")
        symbols[sid] = {"name": name, "kind": kind, "ref": ref, "size": size}
    return data, pool_offset, pool_size, symbols


def value(data, pool, symbol):
    return data[pool + symbol["ref"]:pool + symbol["ref"] + symbol["size"]]


def named(symbols, name):
    return [(sid, symbol) for sid, symbol in symbols.items() if symbol["name"] == name]


def as_words(blob):
    if len(blob) % 8:
        raise ValueError("expected a 64-bit word array")
    return struct.unpack(f"<{len(blob) // 8}Q", blob)


def print_power(data, pool, symbols):
    power_names = {
        0: "MCLK", 1: "VANA", 2: "VDIG", 3: "VIO", 4: "VAF",
        5: "VAF_PWDM", 6: "CUSTOM1", 7: "CUSTOM2", 8: "CUSTOM3", 9: "RESET", 10: "STANDBY",
    }
    items = named(symbols, "powerSetting")
    for index, (sid, symbol) in enumerate(items[:2]):
        blob = value(data, pool, symbol)
        if len(blob) % 24:
            print(f"  powerSetting[{index}] (symbol {sid}): unexpected {len(blob)}-byte layout")
            continue
        records = [struct.unpack_from("<3Q", blob, offset) for offset in range(0, len(blob), 24)]
        print(f"  powerSetting[{index}] (symbol {sid}): " + " ".join(
            f"{power_names.get(kind, f'type{kind}')}={setting:#x}+{delay}ms" for kind, setting, delay in records
        ))


def probe_fields(data, pool, symbols):
    drivers = named(symbols, "sensorDriverData")
    if not drivers:
        return
    driver = value(data, pool, drivers[0][1])
    if len(driver) % 8:
        return
    fields = as_words(driver)
    names = {sid for sid, _ in named(symbols, "sensorName")}
    # Optional fields precede sensorName in some UMI modules. Locate its
    # reference instead of assuming the main sensor's fixed field offset.
    name_index = next((i for i, word in enumerate(fields) if word in names), None)
    if name_index is None or len(fields) < name_index + 12:
        return
    first = name_index + 1
    slave, address_bytes, data_bytes, id_address, chip_id, id_mask = fields[first:first + 6]
    if not (0 < slave <= 0xff and address_bytes in (1, 2, 3, 4) and data_bytes in (1, 2, 3, 4)
            and id_address <= 0xffff and chip_id <= 0xffff):
        return
    return fields, first


def print_probe(data, pool, symbols):
    probe = probe_fields(data, pool, symbols)
    if probe is None:
        return
    fields, first = probe
    slave, address_bytes, data_bytes, id_address, chip_id, id_mask = fields[first:first + 6]
    print(f"  probe: CamX slave {slave:#x} (CCI SID {slave >> 1:#x}), {address_bytes}-byte register, "
          f"{data_bytes}-byte value; ID {id_address:#06x}={chip_id:#06x}, mask {id_mask:#x}")
    for label, count_word, symbol_word in (("power-up", first + 7, first + 8), ("power-down", first + 9, first + 10)):
        count, sid = fields[count_word], fields[symbol_word]
        symbol = symbols.get(sid)
        if symbol and symbol["name"] == "powerSetting":
            print(f"  {label}: {count} steps in powerSetting symbol {sid}")


def resolution_modes(data, pool, symbols, resolution):
    blob = value(data, pool, resolution)
    if len(blob) % RESOLUTION_SIZE:
        raise ValueError(f"unsupported {len(blob)}-byte resolutionData layout")
    modes = []
    for offset in range(0, len(blob), RESOLUTION_SIZE):
        words = struct.unpack_from("<38Q", blob, offset)
        stream_count, stream_id = words[1:3]
        if not 0 < stream_count <= 8 or not 0 < words[11] <= 4 or words[15] not in (0, 1):
            raise ValueError(f"mode at {offset:#x}: unsupported stream or lane configuration")
        stream_symbol = symbols.get(stream_id)
        table_count, table_id = words[18:20]
        table_symbol = symbols.get(table_id)
        if not stream_symbol or stream_symbol["name"] != "streamConfiguration":
            raise ValueError(f"mode at {offset:#x}: invalid streamConfiguration reference")
        if stream_symbol["size"] != stream_count * 64:
            raise ValueError(f"mode at {offset:#x}: stream count does not match its array")
        if not table_symbol or table_symbol["name"] != "regSetting" or table_symbol["size"] != table_count * REGSETTING_SIZE:
            raise ValueError(f"mode at {offset:#x}: register count does not match its array")
        streams = []
        raw_streams = value(data, pool, stream_symbol)
        for stream_offset in range(0, len(raw_streams), 64):
            stream = struct.unpack_from("<8Q", raw_streams, stream_offset)
            streams.append({"vc": stream[0], "data_type": stream[1], "x": stream[2], "y": stream[3],
                            "width": stream[4], "height": stream[5], "bit_width": stream[6]})
        modes.append({"streams": streams, "cfa": words[0], "line_length": words[3], "frame_length": words[4],
                      "pixel_clock": words[7], "fps": struct.unpack_from("<d", blob, offset + 80)[0],
                      "lanes": words[11], "settle": words[14], "cphy": words[15], "table_id": table_id})
    return modes


def table_entries(data, pool, symbols, sid):
    symbol = symbols.get(sid)
    if not symbol or symbol["name"] != "regSetting" or symbol["size"] % REGSETTING_SIZE:
        return None
    entries = []
    for offset in range(0, symbol["size"], REGSETTING_SIZE):
        words = struct.unpack_from("<10Q", data, pool + symbol["ref"] + offset)
        has_slave, slave_sid, address, count, data_sid, address_bytes, data_bytes, operation, has_delay, delay_sid = words
        data_symbol = symbols.get(data_sid)
        delay_symbol = symbols.get(delay_sid) if has_delay else None
        if not data_symbol or data_symbol["name"] != "registerData":
            return None
        values = as_words(value(data, pool, data_symbol))
        if count > len(values):
            return None
        delay = 0
        if delay_symbol:
            delays = as_words(value(data, pool, delay_symbol))
            delay = delays[0] if delays else 0
        entries.extend((address, register_value, address_bytes, data_bytes, operation, delay) for register_value in values[:count])
    return entries


def export_main(data, pool, symbols, sensor_name, version, output, source):
    if sensor_name != "s5khmx":
        raise ValueError("table export currently supports only UMI's main s5khmx module")
    probe = probe_fields(data, pool, symbols)
    if probe is None:
        raise ValueError("unrecognized sensor probe layout")
    driver, first = probe

    def array(name, record_words):
        refs = [(i, symbols[word]) for i, word in enumerate(driver) if word in symbols and symbols[word]["name"] == name]
        if len(refs) != 1 or refs[0][0] == 0:
            raise ValueError(f"ambiguous {name} reference in sensorDriverData")
        index, symbol = refs[0]
        blob = value(data, pool, symbol)
        if len(blob) != driver[index - 1] * record_words * 8:
            raise ValueError(f"{name}: count does not match array size")
        return [struct.unpack_from(f"<{record_words}Q", blob, offset)
                for offset in range(0, len(blob), record_words * 8)]

    variants = {}
    for has_version, version_id, count, resolution_id in array("resolutionInfo", 4):
        version_symbol = symbols.get(version_id)
        resolution = symbols.get(resolution_id)
        if has_version != 1 or not version_symbol or version_symbol["name"] != "sensorVersion":
            raise ValueError("unsupported resolution version selector")
        if not resolution or resolution["name"] != "resolutionData":
            raise ValueError("invalid resolutionData reference")
        variant = as_words(value(data, pool, version_symbol))
        if len(variant) != 1 or variant[0] in variants:
            raise ValueError("ambiguous sensorVersion")
        modes = resolution_modes(data, pool, symbols, resolution)
        if len(modes) != count:
            raise ValueError("resolutionInfo: mode count mismatch")
        variants[variant[0]] = modes
    if version not in variants:
        raise ValueError(f"sensor version {version:#x} is absent; available: {', '.join(hex(v) for v in variants)}")
    modes = variants[version]
    init = [(count, sid) for selected, count, sid in array("initSettings", 3) if selected == version]
    if len(init) != 1:
        raise ValueError("sensor version must select exactly one initialization table")

    def settings(sid, expected=None):
        symbol = symbols.get(sid)
        if not symbol or symbol["name"] != "regSetting" or symbol["size"] % REGSETTING_SIZE:
            raise ValueError(f"invalid register table {sid}")
        if expected is not None and symbol["size"] != expected * REGSETTING_SIZE:
            raise ValueError(f"register table {sid}: count mismatch")
        for offset in range(0, symbol["size"], REGSETTING_SIZE):
            words = struct.unpack_from("<10Q", data, pool + symbol["ref"] + offset)
            if words[0] or words[3] != 1 or words[7] != 0:
                raise ValueError(f"register table {sid}: unsupported slave override, data count or operation")
            if words[8] and (words[9] not in symbols or symbols[words[9]]["name"] != "delayUs"):
                raise ValueError(f"register table {sid}: invalid delay reference")
        entries = table_entries(data, pool, symbols, sid)
        if entries is None:
            raise ValueError(f"cannot decode register table {sid}")
        return entries

    tables = [(f"res{i}", settings(mode["table_id"])) for i, mode in enumerate(modes)]
    tables.append(("init", settings(init[0][1], init[0][0])))
    gain_refs = [i for i, word in enumerate(driver) if word in symbols and symbols[word]["name"] == "regToRealGain"]
    if len(gain_refs) != 1 or gain_refs[0] + 8 >= len(driver):
        raise ValueError("unrecognized stream/group-hold settings layout")
    for i, name in enumerate(("on", "off", "holdon", "holdoff")):
        count, sid = driver[gain_refs[0] + 1 + 2 * i:gain_refs[0] + 3 + 2 * i]
        tables.append((name, settings(sid, count)))

    lanes = named(symbols, "laneAssign")
    if len(lanes) != 1:
        raise ValueError("ambiguous lane assignment")
    lane_assignment, = as_words(value(data, pool, lanes[0][1]))
    out = bytearray(b"RWCTAB02")
    out += struct.pack("<8I", *driver[first:first + 6], lane_assignment, len(modes))
    for mode in modes:
        stream = mode["streams"][0]
        out += struct.pack("<12I", stream["width"], stream["height"], stream["x"], stream["y"], mode["lanes"],
                           mode["cphy"], mode["settle"], mode["pixel_clock"], mode["line_length"], mode["frame_length"],
                           int(mode["fps"] * 1000), stream["data_type"])
    for name, index in (("pwrup", first + 7), ("pwrdn", first + 9)):
        count, sid = driver[index:index + 2]
        symbol = symbols.get(sid)
        if not symbol or symbol["name"] != "powerSetting" or symbol["size"] != count * 24:
            raise ValueError(f"invalid {name} sequence")
        out += name.encode().ljust(8, b"\0") + struct.pack("<I", count)
        blob = value(data, pool, symbol)
        for offset in range(0, len(blob), 24):
            kind, setting, delay = struct.unpack_from("<3Q", blob, offset)
            # Xiaomi's V2 enum inserts CUSTOM3 before RESET. RWCTAB02 uses
            # the upstream 8=RESET, 9=STANDBY numbering. Android's GPIO trace
            # confirms the main module's type 9 drives TLMM 62 (RESET).
            if kind == 8 or kind > 10:
                raise ValueError(f"power type {kind} is not representable in RWCTAB02")
            if kind >= 9:
                kind -= 1
            if kind == 0:
                if setting % 1000:
                    raise ValueError("MCLK must be representable in kHz")
                setting //= 1000
            out += struct.pack("<HHBBBBI", kind, setting, 0, 0, 0, 0, delay * 1000)
    out += struct.pack("<I", len(tables))
    for name, entries in tables:
        out += name.encode().ljust(8, b"\0") + struct.pack("<I", len(entries))
        for address, register_value, address_bytes, data_bytes, operation, delay in entries:
            out += struct.pack("<HHBBBBI", address, register_value, address_bytes, data_bytes, operation, 0, delay)
    destination = Path(output)
    if destination.resolve() == Path(source).resolve():
        raise ValueError("output must differ from the source module")
    destination.write_bytes(out)
    print(f"  exported sensor version {version:#x} to {destination}: {len(out)} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module")
    parser.add_argument("--sensor-version", type=lambda value: int(value, 0))
    parser.add_argument("--output")
    args = parser.parse_args()
    if bool(args.output) != (args.sensor_version is not None):
        parser.error("--output and --sensor-version must be supplied together")
    try:
        data, pool, pool_size, symbols = load_module(args.module)
        sensors = named(symbols, "sensorName")
        sensor_name = value(data, pool, sensors[0][1]).split(b"\0", 1)[0].decode("latin1") if sensors else "unknown"
        lanes = named(symbols, "laneAssign")
        lane_assignment = as_words(value(data, pool, lanes[0][1]))[0] if lanes and lanes[0][1]["size"] >= 8 else None
        print(f"UMI camera module: {sensor_name}; data section {pool_size} bytes")
        print_probe(data, pool, symbols)
        if lane_assignment is not None:
            print(f"  lane assignment: {lane_assignment:#x}")
        print_power(data, pool, symbols)

        for resolution_id, resolution in named(symbols, "resolutionData"):
            modes = resolution_modes(data, pool, symbols, resolution)
            print(f"  resolutionData symbol {resolution_id}: {len(modes)} modes")
            for index, mode in enumerate(modes):
                streams = [f"{s['width']}x{s['height']}@{s['x']},{s['y']}/dt{s['data_type']:#x}" for s in mode["streams"]]
                print(f"    mode {index}: streams {', '.join(streams)}")
                print(f"      {mode['lanes']} {'C-PHY trios' if mode['cphy'] else 'D-PHY lanes'}, settle {mode['settle']}, "
                      f"pixel clock {mode['pixel_clock']}, line {mode['line_length']}, frame {mode['frame_length']}, {mode['fps']:.6f} fps")
                table_id = mode["table_id"]
                entries = table_entries(data, pool, symbols, table_id)
                if entries is not None:
                    print(f"      regSetting symbol {table_id}: {len(entries)} writes; " +
                          " ".join(f"{address:#x}={register_value:#x}" for address, register_value, *_ in entries[:6]))
        if args.output:
            export_main(data, pool, symbols, sensor_name, args.sensor_version, args.output, args.module)
    except (OSError, ValueError, struct.error) as error:
        sys.exit(f"{Path(args.module).name}: {error}")


if __name__ == "__main__":
    main()
