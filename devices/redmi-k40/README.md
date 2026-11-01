# Redmi K40 / POCO F3 (alioth, Snapdragon 870)

The device package for the Redmi K40 / POCO F3. What runs, with screenshots: the [top README](../../README.md). How
each part works and what was measured: [docs/alioth/status.md](../../docs/alioth/status.md).

## Layout

| Path | What |
|---|---|
| `overlays/xnu/files/` | This phone's kernel drivers: display (`bsd/dev/alioth_dsi.c`), GPU (`bsd/dev/hakios.adreno.c`, `iokit/Kernel/hakios.gpu.cpp`), USB (`hakios.dwc3.c`, `hakios.usb.c`), cameras, sensors, audio, modem and the rest. `xnu-device.patch` adds them to the build and to the kernel's start, after upstream's patch. |
| `overlays/userland/` | This phone's user space: `build.sh` (the root disk and the system image), `src/` (OpenGL ES on Mesa in `eagl.c`, the libraries injected into SpringBoard, backboardd and apps, `usbmux.c`, `usbsh.c`, `ipainstall.m`, ...), launchd jobs. |
| `overlays/wifi/` | The Wi-Fi driver's layer for this phone. |
| `loader/` | The boot loader (an EFI application): fastboot over USB, the system image and the payload into RAM, the kernel started on the other cores. |
| `prebuilt/` | The UEFI stage (Project Silicium), its boot shim, the device tree. |
| `boot.args` | The kernel arguments verified together on the phone. |
| `system-apps-removed.txt` | Stock apps left out of the system image, with why. |
| `prepare.py`, `upstream-templates.sha256` | Keep upstream's shared sources unchanged: everything of this phone is an overlay (`prepare.py verify`). |

## Building (Linux or WSL)

You need theos with `sdks/iPhoneOS16.5.sdk` (`THEOS`), XNU's dependencies (`xnu-deps/setup.sh`), clang and lld,
Python 3; your own iOS 16.5 files in `sysimage/` (`sysimage/fetch.sh` downloads the IPSW from Apple's servers); and
this phone's firmware, pulled from its own Android, in the ignored `fw-*/` directories (GPU, audio, Wi-Fi, Bluetooth,
cameras, sensors). `fw-wifi/network` names the network iOS joins (its name on the first line, the passphrase on the
second); `fw-usb/serial` may give your phone's serial, which the loader then shows over USB.

```sh
devices/redmi-k40/build-wifi.sh
devices/redmi-k40/build-kernel.sh -j8 -k
devices/redmi-k40/build-loader.sh
devices/redmi-k40/build-userland.sh
# the boot image: shim + UEFI + loader + device tree
python3 xnuloader/mkpayload.py -o out/redmi-k40/usb-payload.bin loader.efi=out/redmi-k40/loader.efi --usb
UEFI_TARGET=alioth FD_BASE=0x9FC00000 FD_SIZE=0x300000 PAYLOAD_BASE=0xF0000000 \
  UEFI_FD=devices/redmi-k40/prebuilt/SILICIUM_UEFI.fd DTB=devices/redmi-k40/prebuilt/device-tree.dtb \
  SHIM=devices/redmi-k40/prebuilt/BootShim.bin \
  python3 devices/redmi-k40/tools/mkimage.py -p out/redmi-k40/usb-payload.bin -o out/redmi-k40/alioth-xnu.img
# the run payload: kernel, root disk, arguments
python3 xnuloader/mkpayload.py -o out/redmi-k40/run-payload.bin \
  kernel=out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  ramdisk=out/redmi-k40/userland-build/userland/root.apfs --cmdline "$(cat devices/redmi-k40/boot.args)"
```

The system image is `out/redmi-k40/userland-build/userland/system.apfs` (2.4 GB).

## Running (RAM only)

With the phone in its bootloader (`adb reboot bootloader`):

```sh
fastboot boot out/redmi-k40/alioth-xnu.img     # our loader, into RAM
fastboot oem ramdisk
fastboot stage out/redmi-k40/userland-build/userland/system.apfs    # about 97 s
fastboot stage out/redmi-k40/run-payload.bin
fastboot oem run
```

While a run lasts, the loader still answers on its own core: `fastboot getvar result` (has it ended), `fastboot oem log`
then `fastboot get_staged FILE` (the kernel's log), `fastboot oem screenshot` then `get_staged` (the screen),
`fastboot oem stop` (end it). `fastboot reboot` goes back to Android. Nothing is ever written to the phone's storage.

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

## Cameras, flashlight, brightness

`cam=5`: Camera.app on the wide camera (Sony IMX582) through `overlays/xnu/files/bsd/dev/hakios.camera.c`,
whose hardware path is this phone's: the PM8008 on QUP SE16 (GPIO 48/49, reset GPIO 77), PM8150A L2/L3/L5/L7
through RPMh, camcc's Kona clocks and PLL0, CSIPHY 0-4, CSID-lite 0 and IFE-lite 0 (Titan 480) into memory
through the SMMU (streams 0x800/0x840). The sensors' tables come from Android's module files in
`fw-camera/` (`tools/alioth-camsensor.py`; `fw-camera/fitted` names the modules this phone has: Sunny
IMX582, OFilm IMX355, Sunny S5K3T2, Sunny S5K5E9). Bring-up levels: `cam=2 cam_sensor=all` (each sensor's
power and ID), `cam=3` (the CSID's test pattern into memory), `cam=4 cam_sensor=wide cam_mode=N
[cam_pattern=2]` (the sensor's frames, or its own colour bars, into memory: `fastboot oem recording` +
`get_staged`, `tools/camraw.py`). Verified: all four sensors answer with their IDs (the macro's bus needs
PM8150A L2 on, which is voted and left on); the IMX582's colour bars and a scene at 4000x3000; Camera.app's
preview from it with the DW9800 lens motor focusing, and from the front S5K3T2 (the app's switch button).

The iPhone X's Camera.app has a back and a front camera only: `cam_back=ultra` (IMX355) or `cam_back=macro`
(S5K5E9, fixed focus at a few centimetres) makes another sensor its back camera (the frames' header still says
1, `reserved[0]` the sensor itself + 1, by which the media library picks its colour tuning,
`camera-<sensor>.isp` from `tools/alioth-camtuning.py`). Verified: both in Camera.app's preview, with their
own tuning.

`torch=3`: Control Center's flashlight (the PM8150L flash module at SID 5 0xd300, `hakios.torch.c`, at most
125 mA, 30 s at a time). Verified: on and off from Control Center.

Brightness: `AppleARMBacklight` in `MiniIOMobileFramebuffer.cpp` takes CoreBrightness's level (0..2047) to
the panel's DBV by DCS 0x51 (`alioth_dsi.c`), squared (`panel_bl_curve=1`: linear), starting at half of
iOS's scale (a quarter of the panel's light; `panel_bl_init=N`) since the data volume is new at every boot.
Verified: Control Center's slider changes the panel's brightness. Automatic brightness: the ambient light sensor on the sensor DSP (`slpi=1`) feeds CoreBrightness.

## Default arguments (Redmi K40)

`boot.args` is the set verified together on the phone (2026-10-30): the kernel on the prime core with
seven cores (`bootcpu=7 cpus=7`: the loader keeps core 0 and its USB link), touch, keys, haptics, the
audio DSP with speakers and microphone, the GPU with 120 Hz pacing (`fb_vsync=2`), CPU frequency, Wi-Fi
(joining `fw-wifi/network`; `run.sh` adds `wifi_mac=` from `fw-wifi/mac`), Bluetooth, the camera and the
flashlight. The eight-core mode (`bootcpu=0 cpus=8`) froze this phone twice, with and without USB, and is
not used.

Wi-Fi: an access point of the network on 5 GHz is preferred by 15 dB (`lx_80211.c` `preference()`), and
one that turns the station away is skipped for the next: a home network's 2.4 GHz access point refused
authentication (status 1, band steering) while its 5 GHz one accepted. The address in `fw-wifi/mac` should
be the one the router knows -- for a network Android joined, Android's randomized one for it
(Settings > Wi-Fi > the network). `usb=2` adds the kernel's USB serial port (see Microphone/USB notes).
