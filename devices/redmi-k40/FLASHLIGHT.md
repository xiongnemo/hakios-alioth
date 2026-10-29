# UMI flashlight

UMI uses the PM8150L flash peripheral at SPMI SID 5, base `0xd300`.
A read-only device probe identified type `0x18`, subtype `0x04`, revision 0,
owned by execution environment 0 on arbiter channel 97. The PMIC temperature
alarm is on SID 4 at `0x2400`.

The device overlay enables channel 0 alone. Android traces on this device
show channel 0 ramping up normally (`INT_RT_STS=0x90`), while enabling
channel 1 produces `STATUS1=0x08` and the fault interrupt. Android enables
both channels and logs that fault without stopping the working output; the
port keeps channel 1 disabled instead of suppressing the fault guard.
Register offsets and software strobe setup
come from UMI's device tree and
[Xiaomi's vendor driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/umi-q-oss/drivers/leds/leds-qpnp-flash-v2.c).
The upstream Redwood driver remains unchanged.

Boot arguments:

- `torch=1`: read identification, ownership and registers only.
- `torch=2`: bounded two-second test pulse.
- `torch=3`: expose `/dev/hakios-torch` for the existing iOS 16.5 FigFlashlight
  integration in mediaserverd. No light is enabled until requested by iOS.

The working LED uses 12.5 mA steps, a 25 mA minimum and a 125 mA maximum
on channel 0. The minimum avoids PM8150L's low-current resolution workaround.
The stock device tree allows 500 mA per channel; this port deliberately uses
less. Channels 1 and 2 remain disabled. Fault detection stays enabled. Android's
headroom auto-mode initialization and 192 microsecond short-detection delay
are applied before enabling either channel; the firmware leaves the delay at zero.

Faults, register failures, a battery temperature of 45 C or more, or 30 seconds
of continuous use turn the light off. After the time limit, toggle off and on
to restart it. Partial enable failures attempt every shutdown write even after
the fault latch is set. Subsequent enables remain blocked after a fault.

Run `python3 devices/redmi-k40/tools/test-torch.py` to exercise the actual
configuration, current limits, working-channel selection and partial-write cleanup
against mock registers. Hardware operation still requires a device test.
