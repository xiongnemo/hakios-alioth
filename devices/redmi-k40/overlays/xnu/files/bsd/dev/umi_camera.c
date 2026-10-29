/* UMI main-camera bring-up. cam=2 identifies; cam=3 also programs mode 3.
 * cam=4 verifies ISP reset; cam=6 also tests hardware TPG capture.
 * These diagnostics never start a sensor stream.
 * Board wiring and sequencing come from the live UMI DT, V2 sensor module,
 * and Android rear-1x GPIO/regulator/SPMI traces. The PM8150L GPIO sequence
 * below is the traced sequence, including its input/output mode and pull.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern int umi_rpmh_camera_vreg(unsigned, bool);
extern uint64_t umi_camera_iomap_free(void);
int umi_camera_probe(void);
int umi_camera_prepare(void);
int umi_camera_isp_probe(void);
int umi_camera_capture_tpg(void);
extern int umi_camera_tpg(vm_offset_t, vm_offset_t);
extern void umi_camera_dma_quiesce(bool);
extern const uint8_t *hakios_wifi_file(const char *, uint64_t *);
void umi_camera_probe_quiesce(void);

static struct {
	vm_offset_t core, config, observer, cc, gcc, pin[4];
	uint32_t pin_cfg[4], pin_io[4], clock_cfg, clock_branch, gdsc;
	uint32_t gcc_ahb, gcc_xo;
	unsigned pins_valid, pin_io_valid;
	bool active, panic, reset_changed, clock_changed, gdsc_changed, gcc_saved, rails[3];
} uc;
static struct {
	uint16_t ppid, channel;
	vm_offset_t write;
	uint8_t saved[8];
	bool changed;
} gates[3] = { { .ppid = 0x4c2 }, { .ppid = 0x4c0 }, { .ppid = 0x4c3 } };
static const uint8_t gate_regs[8] = { 0x41, 0x42, 0x45, 0x43, 0x40, 0x4a, 0x44, 0x46 };
static const uint8_t gate_on[8] = { 0, 4, 1, 0, 2, 0, 0x80, 0x80 };

static vm_offset_t
umap(vm_offset_t physical, vm_size_t size)
{
	uint64_t available = umi_camera_iomap_free();
	uint64_t required = (size + (physical & 0x3fffUL) + 0x3fffUL) & ~0x3fffUL;
	hakios_tl("UMI camera: mapping %#lx + %#lx, IO window free=%llu\n",
	    (unsigned long)physical, (unsigned long)size, available);
	if (required > available) return 0;
	vm_offset_t mapped = ml_io_map(physical, size);
#if defined(__aarch64__)
	__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
	hakios_tl("UMI camera: mapping %#lx complete\n", (unsigned long)physical);
	return mapped;
}

static uint32_t
ur(vm_offset_t p, unsigned o)
{
	uint32_t v = *(volatile uint32_t *)(p + o);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
uw(vm_offset_t p, unsigned o, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(p + o) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
ums(unsigned n)
{
	if (uc.panic) delay(n * 1000);
	else delay_for_interval(n, NSEC_PER_MSEC);
}

static bool
upoll(vm_offset_t p, unsigned o, uint32_t mask, uint32_t value, unsigned us)
{
	for (unsigned i = 0; i < us; ++i) {
		if ((ur(p, o) & mask) == value) return true;
		delay(1);
	}
	return false;
}

/* The observer channel can read a peripheral regardless of writer ownership.
 * Writes require EE0 ownership and a still-matching PPID on every transfer.
 */
static bool
ugpio_io(unsigned g, uint8_t reg, uint8_t *byte, bool write)
{
	if (g >= 3 || !gates[g].write ||
	    ((ur(uc.core, 0x900 + 4 * gates[g].channel) >> 8) & 0xfff) != gates[g].ppid ||
	    (ur(uc.config, 0x700 + 4 * gates[g].channel) & 7) != 0) return false;
	vm_offset_t channel = write ? gates[g].write : uc.observer + 0x80UL * gates[g].channel;
	if (!uc.panic) hakios_spmi_lock();
	if (write) uw(channel, 0x10, *byte);
	uw(channel, 0, (write ? 0 : 1u << 27) | (uint32_t)reg << 4);
	bool done = upoll(channel, 8, 1, 1, 2000);
	uint32_t status = ur(channel, 8);
	if (!write && done && !(status & 14)) *byte = (uint8_t)ur(channel, 0x18);
	if (!uc.panic) hakios_spmi_unlock();
	if (!done || (status & 14)) {
		hakios_tl("UMI camera: GPIO PPID %#x register %#x write=%u status=%#x\n",
		    gates[g].ppid, reg, write, status);
		return false;
	}
	return true;
}

static bool
ugpio_put(unsigned g, uint8_t reg, uint8_t value)
{
	bool allowed = false;
	for (unsigned i = 0; i < 8; ++i) allowed |= reg == gate_regs[i];
	uint8_t back = 0;
	if (!allowed || !ugpio_io(g, reg, &value, true) || !ugpio_io(g, reg, &back, false)) return false;
	if (back != value) hakios_tl("UMI camera: PPID %#x register %#x wrote %#x read %#x\n", gates[g].ppid, reg, value, back);
	return back == value;
}

static int
uvreg(unsigned rail, bool enable)
{
	int error = EBUSY;
	for (unsigned tries = 0; tries < 50; ++tries) {
		error = umi_rpmh_camera_vreg(rail, enable);
		if (error != EBUSY || uc.panic) return error;
		ums(5);
	}
	return error;
}

static bool
ugpio_prepare(void)
{
	uint32_t version = ur(uc.core, 0), count = ur(uc.core, 4) & 0x7ff;
	hakios_tl("UMI camera: PMIC arbiter version=%#x channels=%u\n", version, count);
	if ((version & 0xf0000000u) != 0x50000000u || count > 512) return false;
	if (!count) count = 512; /* UMI v5 table size, as used by the torch/key clients. */
	for (unsigned g = 0; g < 3; ++g) {
		bool found = false;
		for (unsigned c = 0; c < count; ++c) {
			if (((ur(uc.core, 0x900 + 4 * c) >> 8) & 0xfff) != gates[g].ppid ||
			    (ur(uc.config, 0x700 + 4 * c) & 7) != 0) continue;
			if (found) return false;
			gates[g].channel = (uint16_t)c;
			gates[g].write = umap(0x0c600000UL + 0x10000UL * c, 0x1000);
			found = gates[g].write != 0;
		}
		uint8_t type = 0, subtype = 0;
		if (!found || !ugpio_io(g, 4, &type, false) || !ugpio_io(g, 5, &subtype, false) ||
		    type != 0x10 || (subtype != 0x10 && subtype != 0x11)) return false;
		for (unsigned r = 0; r < 8; ++r)
			if (!ugpio_io(g, gate_regs[r], &gates[g].saved[r], false)) return false;
		if ((gates[g].saved[6] & 0x80) && (gates[g].saved[7] & 0x80)) {
			hakios_tl("UMI camera: PPID %#x already drives high; refusing to take an active supply\n", gates[g].ppid);
			return false;
		}
	}
	return true;
}

static bool
ugpio_on(unsigned g)
{
	gates[g].changed = true;
	for (unsigned r = 0; r < 8; ++r)
		if (!ugpio_put(g, gate_regs[r], gate_on[r])) return false;
	delay(135); /* DT regulator-enable-ramp-delay */
	return true;
}

static void
upin(unsigned p, bool high)
{
	uint32_t config = uc.pin_cfg[p] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12; /* Kona extended-GPIO ownership */
	uw(uc.pin[p], 4, high ? 2 : 0);
	uw(uc.pin[p], 0, config | 1u << 9); /* reset: GPIO output, no pull, 2 mA */
}

/* Open drain bit-banging on the actual CCI0 master1 pins. No CCI MMIO or
 * bus recovery clocks are used. Every clock-high wait has a 1 ms bound.
 */
static bool
uline(unsigned p, bool high)
{
	uint32_t config = uc.pin_cfg[p] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12;
	uw(uc.pin[p], 4, 0);
	uw(uc.pin[p], 0, config | 3u | (high ? 0 : 1u << 9));
	if (p == 3 && high && !upoll(uc.pin[p], 4, 1, 1, 1000)) return false;
	delay(5);
	return true;
}

static bool
ustart(void)
{
	return uline(2, true) && uline(3, true) && (ur(uc.pin[2], 4) & 1) && uline(2, false) && uline(3, false);
}

static void
ustop(void)
{
	(void)uline(2, false); (void)uline(3, true); (void)uline(2, true);
}

static bool
uput(uint8_t byte)
{
	for (unsigned b = 0; b < 8; ++b) {
		if (!uline(2, (byte & 0x80) != 0) || !uline(3, true) || !uline(3, false)) return false;
		byte <<= 1;
	}
	if (!uline(2, true) || !uline(3, true)) return false;
	bool ack = !(ur(uc.pin[2], 4) & 1);
	(void)uline(3, false);
	return ack;
}

static bool
uget(uint8_t *byte, bool ack)
{
	*byte = 0;
	if (!uline(2, true)) return false;
	for (unsigned b = 0; b < 8; ++b) {
		if (!uline(3, true)) return false;
		*byte = (uint8_t)((*byte << 1) | (ur(uc.pin[2], 4) & 1));
		if (!uline(3, false)) return false;
	}
	return uline(2, !ack) && uline(3, true) && uline(3, false) && uline(2, true);
}

static bool
usensor_read(uint16_t reg, uint16_t *value)
{
	uint8_t hi = 0, lo = 0;
	bool ok = ustart() && uput(0x7a) && uput((uint8_t)(reg >> 8)) && uput((uint8_t)reg) &&
	    ustart() && uput(0x7b) && uget(&hi, true) && uget(&lo, false);
	ustop();
	*value = (uint16_t)((uint16_t)hi << 8 | lo);
	return ok;
}

/* Validate the complete container before issuing any initialization writes.
 * The diagnostic uses only init/res3; the stream-on table is never applied.
 */
static uint32_t
utab_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool
usensor_write(uint16_t reg, uint16_t value)
{
	bool ok = ustart() && uput(0x7a) && uput(reg >> 8) && uput(reg) &&
	    uput(value >> 8) && uput(value);
	ustop();
	return ok;
}

static int
umode_prepare(unsigned version)
{
	if (version != 3 && version != 0x13) return ENODEV;
	uint64_t size = 0;
	const char *name = version == 3 ? "camera-s5khmx-v3.tab" : "camera-s5khmx-v19.tab";
	const uint8_t *file = hakios_wifi_file(name, &size);
	if (!file || size < 40 || size > 65536 || memcmp(file, "RWCTAB02", 8) ||
	    utab_u32(file+8) != 0x7a || utab_u32(file+12) != 2 || utab_u32(file+16) != 2 ||
	    utab_u32(file+20) != 0 || utab_u32(file+24) != 0x1ad0 ||
	    utab_u32(file+32) != 0x210 || utab_u32(file+36) != 7) return EINVAL;
	uint64_t offset = 40 + 7*48;
	if (offset > size) return EINVAL;
	const uint8_t *mode = file + 40 + 3*48;
	if (utab_u32(mode) != 3008 || utab_u32(mode+4) != 2256 ||
	    utab_u32(mode+16) != 3 || utab_u32(mode+20) != 1 || utab_u32(mode+44) != 0x2b) return EINVAL;
	for (unsigned p = 0; p < 2; ++p) {
		if (size-offset < 12) return EINVAL;
		uint64_t bytes = 12 + 12ull*utab_u32(file+offset+8);
		if (bytes > size-offset) return EINVAL;
		offset += bytes;
	}
	if (size-offset < 4) return EINVAL;
	unsigned tables = utab_u32(file+offset); offset += 4;
	if (!tables || tables > 32) return EINVAL;
	const uint8_t *selected[2] = { NULL, NULL };
	unsigned counts[2] = { 0, 0 };
	for (unsigned t = 0; t < tables; ++t) {
		if (size-offset < 12) return EINVAL;
		const uint8_t *table = file+offset;
		unsigned count = utab_u32(table+8);
		uint64_t bytes = 12 + 12ull*count;
		if (bytes > size-offset) return EINVAL;
		int slot = !memcmp(table, "init\0\0\0\0", 8) ? 0 :
		    !memcmp(table, "res3\0\0\0\0", 8) ? 1 : -1;
		if (slot >= 0) {
			if (selected[slot] || !count || count > 4096) return EINVAL;
			selected[slot] = table+12; counts[slot] = count;
			for (unsigned i = 0; i < count; ++i) {
				const uint8_t *e = table+12+12*i;
				if (e[4] != 2 || e[5] != 2 || e[6] || e[7] || utab_u32(e+8) > 100000 ||
				    (e[0] == 0 && e[1] == 1 && (e[2] || e[3]))) return EINVAL;
			}
		}
		offset += bytes;
	}
	if (offset != size || !selected[0] || !selected[1]) return EINVAL;
	hakios_tl("UMI camera: initialize version %#x mode 3 without stream-on (%u+%u writes)\n", version, counts[0], counts[1]);
	for (unsigned t = 0; t < 2; ++t)
		for (unsigned i = 0; i < counts[t]; ++i) {
			const uint8_t *e = selected[t]+12*i;
			uint16_t reg = (uint16_t)(e[0] | e[1] << 8), value = (uint16_t)(e[2] | e[3] << 8);
			if (!usensor_write(reg, value)) {
				hakios_tl("UMI camera: table %u entry %u register %#x NACK\n", t, i, reg);
				return EIO;
			}
			unsigned us = utab_u32(e+8);
			if (us >= 1000) ums(us/1000);
			if (us%1000) delay(us%1000);
		}
	/* Restore the indirect-write page before verifying direct output registers. */
	if (!usensor_write(0x6028, 0x4000)) return EIO;
	static const unsigned regs[5] = { 0x034c, 0x034e, 0x0342, 0x0340, 0x0100 };
	const unsigned expected[5] = { 3008, 2256, utab_u32(mode+32), utab_u32(mode+36), 0 };
	for (unsigned i = 0; i < 5; ++i) {
		uint16_t value = 0;
		if (!usensor_read(regs[i], &value)) return EIO;
		hakios_tl("UMI camera: mode register %#x=%#x expected %#x\n", regs[i], value, expected[i]);
		if (value != expected[i]) return EIO;
	}
	return 0;
}

static bool
uclock_on(void)
{
	uc.clock_changed = true;
	uw(uc.cc, 0x5020, (uc.clock_cfg & ~0x371fu) | 1); /* XO 19.2 MHz, divide by 1 */
	uw(uc.cc, 0x501c, ur(uc.cc, 0x501c) | 1);
	if (!upoll(uc.cc, 0x501c, 1, 0, 1000)) return false;
	uw(uc.cc, 0x5034, uc.clock_branch | 1);
	if (!upoll(uc.cc, 0x5034, 1u << 31, 0, 1000)) return false;
	uint32_t config = uc.pin_cfg[1] & ~0x3ffu;
	if (config & (1u << 11)) config |= 1u << 12;
	uw(uc.pin[1], 0, config | 1u << 2 | 1u << 6); /* MCLK1, function 1, 4 mA */
	return true;
}

/* Kona-v2 ISP diagnostic: establish MMIO/reset operation at XO frequency.
 * No PLL, stream, DMA master, or SMMU entry is enabled at this stage.
 * All affected clock branches must be unused before their parents change.
 */
static struct {
	unsigned cmd; uint32_t saved; bool changed;
} isp_rcgs[] = {
	{ .cmd = 0x7058 }, /* slow AHB */
	{ .cmd = 0xc0f8 }, /* CAMNOC AXI */
	{ .cmd = 0xa068 }, /* C-PHY RX */
	{ .cmd = 0x6060 }, /* PHY3 timer */
	{ .cmd = 0xc000 }, /* IFE-lite */
	{ .cmd = 0xc01c }, /* IFE-lite CSID */
};
static struct {
	unsigned reg; bool gcc, delay_only, changed;
} isp_branches[] = {
	{ .reg = 0xb02c, .gcc = true }, { .reg = 0xb030, .gcc = true },
	{ .reg = 0xc0f4 }, /* CPAS AHB */
	{ .reg = 0xc11c }, /* CAMNOC DCD XO */
	{ .reg = 0xc114 }, /* CAMNOC AXI */
	{ .reg = 0xc040 }, /* IFE-lite AHB */
	{ .reg = 0xc044 }, /* IFE-lite AXI */
	{ .reg = 0xc018 }, /* IFE-lite */
	{ .reg = 0xc034 }, /* IFE-lite CSID */
	{ .reg = 0xc03c }, /* IFE-lite C-PHY RX */
	{ .reg = 0x6078 }, /* PHY3 timer */
	{ .reg = 0x607c }, /* PHY3 */
};
static vm_offset_t isp_ife, isp_csid;
static bool isp_resetting;
static uint32_t isp_saved_ife_mask;

static void
uisp_down(void)
{
	umi_camera_dma_quiesce(uc.panic);
	if (isp_resetting) {
		/* Mask/clear only reset IRQs owned by this diagnostic. */
		uw(isp_ife, 0x28, 0); uw(isp_ife, 0x34, 1u << 17); uw(isp_ife, 0x24, 1);
		uw(isp_ife, 0x28, isp_saved_ife_mask);
		uw(isp_csid, 0x74, 0); uw(isp_csid, 0x78, 1); uw(isp_csid, 0x80, 1);
		isp_resetting = false;
	}
	for (unsigned n = sizeof(isp_branches)/sizeof(isp_branches[0]); n; --n) {
		unsigned i = n-1;
		if (!isp_branches[i].changed) continue;
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		uw(base, isp_branches[i].reg, ur(base, isp_branches[i].reg) & ~1u);
		isp_branches[i].changed = false;
	}
	for (unsigned n = sizeof(isp_rcgs)/sizeof(isp_rcgs[0]); n; --n) {
		unsigned i = n-1, cmd = isp_rcgs[i].cmd;
		if (!isp_rcgs[i].changed) continue;
		uw(uc.cc, cmd+4, isp_rcgs[i].saved);
		uw(uc.cc, cmd, ur(uc.cc, cmd) | 1);
		if (!upoll(uc.cc, cmd, 1, 0, 1000))
			hakios_tl("UMI camera: ISP clock %#x restore timed out\n", cmd);
		isp_rcgs[i].changed = false;
	}
}

static int
uisp_probe(void)
{
	/* Check every consumer before changing any source. This is not a way
	 * to take ownership of an ISP already used by another driver.
	 */
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		if (ur(base, isp_branches[i].reg) & 1) {
			hakios_tl("UMI camera: ISP branch %#x already enabled; refusing reparent\n", isp_branches[i].reg);
			return EBUSY;
		}
	}
	for (unsigned i = 0; i < sizeof(isp_rcgs)/sizeof(isp_rcgs[0]); ++i) {
		unsigned cmd = isp_rcgs[i].cmd;
		isp_rcgs[i].saved = ur(uc.cc, cmd+4);
		isp_rcgs[i].changed = true;
		uw(uc.cc, cmd+4, (isp_rcgs[i].saved & ~0x371fu) | 1); /* XO / 1 */
		uw(uc.cc, cmd, ur(uc.cc, cmd) | 1);
		if (!upoll(uc.cc, cmd, 1, 0, 1000)) return ETIMEDOUT;
	}
	/* Register access also needs IFE/CSID core clocks. Keep the entire
	 * external memory path (CAMNOC AXI and both GCC camera AXI branches)
	 * gated while inspecting inherited DMA/path enables.
	 */
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		if (isp_branches[i].gcc || isp_branches[i].reg == 0xc114) continue;
		vm_offset_t base = uc.cc;
		unsigned reg = isp_branches[i].reg;
		isp_branches[i].changed = true;
		uw(base, reg, ur(base, reg) | 1);
		if (isp_branches[i].delay_only) delay(10);
		if (!(ur(base, reg) & 1) || (!isp_branches[i].delay_only &&
		    !upoll(base, reg, 1u<<31, 0, 2000))) {
			hakios_tl("UMI camera: ISP branch %#x failed to start, value %#x\n", reg, ur(base, reg));
			return EIO;
		}
	}
	/* Physical regions overlap: map IFE once, derive CSID's 0x200 offset. */
	if (!isp_ife) isp_ife = umap(0x0acd9000, 0x2200);
	if (!isp_ife) return ENOMEM;
	isp_csid = isp_ife + 0x200;
	hakios_tl("UMI camera: reading IFE core version\n");
	uint32_t ife_version = ur(isp_ife, 0);
	hakios_tl("UMI camera: IFE core version %#x; reading local bus version\n", ife_version);
	uint32_t bus_version = ur(isp_ife, 0x1a00);
	hakios_tl("UMI camera: bus version %#x; reading CSID version\n", bus_version);
	uint32_t csid_version = ur(isp_csid, 0);
	hakios_tl("UMI camera: XO-clocked ISP IDs IFE=%#x bus=%#x CSID=%#x\n", ife_version, bus_version, csid_version);
	if (!ife_version || ife_version == ~0u || !bus_version || bus_version == ~0u ||
	    !csid_version || csid_version == ~0u) return ENODEV;
	/* An inherited reset-only mask is harmless and restored afterward.
	 * WM bit0 is enable; reset-default bit4 alone does not enable DMA. */
	hakios_tl("UMI camera: ISP inherited masks CSID top=%#x RX=%#x IFE=%#x/%#x/%#x\n",
	    ur(isp_csid, 0x74), ur(isp_csid, 0x24), ur(isp_ife, 0x28), ur(isp_ife, 0x2c), ur(isp_ife, 0x30));
	bool busy = ur(isp_csid, 0x74) || ur(isp_csid, 0x24) || (ur(isp_ife, 0x28) & ~(1u<<17)) ||
	    ur(isp_ife, 0x2c) || ur(isp_ife, 0x30);
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t cfg = ur(isp_csid, 0x200+0x100*i), mask = ur(isp_csid, 0x34+0x10*i), wm = ur(isp_ife, 0x1c00+0x100*i);
		hakios_tl("UMI camera: ISP inherited RDI%u cfg=%#x mask=%#x WM=%#x\n", i, cfg, mask, wm);
		busy |= (cfg & (1u<<31)) || mask || (wm & 1);
	}
	if (busy) return EBUSY;
	isp_saved_ife_mask = ur(isp_ife, 0x28);
	for (unsigned i = 0; i < sizeof(isp_branches)/sizeof(isp_branches[0]); ++i) {
		if (isp_branches[i].changed) continue;
		vm_offset_t base = isp_branches[i].gcc ? uc.gcc : uc.cc;
		unsigned reg = isp_branches[i].reg;
		isp_branches[i].changed = true;
		uw(base, reg, ur(base, reg) | 1);
		if (isp_branches[i].delay_only) delay(10);
		if (!(ur(base, reg) & 1) || (!isp_branches[i].delay_only &&
		    !upoll(base, reg, 1u<<31, 0, 2000))) {
			hakios_tl("UMI camera: ISP branch %#x failed to start, value %#x\n", reg, ur(base, reg));
			return EIO;
		}
	}
	isp_resetting = true;
	uw(isp_csid, 0x28, 0x0fffffff);
	for (unsigned i = 0; i < 4; ++i) uw(isp_csid, 0x38+0x10*i, 0x7fff);
	uw(isp_csid, 0x104, 0x80);
	for (unsigned i = 0; i < 4; ++i) uw(isp_csid, 0x200+0x100*i, 2);
	/* The SM8250 driver's global reset performs hardware, then registers. */
	for (unsigned step = 0; step < 2; ++step) {
		bool hw = step == 0;
		uw(isp_csid, 0x78, 1); uw(isp_csid, 0x80, 1);
		if (!upoll(isp_csid, 0x70, 1, 0, 1000)) return EIO;
		if (hw) { uw(isp_csid, 0x74, 1); uw(isp_csid, 0x80, 1); }
		uw(isp_csid, 0x10, hw ? 0x1e : 1);
		if (!hw) { uw(isp_csid, 0x74, 1); uw(isp_csid, 0x80, 1); }
		if (!upoll(isp_csid, 0x70, 1, 1, 100000)) {
			hakios_tl("UMI camera: CSID %s reset timed out, IRQ %#x\n", hw ? "hardware" : "register", ur(isp_csid, 0x70));
			return ETIMEDOUT;
		}
		uw(isp_csid, 0x78, 1); uw(isp_csid, 0x80, 1);
	}
	/* VFE lite48x/top-v3: command 2 resets HW+registers; bit17 ACK.
	 * Redwood's top-v2 command 0x3f9f/bit31 must never be used here.
	 */
	uw(isp_ife, 0x34, 1u<<17); uw(isp_ife, 0x24, 1);
	if (!upoll(isp_ife, 0x40, 1u<<17, 0, 1000)) return EIO;
	uw(isp_ife, 0x28, 1u<<17); uw(isp_ife, 0xc, 2);
	if (!upoll(isp_ife, 0x40, 1u<<17, 1u<<17, 100000)) {
		hakios_tl("UMI camera: IFE-lite48x reset timed out, IRQ %#x\n", ur(isp_ife, 0x40));
		return ETIMEDOUT;
	}
	hakios_tl("UMI camera: CSID480 and IFE-lite48x resets complete; DMA remains disabled\n");
	return 0;
}

static void
ucleanup(void)
{
	if (!uc.active) return;
	uisp_down();
	if (uc.clock_changed) {
		uw(uc.cc, 0x5034, uc.clock_branch);
		(void)upoll(uc.cc, 0x5034, 1u << 31, 1u << 31, 1000);
		uw(uc.cc, 0x5020, uc.clock_cfg);
		uw(uc.cc, 0x501c, ur(uc.cc, 0x501c) | 1);
		(void)upoll(uc.cc, 0x501c, 1, 0, 1000);
		uc.clock_changed = false;
	}
	if (uc.reset_changed) { upin(0, false); ums(1); uc.reset_changed = false; }
	/* Reverse module power order: CUSTOM2, CUSTOM1, VDIG, VANA, VIO, VAF. */
	for (unsigned step = 0; step < 6; ++step) {
		if (step == 0 || step == 1 || step == 3) {
			unsigned g = step == 0 ? 2 : step == 1 ? 1 : 0;
			if (gates[g].changed) {
				for (unsigned r = 0; r < 8; ++r)
					if (!ugpio_put(g, gate_regs[r], gates[g].saved[r]))
						hakios_tl("UMI camera: could not restore PPID %#x register %#x\n", gates[g].ppid, gate_regs[r]);
				gates[g].changed = false;
			}
		} else {
			unsigned rail = step == 2 ? 2 : step == 4 ? 1 : 0;
			if (uc.rails[rail]) {
				int error = uvreg(rail, false);
				hakios_tl("UMI camera: release supply %u error=%d\n", rail, error);
				uc.rails[rail] = false;
			}
		}
		ums(1);
	}
	for (unsigned p = 0; p < 4; ++p)
		if (uc.pins_valid & (1u << p)) {
			if (uc.pin_io_valid & (1u << p)) uw(uc.pin[p], 4, uc.pin_io[p] & 2);
			uw(uc.pin[p], 0, uc.pin_cfg[p]);
		}
	uc.pins_valid = uc.pin_io_valid = 0;
	/* Return CCI pins before collapsing their camera power domain. */
	if (uc.gdsc_changed) { uw(uc.cc, 0xc144, uc.gdsc & ~(1u << 31)); uc.gdsc_changed = false; }
	if (uc.gcc_saved) {
		uw(uc.gcc, 0xb040, (ur(uc.gcc, 0xb040) & ~1u) | (uc.gcc_xo & 1));
		uw(uc.gcc, 0xb008, (ur(uc.gcc, 0xb008) & ~1u) | (uc.gcc_ahb & 1));
		uc.gcc_saved = false;
	}
	uc.active = false;
}

void
umi_camera_probe_quiesce(void)
{
	uc.panic = true;
	ucleanup();
}

static int
uprobe(unsigned stage)
{
	if (uc.active) return EBUSY;
	uc.panic = false;
	uc.core = umap(0x0c440000, 0x1100);
	uc.config = umap(0x0c40a000, 0x1000);
	uc.observer = umap(0x0e600000, 0x10000);
	uc.cc = umap(0x0ad00000, 0x10000);
	uc.gcc = umap(0x00100000, 0x80000);
	vm_offset_t south = umap(0x0f500000, 0x80000);
	vm_offset_t north = umap(0x0f900000, 0x80000);
	static const unsigned pins[4] = { 62, 95, 103, 104 };
	if (!uc.core || !uc.config || !uc.observer || !uc.cc || !uc.gcc || !south || !north) return ENOMEM;
	uc.active = true;
	int error = EIO;
	uint16_t id = 0, revision = 0;
	/* gcc-kona.c marks these clocks critical. Both use BRANCH_HALT_DELAY:
	 * enable, verify the vote, and allow propagation instead of waiting on
	 * a halt status that this branch type does not implement reliably.
	 * CamCC register reads before the AHB vote cause an external abort.
	 */
	uc.gcc_ahb = ur(uc.gcc, 0xb008); uc.gcc_xo = ur(uc.gcc, 0xb040);
	uc.gcc_saved = true;
	hakios_tl("UMI camera: GCC camera AHB=%#x XO=%#x; enabling bus clocks\n", uc.gcc_ahb, uc.gcc_xo);
	uw(uc.gcc, 0xb008, uc.gcc_ahb | 1);
	uw(uc.gcc, 0xb040, uc.gcc_xo | 1);
	delay(10);
	if (!(ur(uc.gcc, 0xb008) & 1) || !(ur(uc.gcc, 0xb040) & 1)) goto done;
	uc.clock_cfg = ur(uc.cc, 0x5020); uc.clock_branch = ur(uc.cc, 0x5034); uc.gdsc = ur(uc.cc, 0xc144);
	if ((uc.clock_branch & 1) || !ugpio_prepare()) {
		hakios_tl("UMI camera: MCLK1 busy or PM8150L GPIOs unavailable\n");
		error = EBUSY;
		goto done;
	}
	/* Stock CCI enables its Titan-top supply before selecting its pinctrl
	 * state. Preserve that ordering for CCI GPIO access, including reads.
	 */
	uc.gdsc_changed = true;
	hakios_tl("UMI camera: enabling Titan-top, initial GDSCR=%#x\n", uc.gdsc);
	uw(uc.cc, 0xc144, uc.gdsc & ~1u);
	if (!upoll(uc.cc, 0xc144, 1u << 31, 1u << 31, 2000)) {
		hakios_tl("UMI camera: Titan-top did not power on, GDSCR=%#x\n", ur(uc.cc, 0xc144));
		goto done;
	}
	for (unsigned p = 0; p < 4; ++p) {
		uc.pin[p] = (p == 0 ? south : north) + 0x1000UL * pins[p];
		hakios_tl("UMI camera: reading GPIO%u configuration\n", pins[p]);
		uc.pin_cfg[p] = ur(uc.pin[p], 0);
#if defined(__aarch64__)
		__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
		uc.pins_valid |= 1u << p;
		if ((uc.pin_cfg[p] & (1u << 11)) && !(uc.pin_cfg[p] & (1u << 12)))
			uw(uc.pin[p], 0, uc.pin_cfg[p] | 1u << 12);
		hakios_tl("UMI camera: GPIO%u cfg=%#x; reading level\n", pins[p], uc.pin_cfg[p]);
		uc.pin_io[p] = ur(uc.pin[p], 4);
#if defined(__aarch64__)
		__asm__ volatile("dsb sy; isb" ::: "memory");
#endif
		hakios_tl("UMI camera: GPIO%u io=%#x\n", pins[p], uc.pin_io[p]);
		uc.pin_io_valid |= 1u << p;
	}
	uc.reset_changed = true;
	upin(0, false);
	for (unsigned step = 0; step < 6; ++step) {
		if (step == 2 || step == 4 || step == 5) {
			unsigned g = step == 2 ? 0 : step == 4 ? 1 : 2;
			if (!ugpio_on(g)) goto done;
		} else {
			unsigned rail = step == 0 ? 0 : step == 1 ? 1 : 2;
			int rail_error = uvreg(rail, true);
			if (rail_error) {
				error = rail_error;
				hakios_tl("UMI camera: supply %u enable failed errno=%d\n", rail, error);
				goto done;
			}
			uc.rails[rail] = true;
		}
		ums(step == 0 ? 10 : 1);
	}
	upin(0, true); ums(3);
	if (!uclock_on()) { hakios_tl("UMI camera: MCLK1/Titan-top startup failed\n"); goto done; }
	ums(2);
	if (!usensor_read(0, &id) || id != 0x1ad0) {
		hakios_tl("UMI camera: S5KHMX ID read failed, got %#x (expected 0x1ad0)\n", id);
		goto done;
	}
	/* Samsung's chip revision register is read separately; no table selection
 * is inferred from it until it has been compared with the module selector.
 */
	if (usensor_read(2, &revision)) hakios_tl("UMI camera: S5KHMX ID %#x, revision register %#x\n", id, revision);
	else hakios_tl("UMI camera: S5KHMX ID %#x; revision read did not answer\n", id);
	/* The module's EEPROM map appends a sensor read at 0x000e after
	 * 0x4000 EEPROM bytes. SensorVersionOffset selects that appended byte,
	 * not register 0x0002 and not EEPROM address 0x4000. Read the full word
	 * here to retain byte-order evidence before selecting a version table.
	 */
	uint16_t selector = 0;
	if (usensor_read(0x000e, &selector))
		hakios_tl("UMI camera: module sensor-version source register 0x000e=%#x\n", selector);
	else hakios_tl("UMI camera: module sensor-version source did not answer\n");
	error = stage >= 3 ? umode_prepare(selector >> 8) : 0;
	if (!error && stage >= 4) error = uisp_probe();
	if (!error && stage == 6) error = umi_camera_tpg(isp_ife, isp_csid);
done:
	ucleanup();
	hakios_tl("UMI camera: sensor probe finished error=%d; sensor stream was not started\n", error);
	return error;
}

int
umi_camera_probe(void)
{
	return uprobe(2);
}

int
umi_camera_prepare(void)
{
	return uprobe(3);
}

int
umi_camera_isp_probe(void)
{
	return uprobe(4);
}

int
umi_camera_capture_tpg(void)
{
	return uprobe(6);
}
