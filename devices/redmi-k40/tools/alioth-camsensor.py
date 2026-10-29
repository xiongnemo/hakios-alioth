#!/usr/bin/env python3
"""Make the register tables of one of alioth's camera sensors, as the kernel's camera driver reads them
(overlays/xnu/files/bsd/dev/hakios.camera.c, boot-arg cam=2 and up), out of the sensor's module file of
Android's camera stack (/vendor/lib64/camera/com.qti.sensormodule.alioth_<maker>_<sensor>_<camera>.bin of
the phone: the vendor's, kept in fw-camera/ and never in the repository; so is what this makes of it).

usage: alioth-camsensor.py MODULE.bin OUT.tab [--sensor-version N]
       alioth-camsensor.py MODULE.bin -          (lists what would be written)

The module files are CamX "Parameter Parser V2.0.0" files, read by umi-camsensor-v2.py beside this. The
driver data's sensor versions (resolutionInfo) each have their modes and an initialisation table
(initSettings, selected by the same version); without --sensor-version the lowest is taken (the IMX582's
version 0 is the full initialisation, its version 1 a subset of it). After regToRealGain come the stream
on and off tables and the group hold's on and off; after testPatternData the sensor's own colour bars
where it has them.

OUT.tab: "RWCTAB02" as tools/camsensor.py writes it: { slave (8-bit address), address bytes and data bytes
of the id read, id address, id, id mask, lane assignment, number of modes } as 32-bit words; per mode
{ width, height, x, y, lanes, C-PHY, settle, output pixel clock, line length, frame length, frames a second
x 1000, data type }; the power sequences "pwrup" and "pwrdn" as name[8], count and count x { kind u16
(0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 6 CUSTOM1, 8 RESET; Xiaomi's V2 numbering, with CUSTOM3 at 8, RESET 9 and
STANDBY 10, turned into this one), value u16 (MCLK in kHz), 0 u32, delay in us u32 }; the number of
tables, and each table: name[8], count, then count x { address u16, data u16, address bytes u8, data
bytes u8, operation u8, 0 u8, delay in us u32 }. The modes' own tables are res0, res1, ...
"""
import importlib.util
import struct
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location('v2', Path(__file__).with_name('umi-camsensor-v2.py'))
v2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v2)


def settings(data, pool, symbols, sid, expected=None):
    symbol = symbols.get(sid)
    if not symbol or symbol['name'] != 'regSetting' or symbol['size'] % v2.REGSETTING_SIZE:
        raise ValueError(f'invalid register table {sid}')
    if expected is not None and symbol['size'] != expected * v2.REGSETTING_SIZE:
        raise ValueError(f'register table {sid}: count mismatch')
    for offset in range(0, symbol['size'], v2.REGSETTING_SIZE):
        words = struct.unpack_from('<10Q', data, pool + symbol['ref'] + offset)
        if words[0] or words[7] != 0:
            raise ValueError(f'register table {sid}: a slave override or an operation not known here')
        if words[8] and (words[9] not in symbols or symbols[words[9]]['name'] != 'delayUs'):
            raise ValueError(f'register table {sid}: invalid delay reference')
    entries = v2.table_entries(data, pool, symbols, sid)
    if entries is None:
        raise ValueError(f'cannot decode register table {sid}')
    return entries


def export(path, output, version=None):
    data, pool, pool_size, symbols = v2.load_module(path)
    probe = v2.probe_fields(data, pool, symbols)
    if probe is None:
        raise ValueError('unrecognized sensor probe layout')
    driver, first = probe

    def array(name, record_words):
        refs = [(i, symbols[w]) for i, w in enumerate(driver) if w in symbols and symbols[w]['name'] == name]
        if len(refs) != 1 or refs[0][0] == 0:
            raise ValueError(f'ambiguous {name} reference in sensorDriverData')
        index, symbol = refs[0]
        blob = v2.value(data, pool, symbol)
        if len(blob) != driver[index - 1] * record_words * 8:
            raise ValueError(f'{name}: count does not match array size')
        return [struct.unpack_from(f'<{record_words}Q', blob, o) for o in range(0, len(blob), record_words * 8)]

    variants = {}
    for has_version, version_id, count, resolution_id in array('resolutionInfo', 4):
        resolution = symbols.get(resolution_id)
        if not resolution or resolution['name'] != 'resolutionData':
            raise ValueError('invalid resolutionData reference')
        key = 0
        if has_version:
            vs = symbols.get(version_id)
            if not vs or vs['name'] != 'sensorVersion':
                raise ValueError('invalid sensorVersion reference')
            words = v2.as_words(v2.value(data, pool, vs))
            if len(words) != 1:
                raise ValueError('ambiguous sensorVersion')
            key = words[0]
        if key in variants:
            raise ValueError(f'sensor version {key} twice')
        modes = v2.resolution_modes(data, pool, symbols, resolution)
        if len(modes) != count:
            raise ValueError('resolutionInfo: mode count mismatch')
        variants[key] = modes
    if version is None:
        version = min(variants)
    if version not in variants:
        raise ValueError(f'sensor version {version} is absent; there are {sorted(variants)}')
    modes = variants[version]
    init = [(c, sid) for selected, c, sid in array('initSettings', 3) if selected == version]
    if len(init) != 1:
        raise ValueError('the sensor version must select exactly one initialisation table')

    tables = [(f'res{i}', settings(data, pool, symbols, m['table_id'])) for i, m in enumerate(modes)]
    tables.append(('init', settings(data, pool, symbols, init[0][1], init[0][0])))
    gain = [i for i, w in enumerate(driver) if w in symbols and symbols[w]['name'] == 'regToRealGain']
    if len(gain) != 1:
        raise ValueError('unrecognized stream/group-hold layout')
    for i, name in enumerate(('on', 'off', 'holdon', 'holdoff')):
        count, sid = driver[gain[0] + 1 + 2 * i:gain[0] + 3 + 2 * i]
        tables.append((name, settings(data, pool, symbols, sid, count) if count else []))
    tpg = [i for i, w in enumerate(driver) if w in symbols and symbols[w]['name'] == 'testPatternData']
    if len(tpg) == 1:
        blob = v2.value(data, pool, symbols[driver[tpg[0]]])
        # testPatternData: { the pattern's kind, its table's count, its table } per pattern; the colour bars' first
        recs = [struct.unpack_from('<3Q', blob, o) for o in range(0, len(blob) - len(blob) % 24, 24)]
        for kind, count, sid in recs:
            if count and sid in symbols and symbols[sid]['name'] == 'regSetting':
                try:
                    tables.append(('tpgon', settings(data, pool, symbols, sid, count)))
                except ValueError:
                    pass
                break

    lanes = v2.named(symbols, 'laneAssign')
    if len(lanes) != 1:
        raise ValueError('ambiguous lane assignment')
    lane_assignment, = v2.as_words(v2.value(data, pool, lanes[0][1]))
    out = bytearray(b'RWCTAB02')
    out += struct.pack('<8I', *driver[first:first + 6], lane_assignment, len(modes))
    for m in modes:
        s = m['streams'][0]
        out += struct.pack('<12I', s['width'], s['height'], s['x'], s['y'], m['lanes'], m['cphy'], m['settle'], m['pixel_clock'],
                           m['line_length'], m['frame_length'], int(round(m['fps'] * 1000)), s['data_type'])
    power = []
    for name, index in (('pwrup', first + 7), ('pwrdn', first + 9)):
        count, sid = driver[index:index + 2]
        symbol = symbols.get(sid)
        if not symbol or symbol['name'] != 'powerSetting' or symbol['size'] != count * 24:
            raise ValueError(f'invalid {name} sequence')
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', count)
        blob = v2.value(data, pool, symbol)
        steps = []
        for o in range(0, len(blob), 24):
            kind, setting, delay = struct.unpack_from('<3Q', blob, o)
            if kind == 8 or kind > 10:
                raise ValueError(f'power type {kind} is not representable in RWCTAB02')
            if kind >= 9:
                kind -= 1
            if kind == 0:
                if setting % 1000:
                    raise ValueError('MCLK must be representable in kHz')
                setting //= 1000
            out += struct.pack('<HHBBBBI', kind, setting, 0, 0, 0, 0, delay * 1000)
            steps.append((kind, setting, delay))
        power.append((name, steps))
    out += struct.pack('<I', len(tables))
    for name, entries in tables:
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(entries))
        for address, value, address_bytes, data_bytes, operation, delay in entries:
            out += struct.pack('<HHBBBBI', address, value, address_bytes, data_bytes, operation, 0, delay)
    print(f'{Path(path).name}: sensor version {version} of {sorted(variants)}; slave {driver[first]:#x}, id {driver[first + 4]:#x} at '
          f'{driver[first + 3]:#x}; lanes {lane_assignment:#x}; {len(modes)} modes; ' +
          ', '.join(f'{n} {len(e)}' for n, e in tables))
    for name, steps in power:
        print(f'  {name}: ' + ' '.join(f'{k}={s}+{d}ms' for k, s, d in steps))
    if output != '-':
        if Path(output).resolve() == Path(path).resolve():
            raise ValueError('the output must differ from the module')
        Path(output).write_bytes(out)


def main():
    args = sys.argv[1:]
    version = None
    if '--sensor-version' in args:
        i = args.index('--sensor-version')
        version = int(args[i + 1], 0)
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    try:
        export(args[0], args[1], version)
    except (OSError, ValueError, struct.error) as error:
        sys.exit(f'{Path(args[0]).name}: {error}')


if __name__ == '__main__':
    main()
