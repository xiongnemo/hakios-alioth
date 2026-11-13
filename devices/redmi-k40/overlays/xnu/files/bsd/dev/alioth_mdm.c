/*
 * The Redmi K40's modem: an SDX55 outside the SoC, on PCIe2, whose GNSS receiver is the phone's GPS
 * (work/research/gps-sdx55.md, from Xiaomi's device tree and kernel for alioth). The first steps towards it,
 * a diagnostic: only with boot-arg mdm_step=N, N = 1 .. 6; nothing here runs otherwise.
 *
 *   1  reads   the SPMI arbiter's table: the channel of the modem's own PMIC's power-on block (PMX55,
 *              "pmxprairie", slave 8, peripheral 0x08 -- kona-pmic-overlay.dtsi's qcom,modem-reset), who may write
 *              through it, and the block's type and subtype
 *   2  writes  that block's warm reset, as Xiaomi's qpnp_pon_modem_pwr_off() makes it (SW_RST_S2_CTL2 0, 500 us,
 *              SW_RST_S2_CTL warm reset, SW_RST_S2_CTL2 enable, 500 us, SW_RST_GO 0xa5), 150 ms, then
 *              AP2MDM_STATUS (GPIO 56) high and AP2MDM_ERRFATAL (57) low, as esoc-mdm-pon.c's first power-on
 *              does; then watches MDM2AP_STATUS (GPIO 3) and MDM2AP_ERRFATAL (GPIO 1) for 2 s
 *   3  writes  PCIe2 up the way umi_pcie.c brings PCIe0 up for the Wi-Fi, with PCIe2's numbers (Linux
 *              gcc-sm8250.c, Xiaomi's kona-pcie.dtsi; its PHY's sequence in alioth_pcie2_phy.h); looks for the
 *              SDX55 (17cb:0306), sizes its BAR and reads its MHI registers -- the version, BHI, the execution
 *              environment. Bus mastering stays off: nothing of the modem's reaches memory.
 *   4  writes  the SMMU: a free match register sends the modem's stream (0x1d01: kona-pcie.dtsi's iommu-map for
 *              requester 0x100) through a free context with page tables of ours for 64 MB at 0x20000000
 *              (kona-mhi.dtsi's qcom,iommu-dma-addr-pool), PCIe2's BDF-to-SID route as umi_pcie.c makes PCIe0's,
 *              then bus mastering on; MHI (alioth_mhi.c): the SBL through BHI, READY, contexts, M0
 *   5  talks   Sahara: the SBL's images, up to mission mode. The images: fw/sdx55m/ of the system image, which
 *              fw/sdx55m/sahara.txt names ("bhi FILE" for the SBL, then "ID FILE" a line, from mdm_helper's
 *              arguments to Android's ks; work/research/gps-sdx55.md); never in a shared image (the modem's EFS
 *              copies hold the phone's IMEI and calibration)
 *   6  talks   QRTR (sys/alioth_qrtr.h) on the IPCR channels 20/21: our HELLO, then for 10 s what the modem tells of
 *              its services -- QMI LOC (0x10) is the GPS's; lookups of the modem's are answered "none"
 * After step 3 alone, or when a step fails, PERST (GPIO 85) is held asserted and the SMMU put back as it was; a
 * modem in mission mode is left running (boot-arg mdm_keep=0: stopped too). On the way back to the loader
 * (loader_return.c) alioth_mdm_quiesce() stops it either way.
 *
 * The SDX55's pins are all in the TLMM's SOUTH tile (Linux pinctrl-sm8250.c), the Wi-Fi's in the NORTH one.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/alioth_mhi.h>
#include <sys/alioth_qrtr.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <stdarg.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include "alioth_pcie2_phy.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern int umi_rpmh_wifi_power(void);
extern int umi_rpmh_bcm_vote(const char *res, unsigned vote_x, unsigned vote_y);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern lck_mtx_t hakios_smmu_lock;      /* (bsd/dev/hakios.wifi.c: the SMMU's global registers are shared) */
/*
 * Every line said to the console AND kept here, all of them said again on the way back to the loader
 * (alioth_mdm_quiesce(), as hakios.mic.c does): the loader's log keeps the boot's first 256 KB and the last 1 MB,
 * and iOS's own boot fills the first ~6 s after the modem's reset -- 2026-11-13, step 6's lines after that fell into
 * the middle that is not kept, and the modem only looked silent.
 */
extern void loader_log_putc(char c);
#define MDM_KEPT                (128 * 1024)    /* (48 KB filled up with step 6's repeated service lists, 11-14) */
static char mdm_kept[MDM_KEPT];
static uint32_t mdm_kept_len;
static bool mdm_kept_full;

static void mdm_say(const char *format, ...) __printflike(1, 2);

static void
mdm_say(const char *format, ...)
{
	char line[192];
	va_list list;
	size_t n;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	hakios_tl("%s", line);
	n = strlen(line);
	if (mdm_kept_len + n < MDM_KEPT) {
		memcpy(mdm_kept + mdm_kept_len, line, n);
		mdm_kept_len += (uint32_t)n;
	} else {
		mdm_kept_full = true;
	}
}
#define say mdm_say

/* ---- registers --------------------------------------------------------------------------------------- */

#define GCC_PHYS        0x00100000UL
#define GCC_SIZE        0xa0000UL
#define TLMM_SOUTH_PHYS 0x0f500000UL
#define TLMM_SIZE       0x300000UL
#define PARF_PHYS       0x01c10000UL
#define PHY_PHYS        0x01c16000UL
#define DBI_PHYS        0x64000000UL
#define ATU_PHYS        0x64001000UL
#define CONF_PHYS       0x64100000UL
#define BAR_PHYS        0x64300000UL

/* the SPMI arbiter (as bsd/dev/hakios.torch.c has it) */
#define ARB_CORE_PHYS   0x0c440000UL
#define ARB_CNFG_PHYS   0x0c40a000UL
#define ARB_OBS_PHYS    0x0e600000UL
#define ARB_CHNLS_PHYS  0x0c600000UL
#define ARB_SYSTEM      0
#define ARB_CHANNEL(n)  (0x900 + 4 * (n))
#define ARB_OWNER(n)    (0x700 + 4 * (n))
#define ARB_MAX         512
#define CH_COMMAND      0x00
#define CH_STATUS       0x08
#define CH_WDATA0       0x10
#define OBS_DATA0       0x18
#define CMD_WRITE       (0u << 27)
#define CMD_READ        (1u << 27)

#define PMX55_PON       0x0808          /* slave 8, peripheral 0x08 (0x800) */
#define PON_TYPE        0x04
#define PON_SUBTYPE     0x05
#define PON_SW_RST_S2_CTL  0x62
#define PON_SW_RST_S2_CTL2 0x63
#define PON_SW_RST_GO      0x64
#define PON_WARM_RESET     0x01         /* PON_POWER_OFF_TYPE_WARM_RESET */
#define PON_RESET_EN       0x80         /* QPNP_PON_RESET_EN */
#define PON_GO             0xa5         /* QPNP_PON_SW_RST_GO_VAL */

#define GPIO_MDM2AP_ERRFATAL 1
#define GPIO_MDM2AP_STATUS   3
#define GPIO_AP2MDM_STATUS   56
#define GPIO_AP2MDM_ERRFATAL 57
#define GPIO_PERST           85
#define GPIO_CLKREQ          86
#define GPIO_WAKE            87

#define SDX55_ID        0x030617cbu     /* device 0306, vendor 17cb */
#define BIT(n)          (1u << (n))

static vm_offset_t gcc, tlmm, parf, phy, dbi, atu, conf, bar, arb_core, arb_cnfg, arb_obs;
static uint32_t mdm_step, mdm_keep = 1, bar_size;
static uint64_t reset_at;                       /* mach_absolute_time() of the modem's warm reset */

static uint64_t
since_reset_ms(void)
{
	uint64_t ns = 0;

	if (reset_at == 0) {
		return 0;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - reset_at, &ns);
	return ns / 1000000;
}

static uint32_t
rd(vm_offset_t a)
{
	uint32_t v = *(volatile uint32_t *)a;

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
wr(vm_offset_t a, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static void
update(vm_offset_t a, uint32_t clear, uint32_t set)
{
	wr(a, (rd(a) & ~clear) | set);
}

static bool
wait_bits(vm_offset_t a, uint32_t mask, uint32_t wanted, unsigned timeout_us, const char *what)
{
	uint32_t v = 0;

	for (unsigned us = 0; us < timeout_us; us += 10) {
		v = rd(a);
		if ((v & mask) == wanted) {
			return true;
		}
		delay(10);
	}
	say("mdm: %s: timed out (%#x)\n", what, v);
	return false;
}

/* ---- the SPMI arbiter: the modem PMIC's power-on block ----------------------------------------------- */

static int pon_channel = -1;
static bool pon_writable;
static vm_offset_t pon_write;

static bool
spmi_wait(vm_offset_t ch)
{
	uint32_t status = 0;

	for (unsigned us = 0; us < 2000; us++) {
		status = rd(ch + CH_STATUS);
		if (status & 1) {
			return (status & 0xe) == 0;     /* done, and neither failed, denied nor dropped */
		}
		delay(1);
	}
	return false;
}

static bool
pon_read(uint8_t reg, uint8_t *value)
{
	vm_offset_t ch = arb_obs + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)pon_channel;
	bool ok;

	hakios_spmi_lock();
	wr(ch + CH_COMMAND, CMD_READ | (uint32_t)reg << 4);
	ok = spmi_wait(ch);
	*value = (uint8_t)rd(ch + OBS_DATA0);
	hakios_spmi_unlock();
	return ok;
}

static bool
pon_put(uint8_t reg, uint8_t value)
{
	bool ok;

	if (!pon_writable || pon_write == 0) {
		return false;
	}
	hakios_spmi_lock();
	wr(pon_write + CH_WDATA0, value);
	wr(pon_write + CH_COMMAND, CMD_WRITE | (uint32_t)reg << 4);
	ok = spmi_wait(pon_write);
	hakios_spmi_unlock();
	if (!ok) {
		say("mdm: PMX55 PON %#04x <- %#04x: the arbiter did not take it\n", reg, value);
	}
	return ok;
}

static bool
step_pon_find(void)
{
	uint8_t type = 0, subtype = 0;

	for (uint32_t n = 0; n < ARB_MAX; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == PMX55_PON) {
			uint32_t owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;

			say("mdm: 1: arbiter channel %u is the PMX55's power-on block (%#x), written by system %u%s\n", n, map, owner,
			    owner == ARB_SYSTEM ? ", which is us" : "");
			if (pon_channel < 0 || owner == ARB_SYSTEM) {
				pon_channel = (int)n;
				pon_writable = owner == ARB_SYSTEM;
			}
		}
	}
	if (pon_channel < 0) {
		say("mdm: 1: NO CHANNEL of the arbiter is the PMX55's power-on block (slave 8, 0x800): no warm reset\n");
		return false;
	}
	pon_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * (uint32_t)pon_channel, 0x1000);
	if (!pon_read(PON_TYPE, &type) || !pon_read(PON_SUBTYPE, &subtype)) {
		say("mdm: 1: the PMX55's power-on block does not answer\n");
		return false;
	}
	say("mdm: 1: the PMX55's power-on block: type %#04x subtype %#04x (%s)\n", type, subtype,
	    type == 0x01 ? "a power-on block" : "NOT a power-on block");
	return type == 0x01;
}

/* ---- the modem's GPIOs (SOUTH tile: a register block of 0x1000 a pin, [9] output enable, [1] the output) ---- */

static void
gpio_out(unsigned pin, bool high, uint32_t config)
{
	vm_offset_t g = tlmm + (vm_offset_t)pin * 0x1000;

	update(g + 4, BIT(1), high ? BIT(1) : 0);       /* (the latch before the output is enabled: no glitch) */
	update(g, 0x3ff, config | BIT(9));
}

static bool
gpio_in(unsigned pin)
{
	return (rd(tlmm + (vm_offset_t)pin * 0x1000 + 4) & 1) != 0;
}

static bool
step_power(void)
{
	/*
	 * boot-arg mdm_pon_type=7 (or 8): the PMX55's HARD reset (PON_POWER_OFF_TYPE_HARD_RESET; 8 = DVDD_HARD_RESET)
	 * instead of the warm one -- its rails off and on again, the modem's PBL from cold as at the phone's power-on.
	 * (2026-10-31: after a warm reset the PBL takes the SBL's signature and every segment's hash and still refuses
	 * the image ~8 ms later, 0xef0e0003; Android's PBL, which takes it, starts from cold.)
	 */
	uint32_t type = PON_WARM_RESET;

	if (PE_parse_boot_argn("mdm_pon_type", &type, sizeof(type)) && type != 7 && type != 8) {
		type = PON_WARM_RESET;
	}
	say("mdm: 2: the PMX55's %s reset (type %u), then AP2MDM_STATUS high (Xiaomi's sdx55m first power-on)\n",
	    type == PON_WARM_RESET ? "warm" : "HARD", type);
	if (!pon_put(PON_SW_RST_S2_CTL2, 0)) {
		return false;
	}
	delay(500);
	if (!pon_put(PON_SW_RST_S2_CTL, (uint8_t)type) || !pon_put(PON_SW_RST_S2_CTL2, PON_RESET_EN)) {
		return false;
	}
	delay(500);
	if (!pon_put(PON_SW_RST_GO, PON_GO)) {
		return false;
	}
	reset_at = mach_absolute_time();
	/* (a hard reset takes the PMIC through its whole power-on sequence again: the pins wait longer for it) */
	delay((type == PON_WARM_RESET ? 150 : 600) * 1000);
	/* (inputs: function 0, no pull -- the modem drives them) */
	update(tlmm + GPIO_MDM2AP_STATUS * 0x1000, 0x3ff, 0);
	update(tlmm + GPIO_MDM2AP_ERRFATAL * 0x1000, 0x3ff, 0);
	gpio_out(GPIO_AP2MDM_ERRFATAL, false, 0);
	gpio_out(GPIO_AP2MDM_STATUS, true, 0);
	if (mdm_step == 2) {
		/* (step 2 alone: the modem's two pins watched for 2 s) */
		for (unsigned ms = 0; ms <= 2000; ms += 250) {
			say("mdm: 2: %u ms: MDM2AP_STATUS %d, MDM2AP_ERRFATAL %d\n", ms, gpio_in(GPIO_MDM2AP_STATUS),
			    gpio_in(GPIO_MDM2AP_ERRFATAL));
			delay(250 * 1000);
		}
	} else {
		/*
		 * Then straight on, as esoc-mdm-pon.c does without a PBLRDY pin (msleep(200)): Android's BHI comes some
		 * 0.3 s after the reset (its dmesg, 16.45 -> 16.75 s); ours came 3.5 s after it, and the PBL refused the
		 * image it read (2026-10-31).
		 */
		delay(200 * 1000);
	}
	say("mdm: 2: %llu ms after the reset: MDM2AP_STATUS %d, MDM2AP_ERRFATAL %d\n", since_reset_ms(),
	    gpio_in(GPIO_MDM2AP_STATUS), gpio_in(GPIO_MDM2AP_ERRFATAL));
	return true;
}

/* ---- PCIe2 (umi_pcie.c's PCIe0 bring-up, PCIe2's numbers: work/research/gps-sdx55.md) --------------------- */

static bool
xo(uint32_t cmd)
{
	update(gcc + cmd + 4, 0x371f, 1);       /* 19.2 MHz XO, divide by 1 */
	update(gcc + cmd, 0, 1);
	return wait_bits(gcc + cmd, 1, 0, 10000, "RCG update");
}

static bool
branch(uint32_t offset)
{
	update(gcc + offset, 0, 1);
	return wait_bits(gcc + offset, BIT(31), 0, 10000, "a clock branch");
}

static bool
pcie2_clocks(void)
{
	static const uint32_t gates[] = { 0x6010, 0x6014, 0x601c, 0x6024, 0x6028 };
	static const uint32_t resets[] = { 0x6000, 0x1f01c };     /* GCC_PCIE_2_BCR, GCC_PCIE_2_PHY_BCR */

	update(gcc + 0x6004, 7, 0);                             /* PCIE_2_GDSC on */
	delay(1);
	if (!wait_bits(gcc + 0x6004, BIT(31), BIT(31), 10000, "PCIE_2_GDSC") || !xo(0x6038) || !xo(0x6f014)) {
		return false;
	}
	if (!branch(0x8c00c) || !branch(0x6f02c) || !branch(0x9000c) || !branch(0x8d058)) {  /* MDM_CLKREF, PHY aux, TBUs */
		return false;
	}
	update(gcc + 0x601c, 0x6000, 0);                        /* (AXI and PIPE keep no memory/peripheral state) */
	update(gcc + 0x6014, 0x6002, 0);
	update(gcc + 0x6024, BIT(1), 0);
	update(gcc + 0x602c, 0x6000, 0);
	update(gcc + 0x52010, 0, 0x7c00);                       /* PCIe2's votes: slv_q2a, slv, mstr, cfg, aux; not PIPE */
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		if (!wait_bits(gcc + gates[i], BIT(31), 0, 10000, "a PCIe2 clock")) {
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
		update(gcc + resets[i], 0, 1);
		delay(1000);
		update(gcc + resets[i], 1, 0);
	}
	return true;
}

static bool
outbound(unsigned index, uint32_t type, uint32_t start, uint32_t end, uint32_t target)
{
	vm_offset_t a = atu + index * 0x200;

	wr(a + 4, 0);
	wr(a, type);
	wr(a + 8, start);
	wr(a + 12, 0);
	wr(a + 16, end);
	wr(a + 20, target);
	wr(a + 24, 0);
	wr(a + 4, BIT(31));
	return wait_bits(a + 4, BIT(31), BIT(31), 1000, "an outbound window");
}

static bool
step_pcie2(void)
{
	uint32_t id = 0, command, bar0, version, bhi;
	int error;

	say("mdm: 3: PCIe2 up, as PCIe0 is for the Wi-Fi\n");
	gpio_out(GPIO_PERST, false, 1);                         /* PERST asserted, pull-down */
	delay(1000);
	if ((error = umi_rpmh_wifi_power()) != 0) {             /* (PM8150 L5/L9 and CX, the PCIe rails) */
		say("mdm: 3: the PCIe rails: %d\n", error);
		return false;
	}
	update(tlmm + GPIO_CLKREQ * 0x1000, 0x3ff, 7);          /* CLKREQ: function 1 (pci_e2), pull-up */
	update(tlmm + GPIO_WAKE * 0x1000, 0x3ff, 3);            /* WAKE: input, pull-up */
	if (!pcie2_clocks()) {
		return false;
	}
	wr(parf + 0x1000, 4);                                   /* a root complex */
	update(parf + 0x40, 1, 0);
	wr(parf + 0x350, 0);
	wr(parf, 0x365e);
	update(parf + 0x174, 0, BIT(4));
	wr(parf + 0x22c, 0);
	wr(parf + 0x358, 0x04000000);
	update(parf + 0x1a8, 0, BIT(31));
	for (unsigned i = 0; i < sizeof(alioth_pcie2_phy) / sizeof(alioth_pcie2_phy[0]); i++) {
		wr(phy + alioth_pcie2_phy[i].offset, alioth_pcie2_phy[i].value);
		if (alioth_pcie2_phy[i].delay_us) {
			delay(alioth_pcie2_phy[i].delay_us);
		}
	}
	delay(1000);
	update(gcc + 0x52010, 0, BIT(15));                      /* PIPE */
	if (!wait_bits(phy + 0xa14, BIT(6), 0, 100000, "the PHY")) {
		return false;
	}
	say("mdm: 3: the PHY is ready (%#x)\n", rd(phy + 0xa14));
	delay(10000);                                           /* (DT ep-latency 10 ms) */
	gpio_out(GPIO_PERST, true, 1);
	delay(1000);
	update(dbi + 0x80c, 0x1f00, 0x100);
	wr(dbi + 0x8a8, 0x20);
	update(dbi + 0x890, BIT(0), 0);
	update(dbi + 0x8bc, 0, BIT(0));
	wr(dbi + 0x154, 0x55555555);
	update(dbi + 0x8bc, BIT(0), 0);
	{
		/* Gen2 to begin with (the SDX55 can do Gen3 x2; Android trains it at 8 GT/s): boot-arg mdm_gen=1..3 */
		uint32_t gen = 2;

		PE_parse_boot_argn("mdm_gen", &gen, sizeof(gen));
		gen = gen < 1 ? 1 : gen > 3 ? 3 : gen;
		update(dbi + 0xa0, 0xf, gen);
		say("mdm: 3: the link's target speed: Gen%u\n", gen);
	}
	wr(dbi + 0x78, (rd(dbi + 0x78) & 0xffff) & ~0x7000u);   /* (the control half only; the upper half is W1C) */
	update(parf + 0x1b0, 0, BIT(8));                        /* LTSSM on */
	if (!wait_bits(dbi + 0xf28, BIT(10), BIT(10), 100000, "the link") ||
	    !wait_bits(dbi + 0x80, BIT(29), BIT(29), 100000, "the data link layer")) {
		say("mdm: 3: link status %#x, LTSSM %#x\n", rd(dbi + 0x80), rd(parf + 0x24));
		return false;
	}
	say("mdm: 3: THE LINK IS UP %llu ms after the reset: status %#x (Gen%u x%u)\n", since_reset_ms(), rd(dbi + 0x80),
	    (rd(dbi + 0x80) >> 16) & 0xf, (rd(dbi + 0x80) >> 20) & 0x3f);
	wr(dbi + 0x18, 0x00010100);                             /* buses 0, 1, 1 */
	wr(dbi + 0x20, 0x64306430);                             /* a 1 MiB bridge window at 0x64300000 */
	wr(dbi + 0xb40, 0x14);
	if (!outbound(0, 4, CONF_PHYS, CONF_PHYS + 0xfff, 0x01000000)) {
		return false;
	}
	for (unsigned i = 0; i < 1000 && (id = rd(conf)) != SDX55_ID; i++) {
		delay(1000);
	}
	say("mdm: 3: the endpoint is %#x%s\n", id, id == SDX55_ID ? ": THE SDX55" : ", NOT the SDX55");
	if (id != SDX55_ID) {
		return false;
	}
	command = rd(conf + 4);
	bar0 = rd(conf + 0x10);
	say("mdm: 3: command %#x, BAR0 %#x\n", command, bar0);
	wr(conf + 4, ((command & 0xffff) & ~7u) | BIT(10));    /* no decode, no bus mastering, no INTx */
	/* the BAR's size, the usual way (all ones, read back), with decoding off */
	wr(conf + 0x10, 0xffffffffu);
	{
		uint32_t mask = rd(conf + 0x10) & ~15u;

		bar_size = mask == 0 ? 0 : ~mask + 1;
	}
	say("mdm: 3: BAR0 is %u KB%s\n", bar_size >> 10, bar_size > 0x100000 ? " (only its first megabyte is mapped)" : "");
	if (bar_size == 0 || bar_size > 0x100000) {
		bar_size = bar_size == 0 ? 0 : 0x100000;
	}
	if (bar_size < 0x1000) {
		say("mdm: 3: a BAR of less than 4 KB is not the SDX55's\n");
		return false;
	}
	/* the BDF-to-SID table may hold anything after a reset: zero, with bus mastering off (as umi_pcie.c for PCIe0) */
	for (unsigned i = 0; i < 256; i++) {
		wr(parf + 0x2000 + 4 * i, 0);
	}
	wr(conf + 0x10, (uint32_t)BAR_PHYS | (bar0 & 15));
	if ((bar0 & 6) == 4) {
		wr(conf + 0x14, 0);
	}
	/* the SDX55's second BAR (BAR2, config 0x18, 64-bit, 4 KB) where Android puts it, right after BAR0 */
	if ((bar0 & 6) == 4) {
		uint32_t bar2 = rd(conf + 0x18), mask;

		wr(conf + 0x18, 0xffffffffu);
		mask = rd(conf + 0x18) & ~15u;
		say("mdm: 3: BAR2 %#x, %u KB\n", bar2, mask == 0 ? 0 : (~mask + 1) >> 10);
		if (mask != 0 && ~mask + 1 <= 0x1000) {
			wr(conf + 0x18, ((uint32_t)BAR_PHYS + 0x1000) | (bar2 & 15));
			if ((bar2 & 6) == 4) {
				wr(conf + 0x1c, 0);
			}
		} else {
			wr(conf + 0x18, bar2);
		}
	}
	if (!outbound(1, 0, BAR_PHYS, BAR_PHYS + 0xfffff, BAR_PHYS)) {
		return false;
	}
	wr(dbi + 4, 2);                                         /* the root bridge: memory decode only */
	wr(conf + 4, ((command & 0xffff) & ~7u) | 2 | BIT(10)); /* the endpoint: memory decode, never bus mastering */
	version = rd(bar + 8);
	bhi = rd(bar + 0x28);
	say("mdm: 3: MHI: length %#x, version %#x, config %#x, BHI at %#x, BHIe at %#x\n", rd(bar), version, rd(bar + 0x10), bhi,
	    rd(bar + 0x2c));
	if (bhi != 0 && bhi < 0x1000) {
		say("mdm: 3: BHI: version %#x, execution environment %#x (0 PBL, 1 SBL, 2 AMSS), status %#x\n", rd(bar + bhi + 4),
		    rd(bar + bhi + 0x28), rd(bar + bhi + 0x2c));
	}
	return true;
}

/* ---- the modem's view of memory: a context of the SMMU (hakios.camera.c's way; hakios.wifi.c's PTEs for PCIe) ---- */

#define SMMU_PHYS               0x15000000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
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
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define MDM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* hakios.wifi.c's RADIO_PTE */
/* boot-arg mdm_dma_cached=1: write-back cacheable (MAIR entry 1), inner shareable -- what Linux's io-pgtable makes
 * for a dma-coherent device (pcie2 is, and kona-mhi.dtsi asks for coherent page tables) */
#define MDM_PTE_CACHED          (3ull | 1ull << 2 | 1ull << 6 | 3ull << 8 | 1ull << 10 | 1ull << 11)
/*
 * boot-arg mdm_msi=1 (2026-11-13): the modem's MSI capability switched on before BHI, its writes going to the GIC's
 * register that raises an SPI, shown to the modem as a device's page at the last page of the window. The OnePlus 8 Pro's
 * port (github.com/dekefake/hackios-op8pro, docs/research/modem-sdx55.md step 4, the same SM8250 + SDX55 on PCIe 2) had
 * this PBL's 0xef0e0003 for the whole sbl1.mbn and then success with this and the cacheable mapping (our
 * mdm_dma_cached=1) changed at once; mdm_dma_cached=1 alone did not do it here (10-31). SPIs 960..991: nothing enables
 * them, so a write only makes one pending (the Wi-Fi's are 800..831, hakios.wifi.c RADIO_FIRST_SPI + 32).
 */
#define MDM_MSI_REGISTER        0x17a10040ull
#define MDM_MSI_SPI             960u
#define MDM_PTE_DEVICE          (MDM_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)   /* MAIR entry 2 (Device-nGnRE), never executed */
#define MDM_STREAM              0x1d01u         /* kona-pcie.dtsi pcie2: iommu-map <0x100 &apps_smmu 0x1d01 1> */
#define MDM_WINDOW              0x20000000ull   /* kona-mhi.dtsi: qcom,iommu-dma-addr-pool <0x20000000 ...> */
#define MDM_WINDOW_SIZE         (64ull << 20)
#define MDM_THIRDS              ((uint32_t)(MDM_WINDOW_SIZE >> 21))
#define MDM_MSI_IOVA            (MDM_WINDOW + MDM_WINDOW_SIZE - 4096)   /* (mdm_msi=1: never handed out by mdm_allocate) */

static vm_offset_t smmu, smmu_second, smmu_context, smmu_contexts[128], tables;
static uint32_t smmu_match = ~0u, smmu_context_n, smmu_old_smr, smmu_old_s2cr;
static bool smmu_on, dma_on, dma_cached, msi_page;
static uint64_t window_next;
/* what mdm_allocate() handed out: where the modem sees it and where it is (for smmu_translation_check()) */
struct mdm_allocation { uint64_t iova, physical, size; };
static struct mdm_allocation allocations[16];
static unsigned allocation_count;

/* A free match register (none may already cover the stream) and a context no match register sends anything to,
 * switched off and without a fault. With hakios_smmu_lock held. */
static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0, idr1, contexts, page, pages;

	if (smmu == 0 && (smmu = ml_io_map(SMMU_PHYS, 0x1000)) == 0) {
		return false;
	}
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	if (2 * pages * page != 0x100000 || contexts > 128 || (rd(smmu + SMMU_SCR0) & BIT(3))) {
		say("mdm: 4: the SMMU's layout is not the one known here (idr1 %#x, scr0 %#x)\n", idr1, rd(smmu + SMMU_SCR0));
		return false;
	}
	smmu_match = ~0u;
	say("mdm: 4: SMMU scr0 %#x (a stream nobody matches %s)\n", rd(smmu + SMMU_SCR0),
	    rd(smmu + SMMU_SCR0) & BIT(10) ? "faults" : "PASSES UNTRANSLATED");
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			/* (any match of PCIe2's streams 0x1d00-0x1d01 said: one that passes them untranslated would let the
			 * modem read physical memory at its "address" -- 2026-10-31, the PBL refused what it read) */
			if ((((smr & 0x7fff) ^ 0x1d00) & ~((smr >> 16 & 0x7fff) | 1)) == 0) {
				say("mdm: 4: SMMU match register %u covers PCIe2's streams: smr %#x, s2cr %#x (%s, context %u)\n", n, smr,
				    s2cr, (s2cr >> 16 & 3) == 0 ? "translated" : (s2cr >> 16 & 3) == 1 ? "BYPASS" : "fault", s2cr & 0xff);
			}
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ MDM_STREAM) & ~(smr >> 16 & 0x7fff)) == 0) {
				say("mdm: 4: SMMU match register %u (%#x) is somebody's for the modem's stream: left alone\n", n, smr);
				return false;
			}
		} else if (smmu_match == ~0u) {
			smmu_match = n;
			smmu_old_smr = smr;
			smmu_old_s2cr = s2cr;
		}
	}
	if (smmu_match == ~0u) {
		say("mdm: 4: the SMMU has no match register left\n");
		return false;
	}
	if (smmu_second == 0 && (smmu_second = ml_io_map(SMMU_PHYS + page, 0x1000)) == 0) {
		return false;
	}
	for (smmu_context_n = 0; smmu_context_n < contexts; smmu_context_n++) {
		if (used[smmu_context_n]) {
			continue;
		}
		if (smmu_contexts[smmu_context_n] == 0) {
			smmu_contexts[smmu_context_n] = ml_io_map(SMMU_PHYS + (uint64_t)(pages + smmu_context_n) * page, 0x1000);
		}
		smmu_context = smmu_contexts[smmu_context_n];
		if (smmu_context != 0 && !(rd(smmu_context + SMMU_CB_SCTLR) & 1) && !(rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL)) {
			return true;
		}
	}
	smmu_match = ~0u;
	say("mdm: 4: the SMMU has no context left\n");
	return false;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("mdm: 4: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		return false;
	}
	return true;
}

static void
smmu_forget(void)
{
	wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
	wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	for (unsigned i = 0; i < 100000 && (rd(smmu_context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

/* Tables of ours for the window (its pages filled in by mdm_allocate()), the context on, the stream through it. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	ok = smmu_find();
	if (ok && tables == 0) {
		ok = kmem_alloc(kernel_map, &tables, (2 + MDM_THIRDS) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) ==
		    KERN_SUCCESS;
	}
	if (ok) {
		root = (uint64_t *)tables;
		second = (uint64_t *)(tables + 4096);
		root[MDM_WINDOW >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
		for (uint32_t t = 0; t < MDM_THIRDS; t++) {
			second[(MDM_WINDOW >> 21 & 0x1ff) + t] = kvtophys(tables + (2 + t) * 4096) | 3;
		}
		for (vm_offset_t line = tables; line < tables + (2 + MDM_THIRDS) * 4096; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		table_root = kvtophys(tables) | (uint64_t)smmu_context_n << 48;
		say("mdm: 4: SMMU: context %u gets our tables for %llu MB at %#llx; match register %u sends stream %#x through it\n",
		    smmu_context_n, MDM_WINDOW_SIZE >> 20, MDM_WINDOW, smmu_match, MDM_STREAM);
		ok = smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) && smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) &&
		    smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) &&
		    smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) && smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) &&
		    smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) &&
		    smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) &&
		    smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) && smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) &&
		    smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) && smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u);
	}
	if (ok) {
		wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
		smmu_forget();
		ok = smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1);
		smmu_on = ok;
	}
	if (ok) {
		ok = smmu_put(smmu, SMMU_S2CR(smmu_match), smmu_context_n, 0x300ff) &&
		    smmu_put(smmu, SMMU_SMR(smmu_match), 1u << 31 | MDM_STREAM, ~0u);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return ok;
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
		say("mdm: the SMMU has refused the modem nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("mdm: THE SMMU HAS REFUSED A REQUEST OF THE MODEM'S: context %u status %#x, address %#llx, syndrome %#x; global "
	    "status %#x, stream %#x\n", smmu_context_n, status,
	    (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR), rd(smmu_context + SMMU_CB_FSYNR0),
	    global, rd(smmu + SMMU_SGFSYNR1));
}

/* As found: the match register, then the context. `leaving`: on the way back to the loader, no lock, no saying. */
static void
smmu_down(bool leaving)
{
	if (smmu_match == ~0u) {
		return;
	}
	if (!leaving) {
		smmu_faults();
		lck_mtx_lock(&hakios_smmu_lock);
	}
	wr(smmu + SMMU_SMR(smmu_match), smmu_old_smr);
	wr(smmu + SMMU_S2CR(smmu_match), smmu_old_s2cr);
	if (smmu_on) {
		uint32_t fsr = rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL;

		wr(smmu_context + SMMU_CB_SCTLR, 0);
		smmu_forget();
		if (fsr != 0) {
			wr(smmu_context + SMMU_CB_FSR, fsr);
		}
	}
	smmu_on = false;
	smmu_match = ~0u;
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
		say("mdm: the SMMU's match register and context put back as they were found\n");
	}
}

/*
 * MHI's memory (alioth_mhi.h): in one physical piece, seen by the modem at the next addresses of the window and by
 * us through an uncached mapping (lx_os_radio_memory()'s way), so that neither side's caches stand between.
 * Handed out upwards, never taken back: the modem may still use what it was given.
 */
static int
mdm_allocate(__unused void *context, size_t size, struct alioth_mhi_dma *dma)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (!smmu_on || window_next + size > MDM_WINDOW_SIZE - (msi_page ? 4096 : 0)) {
		return ENOMEM;
	}
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("mdm: no %lu KB of memory in one piece\n", (unsigned long)(size >> 10));
		return ENOMEM;
	}
	physical = kvtophys(cached);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (dma_cached) {
		uncached = cached;                      /* (the cached view is the one used: mdm_sync() cleans it) */
	} else if ((uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB)) == 0) {
		return ENOMEM;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = window_next + offset;
		uint64_t *third = (uint64_t *)(tables + (2 + (at >> 21)) * 4096);

		third[at >> 12 & 0x1ff] = (physical + offset) | (dma_cached ? MDM_PTE_CACHED : MDM_PTE);
		__asm__ volatile ("dc civac, %0" : : "r" (&third[at >> 12 & 0x1ff]));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	smmu_forget();
	dma->bytes = (void *)uncached;
	dma->address = MDM_WINDOW + window_next;
	dma->size = size;
	if (allocation_count < sizeof(allocations) / sizeof(allocations[0])) {
		allocations[allocation_count++] = (struct mdm_allocation){ dma->address, physical, size };
	}
	window_next += size;
	return 0;
}

/*
 * boot-arg mdm_ats=1 (diagnosis): the SMMU's own address translation (MMU-500's ATS1PR in the context bank, the
 * answer in its PAR) of every page of the first piece of the window -- the BHI image -- set beside the physical
 * pages behind it. Our hash of the image reads it through the CPU; a table entry that sent some page of it
 * elsewhere would hand the modem other bytes without a fault.
 */
#define SMMU_IDR0_ATOSNS        (1u << 26)
#define SMMU_CB_PAR             0x50
#define SMMU_CB_ATS1PR          0x800
#define SMMU_CB_ATSR            0x8f0

static void
smmu_translation_check(void)
{
	const struct mdm_allocation *a = &allocations[0];
	unsigned right = 0, elsewhere = 0, faults = 0, busy = 0;
	uint64_t first_par = 0;

	if (allocation_count == 0 || !smmu_on) {
		return;
	}
	if (rd(smmu + SMMU_IDR0) & SMMU_IDR0_ATOSNS) {
		say("mdm: 4: the SMMU has no address translation operations (idr0 %#x)\n", rd(smmu + SMMU_IDR0));
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	for (uint64_t offset = 0; offset < a->size; offset += 4096) {
		uint64_t va = a->iova + offset, par;
		unsigned spin = 0;

		/* (one 64-bit write: the write is what starts the operation) */
		*(volatile uint64_t *)(smmu_context + SMMU_CB_ATS1PR) = va;
		__asm__ volatile ("dsb sy" ::: "memory");
		while ((rd(smmu_context + SMMU_CB_ATSR) & 1) && spin < 100000) {
			spin++;
		}
		if (spin == 100000) {
			busy++;
			break;
		}
		par = rd(smmu_context + SMMU_CB_PAR) | (uint64_t)rd(smmu_context + SMMU_CB_PAR + 4) << 32;
		if (offset == 0) {
			first_par = par;
		}
		if (par & 1) {
			if (faults++ < 3) {
				say("mdm: 4: translation of %#llx FAULTS: PAR %#llx\n", va, par);
			}
		} else if ((par & 0xfffffffff000ull) != a->physical + offset) {
			if (elsewhere++ < 4) {
				say("mdm: 4: %#llx TRANSLATES TO %#llx, its page is at %#llx\n", va, par & 0xfffffffff000ull, a->physical + offset);
			}
		} else {
			right++;
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	say("mdm: 4: the SMMU's own translation of the image's %llu pages (%#llx -> %#llx): %u right, %u elsewhere, "
	    "%u faults%s; PAR of the first %#llx\n", a->size >> 12, a->iova, a->physical, right, elsewhere, faults,
	    busy ? ", THE OPERATION NEVER FINISHED" : "", first_par);
}

/* PCIe2's requests from the endpoint (bus 1, devfn 0) through the SMMU as stream 0x1d01: umi_pcie_enable_dma()'s
 * route (CRC-8 of the BDF picks the table's entry; BDF, SID offset 1), translation instead of the bypass, then bus
 * mastering on. */
static bool
dma_route(void)
{
	static const uint8_t bdf[] = { 1, 0 };
	uint8_t hash = 0;
	uint32_t route, old, command;

	for (unsigned i = 0; i < sizeof(bdf); i++) {
		hash ^= bdf[i];
		for (unsigned b = 0; b < 8; b++) {
			hash = (uint8_t)((hash << 1) ^ ((hash & 0x80) ? 7 : 0));
		}
	}
	route = 0x2000 + 4u * hash;
	old = rd(parf + route);
	if (old != 0 && old != 0x01000100) {
		say("mdm: 4: PCIe2's BDF-to-SID entry %#x holds %#x already\n", route, old);
		return false;
	}
	wr(parf + route, 0x01000100);
	update(parf + 0x2c00, 1, 0);
	command = rd(conf + 4);
	if (rd(parf + route) != 0x01000100 || (rd(parf + 0x2c00) & 1) || command == UINT32_MAX || (command & 7) != 2) {
		say("mdm: 4: PCIe2's route did not take (entry %#x, configuration %#x, command %#x)\n", rd(parf + route),
		    rd(parf + 0x2c00), command);
		return false;
	}
	/*
	 * PCIe2's way to the memory (Linux's sm8250 interconnect: xm_pcie3_modem -> qns_pcie_modem_mem_noc, BCM SN12, which
	 * nothing keeps alive) voted for, as Android's MHI does before its BHI (msm-bus 108 -> 512, by link speed)
	 */
	{
		int vote = umi_rpmh_bcm_vote("SN12", 0x3fff, 0x3fff);

		say("mdm: 4: the bus vote for PCIe2's way to the memory (BCM SN12): %s (%d)\n", vote == 0 ? "taken" : "NOT TAKEN", vote);
	}
	wr(dbi + 4, 6);                                         /* the root bridge: memory, bus mastering */
	wr(conf + 4, (command & 0xffff) | 4);                   /* the endpoint: bus mastering */
	dma_on = true;
	say("mdm: 4: bus mastering on: the modem reaches our memory as stream %#x (command %#x)\n", MDM_STREAM, rd(conf + 4));
	return (rd(conf + 4) & 7) == 6;
}

/* The modem stopped (PERST), what it was given put back. `leaving`: alioth_mdm_quiesce(), the other cores stopped. */
static void
mdm_stop(bool leaving)
{
	if (dma_on) {
		wr(dbi + 4, 2);                                 /* nothing upstream through the root bridge */
	}
	gpio_out(GPIO_PERST, false, 1);
	for (unsigned i = 0; i < 4000; i++) {           /* (a millisecond or so, no timer needed) */
		(void)rd(tlmm + GPIO_PERST * 0x1000);
	}
	dma_on = false;
	smmu_down(leaving);
}

/* ---- MHI and Sahara over it (bsd/dev/alioth_mhi.c) --------------------------------------------------------- */

#define MDM_IMAGES      32
static struct { uint32_t id; char name[40]; } images[MDM_IMAGES];
static unsigned image_count;
static char bhi_name[40];
static struct alioth_mhi mhi;
static struct alioth_mhi_io mhi_io;

static uint32_t
mhi_read(__unused void *context, uint32_t offset)
{
	return offset < bar_size && !(offset & 3) ? rd(bar + offset) : UINT32_MAX;
}

static void
mhi_write(__unused void *context, uint32_t offset, uint32_t value)
{
	if (offset < bar_size && !(offset & 3)) {
		wr(bar + offset, value);
	}
}

static void
mhi_delay(__unused void *context, unsigned us)
{
	delay(us);
}

/* (mdm_dma_cached=1: what we wrote cleaned to the point of coherency, for a device that does not snoop after all) */
static void
mdm_sync(__unused void *context, const struct alioth_mhi_dma *dma)
{
	if (!dma_cached) {
		return;
	}
	for (vm_offset_t line = (vm_offset_t)dma->bytes & ~(vm_offset_t)63; line < (vm_offset_t)dma->bytes + dma->size; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
mhi_say(__unused void *context, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	say("mdm: %s %#llx %#llx %#llx\n", what, a, b, c);
}

/* fw/sdx55m/sahara.txt (devices/redmi-k40/sdx55m-sahara.txt): "bhi FILE", "ID FILE", '#' to the end of a line. */
static bool
config_load(void)
{
	uint64_t size = 0;
	const char *text = (const char *)hakios_image_file("fw", "sdx55m/sahara.txt", &size, NULL), *end;

	if (text == NULL) {
		say("mdm: 4: no fw/sdx55m/sahara.txt in the system image (MODEM=1 with ~/hk/fw-modem)\n");
		return false;
	}
	end = text + size;
	image_count = 0;
	bhi_name[0] = 0;
	for (const char *p = text; p < end;) {
		const char *line_end = p, *q, *name;
		char word[8];
		unsigned n = 0, length;

		while (line_end < end && *line_end != '\n') {
			line_end++;
		}
		for (q = p; q < line_end && (*q == ' ' || *q == '\t'); q++) {
		}
		while (q < line_end && *q != ' ' && *q != '\t' && *q != '#' && n < sizeof(word) - 1) {
			word[n++] = *q++;
		}
		word[n] = 0;
		while (q < line_end && (*q == ' ' || *q == '\t')) {
			q++;
		}
		for (name = q, length = 0; q < line_end && *q != ' ' && *q != '\t' && *q != '#' && *q != '\r'; q++) {
			length++;
		}
		if (n != 0 && word[0] != '#' && length > 0 && length < sizeof(bhi_name)) {
			if (strcmp(word, "bhi") == 0) {
				memcpy(bhi_name, name, length);
				bhi_name[length] = 0;
			} else if (word[0] >= '0' && word[0] <= '9' && image_count < MDM_IMAGES) {
				images[image_count].id = (uint32_t)strtoul(word, NULL, 10);
				memcpy(images[image_count].name, name, length);
				images[image_count].name[length] = 0;
				image_count++;
			}
		}
		p = line_end + 1;
	}
	say("mdm: 4: sahara.txt: the SBL is %s, %u images by id\n", bhi_name[0] ? bhi_name : "NOT NAMED", image_count);
	return bhi_name[0] != 0;
}

static const uint8_t *
image_named(const char *name, uint64_t *size)
{
	char path[64];

	snprintf(path, sizeof(path), "sdx55m/%s", name);
	return hakios_image_file("fw", path, size, NULL);
}

static const uint8_t *
mdm_image(__unused void *context, uint64_t id, uint64_t *size)
{
	for (unsigned i = 0; i < image_count; i++) {
		if (images[i].id == id) {
			const uint8_t *data = image_named(images[i].name, size);

			say("mdm: 5: the SBL asks for image %llu: %s, %s%llu bytes\n", id, images[i].name, data ? "" : "NOT IN fw/sdx55m, ",
			    data ? *size : 0);
			return data;
		}
	}
	say("mdm: 5: the SBL asks for image %llu, which sahara.txt does not name\n", id);
	return NULL;
}

/*
 * Both ends' PCIe error state (did the modem's reads come back whole?): the endpoint's PCI Express capability (found
 * through its list: device control/status -- the correctable, non-fatal, fatal and unsupported-request flags, MPS,
 * MRRS -- and the link status) and its AER (extended capability 1: uncorrectable/correctable status, the header log
 * of the first error); the root port's the same (DWC: capability at 0x70, AER at 0x100).
 */
static void
pcie_errors(const char *when)
{
	uint32_t at = rd(conf + 0x34) & 0xfc, pcie = 0, aer = 0, msi = 0;

	for (unsigned n = 0; at >= 0x40 && at < 0x100 && n < 32; n++) {
		uint32_t head = rd(conf + at);

		if ((head & 0xff) == 0x10) {
			pcie = at;
		} else if ((head & 0xff) == 0x05) {
			msi = at;
		}
		at = (head >> 8) & 0xfc;
	}
	for (unsigned n = 0, x = 0x100; x >= 0x100 && x < 0x1000 && n < 32; n++) {
		uint32_t head = rd(conf + x);

		if (head == 0 || head == UINT32_MAX) {
			break;
		}
		if ((head & 0xffff) == 1) {
			aer = x;
			break;
		}
		x = head >> 20;
	}
	if (pcie != 0) {
		uint32_t dev = rd(conf + pcie + 8), link = rd(conf + pcie + 0x10);

		say("mdm: %s: endpoint device control/status %#x (status: correctable %u, non-fatal %u, fatal %u, UR %u; MPS %u, "
		    "MRRS %u), link status Gen%u x%u\n", when, dev, dev >> 16 & 1, dev >> 17 & 1, dev >> 18 & 1, dev >> 19 & 1,
		    128u << (dev >> 5 & 7), 128u << (dev >> 12 & 7), link >> 16 & 0xf, link >> 20 & 0x3f);
	}
	if (msi != 0) {
		say("mdm: %s: endpoint MSI control %#x (%s)\n", when, rd(conf + msi) >> 16, rd(conf + msi) & (1u << 16) ? "on" : "off");
	}
	if (aer != 0) {
		say("mdm: %s: endpoint AER: uncorrectable %#x (mask %#x), correctable %#x, header log %#x %#x %#x %#x\n", when,
		    rd(conf + aer + 4), rd(conf + aer + 8), rd(conf + aer + 0x10), rd(conf + aer + 0x1c), rd(conf + aer + 0x20),
		    rd(conf + aer + 0x24), rd(conf + aer + 0x28));
	}
	{
		uint32_t dev = rd(dbi + 0x78);

		say("mdm: %s: root device control/status %#x (status: correctable %u, non-fatal %u, fatal %u, UR %u; MPS %u, "
		    "MRRS %u); root AER (if at 0x100: %#x): uncorrectable %#x, correctable %#x, root status %#x\n", when, dev,
		    dev >> 16 & 1, dev >> 17 & 1, dev >> 18 & 1, dev >> 19 & 1, 128u << (dev >> 5 & 7), 128u << (dev >> 12 & 7),
		    rd(dbi + 0x100), rd(dbi + 0x104), rd(dbi + 0x110), rd(dbi + 0x130));
	}
}

/*
 * mdm_msi=1: the GIC's SETSPI page put at the window's last page (a device's page, MAIR entry 2), then the endpoint's
 * MSI capability given that address and the data MDM_MSI_SPI, with every vector it offers, and switched on -- the
 * OnePlus 8 Pro port's msi_enable() (see MDM_MSI_REGISTER). Before BHI: the PBL probably raises its completion
 * interrupt and fails the transfer when it cannot. Returns whether the capability was there and is on.
 */
static bool
msi_enable(void)
{
	uint32_t at = rd(conf + 0x34) & 0xfc;

	if (!msi_page) {
		uint64_t offset = MDM_MSI_IOVA - MDM_WINDOW;
		uint64_t *third = (uint64_t *)(tables + (2 + (offset >> 21)) * 4096);

		if (window_next > offset) {
			say("mdm: 4: msi: the window's last page is already handed out: no MSI\n");
			return false;
		}
		third[offset >> 12 & 0x1ff] = (MDM_MSI_REGISTER & ~0xfffull) | MDM_PTE_DEVICE;
		__asm__ volatile ("dc civac, %0" : : "r" (&third[offset >> 12 & 0x1ff]));
		__asm__ volatile ("dsb sy" ::: "memory");
		smmu_forget();
		msi_page = true;
		say("mdm: 4: msi: THE MODEM'S SMMU CONTEXT GETS AN ENTRY for the GIC's SETSPI register (%#llx) at %#llx, as a "
		    "device's page\n", MDM_MSI_REGISTER, MDM_MSI_IOVA);
	}
	for (unsigned n = 0; at >= 0x40 && at < 0x100 && n < 32; n++) {
		uint32_t head = rd(conf + at);

		if ((head & 0xff) == 0x05) {
			uint32_t control = head >> 16;
			bool wide = (control & 0x80) != 0;
			uint64_t address = MDM_MSI_IOVA + (MDM_MSI_REGISTER & 0xfff);
			uint32_t on = (control & ~0x70u) | (control >> 1 & 7u) << 4 | 1u;   /* MME = MMC: all it offers; enable */

			wr(conf + at + 4, (uint32_t)address);
			if (wide) {
				wr(conf + at + 8, (uint32_t)(address >> 32));
				wr(conf + at + 12, MDM_MSI_SPI);
			} else {
				wr(conf + at + 8, MDM_MSI_SPI);
			}
			wr(conf + at, (head & 0xffffu) | on << 16);
			say("mdm: 4: msi: capability at %#x (control %#x -> %#x, %s, %u vector%s): address %#llx, data %u\n", at,
			    control, rd(conf + at) >> 16, wide ? "64-bit" : "32-bit", 1u << (control >> 1 & 7),
			    (control >> 1 & 7) ? "s" : "", address, MDM_MSI_SPI);
			return (rd(conf + at) >> 16 & 1) != 0;
		}
		at = (head >> 8) & 0xfc;
	}
	say("mdm: 4: msi: the endpoint has no MSI capability\n");
	return false;
}

static const uint8_t *sbl;
static uint64_t sbl_size;

/* sahara.txt and the SBL read before the modem is reset (step 2): from the reset to BHI only the hardware's work */
static bool
images_ready(void)
{
	if (!config_load()) {
		return false;
	}
	if ((sbl = image_named(bhi_name, &sbl_size)) == NULL) {
		say("mdm: 4: no fw/sdx55m/%s\n", bhi_name);
		return false;
	}
	{
		/* (the file as the kernel has it, FNV-1a: sbl1.mbn of ~/hk/fw-modem is 548056 bytes, 0xa4dfb904) */
		uint32_t hash = 2166136261u;

		for (uint64_t i = 0; i < sbl_size; i++) {
			hash = (hash ^ sbl[i]) * 16777619u;
		}
		say("mdm: 4: %s: %llu bytes, FNV-1a %#x, first bytes %02x %02x %02x %02x\n", bhi_name, sbl_size, hash, sbl[0],
		    sbl[1], sbl[2], sbl[3]);
	}
	return true;
}

static bool
step_mhi(void)
{
	static const struct alioth_mhi_known known[] = {
		{ 2, ALIOTH_MHI_TO_DEVICE, 1 }, { 3, ALIOTH_MHI_FROM_DEVICE, 1 }, { 25, ALIOTH_MHI_FROM_DEVICE, 1 },
		{ 10, ALIOTH_MHI_TO_DEVICE, 1 }, { 11, ALIOTH_MHI_FROM_DEVICE, 1 },
		{ 20, ALIOTH_MHI_TO_DEVICE, 2 }, { 21, ALIOTH_MHI_FROM_DEVICE, 2 },
	};
	uint64_t size = sbl_size;
	uint32_t ee = ~0u;
	int error;

	say("mdm: 4: the SMMU, bus mastering, MHI: the SBL through BHI, READY, M0\n");
	if (sbl == NULL) {
		return false;
	}
	{
		uint32_t cached_dma = 0;

		dma_cached = PE_parse_boot_argn("mdm_dma_cached", &cached_dma, sizeof(cached_dma)) && cached_dma != 0;
		say("mdm: 4: the modem's memory: %s\n", dma_cached ? "CACHED, write-back cacheable and inner shareable to the SMMU "
		    "(mdm_dma_cached=1, Android's way for a dma-coherent device)" : "uncached, non-cacheable to the SMMU (the Wi-Fi's way)");
	}
	if (!smmu_up() || !dma_route()) {
		return false;
	}
	{
		uint32_t want_msi = 0;

		if (PE_parse_boot_argn("mdm_msi", &want_msi, sizeof(want_msi)) && want_msi != 0) {
			(void)msi_enable();
		} else {
			say("mdm: 4: msi: left off (mdm_msi=1 switches it on before BHI)\n");
		}
	}
	/* (in the structure's order: "say" is this file's macro for hakios_tl, so no designators) */
	mhi_io = (struct alioth_mhi_io){ NULL, bar_size, MDM_WINDOW, MDM_WINDOW + MDM_WINDOW_SIZE - 1, mhi_read, mhi_write,
		                         mdm_allocate, mhi_delay, mhi_say, mdm_sync };
	memset(&mhi, 0, sizeof(mhi));
	error = alioth_mhi_probe(&mhi, &mhi_io);
	{
		/* boot-arg mdm_bhi_probe=1 (diagnosis): BHI given an address of the window with nothing mapped behind it --
		 * if the modem's reads go through our context, it records a fault there */
		uint32_t probe = 0;

		if (PE_parse_boot_argn("mdm_bhi_probe", &probe, sizeof(probe)) && probe != 0) {
			mhi.bhi_probe = MDM_WINDOW + MDM_WINDOW_SIZE - 0x100000;
		}
	}
	if (error == 0) {
		error = alioth_mhi_execution_environment(&mhi, &ee);
	}
	if (error == 0 && ee != ALIOTH_MHI_EE_PBL) {
		say("mdm: 4: the modem is in execution environment %u, not its boot ROM: no BHI\n", ee);
		error = EBUSY;
	}
	if (error == 0) {
		const uint8_t *image = sbl;
		uint32_t corrupt = 0, ats = 0;

		/*
		 * boot-arg mdm_bhi_corrupt=N (diagnosis: which of the PBL's checks gives 0xef0e0003?): the image given with
		 * 1 the ELF magic's first byte flipped, 2 one byte flipped inside the first loaded segment (file offset
		 * 0x10000), 3 one byte flipped in the signature at the end, 4 only its first 64 KB, 5 one byte flipped at
		 * the file offset mdm_bhi_corrupt_at= says. (2026-10-31: 2 answers 0xef0a0105 after 26 ms -- a segment's hash
		 * -- where the image as it is gets 0xef0e0003 after 38 ms: the signature passes, the segments' hashes are
		 * checked one by one, and something after segment 5 stops the true image; 5 finds where.)
		 */
		if (PE_parse_boot_argn("mdm_bhi_corrupt", &corrupt, sizeof(corrupt)) && corrupt >= 1 && corrupt <= 5) {
			vm_offset_t copy = 0;
			uint64_t corrupt_at = 0;

			(void)PE_parse_boot_argn("mdm_bhi_corrupt_at", &corrupt_at, sizeof(corrupt_at));
			if (kmem_alloc(kernel_map, &copy, round_page(size), KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
				uint8_t *c = (uint8_t *)copy;
				uint64_t at = corrupt == 1 ? 0 : corrupt == 2 ? 0x10000 : corrupt == 5 ? corrupt_at : size - 16;

				memcpy(c, sbl, (size_t)size);
				if (corrupt == 4) {
					size = 0x10000;
				} else if (at < size) {
					c[at] ^= 0xff;
				}
				image = c;
				say("mdm: 4: THE IMAGE GIVEN IS MADE WRONG ON PURPOSE (mdm_bhi_corrupt=%u: %s, file offset %#llx)\n", corrupt,
				    corrupt == 1 ? "the ELF magic" : corrupt == 2 ? "a byte of the first segment" :
				    corrupt == 3 ? "a byte of the signature" : corrupt == 5 ? "a byte where mdm_bhi_corrupt_at says" :
				    "only its first 64 KB", corrupt == 4 ? 0x10000ull : at);
			}
		}
		pcie_errors("4: before BHI");
		say("mdm: 4: BHI %llu ms after the reset (Android: some 300)\n", since_reset_ms());
		error = alioth_mhi_bhi(&mhi, image, (size_t)size, 2000);
		pcie_errors("4: after BHI");
		if (PE_parse_boot_argn("mdm_ats", &ats, sizeof(ats)) && ats != 0) {
			smmu_translation_check();
		}
	}
	if (error == 0) {
		error = alioth_mhi_ready(&mhi, known, sizeof(known) / sizeof(known[0]), 5000);
	}
	say("mdm: 4: %s (%d): execution environment %u, MHI state %u; MDM2AP_STATUS %d, MDM2AP_ERRFATAL %d\n",
	    error ? "MHI STOPPED" : "THE SBL RUNS, MHI IN M0", error, mhi.ee, mhi.state, gpio_in(GPIO_MDM2AP_STATUS),
	    gpio_in(GPIO_MDM2AP_ERRFATAL));
	if (error != 0) {
		/* the link and both ends' PCI status (bits 28/29: a target/master abort received, 27 signalled, 30 SERR):
		 * did a request of the modem's, or of ours, fail on the way? */
		say("mdm: 4: link status %#x, LTSSM %#x; endpoint command/status %#x, root command/status %#x\n", rd(dbi + 0x80),
		    rd(parf + 0x24), rd(conf + 4), rd(dbi + 4));
	}
	smmu_faults();
	return error == 0;
}

static bool
step_sahara(void)
{
	int error;

	say("mdm: 5: Sahara: the SBL's images, up to mission mode\n");
	error = alioth_mhi_sahara(&mhi, mdm_image, NULL, 5000);
	say("mdm: 5: %s (%d): execution environment %u; MDM2AP_STATUS %d, MDM2AP_ERRFATAL %d; %u events, %u link requests\n",
	    error ? "SAHARA STOPPED" : "THE MODEM IS IN MISSION MODE", error, mhi.ee, gpio_in(GPIO_MDM2AP_STATUS),
	    gpio_in(GPIO_MDM2AP_ERRFATAL), mhi.events, mhi.bandwidth_requests);
	smmu_faults();
	return error == 0;
}

/* ---- step 6: QRTR on the IPCR channels (20 to the modem, 21 from it, event ring 2): the modem's services ---- */

#define IPCR_TO_DEVICE          20
#define IPCR_FROM_DEVICE        21
#define IPCR_EVENT_RING         2
#define IPCR_ELEMENTS           32
#define IPCR_SLOT               0x2000
#define IPCR_QUEUE              8
#define IPCR_PACKET_MAX         0x4000
#define QRTR_OUR_NODE           1               /* the application processor's, as Linux numbers it */
#define IPCR_LISTEN_MS          60000           /* (11-13: the first 15 services came at once, then 30 s of nothing) */
#define MDM_SERVERS             64

static struct {
	unsigned head, tail;
	uint32_t partial, dropped;
	bool skipping;
	uint32_t length[IPCR_QUEUE];
	uint8_t packet[IPCR_QUEUE][IPCR_PACKET_MAX];
} ipcr;
static struct qrtr_server servers[MDM_SERVERS];
static unsigned server_count;

/* (from the poll: only queued, answered by step_qrtr()'s loop; alioth_sahara's way) */
static void
ipcr_received(__unused void *context, __unused unsigned channel, const uint8_t *bytes, uint32_t length, bool end)
{
	unsigned slot = ipcr.tail % IPCR_QUEUE;

	if (ipcr.partial == 0 && !ipcr.skipping && ipcr.tail - ipcr.head == IPCR_QUEUE) {
		ipcr.skipping = true;
	}
	if (!ipcr.skipping) {
		if (ipcr.partial + length <= IPCR_PACKET_MAX) {
			memcpy(ipcr.packet[slot] + ipcr.partial, bytes, length);
		}
		ipcr.partial += length;
	}
	if (!end) {
		return;
	}
	if (ipcr.skipping) {
		ipcr.dropped++;
		ipcr.skipping = false;
		return;
	}
	ipcr.length[slot] = ipcr.partial <= IPCR_PACKET_MAX ? ipcr.partial : 0;
	ipcr.partial = 0;
	ipcr.tail++;
}

static int
ipcr_send(const uint8_t *packet, size_t length)
{
	return length == 0 ? EMSGSIZE : alioth_mhi_send(&mhi, IPCR_TO_DEVICE, packet, (uint32_t)length, true, 2000);
}

/*
 * ---- the AP's TFTP server over QRTR (service 4096, port 0x4002), as Android's tftp_server ----
 * (2026-11-14) In mission mode the SDX55 announced 15 services and then waited, silent, for 30 s: no DMS, NAS or LOC.
 * The OnePlus 8 Pro port (dekefake/hackios-op8pro docs/research/modem-sdx55.md, "TFTP service") found its MPSS and
 * application processor looking up [service 4096, instance 3] -- the AP's TFTP server, where they read their carrier
 * configs and keep their EFS -- "could not resolve remote host". So the server is announced to nodes 2 and 3 with
 * the instance words they tried (0..5 raw and 1|i<<8). FIRST STEP, nothing of the phone's storage is involved:
 * every read is logged and answered "File not found" (the modem partition's files are not in the system image yet
 * -- this says which ones it wants), every write is taken and thrown away (an EFS sync: never written anywhere).
 */
#define TFTP_SERVICE            4096u
#define TFTP_PORT               0x4002u
#define TFTP_BLOCK_MAX          1024u

static unsigned tftp_reads, tftp_writes, tftp_blocks, tftp_said;
static uint64_t tftp_written;

static int
tftp_reply(uint32_t node, uint32_t port, const uint8_t *payload, uint32_t size)
{
	uint8_t out[QRTR_V1_HEADER + 160];

	return ipcr_send(out, qrtr_make(out, sizeof(out), QRTR_TYPE_DATA, QRTR_OUR_NODE, TFTP_PORT, node, port, payload, size,
	    false));
}

static int
tftp_error(uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t p[64];
	size_t n = strlen(text);

	if (n > sizeof(p) - 5) {
		n = sizeof(p) - 5;
	}
	p[0] = 0, p[1] = 5, p[2] = (uint8_t)(code >> 8), p[3] = (uint8_t)code;
	memcpy(p + 4, text, n);
	p[4 + n] = 0;
	return tftp_reply(node, port, p, (uint32_t)(5 + n));
}

static int
tftp_announce(void)
{
	static const uint32_t nodes[] = { 2, 3 };
	uint8_t out[64];
	int error = 0;

	for (unsigned n = 0; n < 2 && error == 0; n++) {
		for (uint32_t i = 0; i <= 5 && error == 0; i++) {
			const uint32_t raw[4] = { TFTP_SERVICE, i, QRTR_OUR_NODE, TFTP_PORT };
			const uint32_t shifted[4] = { TFTP_SERVICE, 1u | i << 8, QRTR_OUR_NODE, TFTP_PORT };

			error = ipcr_send(out, qrtr_make_control(out, sizeof(out), QRTR_TYPE_NEW_SERVER, QRTR_OUR_NODE, nodes[n], raw));
			if (error == 0 && i != 0) {
				error = ipcr_send(out, qrtr_make_control(out, sizeof(out), QRTR_TYPE_NEW_SERVER, QRTR_OUR_NODE, nodes[n],
				    shifted));
			}
		}
	}
	say("mdm: 6: tftp: service %u at node %u port %#x announced to nodes 2 and 3 (%d)\n", TFTP_SERVICE, QRTR_OUR_NODE,
	    TFTP_PORT, error);
	return error;
}

static int
tftp_packet(const struct qrtr_packet *p)
{
	const uint8_t *d = p->payload;
	uint32_t opcode = p->size >= 2 ? (uint32_t)(d[0] << 8 | d[1]) : 0;

	if (opcode == 1 || opcode == 2) {
		/* a request: file name, mode, then option/value pairs, each ended by a zero */
		const char *name = (const char *)d + 2, *end = (const char *)d + p->size, *at;
		char opts[120] = "";
		uint32_t blksize = 512;
		bool oack = false, wsize = false;

		at = name + strnlen(name, (size_t)(end - name)) + 1;
		at += at < end ? strnlen(at, (size_t)(end - at)) + 1 : 0;
		while (at < end) {
			const char *value = at + strnlen(at, (size_t)(end - at)) + 1;

			if (value >= end) {
				break;
			}
			if (strncmp(at, "blksize", 8) == 0) {
				blksize = (uint32_t)strtoul(value, NULL, 10);
				blksize = blksize < 8 ? 8 : blksize > TFTP_BLOCK_MAX ? TFTP_BLOCK_MAX : blksize;
				oack = true;
			} else if (strncmp(at, "wsize", 6) == 0) {
				wsize = oack = true;
			} else if (strncmp(at, "tsize", 6) == 0 || strncmp(at, "timeout", 8) == 0 || strncmp(at, "timeoutms", 10) == 0) {
				oack = true;
			}
			strlcat(opts, at, sizeof(opts));
			strlcat(opts, "=", sizeof(opts));
			strlcat(opts, value, sizeof(opts));
			strlcat(opts, " ", sizeof(opts));
			at = value + strnlen(value, (size_t)(end - value)) + 1;
		}
		if (tftp_said++ < 80) {
			say("mdm: 6: tftp: %s \"%.*s\" from %u:%u, options %s\n", opcode == 1 ? "READ" : "WRITE",
			    (int)strnlen(name, (size_t)(end - name)), name, p->src_node, p->src_port, opts[0] ? opts : "none");
		}
		if (opcode == 1) {
			tftp_reads++;
			return tftp_error(p->src_node, p->src_port, 1, "File not found");
		}
		tftp_writes++;
		if (oack) {
			/* the options taken (blksize at most ours; wsize must be answered, 1, or the client waits for ten blocks) */
			uint8_t o[64];
			int n;

			o[0] = 0, o[1] = 6;
			n = snprintf((char *)o + 2, sizeof(o) - 2, "blksize%c%u%c%s", 0, blksize, 0, wsize ? "wsize" : "");
			n += 2;
			if (wsize) {
				o[n++] = 0, o[n++] = '1', o[n++] = 0;
			}
			return tftp_reply(p->src_node, p->src_port, o, (uint32_t)n);
		}
		{
			const uint8_t ack[4] = { 0, 4, 0, 0 };

			return tftp_reply(p->src_node, p->src_port, ack, 4);
		}
	}
	if (opcode == 3 && p->size >= 4) {
		/* a block of a write: acknowledged, and thrown away */
		const uint8_t ack[4] = { 0, 4, d[2], d[3] };

		tftp_blocks++;
		tftp_written += p->size - 4;
		return tftp_reply(p->src_node, p->src_port, ack, 4);
	}
	if (tftp_said++ < 80) {
		say("mdm: 6: tftp: opcode %u (%u bytes) from %u:%u%s\n", opcode, p->size, p->src_node, p->src_port,
		    opcode == 5 ? ": an ERROR from the client" : "");
	}
	return 0;
}

/*
 * ---- QMI LOC (service 0x10): a GNSS session of our own (boot-arg mdm_loc=1, with mdm_step=6) ----
 * (2026-11-14) With the TFTP server answering, the SDX55's MPSS came all the way up: 64 services, LOC (0x10) on node 3
 * among them. A client port of ours (node 1, LOC_PORT) asks, one after the other, waiting for each response (or 2 s):
 * INFORM_CLIENT_REVISION 2, REG_EVENTS (position, satellites, engine state, fix session state), SET_OPERATION_MODE
 * standalone (no SIM, no network: nothing to assist), SET_ENGINE_LOCK none (the OnePlus 8 Pro port saw every session
 * end at once with status 7, "engine locked", until it sent this), then START: session 1, periodic, high accuracy,
 * intermediate reports, 1 s. NO COORDINATE IS EVER SAID: the position report gives status, accuracy and the satellites'
 * count to the log; latitude/longitude are kept as their raw bits for iOS later (the owner's location is private).
 * Floating point is not used in the kernel: the accuracy is taken from its IEEE single's bits by integer arithmetic.
 */
#define LOC_PORT                0x4003u
#define QMI_REQUEST             0x00
#define QMI_RESPONSE            0x02
#define QMI_INDICATION          0x04
#define LOC_INFORM_REVISION     0x0020
#define LOC_REG_EVENTS          0x0021
#define LOC_START               0x0022
#define LOC_POSITION_REPORT     0x0024
#define LOC_SV_INFO             0x0025
#define LOC_SET_ENGINE_LOCK     0x003a
#define LOC_SET_OPERATION_MODE  0x004a
#define LOC_EVENTS              (0x1ull | 0x2ull | 0x80ull | 0x100ull)    /* position, SV info, engine state, fix session */

static struct {
	bool wanted, found;
	uint32_t node, port;
	uint16_t txn, waiting;          /* the request whose response is awaited (0: none) */
	unsigned step;                  /* 0 revision, 1 events, 2 mode, 3 lock, 4 start, 5 running */
	uint64_t sent_at;
	unsigned responses, reports, fixes, sv_infos, said;
	uint32_t best_m, last_status, most_sats;
	uint64_t lat_bits, lon_bits;    /* (the last fix's, never said) */
} loc = { .best_m = UINT32_MAX };

static uint32_t
loc_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* an IEEE single's value, whole metres (truncated), from its bits */
static uint32_t
loc_f32_metres(uint32_t bits)
{
	int exponent = (int)((bits >> 23) & 0xff) - 127;
	uint64_t mantissa = (bits & 0x7fffff) | 0x800000;

	if ((bits >> 31) || exponent < 0) {
		return 0;
	}
	if (exponent > 31) {
		return UINT32_MAX;
	}
	return (uint32_t)(exponent >= 23 ? mantissa << (exponent - 23) : mantissa >> (23 - exponent));
}

static int
loc_send(uint16_t message, const uint8_t *tlvs, uint16_t tlv_bytes)
{
	uint8_t q[7 + 64], out[QRTR_V1_HEADER + sizeof(q) + 4];

	if (tlv_bytes > sizeof(q) - 7) {
		return EMSGSIZE;
	}
	loc.txn++;
	q[0] = QMI_REQUEST;
	q[1] = (uint8_t)loc.txn, q[2] = (uint8_t)(loc.txn >> 8);
	q[3] = (uint8_t)message, q[4] = (uint8_t)(message >> 8);
	q[5] = (uint8_t)tlv_bytes, q[6] = (uint8_t)(tlv_bytes >> 8);
	memcpy(q + 7, tlvs, tlv_bytes);
	loc.waiting = message;
	loc.sent_at = since_reset_ms();
	return ipcr_send(out, qrtr_make(out, sizeof(out), QRTR_TYPE_DATA, QRTR_OUR_NODE, LOC_PORT, loc.node, loc.port, q,
	    (uint32_t)(7 + tlv_bytes), false));
}

/* a TLV of `n` bytes of `value` (little-endian) at t, its length */
static uint16_t
loc_tlv(uint8_t *t, uint8_t type, uint64_t value, uint16_t n)
{
	t[0] = type, t[1] = (uint8_t)n, t[2] = (uint8_t)(n >> 8);
	for (uint16_t i = 0; i < n; i++) {
		t[3 + i] = (uint8_t)(value >> (8 * i));
	}
	return (uint16_t)(3 + n);
}

/* the next request of the sequence, if the last one was answered or 2 s have gone by */
static int
loc_tick(void)
{
	uint8_t t[64];
	uint16_t n = 0;

	if (!loc.wanted || !loc.found || loc.step >= 5 || (loc.waiting != 0 && since_reset_ms() - loc.sent_at < 2000)) {
		return 0;
	}
	if (loc.waiting != 0) {
		say("mdm: 7: loc: no response to %#x in 2 s: on to the next\n", loc.waiting);
		loc.waiting = 0;
		loc.step++;
	}
	switch (loc.step) {
	case 0:
		n = loc_tlv(t, 0x01, 2, 4);
		say("mdm: 7: loc: the session begins (LOC at node %u port %u): INFORM_CLIENT_REVISION 2\n", loc.node, loc.port);
		return loc_send(LOC_INFORM_REVISION, t, n);
	case 1:
		n = loc_tlv(t, 0x01, LOC_EVENTS, 8);
		return loc_send(LOC_REG_EVENTS, t, n);
	case 2:
		n = loc_tlv(t, 0x01, 4, 4);                     /* STANDALONE */
		return loc_send(LOC_SET_OPERATION_MODE, t, n);
	case 3:
		n = loc_tlv(t, 0x01, 1, 4);                     /* LOCK_NONE */
		return loc_send(LOC_SET_ENGINE_LOCK, t, n);
	case 4:
		n = loc_tlv(t, 0x01, 1, 1);                     /* session id */
		n += loc_tlv(t + n, 0x10, 1, 4);                /* fix recurrence: periodic */
		n += loc_tlv(t + n, 0x11, 3, 4);                /* horizontal accuracy: high */
		n += loc_tlv(t + n, 0x12, 1, 4);                /* intermediate reports: on */
		n += loc_tlv(t + n, 0x13, 1000, 4);             /* minimum interval, ms */
		return loc_send(LOC_START, t, n);
	}
	return 0;
}

static int
loc_packet(const struct qrtr_packet *p)
{
	const uint8_t *d = p->payload, *tlv, *end;
	uint16_t message, length;
	uint32_t status = UINT32_MAX, unc_bits = 0, sats = 0;
	bool have_unc = false, have_lat = false;

	if (p->size < 7) {
		return 0;
	}
	message = (uint16_t)(d[3] | d[4] << 8);
	length = (uint16_t)(d[5] | d[6] << 8);
	end = d + 7 + (length < p->size - 7 ? length : p->size - 7);
	for (tlv = d + 7; tlv + 3 <= end;) {
		uint8_t type = tlv[0];
		uint16_t n = (uint16_t)(tlv[1] | tlv[2] << 8);
		const uint8_t *v = tlv + 3;

		if (v + n > end) {
			break;
		}
		if (d[0] == QMI_RESPONSE && type == 0x02 && n >= 4) {
			status = (uint32_t)(v[0] | v[1] << 8) << 16 | (uint32_t)(v[2] | v[3] << 8);   /* result << 16 | error */
		} else if (d[0] == QMI_INDICATION && type == 0x01 && n >= 4) {
			status = loc_le32(v);
		} else if (message == LOC_POSITION_REPORT && type == 0x10 && n == 8) {
			memcpy(&loc.lat_bits, v, 8);
			have_lat = true;
		} else if (message == LOC_POSITION_REPORT && type == 0x11 && n == 8) {
			memcpy(&loc.lon_bits, v, 8);
		} else if (message == LOC_POSITION_REPORT && type == 0x12 && n == 4) {
			unc_bits = loc_le32(v);
			have_unc = true;
		} else if (message == LOC_SV_INFO && type == 0x10 && n >= 1) {
			sats = v[0];
		}
		tlv = v + n;
	}
	if (d[0] == QMI_RESPONSE) {
		loc.responses++;
		say("mdm: 7: loc: response to %#x: result %u error %u\n", message, status >> 16, status & 0xffff);
		if (message == loc.waiting) {
			loc.waiting = 0;
			loc.step++;
			if (loc.step == 5) {
				say("mdm: 7: loc: STARTED (%s)\n", status == 0 ? "accepted" : "REFUSED");
			}
		}
		return 0;
	}
	if (message == LOC_POSITION_REPORT) {
		loc.reports++;
		loc.last_status = status;
		if (have_lat && have_unc && (status == 0 || status == 1)) {
			uint32_t m = loc_f32_metres(unc_bits);

			loc.fixes++;
			if (m < loc.best_m) {
				loc.best_m = m;
			}
			if (loc.fixes <= 5 || loc.fixes % 30 == 0) {
				say("mdm: 7: loc: A FIX (%u so far): status %u, accuracy %u m (the position itself is not said)\n",
				    loc.fixes, status, m);
			}
		} else if (loc.reports <= 10 || loc.reports % 30 == 0) {
			say("mdm: 7: loc: position report %u: status %u (0 done, 1 in progress, 3 timeout, 7 engine locked), no fix\n",
			    loc.reports, status);
		}
		return 0;
	}
	if (message == LOC_SV_INFO) {
		loc.sv_infos++;
		if (sats > loc.most_sats) {
			loc.most_sats = sats;
		}
		if (loc.sv_infos <= 3 || loc.sv_infos % 30 == 0) {
			say("mdm: 7: loc: satellites in view: %u (most so far %u)\n", sats, loc.most_sats);
		}
		return 0;
	}
	if (loc.said++ < 40) {
		say("mdm: 7: loc: indication %#x, status %u, %u bytes\n", message, status, length);
	}
	return 0;
}

static int
ipcr_handle(const uint8_t *bytes, uint32_t length)
{
	struct qrtr_packet p;
	struct qrtr_server s;
	uint8_t out[64];

	if (!qrtr_parse(bytes, length, &p)) {
		say("mdm: 6: %u bytes that are no QRTR packet (%02x %02x %02x %02x)\n", length, length > 0 ? bytes[0] : 0,
		    length > 1 ? bytes[1] : 0, length > 2 ? bytes[2] : 0, length > 3 ? bytes[3] : 0);
		return 0;
	}
	switch (p.type) {
	case QRTR_TYPE_HELLO:
		say("mdm: 6: HELLO from node %u (QRTR version %u)\n", p.src_node, p.version);
		return tftp_announce();         /* (our one service, told again to a router that has just said hello) */
	case QRTR_TYPE_NEW_SERVER:
	case QRTR_TYPE_DEL_SERVER:
		if (!qrtr_server_of(&p, &s)) {
			return 0;
		}
		if (s.service == 0 && s.node == 0 && s.port == 0) {
			say("mdm: 6: (the end of a list of servers)\n");
			return 0;
		}
		/* (the router sends its whole list again and again: each service said and kept once) */
		if (p.type == QRTR_TYPE_NEW_SERVER) {
			for (unsigned i = 0; i < server_count; i++) {
				if (servers[i].service == s.service && servers[i].instance == s.instance && servers[i].node == s.node &&
				    servers[i].port == s.port) {
					return 0;
				}
			}
		}
		say("mdm: 6: %s service %u (%#x) version %u instance %u at node %u port %u%s\n",
		    p.type == QRTR_TYPE_NEW_SERVER ? "NEW" : "GONE:", s.service, s.service, s.version, s.instance, s.node, s.port,
		    s.service == QRTR_SERVICE_LOC ? ": LOCATION (QMI LOC)" : "");
		if (p.type == QRTR_TYPE_NEW_SERVER && server_count < MDM_SERVERS) {
			servers[server_count++] = s;
		}
		if (p.type == QRTR_TYPE_NEW_SERVER && s.service == QRTR_SERVICE_LOC && !loc.found) {
			loc.found = true;
			loc.node = s.node;
			loc.port = s.port;
		}
		return 0;
	case QRTR_TYPE_NEW_LOOKUP:
		/* we serve nothing yet: the list's end straight away, to the client that asked (Linux qrtr-ns's answer) */
		say("mdm: 6: node %u port %u looks for service %u instance %#x: we have none\n", p.src_node, p.src_port,
		    p.size >= QRTR_CONTROL_SIZE ? qrtr_le32(p.payload + 4) : 0, p.size >= QRTR_CONTROL_SIZE ? qrtr_le32(p.payload + 8) : 0);
		return ipcr_send(out, qrtr_make(out, sizeof(out), QRTR_TYPE_NEW_SERVER, QRTR_OUR_NODE, QRTR_PORT_CTRL, p.src_node,
		    p.src_port, (const uint8_t[QRTR_CONTROL_SIZE]){ QRTR_TYPE_NEW_SERVER }, QRTR_CONTROL_SIZE, false));
	case QRTR_TYPE_DATA:
		if (p.dst_port == TFTP_PORT || p.dst_port == LOC_PORT) {
			int error = p.dst_port == TFTP_PORT ? tftp_packet(&p) : loc_packet(&p);

			if (error != 0 || !p.confirm_rx) {
				return error;
			}
		} else {
			say("mdm: 6: data from %u:%u to port %#x, %u bytes (nobody listens there yet)%s\n", p.src_node, p.src_port,
			    p.dst_port, p.size, p.confirm_rx ? "; RESUME_TX sent" : "");
		}
		/*
		 * (2026-11-13) A sender that asks for it (confirm_rx) waits for RESUME_TX once it has sent its flow limit (10
		 * datagrams) to a port: Linux's qrtr_send_resume_tx(), from the port it reached, to the sender, a control
		 * message of the whole 20 bytes (cmd 7, our node, the port, two zero words). The OnePlus 8 Pro port found the
		 * SDX55 ignoring a 12-byte one ("control message too short") and every QMI reply and TFTP request stopping
		 * after ~13 s (dekefake/hackios-op8pro, docs/research/modem-sdx55.md).
		 */
		if (p.confirm_rx) {
			uint8_t resume[QRTR_CONTROL_SIZE] = { 0 };

			qrtr_put32(resume, QRTR_TYPE_RESUME_TX);
			qrtr_put32(resume + 4, QRTR_OUR_NODE);
			qrtr_put32(resume + 8, p.dst_port);
			return ipcr_send(out, qrtr_make(out, sizeof(out), QRTR_TYPE_RESUME_TX, QRTR_OUR_NODE, p.dst_port, p.src_node,
			    p.src_port, resume, QRTR_CONTROL_SIZE, false));
		}
		return 0;
	default:
		say("mdm: 6: control %u from %u:%u (%u bytes)\n", p.type, p.src_node, p.src_port, p.size);
		return 0;
	}
}

/*
 * ---- the EFS channels (10 to the modem, 11 from it): the SDX55's remote file system over Sahara ----
 * (2026-11-14) 103 s into the first GNSS session every register read all ones: the modem had restarted. The OnePlus 8
 * Pro port names the cause: the modem's application processor (remotefs_sahara.c) talks Sahara to the host on these
 * channels about 75-100 s after the mission begins -- HELLO, then memory debug (the host is to read its EFS image and
 * keep it: Qualcomm's ks -m) -- and with nobody answering it gives up with "ssr:AP:remotefs_sahara.c:850". So it is
 * answered as theirs does: HELLO_RESP, the table read, each region read in 4 KB pieces and COUNTED AND THROWN AWAY
 * (an EFS holds the IMEI: nothing of it is said or kept, nothing written anywhere), then RESET.
 */
#define EFS_TO_DEVICE           10
#define EFS_FROM_DEVICE         11
#define EFS_EVENT_RING          1               /* (step 4's known channels: 10/11 on event ring 1) */
#define EFS_ELEMENTS            16
#define EFS_SLOT                0x2000
#define EFS_QUEUE               8
#define EFS_PIECE               0x1000u

static struct {
	unsigned head, tail, dropped;
	uint32_t length[EFS_QUEUE];
	uint8_t packet[EFS_QUEUE][EFS_SLOT];
} efsq;

enum { EFS_IDLE, EFS_TABLE, EFS_REGION };
static struct {
	int state;
	bool is64, open;
	uint32_t want, got, table_length;
	uint8_t table[1024];
	uint64_t base[16], length[16], done, piece, bytes;
	unsigned entries, entry, sessions, regions, said;
} efs;

static void
efs_received(__unused void *context, __unused unsigned channel, const uint8_t *bytes, uint32_t length, __unused bool end)
{
	unsigned slot = efsq.tail % EFS_QUEUE;

	if (efsq.tail - efsq.head == EFS_QUEUE || length > EFS_SLOT) {
		efsq.dropped++;
		return;
	}
	memcpy(efsq.packet[slot], bytes, length);
	efsq.length[slot] = length;
	efsq.tail++;
}

static int
efs_send(const uint32_t *words, uint32_t length)
{
	uint8_t b[0x30];

	for (uint32_t i = 0; i < length / 4; i++) {
		qrtr_put32(b + 4 * i, words[i]);
	}
	return alioth_mhi_send(&mhi, EFS_TO_DEVICE, b, length, true, 2000);
}

static int
efs_read(uint64_t address, uint64_t length)
{
	if (efs.is64) {
		const uint32_t w[6] = { 0x11, 0x20, (uint32_t)address, (uint32_t)(address >> 32), (uint32_t)length,
			                (uint32_t)(length >> 32) };   /* MEM_READ64 */
		uint32_t padded[8] = { w[0], w[1], w[2], w[3], w[4], w[5], 0, 0 };

		return efs_send(padded, 0x20);
	} else {
		const uint32_t w[4] = { 0x0a, 0x10, (uint32_t)address, (uint32_t)length };   /* MEM_READ */

		return efs_send(w, 0x10);
	}
}

/* the next piece of the region being read, or the next region, or the end of the session */
static int
efs_next(void)
{
	while (efs.entry < efs.entries) {
		uint64_t left = efs.length[efs.entry] - efs.done;

		if (left == 0) {
			efs.entry++;
			efs.done = 0;
			continue;
		}
		efs.piece = left > EFS_PIECE ? EFS_PIECE : left;
		efs.want = (uint32_t)efs.piece;
		efs.got = 0;
		efs.state = EFS_REGION;
		return efs_read(efs.base[efs.entry] + efs.done, efs.piece);
	}
	{
		const uint32_t reset[2] = { 0x07, 0x08 };     /* RESET */

		say("mdm: 6: efs: memory debug %u done: %u regions, %llu bytes read and thrown away\n", efs.sessions, efs.regions,
		    efs.bytes);
		efs.state = EFS_IDLE;
		return efs_send(reset, 8);
	}
}

static int
efs_packet(const uint8_t *p, uint32_t length)
{
	uint32_t cmd;

	if (efs.state == EFS_TABLE || efs.state == EFS_REGION) {
		uint32_t take = length < efs.want - efs.got ? length : efs.want - efs.got;

		if (efs.state == EFS_TABLE && efs.got < sizeof(efs.table)) {
			memcpy(efs.table + efs.got, p, take < sizeof(efs.table) - efs.got ? take : sizeof(efs.table) - efs.got);
		}
		efs.got += take;
		if (efs.state == EFS_REGION) {
			efs.bytes += take;
		}
		if (efs.got < efs.want) {
			return 0;
		}
		if (efs.state == EFS_TABLE) {
			uint32_t entry_size = efs.is64 ? 64 : 52;

			efs.entries = efs.table_length / entry_size;
			efs.entries = efs.entries > 16 ? 16 : efs.entries;
			for (unsigned i = 0; i < efs.entries; i++) {
				const uint8_t *e = efs.table + i * entry_size;

				if (efs.is64) {
					efs.base[i] = (uint64_t)loc_le32(e + 8) | (uint64_t)loc_le32(e + 12) << 32;
					efs.length[i] = (uint64_t)loc_le32(e + 16) | (uint64_t)loc_le32(e + 20) << 32;
				} else {
					efs.base[i] = loc_le32(e + 4);
					efs.length[i] = loc_le32(e + 8);
				}
				say("mdm: 6: efs: region %u: %llu bytes\n", i, efs.length[i]);
			}
			efs.regions = efs.entries;
			efs.entry = 0;
			efs.done = 0;
		} else {
			efs.done += efs.piece;
		}
		return efs_next();
	}
	if (length < 8) {
		return 0;
	}
	cmd = loc_le32(p);
	if (efs.said++ < 20) {
		say("mdm: 6: efs: Sahara command %#x, %u bytes\n", cmd, length);
	}
	switch (cmd) {
	case 0x01: {                                    /* HELLO: answered with its own mode */
		const uint32_t reply[12] = { 0x02, 0x30, 2, 1, 0, length >= 24 ? loc_le32(p + 20) : 0, 0, 0, 0, 0, 0, 0 };

		return efs_send(reply, 0x30);
	}
	case 0x09:                                      /* MEM_DEBUG: table address, length */
	case 0x10: {                                    /* MEM_DEBUG64 */
		uint64_t address, table_length;

		efs.is64 = cmd == 0x10;
		if (length < (efs.is64 ? 24u : 16u)) {
			return 0;
		}
		address = efs.is64 ? ((uint64_t)loc_le32(p + 12) << 32 | loc_le32(p + 8)) : loc_le32(p + 8);
		table_length = efs.is64 ? ((uint64_t)loc_le32(p + 20) << 32 | loc_le32(p + 16)) : loc_le32(p + 12);
		efs.sessions++;
		efs.table_length = table_length > sizeof(efs.table) ? (uint32_t)sizeof(efs.table) : (uint32_t)table_length;
		efs.want = efs.table_length;
		efs.got = 0;
		efs.bytes = 0;
		efs.state = EFS_TABLE;
		say("mdm: 6: efs: memory debug %u (%s): a table of %llu bytes\n", efs.sessions, efs.is64 ? "64-bit" : "32-bit",
		    table_length);
		return efs_read(address, efs.table_length);
	}
	case 0x08:                                      /* RESET_RESP */
		say("mdm: 6: efs: reset answered\n");
		return 0;
	default:
		return 0;
	}
}

static bool
step_qrtr(void)
{
	uint8_t hello[64];
	unsigned locations = 0;
	int error;

	say("mdm: 6: QRTR on the IPCR channels: our HELLO, then what the modem tells of its services\n");
	memset(&ipcr, 0, sizeof(ipcr));
	server_count = 0;
	{
		uint32_t want = 0;

		loc.wanted = PE_parse_boot_argn("mdm_loc", &want, sizeof(want)) && want != 0;
		say("mdm: 7: loc: %s\n", loc.wanted ? "a GNSS session once LOC is there (mdm_loc=1), listened to for 240 s" :
		    "not asked for (mdm_loc=1 starts a GNSS session)");
	}
	error = alioth_mhi_open(&mhi, IPCR_TO_DEVICE, ALIOTH_MHI_TO_DEVICE, IPCR_EVENT_RING, IPCR_ELEMENTS, IPCR_SLOT, NULL, NULL);
	if (error == 0) {
		error = alioth_mhi_open(&mhi, IPCR_FROM_DEVICE, ALIOTH_MHI_FROM_DEVICE, IPCR_EVENT_RING, IPCR_ELEMENTS, IPCR_SLOT,
		    ipcr_received, NULL);
	}
	if (error == 0) {
		error = ipcr_send(hello, qrtr_make_control(hello, sizeof(hello), QRTR_TYPE_HELLO, QRTR_OUR_NODE, QRTR_NODE_BCAST, NULL));
	}
	if (error == 0) {
		error = tftp_announce();
	}
	if (error == 0) {
		/* the EFS channels for the modem's remote file system (see efs_packet); not fatal if they do not open */
		int e;

		memset(&efsq, 0, sizeof(efsq));
		e = alioth_mhi_open(&mhi, EFS_TO_DEVICE, ALIOTH_MHI_TO_DEVICE, EFS_EVENT_RING, EFS_ELEMENTS, EFS_SLOT, NULL, NULL);
		if (e == 0) {
			e = alioth_mhi_open(&mhi, EFS_FROM_DEVICE, ALIOTH_MHI_FROM_DEVICE, EFS_EVENT_RING, EFS_ELEMENTS, EFS_SLOT,
			    efs_received, NULL);
		}
		efs.open = e == 0;
		say("mdm: 6: efs: channels 10/11 %s (%d)\n", efs.open ? "open" : "DID NOT OPEN", e);
	}
	/*
	 * (2026-11-13) Listened to for IPCR_LISTEN_MS by the clock, with a line every second: the first listen on the phone
	 * went silent for good after the modem's second HELLO (15 services in), with no line to say where it was.
	 */
	{
		uint64_t start = since_reset_ms(), beat = start + 1000, hello_at = start + 5000, now;
		uint64_t listen_ms = loc.wanted ? 240000 : IPCR_LISTEN_MS;      /* (a GNSS cold start with no help: minutes) */
		unsigned handled = 0;

		while (error == 0 && (now = since_reset_ms()) - start < listen_ms) {
			/* (a line a second for the first 20 s, then every 10 s: the kept lines have room for the GNSS session) */
			if (now >= beat) {
				say("mdm: 6: listening, %llu ms after the reset: %u packets handled, %u services, %u dropped, %u partial bytes, "
				    "%u events in all; tftp %u reads, %u writes, %u blocks; loc step %u, %u reports, %u fixes, best %u m, "
				    "%u satellites at most\n", now, handled, server_count, ipcr.dropped, ipcr.partial, mhi.events, tftp_reads,
				    tftp_writes, tftp_blocks, loc.step, loc.reports, loc.fixes, loc.best_m, loc.most_sats);
				beat += now - start < 20000 ? 1000 : 10000;
			}
			if (error == 0) {
				error = loc_tick();
			}
			/* (our HELLO again every 5 s: does the router's answer -- its list of services -- grow?) */
			if (now >= hello_at) {
				error = ipcr_send(hello, qrtr_make_control(hello, sizeof(hello), QRTR_TYPE_HELLO, QRTR_OUR_NODE,
				    QRTR_NODE_BCAST, NULL));
				hello_at += 5000;
			}
			error = alioth_mhi_poll(&mhi);
			while (error == 0 && ipcr.head != ipcr.tail) {
				unsigned slot = ipcr.head++ % IPCR_QUEUE;

				error = ipcr_handle(ipcr.packet[slot], ipcr.length[slot]);
				handled++;
			}
			while (error == 0 && efsq.head != efsq.tail) {
				unsigned slot = efsq.head++ % EFS_QUEUE;

				error = efs_packet(efsq.packet[slot], efsq.length[slot]);
			}
			delay(1000);
		}
		if (error != 0) {
			say("mdm: 6: the listen stopped on an error (%d) after %llu ms, %u packets handled\n", error,
			    since_reset_ms() - start, handled);
		}
	}
	for (unsigned i = 0; i < server_count; i++) {
		locations += servers[i].service == QRTR_SERVICE_LOC;
	}
	say("mdm: 6: %s (%d): %u services of the modem's; location (QMI LOC, 0x10) %s; %u packets dropped; tftp: %u reads "
	    "(all \"not found\"), %u writes, %u blocks, %llu bytes thrown away\n",
	    error ? "QRTR STOPPED" : "listened", error, server_count, locations ? "IS THERE" : "not seen", ipcr.dropped,
	    tftp_reads, tftp_writes, tftp_blocks, tftp_written);
	say("mdm: 6: efs: %u memory debug sessions, %u regions, %llu bytes thrown away, %u packets dropped, state %d\n",
	    efs.sessions, efs.regions, efs.bytes, efsq.dropped, efs.state);
	if (loc.wanted) {
		say("mdm: 7: loc: %s: step %u, %u responses, %u position reports (last status %u), %u FIXES, best accuracy %u m, "
		    "%u satellites at most\n", loc.fixes ? "THE MODEM'S GNSS GAVE A POSITION" : "no position", loc.step,
		    loc.responses, loc.reports, loc.last_status, loc.fixes, loc.best_m, loc.most_sats);
	}
	return error == 0 && server_count > 0;
}

/* On the way back to the loader (osfmk/arm64/loader_return.c), the other cores stopped: the modem held in reset,
 * the SMMU as it was found. */
void alioth_mdm_quiesce(void);

void
alioth_mdm_quiesce(void)
{
	if (dma_on || smmu_match != ~0u) {
		mdm_stop(true);
	}
	if (mdm_kept_len != 0) {
		for (const char *c = "mdm: what happened in this run, again (the log's middle is not kept):\n"; *c != 0; c++) {
			loader_log_putc(*c);
		}
		for (uint32_t i = 0; i < mdm_kept_len; i++) {
			if (i == 0 || mdm_kept[i - 1] == '\n') {
				loader_log_putc('>');
				loader_log_putc(' ');
			}
			loader_log_putc(mdm_kept[i]);
		}
		if (mdm_kept[mdm_kept_len - 1] != '\n') {
			loader_log_putc('\n');
		}
		if (mdm_kept_full) {
			for (const char *c = "> (and more: the kept lines filled their 48 KB)\n"; *c != 0; c++) {
				loader_log_putc(*c);
			}
		}
	}
}

/* ---- the steps ------------------------------------------------------------------------------------------ */

static void
mdm_thread(__unused void *arg, __unused wait_result_t wr_result)
{
	bool ok = true;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SIZE);
	parf = ml_io_map(PARF_PHYS, 0x3000);
	phy = ml_io_map(PHY_PHYS, 0x2000);
	dbi = ml_io_map(DBI_PHYS, 0x1000);
	atu = ml_io_map(ATU_PHYS, 0x1000);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	bar = ml_io_map(BAR_PHYS, 0x100000);
	arb_core = ml_io_map(ARB_CORE_PHYS, 0x1100);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, 0x1000);
	arb_obs = ml_io_map(ARB_OBS_PHYS, 0x10000);
	if (!gcc || !tlmm || !parf || !phy || !dbi || !atu || !conf || !bar || !arb_core || !arb_cnfg || !arb_obs) {
		say("mdm: the registers could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	ok = step_pon_find();
	if (ok && mdm_step >= 4) {
		ok = images_ready();
	}
	if (ok && mdm_step >= 2) {
		ok = step_power();
	}
	if (ok && mdm_step >= 3) {
		ok = step_pcie2();
	}
	if (ok && mdm_step >= 4) {
		ok = step_mhi();
	}
	if (ok && mdm_step >= 5) {
		ok = step_sahara();
	}
	if (ok && mdm_step >= 6) {
		ok = step_qrtr();
	}
	if (mdm_step >= 3 && (!ok || mdm_step < 5 || !mdm_keep)) {
		mdm_stop(false);                                /* (held in reset again; a modem in mission mode stays) */
	}
	say("mdm: steps 1..%u %s\n", mdm_step, ok ? "done" : "STOPPED (see above)");
	thread_terminate(current_thread());
}

/* Called while the system starts (bsd/dev/hakios.input.c), with the other device's drivers. */
void alioth_mdm_init(void);

void
alioth_mdm_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mdm_step", &mdm_step, sizeof(mdm_step)) || mdm_step == 0) {
		return;
	}
	if (mdm_step > 6) {
		mdm_step = 6;
	}
	PE_parse_boot_argn("mdm_keep", &mdm_keep, sizeof(mdm_keep));
	if (kernel_thread_start(mdm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
