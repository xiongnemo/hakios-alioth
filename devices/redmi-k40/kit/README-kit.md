# alioth kit: iOS 16.5 on the Redmi K40 / POCO F3

Built by GitHub Actions from [xiongnemo/hakios-alioth](https://github.com/xiongnemo/hakios-alioth) alone. It is half
of a bootable package; `assemble.bat` (Windows) or `assemble.sh` (Linux) makes the other half on your computer.

| Here | |
|---|---|
| `prebuilt/kernel-k40.bin` | The XNU kernel with this phone's drivers (no firmware in it) |
| `prebuilt/userland/` | Our programs and libraries for iOS, Mesa's OSMesa, the IPA installer |
| `prebuilt/BUILD`, `SHA256SUMS` | What it was built from |
| `source/` | The repository at that commit: the scripts that assemble the package, and the source of everything above |
| `assemble.bat`, `assemble.sh` | Make the package |

## What you need

- A Redmi K40 / POCO F3 (`alioth`) with its bootloader unlocked, USB debugging on, and **root** in Android (Magisk or
  the like): the audio and sensor DSPs' firmware, the Bluetooth firmware and the sensors' calibration are readable by
  root only. Nothing on the phone is written.
- Windows 10 or 11 with WSL (`wsl --install` in an administrator PowerShell), or Linux.
- About 15 GB of free space and an Internet connection.

## Making the package

Connect the phone in Android, then run `assemble.bat` (or `./assemble.sh`). It

1. downloads the iOS 16.5 IPSW for the iPhone X (`iPhone10,3`) from Apple's public servers and unpacks the parts it
   needs (about 7.4 GB; nothing is decrypted or circumvented);
2. copies the firmware from the phone (`source/devices/redmi-k40/kit/pull-firmware.sh`; allow root when the phone asks);
3. asks for the Wi-Fi network iOS should join;
4. builds the system image and the root disk with the programs in `prebuilt/` (about an hour, mostly packing);
5. builds the loader with the iPhone identity from the IPSW, the boot image and the payload;
6. writes `ios-alioth-k40/` beside the kit, with `start.bat`, Google's adb and fastboot, and the port's apps.

Then run `start.bat` in `ios-alioth-k40/` with the phone in Android. A reboot brings Android back.

**Keep `ios-alioth-k40/` to yourself**: it holds Apple's files, the vendor's firmware and your Wi-Fi password.
