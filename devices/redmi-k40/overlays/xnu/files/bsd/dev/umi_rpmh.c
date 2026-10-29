/* UMI PM8150A L1 ACTIVE vote through the SM8250 RPMh apps DRV.
 * Adapted from the validated older UMI project; bounded waits, no recovery
 * or sleep/wake programming. Used by the gated UMI touch and PCIe clients. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>

/* Preserve hardware diagnostics in the USB log even in quiet UI boots. */
extern void hakios_tl(const char *, ...) __printflike(1, 2);
#define printf hakios_tl
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern lck_mtx_t hakios_rsc_lock;
int umi_rpmh_vreg_enable(const char *res, unsigned mv);
int umi_rpmh_wifi_power(void);
int umi_rpmh_bluetooth_power(void);
int umi_rpmh_camera_vreg(unsigned rail, bool enable);

/* Live downstream DT has two positional reg entries, no reg-names. */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_VRM          4u
#define RH_TIMEOUT_US   100000u

static vm_offset_t rh_rsc, rh_tcs, rh_db, rh_gic;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;
static uint32_t rh_uncertain_irq;

static uint32_t
rh_read(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		delay(1);
	}
	printf("UMI RPMH: readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_gic) rh_gic = ml_io_map(0x17a00000, 0x10000);
	if (!rh_rsc) {
		rh_rsc = ml_io_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = ml_io_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_rsc || !rh_tcs) {
		printf("UMI RPMH: RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	printf("UMI RPMH: rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		printf("UMI RPMH: unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		/* umi live DT reserved-memory/cmd_db, not sweet's IMEM locator. */
		uint32_t pa = 0x80860000u;
		uint32_t size = 0x20000u;
		printf("UMI RPMH: cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			printf("UMI RPMH: invalid cmd-db locator\n");
			return EINVAL;
		}
		rh_db = ml_io_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	printf("UMI RPMH: cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		printf("UMI RPMH: unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

/* A resource of cmd-db: its address, and (ARC) where its levels are. VRM: rh_lookup(). */
static int
rh_lookup_slave(const char *res, unsigned want, uint32_t *address, uint32_t *aux_at, uint32_t *aux_len)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
			printf("UMI RPMH: malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			if (slave != want || (addr >> 16) != want || (addr & 15u) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
				printf("UMI RPMH: invalid entry for %s (slave %u, wanted %u)\n", res, slave, want);
				return EINVAL;
			}
			if (aux_at != NULL) *aux_at = aux + rh_db16(e + 22);
			if (aux_len != NULL) *aux_len = rh_db16(e + 20);
			/* UMI's firmware repeats some VRM entries across slave headers.
			 * An alias is harmless only when it resolves to the same register. */
			if (matches && *address != addr) {
				printf("UMI RPMH: conflicting addresses for %s: %#x / %#x\n", res, *address, addr);
				return EINVAL;
			}
			*address = addr;
			++matches;
			printf("UMI RPMH: %s addr=%#x slave=%u aux-version=%#x priority=%#x:%#x\n",
			    res, addr, slave, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches == 0) {
		printf("UMI RPMH: %s lookup matches=%u; refusing\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	return rh_lookup_slave(res, RH_VRM, address, NULL, NULL);
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* UEFI leaves AMC_EN set on an empty idle ACTIVE slot. No trigger,
	 * commands, wait mask or pending completion may remain. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    (rh_read(rh_tcs, b + RH_CONTROL) & ~RH_AMC_EN) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

/* One ACTIVE request of up to three commands (address + offsets[c] = values[c]) to one resource. */
static int
rh_send_cmds_locked(uint32_t address, const uint32_t *offsets, const uint32_t *values, unsigned commands)
{
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		printf("UMI RPMH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address) {
				printf("UMI RPMH: conflicting inherited VRM request in TCS%u\n", t);
				return EBUSY;
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("UMI RPMH: no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	uint32_t inherited_control = rh_read(rh_tcs, b + RH_CONTROL);
	if (irq_en == 0xffffffffu || !rh_free(t)) {
		return EBUSY;
	}
	rh_uncertain_irq = bit;
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	uint32_t command_mask = (1u << commands) - 1;
	printf("UMI RPMH: TCS%u ACTIVE %#x = %u commands=%u second=%u irq-enable=%#x\n",
	    t, address, values[0], commands, commands > 1 ? values[1] : 0, irq_en);
	for (unsigned c = 0; c < commands; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + RH_WAIT, command_mask) || !rh_sync(b + RH_ENABLE, command_mask) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	/* The live DT's DRV2 interrupt is SPI5 (GIC INTID37), level high.
	 * Latch a completion IRQ only while that line is masked at the GIC and
	 * belongs to nonsecure Group1. No GIC state is changed. Without this
	 * latch, a fast transaction can finish before the first busy sample and
	 * leave only CMD_STATUS values identical to the previous transaction.
	 */
	bool latch_irq = rh_gic && (rh_read(rh_gic, 0x84) & (1u << 5)) &&
	    !(rh_read(rh_gic, 0x104) & (1u << 5)) && !(rh_read(rh_gic, 0xc08) & (2u << 10));
	if (latch_irq && !rh_sync(RH_IRQ_EN, rh_read(rh_tcs, RH_IRQ_EN) | bit)) return EIO;
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[3] = {0, 0, 0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < commands; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		/* A fresh IRQ or observed execution transition excludes stale CMD_STATUS. */
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete) {
			done = true;
			break;
		}
		delay(1);
	}
	printf("UMI RPMH: TCS%u done=%d latch=%d irq=%#x idle=%#x control=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, latch_irq, irq, idle, rh_read(rh_tcs, b + RH_CONTROL), status[0], status[1], status[2]);
	if (!done) {
		printf("UMI RPMH: timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		return ETIMEDOUT;
	}
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(RH_IRQ_EN, rh_read(rh_tcs, RH_IRQ_EN) & ~bit) ||
	    !rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, inherited_control) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_poisoned = false;
	printf("UMI RPMH: ACTIVE vote complete (rail voltage requires hardware verification)\n");
	return 0;
}

static int
rh_send_cmds(uint32_t address, const uint32_t *offsets, const uint32_t *values, unsigned commands)
{
	/* Share the sequencer lock with ADSP/MPSS. Panic cleanup cannot wait for
	 * a mutex held by another stopped CPU.
	 */
	if (ml_get_interrupts_enabled()) lck_mtx_lock(&hakios_rsc_lock);
	else if (!lck_mtx_try_lock(&hakios_rsc_lock)) return EBUSY;
	int error = rh_send_cmds_locked(address, offsets, values, commands);
	if (rh_poisoned && rh_uncertain_irq) {
		uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
		if (irq_en != 0xffffffffu) (void)rh_sync(RH_IRQ_EN, irq_en & ~rh_uncertain_irq);
	}
	if (!rh_poisoned) rh_uncertain_irq = 0;
	lck_mtx_unlock(&hakios_rsc_lock);
	return error;
}

static int
rh_send_state(uint32_t address, unsigned mv, bool set_ldo_mode, bool enable)
{
	/* Touch's PMIC5 LDO needs HPM. Wi-Fi's shared SMPS/LDO rails retain
	 * their inherited mode; a PMIC5 LDO mode must not be applied to an SMPS. */
	const uint32_t offsets[3] = {0, set_ldo_mode ? 8u : 4u, 4};
	const uint32_t values[3] = {mv, set_ldo_mode ? 7u : (uint32_t)enable, (uint32_t)enable};
	return rh_send_cmds(address, offsets, values, set_ldo_mode ? 3 : 2);
}

static int
rh_send(uint32_t address, unsigned mv, bool set_ldo_mode)
{
	return rh_send_state(address, mv, set_ldo_mode, true);
}

/*
 * An ACTIVE vote for a power domain's level (an ARC resource of cmd-db, "mmcx.lvl", say): the lowest of
 * its levels (cmd-db's aux data, 16-bit corners: 64 low SVS, 128 SVS, 192 SVS L1, 256 NOM, 384 turbo) that
 * is at least `corner`, sent as that level's index -- as Linux rpmhpd does. Held until reset; a vote
 * only raises the domain (the levels of the other voters still count).
 */
#define RH_ARC          3u
int umi_rpmh_arc_level(const char *res, unsigned corner);
int umi_rpmh_bcm_vote(const char *res, unsigned vote_x, unsigned vote_y);

/*
 * An ACTIVE vote of a bus clock manager (a BCM resource of cmd-db: "MC0" the DDR, "SH0" the system
 * cache's fabric): x the average, y the peak, in the BCM's own units, 14 bits each (Linux
 * include/soc/qcom/tcs.h BCM_TCS_CMD: commit 30, valid 29, x << 14, y). 0x3fff each asks for the most
 * the bus has. Held until reset.
 */
#define RH_BCM          5u

int
umi_rpmh_bcm_vote(const char *res, unsigned vote_x, unsigned vote_y)
{
	uint32_t address = 0, aux = 0, len = 0;
	int error = EIO;

	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	if (rh_poisoned) goto out;
	error = rh_map();
	if (!error) error = rh_lookup_slave(res, RH_BCM, &address, &aux, &len);
	if (!error) {
		const uint32_t offsets[1] = {0};
		const uint32_t values[1] = {1u << 30 | 1u << 29 | (vote_x & 0x3fffu) << 14 | (vote_y & 0x3fffu)};

		printf("UMI RPMH: %s BCM unit data %#x %#x\n", res, len >= 4 ? rh_read(rh_db, aux & ~3u) : 0, len);
		error = rh_send_cmds(address, offsets, values, 1);
	}
out:
	printf("UMI RPMH: %s ACTIVE bus vote %#x/%#x: error %d\n", res, vote_x, vote_y, error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

int
umi_rpmh_arc_level(const char *res, unsigned corner)
{
	uint32_t address = 0, aux = 0, len = 0, level = 0;
	int error = EIO;
	char levels[96];
	int n = 0;
	bool found = false;

	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	if (rh_poisoned) goto out;
	error = rh_map();
	if (!error) error = rh_lookup_slave(res, RH_ARC, &address, &aux, &len);
	if (!error && (len < 2 || len > 32 || (len & 1u))) {
		printf("UMI RPMH: %s has %u bytes of levels\n", res, len);
		error = EINVAL;
	}
	for (uint32_t i = 0; !error && i < len / 2; i++) {
		unsigned c = rh_db16(aux + 2 * i);

		n += snprintf(levels + n, sizeof(levels) - (size_t)n, " %u", c);
		if (!found && c >= corner) {
			level = i;
			found = true;
		}
	}
	if (!error) {
		printf("UMI RPMH: %s levels:%s; corner %u is level %u\n", res, levels, corner, found ? level : 99);
		if (!found) error = ERANGE;
	}
	if (!error) {
		const uint32_t offsets[1] = {0}, values[1] = {level};

		error = rh_send_cmds(address, offsets, values, 1);
	}
out:
	printf("UMI RPMH: %s ACTIVE corner %u: error %d\n", res, corner, error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* Camera owns these three PM8009 supplies. Wi-Fi, Bluetooth and touch use
 * other resources. Drop only votes successfully established by this client.
 * Values are the actual Android rear-1x startup requests, including L1's
 * PMIC voltage-step rounding to 1104 mV.
 */
int
umi_rpmh_camera_vreg(unsigned rail, bool enable)
{
	static const struct { const char *resource; unsigned mv; } rails[] = {
		{ "ldof6", 2850 }, { "ldof7", 1800 }, { "ldof1", 1104 },
	};
	static bool owned[3];
	if (rail >= sizeof(rails) / sizeof(rails[0])) return EINVAL;
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	int error = 0;
	uint32_t address = 0;
	if (owned[rail] == enable) goto out;
	if (rh_poisoned) { error = EIO; goto out; }
	error = rh_map();
	if (!error) error = rh_lookup(rails[rail].resource, &address);
	if (!error) error = rh_send_state(address, rails[rail].mv, true, enable);
	if (!error) owned[rail] = enable;
	printf("UMI RPMH: camera %s %u mV enable=%u error=%d\n",
	    rails[rail].resource, rails[rail].mv, enable, error);
out:
	if (error) printf("UMI RPMH: camera supply %u enable=%u refused errno=%d poisoned=%u\n",
	    rail, enable, error, rh_poisoned);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/*
 * alioth: the PM8150A LDOs its cameras use (hakios.camera.c), by name and only at the device tree's
 * voltages: L2C 1.2 V (the macro's VDIG; the display's DVDD too, which the camera driver never switches
 * off), L3C 1.2 V (the ultra-wide's VDIG), L5C 1.8 V (the four sensors' VIO), L7C 2.856 V (the wide one's
 * lens motor). PMIC5 LDO high power mode, enabled or not.
 */
int umi_rpmh_camera_ldo(const char *res, unsigned mv, bool enable);

int
umi_rpmh_camera_ldo(const char *res, unsigned mv, bool enable)
{
	static const struct { const char *resource; unsigned mv; } rails[] = {
		{ "ldoc2", 1200 }, { "ldoc3", 1200 }, { "ldoc5", 1800 }, { "ldoc7", 2856 },
	};
	bool known = false;

	for (unsigned i = 0; res != NULL && i < sizeof(rails) / sizeof(rails[0]); i++) {
		known |= strcmp(res, rails[i].resource) == 0 && mv == rails[i].mv;
	}
	if (!known) {
		printf("UMI RPMH: camera rail %s at %u mV is not one of alioth's\n", res ? res : "(none)", mv);
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	int error = EIO;
	uint32_t address = 0;
	if (rh_poisoned) goto out;
	error = rh_map();
	if (!error) error = rh_lookup(res, &address);
	if (!error) error = rh_send_state(address, mv, true, enable);
	printf("UMI RPMH: camera %s %u mV enable=%u error=%d\n", res, mv, enable, error);
out:
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

int
umi_rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* Board-specific helper: do not apply PMIC5-LDO policy to arbitrary resources. */
	/* umi: st,fts vdd-supply -> PM8150A L1, resource ldoc1, exactly 1.8V. */
	if (!res || strcmp(res, "ldoc1") || mv != 1800) {
		printf("UMI RPMH: expected ldoc1 at 1800 mV\n");
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (rh_poisoned) {
		printf("UMI RPMH: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, &address);
	}
	if (!error) {
		error = rh_send(address, mv, true);
	}
out:
	if (error) {
		printf("UMI RPMH: enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* Exact UMI supplies from the merged stock DT, not a generic regulator API.
 * ACTIVE votes are held until reset; no SLEEP/WAKE votes or shared-rail disable.
 * Called only by the explicit PCIe diagnostic, with the endpoint held in reset.
 */
int
umi_rpmh_wifi_power(void)
{
	static const struct { const char *resource; unsigned mv; } rails[] = {
		{ "smpa4", 1800 }, /* WLAN IO */
		{ "smpa6", 950 },  /* WLAN AON */
		{ "smpa5", 1900 }, /* WLAN RFA1 */
		{ "smpc8", 1350 }, /* WLAN RFA2 */
		{ "ldoa9", 1200 }, /* PCIe vreg-1p8: DT actually requests 1.2 V */
		{ "ldoa5", 880 },  /* PCIe vreg-0p9 */
	};
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	int error = EIO;
	uint32_t addresses[sizeof(rails) / sizeof(rails[0])];
	if (rh_poisoned) goto out;
	error = rh_map();
	/* Validate every resource before starting the first power transaction. */
	for (unsigned i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i)
		error = rh_lookup(rails[i].resource, &addresses[i]);
	for (unsigned i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i) {
		printf("UMI RPMH: Wi-Fi/PCIe %s minimum=%u mV\n", rails[i].resource, rails[i].mv);
		error = rh_send(addresses[i], rails[i].mv, false);
	}
out:
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* QCA6390 BT rails from the UMI stock DT; all are shared with WLAN. */
int
umi_rpmh_bluetooth_power(void)
{
	static const struct { const char *resource; unsigned mv; } rails[] = {
		{ "smpa6", 950 },  /* AON */
		{ "smpa5", 1900 }, /* RFA1 */
		{ "smpc8", 1350 }, /* RFA2 */
		{ "ldoa16", 3025 },/* ASD, minimum of stock 3025-3300 mV range */
	};
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
	int error = EIO;
	uint32_t addresses[sizeof(rails) / sizeof(rails[0])];
	if (rh_poisoned) goto out;
	error = rh_map();
	for (unsigned i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i)
		error = rh_lookup(rails[i].resource, &addresses[i]);
	for (unsigned i = 0; !error && i < sizeof(rails) / sizeof(rails[0]); ++i) {
		printf("UMI RPMH: Bluetooth %s minimum=%u mV\n", rails[i].resource, rails[i].mv);
		error = rh_send(addresses[i], rails[i].mv, false);
	}
out:
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
