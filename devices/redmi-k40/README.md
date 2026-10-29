# Xiaomi Mi 10 (UMI, Snapdragon 865)

This device port follows unified upstream `88513176b1e96ecf0b764cd62c6f4e6ac71345e3`.
The previous UMI kernel and userland history is retained by the integration merge.

## Source isolation

All UMI implementations live here. Shared upstream sources in `xnu/files/`,
`userland/src/`, `wifi/lx/`, the shared loader, and `xnu/hakios.patch` remain
unchanged. `upstream-templates.sha256` records their contents;
`python3 devices/redmi-k40/prepare.py verify` checks that invariant.

`overlays/xnu/files/` contains the UMI-specific driver variants and helpers.
`xnu-device.patch` adds their build entries and startup calls to the generated
Apple source tree; it is applied after upstream's patch. It also retains the
32 MiB peripheral mapping window required by the working UMI port.

`overlays/wifi/` selects QCA6390 PCIe/MHI and retains UMI authentication fixes.
`overlays/userland/` holds UMI firmware packaging, Bluetooth startup, and its
controller compatibility library. The build scripts stage these into private
copies under `out/redmi-k40/`; they do not replace upstream templates.

The loader and UEFI source port are likewise device-local (`loader/`, `uefi/`).
The supplied device tree is `prebuilt/device-tree.dtb`. Firmware, generated Apple
identity headers, build output, captures, and credentials remain outside git.

## Building

Set `THEOS` to a toolchain directory containing `sdks/iPhoneOS16.5.sdk`; prepare
XNU's dependencies with upstream `xnu-deps/setup.sh` when needed. Keep your own
extracted iOS files in `sysimage/`, and the phone's firmware in the ignored
`fw-gpu/`, `fw-audio/`, `fw-haptic/`, `fw-wifi/`, and `fw-bluetooth/` directories.
UMI Bluetooth needs `htbtfw20.tlv` and `htnv20.bin`; its firmware is embedded only
in the generated kernel tree. No vendor firmware is committed here.

```sh
devices/redmi-k40/build-wifi.sh
devices/redmi-k40/build-kernel.sh -j8 -k
devices/redmi-k40/build-loader.sh
devices/redmi-k40/build-userland.sh
```

The kernel is `out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
The images and loader are `out/redmi-k40/{root.apfs,system.apfs,loader.efi}`.
Build or supply a UMI UEFI image implementing the existing XNUP hook:
`UEFI_FD=/path/to/SILICIUM_UEFI.fd`, with the device-local UEFI source in `uefi/`.
The UMI FD is 3 MiB at `0x9fc00000`; packaging rejects another model's DTB.

## RAM boot

```sh
devices/redmi-k40/run.sh reload
devices/redmi-k40/run.sh ramdisk
XL_SECS=1800 devices/redmi-k40/run.sh run
```

`run.sh` uses a generated, device-selected copy of upstream `xl`. It shares the
phone lock with the earlier UMI workspace and refuses to fall back to a POCO
kernel or loader. Nothing is flashed. The default arguments are in `boot.args`;
they keep touch, keys, haptics, audio, GPU, scan synchronization, Wi-Fi, Bluetooth,
and all eight cores. Stock maximum GPU frequency remains 587 MHz.

For live USB logs, use `bootcpu=7 cpus=7` instead of the eight-core pair in
`boot.args`. `XL_FOLLOW=5 XL_FOLLOW_KEEP=1` retains timing snapshots. `thermal=2`
is an optional comparison mode: CPU caps are not applied, while sensors,
pressure reporting and the emergency shutdown stay active. It is not a default.

The upstream compositor currently uses OpenGL ES through Mesa. This update
follows upstream's unfinished Metal status; it does not claim a Metal compositor.
The UMI display variant retains the validated staging/cache fixes and releases
source ownership after staging so rendering can overlap panel presentation.

The physical panel buffer and the CPU staging buffer each hold a complete
1080 x 2340 frame (10,108,800 bytes with the bootloader's stride). Increasing
their dimensions adds memory traffic. Command-mode copies instead use the full
measured DMA idle interval, subject to the copy duration plus a safety margin,
rather than accepting frames only in its first 128 lines (about 0.9 ms).
The DMA completion is checked between two phase reads to reject refresh wraps.
The wait sleeps through the known active scan interval and polls only near the
idle boundary, rather than spinning a CPU core throughout the wait.

## Validation

The kernel, Wi-Fi object, Mesa, userland images and UMI loader build. The actual
scan-reader register-model test passes (`python3 devices/redmi-k40/tools/test-display.py`).
The template checksum test passes. The Android RAM-boot package builds with the
supplied UMI device tree and UMI UEFI layout. The updated kernel and matching
iOS images booted on UMI; touch initialized, the GPU rendered apps at its
587 MHz setting, and the audio DSP and Wi-Fi interface initialized. The user
confirmed that the build works, while scrolling skips/freezes remain. This
source migration does not claim to resolve that remaining performance issue.

## Microphone

`mic=4` (with `adsp=1 adsp_umi=1`) gives iOS the bottom microphone: HakiosAudio.driver's "Codec"
input, which VirtualAudio makes the built-in "iPhone Microphone". The kernel side,
`overlays/xnu/files/bsd/dev/hakios.mic.c`, is generated from the upstream driver by
`tools/patch-mic-alioth.py` with kona's values: WCD938x reset on GPIO32 (TLMM south tile),
three LPASS SoundWire pins, a v1.5.1 TX SoundWire master, bolero 1.x, Xiaomi's main-mic path
(ADC1, "ADC1 Volume" 20, TX SMIC MUX0 ADC0, decimator 0, TX_CDC_DMA_TX_3) and sm8250.dtsi's
port 2 timing (sample interval 1, offset 1, lane 0). `mic=1..3` are the bring-up levels
(look; codec enumerated; `mic_secs` of listening, with `rec_sound=N rec_sound_mic=1` keeping
the samples for `xl sound` / `tools/sound.py`).

Measured on the phone: the codec enumerates (0x000d01170223); a 2 s on / 2 s off 1 kHz beep
from a nearby PC shows in the kernel's recording 35 dB above its absence; a tone from the
phone's own speaker 29.5 dB quieter arrives 28 dB quieter (linear); `record_test`
(AVAudioRecorder) gets 6.00 s of 6.00 and Voice Memos records with a moving waveform. iOS's
input applies automatic gain: room noise at -53 dBFS in the kernel is about -22 dBFS in an
application. The test tools need `ldid` at build time for their entitlements (without it
`record_test` is refused the microphone and `battery_test` cannot open URLs).
