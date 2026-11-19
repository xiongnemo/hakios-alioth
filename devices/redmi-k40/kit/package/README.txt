iOS 16.5 on the Redmi K40 / POCO F3 (alioth) -- a RAM-only boot package
=======================================================================

How to use
----------
1. Boot the phone into Android, turn on USB debugging and connect it to this PC (the bootloader must be unlocked).
2. Double-click start.bat (or 启动.bat). It reboots the phone into its bootloader, puts our loader into RAM with
   "fastboot boot", sends the system image (about 2.3 GB, a minute and a half over USB) and the kernel, and starts iOS.
3. To go back to Android, restart the phone (hold the power button). Restarting from within iOS returns to our
   loader's screen instead; hold the power button there.

启动-超广角.bat / 启动-微距.bat: the same, with the ultra-wide or the macro camera as Camera.app's back camera.

Nothing on the phone is flashed or erased; Android and its data are untouched. iOS keeps its own data in RAM, so
every boot starts fresh. apps\: IPAs installed at every boot (see apps\说明.txt).

By hand (what start.bat does), with the phone in its bootloader:
    fastboot boot boot\alioth-boot.img          the loader, in RAM; it answers "getvar product" with xnuloader
    fastboot oem ramdisk                        the next file staged is the system disk ...
    fastboot stage boot\system.apfs             ... the system image (2.3 GB, about 96 s)
    fastboot stage boot\run-payload.bin         the kernel, the root disk and the kernel's arguments
    fastboot oem run
The system image has to be staged first, on its own disk: with "oem ramdisk none" only the payload's small root
disk is there, and the kernel stops at "Process 1 exec of /sbin/launchd failed". start.bat also sets the clock
(epoch=) and the apps' token in the payload's arguments before staging it; by hand iOS starts with an old clock.

What works and what does not: https://github.com/xiongnemo/hakios-alioth (说明.txt has the same in Chinese).

This package was made on your computer from Apple's iOS files, the vendor's firmware from your phone and your
Wi-Fi password (boot\run-payload.bin): keep it to yourself.
