/*
 * The Redmi K40's modem: an SDX55 outside the SoC, on PCIe2, whose GNSS receiver is the phone's GPS
 * (work/research/gps-sdx55.md, from Xiaomi's device tree and kernel for alioth). The first steps towards it,
 * a diagnostic: only with boot-arg mdm_step=N, N = 1 .. 3; nothing here runs otherwise.
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
 *              SDX55 (17cb:0306) and reads its MHI registers -- the version, BHI, the execution environment.
 *              Bus mastering stays off: nothing of the modem's reaches memory.
 * At the end PERST (GPIO 85) is held asserted; the modem stays powered (the next warm reset restarts it).
 *
 * The SDX55's pins are all in the TLMM's SOUTH tile (Linux pinctrl-sm8250.c), the Wi-Fi's in the NORTH one.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include "alioth_pcie2_phy.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern int umi_rpmh_wifi_power(void);
extern kern_return_t thread_terminate(thread_t thread);
#define say hakios_tl

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
static uint32_t mdm_step;

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
	say("mdm: 2: the PMX55's warm reset, then AP2MDM_STATUS high (Xiaomi's sdx55m first power-on)\n");
	if (!pon_put(PON_SW_RST_S2_CTL2, 0)) {
		return false;
	}
	delay(500);
	if (!pon_put(PON_SW_RST_S2_CTL, PON_WARM_RESET) || !pon_put(PON_SW_RST_S2_CTL2, PON_RESET_EN)) {
		return false;
	}
	delay(500);
	if (!pon_put(PON_SW_RST_GO, PON_GO)) {
		return false;
	}
	delay(150 * 1000);
	/* (inputs: function 0, no pull -- the modem drives them) */
	update(tlmm + GPIO_MDM2AP_STATUS * 0x1000, 0x3ff, 0);
	update(tlmm + GPIO_MDM2AP_ERRFATAL * 0x1000, 0x3ff, 0);
	gpio_out(GPIO_AP2MDM_ERRFATAL, false, 0);
	gpio_out(GPIO_AP2MDM_STATUS, true, 0);
	for (unsigned ms = 0; ms <= 2000; ms += 250) {
		say("mdm: 2: %u ms: MDM2AP_STATUS %d, MDM2AP_ERRFATAL %d\n", ms, gpio_in(GPIO_MDM2AP_STATUS),
		    gpio_in(GPIO_MDM2AP_ERRFATAL));
		delay(250 * 1000);
	}
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
	update(dbi + 0xa0, 0xf, 2);                             /* Gen2 to begin with (the SDX55 can do Gen3 x2) */
	wr(dbi + 0x78, (rd(dbi + 0x78) & 0xffff) & ~0x7000u);   /* (the control half only; the upper half is W1C) */
	update(parf + 0x1b0, 0, BIT(8));                        /* LTSSM on */
	if (!wait_bits(dbi + 0xf28, BIT(10), BIT(10), 100000, "the link") ||
	    !wait_bits(dbi + 0x80, BIT(29), BIT(29), 100000, "the data link layer")) {
		say("mdm: 3: link status %#x, LTSSM %#x\n", rd(dbi + 0x80), rd(parf + 0x24));
		return false;
	}
	say("mdm: 3: THE LINK IS UP: status %#x (Gen%u x%u)\n", rd(dbi + 0x80), (rd(dbi + 0x80) >> 16) & 0xf,
	    (rd(dbi + 0x80) >> 20) & 0x3f);
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
	wr(conf + 0x10, (uint32_t)BAR_PHYS | (bar0 & 15));
	if ((bar0 & 6) == 4) {
		wr(conf + 0x14, 0);
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
	if (ok && mdm_step >= 2) {
		ok = step_power();
	}
	if (ok && mdm_step >= 3) {
		ok = step_pcie2();
		gpio_out(GPIO_PERST, false, 1);                 /* (held in reset again: no MHI here yet) */
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
	if (mdm_step > 3) {
		mdm_step = 3;
	}
	if (kernel_thread_start(mdm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
