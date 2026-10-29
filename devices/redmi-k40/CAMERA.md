# UMI camera bring-up

The iOS capture bridge displays synthetic frames with `cam=5 cam_fake=1`.
Physical capture is still disabled in the UMI kernel overlay. `cam=1` reads
UMI GPIO and CamCC state and, when powered and clocked, the IFE-lite/CSID-lite identifiers.
It first checks GCC's camera AHB vote and skips camera register reads when
that bus is gated; CamCC reads with the bus off raise an external abort.
Seeing color bars does not establish that a sensor is streaming.

`cam=2` runs the UMI-specific S5KHMX power/identification path in
`overlays/xnu/files/bsd/dev/umi_camera.c`. It uses the traced PM8150L GPIO
sequence, camera-only PM8009 RPMh votes, MCLK1 at 19.2 MHz, and bounded
GPIO I2C transfers on CCI0 master 1. It reads chip identification and
revision, then restores GPIO/clock state and releases its camera votes.
It refuses an already-enabled MCLK or an active camera GPIO supply.
The module EEPROM map reads 0x2000 bytes at addresses 0 and 0x2000 from
slave 0xa0, then appends a one-byte read from sensor slave 0x7a register
0x000e. Its `SensorVersionOffset=0x4000` points to that appended sensor
value; it is not an EEPROM address or the chip revision at register 2.
The device returned `0x1301` at 0x000e: the first byte selects version
`0x13`. The distinct register-2 revision remains `0xa000`.
The `cam=2` path does not initialize a sensor mode or produce capture frames.

`cam=3` additionally selects the table from the high byte of register 0x000e,
validates the entire RWCTAB02 container before initialization writes, and
programs `init` followed by `res3` (3008×2256). It reads back dimensions,
line/frame lengths, and the disabled stream register. Only versions 3 and
0x13 are accepted; malformed settings and stream-on writes are rejected.
This diagnostic always powers down afterward. CSI/IFE capture is not started.
`cam=4` extends mode verification with a Kona-v2 ISP clock/reset probe.
It uses XO at 19.2 MHz for the six camera RCGs, checks that required clock
branches are unused before reparenting, and enables the IFE/CSID/PHY3
clock dependencies. Configuration AHB and IFE/CSID core clocks come on first so inherited
DMA/path enables can be inspected with CAMNOC/GCC camera AXI clocks still off;
active write masters are refused before they can resume. It maps IFE-lite0 at `0x0acd9000` once and derives
CSID-lite0 at offset `0x200`, avoiding overlapping independent mappings.
The probe reads block IDs and tests CSID480 and IFE-lite48x reset.
IFE top-v3 uses reset command `2` and completion bit 17 at status `0x40`;
the inherited top-v2 command/bit are incompatible. No write master, sensor
stream, SMMU mapping, or PLL is enabled. Cleanup restores parents and
only disables branches enabled by this probe. Live IDs are IFE `0x10000001`,
bus `0x20000000`, and CSID `0x20000001`. The inherited IFE reset mask
`0x20000` is accepted and restored; each disabled write master reads `0x10`,
whose bit 0 is clear. Other IRQ enables, enabled RDIs or enabled write
masters remain grounds to refuse the probe. Both CSID reset phases and IFE-lite48x reset passed on the device.
The optional core-AHB branch at `0xc150` is not a required IFE/CSID DT
clock dependency and did not retain its enable bit in the first test.
It is excluded from this probe.

```sh
python3 devices/redmi-k40/tools/test-camera-isp.py
```

`cam=6` adds a hardware-generated 320×240 RAW10 test frame through
CSID480 RDI0 and IFE-lite48x's bus-v3 write master. This is separate from
the CPU-generated frames used by `cam=5 cam_fake=1`; neither is a physical
sensor preview. `umi_camera_dma.c` owns the diagnostic's buffer, page tables,
SMMU context and stream matches. It rejects occupied routes, checks context
and route readbacks before enabling a writer, bounds the capture wait, and
restores its SMMU state after stopping the engines. The buffer remains wired
for the loader's `camera` export. Buffer guards and completion/data checks
must pass before it reports a frame. Device validation captured one complete
frame with no SMMU faults and intact guards; the exported RAW10 decodes to
clean color bars.

UMI accepted the DT-shaped `0x800/0x400` stream match, but rejected a combined
`0x800/0x441` match and the odd `0x801/0x400` entry. The diagnostic now requests
only data writers `0x800` and `0x840`, each with mask `0x400`; both routes
read back correctly and the hardware TPG capture completed. Physical sensor
frame capture remains disabled. Reserved FSR bit `0x400` is not a context fault:
only the architected `0xc00001fe` fault bits are tested or acknowledged.

```sh
python3 devices/redmi-k40/tools/test-camera-dma.py
```

The host parser/transaction test uses the locally generated tables:

```sh
python3 devices/redmi-k40/tools/test-camera-tables.py \
  out/redmi-k40/camera-reference/camera-s5khmx-v3.tab \
  out/redmi-k40/camera-reference/camera-s5khmx-v19.tab
```

The probe is compiled with `-mgeneral-regs-only`; its kernel thread does
not own a userspace SIMD context. The power fault-injection check can also
check the actual compiled object:

```sh
python3 devices/redmi-k40/tools/test-camera-power.py --object \
  out/redmi-k40/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/umi_camera.o
```

The main rear sensor is S5KHMX. Its stock Android kernel reports ID `0x1ad0`
and CamX slave address `0x7a` (CCI SID `0x3d`). The live device tree and the
Android rear 1× startup trace captured on 2026-10-27 identify this path:

| Item | UMI configuration |
| --- | --- |
| Sensor | CCI0 master 1, `0x0ac4f000`, 16-bit register and value |
| ID | register `0x0000`, expected `0x1ad0` |
| CCI pins | TLMM 103 data, 104 clock |
| Reset | TLMM 62, active high |
| MCLK | MCLK1, TLMM 95, 19.2 MHz |
| Camera bus | GCC AHB `0xb008` and XO `0xb040`, enable before any CamCC access |
| CSI | CSIPHY 3, three C-PHY trios, assignment `0x210`, settle count 19 |
| VAF | PM8009 L6 (`ldof6`), 2.85 V during stock preview |
| VIO | PM8009 L7 (`ldof7`), 1.8 V |
| VDIG | PM8009 L1 (`ldof1`), stock request/actual 1.104 V; DT specifies 1.1 V |
| VANA | `camera_ultra_wide_vana_ldo`, PM8150L GPIO 3, 2.8 V |
| CUSTOM1 | `camera_wide_vana_ldo`, PM8150L GPIO 1, 2.8 V |
| CUSTOM2 | `camera_tele_vana_ldo`, PM8150L GPIO 4, 2.8 V |

PMIC GPIO numbers in the table are the device-tree specifier cells (the
SPMI GPIO driver translates them to its hardware pin indices). GPIOs 103/104
are on CCI0; other rear modules use CCI1. Android's rear 1× logical camera
also opens another rear sensor, so its complete active regulator snapshot
contains additional supplies that the main sensor alone does not need.

The main module's power-up sequence is VAF (10 ms), VIO (1 ms), VANA (1 ms),
VDIG (1 ms), CUSTOM1 (1 ms), CUSTOM2 (1 ms), RESET high (3 ms), MCLK
19.2 MHz (2 ms). Shutdown reverses the sequence. Xiaomi's V2 power enum
inserts CUSTOM3 at type 8, so RESET is type 9 and STANDBY is type 10.
The converter normalizes RESET/STANDBY to RWCTAB02 types 8/9 and rejects
CUSTOM3 export. The Android trace confirms type 9 drives TLMM 62.

`tools/umi-camsensor-v2.py` decodes the stock Parameter Parser V2 module.
Its resolution elements are 304 bytes and register-setting elements are
80 bytes. The main sensor has seven modes in each of two version sets:
`3` and `0x13`. Each set has its own initialization table. Mode 3 is
3008×2256 at about 30 fps; mode 5 is 1920×1080 at 120 fps.
All seven primary dimensions, line lengths and frame lengths agree with
the corresponding sensor registers `0x034c`, `0x034e`, `0x0342`, `0x0340`.

For example, export a particular version with:

```sh
python3 devices/redmi-k40/tools/umi-camsensor-v2.py \
  fw-camera/com.qti.sensormodule.umi_semco_s5khmx.bin \
  --sensor-version 0x13 --output out/camera-s5khmx-v19.tab
```

The UMI userland build packages `camera-s5khmx-v3.tab` and
`camera-s5khmx-v19.tab` when that vendor module is present in `fw-camera/`.
It does not select a default revision or enable physical capture. Vendor
modules, generated register tables and Android captures remain ignored.

The power/ID probe implements UMI RPMh votes, PM8150L supply switches,
TLMM GPIO I2C, and the CamCC MCLK path. The device test read S5KHMX ID
`0x1ad0`, revision register `0xa000`, and table selector `0x13`, then
successfully released all three camera regulator votes. Mode programming
passed hardware readback: version 0x13 applied 259 initialization and
179 mode writes; width/height were 0x0bc0/0x08d0, line/frame lengths
0x3ea0/0x0945, and stream register 0x0100 remained zero. All three
camera regulator votes were released afterward.
Remaining capture work includes high-rate CamCC power/clock setup,
CSIPHY v1.2.1, the live sensor stream, and its iOS capture integration.
The CSID480/IFE48x DMA and camera SMMU path passed the hardware TPG test. The inherited Redwood capture
register map remains gated.

The RPMh client shares the ADSP sequencer mutex. It can latch its own TCS
completion source when the DRV2 SPI5 interrupt is nonsecure, level-triggered,
and masked at the GIC; it does not modify GIC configuration. Otherwise it
requires an observed execution transition. Stale command status alone is
never treated as completion. Run the transaction model with:

```sh
python3 devices/redmi-k40/tools/test-rpmh.py
```

Clock and GPIO register definitions are from Xiaomi's
[UMI GCC driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/umi-q-oss/drivers/clk/qcom/gcc-kona.c)
and [Kona pin controller](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/umi-q-oss/drivers/pinctrl/qcom/pinctrl-kona.c).
