# Fresh UMI UEFI port

Mu-Silicium base: `42fe563791e51bf5500c2e470ed294ca0c94b97c`.
The `BASE` file pins the source; its submodule commits pin the dependencies.

Verified on the connected Xiaomi Mi 10 on 2026-10-22:

- temporary `fastboot boot out/umi-boot.img` accepted;
- user observed the Project Silicium logo and the UMI loader-handoff message;
- USB `product: xnuloader`, `version: 0.5`;
- loader log: `platform umi`, `display 1080 x 2340 at 9c000000`, `usb: started`;
- bundled XNU reached checkpoint 1 and returned;
- a normal eight-second XNU run initialized GICv3, timer PPI 27 and the
  framebuffer, then returned when its session timer expired (12,241 log bytes).

Subsequent RAM-disk diagnostics mounted APFS and executed static init as PID 1,
with both one CPU and four CPUs (`bootcpu=7 cpus=4`). Each ran 22 checks;
21 passed and the directory-skeleton check failed because that deliberately
minimal image did not include the iOS var.skel. Both returned to the loader.
Logs: `logs/145420-run.log` and `logs/145553-run.log`.

This verifies UEFI, USB and XNU handoff. iOS 16.5 has subsequently reached the SpringBoard home screen; see
[the iOS boot record](../../docs/UMI-BOOT.md#verified-ios-result-2026-10-22).
Logs and screen captures are under `logs/umi-uefi-first/` and the timestamped
`logs/144308-run-cp1.log`, `logs/144348-run.log` (local, untracked).

Initial boot image SHA256:
`7eafc24abb650790294503903555ed33fc1c28d5a11950f2d63aa19d32806e41`.
Initial raw FD SHA256:
`9f7ae84961110cd82320178a0ee582b0d69457e5b91c245a3eebe982fe0d27cb`.

## What changed

The device package follows Silicium's Kona platform and alioth package layout,
with SM8250 (not SM8250-AC), Mi 10 identity, a 1080x2340 simple framebuffer,
a memory map derived from this phone's own XBL `uefiplat.cfg`, and explicit
reservations for the DTB's low-memory hole and the 64 MiB XNUP container.
FD base/size are 0x9fc00000/0x300000; XNUP is at 0xf0000000.

Firmware drivers are extracted from this UMI's XBL, matched by file GUID to
the pinned Kona INF descriptions. No alioth driver executable is substituted.
Only the known Kona TzDxeLA app-loaded flag is changed (offset 0xd469,
0 -> 1, exact binary revision guarded). UFS/SD/filesystem drivers, vendor
panel initialization and DSP loaders are omitted from the initial image.
The display uses the scanout ABL already initialized. The UMI-local BDS
hook loads the EFI application directly from RAM; it writes no partition log.

## Rebuild

Keep local firmware dumps out of git. From the kernel repository:

```sh
# A clean checkout must be at uefi/umi/BASE, with its pinned submodules:
git clone https://github.com/Project-Silicium/Mu-Silicium.git Mu-Silicium
git -C Mu-Silicium checkout "$(cat uefi/umi/BASE)"
git -C Mu-Silicium submodule update --init --recursive
python3 -m venv venv-uefi
venv-uefi/bin/pip install -r Mu-Silicium/pip-requirements.txt uefi_firmware==1.16

DTB=../umi.dtb uefi/umi/apply.sh
# The phone must report umi in recovery before pulling the firmware:
mkdir -p work/umi-firmware
adb pull /dev/block/bootdevice/by-name/xbl_4 work/umi-firmware/xbl_4.img
venv-uefi/bin/python uefi/umi/extract.py work/umi-firmware/xbl_4.img
# nasm, ndisasm, iasl, clang, lld-link and aarch64-linux-gnu-objcopy on PATH:
PREPARE=1 uefi/umi/build.sh
```

This host uses locally extracted nasm/iasl in `work/host-tools/usr/bin`, so:

```sh
PATH="$PWD/work/host-tools/usr/bin:$PATH" uefi/umi/build.sh
```

Output: `out/umi-boot.img`, containing UMI UEFI and its USB-resident EFI loader.
Test with `fastboot boot`, never `fastboot flash`.
The UEFI sources live in `files/`; firmware modules remain in ignored
`Mu-Silicium/Binaries/umi`. Update `files/` after source changes in Mu-Silicium.
