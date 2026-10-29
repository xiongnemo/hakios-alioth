/*
 * The cameras of the Redmi K40 / POCO F3 (alioth, SM8250-AC "Kona"), as Android has them fitted on
 * this phone (persist.vendor.camera.*.info): the wide one, a Sony IMX582 (48 MP, Android's camera 0,
 * "alioth_sunny_imx582_wide"), the front one, a Samsung S5K3T2 (20 MP, camera 1), the ultra-wide one,
 * a Sony IMX355 (8 MP, camera 2, "alioth_ofilm_imx355_ultra") and the macro one, a Samsung S5K5E9 (5 MP,
 * camera 3). Every address and number below belongs to this phone, from Android's own device tree
 * (cam-sensor0 to cam-sensor3, the CCIs, the CSIPHYs, csid-lite0, ife-lite0 and what they point to),
 * the sensors' module files of Android's camera stack (/vendor/lib64/camera/com.qti.sensormodule.
 * alioth_*.bin, CamX's Parameter Parser V2: the slave address, the chip ID and where it is read, the
 * power sequence, the tables; devices/redmi-k40/tools/alioth-camsensor.py) and, for the SoC's camera
 * blocks, Linux's drivers for SM8250 (pinctrl-sm8250.c, camcc-sm8250.c, gcc-sm8250.c and camss: its
 * csiphy-3ph-1-0, csid-gen2 and vfe-480), read as data. The wide one (the others' differences are in
 * sensors[] below):
 *
 *   the sensor     on CCI 0, master 0 (GPIO 101 data, 102 clock, function cci_i2c, internal pull-ups),
 *                  slave address 0x10 (CamX's 0x20); done by hand here, as the other drivers do their
 *                  I2C (the two pins taken for each transaction and given back as they were found)
 *   chip ID        0x0582 at the 16-bit address 0x0016, a 16-bit read
 *   reset          GPIO 93, high = running
 *   MCLK           camcc's mclk0 (RCG 0x5000, branch 0x5018) at 19.2 MHz from the crystal on GPIO 94
 *                  (function cam_mclk, 4 mA), after the camera's top power domain ("cam_clk":
 *                  titan_top_gdsc, camcc 0xc144); camcc is only read or written with GCC's camera
 *                  AHB and XO clocks on (0xb008, 0xb040: without them a read is an external abort)
 *   VANA           the PM8008's LDO7 at 2.9 V, VDIG its LDO2 at 1.1 V, CUSTOM1 its LDO6 at 1.8 V
 *                  (normal power mode for the loads Android asks). The PM8008 is on QUP SE16's I2C
 *                  (i2c@888000: GPIO 48 data, 49 clock, the TLMM's south tile; addresses 0x08 for the
 *                  chip, 0x09 for the regulators, 16-bit registers), done by hand too; its reset pin
 *                  is GPIO 77, an output held high as Android's pinctrl "pm8008_active" has it
 *   VIO            PM8150A's L5 at 1.8 V (RPMh's "ldoc5", shared by the four sensors and nothing else)
 *   power-up       (the module file's): reset low, 1 ms; VANA, CUSTOM1, VDIG, VIO on, 1 ms; MCLK on,
 *                  1 ms; reset high, 2 ms. Power-down: reset low, 2 ms; MCLK off; the rest off.
 *   CSI            CSIPHY 1 (0xac6c000), four D-PHY lanes (lane assignment 0x3210)
 *
 * The front one: CCI 1 master 1 (GPIO 107/108), slave 0x10, chip ID 0x3142 at 0x0000, reset GPIO 109,
 * MCLK3 (GPIO 97); VANA LDO5 at 2.8 V, VDIG LDO1 at 1.05 V; four lanes into CSIPHY 4.
 * The ultra-wide one: CCI 1 master 0 (GPIO 105/106), slave 0x1a, chip ID 0x0355 at 0x0016, reset GPIO
 * 78, MCLK2 (GPIO 96); VANA LDO3 at 2.7 V, VDIG PM8150A L3 ("ldoc3") at 1.2 V; four lanes, CSIPHY 2.
 * The macro one: CCI 0 master 1 (GPIO 103/104), slave 0x2d, chip ID 0x559b at 0x0000, reset GPIO 92,
 * MCLK1 (GPIO 95); VANA LDO4 at 2.8 V, VDIG PM8150A L2 ("ldoc2", 1.2 V: the display's DVDD too, so
 * never switched off here); two lanes into CSIPHY 0.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: the camera's clocks and power domain as they are, the pins, the
 *           PM8008's reset pin, and if it is out of reset the PM8008 itself (its chip enable and
 *           the sensor's LDOs), the rails
 *   cam=2   ... and the sensor powered up as Android powers it, its chip ID read, and powered
 *           down again (every write read back); cam_sensor=all: the four, one after the other
 *   cam=3   the CSID's test pattern through RDI 0 and IFE-lite 0 into memory, which the loader hands
 *           to the host after the run (xl camera): the camera subsystem's rails, PLL, clocks, SMMU
 *           and blocks without the sensor (capture_tpg() below)
 *   cam=4   the sensor's own frames that way, through its CSIPHY (capture_sensor() below)
 *   cam=5   the frames for iOS (/dev/hakios-camera); cam_fake=1 a chart made here instead
 *
 * Boot-arg cam_sensor=macro|wide|front|ultra picks the sensor (default: the wide one).
 *
 * Limits, whatever asks: Android's voltages only (the device tree's; nothing else is ever written to
 * the LDOs' set points); only the registers named above are written (a list each, checked at every
 * write) and each is read back; any write that does not read back, a regulator that is not ready
 * in time or reports over-current, a byte nobody acknowledges, and everything is switched off again
 * in the power-down's order and nothing more is done. At the run's end (and on a panic's way out)
 * whatever was switched on is switched off. Everything that was found switched on is left on.
 * The camera subsystem's own registers (camcc's PLL0 and the clocks named there, the sensor's CSIPHY,
 * CSID-lite 0, IFE-lite 0, two match registers and a context of the SMMU) are written the same way,
 * read back where they keep what they are given, and taken back in the reverse order.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.camera.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is written in this run\n", why);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

/*
 * SM8250's pin controller is in tiles (pinctrl-sm8250.c): every pin used here is in the north one
 * (0xf900000) but the PM8008's bus, GPIO 48 and 49, which are in the south one (0xf500000). A pin's
 * registers are at its tile + 0x1000 x its number.
 */
#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_TILE_SIZE          0x90000UL                       /* (pins 0 to 143) */
#define PIN_SOUTH(n)            ((n) == 48 || (n) == 49)
#define GPIO_CFG(n)             ((PIN_SOUTH(n) ? tlmm_south : tlmm) + 0x1000UL * (n))  /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (GPIO_CFG(n) + 4)               /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)

#define PIN_PMIC_SDA            48      /* QUP SE16's I2C (i2c@888000): the PM8008 */
#define PIN_PMIC_SCL            49
#define PIN_PMIC_RESET          77      /* the PM8008's reset: "pm8008_active", an output high with a pull-up, 2 mA */

static vm_offset_t tlmm, tlmm_south;    /* (tlmm: the north tile) */

/*
 * A pin's low ten bits (pull, function, drive, output) written, the rest of its register as it is
 * (and, as umi_camera.c does it for Kona, bit 12 with bit 11).
 */
static void
cfg_put(unsigned pin, uint32_t low)
{
	uint32_t cfg = rd(GPIO_CFG(pin)) & ~0x3ffu;

	if (cfg & (1u << 11)) {
		cfg |= 1u << 12;
	}
	wr(GPIO_CFG(pin), cfg | low);
}

/*
 * The four sensors, as Android's device tree has them (cam-sensor3 the macro, cam-sensor0 the wide,
 * cam-sensor1 the front, cam-sensor2 the ultra-wide; the indexes are the ones libhakios.media knows: 1 the
 * back camera, 2 the front one): the pins of their CCI master (taken by hand), the reset, MCLK (camcc's
 * mclkN: RCG 0x5000 + 0x1c N, branch 0x5018 + 0x1c N; the pin and its function 1, cam_mclk, at 4 mA),
 * the supplies the module file's power sequence switches (a PM8008 LDO, or a PM8150A LDO through RPMh,
 * at the device tree's voltage), the CSIPHY, how the sensor's exposure registers are laid out, and
 * whether a lens motor sits on its bus. Boot-arg cam_sensor=macro|wide|front|ultra picks one (default:
 * the wide one).
 */
enum { SUPPLY_NONE, SUPPLY_LDO, SUPPLY_RPMH };
struct supply {
	uint8_t         kind, ldo;      /* SUPPLY_LDO: PM8008 LDO n ... */
	uint16_t        mv;             /* ... at this set point (the device tree's) */
	const char *    rail;           /* SUPPLY_RPMH: RPMh's name of a PM8150A LDO ("ldocN") */
};
struct sensor_hw {
	const char *    name;
	const char *    tab;            /* in fw/ of the small image (devices/redmi-k40/tools/alioth-camsensor.py) */
	const char *    bus_name;
	unsigned        sda, scl, reset;
	unsigned        mclk;           /* camcc's mclkN */
	unsigned        mclk_pin;
	struct supply   vana, vdig, vio, custom1;
	unsigned        csiphy;
	uint8_t         regs;           /* the exposure registers: REGS_SMIA or REGS_SONY */
	uint8_t         bayer;          /* the picture's order as it comes here (camraw.py's codes: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR) */
	uint8_t         pattern_bayer;  /* ... and its own colour bars' (its module file's "tpgon") */
	const char *    chip;           /* for the frames' header */
	uint16_t        gain_unity, gain_max;   /* the analog gain's 1x, and the most it is given (REGS_SONY: in 1/256) */
	bool            lens;           /* a lens motor (DW9800) at 0x0c on its bus */
};
enum {
	REGS_SMIA,                      /* SMIA: frame length 0x0340, coarse integration 0x0202, analog gain 0x0204 (linear), hold 0x0104 */
	REGS_SONY,                      /* the same registers, the analog gain a code: gain = 1024 / (1024 - code) */
};
static const struct sensor_hw sensors[] = {
	{       /* cam-sensor3: S5K5E9, CCI 0 master 1 (GPIO 103/104), reset GPIO 92, MCLK1 (GPIO 95), csiphy 0 */
		.name = "macro (S5K5E9)", .tab = "camera-s5k5e9.tab", .bus_name = "CCI 0 master 1", .sda = 103, .scl = 104, .reset = 92,
		.mclk = 1, .mclk_pin = 95,
		.vana = { SUPPLY_LDO, 4, 2800, NULL }, .vdig = { SUPPLY_RPMH, 0, 1200, "ldoc2" }, .vio = { SUPPLY_RPMH, 0, 1800, "ldoc5" },
		.csiphy = 0, .regs = REGS_SMIA, .bayer = 1, .pattern_bayer = 1, .chip = "S5K5E9", .gain_unity = 32, .gain_max = 0x200,
	},
	{       /* cam-sensor0: IMX582, CCI 0 master 0 (GPIO 101/102), reset GPIO 93, MCLK0 (GPIO 94), csiphy 1 */
		.name = "wide (IMX582)", .tab = "camera-imx582.tab", .bus_name = "CCI 0 master 0", .sda = 101, .scl = 102, .reset = 93,
		.mclk = 0, .mclk_pin = 94,
		.vana = { SUPPLY_LDO, 7, 2900, NULL }, .vdig = { SUPPLY_LDO, 2, 1100, NULL }, .vio = { SUPPLY_RPMH, 0, 1800, "ldoc5" },
		.custom1 = { SUPPLY_LDO, 6, 1800, NULL },
		.csiphy = 1, .regs = REGS_SONY, .bayer = 0, .pattern_bayer = 0, .chip = "IMX582", .gain_unity = 256, .gain_max = 16 * 256,
		.lens = true,
	},
	{       /* cam-sensor1: S5K3T2, CCI 1 master 1 (GPIO 107/108), reset GPIO 109, MCLK3 (GPIO 97), csiphy 4 */
		.name = "front (S5K3T2)", .tab = "camera-s5k3t2.tab", .bus_name = "CCI 1 master 1", .sda = 107, .scl = 108, .reset = 109,
		.mclk = 3, .mclk_pin = 97,
		.vana = { SUPPLY_LDO, 5, 2800, NULL }, .vdig = { SUPPLY_LDO, 1, 1050, NULL }, .vio = { SUPPLY_RPMH, 0, 1800, "ldoc5" },
		.csiphy = 4, .regs = REGS_SMIA, .bayer = 2, .pattern_bayer = 2, .chip = "S5K3T2", .gain_unity = 32, .gain_max = 0x200,
	},
	{       /* cam-sensor2: IMX355, CCI 1 master 0 (GPIO 105/106), reset GPIO 78, MCLK2 (GPIO 96), csiphy 2 */
		.name = "ultra-wide (IMX355)", .tab = "camera-imx355.tab", .bus_name = "CCI 1 master 0", .sda = 105, .scl = 106, .reset = 78,
		.mclk = 2, .mclk_pin = 96,
		.vana = { SUPPLY_LDO, 3, 2700, NULL }, .vdig = { SUPPLY_RPMH, 0, 1200, "ldoc3" }, .vio = { SUPPLY_RPMH, 0, 1800, "ldoc5" },
		.csiphy = 2, .regs = REGS_SONY, .bayer = 0, .pattern_bayer = 0, .chip = "IMX355", .gain_unity = 256, .gain_max = 16 * 256,
	},
};
static const struct sensor_hw *hw = &sensors[1];
static unsigned hw_index = 1;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("camera: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u (cfg %#x)\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1, cfg);
}

/* The pins written while powered up, as they were found, to be put back: the reset pin, MCLK's. */
static uint32_t found_reset_cfg, found_reset_io, found_mclk_cfg;
static bool pins_found;

/* the sensor's reset and MCLK pins as they are now (to be put back when it is powered down) */
static void
find_pins(void)
{
	found_reset_cfg = rd(GPIO_CFG(hw->reset));
	found_reset_io = rd(GPIO_IO(hw->reset));
	found_mclk_cfg = rd(GPIO_CFG(hw->mclk_pin));
	pins_found = true;
}

static void
reset_pin(bool high)
{
	wr(GPIO_IO(hw->reset), high ? 2 : 0);
	cfg_put(hw->reset, CFG_OUTPUT | CFG_DRIVE_MA(2));               /* GPIO, no pull, 2 mA: Android's cam_sensor_*_active */
}

/* ---- I2C by hand (as in hakios.haptic.c) ---------------------------------------------------------- */

struct bus {
	const char *    name;
	unsigned        sda, scl;
	uint32_t        cfg[2], io[2];  /* the pins as they were found, while taken */
};
static struct bus pmic_bus = { .name = "the PM8008's I2C", .sda = PIN_PMIC_SDA, .scl = PIN_PMIC_SCL };
static struct bus cci_bus;              /* the sensor's (from hw) */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	cfg_put(pin, CFG_PULL_UP | (high ? 0 : CFG_OUTPUT) | CFG_DRIVE_MA(2));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(struct bus *b)
{
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	line(b->sda, false);
	half_bit();
	line(b->scl, false);
	half_bit();
}

static void
i2c_stop(struct bus *b)
{
	line(b->sda, false);
	half_bit();
	line(b->scl, true);
	half_bit();
	line(b->sda, true);
	half_bit();
}

static bool
i2c_bit(struct bus *b, bool out)
{
	bool in;

	line(b->sda, out);
	half_bit();
	line(b->scl, true);
	for (unsigned stretched = 0; !level_of(b->scl) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(b->sda);
	line(b->scl, false);
	return in;
}

static bool
i2c_write(struct bus *b, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(b, (byte >> bit) & 1);
	}
	return !i2c_bit(b, true);
}

static uint8_t
i2c_read(struct bus *b, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(b, true));
	}
	i2c_bit(b, !more);
	return byte;
}

static void
bus_give_back(struct bus *b)
{
	wr(GPIO_IO(b->sda), b->io[0] & 2);
	wr(GPIO_IO(b->scl), b->io[1] & 2);
	wr(GPIO_CFG(b->sda), b->cfg[0]);
	wr(GPIO_CFG(b->scl), b->cfg[1]);
}

/*
 * A sensor stopped in the middle of a byte -- the camera switched away from it between frames, its
 * digital rail left on because the driver found it on -- goes on holding the data line down, and the
 * bus never comes to rest again however long one waits for it. The way out is the one Linux's
 * bit-banged master takes (i2c_generic_scl_recovery): clock the line that is free until the sensor has
 * finished that byte and lets the data line go, then a stop. Nine clocks are a byte and the
 * acknowledgement after it. This is what kept the wide camera's bus (CCI 0 master 1) for the rest of a
 * session once Camera.app had switched to the front camera and back.
 */
static bool
bus_unwedge(struct bus *b)
{
	for (unsigned clocks = 1; clocks <= 9; clocks++) {
		line(b->scl, false);
		half_bit();
		line(b->scl, true);
		half_bit();
		if (level_of(b->sda)) {
			i2c_stop(b);
			say("camera: %s: the data line was held down and came back after %u clock(s)\n", b->name, clocks);
			return true;
		}
	}
	return false;
}

static bool
bus_take(struct bus *b)
{
	b->cfg[0] = rd(GPIO_CFG(b->sda));
	b->cfg[1] = rd(GPIO_CFG(b->scl));
	b->io[0] = rd(GPIO_IO(b->sda));
	b->io[1] = rd(GPIO_IO(b->scl));
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	if (level_of(b->sda) && level_of(b->scl)) {
		return true;
	}
	/* the data line alone held down is a sensor that did not finish a byte: clock it out (see above) */
	if (level_of(b->scl) && !level_of(b->sda) && bus_unwedge(b) && level_of(b->scl)) {
		return true;
	}
	{
		bool sda = level_of(b->sda), scl = level_of(b->scl);

		bus_give_back(b);
		say("camera: %s is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", b->name, b->sda, sda, b->scl, scl);
	}
	return false;
}

/*
 * A transaction: `out' (the register's address, and for a write the data) written, then for a read
 * `in_count' bytes read after a repeated start. False if anybody did not acknowledge.
 */
static bool
i2c_transfer(struct bus *b, uint8_t address, const uint8_t *out, unsigned out_count, uint8_t *in, unsigned in_count)
{
	bool answered;

	if (!bus_take(b)) {
		return false;
	}
	i2c_start(b);
	answered = i2c_write(b, (uint8_t)(address << 1));
	for (unsigned i = 0; answered && i < out_count; i++) {
		answered = i2c_write(b, out[i]);
	}
	if (answered && in_count != 0) {
		i2c_start(b);
		answered = i2c_write(b, (uint8_t)(address << 1 | 1));
		for (unsigned i = 0; answered && i < in_count; i++) {
			in[i] = i2c_read(b, i + 1 < in_count);
		}
	}
	i2c_stop(b);
	bus_give_back(b);
	return answered;
}

/* ---- the PMIC arbiter (version 5), read only: the PM8150A's LDOs as they are (as in hakios.torch.c) ---- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0

#define ARB_VERSION             0x000
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

static vm_offset_t arb_core, arb_cnfg, arb_observer;

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

/*
 * The PM8008's reset pin, GPIO 77, as Android's "pm8008_active" has it: a GPIO, an output driven high,
 * a pull-up, 2 mA. As found it is put back when the kernel leaves (hakios_camera_quiesce).
 */
static uint32_t pm_reset_found_cfg, pm_reset_found_io;
static bool pm_reset_changed;

static bool
pm_reset_active(void)
{
	uint32_t cfg = rd(GPIO_CFG(PIN_PMIC_RESET));

	return ((cfg >> 2) & 15) == 0 && (cfg & CFG_OUTPUT) && (rd(GPIO_IO(PIN_PMIC_RESET)) & 2);
}

/* ---- the PM8008 ------------------------------------------------------------------------------------ */

#define PM8008_CHIP             0x08    /* its I2C addresses: the chip ... */
#define PM8008_REGULATORS       0x09    /* ... and its regulators */
#define REVID_BASE              0x0100
#define MISC_CHIP_ENABLE        0x0950  /* [0] */
#define LDO_BASE(n)             (0x4000 + 0x100 * ((n) - 1))
#define LDO_STATUS1             0x08    /* [7] ready, [5] over-current, [1:0] mode: 3 normal, 2 low power */
#define LDO_VSET_LB             0x40    /* the set point in mV, low byte ... */
#define LDO_VSET_UB             0x41    /* ... high byte */
#define LDO_MODE_CTL1           0x45    /* [2:0] 7 normal, 4 low power */
#define LDO_ENABLE              0x46    /* [7] */

/* LDO n is one of the sensor's supplies, at that set point. */
static bool
sensor_ldo(unsigned n, unsigned mv)
{
	const struct supply *all[4] = { &hw->vana, &hw->vdig, &hw->vio, &hw->custom1 };

	for (int i = 0; i < 4; i++) {
		if (all[i]->kind == SUPPLY_LDO && all[i]->ldo == n && (mv == 0 || all[i]->mv == mv)) {
			return true;
		}
	}
	return false;
}

/*
 * The PM8008i's I2C is bit-banged, on a bus whose lines the phone's own firmware holds as well, and
 * a transfer now and then goes unanswered -- more often the longer the run, and most often right
 * after the camera has been switched off and on again. Taken for a fault the first time, the camera
 * was gone for the rest of the run (give_up: nothing more is written); asked again, the chip
 * answers. The number of goes that were needed is said at the end of the run.
 */
#define PM8008_GOES             4

static uint64_t pm8008_retries;

static bool
pm8008_get(uint8_t address, uint16_t reg, uint8_t *values, unsigned count)
{
	uint8_t where[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

	for (unsigned go = 0; go < PM8008_GOES; go++) {
		if (i2c_transfer(&pmic_bus, address, where, 2, values, count)) {
			pm8008_retries += go;
			return true;
		}
		wait_ms(1);
	}
	pm8008_retries += PM8008_GOES;
	return false;
}

/* The registers the PM8008i may be written in: the chip enable, and the set point, mode and enable of the sensor's LDOs. */
static bool
pm8008_put(uint8_t address, uint16_t reg, uint8_t value, bool force)
{
	uint8_t out[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value }, back = 0xff;
	uint16_t base = reg & 0xff00, offset = reg & 0xff;
	bool allowed = (address == PM8008_CHIP && reg == MISC_CHIP_ENABLE) ||
	    (address == PM8008_REGULATORS && base >= LDO_BASE(1) && base <= LDO_BASE(7) && sensor_ldo((base - LDO_BASE(1)) / 0x100 + 1, 0) &&
	    (offset == LDO_MODE_CTL1 || offset == LDO_ENABLE || offset == LDO_VSET_LB || offset == LDO_VSET_UB));

	if ((broken && !force) || !allowed) {
		give_up(allowed ? "a write to the PM8008i after a fault" : "a PM8008i register that is not to be written");
		return false;
	}
	for (unsigned go = 0; go < PM8008_GOES; go++) {
		if (i2c_transfer(&pmic_bus, address, out, 3, NULL, 0) && pm8008_get(address, reg, &back, 1) && back == value) {
			pm8008_retries += go;
			return true;
		}
		wait_ms(1);
	}
	say("camera: PM8008i %#04x:%#06x <- %#04x: %s (reads %#04x), %u goes\n", address, reg, value, back == 0xff ? "no answer" :
	    "does not read back", back, PM8008_GOES);
	give_up("a write to the PM8008i that failed");
	return false;
}

static void
show_ldo(unsigned n)
{
	uint8_t status = 0, vset[2] = { 0, 0 }, mode = 0, enable = 0;

	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_VSET_LB), vset, 2) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_MODE_CTL1), &mode, 1) &&
	    pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1)) {
		say("camera: PM8008i LDO%u: status %#04x (%sready%s, mode %u), set to %u mV, mode control %#04x, enable %#04x\n", n,
		    status, status & 0x80 ? "" : "not ", status & 0x20 ? ", OVER-CURRENT" : "", status & 3, vset[1] << 8 | vset[0], mode,
		    enable);
	} else {
		say("camera: PM8008i LDO%u: no answer\n", n);
	}
}

/*
 * An LDO on as Android's regulator framework puts it on (set point, load -> normal mode, enable),
 * then ready within 10 tries of the driver's wait, and not over its current.
 */
static bool ldo_was_on[8], ldo_switched[8];

static bool
ldo_on(unsigned n, unsigned mv)
{
	uint16_t base = (uint16_t)LDO_BASE(n);
	uint8_t enable = 0, mode = 0, status = 0;

	if (!sensor_ldo(n, mv)) {
		give_up("a voltage that is not Android's");
		return false;
	}
	if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), &enable, 1) ||
	    !pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), &mode, 1)) {
		give_up("a PM8008i that does not answer");
		return false;
	}
	ldo_was_on[n] = enable & 0x80;
	if (ldo_was_on[n]) {
		say("camera: PM8008i LDO%u was on already: left as it is\n", n);
		return true;
	}
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_LB), (uint8_t)mv, false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_VSET_UB), (uint8_t)(mv >> 8), false) ||
	    !pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_MODE_CTL1), (uint8_t)((mode & ~7) | 7), false)) {
		return false;
	}
	ldo_switched[n] = true;
	if (!pm8008_put(PM8008_REGULATORS, (uint16_t)(base + LDO_ENABLE), (uint8_t)(enable | 0x80), false)) {
		return false;
	}
	for (int tries = 0, allones = 0; tries < 10; tries++) {
		spin_us(20 + mv * 1000 / 38400 + 1);   /* the driver's wait: 20 us and the set point at the stepper's 38.4 mV/us */
		if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_STATUS1), &status, 1)) {
			give_up("a PM8008i that does not answer");
			return false;
		}
		/*
		 * A byte with every bit set is not a status: a good one reads 0x87, and the bits between
		 * "ready" and the mode are not used. It is what this bus reads when the PM8008i has not
		 * answered properly, as it does for a few goes right after the front camera's rails have
		 * gone down -- and it was read as an over-current, which switched the camera off for the
		 * rest of the run just as Camera.app went back to the wide camera. Ask again.
		 */
		if (status == 0xff) {
			if (allones++ == 0) {
				say("camera: PM8008i LDO%u: a status of all ones, which is no answer: asking again\n", n);
			}
			continue;
		}
		if (status & 0x20) {
			uint8_t again = 0;

			/* as healthy() does while it runs: an over-current counts when it is still there */
			wait_ms(2);
			if (pm8008_get(PM8008_REGULATORS, (uint16_t)(base + LDO_STATUS1), &again, 1) && !(again & 0x20)) {
				say("camera: PM8008i LDO%u: status %#04x, and %#04x when asked again\n", n, status, again);
				status = again;
			} else {
				say("camera: PM8008i LDO%u reports over-current (status %#04x)\n", n, status);
				give_up("an LDO over its current");
				return false;
			}
		}
		if (status & 0x80) {
			say("camera: PM8008i LDO%u on at %u mV (status %#04x)\n", n, mv, status);
			return true;
		}
	}
	say("camera: PM8008i LDO%u not ready (status %#04x)\n", n, status);
	give_up(status == 0xff ? "a PM8008i whose status is all ones" : "an LDO that is not ready");
	return false;
}

static void
ldo_off(unsigned n)
{
	uint8_t enable = 0;

	if (!ldo_switched[n]) {
		return;
	}
	if (pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), &enable, 1) &&
	    pm8008_put(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_ENABLE), (uint8_t)(enable & ~0x80), true)) {
		ldo_switched[n] = false;
		say("camera: PM8008i LDO%u off\n", n);
	} else {
		say("camera: PM8008i LDO%u COULD NOT BE SWITCHED OFF\n", n);
	}
}

/* ---- the camera's clocks: camcc --------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x80000UL
#define GCC_CAMERA_AHB_CBCR     0xb008          /* (gcc-kona.c: critical) camcc is read and written only with these two on */
#define GCC_CAMERA_XO_CBCR      0xb040
#define GCC_CAMERA_HF_AXI_CBCR  0xb02c
#define GCC_CAMERA_SF_AXI_CBCR  0xb030
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL
#define MCLK_CMD_RCGR           (0x5000 + 0x1c * hw->mclk)      /* [0] update, [31] the root is off */
#define MCLK_CFG_RCGR           (MCLK_CMD_RCGR + 4)     /* [4:0] divider (2n - 1), [10:8] source (0: the crystal), [13:12] M/N mode */
#define MCLK_M                  (MCLK_CMD_RCGR + 8)
#define MCLK_N                  (MCLK_CMD_RCGR + 12)
#define MCLK_D                  (MCLK_CMD_RCGR + 16)
#define MCLK_CBCR               (0x5018 + 0x1c * hw->mclk)      /* [0] enable, [31] the clock is off */
#define TITAN_TOP_GDSCR         0xc144          /* [0] collapse, [11] keep the registers, [31] on */
#define CBCR_OFF                (1u << 31)
#define CMD_UPDATE              1u
#define GDSC_COLLAPSE           1u
#define GDSC_ON                 (1u << 31)

static vm_offset_t gcc, camcc;
static uint32_t found_mclk_cfg_rcgr, found_mclk_cbcr, found_gdscr;
static bool gdsc_switched, mclk_switched;

/*
 * GCC's camera AHB and XO branches: on before camcc is touched at all (a camcc read with them off is an
 * external abort: umi_camera.c), and as they were found when the kernel leaves.
 */
static uint32_t found_gcc_ahb, found_gcc_xo;
static bool gcc_switched;

static bool
gcc_camera_bus(void)
{
	if (gcc_switched) {
		return true;
	}
	found_gcc_ahb = rd(gcc + GCC_CAMERA_AHB_CBCR);
	found_gcc_xo = rd(gcc + GCC_CAMERA_XO_CBCR);
	gcc_switched = true;
	wr(gcc + GCC_CAMERA_AHB_CBCR, found_gcc_ahb | 1);
	wr(gcc + GCC_CAMERA_XO_CBCR, found_gcc_xo | 1);
	spin_us(10);
	if (!(rd(gcc + GCC_CAMERA_AHB_CBCR) & 1) || !(rd(gcc + GCC_CAMERA_XO_CBCR) & 1)) {
		say("camera: GCC's camera AHB (%#x) and XO (%#x) clocks do not come on: camcc is not touched\n",
		    rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
		return false;
	}
	say("camera: GCC's camera AHB and XO clocks on (were %#x and %#x)\n", found_gcc_ahb, found_gcc_xo);
	return true;
}

static void
gcc_camera_bus_back(void)
{
	if (gcc_switched) {
		wr(gcc + GCC_CAMERA_XO_CBCR, (rd(gcc + GCC_CAMERA_XO_CBCR) & ~1u) | (found_gcc_xo & 1));
		wr(gcc + GCC_CAMERA_AHB_CBCR, (rd(gcc + GCC_CAMERA_AHB_CBCR) & ~1u) | (found_gcc_ahb & 1));
		gcc_switched = false;
	}
}

static void
show_clocks(void)
{
	say("camera: GCC: camera AHB %#x, camera XO %#x\n", rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
	say("camera: camcc: titan top GDSC %#x; the sensor's MCLK RCG cmd %#x cfg %#x m %#x n %#x d %#x, branch %#x\n", rd(camcc + TITAN_TOP_GDSCR),
	    rd(camcc + MCLK_CMD_RCGR), rd(camcc + MCLK_CFG_RCGR), rd(camcc + MCLK_M), rd(camcc + MCLK_N), rd(camcc + MCLK_D),
	    rd(camcc + MCLK_CBCR));
}

/* camcc's registers: only these, each read back (an RCG's update bit and a GDSC's status by waiting for them) */
static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	if (offset != TITAN_TOP_GDSCR && offset != MCLK_CFG_RCGR && offset != MCLK_CMD_RCGR && offset != MCLK_CBCR) {
		give_up("a camcc register that is not to be written");
		return false;
	}
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check_mask) != (value & check_mask)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_bits(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static bool
gdsc_on(void)
{
	found_gdscr = rd(camcc + TITAN_TOP_GDSCR);
	if (!(found_gdscr & GDSC_COLLAPSE) && (found_gdscr & GDSC_ON)) {
		say("camera: the titan top power domain was on already\n");
		return true;
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found_gdscr & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	spin_us(1);
	if (!wait_bits(TITAN_TOP_GDSCR, GDSC_ON, GDSC_ON, 100)) {
		say("camera: the titan top power domain does not come on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
		give_up("a power domain that does not come on");
		return false;
	}
	say("camera: the titan top power domain on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
	return true;
}

static void
gdsc_off(void)
{
	if (!gdsc_switched) {
		return;
	}
	wr(camcc + TITAN_TOP_GDSCR, rd(camcc + TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
	gdsc_switched = false;
	say("camera: the titan top power domain %s (%#x)\n", wait_bits(TITAN_TOP_GDSCR, GDSC_ON, 0, 100) ? "off" : "NOT OFF",
	    rd(camcc + TITAN_TOP_GDSCR));
}

/* MCLK at 19.2 MHz (every sensor's here): the RCG from the crystal, divider 1, no M/N; then the branch; then the pin. */
static bool
mclk_on(uint32_t khz)
{
	found_mclk_cfg_rcgr = rd(camcc + MCLK_CFG_RCGR);
	found_mclk_cbcr = rd(camcc + MCLK_CBCR);
	if (khz != 19200) {
		give_up("an MCLK rate other than the crystal's");
		return false;
	}
	if (found_mclk_cbcr & 1) {
		give_up("MCLK is on already (somebody else's)");
		return false;
	}
	mclk_switched = true;
	if (!camcc_put(MCLK_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | 0u << 8 | 1u, 0x371f) ||
	    !camcc_put(MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500)) {
		say("camera: MCLK's RCG does not take its configuration (cmd %#x)\n", rd(camcc + MCLK_CMD_RCGR));
		give_up("an RCG that does not update");
		return false;
	}
	if (!camcc_put(MCLK_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(MCLK_CBCR, CBCR_OFF, 0, 500)) {
		say("camera: MCLK's branch does not come on (%#x)\n", rd(camcc + MCLK_CBCR));
		give_up("a clock that does not come on");
		return false;
	}
	cfg_put(hw->mclk_pin, CFG_FUNCTION(1) | CFG_DRIVE_MA(4));       /* cam_mclk, no pull, 4 mA: cam_sensor_mclkN_active */
	say("camera: MCLK on at %u.%u MHz (RCG cfg %#x, branch %#x)\n", khz / 1000, khz % 1000 / 100, rd(camcc + MCLK_CFG_RCGR),
	    rd(camcc + MCLK_CBCR));
	return true;
}

static void
mclk_off(void)
{
	if (!mclk_switched) {
		return;
	}
	if (pins_found) {
		wr(GPIO_CFG(hw->mclk_pin), found_mclk_cfg);
	}
	wr(camcc + MCLK_CBCR, rd(camcc + MCLK_CBCR) & ~1u);
	(void)wait_bits(MCLK_CBCR, CBCR_OFF, CBCR_OFF, 500);
	wr(camcc + MCLK_CFG_RCGR, found_mclk_cfg_rcgr);
	wr(camcc + MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE);
	(void)wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500);
	mclk_switched = false;
	say("camera: MCLK off (branch %#x, RCG cfg %#x as found)\n", rd(camcc + MCLK_CBCR), rd(camcc + MCLK_CFG_RCGR));
}

/* ---- the sensor's tables, its power and its ID ---------------------------------------------------------- */

/*
 * The sensor's tables are Android's, read at run time from fw/ of the small image, where
 * userland/build.sh puts what devices/redmi-k40/tools/alioth-camsensor.py makes of the sensor's module
 * file ("RWCTAB02"): the slave address and the chip ID's read, the lane assignment, the modes, the power
 * sequences (Android's cam_sensor kinds: 0 MCLK, 1 VANA, 2 VDIG, 3 VIO, 6 CUSTOM1, 8 RESET, with their
 * delays) and the register tables (init, res0.., on, off, holdon, holdoff).
 */
struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;  /* (a power sequence's: kind, value -- MCLK in kHz) */
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static const struct ctab_head *ctab;
static const struct ctab_mode *ctab_modes;

/* The fake-camera path validates iOS's capture graph without touching sensor MMIO. */
static const struct ctab_head fake_ctab = { .magic = "RWCTAB02", .modes = 1 };
static const struct ctab_mode fake_mode = {
    .width = 1280, .height = 720, .lanes = 4, .pixel_clock = 9000000,
    .line_length = 800, .frame_length = 750, .fps_milli = 15000, .data_type = 0x2b,
};
static const uint8_t fake_power[24];
static const uint32_t fake_tables;
static bool fake;

/* the sensor the rest works on: its descriptor, its CCI master's pins (its tables are loaded afresh: ctab_load()) */
static void
select_sensor(unsigned i)
{
	hw_index = i;
	hw = &sensors[i];
	cci_bus.name = hw->bus_name;
	cci_bus.sda = hw->sda;
	cci_bus.scl = hw->scl;
	ctab = NULL;
}
static const uint8_t *ctab_power, *ctab_tables;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	const uint8_t *at, *end;

	if (fake) {
		ctab = &fake_ctab;
		ctab_modes = &fake_mode;
		ctab_power = fake_power;
		ctab_tables = (const uint8_t *)&fake_tables;
		ctab_size = sizeof(fake_ctab) + sizeof(fake_mode) + sizeof(fake_power) + sizeof(fake_tables);
		say("camera: UMI capture-pipeline check: synthetic 1280x720 RAW10 stream; no camera rails, pins or registers touched\n");
		return true;
	}
	ctab = (const struct ctab_head *)hakios_wifi_file(hw->tab, &ctab_size);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16) {
		say("camera: no %s (RWCTAB02) in fw/ of the small image (made from fw-camera/ when it is built)\n", hw->tab);
		ctab = NULL;
		return false;
	}
	end = (const uint8_t *)ctab + ctab_size;
	ctab_modes = (const struct ctab_mode *)(ctab + 1);
	ctab_power = (const uint8_t *)(ctab_modes + ctab->modes);
	at = ctab_power;
	for (int p = 0; p < 2; p++) {
		if (at + 12 > end) {
			ctab = NULL;
			return false;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	ctab_tables = at;
	if (ctab_tables + 4 > end) {
		ctab = NULL;
		return false;
	}
	say("camera: the %s sensor's tables: slave %#x, chip ID %#x at %#x, lanes %#x, %u modes, %u tables\n", hw->name, ctab->slave,
	    ctab->id, ctab->id_address, ctab->lane_assign, ctab->modes, *(const uint32_t *)ctab_tables);
	for (uint32_t m = 0; m < ctab->modes; m++) {
		const struct ctab_mode *o = &ctab_modes[m];

		say("camera:   mode %u: %ux%u at %u,%u, %u %s, settle %u, pixel clock %u, line %u, frame %u, %u.%03u frames/s\n", m, o->width,
		    o->height, o->x, o->y, o->lanes, o->cphy ? "trio(s), C-PHY" : "lane(s), D-PHY", o->settle, o->pixel_clock,
		    o->line_length, o->frame_length, o->fps_milli / 1000, o->fps_milli % 1000);
	}
	return true;
}

static const struct ctab_entry *
ctab_block(const uint8_t *at, uint32_t blocks, const char *name, uint32_t *count)
{
	const uint8_t *end = (const uint8_t *)ctab + ctab_size;

	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	return ctab == NULL ? NULL : ctab_block(ctab_tables + 4, *(const uint32_t *)ctab_tables, name, count);
}

static uint8_t
sensor_address(void)
{
	return (uint8_t)(ctab->slave >> 1);    /* (the module file's is the 8-bit one) */
}

static bool chip_enable_switched, powered, reset_driven;

/*
 * A PM8150A LDO through RPMh ("ldocN": PMIC5 LDO N of the PM8150A, slave 4, peripheral 0x40 + N - 1),
 * as Android's rpmh-regulator asks for it: its voltage, high power mode, enabled (umi_rpmh.c). One found
 * enabled already (somebody else's vote: L2 is the display's DVDD) is left as it is, and never switched
 * off here; one switched on here is switched off again with the sensor.
 */
extern int umi_rpmh_camera_ldo(const char *rail, unsigned mv, bool enable);
static bool pmic_peek(uint16_t id, uint8_t reg, uint8_t *values, unsigned count);
#define RAIL_SLOTS              8               /* (ldoc1 to ldoc8) */
static bool rail_switched[RAIL_SLOTS + 1];

static unsigned
rail_number(const struct supply *s)
{
	unsigned n = s->rail != NULL && strncmp(s->rail, "ldoc", 4) == 0 ? (unsigned)(s->rail[4] - '0') : 0;

	return n >= 1 && n <= RAIL_SLOTS && s->rail[5] == '\0' ? n : 0;
}

/* the LDO's status (STATUS1: [7] ready) and enable (EN_CTL: [7]) as the PMIC has them; false if they cannot be read */
static bool
rail_state(unsigned n, uint8_t *status, uint8_t *enable)
{
	uint16_t id = (uint16_t)(0x400 | (0x40 + n - 1));

	return pmic_peek(id, 0x08, status, 1) && pmic_peek(id, 0x46, enable, 1);
}

static bool
rail_on(const struct supply *s)
{
	unsigned n = rail_number(s);
	uint8_t status = 0, enable = 0;
	int error;

	if (n == 0) {
		give_up("a supply not known here");
		return false;
	}
	if (rail_switched[n]) {
		return true;
	}
	/*
	 * (The PM8150A's LDOs are RPMh's: the arbiter gives this side no channel to read them by. L2 is the
	 * display's DVDD as well, on for as long as the panel shows anything: it is not asked for here.)
	 */
	bool known = rail_state(n, &status, &enable);
	static bool l2_voted;

	if (!known && n == 2) {
		/*
		 * Not to be found on, as it turned out: the macro sensor held its bus down with only that (its VDIG)
		 * left to the display. Voted on at 1.2 V (the device tree's for both), and never off here.
		 */
		if (!l2_voted) {
			int error = umi_rpmh_camera_ldo(s->rail, s->mv, true);

			say("camera: PM8150A L2 (ldoc2, the display's DVDD too) on at %u mV through RPMh%s, never off here\n", s->mv,
			    error == 0 ? "" : " -- NOT TAKEN");
			l2_voted = error == 0;
			spin_us(500);
		}
		return true;
	}
	if (known && (enable & 0x80) && (status & 0x80)) {
		say("camera: PM8150A L%u (%s) was on already (status %#04x): left as it is\n", n, s->rail, status);
		return true;
	}
	error = umi_rpmh_camera_ldo(s->rail, s->mv, true);
	if (error != 0) {
		say("camera: RPMh did not take %s at %u mV (error %d)\n", s->rail, s->mv, error);
		give_up("a rail RPMh did not take");
		return false;
	}
	rail_switched[n] = true;
	if (!known) {
		/* (RPMh has said done: the LDO is on by the time it does -- rpmh-regulator's own wait is its ramp's) */
		spin_us(500);
		say("camera: PM8150A L%u (%s) on at %u mV through RPMh\n", n, s->rail, s->mv);
		return true;
	}
	for (int tries = 0; tries < 20; tries++) {
		if (rail_state(n, &status, &enable) && (status & 0x80)) {
			say("camera: PM8150A L%u (%s) on at %u mV through RPMh (status %#04x)\n", n, s->rail, s->mv, status);
			return true;
		}
		spin_us(100);
	}
	say("camera: PM8150A L%u (%s) not ready (status %#04x, enable %#04x)\n", n, s->rail, status, enable);
	give_up("a rail that is not ready");
	return false;
}

static void
rail_off(const struct supply *s)
{
	unsigned n = rail_number(s);

	if (n != 0 && rail_switched[n]) {
		int error = umi_rpmh_camera_ldo(s->rail, s->mv, false);

		rail_switched[n] = error != 0;
		say("camera: PM8150A L%u (%s) %s\n", n, s->rail, error == 0 ? "off" : "COULD NOT BE SWITCHED OFF");
	}
}

static bool
supply_on(const struct supply *s)
{
	if (s->kind == SUPPLY_LDO) {
		return ldo_on(s->ldo, s->mv);
	}
	if (s->kind == SUPPLY_RPMH) {
		return rail_on(s);
	}
	give_up("a supply this sensor does not have");
	return false;
}

static void
supply_off(const struct supply *s)
{
	if (s->kind == SUPPLY_LDO) {
		ldo_off(s->ldo);
	} else if (s->kind == SUPPLY_RPMH) {
		rail_off(s);
	}
}

static void
sensor_delay(uint32_t us)
{
	if (us >= 1000) {
		wait_ms(us / 1000);
		us %= 1000;
	}
	spin_us(us);
}

/* One step of the module file's power sequence (`up': part of the power-up, where a fault stops it). */
static bool
power_step(const struct ctab_entry *e, bool up)
{
	const struct supply *s = e->address == 1 ? &hw->vana : e->address == 2 ? &hw->vdig : e->address == 3 ? &hw->vio :
	    e->address == 6 ? &hw->custom1 : NULL;

	switch (e->address) {
	case 8:
		reset_driven = true;
		reset_pin(e->data != 0);
		break;
	case 0:
		if (e->data != 0 && e->data != 19200) {
			give_up("an MCLK rate not known here");
			return false;
		}
		if (e->data != 0) {
			if (!up || !gdsc_on() || !mclk_on(e->data)) {
				return false;
			}
		} else {
			mclk_off();
		}
		break;
	case 1: case 2: case 3: case 6:
		if (s->kind == SUPPLY_NONE) {
			give_up("a supply the device tree does not give this sensor");
			return false;
		}
		if (e->data != 0) {
			if (!up || !supply_on(s)) {
				return false;
			}
		} else {
			supply_off(s);
		}
		break;
	default:
		say("camera: a power step of kind %u, which this does not know\n", e->address);
		give_up("a power sequence not understood");
		return false;
	}
	sensor_delay(e->delay_us);
	return true;
}

/* Everything that was switched on, off again: the module file's power-down, then the rest as it was found. */
static void
power_down(void)
{
	bool was = powered || mclk_switched || gdsc_switched || reset_driven || chip_enable_switched || pm_reset_changed;
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrdn", &count) : NULL;

	for (unsigned n = 1; n <= 7; n++) {
		was |= ldo_switched[n];
	}
	for (unsigned n = 1; n <= RAIL_SLOTS; n++) {
		was |= rail_switched[n];
	}
	if (!was) {
		return;
	}
	say("camera: powering down\n");
	for (uint32_t i = 0; e != NULL && i < count; i++) {
		(void)power_step(&e[i], false);
	}
	/* (whatever the sequence did not reach) */
	mclk_off();
	if (reset_driven) {
		reset_pin(false);
	}
	supply_off(&hw->custom1);
	supply_off(&hw->vio);
	supply_off(&hw->vana);
	supply_off(&hw->vdig);
	gdsc_off();
	/*
	 * The PM8008 itself stays as it is between one use of the camera and the next: out of reset and
	 * enabled, with its LDOs off (they are switched above). (On the POCO X5 Pro's PM8008i, put back into
	 * reset it came out again, but its regulators were deaf from then on.) It is put back as it was found
	 * when the kernel leaves (hakios_camera_quiesce), which is where Android gets the phone back.
	 */
	if (leaving && chip_enable_switched) {
		uint8_t enable = 0;

		if (pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1) &&
		    pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable & ~1), true)) {
			say("camera: the PM8008's chip enable back off\n");
		} else {
			say("camera: the PM8008's chip enable COULD NOT BE PUT BACK\n");
		}
		chip_enable_switched = false;
	}
	if (leaving && pm_reset_changed) {
		wr(GPIO_IO(PIN_PMIC_RESET), pm_reset_found_io & 2);
		wr(GPIO_CFG(PIN_PMIC_RESET), pm_reset_found_cfg);
		pm_reset_changed = false;
		say("camera: the PM8008's reset pin (gpio%u) back as it was found\n", PIN_PMIC_RESET);
	}
	if (reset_driven && pins_found) {
		wr(GPIO_IO(hw->reset), found_reset_io & 2);
		wr(GPIO_CFG(hw->reset), found_reset_cfg);
		reset_driven = false;
	}
	powered = false;
	if (pm8008_retries != 0) {
		say("camera: powered down (the PM8008 needed %llu more goes on its I2C)\n", pm8008_retries);
	} else {
		say("camera: powered down\n");
	}
}

/* The PM8008 out of reset (its reset pin as Android drives it) and enabled. */
static bool
pm8008_up(void)
{
	uint8_t revid[4] = { 0 }, enable = 0;

	if (!pm_reset_changed) {        /* (as it was found, which is what the run's end puts back) */
		pm_reset_found_cfg = rd(GPIO_CFG(PIN_PMIC_RESET));
		pm_reset_found_io = rd(GPIO_IO(PIN_PMIC_RESET));
	}
	if (!pm_reset_active()) {
		say("camera: the PM8008 is held in reset (gpio%u cfg %#x io %#x): let out as Android lets it out\n", PIN_PMIC_RESET,
		    rd(GPIO_CFG(PIN_PMIC_RESET)), rd(GPIO_IO(PIN_PMIC_RESET)));
		pm_reset_changed = true;
		wr(GPIO_IO(PIN_PMIC_RESET), 2);
		cfg_put(PIN_PMIC_RESET, CFG_OUTPUT | CFG_PULL_UP | CFG_DRIVE_MA(2));
		if (!pm_reset_active()) {
			give_up("the PM8008's reset pin does not read back");
			return false;
		}
		wait_ms(5);
	}
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		give_up("the PM8008 does not answer");
		return false;
	}
	say("camera: the PM8008 answers: revision %02x %02x %02x %02x\n", revid[0], revid[1], revid[2], revid[3]);
	if (!pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1)) {
		give_up("the PM8008 does not answer");
		return false;
	}
	if (!(enable & 1)) {
		if (!pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, (uint8_t)(enable | 1), false)) {
			return false;
		}
		chip_enable_switched = true;
		say("camera: the PM8008's chip enabled\n");
	}
	return true;
}

/* The module file's power-up, step by step. */
static bool
power_up(void)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrup", &count) : NULL;

	if (e == NULL) {
		give_up("no power sequence");
		return false;
	}
	powered = true;
	if (!pm8008_up()) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!power_step(&e[i], true)) {
			return false;
		}
	}
	say("camera: the %s sensor powered up (%u steps)\n", hw->name, count);
	return true;
}

static bool
read_chip_id(void)
{
	uint8_t where[2] = { (uint8_t)(ctab->id_address >> 8), (uint8_t)ctab->id_address }, id[4] = { 0, 0, 0, 0 };
	unsigned out = ctab->address_bytes == 2 ? 2 : 1, in = ctab->data_bytes >= 1 && ctab->data_bytes <= 4 ? ctab->data_bytes : 2;
	uint32_t chip = 0;

	/*
	 * The first word asked of the sensor after its supplies have come up. Powered up again within a
	 * run -- the capture graph stops and starts the stream as the application reconfigures its
	 * session -- it is not always ready by the time the power sequence's own waits are over, and one
	 * unanswered transfer used to end the camera for the rest of the run. It is asked again, for as
	 * long as the sensor's own data sheet would have it waiting.
	 */
	unsigned go;

	for (go = 0; go < 20; go++) {
		if (i2c_transfer(&cci_bus, sensor_address(), out == 2 ? where : where + 1, out, id, in)) {
			break;
		}
		wait_ms(2);
	}
	if (go == 20) {
		say("camera: the %s sensor does not answer at %#04x\n", hw->name, sensor_address());
		return false;
	}
	if (go != 0) {
		say("camera: the %s sensor answered at %#04x after %u more goes (%u ms)\n", hw->name, sensor_address(), go, go * 2);
	}
	for (unsigned i = 0; i < in; i++) {
		chip = chip << 8 | id[i];
	}
	say("camera: the %s sensor's chip ID: %#06x (%s)\n", hw->name, chip, (chip & ctab->id_mask) == (ctab->id & ctab->id_mask) ?
	    "as Android expects" : "NOT what Android expects");
	return (chip & ctab->id_mask) == (ctab->id & ctab->id_mask);
}

/*
 * While powered up: the LDOs switched on still ready and not over their current. An over-current is
 * asked again 2 ms on before it counts: the lens motor's moves show one on LDO5 for an instant (at a
 * move of a few hundred steps, now and then), and the camera went on as before every time it was not
 * looked at then.
 */
static bool
healthy(void)
{
	for (unsigned n = 1; n <= 7; n++) {
		uint8_t status = 0;

		if (!ldo_switched[n]) {
			continue;
		}
		for (int look = 0; look < 2; look++) {
			if (!pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) || !(status & 0x80)) {
				break;
			}
			if (!(status & 0x20)) {
				break;
			}
			if (look == 0) {
				say("camera: PM8008i LDO%u: status %#04x, asked again\n", n, status);
				wait_ms(2);
			}
		}
		if (!(status & 0x80) || (status & 0x20)) {
			say("camera: PM8008i LDO%u: status %#04x\n", n, status);
			give_up("an LDO that is no longer well");
			return false;
		}
	}
	return true;
}

/* ---- the camera subsystem's surroundings, read only: rails, the SMMU, camcc's PLLs ------------------- */

/*
 * What the camera's blocks need beyond the sensor (a look first, nothing written): the MMCX and MX
 * rails' levels (camcc's clocks are on MMCX, which the display holds at NOM; its PLLs on MX), as RPMh
 * has them in the boot loader's command database; what the application processors' RPMh sequencer
 * (DRV 2) holds for them; the CSIPHYs' supplies (PM8150 L9 at 1.2 V, the display's DSI PHY's too, and
 * the reference generator at 0x88e7000, which the DSI PHY needs as well); what the SMMU has for the
 * IFEs' streams (dt: 0x800, 0x840 and their aliases, the mask 0x400); camcc's PLL0 to PLL4.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* DRV 2's (dt: rsc@18200000, drv-id 2) */
#define RSC_TCS_BASE            0xd00
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define RSC_TCS_COUNT           8               /* dt qcom,tcs-config: 2 active, 3 sleep, 3 wake */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
static const struct cmd_db_header *cmd_db;
static vm_offset_t rsc;

/* RPMh's address for a resource, and its levels (as many as `max'); 0: not there. */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned max, unsigned *count)
{
	char id[8] = { 0 };

	*count = 0;
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(id) && name[i] != '\0'; i++) {
		id[i] = name[i];        /* (cmd-db's names are eight bytes, not NUL-terminated at eight: "mmcx.lvl") */
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return entry[i].address;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			for (unsigned l = 0; l < entry[i].length / 2u && l < max; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rail(const char *name, uint32_t *address)
{
	uint16_t levels[16];
	unsigned count;
	char line[200];
	int at;

	*address = rpmh_resource(name, levels, 16, &count);
	at = snprintf(line, sizeof(line), "camera: RPMh's %s at %#x, levels:", name, *address);
	for (unsigned l = 0; l < count && at < (int)sizeof(line) - 8; l++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[l]);
	}
	say("%s\n", line);
	/* what DRV 2's sequencer holds for it, in any of its TCSs */
	for (unsigned t = 0; *address != 0 && t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = rd(rsc + RSC_TCS(t) + TCS_ENABLED);

		for (unsigned c = 0; c < 16; c++) {
			uint32_t where = rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 4);

			if (where == *address) {
				say("camera:   TCS %u command %u: value %u (position), %senabled, message id %#x, status %#x\n", t, c,
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 8), (enabled >> c) & 1 ? "" : "not ",
				    rd(rsc + RSC_TCS(t) + TCS_COMMAND(c)), rd(rsc + RSC_TCS(t) + TCS_COMMAND(c) + 12));
			}
		}
	}
}

/* A PMIC peripheral's registers, read through the arbiter's observer (any channel that is that peripheral's). */
static bool
pmic_peek(uint16_t id, uint8_t reg, uint8_t *values, unsigned count)
{
	for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		for (unsigned i = 0; i < count; i++) {
			vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * n;

			lock();
			wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)(reg + i) << 4);
			const char *problem = wait_done(channel);
			values[i] = (uint8_t)rd(channel + OBSERVER_DATA0);
			unlock();
			if (problem != NULL) {
				return false;
			}
		}
		return true;
	}
	return false;
}

/* a PMIC5 regulator (LDO or SMPS) as its peripheral has it: type, subtype, status, set point, enable */
static void
show_vreg(const char *what, uint16_t id)
{
	uint8_t head[2] = { 0, 0 }, set[2] = { 0, 0 }, status = 0, mode = 0, enable = 0;

	if (!pmic_peek(id, 0x04, head, 2) || !pmic_peek(id, 0x40, set, 2) || !pmic_peek(id, 0x08, &status, 1) ||
	    !pmic_peek(id, 0x45, &mode, 1) || !pmic_peek(id, 0x46, &enable, 1)) {
		say("camera: %s (peripheral %#x): cannot be read\n", what, id);
		return;
	}
	say("camera: %s (peripheral %#x): type %#04x subtype %#04x, status %#04x (%sready), %u mV, mode %#04x, enable %#04x\n",
	    what, id, head[0], head[1], status, status & 0x80 ? "" : "not ", set[1] << 8 | set[0], mode, enable);
}

#define SMMU_PHYS               0x15000000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
/*
 * The IFEs' data streams (dt msm_cam_smmu_ife: 0x800, 0x840 and their aliases 0xc00, 0xc40), each matched
 * with the mask 0x400 -- as umi_camera_dma.c found the Mi 10's SMMU takes them (it refused 0x801 and a
 * combined 0x441 mask).
 */
#define IFE_STREAM_A            0x800
#define IFE_STREAM_B            0x840
#define IFE_STREAM_MASK         0x400

static vm_offset_t smmu;

static void
show_smmu(void)
{
	uint32_t scr0 = rd(smmu + SMMU_SCR0), idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), free_matches = 0;
	bool used[256] = { false };
	unsigned contexts = idr1 & 0xff, free_contexts = 0;

	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (!(smr >> 31)) {
			free_matches++;
			continue;
		}
		used[s2cr & 0xff] = true;
		for (uint32_t i = 0; i < 4; i++) {
			uint32_t sid = (i & 1 ? IFE_STREAM_B : IFE_STREAM_A) | (i & 2 ? IFE_STREAM_MASK : 0);

			if (((stream ^ sid) & ~mask) == 0) {
				say("camera: SMMU match register %u (stream %#x mask %#x, s2cr %#x) covers the IFEs' stream %#x\n", n, stream,
				    mask, s2cr, sid);
				break;
			}
		}
	}
	for (unsigned c = 0; c < contexts; c++) {
		free_contexts += !used[c];
	}
	say("camera: the SMMU: scr0 %#x (a stream nobody matches %s), %u match registers free, %u of %u contexts unused by them\n", scr0,
	    scr0 & (1u << 10) ? "faults" : "passes", free_matches, free_contexts, contexts);
}

static void
show_plls(void)
{
	for (unsigned p = 0; p <= 4; p++) {
		vm_offset_t pll = camcc + 0x1000UL * p;

		say("camera: camcc PLL%u: mode %#x, l %#x, cal_l %#x, user_ctl %#x %#x, config_ctl %#x %#x %#x, opmode %#x, status %#x, "
		    "alpha %#x\n", p, rd(pll + 0x00), rd(pll + 0x04), rd(pll + 0x08), rd(pll + 0x0c), rd(pll + 0x10), rd(pll + 0x18),
		    rd(pll + 0x1c), rd(pll + 0x20), rd(pll + 0x38), rd(pll + 0x30), rd(pll + 0x40));
	}
}

#define REFGEN_PHYS             0x088e7000UL    /* dt refgen-regulator@88e7000 (refgen-kona: [0] of PWRDWN_CTRL5 at 0x80 the enable) */

static vm_offset_t refgen;

static void
look_around(void)
{
	uint32_t mmcx = 0, mx = 0;

	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, 0x10000);
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
		refgen = ml_io_map(REFGEN_PHYS, 0x1000);
	}
	show_rail("mmcx.lvl", &mmcx);
	show_rail("mx.lvl", &mx);
	show_vreg("PM8150 L9 (the CSIPHYs' 1.2 V)", 0x048);
	show_vreg("PM8150A L2 (ldoc2)", 0x441);
	show_vreg("PM8150A L3 (ldoc3)", 0x442);
	show_vreg("PM8150A L5 (ldoc5)", 0x444);
	show_vreg("PM8150A L7 (ldoc7, the lens motor's)", 0x446);
	show_vreg("PM8150A S8 (the PM8008's L1/L2 input)", 0x429);
	say("camera: the reference generator: bias %#x, bg %#x, pwrdwn_ctrl5 %#x\n", rd(refgen + 0x08), rd(refgen + 0x14), rd(refgen + 0x80));
	show_smmu();
	show_plls();
}

/* ---- the camera subsystem (Titan 165): rails, PLL0, clocks ------------------------------------------- */

/*
 * cam=3: the path a picture takes, tried first with the CSID's own test pattern generator, so that
 * nothing outside the SoC is involved: CSID-lite 0 (0xacd9200, "csid-lite480": the layout of Linux's
 * camss-csid-gen2.c) makes RAW10 frames of a colour bar and sends them down its RDI 0 path to
 * IFE-lite 0 (0xacd9000, "vfe-lite480": camss-vfe-480.c, bus version 3), whose write master 0 writes
 * each frame as it comes (MIPI RAW, frame mode) to memory through the SMMU, as the IFEs' streams
 * 0x800 and 0x840 (mask 0x400), into a context of ours: what umi_camera_dma.c found to work on the
 * Mi 10. The memory is one physically contiguous piece that the loader hands to the host after the run
 * ("xl camera"; tools/camraw.py makes pictures of it).
 *
 * What it takes (camcc-sm8250.c, gcc-sm8250.c, the device tree's clock rates of csid-lite0, ife-lite0
 * and the sensor's csiphy, and camss's of vfe_lite0):
 *   RPMh   MMCX at NOM for camcc's clocks (the display's vote already), MX at NOM for PLL0 -- votes of
 *          umi_rpmh.c, held until reset (a vote of DRV 2 replaces the last one of DRV 2: never lower)
 *   GCC    camera_hf_axi (0xb02c), camera_sf_axi (0xb030)
 *   camcc  PLL0 (Lucid, 1200 MHz: L 0x3e, alpha 0x8000, its config and user values), then the RCGs
 *          slow_ahb 80 MHz, camnoc_axi 400, cphy_rx 400, csiNphytimer 300, ife_lite_csid 400,
 *          ife_lite 400 (all from PLL0's even (600) or odd (400) output) and the branches
 *          cpas_ahb, ife_lite_ahb, ife_lite, ife_lite_csid, ife_lite_cphy_rx, csiphyN, csiNphytimer,
 *          camnoc_dcd_xo, camnoc_axi, ife_lite_axi and GCC's two (N is the sensor's CSIPHY)
 * Everything is taken back in the reverse order at the end, and on any fault (but the votes).
 */
extern int umi_rpmh_arc_level(const char *res, unsigned corner);
static bool rails_voted;

/* MMCX and MX at NOM (256), once a run. True: RPMh has said done. */
static bool
rails(bool on)
{
	if (!on || rails_voted) {
		return true;
	}
	int mmcx = umi_rpmh_arc_level("mmcx.lvl", 256), mx = umi_rpmh_arc_level("mx.lvl", 256);

	say("camera: RPMh: MMCX at NOM %s, MX at NOM %s\n", mmcx == 0 ? "taken" : "NOT TAKEN", mx == 0 ? "taken" : "NOT TAKEN");
	rails_voted = mmcx == 0 && mx == 0;
	return rails_voted;
}

/* camcc's PLL0 (Lucid): the registers of clk-alpha-pll.c's Lucid layout */
#define PLL_MODE                0x00            /* [0] outputs, [2] out of reset, [23] update bypass, [31] locked */
#define PLL_L_VAL               0x04
#define PLL_CAL_L_VAL           0x08
#define PLL_USER_CTL            0x0c            /* [2:0] the outputs: main, even, odd */
#define PLL_USER_CTL_U          0x10
#define PLL_USER_CTL_U1         0x14
#define PLL_CONFIG_CTL          0x18
#define PLL_CONFIG_CTL_U        0x1c
#define PLL_CONFIG_CTL_U1       0x20
#define PLL_OPMODE              0x38            /* 0 standby, 1 run */
#define PLL_ALPHA_VAL           0x40

static bool pll_on;

static bool
camcc_any(uint32_t offset, uint32_t value, uint32_t check)
{
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check) != (value & check)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
pll0_up(void)
{
	static const struct {
		uint32_t offset, value;
	} config[] = {                  /* camcc-sm8250.c: cam_cc_pll0_config (1200 MHz); CAL_L as clk-alpha-pll.c's Lucid default */
		{ PLL_L_VAL, 0x3e }, { PLL_CAL_L_VAL, 0x44 }, { PLL_ALPHA_VAL, 0x8000 }, { PLL_CONFIG_CTL, 0x20485699 },
		{ PLL_CONFIG_CTL_U, 0x00002261 }, { PLL_CONFIG_CTL_U1, 0x329a699c }, { PLL_USER_CTL, 0x00003100 },
		{ PLL_USER_CTL_U, 0x00000805 }, { PLL_USER_CTL_U1, 0x00000000 },
	};

	if (rd(camcc + PLL_MODE) & 1) {
		give_up("camcc's PLL0 is running already (somebody else's)");
		return false;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (!camcc_any(config[i].offset, config[i].value, ~0u)) {
			return false;
		}
	}
	pll_on = true;
	/* clk_lucid_pll_configure(): update bypass, outputs off, standby, out of reset */
	if (!camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1u << 23, 1u << 23) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) & ~1u, 1) ||
	    !camcc_any(PLL_OPMODE, 0, 1) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1u << 2, 1u << 2)) {
		return false;
	}
	/* alpha_pll_lucid_enable(): run, locked, the outputs on */
	if (!camcc_any(PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(PLL_MODE, 1u << 31, 1u << 31, 1000)) {
		say("camera: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc + PLL_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_any(PLL_USER_CTL, rd(camcc + PLL_USER_CTL) | 7, 7) || !camcc_any(PLL_MODE, rd(camcc + PLL_MODE) | 1, 1)) {
		return false;
	}
	say("camera: camcc's PLL0 locked at 1200 MHz (mode %#x, user_ctl %#x)\n", rd(camcc + PLL_MODE), rd(camcc + PLL_USER_CTL));
	return true;
}

static void
pll0_down(void)
{
	if (!pll_on) {
		return;
	}
	/* alpha_pll_lucid_disable(), then back into reset as it was found */
	wr(camcc + PLL_MODE, rd(camcc + PLL_MODE) & ~1u);
	wr(camcc + PLL_USER_CTL, rd(camcc + PLL_USER_CTL) & ~7u);
	wr(camcc + PLL_OPMODE, 0);
	wr(camcc + PLL_MODE, rd(camcc + PLL_MODE) & ~(1u << 2));
	pll_on = false;
	say("camera: camcc's PLL0 off (mode %#x)\n", rd(camcc + PLL_MODE));
}

/* An RCG set (source, divider field 2n - 1, no M/N) and updated; then branches on. */
struct rcg {
	const char *    name;
	uint32_t        cmd, source, divider;
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {                    /* (camcc-sm8250.c's parent map 0: 2 PLL0 even, 600 MHz; 3 PLL0 odd, 400 MHz) */
	{ "slow_ahb 80 MHz", 0x7058, 2, 14 },           /* PLL0 even / 7.5 */
	{ "camnoc_axi 400 MHz", 0xc0f8, 3, 1 },         /* PLL0 odd / 1 */
	{ "cphy_rx 400 MHz", 0xa068, 3, 1 },
	{ "csiNphytimer 300 MHz", 0, 2, 3 },            /* PLL0 even / 2 (the sensor's CSIPHY's: 0x6000 + 0x20 N) */
	{ "ife_lite_csid 400 MHz", 0xc01c, 3, 1 },
	{ "ife_lite 400 MHz", 0xc000, 3, 1 },
};
struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
/* (in the order umi_camera.c found them to come on in: the blocks' own first, the memory's path last) */
static struct branch branches[] = {
	{ "cpas_ahb", false, 0xc0f4 }, { "ife_lite_ahb", false, 0xc040 }, { "ife_lite", false, 0xc018 },
	{ "ife_lite_csid", false, 0xc034 }, { "ife_lite_cphy_rx", false, 0xc03c }, { "csiphyN", false, 0 },
	{ "csiNphytimer", false, 0 }, { "camnoc_dcd_xo", false, 0xc11c }, { "camnoc_axi", false, 0xc114 },
	{ "ife_lite_axi", false, 0xc044 },
	{ "gcc camera_hf_axi", true, GCC_CAMERA_HF_AXI_CBCR }, { "gcc camera_sf_axi", true, GCC_CAMERA_SF_AXI_CBCR },
};

static bool
clocks_up(void)
{
	/* (the sensor's CSIPHY: its own branch and timer) */
	rcgs[3].cmd = 0x6000 + 0x20 * hw->csiphy;
	branches[5].cbcr = 0x601c + 0x20 * hw->csiphy;
	branches[6].cbcr = 0x6018 + 0x20 * hw->csiphy;
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4;

		r->found_cfg = rd(camcc + cfg);
		if (!camcc_any(cfg, (r->found_cfg & ~0x371fu) | r->source << 8 | r->divider, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
		if (!wait_bits(r->cmd, CMD_UPDATE, 0, 500)) {
			say("camera: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc + r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (rd(base + b->cbcr) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(base + b->cbcr, rd(base + b->cbcr) | 1);
		if (!(rd(base + b->cbcr) & 1)) {
			say("camera: the branch %s does not take its enable (%#x)\n", b->name, rd(base + b->cbcr));
			give_up("a clock that does not read back");
			return false;
		}
		b->set = true;
		for (unsigned waited = 0; rd(base + b->cbcr) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("camera: the branch %s does not come on (%#x)\n", b->name, rd(base + b->cbcr));
				give_up("a clock that does not come on");
				return false;
			}
			spin_us(10);
		}
	}
	say("camera: the camera's clocks on (%u RCGs, %u branches)\n", (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])),
	    (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
clocks_down(void)
{
	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc : camcc;

		if (b->set) {
			wr(base + b->cbcr, rd(base + b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc + r->cmd + 4, r->found_cfg);
			wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
			(void)wait_bits(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
	say("camera: the camera's clocks off\n");
}

/* ---- the frames' memory, and the SMMU ---------------------------------------------------------------- */

#define FRAME_ADDRESSES         0x40000000ull           /* the IFE's addresses for it (in dt's iova pool 0x100000-0xfff00000) */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.sound.c's */

/* What the loader hands to the host (tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;

static vm_offset_t frames, smmu_second, smmu_context, page_tables;
static vm_offset_t keep;                /* memory in one piece, taken at boot (hakios_camera_init()) */
static uint64_t keep_bytes;
static uint64_t frames_bytes;
static uint32_t smmu_match[2] = { ~0u, ~0u }, smmu_context_n, smmu_old_smr[2], smmu_old_s2cr[2], smmu_pages_size, smmu_pages;
static bool smmu_on;

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	/* (and room after the last frame: the write master goes a little past a frame's end -- 11 KB, seen at 4000x3000) */
	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes + (256u << 10));
	if (keep != 0 && keep_bytes >= frames_bytes) {
		frames = keep;                  /* (the piece taken at boot: hakios_camera_init()) */
		bzero((void *)frames, CAM_HEADER_BYTES);
	} else if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = 0;                   /* (RGGB: the CSID's test pattern; a sensor's picture: capture_sensor()) */
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, !source ? "CSID TPG" : hw->chip, sizeof(h->sensor));
	/* (what the processor wrote out of its caches; the IFE writes past them) */
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("camera: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

/*
 * Two free match registers and a context no match register sends anything to, switched off and without
 * a fault (umi_camera_dma.c's choice). The contexts' pages are mapped once each (a session's every start
 * comes here).
 */
static vm_offset_t smmu_contexts[128];

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), contexts = idr1 & 0xff;
	unsigned found = 0;

	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000 || contexts > 128) {
		say("camera: the SMMU's layout is not the one known here\n");
		return false;
	}
	smmu_match[0] = smmu_match[1] = ~0u;
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			for (unsigned i = 0; i < 2; i++) {
				if ((((smr & 0x7fff) ^ (i ? IFE_STREAM_B : IFE_STREAM_A)) & ~((smr >> 16 & 0x7fff) | IFE_STREAM_MASK)) == 0) {
					say("camera: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
					return false;
				}
			}
		} else if (found < 2) {
			smmu_match[found] = n;
			smmu_old_smr[found] = smr;
			smmu_old_s2cr[found] = s2cr;
			found++;
		}
	}
	if (found < 2) {
		smmu_match[0] = smmu_match[1] = ~0u;
		say("camera: the SMMU has no two match registers left\n");
		return false;
	}
	if (smmu_second == 0) {
		smmu_second = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
	}
	for (smmu_context_n = 0; smmu_context_n < contexts; smmu_context_n++) {
		if (used[smmu_context_n]) {
			continue;
		}
		if (smmu_contexts[smmu_context_n] == 0) {
			smmu_contexts[smmu_context_n] = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
		}
		smmu_context = smmu_contexts[smmu_context_n];
		if (!(rd(smmu_context + SMMU_CB_SCTLR) & 1) && !(rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL)) {
			return true;
		}
	}
	smmu_match[0] = smmu_match[1] = ~0u;
	say("camera: the SMMU has no context left\n");
	return false;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFEs' streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);

	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match registers %u and %u send streams %#x and %#x (mask %#x) "
	    "through it\n", smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match[0], smmu_match[1], IFE_STREAM_A, IFE_STREAM_B,
	    IFE_STREAM_MASK);
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) || !smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) ||
	    !smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) || !smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) || !smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) || !smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
	wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	for (unsigned waited = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; waited++) {
		if (waited > 1000) {
			lck_mtx_unlock(&hakios_smmu_lock);
			give_up("the SMMU does not finish forgetting");
			return false;
		}
		spin_us(10);
	}
	if (!smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	smmu_on = true;
	for (unsigned i = 0; i < 2; i++) {
		/* (the route as umi_camera_dma.c gives it: the context, and [9:8] 3, non-shareable output on this MMU-500) */
		if (!smmu_put(smmu, SMMU_S2CR(smmu_match[i]), smmu_context_n | 3u << 8, 0x300ff) ||
		    !smmu_put(smmu, SMMU_SMR(smmu_match[i]), 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | (i ? IFE_STREAM_B : IFE_STREAM_A),
		    ~0u)) {
			lck_mtx_unlock(&hakios_smmu_lock);
			return false;
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return true;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && smmu_match[0] == ~0u) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	for (unsigned i = 0; i < 2; i++) {
		if (smmu_match[i] != ~0u) {
			wr(smmu + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
			wr(smmu + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
		}
	}
	if (smmu_on) {
		uint32_t fsr = rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL;

		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
		if (fsr != 0) {
			wr(smmu_context + SMMU_CB_FSR, fsr);    /* (a fault of ours not left for the next one to find the context by) */
		}
	}
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_match[0] = smmu_match[1] = ~0u;
	say("camera: the SMMU's match registers and context put back as they were found\n");
}

/* ---- CSID-lite 0 and IFE-lite 0 (Titan 480) ----------------------------------------------------------- */

/*
 * IFE-lite 0's registers are at 0xacd9000 and CSID-lite 0's among them, at 0xacd9200 (the device tree's
 * ife-lite0 and csid-lite0): one mapping of IFE-lite 0's 0x2200 bytes, the CSID's 0x200 into it, as
 * umi_camera.c has it. The CSIPHYs are at 0xac6a000 + 0x2000 N.
 */
#define IFE_PHYS                0x0acd9000UL
#define IFE_SIZE                0x2200UL
#define CSID_IN_IFE             0x200
#define CSIPHY_PHYS(n)          (0x0ac6a000UL + 0x2000UL * (n))
#define CSIPHYS                 6

/* CSID gen2, the lite one (camss-csid-gen2.c; Android's cam_ife_csid_lite480.h for the counters) */
#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100           /* [1:0] lanes - 1, [19:4] the lanes' assignment, [22:20] the PHY, [24] C-PHY */
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_LONG_PKT   0x128           /* the last long packet's header (captured) */
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_FORMAT        0x278           /* the frame as measured: [29:16] lines, [15:0] the line's pixels */
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define RDI_IRQ_OVERFLOW        (1u << 2)       /* (Android's CSID_PATH_*: the path's FIFO) */
#define RDI_IRQ_EOF             (1u << 9)
#define RDI_IRQ_SOF             (1u << 12)
#define RDI_IRQ_PIX_COUNT       (1u << 13)
#define RDI_IRQ_LINE_COUNT      (1u << 14)
#define TPG_VC                  0xa
#define RAW10_DT                0x2b
/*
 * A line's bytes as the write master lays them out: RAW10 (four pixels in five bytes), the line rounded
 * up to 16 bytes, the bus's width -- a 4000-pixel line takes 5008, not 5000. (Taken for 5000, the IMX582's
 * own colour bars came out slanted, and the frame ran 24 KB past its place.)
 */
#define RAW10_STRIDE(width)     (((width) * 10 / 8 + 15) & ~15u)

/* VFE lite 480 and its bus, version 3 (camss-vfe-480.c, the lite one's offsets; RDI 0's write master is 0) */
#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x000c          /* 2: the hardware and the registers */
#define IFE_REG_UPDATE          0x0020          /* [n] RDI n */
#define IFE_IRQ_CMD             0x0024          /* [0] the clears take effect */
#define IFE_IRQ_MASK0           0x0028          /* [17] the reset done, [4] the bus's */
#define IFE_IRQ_MASK1           0x002c
#define IFE_IRQ_CLEAR0          0x0034
#define IFE_IRQ_CLEAR1          0x0038
#define IFE_IRQ_STATUS0         0x0040
#define IFE_IRQ_STATUS1         0x0044
#define IFE_RESET_DONE          (1u << 17)
#define BUS_HW_VERSION          0x1a00
#define BUS_CGC_OVERRIDE        0x1a08
#define BUS_IRQ_MASK0           0x1a18          /* [n] RDI n's register update taken, [4 + n] its frame written */
#define BUS_IRQ_MASK1           0x1a1c
#define BUS_IRQ_CLEAR0          0x1a20
#define BUS_IRQ_CLEAR1          0x1a24
#define BUS_IRQ_STATUS0         0x1a28
#define BUS_IRQ_STATUS1         0x1a2c
#define BUS_IRQ_GLOBAL_CLEAR    0x1a30
#define BUS_ADDR_SYNC           0x1a34          /* (umi_camera_dma.c writes 0) */
#define BUS_VIOLATION0          0x1a68          /* (umi_camera_dma.c reads these after its frame) */
#define BUS_VIOLATION1          0x1a70
#define BUS_TEST_CTRL           0x1adc
#define WM0_CFG                 0x1c00          /* [0] on, [16] 1 MIPI RAW */
#define WM0_IMAGE_ADDR          0x1c04
#define WM0_FRAME_INC           0x1c08
#define WM0_IMAGE_CFG_0         0x1c0c          /* 0xffff: the frame as it comes */
#define WM0_IMAGE_CFG_1         0x1c10
#define WM0_IMAGE_CFG_2         0x1c14          /* the stride */
#define WM0_PACKER              0x1c18
#define WM0_IRQ_SUB_PERIOD      0x1c30
#define WM0_IRQ_SUB_PATTERN     0x1c34
#define WM0_FRAMEDROP_PERIOD    0x1c38
#define WM0_FRAMEDROP_PATTERN   0x1c3c
#define WM0_BURST_LIMIT         0x1c64
#define BUS_RUP0                1u              /* RDI 0's register update taken: the address given is the frame's now */
#define BUS_DONE0               (1u << 4)       /* RDI 0's (comp group 0's) frame written */

static vm_offset_t csid, ife, csiphy;
static bool csid_running, ife_running, tpg_running;
static uint32_t ife_found_mask;

/* A register of the three camera blocks: written, and read back where it is one that keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/*
 * The CSID's interrupts masked and cleared, its paths off, then its two resets -- the hardware's, then
 * the registers' -- each waited for in its top status (bit 0): umi_camera.c's, which passed on the Mi 10.
 */
static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 0, false);
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0x0fffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 0, 1000)) {
			say("camera: the CSID's top status does not clear (%#x)\n", rd(csid + CSID_TOP_IRQ_STATUS));
		}
		if (hw == 0) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw == 0 ? 0x1e : 0x1, false);
		if (hw == 1) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("camera: the CSID's %s reset does not finish (top irq %#x)\n", hw == 0 ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 0, false);
	say("camera: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/*
 * The RX (from PHY `phy', or the test pattern generator: `phy' -1) and RDI 0, for RAW10 of vc/dt, the
 * payload as it comes (camss-csid-gen2.c's __csid_configure_rx() and __csid_configure_rdi_stream()).
 * Boot-arg cam_phy_base=N: the PHY's number counted from N (Android's newer CSIDs count from 1).
 */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height, bool cphy)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u, base = 0;

	(void)PE_parse_boot_argn("cam_phy_base", &base, sizeof(base));
	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)cphy << 24 |
	    (uint32_t)(phy < 0 ? 0 : ((uint32_t)phy + base) & 7) << 20, true) ||
	    !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0) {
		/* one data type, frames without end, blanking 0x3ff lines and 0x740 pixels, RAW10, colour bars (umi_camera_dma.c's) */
		if (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 1u << 10 | vc, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
		    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
			return false;
		}
	}
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	/* (every RX event and error latched for a look, and the path's start and end of frame, overflow and count errors) */
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0x0fffffff, false);
	blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, RDI_IRQ_OVERFLOW | RDI_IRQ_EOF | RDI_IRQ_SOF | RDI_IRQ_PIX_COUNT | RDI_IRQ_LINE_COUNT,
	    false);
	/* resume at the frame's boundary */
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true);
}

static bool
tpg_start(void)
{
	/* the test pattern on, its frame start and end packets, four lanes' worth (umi_camera_dma.c's) */
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	tpg_running = true;
	return true;
}

/*
 * IFE-lite 0 reset (command 2, its done at bit 17: umi_camera.c's), then RDI 0's write master as
 * camss-vfe-480.c's vfe_wm_start() has it -- frame mode, MIPI RAW, a frame every frame -- the frame's
 * address, and the register update that makes it the next frame's.
 */
static bool
ife_config(uint64_t address, uint32_t stride, uint32_t frame_inc)
{
	ife_found_mask = rd(ife + IFE_IRQ_MASK0);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, IFE_RESET_DONE, false);
	blk_put(ife, "IFE", IFE_IRQ_CMD, 1, false);
	(void)wait_reg(ife, IFE_IRQ_STATUS0, IFE_RESET_DONE, 0, 1000);
	blk_put(ife, "IFE", IFE_IRQ_MASK0, IFE_RESET_DONE, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 2, false);
	if (!wait_reg(ife, IFE_IRQ_STATUS0, IFE_RESET_DONE, IFE_RESET_DONE, 100000)) {
		say("camera: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CMD, 1, false);
	say("camera: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	blk_put(ife, "IFE", BUS_CGC_OVERRIDE, 0x3ffffff, false);
	blk_put(ife, "IFE", BUS_ADDR_SYNC, 0, false);
	blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false);
	blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, false);
	blk_put(ife, "IFE", WM0_IMAGE_CFG_0, 0xffff, false);
	blk_put(ife, "IFE", WM0_IMAGE_CFG_1, 0, false);
	blk_put(ife, "IFE", WM0_IMAGE_CFG_2, stride, false);
	blk_put(ife, "IFE", WM0_PACKER, 0, false);
	blk_put(ife, "IFE", WM0_FRAME_INC, frame_inc, false);
	blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, false);
	blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, false);
	blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, false);
	blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, false);
	blk_put(ife, "IFE", BUS_IRQ_CLEAR0, BUS_RUP0 | BUS_DONE0, false);
	blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
	blk_put(ife, "IFE", BUS_IRQ_MASK0, BUS_RUP0 | BUS_DONE0, false);
	blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false);
	blk_put(ife, "IFE", WM0_CFG, 1u | 1u << 16, false);
	ife_running = true;
	if (rd(ife + WM0_IMAGE_ADDR) != (uint32_t)address || rd(ife + WM0_IMAGE_CFG_2) != stride) {
		say("camera: the write master reads address %#x (given %#x), stride %u (given %u)\n", rd(ife + WM0_IMAGE_ADDR),
		    (uint32_t)address, rd(ife + WM0_IMAGE_CFG_2), stride);
	}
	/* RDI 0's register update: the address is the next frame's */
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 1, false);
}

static void
show_paths(const char *when)
{
	say("camera: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u, last long packet %#x; RDI0 irq %#x status %#x, frame %ux%u, "
	    "bytes %u/%u; IFE irq %#x %#x; bus irq %#x %#x, violations %#x %#x; WM0 cfg %#x address %#x\n", when,
	    rd(csid + CSID_CSI2_RX_IRQ_STATUS), rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_CSI2_RX_ECC), rd(csid + CSID_CSI2_RX_CRC),
	    rd(csid + CSID_CSI2_RX_LONG_PKT), rd(csid + CSID_RDI0_IRQ_STATUS), rd(csid + CSID_RDI0_STATUS),
	    rd(csid + CSID_RDI0_FORMAT) & 0xffff, rd(csid + CSID_RDI0_FORMAT) >> 16 & 0x3fff, rd(csid + CSID_RDI0_BYTES_PING),
	    rd(csid + CSID_RDI0_BYTES_PONG), rd(ife + IFE_IRQ_STATUS0), rd(ife + IFE_IRQ_STATUS1), rd(ife + BUS_IRQ_STATUS0),
	    rd(ife + BUS_IRQ_STATUS1), rd(ife + BUS_VIOLATION0), rd(ife + BUS_VIOLATION1), rd(ife + WM0_CFG), rd(ife + WM0_IMAGE_ADDR));
}

/* the CSID's latched RX and RDI 0 events, cleared (for a look at what comes next) */
static void
csid_clear(void)
{
	wr(csid + CSID_CSI2_RX_IRQ_CLEAR, rd(csid + CSID_CSI2_RX_IRQ_STATUS));
	wr(csid + CSID_RDI0_IRQ_CLEAR, rd(csid + CSID_RDI0_IRQ_STATUS));
	wr(csid + CSID_IRQ_CMD, 1);
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		/* RDI 0 halted at once, then the path and the RX off (umi_camera_dma.c's order) */
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_CSI2_RX_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		csid_clear();
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 1);
		spin_us(1000);
		wr(ife + BUS_IRQ_MASK0, 0);
		wr(ife + BUS_IRQ_CLEAR0, BUS_RUP0 | BUS_DONE0);
		wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
		wr(ife + IFE_IRQ_MASK0, 0);
		wr(ife + IFE_IRQ_CLEAR0, IFE_RESET_DONE);
		wr(ife + IFE_IRQ_CMD, 1);
		wr(ife + IFE_IRQ_MASK0, ife_found_mask);
		ife_running = false;
	}
	if (csid != 0) {
		wr(csid + CSID_CSI2_RX_CFG0, 0);
	}
}

/* The camera's blocks up to their clocks; false (and everything taken back by titan_down()) on any fault. */
static bool
titan_up(void)
{
	static vm_offset_t csiphys[CSIPHYS];

	if (ife == 0) {
		ife = ml_io_map(IFE_PHYS, IFE_SIZE);
		csid = ife + CSID_IN_IFE;
	}
	if (hw->csiphy >= CSIPHYS) {
		give_up("a CSIPHY not known here");
		return false;
	}
	if (csiphys[hw->csiphy] == 0) {
		csiphys[hw->csiphy] = ml_io_map(CSIPHY_PHYS(hw->csiphy), 0x2000);
	}
	csiphy = csiphys[hw->csiphy];   /* (the sensor's: another one after a change of sensor) */
	if (!(rd(camcc + TITAN_TOP_GDSCR) & GDSC_ON) && !gdsc_on()) {
		return false;
	}
	if (!rails(true)) {
		give_up("RPMh did not take the camera's rails");
		return false;
	}
	if (!pll0_up() || !clocks_up()) {
		return false;
	}
	say("camera: IFE-lite 0 %#x (bus %#x), CSID-lite 0 %#x\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION), rd(csid + CSID_HW_VERSION));
	return true;
}

static void
titan_down(void)
{
	paths_down();
	smmu_down();
	clocks_down();
	pll0_down();
}

/* cam=3: a few frames of the CSID's test pattern through RDI 0 and IFE-lite 0 into memory. */
static void
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = RAW10_STRIDE(width);
	struct cam_header *h;

	if (!frames_alloc(width, height, stride, 0) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config(-1, 4, 0x3210, TPG_VC, width, height, false) ||
	    !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride, 0)) {
		titan_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		titan_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 10) {
		wait_ms(10);
		if (rd(ife + BUS_IRQ_STATUS0) & BUS_DONE0) {
			h->frames = 1;
			say("camera: write master 0 says its frame is done after %u ms\n", ms + 10);
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	/* (what the IFE wrote, a look: the first bytes of the frame, past the processor's caches) */
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=4: the sensor's own frames ------------------------------------------------------------------ */

/*
 * The sensor's tables are Android's, read at run time: tools/camsensor.py makes camera-ov02b10.tab
 * and camera-s5khm2sd.tab out of the module files (userland/build.sh puts them into fw/ of the small
 * image). The sequence is Android's: power up, the chip ID, the initialisation ("init": the macro's
 * 108 writes, a soft reset and then its registers page by page; the wide one's 1441), the mode's own
 * table ("resN": boot-arg cam_mode=N, or the mode with the fewest pixels: the macro's 1600x1200 RAW10
 * on one lane at 665.6 Mbit/s, 30 frames a second; the wide one's 1920x1080 RAW10 on three C-PHY
 * trios at 3.11 Gbit/s each, 120 frames a second); the sensor's CSIPHY, CSID-lite 0's RX from that
 * PHY and RDI 0, IFE-lite 0's write master as for cam=3; then "on", the frames, "off" (and its 30 ms),
 * and everything down again. The sensor's writes are not read back (some of its registers start
 * something and clear themselves, a soft reset among them); each byte must be acknowledged.
 *
 * The CSIPHY ("qcom,csiphy-v1.2.1": Android's cam_csiphy_1_2_1_hwreg.h, cam_csiphy_core.c): its reset,
 * the common registers with the lanes to enable (from the module file's lane assignment: the macro's
 * data lane 0 and the clock lane; the wide one's three trios), then the two-phase (D-PHY) settings of
 * every lane position or the three-phase (C-PHY) ones of every trio with the row of the data rate's
 * table that covers the rate, the settle count among them, and its interrupts (status only, nothing
 * handled). The settle count is the module file's for the mode (the macro's 20, the wide one's 34,
 * in periods of the 300 MHz timer). Boot-arg cam_settle=N sets it instead.
 */
/* One of the sensor's tables written in order, as Android's sensor driver writes it. */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("camera: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t out[4];
		unsigned n = 0;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("camera: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes,
			    e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (e[i].address_bytes == 2) {
			out[n++] = (uint8_t)(e[i].address >> 8);
		}
		out[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			out[n++] = (uint8_t)(e[i].data >> 8);
		}
		out[n++] = (uint8_t)e[i].data;
		if (broken || !i2c_transfer(&cci_bus, sensor_address(), out, n, NULL, 0)) {
			say("camera: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us != 0) {
			spin_us(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	say("camera: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

static bool csiphy_running;

/* The common part of cam_csiphy_config_dev(): the lanes to enable, then the registers of the PHY's kind. */
static bool
csiphy_common(uint32_t lane_enable, bool cphy)
{
	if (!blk_put(csiphy, "CSIPHY", 0x814, lane_enable, true) || !blk_put(csiphy, "CSIPHY", 0x818, 0x01, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, cphy ? 0x52 : 0x02, true)) {
		return false;
	}
	wr(csiphy + 0x800, 0x03);
	spin_us(1);
	wr(csiphy + 0x800, cphy ? 0x0e : 0x02);
	return true;
}

/* D-PHY: csiphy_2ph_v1_2_1_reg, every lane position (data lanes 0..3 and the clock lane) */
static bool
csiphy_dphy(unsigned settle)
{
	static const uint32_t lanes[5][2] = {           /* each lane position's two blocks: data lane 0, the clock lane, data lanes 1, 2, 3 */
		{ 0x000, 0x900 }, { 0x700, 0xc80 }, { 0x200, 0xa00 }, { 0x400, 0xb00 }, { 0x600, 0xc00 },
	};

	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l][0], m = lanes[l][1];
		bool clock = l == 1;

		if (!blk_put(csiphy, "CSIPHY", b + 0x30, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x0, 0x05, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x8, 0x10, true) || !blk_put(csiphy, "CSIPHY", m + 0x4, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x4, 0x07, true) || !blk_put(csiphy, "CSIPHY", b + 0x04, 0x0c, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x01, true) || !blk_put(csiphy, "CSIPHY", b + 0x34, 0x07, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x10, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x3c, 0xb8, true) || !blk_put(csiphy, "CSIPHY", b + 0x08, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x00, clock ? 0x80 : 0x8d, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x0c, 0xa5, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x38, clock ? 0x1f : 0xfe, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x60, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x28, 0x04, true)) || !blk_put(csiphy, "CSIPHY", 0x800, 0x02, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	return true;
}

/*
 * C-PHY: csiphy_3ph_v1_2_1_reg, every trio position (three), then cam_csiphy_cphy_data_rate_config()'s
 * deltas of data_rate_delta_table_1_2_1 for the first rate that covers this one (its rows go up to
 * 2.5, 3.5 and 4.5 Gsym/s a trio, 2.28 bits a symbol).
 */
static bool
csiphy_cphy(unsigned settle, uint64_t bits_per_trio)
{
	static const uint32_t trios[3][2] = { { 0x100, 0x980 }, { 0x300, 0xa80 }, { 0x500, 0xb80 } };
	static const struct {
		uint64_t        bandwidth;
		uint8_t         r144, r164, r16c, rb4;  /* (rb4: 0 for none) */
	} rates[3] = {
		{ 5700000000ull, 0x32, 0x0b, 0x25, 0 }, { 7980000000ull, 0xb2, 0x33, 0x1d, 0x03 }, { 10260000000ull, 0xb2, 0x33, 0x25, 0x02 },
	};
	unsigned r = 0;

	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", m + 0x10, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x14, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x18, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x10, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x14, 0x08, true) || !blk_put(csiphy, "CSIPHY", m + 0x18, 0x1a, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x0c, 0xaf, true)) {
			return false;
		}
		spin_us(1);
		if (!blk_put(csiphy, "CSIPHY", b + 0x5c, 0x46, true) || !blk_put(csiphy, "CSIPHY", b + 0x68, 0xa0, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x04, 0x06, true) || !blk_put(csiphy, "CSIPHY", b + 0x0c, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x08, (settle >> 8) & 0xff, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x50, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x88, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x8c, 0x7f, true) || !blk_put(csiphy, "CSIPHY", b + 0x90, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x18, 0x3e, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x20, 0x41, true) || !blk_put(csiphy, "CSIPHY", b + 0x24, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x28, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x60, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0xcc, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0xdc, 0x50, true) || !blk_put(csiphy, "CSIPHY", m + 0x04, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x08, 0x05, true) || !blk_put(csiphy, "CSIPHY", m + 0x00, 0x60, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x2c, 0x35, true) || !blk_put(csiphy, "CSIPHY", m + 0x30, 0x01, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x800, 0x0e, true) || !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	while (r < 2 && bits_per_trio > rates[r].bandwidth) {
		r++;
	}
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", b + 0x44, rates[r].r144, true) || !blk_put(csiphy, "CSIPHY", b + 0x64, rates[r].r164, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x6c, rates[r].r16c, true) ||
		    (rates[r].rb4 != 0 && !blk_put(csiphy, "CSIPHY", m + 0x34, rates[r].rb4, true))) {
			return false;
		}
		if (rates[r].rb4 != 0) {
			spin_us(1);
		}
	}
	say("camera: CSIPHY %u: C-PHY rate row %u (up to %llu bit/s a trio) for %llu\n", hw->csiphy, r, rates[r].bandwidth, bits_per_trio);
	return true;
}

/* cam_csiphy_reset(), then the configuration for `lanes' lanes (trios) of the lane assignment, and the interrupt masks. */
static bool
csiphy_up(unsigned lanes, bool cphy, uint32_t lane_assign, unsigned settle, uint64_t bits_per_lane)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0;

	/* cam_csiphy_get_lane_enable(): D-PHY data lane n at bit 2n, the clock lane at 7; C-PHY trio n at bit 2n + 1 */
	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > (cphy ? 2u : 3u)) {
			give_up("a lane assignment not known here");
			return false;
		}
		lane_enable |= cphy ? 1u << (2 * n + 1) : 1u << (2 * n) | 0x80u;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = true;
	if (!csiphy_common(lane_enable, cphy) || !(cphy ? csiphy_cphy(settle, bits_per_lane) : csiphy_dphy(settle))) {
		return false;
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("camera: CSIPHY %u configured: %u %s (lanes enabled %#x), settle count %u\n", hw->csiphy, lanes, cphy ? "trio(s), C-PHY" :
	    "lane(s), D-PHY", lane_enable, settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "camera: CSIPHY %u %s, interrupt status:", hw->csiphy, when);

	for (unsigned i = 0; i < 11 && at < (int)sizeof(line) - 6; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", rd(csiphy + 0x8b0 + 4 * i) & 0xff);
	}
	say("%s\n", line);
}

static void
csiphy_down(void)
{
	if (!csiphy_running) {
		return;
	}
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = false;
}

/*
 * The exposure. Every sensor here has the SMIA registers: the frame length 0x0340, the coarse integration
 * 0x0202 (lines), the analog gain 0x0204, all 16 bits, written between the group hold's on and off (0x0104,
 * the module files' "holdon"/"holdoff" tables) -- the Samsung ones' gain linear (32 is 1x), the Sony ones'
 * a code, gain = 1024 / (1024 - code) (imx355.c's; 16x the most given). The first exposure is the tables'.
 * Boot-args for a look in other light: cam_shutter=LINES, cam_gain=N (in the units of gain_unity: 32 the
 * Samsung ones' 1x, 256 the Sony ones'), cam_vblank=LINES (the frame that much longer).
 */
static bool
sensor_write(uint16_t address, uint16_t value, bool sixteen)
{
	uint8_t out[4] = { (uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(value >> 8), (uint8_t)value };

	return sixteen ? i2c_transfer(&cci_bus, sensor_address(), out, 4, NULL, 0) :
	       i2c_transfer(&cci_bus, sensor_address(), (uint8_t[]){ out[0], out[1], out[3] }, 3, NULL, 0);
}

/* a gain in the units of gain_unity as the sensor's register takes it */
static uint16_t
gain_register(uint32_t gain)
{
	if (hw->regs == REGS_SONY) {
		uint32_t code = 1024 - (1024 * (uint32_t)hw->gain_unity + gain / 2) / (gain ? gain : 1);

		return (uint16_t)(code > 960 ? 960 : code);
	}
	return (uint16_t)gain;
}

/*
 * The wide camera's lens: a voice-coil motor driven by a Dongwoon DW9800 on the sensor's own I2C at 0x0c
 * (the module's actuator, "alioth_dw9800"; Android's actuator@0), powered by the PM8150A's L7 (the device
 * tree's cam_vaf at 2.856 V, RPMh's "ldoc7"). Its initialisation as the POCO X5 Pro's DW9800 has it in its
 * module file: 0x02 = 0x01 (power down), 0x02 = 0x00 and 1 ms, 0x02 = 0x02 (ring control), 0x06 = 0x40
 * (SAC3), 0x07 = 0x07 (its timing), the position 0x190 and 20 ms, then 0x12c. The position is ten bits,
 * 0x03 (the high two) and 0x04 written together. Those registers only are written. The other three
 * cameras' lenses are fixed. When the motor does not answer, L7 is switched on (and back off at the end)
 * and it is asked again; at the end the lens is let down to 0 and the chip powered down.
 */
#define AF_ADDRESS              0x0c
#define AF_CONTROL              0x02            /* [0] power down, [1] ring control */
#define AF_POSITION             0x03            /* and 0x04: ten bits */
#define AF_MODE                 0x06
#define AF_TIMING               0x07

static const struct supply vaf_supply = { SUPPLY_RPMH, 0, 2856, "ldoc7" };
static bool vaf_voted;
static uint32_t af_position = HAKIOS_CAMERA_NO_FOCUS;

/* (a look, for now: what the LDOs say after the motor does something; said only when not all is well) */
static void
af_look(const char *when)
{
	for (unsigned n = 1; n <= 7; n++) {
		uint8_t status = 0;

		if (ldo_switched[n] && pm8008_get(PM8008_REGULATORS, (uint16_t)(LDO_BASE(n) + LDO_STATUS1), &status, 1) &&
		    (status & 0xb8) != 0x80) {
			say("camera: %s: PM8008 LDO%u status %#04x\n", when, n, status);
		}
	}
}

static bool
af_write(uint8_t reg, uint16_t value)
{
	uint8_t out[3] = { reg, (uint8_t)value, 0 };

	if (reg != AF_CONTROL && reg != AF_POSITION && reg != AF_MODE && reg != AF_TIMING) {
		return false;
	}
	if (reg == AF_POSITION) {
		out[1] = (uint8_t)(value >> 8 & 3);
		out[2] = (uint8_t)value;
	}
	return i2c_transfer(&cci_bus, AF_ADDRESS, out, reg == AF_POSITION ? 3 : 2, NULL, 0);
}

/* L7C at 2.856 V, or given back. True: it is on (or off). */
static bool
vaf(bool on)
{
	if (on) {
		vaf_voted = rail_on(&vaf_supply);
		return vaf_voted;
	}
	rail_off(&vaf_supply);
	vaf_voted = false;
	return true;
}

static void
af_up(bool sensor_frames)
{
	af_position = HAKIOS_CAMERA_NO_FOCUS;
	if (!hw->lens || !sensor_frames) {
		return;
	}
	if (!af_write(AF_CONTROL, 0x01)) {
		say("camera: the lens motor (DW9800 at 0x0c) does not answer: its supply on\n");
		if (!vaf(true)) {
			return;
		}
		wait_ms(2);
		af_look("the lens motor's supply on");
		if (!af_write(AF_CONTROL, 0x01)) {
			say("camera: the lens motor does not answer with its supply on either: no focus in this session\n");
			(void)vaf(false);
			return;
		}
	}
	if (!af_write(AF_CONTROL, 0x00)) {
		goto failed;
	}
	wait_ms(1);
	if (!af_write(AF_CONTROL, 0x02) || !af_write(AF_MODE, 0x40) || !af_write(AF_TIMING, 0x07) || !af_write(AF_POSITION, 0x190)) {
		goto failed;
	}
	wait_ms(20);
	if (!af_write(AF_POSITION, 0x12c)) {
		goto failed;
	}
	af_position = 0x12c;
	af_look("the lens motor initialised");
	say("camera: the lens motor (DW9800) is up, the lens at %u\n", af_position);
	return;
failed:
	say("camera: the lens motor stopped answering during its initialisation: no focus in this session\n");
}

static void
af_move(uint32_t position)
{
	if (af_position == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	position = position > 1023 ? 1023 : position;
	if (position != af_position && af_write(AF_POSITION, (uint16_t)position)) {
		char when[48];

		snprintf(when, sizeof(when), "the lens from %u to %u", af_position, position);
		af_position = position;
		af_look(when);
	}
}

static void
af_down(void)
{
	if (af_position != HAKIOS_CAMERA_NO_FOCUS) {
		/* down in steps, not to knock the lens against its stop */
		while (af_position > 0) {
			uint32_t was = af_position;

			af_move(af_position > 48 ? af_position - 48 : 0);
			if (af_position == was) {
				break;
			}
			wait_ms(10);
		}
		(void)af_write(AF_CONTROL, 0x01);
		af_position = HAKIOS_CAMERA_NO_FOCUS;
	}
	if (vaf_voted) {
		(void)vaf(false);
	}
}

static bool
exposure(const struct ctab_mode *mode)
{
	uint32_t shutter = 0, gain = 0, vblank = 0;
	bool ok;

	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	(void)PE_parse_boot_argn("cam_vblank", &vblank, sizeof(vblank));
	if (shutter == 0 && gain == 0 && vblank == 0) {
		return true;
	}
	vblank = vblank > 0x3fff ? 0x3fff : vblank;
	if (shutter > mode->frame_length + vblank - 40) {
		shutter = mode->frame_length + vblank - 40;     /* (within the frame's lines, as the sensors want) */
	}
	gain = gain > hw->gain_max ? hw->gain_max : gain;
	ok = sensor_table("holdon") && (vblank == 0 || sensor_write(0x0340, (uint16_t)(mode->frame_length + vblank), true)) &&
	    (shutter == 0 || sensor_write(0x0202, (uint16_t)shutter, true)) && (gain == 0 || sensor_write(0x0204, gain_register(gain), true)) &&
	    sensor_table("holdoff");
	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	say("camera: the exposure set to %u lines, gain %u (register %#x), %u extra lines a frame\n", shutter, gain,
	    gain ? gain_register(gain) : 0, vblank);
	return true;
}

static void
capture_sensor(void)
{
	uint32_t width, height, stride, settle = 0, pattern = 0, wanted = 8, mode_n = ~0u;
	const struct ctab_mode *mode;
	struct cam_header *h;
	char res[8];
	uint64_t rate;

	if (!ctab_load()) {
		return;
	}
	/* the mode: cam_mode=N, or the one with the fewest pixels */
	if (!PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n)) || mode_n >= ctab->modes) {
		mode_n = 0;
		for (uint32_t m = 1; m < ctab->modes; m++) {
			if ((uint64_t)ctab_modes[m].width * ctab_modes[m].height <
			    (uint64_t)ctab_modes[mode_n].width * ctab_modes[mode_n].height) {
				mode_n = m;
			}
		}
	}
	mode = &ctab_modes[mode_n];
	snprintf(res, sizeof(res), "res%u", mode_n);
	width = mode->width;
	height = mode->height;
	stride = RAW10_STRIDE(width);
	rate = (uint64_t)mode->pixel_clock * 10 / (mode->lanes ? mode->lanes : 1);
	(void)PE_parse_boot_argn("cam_frames", &wanted, sizeof(wanted));
	wanted = wanted < 1 ? 1 : wanted > 300 ? 300 : wanted;
	if (!PE_parse_boot_argn("cam_settle", &settle, sizeof(settle)) || settle == 0) {
		settle = mode->settle;  /* (the module file's, for this mode) */
	}
	if (mode->lanes < 1 || mode->lanes > (mode->cphy ? 3u : 4u) || mode->data_type != RAW10_DT || width % 4 != 0) {
		say("camera: mode %u: %u lanes, data type %#x: not what this is written for\n", mode_n, mode->lanes, mode->data_type);
		return;
	}
	say("camera: mode %u: %ux%u, %u %s at %llu bit/s each, settle count %u\n", mode_n, width, height, mode->lanes,
	    mode->cphy ? "trio(s)" : "lane(s)", rate, settle);
	if (!frames_alloc(width, height, stride, 1)) {
		return;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(res) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, mode->lanes, ctab->lane_assign & ((1u << (4 * mode->lanes)) - 1), 0, width, height,
	    mode->cphy != 0) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride, 0) ||
	    !csiphy_up(mode->lanes, mode->cphy != 0, ctab->lane_assign, settle, rate)) {
		csiphy_down();
		titan_down();
		return;
	}
	show_paths("configured");
	csid_clear();
	h = (struct cam_header *)frames;
	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	/*
	 * The picture's order is the one each sensor's module file gives its modes (resolutionData's
	 * colorFilterArrangement, in CamX's order: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB -- camera.qcom.so's
	 * TranslateColorFilterArrangement turns it into Android's by the table 3, 2, 1, 0): the IMX582's
	 * and the IMX355's RGGB, the S5K3T2's GBRG, the S5K5E9's GRBG (devices/redmi-k40/tools/
	 * alioth-camsensor.py lists it). A sensor whose module file has no colour bars of its own gives
	 * its picture instead.
	 */
	/*
	 * cam_pattern=N: the sensor's own test pattern, SMIA's test_pattern_mode (0x0600, 16 bits: 1 a solid
	 * colour, 2 colour bars, 3 colour bars fading to grey, 4 PN9) -- every sensor here has it. (The module
	 * files' testPatternData tables are its "off".) A known picture: whether the lines come as they should.
	 */
	h->bayer = hw->bayer;
	if ((pattern == 0 || sensor_write(0x0600, (uint16_t)pattern, true)) && sensor_table("on") && exposure(mode)) {
		if (pattern != 0) {
			say("camera: the sensor's test pattern %u on (0x0600)\n", pattern);
		}
		/* frames one after the other into the two places, the last of `wanted' kept (the first ones come before the exposure) */
		for (unsigned ms = 0, done = 0; ms < 5000 && done < wanted; ms += 2) {
			sensor_delay(2000);
			if (ms == 200 && done == 0) {
				show_paths("200 ms after the sensor's start, no frame yet");
				show_csiphy("200 ms after the sensor's start");
			}
			if (!(rd(ife + BUS_IRQ_STATUS0) & BUS_DONE0)) {
				continue;
			}
			done++;
			if (done <= 3 || done == wanted) {
				say("camera: frame %u written, %u ms after the sensor was started\n", done, ms + 2);
			}
			if (done == 1) {
				show_paths("the first frame");
			}
			blk_put(ife, "IFE", BUS_IRQ_CLEAR0, BUS_DONE0 | BUS_RUP0, false);
			blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
			if (done < wanted) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES + (uint64_t)(done & 1) *
				    stride * height), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 1, false);
			}
			h->frames = done < CAM_FRAMES ? done : CAM_FRAMES;
			h->last = (done - 1) & 1;
			if ((done & 7) == 0 && !healthy()) {
				break;
			}
		}
		show_paths("after the frames");
		show_csiphy("after the frames");
		(void)sensor_table("off");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=5: the wide camera for iOS (/dev/hakios-camera) ------------------------------------------- */

/*
 * The sensor's frames for a program (iOS's camera stack in mediaserverd, libhakios.media.dylib), for
 * as long as it holds /dev/hakios-camera open (sys/hakios.camera.h): the sensor powered up and
 * streaming at the first read after the open, everything down again at the last close, after
 * IDLE_MS without a read, and on any fault (and then for the rest of the run). The sequence is
 * cam=4's. The frames go into three places in turn (kernel memory, mapped page by page through the
 * SMMU): one the IFE writes, one a read may be copying, one ready.
 *
 * Which place the IFE writes: each time the write master takes an address (its register update taken,
 * which it does at a frame's start: the bus's RUP of RDI 0, polled every millisecond) the address of the
 * place for the frame after it goes to the write master, which takes it at the next frame's start: a
 * whole frame's time to give it in, not the blanking between two frames. Should no address be waiting
 * then (the write master's frame increment is 0 here: it then writes the frame where the last one
 * went, not past it), that frame is counted as written over. The exposure the program asks for is
 * written between two frames, after the address, in the sensor's group hold; it counts from the second
 * frame after.
 *
 * A picture for the host (HAKIOS_CAMERA_KEEP: what the program made of a frame) goes into one
 * piece of memory the loader hands over after the run, as cam=3 and cam=4's frames do (xl camera).
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame: some come longer than the mode's lines */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         40
#define GAIN_UNITY              (hw->gain_unity)
#define GAIN_MAX                (hw->gain_max)
#define FRAME_LINES_MAX         0xfff0
#define RDI0_SOF                BUS_RUP0        /* BUS_IRQ_STATUS0: the address given taken, at a frame's start */
#define WM0_DONE                BUS_DONE0       /* BUS_IRQ_STATUS0 */

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(ios_lck_grp, "hakios-camera");
static LCK_MTX_DECLARE(ios_lock, &ios_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring;
static uint64_t ring_bytes, ring_tables_bytes, ring_place_bytes;
static uint32_t ring_frame_bytes, ios_odd_frames;
static const struct ctab_mode *ios_mode;
static uint32_t ios_mode_n, ios_line_ns;
static volatile bool ios_open, ios_streaming, ios_wanted;  /* open; frames coming; a read wants them */
static uint64_t ios_last_read, ios_given, ios_frames_done, ios_writes_late;
static struct { uint32_t lines, gain, frame_lines; bool pending; uint32_t focus; bool focus_pending; } ios_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } ios_now, ios_next;
/*
 * What the frames are (boot-arg cam_pattern): 0 the sensor's picture; 1 colour bars -- the sensor's own,
 * where its module file has a table for them (Android's "testPatternData"), else the CSID's; 2 the
 * CSID's colour bars (no sensor at all). The wide camera's module file has no colour bars of the
 * sensor's (its one test pattern table is empty), so cam_pattern=1 gives it the CSID's.
 */
enum { SOURCE_SENSOR, SOURCE_SENSOR_PATTERN, SOURCE_CSID_TPG };
static int ios_source = SOURCE_SENSOR;
/* HAKIOS_CAMERA_SENSOR: the sensor (and mode) to change to, taken between sessions */
static volatile unsigned ios_switch_to;         /* 1 + the sensor's index; 0: none asked for */
static uint32_t ios_switch_mode;

static uint64_t
ios_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* exposure_lines, gain, frame_lines within what the sensor takes (frame_lines 0: the shortest that holds the exposure) */
static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = ios_mode->frame_length;

	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > GAIN_MAX ? GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - EXPOSURE_MARGIN ? FRAME_LINES_MAX - EXPOSURE_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + EXPOSURE_MARGIN > shortest ? *lines + EXPOSURE_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - EXPOSURE_MARGIN) {
		*lines = *frame_lines - EXPOSURE_MARGIN;
	}
}

/*
 * The exposure, gain and frame length, written the sensor's way in its group hold (its module file's
 * "holdon" and "holdoff" tables): 0x0340 frame length, 0x0202 coarse integration (lines), 0x0204
 * analog gain (gain_register(): the Samsung ones' linear, the Sony ones' a code), each 16 bits.
 */
static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	bool ok = sensor_table("holdon") && sensor_write(0x0340, (uint16_t)frame_lines, true) &&
	    sensor_write(0x0202, (uint16_t)lines, true) && sensor_write(0x0204, gain_register(gain), true) && sensor_table("holdoff");

	if (!ok) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	return true;
}

static void
ring_free(void)
{
	if (page_tables != 0) {
		kmem_free(kernel_map, page_tables, ring_tables_bytes);
		page_tables = 0;
	}
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	for (vm_offset_t line = ring; line < ring + ring_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	ring_tables_bytes = (2 + ((ring_bytes + 0x1fffff) >> 21)) * 4096;
	return true;
}

/* The session's end, whatever ended it: the sensor's "off", the blocks, the supplies, the memory. */
static void
ios_stop(const char *why)
{
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	if (!broken && ios_source != SOURCE_CSID_TPG) {
		(void)sensor_table("off");
	}
	af_down();
	/* (only when the paths were set up: a start that failed before them -- a PMIC that did not answer -- has no CSID mapped) */
	if (csid_running || ife_running) {
		show_paths("at the end");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	power_down();
	ring_free();
	say("camera: the frames for iOS stopped (%s): %llu done, %llu given, %llu written over\n", why, ios_frames_done, ios_given,
	    ios_writes_late);
}

static const char *ios_res(void);

static bool
ios_start(void)
{
	uint32_t lines, gain, frame_lines;

	ios_frames_done = ios_given = ios_writes_late = 0;
	ios_odd_frames = 0;
	if (!ring_alloc(RAW10_STRIDE(ios_mode->width) * ios_mode->height)) {
		return false;
	}
	if (ios_source == SOURCE_CSID_TPG) {
		/* the CSID's colour bars in the mode's size, as cam=3 makes them: no sensor, no CSIPHY */
		if (!titan_up() || !smmu_up() || !csid_reset() ||
		    !csid_config(-1, 4, 0x3210, TPG_VC, ios_mode->width, ios_mode->height, false) ||
		    !ife_config(FRAME_ADDRESSES, RAW10_STRIDE(ios_mode->width), 0) || !tpg_start()) {
			return false;
		}
		lck_mtx_lock(&ios_lock);
		for (int i = 0; i < RING; i++) {
			place[i].state = PLACE_FREE;
		}
		place[0].state = PLACE_WRITING;
		lck_mtx_unlock(&ios_lock);
		ios_now.lines = ios_next.lines = ios_mode->frame_length - EXPOSURE_MARGIN;
		ios_now.gain = ios_next.gain = GAIN_UNITY;
		ios_now.frame_lines = ios_next.frame_lines = ios_mode->frame_length;
		ios_next.from = 0;
		say("camera: the frames for iOS: the CSID's colour bars, %ux%u (no sensor)\n", ios_mode->width, ios_mode->height);
		return true;
	}
	if (!power_up() || !healthy() || !read_chip_id() || !sensor_table("init") || !sensor_table(ios_res()) || !healthy() ||
	    !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config((int)hw->csiphy, ios_mode->lanes, ctab->lane_assign & ((1u << (4 * ios_mode->lanes)) - 1), 0, ios_mode->width,
	    ios_mode->height, ios_mode->cphy != 0) || !ife_config(FRAME_ADDRESSES, RAW10_STRIDE(ios_mode->width), 0) ||
	    !csiphy_up(ios_mode->lanes, ios_mode->cphy != 0, ctab->lane_assign, ios_mode->settle,
	    (uint64_t)ios_mode->pixel_clock * 10 / ios_mode->lanes)) {
		return false;
	}
	if (ios_source == SOURCE_SENSOR_PATTERN && !sensor_write(0x0600, 2, true)) {   /* (SMIA's colour bars) */
		return false;
	}
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	place[0].state = PLACE_WRITING;
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	lck_mtx_unlock(&ios_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	if (!sensor_table("on") || !exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	af_up(ios_source == SOURCE_SENSOR);
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	say("camera: the frames for iOS: mode %u, %ux%u, %u lines of %u ns a frame at the shortest; exposure %u lines, gain %#x, "
	    "frame %u lines\n", ios_mode_n, ios_mode->width, ios_mode->height, ios_mode->frame_length, ios_line_ns, lines, gain, frame_lines);
	return true;
}

static void
ios_session(void)
{
	int writing = 0, next = 0;      /* (place 0's address is the write master's from ife_config(): taken at the first SOF) */
	uint64_t last_frame;

	if (!ios_start()) {
		give_up("the camera did not start");
		ios_stop("it did not start");
		return;
	}
	last_frame = ios_ms();
	/* (the bus's status not cleared here: a register update taken already, place 0's address, counts) */
	lck_mtx_lock(&ios_lock);
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	while (!broken && ios_open && ios_switch_to == 0) {
		uint32_t top, bus;
		uint64_t now;

		wait_ms(1);
		now = ios_ms();
		top = bus = rd(ife + BUS_IRQ_STATUS0);
		if (!(top & RDI0_SOF) && !(bus & WM0_DONE)) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				give_up("no frame for 1.5 s");
				break;
			}
			if (now - ios_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		blk_put(ife, "IFE", BUS_IRQ_CLEAR0, bus & (RDI0_SOF | WM0_DONE), false);
		blk_put(ife, "IFE", BUS_IRQ_GLOBAL_CLEAR, 1, false);
		lck_mtx_lock(&ios_lock);
		if (bus & WM0_DONE) {
			uint32_t ping = rd(csid + CSID_RDI0_BYTES_PING), pong = rd(csid + CSID_RDI0_BYTES_PONG);
			uint32_t data = ios_mode->width * 10 / 8 * ios_mode->height;

			last_frame = now;
			ios_frames_done++;
			if ((ping != data || (pong != data && ios_frames_done > 1)) && ios_odd_frames++ < 8) {
				say("camera: frame %llu: the CSID counted %u and %u bytes (the mode's frame is %u)\n", ios_frames_done, ping, pong,
				    data);
			}
			if (ios_frames_done >= ios_next.from && ios_next.from != 0) {
				ios_now = ios_next;
				ios_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = ios_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = ios_now.lines;
			place[writing].gain = ios_now.gain;
			place[writing].frame_lines = ios_now.frame_lines;
		}
		if (top & RDI0_SOF) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last SOF, taken at this one) */
				next = -1;
			} else {
				ios_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			/* the address for the frame after this one, taken at the next SOF: a whole frame's time to give it in */
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes), false);
				blk_put(ife, "IFE", IFE_REG_UPDATE, 1, false);
			}
		}
		bool ask = (bus & WM0_DONE) && ios_ask.pending, focus = (bus & WM0_DONE) && ios_ask.focus_pending;
		uint32_t lines = ios_ask.lines, gain = ios_ask.gain, frame_lines = ios_ask.frame_lines, position = ios_ask.focus;

		ios_ask.pending = ios_ask.pending && !ask;
		ios_ask.focus_pending = ios_ask.focus_pending && !focus;
		lck_mtx_unlock(&ios_lock);
		if (focus) {
			af_move(position);
		}
		if (bus & WM0_DONE) {
			wakeup(&ios_frames_done);
		}
		if (ask && ios_source != SOURCE_CSID_TPG) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != ios_now.lines || gain != ios_now.gain || frame_lines != ios_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				ios_next.lines = lines;
				ios_next.gain = gain;
				ios_next.frame_lines = frame_lines;
				ios_next.from = ios_frames_done + 2;
			}
		}
		if ((bus & WM0_DONE) && ios_frames_done % 30 == 0) {
			uint32_t fsr = rd(smmu_context + SMMU_CB_FSR);

			if (!healthy()) {
				break;
			}
			if (fsr & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (ios_frames_done == 30 || ios_frames_done % 900 == 0) {
				show_paths("streaming");
			}
		}
	}
	ios_stop(broken ? "a fault" : !ios_open ? "closed" : ios_switch_to != 0 ? "another sensor asked for" : "nobody reads");
}

/*
 * cam_fake=1 (with cam=5; for QEMU): /dev/hakios-camera without the phone's camera. The sessions,
 * sensors, modes and frame headers are cam=5's, from the same tables in fw/, but nothing of the phone
 * is touched: the frames are a colour chart made here -- eight bars (white, beige, red, yellow, green,
 * cyan, blue, 18 % grey) over a grey ramp -- in RAW10 in the sensor's own Bayer order, as bright as the
 * exposure asked for makes them, so that everything above the kernel (the ISP of camera_test and of
 * the media library, the capture graph, Camera.app) runs as it does on the phone and has a known
 * picture to show. cam_fake_fps: frames a second (default 5: QEMU's processor is slow).
 */
static bool fake;

/* the bars' linear reflectance, R, G, B in thousandths */
static const uint16_t fake_bars[8][3] = {
	{ 900, 900, 900 }, { 730, 550, 300 }, { 600, 50, 50 }, { 750, 650, 50 },
	{ 80, 450, 100 }, { 50, 500, 600 }, { 50, 100, 550 }, { 180, 180, 180 },
};

/* one line of the chart, RAW10: `ramp' the grey ramp's, else the bars'; `odd' the second line of the cells */
static void
fake_line(uint8_t *out, uint32_t width, bool odd, bool ramp, uint64_t light)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };
	int r = red[hw->bayer & 3], b = blue[hw->bayer & 3];
	uint32_t v[4];

	for (uint32_t x = 0; x < width; x += 4) {
		for (int i = 0; i < 4; i++) {
			uint32_t px = x + (uint32_t)i, site = (odd ? 2u : 0u) + (px & 1), c = site == (uint32_t)r ? 0 : site == (uint32_t)b ? 2 : 1;
			uint64_t refl = ramp ? (uint64_t)px * 1000 / width : fake_bars[px * 8 / width][c];
			/* (2/15 of a 10-bit step per thousandth of reflectance and line at 1x: 18 % grey at AE's 15 % of white near 6000) */
			uint64_t level = 64 + refl * light * 2 / 15000;

			v[i] = level > 1023 ? 1023 : (uint32_t)level;
		}
		uint8_t *g = out + (x / 4) * 5;

		g[0] = (uint8_t)(v[0] >> 2);
		g[1] = (uint8_t)(v[1] >> 2);
		g[2] = (uint8_t)(v[2] >> 2);
		g[3] = (uint8_t)(v[3] >> 2);
		g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
	}
}

/* place p filled with the chart as an exposure of `lines' at `gain' shows it */
static void
fake_fill(int p, uint32_t lines, uint32_t gain)
{
	uint8_t *at = (uint8_t *)(ring + (uint64_t)p * ring_place_bytes);
	uint32_t width = ios_mode->width, height = ios_mode->height, stride = RAW10_STRIDE(width), ramp_from = height * 3 / 4 & ~1u;
	uint64_t light = (uint64_t)lines * gain / GAIN_UNITY;

	fake_line(at, width, false, false, light);
	fake_line(at + stride, width, true, false, light);
	fake_line(at + (uint64_t)ramp_from * stride, width, false, true, light);
	fake_line(at + (uint64_t)(ramp_from + 1) * stride, width, true, true, light);
	for (uint32_t y = 2; y < height; y++) {
		if (y != ramp_from && y != ramp_from + 1) {
			memcpy(at + (uint64_t)y * stride, at + (uint64_t)((y & 1) + (y >= ramp_from ? ramp_from : 0)) * stride, stride);
		}
	}
}

/* a session of cam_fake: ios_session()'s, with the chart for the sensor and a timer for its frames */
static void
fake_session(void)
{
	uint32_t fps = 5, lines, gain, frame_lines, filled_lines[RING] = { 0 }, filled_gain[RING] = { 0 };

	(void)PE_parse_boot_argn("cam_fake_fps", &fps, sizeof(fps));
	fps = fps < 1 ? 1 : fps > 30 ? 30 : fps;
	ios_frames_done = ios_given = ios_writes_late = 0;
	if (!ring_alloc(RAW10_STRIDE(ios_mode->width) * ios_mode->height)) {
		return;
	}
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	exposure_limits(&lines, &gain, &frame_lines);
	ios_now.lines = lines;
	ios_now.gain = gain;
	ios_now.frame_lines = frame_lines;
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	say("camera: the frames for iOS: cam_fake's colour chart, %ux%u in %s's order, %u a second\n", ios_mode->width, ios_mode->height,
	    hw->chip, fps);
	while (ios_open && ios_switch_to == 0) {
		int p = -1;

		wait_ms(1000 / fps);
		lck_mtx_lock(&ios_lock);
		if (ios_ms() - ios_last_read > IDLE_MS) {
			lck_mtx_unlock(&ios_lock);
			break;
		}
		if (ios_ask.pending) {
			lines = ios_ask.lines;
			gain = ios_ask.gain;
			frame_lines = ios_ask.frame_lines;
			ios_ask.pending = false;
			exposure_limits(&lines, &gain, &frame_lines);
			ios_now.lines = lines;
			ios_now.gain = gain;
			ios_now.frame_lines = frame_lines;
		}
		for (int i = 0; i < RING; i++) {
			if (place[i].state != PLACE_READING && (p < 0 || place[i].sequence < place[p].sequence)) {
				p = i;
			}
		}
		if (p >= 0) {
			place[p].state = PLACE_WRITING;
		}
		lck_mtx_unlock(&ios_lock);
		if (p < 0) {
			ios_writes_late++;
			continue;
		}
		if (filled_lines[p] != ios_now.lines || filled_gain[p] != ios_now.gain) {
			fake_fill(p, ios_now.lines, ios_now.gain);
			filled_lines[p] = ios_now.lines;
			filled_gain[p] = ios_now.gain;
		}
		lck_mtx_lock(&ios_lock);
		place[p].state = PLACE_READY;
		place[p].sequence = ++ios_frames_done;
		place[p].time = mach_absolute_time();
		place[p].exposure_lines = ios_now.lines;
		place[p].gain = ios_now.gain;
		place[p].frame_lines = ios_now.frame_lines;
		lck_mtx_unlock(&ios_lock);
		wakeup(&ios_frames_done);
	}
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	ring_free();
	say("camera: the frames for iOS stopped (%s): %llu done, %llu given\n", !ios_open ? "closed" : ios_switch_to != 0 ?
	    "another sensor asked for" : "nobody reads", ios_frames_done, ios_given);
}

static const char *
ios_res(void)
{
	static char res[8];

	snprintf(res, sizeof(res), "res%u", ios_mode_n);
	return res;
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	ios_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_open = false;
	ios_wanted = false;
	wakeup(&ios_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = ios_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&ios_lock);
	ios_last_read = ios_ms();
	if (!ios_streaming && !ios_wanted) {
		ios_wanted = true;
		wakeup(&ios_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&ios_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > ios_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (ios_ms() - since > (ios_streaming ? 3000u : 15000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&ios_lock);
			return EAGAIN;
		}
		if (!ios_streaming && !ios_wanted) {
			ios_wanted = true;      /* (the frames stopped meanwhile: another sensor) */
			wakeup(&ios_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&ios_frames_done, &ios_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&ios_lock);
			return error;
		}
		ios_last_read = ios_ms();
	}
	place[b].state = PLACE_READING;
	ios_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = ios_mode->width;
	f.height = ios_mode->height;
	f.stride = RAW10_STRIDE(ios_mode->width);
	f.format = 1;
	f.bayer = ios_source == SOURCE_CSID_TPG ? 0 : ios_source == SOURCE_SENSOR_PATTERN ? hw->pattern_bayer : hw->bayer;
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = ios_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = ios_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = EXPOSURE_MARGIN;
	f.sensor = hw_index;
	f.focus = af_position;
	lck_mtx_unlock(&ios_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = ring + (uint64_t)b * ring_place_bytes;
		uint32_t n = (uint32_t)MIN((uint64_t)ring_frame_bytes, (uint64_t)uio_resid(uio));

		uint64_t t0 = mach_absolute_time(), t1, t2;

		/* (what the IFE wrote went past the processor's caches) */
		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		t1 = mach_absolute_time();
		error = uiomove((caddr_t)at, (int)n, uio);
		t2 = mach_absolute_time();
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			uint64_t a, b;

			absolutetime_to_nanoseconds(t1 - t0, &a);
			absolutetime_to_nanoseconds(t2 - t1, &b);
			say("camera: frame %llu read: %u bytes, the caches' lines forgotten in %llu us, copied in %llu us\n", f.sequence, n,
			    a / 1000, b / 1000);
		}
	}
	lck_mtx_lock(&ios_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	return error;
}

/* HAKIOS_CAMERA_KEEP: a picture for the host, in one piece of memory the loader hands over. */
static int
keep_picture(const struct hakios_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep == 0 || keep_bytes < want) {
		return ENOSPC;                  /* (the piece is made when the device is, while there is memory in one piece) */
	}
	h = (struct cam_header *)keep;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? hw->bayer : 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "iOS", sizeof(h->sensor));
	int error = uiomove((caddr_t)(keep + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	for (vm_offset_t line = keep; line < keep + want; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep;
	say("camera: a picture from iOS for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == HAKIOS_CAMERA_EXPOSURE) {
		lck_mtx_lock(&ios_lock);
		ios_ask.lines = c.a;
		ios_ask.gain = c.b;
		ios_ask.frame_lines = c.c;
		ios_ask.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_KEEP) {
		return keep_picture(&c, uio);
	}
	if (c.command == HAKIOS_CAMERA_FOCUS) {
		lck_mtx_lock(&ios_lock);
		ios_ask.focus = c.a;
		ios_ask.focus_pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_SENSOR) {
		if (c.a >= sizeof(sensors) / sizeof(sensors[0])) {
			return EINVAL;
		}
		lck_mtx_lock(&ios_lock);
		ios_switch_mode = c.b;
		ios_switch_to = c.a + 1;
		lck_mtx_unlock(&ios_lock);
		wakeup(&ios_open);
		return 0;
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The device node, made as the thread starts (before mediaserverd does: its capture stack looks for
 * cameras once, when it starts); reads wait until serve_ios() is there to answer them.
 */
static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
}

/*
 * The sensor (and its mode) the sessions take: its tables, the mode asked for or else the one with
 * the most pixels of those at 4:3, at most 30 frames a second and 13 megapixels (the wide one's
 * 4000x3000, the front one's 2304x1728, the ultra-wide's 3264x2448, the macro's 1600x1200), the
 * colour bars' source (cam_pattern), the first exposure. False if the sensor cannot be used.
 */
static bool
ios_select(unsigned index, uint32_t mode_n)
{
	select_sensor(index);
	if (!fake) {
		find_pins();
	}
	if (!ctab_load()) {
		return false;
	}
	if (mode_n >= ctab->modes) {
		uint64_t best = 0;

		mode_n = 0;
		for (uint32_t m = 0; m < ctab->modes; m++) {
			const struct ctab_mode *o = &ctab_modes[m];
			uint64_t pixels = (uint64_t)o->width * o->height;

			if (o->width * 3 == o->height * 4 && o->fps_milli <= 31000 && pixels <= 13000000 && pixels > best) {
				best = pixels;
				mode_n = m;
			}
		}
	}
	ios_mode_n = mode_n;
	ios_mode = &ctab_modes[mode_n];
	if (ios_mode->lanes < 1 || ios_mode->lanes > (ios_mode->cphy ? 3u : 4u) || ios_mode->data_type != RAW10_DT ||
	    ios_mode->width % 4 != 0 || ios_mode->fps_milli == 0 || ios_mode->frame_length == 0) {
		say("camera: mode %u is not what this is written for\n", mode_n);
		return false;
	}
	ios_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)ios_mode->fps_milli * ios_mode->frame_length));
	uint32_t pattern = 0;

	(void)PE_parse_boot_argn("cam_pattern", &pattern, sizeof(pattern));
	ios_source = pattern == 0 ? SOURCE_SENSOR : pattern == 1 ? SOURCE_SENSOR_PATTERN : SOURCE_CSID_TPG;
	if (pattern != 0) {
		say("camera: cam_pattern=%u: %s\n", pattern, ios_source == SOURCE_SENSOR_PATTERN ? "the sensor's colour bars (SMIA 0x0600)" :
		    "the CSID's colour bars");
	}
	/* the first exposure, until the program asks for its own: boot-args as cam=4's, else a room's */
	ios_ask.lines = ios_mode->frame_length - EXPOSURE_MARGIN;
	ios_ask.gain = 8 * GAIN_UNITY;
	ios_ask.frame_lines = 0;
	(void)PE_parse_boot_argn("cam_shutter", &ios_ask.lines, sizeof(ios_ask.lines));
	(void)PE_parse_boot_argn("cam_gain", &ios_ask.gain, sizeof(ios_ask.gain));
	say("camera: /dev/hakios-camera: the %s camera's frames (mode %u: %ux%u, %u.%03u frames/s at the most), whenever it is "
	    "read\n", hw->name, mode_n, ios_mode->width, ios_mode->height, ios_mode->fps_milli / 1000, ios_mode->fps_milli % 1000);
	return true;
}

/* cam=5: a session whenever a read wants frames, another sensor whenever one is asked for, until a fault. */
static void
serve_ios(void)
{
	uint32_t mode_n = ~0u;

	if (!camdev_made) {
		say("camera: /dev/hakios-camera cannot be made\n");
		return;
	}
	(void)PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n));
	if (!ios_select(hw_index, mode_n)) {
		return;
	}
	while (!broken) {
		lck_mtx_lock(&ios_lock);
		while (!broken && !(ios_open && ios_wanted) && ios_switch_to == 0) {
			(void)msleep(&ios_open, &ios_lock, PZERO, "hakios-camera-idle", NULL);
		}
		unsigned to = ios_switch_to;

		ios_switch_to = 0;
		lck_mtx_unlock(&ios_lock);
		if (broken) {
			break;
		}
		if (to != 0) {
			/* (the last session is over: everything is down) */
			if (!ios_select(to - 1, ios_switch_mode)) {
				give_up("a sensor that cannot be used");
				break;
			}
			continue;
		}
		lck_mtx_lock(&ios_lock);
		ios_wanted = false;
		lck_mtx_unlock(&ios_lock);
		if (fake) {
			fake_session();
		} else {
			ios_session();
		}
	}
	say("camera: /dev/hakios-camera: no more frames in this run\n");
}

/* ---- the look, and the thread ----------------------------------------------------------------------- */

static void
show_ldos(void)
{
	const struct supply *all[4] = { &hw->vana, &hw->vdig, &hw->vio, &hw->custom1 };

	for (int i = 0; i < 4; i++) {
		if (all[i]->kind == SUPPLY_LDO) {
			show_ldo(all[i]->ldo);
		}
	}
}

static void
look(void)
{
	uint8_t revid[4] = { 0 }, enable = 0;

	show_clocks();
	show_pin(PIN_PMIC_SDA, "the PM8008's I2C data");
	show_pin(PIN_PMIC_SCL, "the PM8008's I2C clock");
	show_pin(PIN_PMIC_RESET, "the PM8008's reset");
	show_pin(hw->mclk_pin, "the sensor's MCLK");
	show_pin(hw->sda, "the sensor's CCI data");
	show_pin(hw->scl, "the sensor's CCI clock");
	show_pin(hw->reset, "the sensor's reset");
	if (!pm_reset_active()) {
		say("camera: the PM8008 is held in reset (not as Android's pm8008_active): not asked\n");
		return;
	}
	if (!pm8008_get(PM8008_CHIP, REVID_BASE, revid, 4)) {
		say("camera: the PM8008 does not answer (its bus is not at rest?)\n");
		return;
	}
	(void)pm8008_get(PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1);
	say("camera: the PM8008: revision %02x %02x %02x %02x, chip enable %#04x\n", revid[0], revid[1], revid[2], revid[3], enable);
	show_ldos();
}

/* cam=2: the sensor powered up, its chip ID (and, for the Sony ones, the revision at 0x0018) read, powered down. */
static bool
probe_sensor(void)
{
	bool ok = false;

	find_pins();
	if (ctab_load() && power_up() && healthy()) {
		ok = read_chip_id();
		if (ok) {
			uint8_t where[2] = { 0x00, hw->regs == REGS_SONY ? 0x18 : 0x02 }, value[2] = { 0, 0 };

			if (i2c_transfer(&cci_bus, sensor_address(), where, 2, value, 2)) {
				say("camera: the %s sensor's register %#06x: %02x %02x\n", hw->name, where[1], value[0], value[1]);
			}
		}
		(void)healthy();
		show_ldos();
	}
	power_down();
	return ok;
}

extern void umi_camera_probe_quiesce(void);

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;
	char which[16] = "";
	bool all = false;

	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	if (level == 5) {
		camdev_make();
	}
	select_sensor(1);
	if (PE_parse_boot_arg_str("cam_sensor", which, sizeof(which))) {
		all = strcmp(which, "all") == 0;
		for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (strncmp(which, sensors[i].name, strlen(which)) == 0 && (sensors[i].name[strlen(which)] == ' ' ||
			    sensors[i].name[strlen(which)] == '-')) {
				select_sensor(i);
			}
		}
	}
	if (fake) {
		say("camera: cam=5 cam_fake=1: the %s camera's frames for iOS are a chart made here; nothing of the phone is touched\n",
		    hw->name);
		serve_ios();
		say("camera: done\n");
		thread_terminate(current_thread());
	}
	wait_ms(secs * 1000);           /* (the system up first) */
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	say("camera: cam=%u: %s%s camera%s\n", level, all ? "each" : "the ", all ? "" : hw->name, level == 5 ? ": its frames for iOS" :
	    level == 4 ? ": its frames through its CSIPHY and RDI 0 into memory" :
	    level == 3 ? ": the CSID's test pattern through RDI 0 into memory" :
	    level == 2 ? ": powered up, its chip ID read, powered down" : ": a look, reading only");
	if (!gcc_camera_bus()) {
		thread_terminate(current_thread());
		return;
	}
	find_pins();
	look();
	look_around();
	if (level == 3) {
		capture_tpg();
		power_down();
	} else if (level == 5) {
		serve_ios();
		power_down();
	} else if (level == 4) {
		capture_sensor();
		power_down();
	} else if (level >= 2) {
		unsigned answered = 0, asked = 0;

		for (unsigned i = all ? 0 : hw_index; i < (all ? sizeof(sensors) / sizeof(sensors[0]) : hw_index + 1); i++) {
			select_sensor(i);
			say("camera: --- the %s sensor ---\n", hw->name);
			asked++;
			answered += probe_sensor();
			if (broken) {
				break;
			}
		}
		say("camera: %u of %u sensors answered with the chip ID Android expects\n", answered, asked);
		look();
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
hakios_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 5 ? 5 : level;
	uint32_t fake_n = 0;

	fake = level == 5 && PE_parse_boot_argn("cam_fake", &fake_n, sizeof(fake_n)) && fake_n != 0;
	if (level >= 3) {
		/*
		 * Memory in one piece for the host (cam_keep_mb, default 24: a 12-megapixel NV12 picture of cam=5;
		 * 32 for cam=3 and cam=4: two frames of up to 15 MB each, the wide camera's 4000x3000), taken now,
		 * while the system's memory still is in large pieces (at 25 s it no longer is).
		 */
		uint32_t keep_mb = level == 5 ? 24 : 32;

		(void)PE_parse_boot_argn("cam_keep_mb", &keep_mb, sizeof(keep_mb));
		keep_bytes = (uint64_t)(keep_mb > 64 ? 64 : keep_mb) << 20;
		if (keep_bytes != 0 && kmem_alloc_contig(kernel_map, &keep, keep_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("camera: no memory in one piece for a picture for the host (%u MB)\n", keep_mb);
			keep = 0;
			keep_bytes = 0;
		}
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off. */
void
hakios_camera_quiesce(void)
{
	umi_camera_probe_quiesce();
	if (level < 2 || tlmm == 0 || !gcc_switched) {
		return;
	}
	leaving = true;
	broken = false;                 /* (one last time, whatever went before) */
	csiphy_down();
	titan_down();
	power_down();
	gcc_camera_bus_back();
}
