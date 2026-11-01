# iOS 16 on the Redmi K40 / POCO F3 (alioth)

English · [中文](README.zh.md)

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
time: 81 of 83 pass. The two left need Metal, which this port does not have yet.

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

## What works

Each item has been checked on the phone.

| Area | |
|---|---|
| Display | 120 Hz (the panel's TE every 8.2 ms); brightness; dark mode; rotation |
| Accessibility | AssistiveTouch (the button, its menu, Control Center from it); Bold Text; Smart and Classic Invert; Color Filters (grayscale, the colour-blindness filters, tint); Night Shift; Auto-Brightness from the ambient light sensor. The colour effects are made by the display engine's colour correction, as on an iPhone |
| Graphics | The compositor and applications on the Adreno 650 through Mesa (freedreno): OpenGL ES 1, 2 and 3, GLKit, SceneKit, SpriteKit, Core Image; PVRTC textures decoded on the CPU; a GPU address space per process |
| Input | Touch screen with multitouch, power and volume buttons, vibration |
| Sound | Speakers (CS35L41), microphone (Voice Memos records) |
| Wi-Fi | Joins the network configured at build time, 5 GHz preferred |
| Bluetooth | Discoverable, scan, pairing, Bluetooth audio to speakers and headphones |
| Cameras | Camera.app preview and photos: the main camera (IMX582, autofocus) and the front one (S5K3T2); the ultra-wide (IMX355) or the macro (S5K5E9) as the back camera instead, chosen at boot |
| Flashlight | From Control Center |
| Power | Battery level and charging |
| Sensors | Ambient light, accelerometer, gyroscope, device motion, compass (true north with location) through the sensor DSP |
| Location | Wi-Fi positioning through Apple's location service |
| USB | The PC sees an iPhone: pairing ("Trust This Computer"), files over AFC |
| Apps | Unsigned, decrypted IPAs install from the PC at boot or with the on-phone installer |

Apps written for this port: [GPU Check](https://github.com/xiongnemo/ios-gpu-check),
[Sensor Check](https://github.com/xiongnemo/ios-sensor-check), [Motion Test](https://github.com/xiongnemo/ios-motion-test)
and [Map](https://github.com/xiongnemo/ios-map) (OpenStreetMap; Apple Maps needs Metal). Their IPAs are on each repository's
Releases page.

## Not yet

- **Metal**: apps that need it close at once (many newer 3D games; Apple's Maps and Measure, which need it, are left out
  of the system image). OpenGL ES works.
- AssistiveTouch's Home action does nothing yet (its button, menu and Control Center work).
- **Persistence**: iOS runs from RAM, so settings, photos and installed apps are gone after a reboot.
- GPS, cellular, NFC, fingerprint, the proximity sensor.
- Recovery from a GPU hang: an app that hangs the GPU stops the whole screen until the phone is restarted.
- Power: the display link and the GPU stay on with the screen off.
- Pulling the USB cable while iOS runs: the PC does not see the phone again until the next boot (a fix is being tested).
- Safari cannot pick photos from the library.

How each part works, what was measured and what comes next: [docs/alioth/status.md](docs/alioth/status.md).

## Getting a package you can boot

> The workflow and the assembly script described here are being set up. Until the first kit is on the Releases page,
> the only way is to build everything from source.

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

What you need: a Redmi K40 / POCO F3 with its bootloader unlocked and USB debugging on; a PC with Windows 10 or 11
and WSL, or Linux; about 15 GB of free disk space; an Internet connection.

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
