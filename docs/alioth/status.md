# Technical status

English · [中文](status.zh.md)

State of the Redmi K40 / POCO F3 (`alioth`, Snapdragon 870 / SM8250) port as of November 2026. Every "works" here
was checked on the phone; numbers are measurements from it.

## How it boots

1. From Android, `adb reboot bootloader`, then `fastboot boot` of one image: a boot shim, the UEFI stage for this
   phone, our loader and the phone's device tree. The stock bootloader runs it from RAM; nothing is written.
2. The loader speaks fastboot over the UEFI's USB device protocol. It takes the system image into RAM
   (`fastboot stage`, 2.4 GB, about 97 s at 25 MB/s) and then the payload: kernel, root disk and arguments.
3. XNU (`xnu-8796.121.2` with the HakiOS patch and this phone's drivers) starts on seven cores
   (`bootcpu=7 cpus=7`). The eighth stays with the loader, which keeps its USB link during development runs:
   the kernel log, screenshots and the end of a run. In the release the USB controller goes to iOS instead.
4. The root disk holds our programs, launchd jobs and the phone's firmware. The system image is the stock iOS 16.5
   (20F66) file system for the iPhone X, built from the IPSW, mounted read-only. The data volume is a RAM disk.
5. When a run ends the kernel hands the hardware back and returns to the loader; a reboot starts Android.

The one-click release does all of this from a PC script; development runs do it step by step.

## Kernel drivers

| Part | How | State |
|---|---|---|
| Display | DSI video panel 1080 x 2400 at 120 Hz. iOS's frames go straight to the display engine: page flips of pipe VIG0 between two scan-out slots in the display's reserved memory, scaled from 1125 x 2436 by the engine (no CPU copy). Frames are paced on the panel's TE (8.24 ms). The colour correction block (DSPP PCC) does Invert Colors, Color Filters and Night Shift. Brightness by DCS. | Works |
| GPU | Adreno 650 brought up by the kernel itself: GMU, SMMU page tables, ring buffer, the SQE microcode from the phone's firmware; 670 MHz. Mesa's freedreno talks to it through an msm-style interface (buffers, submissions, fences, IOSurface import). Every client has its own GPU address space: before a client's commands the core is let go idle and the SMMU context gets that client's tables. | Works |
| Touch, keys, haptics | FocalTech touch controller with multitouch; power and volume keys from the PMIC; vibration motor | Works |
| Sound | The audio DSP started and driven over APR; two CS35L41 amplifiers; the WCD938x codec's microphone over SoundWire | Works |
| Wi-Fi | PCIe and MHI to the Qualcomm Wi-Fi chip, its firmware, Linux's ath11k under a compatibility layer; joins WPA2 networks, prefers 5 GHz | Works |
| Bluetooth | HCI over UART at 921600 baud, firmware and NVM loaded; iOS's own bluetoothd on top | Works, including audio |
| Cameras | Titan 480 IFE-lite path into memory, power through the PM8008; four sensors (IMX582 wide, S5K3T2 front, IMX355 ultra-wide, S5K5E9 macro) with colour tuning made from Android's files | Works; one back camera per boot |
| Flashlight | PM8150L flash module | Works |
| Sensors | The sensor DSP (SLPI) started; FastRPC and its sensor service: ambient light, accelerometer, gyroscope, magnetometer, rotation vector, orientation | Works |
| Battery | The fuel gauge: level and charging state | Works |
| USB | DWC3 in device mode: a serial port for development, or an iPhone with Apple's usbmux for the PC (pairing, AFC) | Works; re-plugging during a session is being fixed |
| CPU | Frequency scaling and thermal limits on seven cores | Works |
| Modem | The external SDX55 over PCIe: its boot loader refuses the image after authentication | Not working |
| GPS, NFC, fingerprint, proximity | — | Not started |

## User space

- **OpenGL ES.** `EAGLContext` is ours, on Mesa (OSMesa with freedreno), both for the compositor (backboardd) and,
  in their own process, for apps: ES 1, 2 and 3, `CAEAGLLayer` frames handed to the compositor as IOSurfaces without
  a copy, sharegroups, GLKit's texture loader, Core Video texture caches. iPhone-only formats are filled in: PVRTC
  textures are decoded on the CPU, the APPLE multisample and BGRA extensions are provided. Presents are capped at
  62 per second, as on a 60 Hz iPhone.
- **Libraries injected into SpringBoard, backboardd and apps** fill the gaps between an iPhone and this phone:
  the ambient light sensor as a HID service for automatic brightness, device motion and heading for Core Motion and
  Core Location (true north from the WMM2025 magnetic model), rotation, the camera's sensor selection.
- **Apps without signatures** install through LaunchServices: from the PC at every boot, or with an on-phone
  installer.
- **Accessibility**: iOS's accessibility bundles are in the system image, so AssistiveTouch, Bold Text and the rest work; the display's colour settings (Invert, Color Filters, Night Shift) reach the display engine's colour correction through the framebuffer driver, as on an iPhone.
- **USB as an iPhone**: our usbmux daemon carries the PC's connections to lockdownd and the services it starts.

## Measurements

| What | Value |
|---|---|
| Panel refresh | 120 Hz (TE every 8.24 ms), 121.4 frames a second measured in an app |
| Scrolling with a finger down | about 75 % of frames at the 120 Hz pace, the rest about 16 ms |
| Compositor CPU time per frame | about 3.1 ms |
| Minecraft PE 1.2.10 / Terraria 1.2 / The Sims 3 Ambitions | 60.6 / 60.6 / 62 frames a second (62 is the cap) |
| GPU Check | 81 of 83 (the two left need Metal) |
| GPU address spaces | 6 clients, 33 012 switches in 25 minutes, none refused, no SMMU fault; a present costs the same 0.2–0.5 ms as without |
| System image | 2.4 GB, in RAM for the whole session |
| Boot from the PC | about 3 minutes, most of it the system image over USB |

## Known problems

| Problem | Cause, as far as known |
|---|---|
| No Metal | No Metal device is published. Plan below. |
| Nothing persists | The data volume is a RAM disk: settings, photos and apps are gone after a reboot. |
| A GPU hang stops the screen | There is no hang detection or GPU reset yet. |
| Warm with the screen off | The CPU slows down, but the display link and the GPU stay on; no GPU power management yet (fixed clock). |
| USB after re-plugging | Fixed: a race in select() panicked the kernel at the unplug; the controller's EP0 stayed dead or answered wrongly after a replug (the controller is now restarted when the computer resets it in vain); the old session's data and a waiting write leaked into the new one. Tested with the port leaving and coming back by itself, three times a run (boot-arg usb_bounce); a hand-pulled cable is to be tried with this version. |
| Safari cannot pick photos | The photo picker does not reach the library here; not investigated yet. |
| A game asks for notifications at every boot | The answer is kept in the data volume, which is in RAM. |
| AssistiveTouch's Home does nothing | Its button, menu and Control Center work; the Home action is not wired to anything here yet. |

## Next

- **Power**: GPU frequency scaling and idle power collapse, the display link at screen-off, interrupts instead of polling.
- **Smoothness**: a third scan-out slot and swaps that do not wait for the flip.
- **GPU robustness**: per-buffer fences, a fence interrupt, hang recovery.
- **Metal** (estimated 2.5–3.5 months): a Metal plug-in published on the accelerator service, Metal translated to
  Vulkan (metal2vulkan) and Mesa's Turnip driver on the same kernel interface. First goal: a Metal device and a
  triangle; then apps.
- **Persistent install** (estimated 3–5 weeks, plus a boot menu): the kernel already has UFS code; the hard parts are
  a writable file system for the data volume and space on the phone's storage, which means repartitioning — only
  with the owner's explicit agreement and a full backup.
- **iOS 26** (researched; estimated 4–6 months to reach today's state): the kernel source is out, but all of iOS 26
  is arm64e and this CPU has no pointer authentication, so the user space must be converted offline; the new
  interface wants Metal to be smooth.
