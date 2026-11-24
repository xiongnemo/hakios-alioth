# iOS 16 on the Redmi K40 / POCO F3 (alioth)

English · [中文](README.zh.md)

> **Issues welcome, pull requests not.** Found a bug, have a question, or know how something should be fixed? Please
> open an [issue](https://github.com/xiongnemo/hakios-alioth/issues), and feel free to propose a fix in it, as a
> description or a diff. Because of how this port is developed, pull requests cannot be accepted, so they are
> turned off for this repository. The repository's owner is very busy in real life, so issues are read about once
> every two days: expect a turnaround of roughly 48 hours.

The stock iOS 16.5 (20F66) user space on the Redmi K40 / POCO F3 (`alioth`, Snapdragon 870), running on the
open-source XNU kernel with our own boot loader and drivers. It is a port of [HakiOS](https://github.com/oosexgang/hakios-os)
to this phone, built on the Mi 10 port of the same SoC family
([hackios-xnu-kernel-umi](https://github.com/JopangJovan/hackios-xnu-kernel-umi)). HakiOS's own README, for the
project as a whole and its other phones, is [README.hakios.md](README.hakios.md); this phone's code is in
[devices/redmi-k40/](devices/redmi-k40/).

**Nothing is flashed.** The loader goes into RAM through `fastboot boot`, iOS runs from RAM for one session, and a
reboot brings the phone back to its own Android with its data untouched. iOS sees the phone as an iPhone X
(`iPhone10,3`).

## Screenshots

Taken on the phone with the side button and volume up. Network names and the phone's own addresses are hidden.

### Games

OpenGL ES 1/2/3 games run on the Adreno 650 in their own process, each in a GPU address space of its own.

<p>
  <img src="docs/alioth/screenshots/games/minecraft-title.png" width="420" alt="Minecraft PE 1.2.10: title screen">
  <img src="docs/alioth/screenshots/games/minecraft-world-1.png" width="420" alt="Minecraft: in a world">
  <img src="docs/alioth/screenshots/games/minecraft-world-2.png" width="420" alt="Minecraft: in a world">
</p>

Minecraft PE 1.2.10: title screen, and in a world at 60 fps.

<p>
  <img src="docs/alioth/screenshots/games/terraria-title.png" width="420" alt="Terraria 1.2: title screen">
  <img src="docs/alioth/screenshots/games/terraria-world-1.png" width="420" alt="Terraria: in a world">
  <img src="docs/alioth/screenshots/games/terraria-world-2.png" width="420" alt="Terraria: in a world">
</p>

Terraria 1.2: title screen, and in a world at 60 fps.

<p>
  <img src="docs/alioth/screenshots/games/sims3-title.png" width="420" alt="The Sims 3 Ambitions: title screen">
  <img src="docs/alioth/screenshots/games/sims3-create-a-sim.png" width="420" alt="The Sims 3 Ambitions: Create a Sim">
  <img src="docs/alioth/screenshots/games/sims3-house.png" width="420" alt="The Sims 3 Ambitions: at home">
  <img src="docs/alioth/screenshots/games/sims3-house-2.png" width="420" alt="The Sims 3 Ambitions: at home">
  <img src="docs/alioth/screenshots/games/sims3-town.png" width="420" alt="The Sims 3 Ambitions: the town">
</p>

The Sims 3 Ambitions (OpenGL ES 1, PVRTC textures decoded on the CPU): title screen, Create a Sim, at home, the
town; 62 fps.

### Graphics

<p>
  <img src="docs/alioth/screenshots/graphics/gpu-check-summary.png" width="260" alt="GPU Check: summary">
  <img src="docs/alioth/screenshots/graphics/gpu-check-scenekit.png" width="260" alt="GPU Check: SceneKit and SpriteKit">
</p>

[GPU Check](https://github.com/xiongnemo/ios-gpu-check) tests every graphics capability a game may need, one at a
time: 81 of 83 passed when these were taken; the two left need Metal, which the port did not have then (see What
works for Metal now).

### System and settings

<p>
  <img src="docs/alioth/screenshots/system/settings.png" width="260" alt="Settings">
  <img src="docs/alioth/screenshots/system/about.png" width="260" alt="About">
  <img src="docs/alioth/screenshots/system/battery.png" width="260" alt="Battery">
  <img src="docs/alioth/screenshots/system/display.png" width="260" alt="Display and Brightness">
  <img src="docs/alioth/screenshots/system/wifi.png" width="260" alt="Wi-Fi">
  <img src="docs/alioth/screenshots/system/bluetooth-pairing.png" width="260" alt="Bluetooth pairing">
  <img src="docs/alioth/screenshots/system/bluetooth-connected.png" width="260" alt="Bluetooth connected">
</p>

Settings; About (iOS 16.5 as an iPhone X); Battery, from the phone's fuel gauge; Display & Brightness; Wi-Fi;
Bluetooth pairing with another phone, then connected.

### Accessibility

<p>
  <img src="docs/alioth/screenshots/accessibility/assistivetouch.png" width="260" alt="AssistiveTouch menu">
  <img src="docs/alioth/screenshots/accessibility/control-center.png" width="260" alt="Control Center with the AssistiveTouch button">
  <img src="docs/alioth/screenshots/accessibility/color-filters.png" width="260" alt="Color Filters: grayscale">
  <img src="docs/alioth/screenshots/accessibility/classic-invert.png" width="260" alt="Classic Invert on">
  <img src="docs/alioth/screenshots/accessibility/night-shift.png" width="260" alt="Night Shift">
  <img src="docs/alioth/screenshots/accessibility/auto-brightness.png" width="260" alt="Auto-Brightness">
</p>

AssistiveTouch and its menu; Control Center with the AssistiveTouch button; Color Filters; Classic Invert; Night Shift;
Auto-Brightness. Invert, Color Filters and Night Shift are done by the display engine's colour correction, as on an
iPhone, so screenshots do not show their colours: the screen does.

### Sensors

<p>
  <img src="docs/alioth/screenshots/sensors/sensor-check-light.png" width="260" alt="Ambient light and proximity">
  <img src="docs/alioth/screenshots/sensors/sensor-check-motion.png" width="260" alt="Motion sensors">
  <img src="docs/alioth/screenshots/sensors/sensor-check-power.png" width="260" alt="Battery, thermal state, microphone, cameras">
  <img src="docs/alioth/screenshots/sensors/sensor-check-display.png" width="260" alt="Orientation, volume buttons, display">
  <img src="docs/alioth/screenshots/sensors/sensor-check-touch.png" width="260" alt="Touch and haptics">
</p>

[Sensor Check](https://github.com/xiongnemo/ios-sensor-check) lists every sensor and hardware input an app can
reach, live: ambient light, accelerometer, gyroscope, device motion, battery, thermal state, microphone, cameras,
the display at 121 fps, touch and haptics.

### Apps from elsewhere: a Telegram client

<p>
  <img src="docs/alioth/screenshots/apps/nagram-chat-1.png" width="260" alt="Nagram (a Telegram client): a group chat">
  <img src="docs/alioth/screenshots/apps/nagram-chat-2.png" width="260" alt="Nagram: the same group chat">
</p>

[Nagram](https://github.com/NextAlone/Nagram-iOS), an open-source Telegram client, installed from its unsigned GitHub
release: the installer signs what it installs ad hoc, with the entitlements such an app needs (its app group, iCloud,
keychain), so it opens with its shared data, logs in and chats. Other people's names and pictures are hidden.

### GPS

<p>
  <img src="docs/alioth/screenshots/gps/system-check-gps.png" width="260" alt="System Check: GPS satellites, their signals and the fix">
</p>

[System Check](https://github.com/xiongnemo/ios-system-check)'s GPS tab: the satellites the modem's receiver sees
(GPS, BeiDou, GLONASS, Galileo, QZSS) with their signal strengths, its fix, and the same fix as iOS's location
services give it to apps. The position itself is hidden.

## What works

Each item has been checked on the phone.

| Area | |
|---|---|
| Display | 120 Hz (the panel's TE every 8.2 ms); brightness; dark mode; rotation |
| Accessibility | AssistiveTouch (the button, its menu, Control Center from it); Bold Text; Smart and Classic Invert; Color Filters (grayscale, the colour-blindness filters, tint); Night Shift; Auto-Brightness from the ambient light sensor. The colour effects are made by the display engine's colour correction, as on an iPhone |
| Graphics | The compositor and applications on the Adreno 650 through Mesa (freedreno): OpenGL ES 1, 2 and 3, GLKit, SceneKit, SpriteKit, Core Image; PVRTC textures decoded on the CPU; a GPU address space per process; a hung GPU is reset after about 2 s |
| Metal | Installed apps get a Metal device (this port's own driver, on the Adreno 650): Map draws with Metal at about 60 fps; SceneKit, SpriteKit, Core Image and Flutter run on it; System Check passes 95 of its 98 Metal tests (the one failure: no Metal plug-in published on the accelerator service, by design) |
| Modem and GPS | The external Snapdragon X55 modem is booted by the kernel over PCIe (MHI, Sahara, QRTR, QMI). GPS, BeiDou, GLONASS, Galileo and QZSS fixes go into iOS's location services, for every app; with assistance (network time, XTRA orbits, a Wi-Fi position) the first fix comes in 41–63 s. About shows the IMEI; Settings has its Cellular row. No SIM |
| Input | Touch screen with multitouch, power and volume buttons, vibration |
| Sound | Speakers (CS35L41), microphone (Voice Memos records) |
| Wi-Fi | Joins the network configured at build time, 5 GHz preferred |
| Bluetooth | Discoverable, scan, pairing, Bluetooth audio to speakers and headphones |
| Cameras | Camera.app preview and photos: the main camera (IMX582, autofocus) and the front one (S5K3T2); the ultra-wide (IMX355) or the macro (S5K5E9) as the back camera instead, chosen at boot |
| Flashlight | From Control Center |
| Power | Battery level and charging; the cores' and the GPU's speed follow the load; the panel sleeps with the screen off |
| Sensors | Ambient light, accelerometer, gyroscope, device motion, compass (true north with location) through the sensor DSP |
| Location | The modem's GPS (above), and Wi-Fi positioning through Apple's location service |
| USB | The PC sees an iPhone: pairing ("Trust This Computer"), files over AFC |
| Apps | Unsigned, decrypted IPAs install from the PC at boot or with the on-phone installer; an app's data (a login, say) can be saved to the PC and is put back when it is installed at the next boot |

Apps written for this port: [System Check](https://github.com/xiongnemo/ios-system-check) (GPU Check, Sensor Check and
Motion Test in one, with Metal and GPS tabs) and [Map](https://github.com/xiongnemo/ios-map) (OpenStreetMap). Their IPAs
are on each repository's Releases page.

## Not yet

- The system's own programs (the home screen, the compositor) draw with OpenGL ES, not Metal; Apple's Maps and Measure
  are left out of the system image.
- AssistiveTouch's Home action does nothing yet (its button, menu and Control Center work).
- **Persistence**: iOS runs from RAM, so settings, photos and installed apps are gone after a reboot (an app's own data
  can be kept on the PC, above).
- Cellular with a SIM (calls, SMS, mobile data), NFC, fingerprint, the proximity sensor.
- Power: with the screen off the panel sleeps and the GPU is powered off, but the display engine stays powered.

How each part works, what was measured and what comes next: [docs/alioth/status.md](docs/alioth/status.md).
The optional boot-args (all off by default): [docs/alioth/boot-args.md](docs/alioth/boot-args.md).

## The phone it is developed on

This port is developed and tested on one phone, and parts of it are still fitted to that phone rather than read from
yours:

| | |
|---|---|
| Model | Redmi K40, `M2012K11AC` (China), `hwc=CN`, SKU `nfc` |
| Memory, storage | 12 GB RAM, 256 GB UFS |
| Android on it | PixelOS 17.0 (Android 17, the build of 2026-11-12) |
| Modem firmware | `2.5.c1-gl-21664.3-0307_0159_075a1c585d` (Android's baseband version) |
| `reserved-memory` | as in [`prebuilt/device-tree.dtb`](devices/redmi-k40/prebuilt/device-tree.dtb): `removed_region@80b00000` (0x5300000 long), then `pil_gpu_region@8681a000`, `pil_slpi_region@88c00000`, `pil_adsp_region@8bb00000` among the others |

What is fitted to this phone so far:

- The device tree the boot stage hands the kernel is this phone's (`devices/redmi-k40/prebuilt/device-tree.dtb`), not
  your phone's.
- Three drivers put their firmware at addresses taken from this phone's `reserved-memory`: the GPU's zap shader
  (`pil_gpu_region`, `0x8681a000`), the sensor DSP (`pil_slpi_region`, `0x88c00000`) and the audio DSP
  (`pil_adsp_region`, `0x8bb00000`). Firmware can move these regions: on an 8 GB K40 with HyperOS 1.0 global firmware
  every region from `pil_camera_region` up is `0x5c00000` higher, and the GPU stalled at its start until it was given
  the right address ([#1](https://github.com/xiongnemo/hakios-alioth/issues/1)). Taking them from your own phone is
  planned.

To compare yours, in Android: `adb shell ls /proc/device-tree/reserved-memory/` lists the regions with their
addresses (no root needed); with root, `adb shell su -c 'cat /sys/firmware/fdt' > fdt.bin` gives the whole tree
(`dtc -I dtb -O dts fdt.bin`).

**If you try it on your own phone and something goes wrong, please
[open an issue](https://github.com/xiongnemo/hakios-alioth/issues).** Say which phone it is (model, RAM, region,
Android and firmware version) and what happened, and attach your `reserved-memory` list. Leave out anything personal:
the serial number, IMEI, Wi-Fi names and passwords, Bluetooth and MAC addresses.

## Getting a package you can boot

> The kits: [Releases](https://github.com/xiongnemo/hakios-alioth/releases) (`alioth-kit-<version>.zip`). Built by
> [the kit workflow](.github/workflows/kit.yml) from a tag `kit-v<version>`.

A bootable package has two halves, and only one of them can be published.

**What GitHub Actions builds** (from this repository's sources alone, published on the Releases page as
`alioth-kit-<version>.zip`):

| Folder | What is in it |
|---|---|
| `kernel/` | The XNU kernel with this phone's drivers |
| `userland/` | Our programs and libraries for iOS (OpenGL ES on Mesa, the USB daemon, the IPA installer and the rest), Mesa's OSMesa library, and the apps written for this port |
| `boot/` | The UEFI stage for this phone (Project Silicium) and its boot shim |
| `assemble/` | The scripts that make the other half on your computer, and `start.bat` |

**What is made on your computer**, because it cannot be redistributed: the system image (Apple's iOS files), the
phone's firmware (the vendor's), the loader's iPhone identity (taken from Apple's device tree) and your Wi-Fi password.

What you need: a Redmi K40 / POCO F3 with its bootloader unlocked, USB debugging on and root in Android (Magisk or the
like: the DSPs' firmware, the Bluetooth firmware and the sensors' calibration are readable by root only; nothing is
written to the phone); a PC with Windows 10 or 11 and WSL, or Linux; about 15 GB of free disk space; an Internet
connection. Your phone may differ from the one this is developed on: see
[The phone it is developed on](#the-phone-it-is-developed-on) above.

1. Download the kit from the Releases page and unpack it.
2. With the phone in Android and connected, run `assemble.bat` (or `assemble/assemble.sh` on Linux). It
   - downloads the iOS 16.5 IPSW for the iPhone X (`iPhone10,3`) from Apple's public servers and builds the system
     image from it, without decrypting or circumventing anything;
   - pulls the firmware the drivers need (GPU, audio, Wi-Fi, Bluetooth, sensors, cameras) from the phone's own Android;
   - asks for the Wi-Fi network iOS should join;
   - makes the loader's identity from the IPSW and builds the boot image;
   - writes the package folder, `ios-alioth-k40/`.
3. Run `start.bat` in that folder with the phone in Android. Nothing is flashed; a reboot brings Android back.

Keep the package to yourself: it holds Apple's files, the vendor's firmware and your Wi-Fi password.

## Not in this repository

Apart from the screenshots above, no file of iOS: you build the system image from your own copy of the iOS 16.5
IPSW. No vendor firmware either (it is pulled from your own phone), and no keys, certificates or activation. Signing
in to an Apple account is not possible and not a goal.

## Thanks

[HakiOS](https://github.com/oosexgang/hakios-os) (the kernel, loader and user space this is built on),
[hackios-xnu-kernel-umi](https://github.com/JopangJovan/hackios-xnu-kernel-umi) (the Mi 10 port),
[Project Silicium](https://github.com/Project-Silicium/Mu-Silicium) (the UEFI stage), [ios-alioth](https://github.com/MrMoxing/ios-alioth)
(the first boot package for this phone, which this started from), FFT-infra's QcomXnuUsbDebug (the USB device driver)
and [Mesa](https://mesa3d.org/) (freedreno).
