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
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern lck_mtx_t hakios_smmu_lock;      /* (bsd/dev/hakios.wifi.c: the SMMU's global registers are shared) */
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
static uint32_t mdm_step, mdm_keep = 1, bar_size;

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
#define MDM_STREAM              0x1d01u         /* kona-pcie.dtsi pcie2: iommu-map <0x100 &apps_smmu 0x1d01 1> */
#define MDM_WINDOW              0x20000000ull   /* kona-mhi.dtsi: qcom,iommu-dma-addr-pool <0x20000000 ...> */
#define MDM_WINDOW_SIZE         (64ull << 20)
#define MDM_THIRDS              ((uint32_t)(MDM_WINDOW_SIZE >> 21))

static vm_offset_t smmu, smmu_second, smmu_context, smmu_contexts[128], tables;
static uint32_t smmu_match = ~0u, smmu_context_n, smmu_old_smr, smmu_old_s2cr;
static bool smmu_on, dma_on, dma_cached;
static uint64_t window_next;

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
	if (!smmu_on || window_next + size > MDM_WINDOW_SIZE) {
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
	window_next += size;
	return 0;
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

static bool
step_mhi(void)
{
	static const struct alioth_mhi_known known[] = {
		{ 2, ALIOTH_MHI_TO_DEVICE, 1 }, { 3, ALIOTH_MHI_FROM_DEVICE, 1 }, { 25, ALIOTH_MHI_FROM_DEVICE, 1 },
		{ 10, ALIOTH_MHI_TO_DEVICE, 1 }, { 11, ALIOTH_MHI_FROM_DEVICE, 1 },
		{ 20, ALIOTH_MHI_TO_DEVICE, 2 }, { 21, ALIOTH_MHI_FROM_DEVICE, 2 },
	};
	const uint8_t *sbl;
	uint64_t size = 0;
	uint32_t ee = ~0u;
	int error;

	say("mdm: 4: the SMMU, bus mastering, MHI: the SBL through BHI, READY, M0\n");
	if (!config_load()) {
		return false;
	}
	if ((sbl = image_named(bhi_name, &size)) == NULL) {
		say("mdm: 4: no fw/sdx55m/%s\n", bhi_name);
		return false;
	}
	{
		/* (the file as the kernel has it, FNV-1a: sbl1.mbn of ~/hk/fw-modem is 548056 bytes, 0xa4dfb904) */
		uint32_t hash = 2166136261u;

		for (uint64_t i = 0; i < size; i++) {
			hash = (hash ^ sbl[i]) * 16777619u;
		}
		say("mdm: 4: %s: %llu bytes, FNV-1a %#x, first bytes %02x %02x %02x %02x\n", bhi_name, size, hash, sbl[0], sbl[1],
		    sbl[2], sbl[3]);
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
		error = alioth_mhi_bhi(&mhi, sbl, (size_t)size, 2000);
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
#define IPCR_LISTEN_MS          10000
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
		return 0;
	case QRTR_TYPE_NEW_SERVER:
	case QRTR_TYPE_DEL_SERVER:
		if (!qrtr_server_of(&p, &s)) {
			return 0;
		}
		if (s.service == 0 && s.node == 0 && s.port == 0) {
			say("mdm: 6: (the end of a list of servers)\n");
			return 0;
		}
		say("mdm: 6: %s service %u (%#x) version %u instance %u at node %u port %u%s\n",
		    p.type == QRTR_TYPE_NEW_SERVER ? "NEW" : "GONE:", s.service, s.service, s.version, s.instance, s.node, s.port,
		    s.service == QRTR_SERVICE_LOC ? ": LOCATION (QMI LOC)" : "");
		if (p.type == QRTR_TYPE_NEW_SERVER && server_count < MDM_SERVERS) {
			servers[server_count++] = s;
		}
		return 0;
	case QRTR_TYPE_NEW_LOOKUP:
		/* we serve nothing yet: the list's end straight away, to the client that asked (Linux qrtr-ns's answer) */
		say("mdm: 6: node %u port %u looks for service %u instance %#x: we have none\n", p.src_node, p.src_port,
		    p.size >= QRTR_CONTROL_SIZE ? qrtr_le32(p.payload + 4) : 0, p.size >= QRTR_CONTROL_SIZE ? qrtr_le32(p.payload + 8) : 0);
		return ipcr_send(out, qrtr_make(out, sizeof(out), QRTR_TYPE_NEW_SERVER, QRTR_OUR_NODE, QRTR_PORT_CTRL, p.src_node,
		    p.src_port, (const uint8_t[QRTR_CONTROL_SIZE]){ QRTR_TYPE_NEW_SERVER }, QRTR_CONTROL_SIZE, false));
	case QRTR_TYPE_DATA:
		say("mdm: 6: data from %u:%u to port %#x, %u bytes (nobody listens there yet)\n", p.src_node, p.src_port, p.dst_port,
		    p.size);
		return 0;
	default:
		say("mdm: 6: control %u from %u:%u (%u bytes)\n", p.type, p.src_node, p.src_port, p.size);
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
	error = alioth_mhi_open(&mhi, IPCR_TO_DEVICE, ALIOTH_MHI_TO_DEVICE, IPCR_EVENT_RING, IPCR_ELEMENTS, IPCR_SLOT, NULL, NULL);
	if (error == 0) {
		error = alioth_mhi_open(&mhi, IPCR_FROM_DEVICE, ALIOTH_MHI_FROM_DEVICE, IPCR_EVENT_RING, IPCR_ELEMENTS, IPCR_SLOT,
		    ipcr_received, NULL);
	}
	if (error == 0) {
		error = ipcr_send(hello, qrtr_make_control(hello, sizeof(hello), QRTR_TYPE_HELLO, QRTR_OUR_NODE, QRTR_NODE_BCAST, NULL));
	}
	for (unsigned ms = 0; error == 0 && ms < IPCR_LISTEN_MS; ms++) {
		error = alioth_mhi_poll(&mhi);
		while (error == 0 && ipcr.head != ipcr.tail) {
			unsigned slot = ipcr.head++ % IPCR_QUEUE;

			error = ipcr_handle(ipcr.packet[slot], ipcr.length[slot]);
		}
		delay(1000);
	}
	for (unsigned i = 0; i < server_count; i++) {
		locations += servers[i].service == QRTR_SERVICE_LOC;
	}
	say("mdm: 6: %s (%d): %u services of the modem's; location (QMI LOC, 0x10) %s; %u packets dropped\n",
	    error ? "QRTR STOPPED" : "listened", error, server_count, locations ? "IS THERE" : "not seen", ipcr.dropped);
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
