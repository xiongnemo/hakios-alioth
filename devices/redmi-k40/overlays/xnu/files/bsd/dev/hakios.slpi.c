/*
 * The sensor DSP of the Redmi K40 (alioth): the SLPI (Snapdragon "sensor low-power island", Android's "slpi",
 * the dt's qcom,ssc@5c00000), started, and spoken to: step 1 of the sensors (the ambient light sensor first).
 * Made from hakios.adsp.c by work/wsl/mk-slpi.py -- the same way, the SLPI's numbers: processor 3 in the shared
 * memory, its SMP2P items 481 (its) and 430 (ours), its last words in item 424; IPCC client 4 (signal 0 GLINK, 2
 * SMP2P); TrustZone's image 12; its memory pil_slpi_region@88c00000, 0x2f00000 bytes; "slpi" to the always-on
 * processor; rails LCX and LMX at 384 while it boots. The firmware: the phone's own slpi.mdt and slpi.b00-b21
 * (from /vendor/firmware_mnt/image) in fw/ of the small image (mkroot-variant.sh SLPI=1) or of the system image.
 * Boot-args as the audio DSP's, slpi_ for adsp_: slpi=1 (all of it, left up), slpi_step=N, slpi_keep=1, ...
 * The sensors' process asks the AP for its configuration over FastRPC (fastrpcglink-apps-dsp), and without an
 * answer it gives up some 40 s on (Linux on this phone: "SLPI watchdog every 40 s" without hexagonrpcd): steps
 * 11 and 12 attach to it and answer it as hexagonrpcd does, from fw/sensors/ (since made by hand, not by
 * mk-slpi.py any more).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the Wi-Fi's DSP and this one both use (hakios.wifi.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the other DSP's driver */

static LCK_GRP_DECLARE(slpi_lock_group, "hakios-slpi");

#define STEPS                   13
#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios.wifi.c);
 * with slpi=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &slpi_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in hakios.wifi.c) ------------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("slpi: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("slpi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("slpi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (hakios.wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_SLPI               3               /* dt: qcom,remote-pid of qcom,smp2p-dsps and qcom,glink/dsps */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-dsps, <481 430>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         430
#define ITEM_SMP2P_THEIRS       481
#define ITEM_CRASH_REASON       424
#define ITEM_GLINK_OUR_QUEUE    479

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_SLPI }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no partition for us and the sensor DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("slpi: %u: the partition for us and the sensor DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("slpi: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("slpi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("slpi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("slpi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("slpi: %u: THE SENSOR DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (hakios.wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_SLPI               4u              /* dt: mboxes of qcom,smp2p-dsps and qcom,glink/dsps */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The sensor DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_SLPI) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("slpi: doorbell: +%llu.%03llu ms: signal %u of the sensor DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as hakios.wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("slpi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("slpi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("slpi: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("slpi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("slpi: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("slpi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("slpi: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The sensor DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

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

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("slpi: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("slpi: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("slpi: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("slpi: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	lck_mtx_lock(&hakios_rsc_lock);
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("slpi: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	printf("slpi: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the sensor DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("slpi: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the sensor DSP in the shared memory (0x80900000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (481) is there: it has run since the phone was started", &words);
	} else {
		printf("slpi: %u: the DSP's SMP2P item (481) is not there: it has not run since the phone was started\n", step);
	}
	printf("slpi: %u: our SMP2P item (430): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("slpi: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_SLPI, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define HAKIOS_SLPI_MEMORY     0x88c00000u     /* alioth: reserved-memory pil_slpi_region@88c00000 */
#define HAKIOS_SLPI_SIZE       0x02f00000u
#define PAS_SLPI                12              /* dt: qcom,pas-id of qcom,ssc@5c00000 */

static uint32_t slpi_memory = HAKIOS_SLPI_MEMORY;
static uint32_t slpi_memory_size = HAKIOS_SLPI_SIZE;


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-sensors/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only slpi=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > slpi_memory_size) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("slpi: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* A file of fw/ in the small image (ADSP=1 userland/build.sh puts fw-sensors/ there), else of /fw on the root (above). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios.image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			return data;
		}
	}
	printf("slpi: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-sensors/ and run SYSTEM=1 userland/build.sh (or ADSP=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MDT_RELOCATABLE         (1u << 27)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size, segment_base;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0, highest = 0;
	uint32_t count, hashes = 0, lowest = ~0u;
	unsigned loadable = 0;
	bool relocatable = false;

	announce("find the sensor DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("slpi.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("slpi: %u: fw/slpi.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	/* Qualcomm's relocatable MDTs use segment addresses relative to their lowest segment. PAS
	 * memory setup relocates that span into the reserved carveout from the device tree. */
	for (uint32_t i = 0; i < count; i++) {
		uint64_t end;

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.file_size > segment.memory_size) {
			printf("slpi: %u: segment %u has %u file bytes but only %u memory bytes\n", step, i, segment.file_size,
			    segment.memory_size);
			goto fail;
		}
		relocatable |= (segment.flags & MDT_RELOCATABLE) != 0;
		end = (uint64_t)segment.paddr + segment.memory_size;
		if (end > UINT32_MAX) {
			printf("slpi: %u: segment %u wraps the firmware address space\n", step, i);
			goto fail;
		}
		if (segment.paddr < lowest) {
			lowest = segment.paddr;
		}
		if (end > highest) {
			highest = end;
		}
		loadable++;
	}
	image.segment_base = relocatable ? lowest : slpi_memory;
	if (loadable == 0 || lowest < image.segment_base || highest < image.segment_base ||
	    highest - image.segment_base > slpi_memory_size ||
	    ((highest - image.segment_base + 0xfff) & ~0xfffull) > slpi_memory_size) {
		printf("slpi: %u: the image span %#x..%#llx (%s) does not fit the DSP's %#x-byte carveout at %#x\n", step, lowest,
		    highest, relocatable ? "relocatable" : "fixed", slpi_memory_size, slpi_memory);
		goto fail;
	}
	image.size = (uint32_t)((highest - image.segment_base + 0xfff) & ~0xfffull);
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		unsigned s = image.segments++;
		image.segment[s].at = segment.paddr - image.segment_base;
		image.segment[s].file_size = segment.file_size;
		image.segment[s].memory_size = segment.memory_size;
		image.segment[s].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "slpi.b%02u", i);
			image.segment[s].file = image_file(name, &file_size);
			if (image.segment[s].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("slpi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				goto fail;
			}
			files += file_size;
		}
	}
	if (image.segments != loadable || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("slpi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	printf("slpi: %u: the image is whole (%s, %s MDT): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, "
	    "span %#x from %#x relocated into the DSP's %#x-byte carveout at %#x\n", step,
	    from_root ? "from /fw on the root" : "from the small image", relocatable ? "relocatable" : "fixed", size, hashes,
	    image.segments, files, image.size, image.segment_base, slpi_memory_size, slpi_memory);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &slpi_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("slpi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("slpi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("slpi: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("slpi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("slpi: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("slpi: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("slpi: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("slpi: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("slpi: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("slpi: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the sensor DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("slpi: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE SENSOR DSP ACKNOWLEDGES THE STOP" : "THE SENSOR DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);
static void rpc_space_release(void);
static void rpc_heap_free(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("slpi: %u: %s: the sensor DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the sensor DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_SLPI, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT SHUT THE SENSOR DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("slpi: %u: THE SENSOR DSP IS SHUT DOWN\n", step);
	if (!leaving) {
		rpc_space_release();    /* (no more of its requests: the SMMU as it was, the memory it had ours again) */
		rpc_heap_free();
	}
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_SLPI << 16 | IPCC_SMP2P);
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: slpi, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an sensor DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("slpi_again")) {
			printf("slpi: %u: THE SENSOR DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (slpi_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("slpi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("slpi: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the sensor DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_SLPI, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("slpi: %u: TRUSTZONE ACCEPTS THE SENSOR DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_SLPI, slpi_memory, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("slpi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, slpi_memory);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static uint8_t
segment_byte(unsigned s, uint32_t at)
{
	return at < image.segment[s].file_size ? image.segment[s].file[at] : 0;
}

static bool
step_image(void)
{
	volatile uint8_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the sensor DSP's reserved memory, with zeroes where a segment is longer than its "
	    "file, and read them back");
	dsp_memory = (volatile uint8_t *)ml_io_map_unmappable(slpi_memory, slpi_memory_size, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, slpi_memory);
	for (unsigned s = 0; s < image.segments; s++) {
		uint32_t at = image.segment[s].at, i = 0, size = image.segment[s].memory_size;

		/* Keep exact segment boundaries. Most MDTs are word-aligned, but the UMI image ends
		 * one read-only segment on a four-byte boundary, before its separate BSS segment. */
		while (i < size && ((at + i) & 7)) {
			dsp_memory[at + i] = segment_byte(s, i);
			i++;
		}
		for (; i + 8 <= size; i += 8) {
			volatile uint64_t *to = (volatile uint64_t *)(dsp_memory + at + i);

			*to = segment_word(s, i);
		}
		for (; i < size; i++) {
			dsp_memory[at + i] = segment_byte(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		uint32_t at = image.segment[s].at, i = 0, size = image.segment[s].memory_size;

		while (i < size && ((at + i) & 7)) {
			wrong += dsp_memory[at + i] != segment_byte(s, i);
			i++;
		}
		for (; i + 8 <= size; i += 8) {
			volatile uint64_t *from = (volatile uint64_t *)(dsp_memory + at + i);

			wrong += *from != segment_word(s, i);
		}
		for (; i < size; i++) {
			wrong += dsp_memory[at + i] != segment_byte(s, i);
		}
	}
	printf("slpi: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, slpi_memory_size);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* An item made in the partition, as Linux's qcom_smem_alloc_private() makes one. */
static bool
smem_private_item_make(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("slpi: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock for item %u", step, item);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("slpi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("slpi: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			printf("slpi: %u: item %u appeared while the allocation lock was held: nothing is written\n", step, item);
			goto out;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("slpi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("slpi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("slpi: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

/* Our SMP2P item and GLINK's 16 KiB host-to-DSP queue are private items in this partition. */
static bool
our_item_make(uint32_t partition)
{
	return smem_private_item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p));
}

/* Repair a malformed old entry only when its allocation is exactly the requested size. */
static bool
smem_private_item_repair_padding(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, expected_padding = item_size - bytes;
	uint32_t owner = ~0u, at;
	bool repaired = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);
	need(&smem_lock);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("slpi: %u: cannot inspect item %u: shared-memory lock stays with processor %u\n", step, item, owner);
		goto out;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("slpi: %u: cannot inspect item %u: the partition's marks make no sense\n", step, item);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			if (entry.size != item_size || entry.padding_header != 0 || entry.padding_data > entry.size) {
				printf("slpi: %u: item %u cannot be repaired safely: allocation %u bytes, padding %u/%u\n", step, item,
				    entry.size, entry.padding_header, entry.padding_data);
				goto out;
			}
			if (entry.padding_data != expected_padding) {
				printf("slpi: %u: correcting item %u padding from %u to %u bytes; its allocation remains %u bytes\n", step,
				    item, entry.padding_data, expected_padding, entry.size);
				smem_poke(partition + at + 8, expected_padding | (uint32_t)entry.padding_header << 16);
				settle();
			}
			repaired = true;
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (!repaired) {
		printf("slpi: %u: item %u was not found in this private partition to repair\n", step, item);
	}
out:
	if (owner == LOCK_US) {
		wr(smem_lock.base + LOCK_OWNER, 0);
	}
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return repaired;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (430) in our partition with the sensor DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("slpi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("slpi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("slpi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_SLPI << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("slpi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	{
		uint32_t queue_size = 0, queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);

		if (queue != 0) {
			if (queue_size != 0x4000) {
				printf("slpi: %u: GLINK's host queue (item %u) reads as %u bytes; checking its private allocation\n", step,
				    ITEM_GLINK_OUR_QUEUE, queue_size);
				if (!smem_private_item_repair_padding(partition, ITEM_GLINK_OUR_QUEUE, 0x4000)) {
					return false;
				}
				queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);
				if (queue == 0 || queue_size != 0x4000) {
					printf("slpi: %u: GLINK's host queue (item %u) remains the wrong size (%u; expected 16384)\n", step,
					    ITEM_GLINK_OUR_QUEUE, queue_size);
					return false;
				}
			}
			printf("slpi: %u: GLINK's host queue (item %u) is already there, at %#lx, %u bytes\n", step,
			    ITEM_GLINK_OUR_QUEUE, SMEM_PHYS + queue, queue_size);
		} else {
			if (!smem_private_item_make(partition, ITEM_GLINK_OUR_QUEUE, 0x4000)) {
				return false;
			}
			queue = smem_item(ITEM_GLINK_OUR_QUEUE, &queue_size);
			if (queue == 0 || queue_size != 0x4000) {
				printf("slpi: %u: GLINK's host queue (item %u) did not read back at 16384 bytes\n", step,
				    ITEM_GLINK_OUR_QUEUE);
				return false;
			}
			printf("slpi: %u: made GLINK's host queue (item %u), at %#lx, %u bytes\n", step,
			    ITEM_GLINK_OUR_QUEUE, SMEM_PHYS + queue, queue_size);
		}
	}
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("slpi: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("slpi_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the sensor DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_SLPI; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_SLPI << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_SLPI << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: slpi, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the sensor DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_SLPI, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("slpi: %u: TRUSTZONE DOES NOT START THE SENSOR DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: slpi, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("slpi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE SENSOR DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("slpi: %u: THE SENSOR DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("slpi: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("slpi: %u: THE SENSOR DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("slpi: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (430) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("slpi: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("slpi: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("slpi: %u: %s\n", step, spoke ? "THE SENSOR DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE SENSOR DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (hakios.wifi.c; items 478 to 480 of our partition with the sensor DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,slpi_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_FASTRPC, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &slpi_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		/* (dt: qcom,msm_fastrpc_rpmsg of glink/dsps, qcom,intents <0x64 0x40>: answers of a few words each) */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 2, .intents = { { 0x80, 16 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("slpi: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("slpi: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("slpi: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("slpi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("slpi: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("slpi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("slpi: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("slpi: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("slpi: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("slpi: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("slpi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("slpi: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (hakios.wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("slpi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("slpi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("slpi: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("slpi: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("slpi: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

#define SEE_PORT                0x4000u         /* our port for the sensors' service (step 13) */
static void see_input(const uint8_t *data, uint32_t length);

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("slpi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("slpi: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.to_port == SEE_PORT) {
		see_input(data + at, header.size);
		return;
	}
	printf("slpi: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- FastRPC: the sensors' process reads its configuration through us ---------------------------------- */

/*
 * The sensors' process on the SLPI (its "sensors protection domain") reads what it is to run with through the
 * AP: the registry, the sensors' JSON files, the SoC's identity -- by FastRPC's reverse calls, which on Linux
 * hexagonrpcd answers (sensh's hexagonrpc, data/hexagonrpcd-sdsp.service: -f /dev/fastrpc-sdsp -d sdsp -s);
 * with nobody there it gives up some 40 s after it starts. FastRPC (Linux drivers/misc/fastrpc.c): a message
 * on the GLINK channel "fastrpcglink-apps-dsp" names a buffer of ours, in memory the DSP reaches through its
 * stream of the SMMU, that holds a call's arguments; the DSP answers on the channel with the call's context
 * and its result, the outputs written into the buffer. Its reverse calls ride on one call of ours that it
 * holds until it has one (adsp_listener's next2): that call's outputs are the call the DSP wants answered,
 * the next one's inputs the answer (hexagonrpcd/listener.c). The files themselves are fw/sensors/ of the
 * small image, laid out as the DSP names them (mkroot-variant.sh SLPI=1, from the phone's own Android).
 */
#define RPC_STREAM              0x0541u         /* dt: qcom,msm_fastrpc_compute_cb13 "sdsprpc-smd", iommus <&apps_smmu 0x541 0> */
#define RPC_ADDRESSES           0x80000000ull   /* its qcom,iommu-dma-addr-pool */
#define RPC_STREAM_BITS         ((uint64_t)(RPC_STREAM & 0xf) << 32)    /* (fastrpc.c: buf->phys += sid << 32) */
#define RPC_THIRDS              2               /* tables of the third level, 2 MB of addresses each */
#define RPC_WINDOW              (RPC_THIRDS * 0x200000u)
#define RPC_MESSAGE_BYTES       (512u << 10)    /* one call's arguments (one call at a time) */
#define RPC_REQUEST_BYTES       4096u           /* a reverse call's inputs, as next2 hands them over */
#define RPC_REPLY_BYTES         (256u << 10)    /* its outputs, as the next next2 takes them */
#define RPC_PID                 0x4b40          /* the "process" we are to the DSP (Linux: the caller's tgid) */
#define RPC_SENSORS_PD          2               /* fastrpc.c SENSORS_PD, what INIT_ATTACH_SNS attaches to */
#define RPC_INIT_HANDLE         1
#define RPC_LISTENER_HANDLE     3               /* adsp_listener (hexagonrpcd/interface/adsp_listener.h) */
#define RPC_ALIGN               128
#define RPC_ARGS_MAX            16
#define RPC_SCALARS(method, in, out)    ((uint32_t)((method) & 0x1f) << 24 | (uint32_t)((in) & 0xff) << 16 | \
	                                 (uint32_t)((out) & 0xff) << 8)
#define RPC_METHOD(sc)          ((sc) >> 24 & 0x1f)
#define RPC_INBUFS(sc)          ((sc) >> 16 & 0xff)
#define RPC_OUTBUFS(sc)         ((sc) >> 8 & 0xff)
#define AEE_EFAILED             1
#define AEE_ENOMEMORY           2
#define AEE_EBADPARM            14
#define AEE_EUNSUPPORTED        20
#define AEE_ENOSUCH             39

/* -- the DSP's view of our memory: stream 0x541 of the SMMU, as hakios.sound.c does it for 0x1801 -- */

static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };      /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the sensors' calls' context", 0, 0x1000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
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
#define RPC_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.sound.c's */

extern lck_mtx_t hakios_smmu_lock;             /* (hakios.wifi.c) the SMMU's match registers and contexts, for all of us */

static struct {
	bool found, on, passed, reclaimed;      /* passed: the boot chain lets the stream through untranslated */
	uint32_t page, pages, match, context, old_smr, old_s2cr;
	uint32_t old_cbar, old_cba2r, old_sctlr, old_tcr2, old_tcr;
	uint32_t old_ttbr0_low, old_ttbr0_high, old_ttbr1_low, old_ttbr1_high, old_mair0, old_mair1;
	vm_offset_t tables;                     /* 4 KB each: the root, one of the second level, RPC_THIRDS of the third */
	uint32_t given;                         /* bytes of addresses given out, from RPC_ADDRESSES on */
} rpc_space;

static void
rpc_context_save(void)
{
	rpc_space.old_cbar = get(&apps_smmu_second, "cbar", SMMU_CBAR(rpc_space.context));
	rpc_space.old_cba2r = get(&apps_smmu_second, "cba2r", SMMU_CBA2R(rpc_space.context));
	rpc_space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	rpc_space.old_tcr2 = get(&apps_smmu_context, "tcr2", SMMU_CB_TCR2);
	rpc_space.old_tcr = get(&apps_smmu_context, "tcr", SMMU_CB_TCR);
	rpc_space.old_ttbr0_low = get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0);
	rpc_space.old_ttbr0_high = get(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4);
	rpc_space.old_ttbr1_low = get(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1);
	rpc_space.old_ttbr1_high = get(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4);
	rpc_space.old_mair0 = get(&apps_smmu_context, "mair0", SMMU_CB_MAIR0);
	rpc_space.old_mair1 = get(&apps_smmu_context, "mair1", SMMU_CB_MAIR1);
}

static bool
rpc_tlb_sync(void)
{
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, rpc_space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			printf("slpi: rpc: the SMMU does not finish forgetting translations\n");
			return false;
		}
		sleep_ms(1);
	}
	return true;
}

static void
rpc_context_restore(void)
{
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	(void)rpc_tlb_sync();
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, rpc_space.old_tcr2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, rpc_space.old_tcr);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, rpc_space.old_ttbr0_low);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, rpc_space.old_ttbr0_high);
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, rpc_space.old_ttbr1_low);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, rpc_space.old_ttbr1_high);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, rpc_space.old_mair0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, rpc_space.old_mair1);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(rpc_space.context), rpc_space.old_cba2r);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(rpc_space.context), rpc_space.old_cbar);
	put(&apps_smmu, "s2cr", SMMU_S2CR(rpc_space.match), rpc_space.old_s2cr);
	put(&apps_smmu, "smr", SMMU_SMR(rpc_space.match), rpc_space.old_smr);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, rpc_space.old_sctlr);
}

/* A free match register and context for the stream (or the boot chain's own setting of it). With hakios_smmu_lock held. */
static bool
rpc_space_find(void)
{
	bool used[256] = { false };
	uint32_t context_users[256] = { 0 }, covered = 0, covered_match = ~0u, covered_smr = 0, covered_s2cr = 0;
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		printf("slpi: rpc: this SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	rpc_space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	rpc_space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * rpc_space.pages * rpc_space.page != 0x100000) {
		printf("slpi: rpc: the SMMU says it has 2 x %u pages of %#x bytes, not a megabyte\n", rpc_space.pages, rpc_space.page);
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + rpc_space.page;
	rpc_space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((s2cr >> 16 & 3) == 0) {
				context_users[s2cr & 0xff]++;
			}
			if (((stream ^ RPC_STREAM) & ~mask) == 0) {
				printf("slpi: rpc: match register %u covers the stream already: stream %#x mask %#x, s2cr %#x\n", n, stream, mask,
				    s2cr);
				covered++;
				covered_match = n;
				covered_smr = smr;
				covered_s2cr = s2cr;
			}
		} else if (rpc_space.match == ~0u) {
			rpc_space.match = n;
			rpc_space.old_smr = smr;
			rpc_space.old_s2cr = s2cr;
		}
	}
	if (covered) {
		uint32_t kind = covered_s2cr >> 16 & 3, context = covered_s2cr & 0xff;

		if (covered != 1) {
			printf("slpi: rpc: %u match registers cover the stream: ambiguous, left alone\n", covered);
			return false;
		}
		if (kind == 1) {
			rpc_space.passed = true;
			return true;
		}
		/* (a kernel before this one, on its way back after a panic, may have left ours set) */
		if (kind != 0 || (covered_smr & 0x7fff) != RPC_STREAM || (covered_smr >> 16 & 0x7fff) != 0 || context >= contexts ||
		    context_users[context] != 1) {
			printf("slpi: rpc: the existing match is not one translated stream of its own: left alone\n");
			return false;
		}
		rpc_space.match = covered_match;
		rpc_space.context = context;
		rpc_space.old_smr = covered_smr;
		rpc_space.old_s2cr = covered_s2cr;
		apps_smmu_context.phys = apps_smmu.phys + (rpc_space.pages + rpc_space.context) * rpc_space.page;
		apps_smmu_context.base = 0;
		printf("slpi: rpc: the stream's match register %u and context %u are taken over, to be put back as found\n",
		    rpc_space.match, rpc_space.context);
		rpc_context_save();
		rpc_space.reclaimed = rpc_space.found = true;
		return true;
	}
	for (rpc_space.context = 0; rpc_space.context < contexts && used[rpc_space.context]; rpc_space.context++) {
	}
	if (rpc_space.match == ~0u || rpc_space.context >= contexts) {
		printf("slpi: rpc: the SMMU has no match register or no context left\n");
		return false;
	}
	apps_smmu_context.phys = apps_smmu.phys + (rpc_space.pages + rpc_space.context) * rpc_space.page;
	apps_smmu_context.base = 0;
	printf("slpi: rpc: the boot chain has set nothing up for stream %#x; free: match register %u (smr %#x, s2cr %#x) and context "
	    "%u, at %#x\n", RPC_STREAM, rpc_space.match, rpc_space.old_smr, rpc_space.old_s2cr, rpc_space.context, apps_smmu_context.phys);
	rpc_context_save();
	if (rpc_space.old_sctlr & 1) {
		printf("slpi: rpc: CONTEXT %u IS SWITCHED ON although no stream goes through it: somebody's, left alone\n", rpc_space.context);
		return false;
	}
	rpc_space.found = true;
	return true;
}

/* Tables of ours (nothing mapped yet) behind the stream's addresses from RPC_ADDRESSES on. With hakios_smmu_lock held. */
static bool
rpc_space_on(void)
{
	uint64_t *root, *second, table_root;
	uint32_t smr = SMMU_SMR(rpc_space.match), s2cr = SMMU_S2CR(rpc_space.match), sees = 1u << 31 | RPC_STREAM;

	if (rpc_space.tables == 0 && kmem_alloc(kernel_map, &rpc_space.tables, (2 + RPC_THIRDS) * 4096, KMA_KOBJECT | KMA_ZERO |
	    KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		rpc_space.tables = 0;
		return false;
	}
	bzero((void *)rpc_space.tables, (2 + RPC_THIRDS) * 4096);
	root = (uint64_t *)rpc_space.tables;
	second = (uint64_t *)(rpc_space.tables + 4096);
	root[RPC_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (unsigned t = 0; t < RPC_THIRDS; t++) {
		second[(RPC_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys(rpc_space.tables + 8192 + 4096 * t) | 3;
	}
	for (vm_offset_t line = rpc_space.tables; line < rpc_space.tables + (2 + RPC_THIRDS) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	rpc_space.given = 0;
	table_root = kvtophys(rpc_space.tables) | (uint64_t)rpc_space.context << 48;
	printf("slpi: rpc: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) for %u MB of addresses at "
	    "%#llx, and match register %u sends stream %#x through it\n", rpc_space.context, kvtophys(rpc_space.tables),
	    RPC_WINDOW >> 20, RPC_ADDRESSES, rpc_space.match, RPC_STREAM);
	hold();
	rpc_space.on = true;            /* (even a setup that fails is undone) */
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(rpc_space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(rpc_space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	if (!rpc_tlb_sync()) {
		return false;
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	put(&apps_smmu, "s2cr", s2cr, rpc_space.context);
	put(&apps_smmu, "smr", smr, sees);
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != rpc_space.context ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1)) {
		printf("slpi: rpc: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x does not go through context %u\n", RPC_STREAM,
		    rpc_space.context);
		return false;
	}
	printf("slpi: rpc: THE SENSORS' CALLS' STREAM GOES THROUGH CONTEXT %u of the SMMU, with our tables\n", rpc_space.context);
	return true;
}

/* `bytes` of ours at `memory` (whole 4 KB pages) where the DSP can reach them: their address to it, 0 if there is no room. */
static uint64_t
rpc_space_map(vm_offset_t memory, uint32_t bytes)
{
	uint64_t *third = (uint64_t *)(rpc_space.tables + 8192);
	uint32_t pages = (bytes + 4095) / 4096, first = rpc_space.given / 4096;
	uint64_t address;

	if (rpc_space.passed) {
		for (uint32_t page = 1; page < pages; page++) {
			if (kvtophys(memory + 4096 * page) != kvtophys(memory) + 4096 * page) {
				printf("slpi: rpc: %u bytes that are not one piece of memory, for an untranslated stream\n", bytes);
				return 0;
			}
		}
		return kvtophys(memory) | RPC_STREAM_BITS;
	}
	if (!rpc_space.on || rpc_space.given + 4096ull * pages > RPC_WINDOW) {
		printf("slpi: rpc: no room for %u more bytes in the DSP's addresses (%u given out)\n", bytes, rpc_space.given);
		return 0;
	}
	for (uint32_t page = 0; page < pages; page++) {
		third[first + page] = kvtophys(memory + 4096 * page) | RPC_PTE;
	}
	for (vm_offset_t line = (vm_offset_t)&third[first] & ~63ul; line < (vm_offset_t)&third[first + pages]; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	(void)rpc_tlb_sync();
	address = RPC_ADDRESSES + rpc_space.given;
	rpc_space.given += 4096 * pages;
	return address | RPC_STREAM_BITS;
}

/* After the DSP is down: the stream and the context as they were found, and what the SMMU refused said. */
static void
rpc_space_release(void)
{
	uint32_t status, global, s2cr;

	lck_mtx_lock(&hakios_smmu_lock);
	if (rpc_space.on) {
		status = rd(apps_smmu_context.base + SMMU_CB_FSR);
		global = rd(apps_smmu.base + SMMU_SGFSR);
		if ((status & SMMU_FSR_ALL) != 0 || global != 0) {
			printf("slpi: rpc: THE SMMU HAS REFUSED THE DSP A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the "
			    "SMMU as a whole: status %#x, stream %#x\n", rpc_space.context, status, (uint64_t)rd(apps_smmu_context.base +
			    SMMU_CB_FAR + 4) << 32 | rd(apps_smmu_context.base + SMMU_CB_FAR), rd(apps_smmu_context.base + SMMU_CB_FSYNR0),
			    global, rd(apps_smmu.base + SMMU_SGFSYNR1));
		}
		printf("slpi: rpc: the stream and context %u of the SMMU are put back as they were found\n", rpc_space.context);
		s2cr = rd(apps_smmu.base + SMMU_S2CR(rpc_space.match));
		put(&apps_smmu, "s2cr (blocked while it is restored)", SMMU_S2CR(rpc_space.match), (s2cr & ~0x300ffu) | 2u << 16 |
		    rpc_space.context);
		rpc_context_restore();
		rpc_space.on = false;
	}
	rpc_space.found = rpc_space.passed = rpc_space.reclaimed = false;
	rpc_space.given = 0;
	lck_mtx_unlock(&hakios_smmu_lock);
}

/* -- calls to the DSP (fastrpc.c: fastrpc_get_args(), fastrpc_invoke_send(), fastrpc_put_args()) -- */

struct rpc_message {                    /* struct fastrpc_msg */
	int32_t pid, tid;
	uint64_t context;
	uint32_t handle, scalars;
	uint64_t address, size;
};
struct rpc_answer {                     /* struct fastrpc_invoke_rsp */
	uint64_t context;
	int32_t result;
} __attribute__((packed));
struct rpc_remote_buf { uint64_t pv, length; };
struct rpc_invoke_buf { uint32_t num, page_index; };
struct rpc_page { uint64_t address, size; };
struct rpc_arg {
	void *data;                     /* an input's bytes are taken from here, an output's put here */
	uint32_t length;
};

static vm_offset_t rpc_buffer;          /* RPC_MESSAGE_BYTES where a call's arguments are put */
static uint64_t rpc_buffer_address;     /* the DSP's address for it */
static LCK_MTX_DECLARE(rpc_lock, &slpi_lock_group);     /* one call at a time */
static volatile uint64_t rpc_waiting_for;               /* the context of the call that waits for its answer */
static volatile bool rpc_answered, rpc_gone;
static volatile int32_t rpc_result;
static uint32_t rpc_serial, rpc_strays;
static struct {
	uint32_t calls, served, failed, unknown;
} rpc_count;

/* An answer from the DSP (the link's thread). */
static void
rpc_input(const uint8_t *data, uint32_t length)
{
	struct rpc_answer answer;

	if (length < sizeof(answer)) {
		printf("slpi: rpc: %u bytes on the channel, too few for an answer\n", length);
		return;
	}
	memcpy(&answer, data, sizeof(answer));
	/* (fastrpc.c matches the context's bits 4-11 only: FASTRPC_CTXID_MASK) */
	if (rpc_waiting_for != 0 && (answer.context & 0xff0) == (rpc_waiting_for & 0xff0) && !rpc_answered) {
		rpc_result = answer.result;
		__asm__ volatile ("dmb ish" ::: "memory");
		rpc_answered = true;
		thread_wakeup((event_t)&rpc_answered);
	} else if (rpc_strays++ < 8) {
		printf("slpi: rpc: an answer for context %#llx (%d), which no call waits for (%#llx)\n", answer.context, answer.result,
		    rpc_waiting_for);
	}
}

/* No more answers: the DSP is going (link_stop()); a call that waits gives up. */
static void
rpc_end(void)
{
	rpc_gone = true;
	thread_wakeup((event_t)&rpc_answered);
}

static void
rpc_clean(vm_offset_t from, uint32_t bytes)
{
	for (vm_offset_t line = from & ~63ul; line < from + bytes; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));        /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
}

static void
rpc_discard(vm_offset_t from, uint32_t bytes)
{
	for (vm_offset_t line = from & ~63ul; line < from + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));       /* what the DSP wrote, not what our caches kept */
	}
	__asm__ volatile ("dsb sy");
}

/*
 * One call: method and buffers in `scalars`, the inputs first in `args`, then the outputs. kernel: one of the
 * driver's own (attach, a mapping), with process 0, as fastrpc.c makes them. Returns an errno for the call
 * not made or not answered; the DSP's result in *result.
 */
static int
rpc_call(uint32_t handle, uint32_t scalars, struct rpc_arg *args, bool kernel, int32_t *result)
{
	unsigned n = RPC_INBUFS(scalars) + RPC_OUTBUFS(scalars), in = RPC_INBUFS(scalars);
	uint32_t meta = (uint32_t)(sizeof(struct rpc_remote_buf) + sizeof(struct rpc_invoke_buf) + sizeof(struct rpc_page)) * n +
	    8 * 16 + 4 * 64;            /* (fastrpc_get_meta_size(): FASTRPC_MAX_FDLIST, FASTRPC_MAX_CRCLIST) */
	uint32_t at = meta, offset[RPC_ARGS_MAX];
	struct rpc_remote_buf *remote = (struct rpc_remote_buf *)rpc_buffer;
	struct rpc_invoke_buf *list = (struct rpc_invoke_buf *)(remote + n);
	struct rpc_page *pages = (struct rpc_page *)(list + n);
	struct rpc_message message;
	int error;

	if (n > RPC_ARGS_MAX || rpc_buffer == 0) {
		return EINVAL;
	}
	for (unsigned i = 0; i < n; i++) {
		if (args[i].length != 0) {
			at = (at + RPC_ALIGN - 1) & ~(RPC_ALIGN - 1);
		}
		offset[i] = at;
		at += args[i].length;
	}
	if (at > RPC_MESSAGE_BYTES) {
		printf("slpi: rpc: a call of %u bytes, more than the %u there is room for\n", at, RPC_MESSAGE_BYTES);
		return E2BIG;
	}
	lck_mtx_lock(&rpc_lock);
	if (rpc_gone) {
		lck_mtx_unlock(&rpc_lock);
		return ENOTCONN;
	}
	bzero((void *)rpc_buffer, meta);
	for (unsigned i = 0; i < n; i++) {
		uint64_t first = (rpc_buffer_address + offset[i]) & ~0xfffull;
		uint64_t last = (rpc_buffer_address + offset[i] + (args[i].length ? args[i].length - 1 : 0)) & ~0xfffull;

		remote[i].pv = rpc_buffer + offset[i];          /* (fastrpc.c: the kernel's address; its page offset is the DSP's) */
		remote[i].length = args[i].length;
		list[i].num = args[i].length ? 1 : 0;
		list[i].page_index = i;
		if (args[i].length != 0) {
			pages[i].address = first;
			pages[i].size = last - first + 4096;
			if (i < in) {
				memcpy((void *)(rpc_buffer + offset[i]), args[i].data, args[i].length);
			}
		}
	}
	rpc_clean(rpc_buffer, at);
	bzero(&message, sizeof(message));
	message.pid = kernel ? 0 : RPC_PID;
	message.tid = RPC_PID;
	rpc_serial = rpc_serial % 255 + 1;
	message.context = (uint64_t)rpc_serial << 4 | RPC_SENSORS_PD;
	message.handle = handle;
	message.scalars = scalars;
	message.address = n ? rpc_buffer_address : 0;
	message.size = n ? (at + 4095) & ~4095u : 0;
	rpc_answered = false;
	rpc_waiting_for = message.context;
	rpc_count.calls++;
	error = glink_send(&glink.channel[CHANNEL_FASTRPC], &message, sizeof(message));
	while (error == 0) {
		assert_wait_timeout((event_t)&rpc_answered, THREAD_UNINT, 200, NSEC_PER_MSEC);
		if (rpc_answered || rpc_gone) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
	rpc_waiting_for = 0;
	if (error == 0 && !rpc_answered) {
		error = ENOTCONN;
	}
	if (error == 0) {
		*result = rpc_result;
		rpc_discard(rpc_buffer, at);
		for (unsigned i = in; i < n; i++) {
			uint32_t length = remote[i].length < args[i].length ? (uint32_t)remote[i].length : args[i].length;

			memcpy(args[i].data, (void *)(rpc_buffer + offset[i]), length);
			if (length < args[i].length) {
				bzero((uint8_t *)args[i].data + length, args[i].length - length);
			}
		}
	}
	lck_mtx_unlock(&rpc_lock);
	return error;
}

/* -- the files the DSP reads: fw/sensors/ of the small image, as the DSP names them (hexagonrpcd's rpcd_builder.c) -- */

#define RPC_FILES_ROOT          "/redwood/fw/sensors"
#define RPC_FILES               32

static struct rpc_file {
	bool used, directory;
	char path[200];                 /* ours */
	off_t offset, size;
	char *names;                    /* a directory: its entries, one after another with a 0 after each */
	uint32_t names_size, next;
} rpc_files[RPC_FILES];

/* The DSP's name for a file (relative ones to `dir`) as ours; false for one that would leave fw/sensors. */
static bool
rpc_our_path(const char *theirs, const char *dir, char *ours, size_t size)
{
	char joined[256];
	const char *p = joined;
	size_t length;

	if (theirs[0] == '/') {
		strlcpy(joined, theirs, sizeof(joined));
	} else {
		snprintf(joined, sizeof(joined), "%s/%s", dir, theirs);
	}
	if (strncmp(p, "/system/vendor/", 15) == 0) {
		p += 7;                         /* (the same vendor/ seen from two places) */
	}
	for (const char *c = p; *c != 0; c++) {
		if (c[0] == '.' && c[1] == '.' && (c == p || c[-1] == '/') && (c[2] == 0 || c[2] == '/')) {
			return false;
		}
	}
	if (strncmp(p, "/persist/", 9) == 0) {
		snprintf(ours, size, "%s/mnt/vendor%s", RPC_FILES_ROOT, p);
	} else {
		snprintf(ours, size, "%s%s", RPC_FILES_ROOT, p);
	}
	for (length = strlen(ours); length > 1 && ours[length - 1] == '/'; length--) {
		ours[length - 1] = 0;
	}
	return true;
}

static int
rpc_file_new(void)
{
	for (int i = 0; i < RPC_FILES; i++) {
		if (!rpc_files[i].used) {
			bzero(&rpc_files[i], sizeof(rpc_files[i]));
			rpc_files[i].used = true;
			return i;
		}
	}
	return -1;
}

static void
rpc_file_close(uint32_t fd)
{
	if (fd < RPC_FILES && rpc_files[fd].used) {
		if (rpc_files[fd].names != NULL) {
			kfree_data(rpc_files[fd].names, rpc_files[fd].names_size);
		}
		bzero(&rpc_files[fd], sizeof(rpc_files[fd]));
	}
}

/* A file of ours opened for the DSP: its number, or -1. */
static int
rpc_open_file(const char *path)
{
	vfs_context_t context = vfs_context_kernel();
	struct vnode_attr attributes;
	vnode_t vnode = NULLVP;
	int fd;

	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return -1;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_isdir(vnode) || vnode_getattr(vnode, &attributes, context) != 0 || (fd = rpc_file_new()) < 0) {
		vnode_put(vnode);
		return -1;
	}
	vnode_put(vnode);
	strlcpy(rpc_files[fd].path, path, sizeof(rpc_files[fd].path));
	rpc_files[fd].size = (off_t)attributes.va_data_size;
	return fd;
}

/* A directory of ours opened for the DSP, its entries read at once: its number, or -1. */
static int
rpc_open_directory(const char *path)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	char *buffer, *names;
	uint32_t used = 0, room = 4096;
	off_t offset = 0;
	int fd;

	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return -1;
	}
	if (!vnode_isdir(vnode) || (fd = rpc_file_new()) < 0) {
		vnode_put(vnode);
		return -1;
	}
	buffer = kalloc_data(4096, Z_WAITOK);
	names = kalloc_data(room, Z_WAITOK | Z_ZERO);
	for (bool done = buffer == NULL || names == NULL; !done;) {
		char uio_buf[UIO_SIZEOF(1)];
		uio_t uio = uio_createwithbuffer(1, offset, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
		int eof = 0, items = 0;
		uint32_t got;

		uio_addiov(uio, CAST_USER_ADDR_T(buffer), 4096);
		if (VNOP_READDIR(vnode, uio, 0, &eof, &items, context) != 0) {
			break;
		}
		got = 4096 - (uint32_t)uio_resid(uio);
		offset = uio_offset(uio);
		done = eof || got == 0;
		for (char *p = buffer; p < buffer + got;) {
			struct dirent *entry = (struct dirent *)p;

			if (entry->d_reclen == 0 || p + entry->d_reclen > buffer + got) {
				done = true;
				break;
			}
			p += entry->d_reclen;
			if (entry->d_namlen == 0 || (entry->d_namlen == 1 && entry->d_name[0] == '.') ||
			    (entry->d_namlen == 2 && entry->d_name[0] == '.' && entry->d_name[1] == '.')) {
				continue;
			}
			if (used + entry->d_namlen + 1 > room) {
				done = true;            /* (4 KB of names: more than any of these directories has) */
				break;
			}
			memcpy(names + used, entry->d_name, entry->d_namlen);
			used += entry->d_namlen + 1;
		}
	}
	vnode_put(vnode);
	if (buffer != NULL) {
		kfree_data(buffer, 4096);
	}
	rpc_files[fd].directory = true;
	rpc_files[fd].names = names;
	rpc_files[fd].names_size = names != NULL ? room : 0;
	rpc_files[fd].size = used;
	strlcpy(rpc_files[fd].path, path, sizeof(rpc_files[fd].path));
	return fd;
}

static int
rpc_read(struct rpc_file *file, uint8_t *to, uint32_t bytes, uint32_t *got)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	int left = 0, error;

	*got = 0;
	if (file->offset >= file->size || bytes == 0) {
		return 0;
	}
	if (bytes > file->size - file->offset) {
		bytes = (uint32_t)(file->size - file->offset);
	}
	if ((error = vnode_lookup(file->path, 0, &vnode, context)) != 0) {
		return error;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)to, (int)bytes, file->offset, UIO_SYSSPACE, 0, vfs_context_ucred(context), &left,
	    vfs_context_proc(context));
	vnode_put(vnode);
	if (error == 0) {
		*got = bytes - (uint32_t)left;
		file->offset += *got;
	}
	return error;
}

/* -- the reverse calls: what the DSP asks, answered (hexagonrpcd's localctl.c, apps_std.c, apps_mem.c) -- */

struct rpc_in {
	const uint8_t *data;
	uint32_t length;
};
struct rpc_out {
	uint8_t *data;
	uint32_t length;
};

static uint8_t rpc_out_space[RPC_REPLY_BYTES];  /* the outputs of the call being answered, before they are put in order */
static uint32_t rpc_out_used;

static uint8_t *
rpc_out_take(struct rpc_out *out, uint32_t length)
{
	if (rpc_out_used + length > sizeof(rpc_out_space)) {
		out->data = NULL;
		out->length = 0;
		return NULL;
	}
	out->data = rpc_out_space + rpc_out_used;
	out->length = length;
	bzero(out->data, length);
	rpc_out_used += (length + 7) & ~7u;
	return out->data;
}

static uint32_t
rpc_word(const struct rpc_in *in, unsigned index)
{
	uint32_t word = 0;

	if (in->length >= 4 * (index + 1)) {
		memcpy(&word, in->data + 4 * index, 4);
	}
	return word;
}

/* A string the DSP sent (with its 0 at the end), or NULL. */
static const char *
rpc_string(const struct rpc_in *in)
{
	return in->length > 0 && in->data[in->length - 1] == 0 ? (const char *)in->data : NULL;
}

static uint32_t rpc_lines;

static void
rpc_note(const char *format, ...) __printflike(1, 2);

static void
rpc_note(const char *format, ...)
{
	char line[300];
	va_list list;

	if (rpc_lines++ >= 400) {
		return;                 /* (the first 400 are what tells how it went) */
	}
	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("slpi: rpc: %s\n", line);
}

/* heap pages the DSP asked for (apps_mem), kept until it is down */
static struct {
	vm_offset_t memory;
	uint32_t bytes;
} rpc_heap[16];

static void
rpc_heap_free(void)
{
	for (unsigned i = 0; i < sizeof(rpc_heap) / sizeof(rpc_heap[0]); i++) {
		if (rpc_heap[i].memory != 0) {
			kmem_free(kernel_map, rpc_heap[i].memory, rpc_heap[i].bytes);
		}
	}
	bzero(rpc_heap, sizeof(rpc_heap));
}

/*
 * One reverse call: handle 0 is ours to open others with (remotectl), 1 apps_std (files), 2 apps_mem (memory).
 * Fills outs (as many as the call has output buffers, each of the size the DSP expects) and gives the result.
 */
static uint32_t
rpc_serve(uint32_t handle, uint32_t scalars, const struct rpc_in *ins, struct rpc_out *outs)
{
	uint32_t method = RPC_METHOD(scalars), n_in = RPC_INBUFS(scalars), n_out = RPC_OUTBUFS(scalars);
	struct rpc_in first = n_in > 0 ? ins[0] : (struct rpc_in){ NULL, 0 };
	char path[200];
	uint8_t *o;

	if (method == 31 && first.length >= 4) {
		method = rpc_word(&first, 0);   /* (a method of 31 and up: its number is the first word) */
		first.data += 4;
		first.length -= 4;
	}
	if (scalars & 0xff) {
		rpc_note("handle %u method %u with handles of its own (%#x): not supported", handle, method, scalars);
		return AEE_EBADPARM;
	}
#define WANT(i, o)      do { if (n_in < (i) || n_out < (o)) { rpc_note("handle %u method %u: %u inputs and %u outputs, " \
	                     "too few", handle, method, n_in, n_out); return AEE_EBADPARM; } } while (0)
	switch (handle << 8 | method) {
	case 0 << 8 | 0: {              /* remotectl open(name, rout handle, rout dlerror, rout error) */
		const char *name;

		WANT(2, 2);
		name = rpc_string(&ins[1]);
		o = rpc_out_take(&outs[0], 8);
		rpc_out_take(&outs[1], rpc_word(&first, 1));
		if (o == NULL || name == NULL) {
			return AEE_EBADPARM;
		}
		uint32_t local = strcmp(name, "apps_std") == 0 ? 1 : strcmp(name, "apps_mem") == 0 ? 2 : 0;
		rpc_note("the DSP opens our \"%s\": %s", name, local ? "there" : "NOT HERE");
		if (local == 0) {
			uint32_t error = (uint32_t)-5;

			memcpy(o + 4, &error, 4);
			return (uint32_t)-5;    /* (as hexagonrpcd's localctl_open()) */
		}
		memcpy(o, &local, 4);
		return 0;
	}
	case 0 << 8 | 1:                /* remotectl close(handle, rout dlerror, rout error) */
		WANT(1, 2);
		rpc_out_take(&outs[0], 4);
		rpc_out_take(&outs[1], rpc_word(&first, 1));
		return 0;
	case 1 << 8 | 0:                /* apps_std fopen(name, mode, rout file) */
	case 1 << 8 | 19: {             /* fopen_with_env(variable, delimiter, name, mode, rout file) */
		const char *name = rpc_string(&ins[method == 0 ? 1 : 3]), *mode = rpc_string(&ins[method == 0 ? 2 : 4]);
		const char *variable = method == 0 ? "" : rpc_string(&ins[1]);
		int fd;

		WANT(method == 0 ? 3 : 5, 1);
		o = rpc_out_take(&outs[0], 4);
		if (o == NULL || name == NULL || mode == NULL || variable == NULL) {
			return AEE_EBADPARM;
		}
		if (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL) {
			rpc_note("open %s (%s) to write: not here", name, mode);
			return AEE_EUNSUPPORTED;
		}
		if (!rpc_our_path(name, strcmp(variable, "ADSP_AVS_CFG_PATH") == 0 ? "/vendor/etc/acdbdata" : "/usr/lib/qcom/adsp", path,
		    sizeof(path)) || (fd = rpc_open_file(path)) < 0) {
			rpc_note("open %s%s%s: NOT THERE", variable[0] ? variable : "", variable[0] ? " " : "", name);
			return AEE_EFAILED;
		}
		rpc_note("open %s: %u bytes, file %d", name, (uint32_t)rpc_files[fd].size, fd);
		memcpy(o, &fd, 4);
		return 0;
	}
	case 1 << 8 | 2:                /* fflush(file) */
		return 0;
	case 1 << 8 | 3:                /* fclose(file) */
		WANT(1, 0);
		rpc_file_close(rpc_word(&first, 0));
		return 0;
	case 1 << 8 | 4: {              /* fread(file, rout buffer, rout read, rout eof) */
		uint32_t fd = rpc_word(&first, 0), want = rpc_word(&first, 1), got = 0;
		uint8_t *data;

		WANT(1, 2);
		o = rpc_out_take(&outs[0], 8);
		data = rpc_out_take(&outs[1], want);
		if (o == NULL || data == NULL || fd >= RPC_FILES || !rpc_files[fd].used || rpc_files[fd].directory) {
			rpc_note("read %u bytes of file %u: %s", want, fd, data == NULL ? "MORE THAN THERE IS ROOM FOR" : "no such file");
			return AEE_EFAILED;
		}
		if (rpc_read(&rpc_files[fd], data, want, &got) != 0) {
			return AEE_EFAILED;
		}
		uint32_t eof = got < want;
		memcpy(o, &got, 4);
		memcpy(o + 4, &eof, 4);
		return 0;
	}
	case 1 << 8 | 8:                /* ftell(file, rout position) */
	case 1 << 8 | 10:               /* flen(file, rout length) */
	case 1 << 8 | 12: {             /* feof(file, rout eof) */
		uint32_t fd = rpc_word(&first, 0);
		uint64_t value;

		WANT(1, 1);
		o = rpc_out_take(&outs[0], method == 10 ? 8 : 4);
		if (o == NULL || fd >= RPC_FILES || !rpc_files[fd].used) {
			return AEE_EFAILED;
		}
		value = method == 8 ? (uint64_t)rpc_files[fd].offset : method == 10 ? (uint64_t)rpc_files[fd].size :
		    rpc_files[fd].offset >= rpc_files[fd].size;
		memcpy(o, &value, method == 10 ? 8 : 4);
		return 0;
	}
	case 1 << 8 | 9: {              /* fseek(file, offset, whence) */
		uint32_t fd = rpc_word(&first, 0), whence = rpc_word(&first, 2);
		int32_t by = (int32_t)rpc_word(&first, 1);
		off_t to;

		WANT(1, 0);
		if (fd >= RPC_FILES || !rpc_files[fd].used || whence > 2) {
			return AEE_EFAILED;
		}
		to = (whence == 0 ? 0 : whence == 1 ? rpc_files[fd].offset : rpc_files[fd].size) + by;
		if (to < 0 || to > rpc_files[fd].size) {
			return AEE_EFAILED;
		}
		rpc_files[fd].offset = to;
		return 0;
	}
	case 1 << 8 | 11:               /* rewind(file) */
		if (rpc_word(&first, 0) < RPC_FILES) {
			rpc_files[rpc_word(&first, 0)].offset = 0;
		}
		return 0;
	case 1 << 8 | 15: {             /* print_string(text): the DSP's word to us, said */
		const char *text = n_in > 1 ? rpc_string(&ins[1]) : NULL;

		rpc_note("the DSP says: %s", text != NULL ? text : "?");
		return 0;
	}
	case 1 << 8 | 26: {             /* opendir(name, rout directory) */
		const char *name = n_in > 1 ? rpc_string(&ins[1]) : NULL;
		int fd;

		WANT(2, 1);
		o = rpc_out_take(&outs[0], 8);
		if (o == NULL || name == NULL) {
			return AEE_EBADPARM;
		}
		if (!rpc_our_path(name, "", path, sizeof(path)) || (fd = rpc_open_directory(path)) < 0) {
			rpc_note("open the directory %s: NOT THERE", name);
			return AEE_EFAILED;
		}
		rpc_note("open the directory %s: %u bytes of names, file %d", name, (uint32_t)rpc_files[fd].size, fd);
		uint64_t dir = (uint64_t)fd;
		memcpy(o, &dir, 8);
		return 0;
	}
	case 1 << 8 | 27: {             /* closedir(directory) */
		uint64_t dir = 0;

		WANT(1, 0);
		memcpy(&dir, first.data, first.length >= 8 ? 8 : 0);
		rpc_file_close((uint32_t)dir);
		return 0;
	}
	case 1 << 8 | 28: {             /* readdir(directory, rout entry, rout eof): the entry is an inode word and 256 bytes of name */
		uint64_t dir = 0;
		struct rpc_file *file;

		WANT(1, 1);
		memcpy(&dir, first.data, first.length >= 8 ? 8 : 0);
		o = rpc_out_take(&outs[0], 4 + 256 + 4);
		if (o == NULL || dir >= RPC_FILES || !(file = &rpc_files[dir])->used || !file->directory) {
			return AEE_EFAILED;
		}
		if (file->names != NULL && file->next < (uint32_t)file->size) {
			const char *name = file->names + file->next;

			strlcpy((char *)o + 4, name, 256);
			file->next += (uint32_t)strlen(name) + 1;
		} else {
			uint32_t eof = 1;

			memcpy(o + 260, &eof, 4);
		}
		return 0;
	}
	case 1 << 8 | 31: {             /* stat(name, rout stat): 12 fields, 96 bytes (apps_std.idl's STAT) */
		const char *name = n_in > 1 ? rpc_string(&ins[1]) : NULL;
		vfs_context_t context = vfs_context_kernel();
		struct vnode_attr attributes;
		vnode_t vnode = NULLVP;
		uint64_t size;
		uint32_t mode;

		WANT(2, 1);
		o = rpc_out_take(&outs[0], 96);
		if (o == NULL || name == NULL) {
			return AEE_EBADPARM;
		}
		if (!rpc_our_path(name, "/usr/lib/qcom/adsp", path, sizeof(path)) || vnode_lookup(path, 0, &vnode, context) != 0) {
			rpc_note("stat %s: NOT THERE", name);
			return AEE_EFAILED;
		}
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		size = vnode_getattr(vnode, &attributes, context) == 0 ? attributes.va_data_size : 0;
		mode = vnode_isdir(vnode) ? S_IFDIR | 0755 : S_IFREG | 0644;
		vnode_put(vnode);
		uint32_t links = 1;
		memcpy(o + 24, &mode, 4);
		memcpy(o + 28, &links, 4);
		memcpy(o + 40, &size, 8);
		rpc_note("stat %s: %llu bytes", name, size);
		return 0;
	}
	case 2 << 8 | 2: {              /* apps_mem request_map64(heap, lflags, rflags, vin, len, rout vapps, rout vadsp) */
		uint32_t rflags = rpc_word(&first, 2);
		uint64_t length = 0, address;
		vm_offset_t memory = 0;
		unsigned slot;

		WANT(1, 1);
		if (first.length >= 32) {
			memcpy(&length, first.data + 24, 8);
		}
		o = rpc_out_take(&outs[0], 16);
		for (slot = 0; slot < sizeof(rpc_heap) / sizeof(rpc_heap[0]) && rpc_heap[slot].memory != 0; slot++) {
		}
		if (o == NULL || !(rflags & 0x1000) || length == 0 || length > (1u << 20) ||
		    slot == sizeof(rpc_heap) / sizeof(rpc_heap[0])) {
			rpc_note("the DSP asks for %llu bytes of memory (flags %#x): NOT GIVEN", length, rflags);
			return AEE_EUNSUPPORTED;
		}
		length = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
		lck_mtx_lock(&hakios_smmu_lock);
		address = kmem_alloc(kernel_map, &memory, (vm_size_t)length, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) ==
		    KERN_SUCCESS ? rpc_space_map(memory, (uint32_t)length) : 0;
		lck_mtx_unlock(&hakios_smmu_lock);
		if (address == 0) {
			if (memory != 0) {
				kmem_free(kernel_map, memory, (vm_size_t)length);
			}
			return AEE_ENOMEMORY;
		}
		rpc_heap[slot].memory = memory;
		rpc_heap[slot].bytes = (uint32_t)length;
		rpc_clean(memory, (uint32_t)length);
		{
			struct {
				int32_t pgid;
				uint32_t flags;
				uint64_t vaddr;
				int32_t num;
				uint32_t pad;
			} request = { RPC_PID, rflags, 0, (int32_t)sizeof(struct rpc_page), 0 };
			struct rpc_page page = { address, length };
			uint64_t theirs = 0;
			struct rpc_arg args[3] = { { &request, 24 }, { &page, sizeof(page) }, { &theirs, 8 } };
			int32_t result = -1;
			int error = rpc_call(RPC_INIT_HANDLE, RPC_SCALARS(4, 2, 1), args, true, &result);   /* FASTRPC_RMID_INIT_MMAP */

			rpc_note("the DSP asks for %llu bytes of memory: ours at %#llx, %s (%d, %d), at %#llx for it", length, address,
			    error == 0 && result == 0 ? "MAPPED" : "NOT MAPPED", error, result, theirs);
			if (error != 0 || result != 0) {
				return AEE_EFAILED;
			}
			uint64_t vapps = 0, vadsp = theirs & 0xffffffffu;
			memcpy(o, &vapps, 8);
			memcpy(o + 8, &vadsp, 8);
		}
		return 0;
	}
	default:
		rpc_count.unknown++;
		rpc_note("handle %u method %u (%u inputs, %u outputs; the first %u bytes): NOT ANSWERED", handle, method, n_in, n_out,
		    first.length);
		for (unsigned i = 0; i < n_out; i++) {
			outs[i].data = NULL;
			outs[i].length = 0;
		}
		return AEE_EUNSUPPORTED;
	}
#undef WANT
}

/* next2's inputs taken apart: per buffer a word of its size, then (if any) its bytes from the next multiple of 8 on. */
static bool
rpc_decode(const uint8_t *from, uint32_t length, unsigned count, struct rpc_in *ins)
{
	uint32_t at = 0;

	for (unsigned i = 0; i < count; i++) {
		uint32_t size;

		if (at + 4 > length) {
			return false;
		}
		memcpy(&size, from + at, 4);
		at += 4;
		if (size != 0) {
			at = (at + 7) & ~7u;
		}
		if (at + size > length) {
			return false;
		}
		ins[i].data = from + at;
		ins[i].length = size;
		at += size;
	}
	return true;
}

/* An answer's outputs put together for next2, the same way (hexagonrpcd's outbufs_encode()). */
static uint32_t
rpc_encode(const struct rpc_out *outs, unsigned count, uint8_t *to, uint32_t room)
{
	uint32_t at = 0;

	for (unsigned i = 0; i < count; i++) {
		uint32_t size = outs[i].data != NULL ? outs[i].length : 0;

		if (at + 4 > room) {
			return 0;
		}
		memcpy(to + at, &size, 4);
		at += 4;
		if (size != 0) {
			uint32_t aligned = (at + 7) & ~7u;

			if (aligned + size > room) {
				return 0;
			}
			bzero(to + at, aligned - at);
			memcpy(to + aligned, outs[i].data, size);
			at = aligned + size;
		}
	}
	return at;
}

static volatile bool rpc_listening;

static void
rpc_listener(__unused void *parameter, __unused wait_result_t wait)
{
	static uint8_t request[RPC_REQUEST_BYTES], reply[RPC_REPLY_BYTES];
	uint32_t rctx = 0, answer = 0xffffffffu, reply_length = 0;
	int32_t result = -1;
	int error;

	{
		struct rpc_arg none[1];

		error = rpc_call(RPC_LISTENER_HANDLE, RPC_SCALARS(3, 0, 0), none, false, &result);      /* adsp_listener init2 */
		printf("slpi: rpc: the listener's init2: %s (%d, %d)\n", error == 0 && result == 0 ? "DONE" : "FAILED", error, result);
	}
	while (error == 0 && result == 0 && !rpc_gone) {
		struct {
			uint32_t rctx, result, reply_length, request_room;
		} in0 = { rctx, answer, reply_length, RPC_REQUEST_BYTES };
		struct {
			uint32_t rctx, handle, scalars, request_length;
		} out0 = { 0, 0, 0, 0 };
		struct rpc_arg args[4] = { { &in0, sizeof(in0) }, { reply, reply_length }, { &out0, sizeof(out0) }, { request, sizeof(request) } };
		struct rpc_in ins[RPC_ARGS_MAX];
		struct rpc_out outs[RPC_ARGS_MAX];

		error = rpc_call(RPC_LISTENER_HANDLE, RPC_SCALARS(4, 2, 2), args, false, &result);      /* adsp_listener next2 */
		if (error != 0 || result != 0) {
			if (!rpc_gone) {
				printf("slpi: rpc: THE LISTENER'S NEXT2 FAILED (%d, %d): no more of the DSP's calls are answered\n", error, result);
			}
			break;
		}
		rctx = out0.rctx;
		bzero(ins, sizeof(ins));
		bzero(outs, sizeof(outs));
		rpc_out_used = 0;
		if (RPC_INBUFS(out0.scalars) > RPC_ARGS_MAX || RPC_OUTBUFS(out0.scalars) > RPC_ARGS_MAX ||
		    out0.request_length > sizeof(request) ||
		    !rpc_decode(request, out0.request_length, RPC_INBUFS(out0.scalars), ins)) {
			rpc_note("a call of the DSP's that does not decode: handle %u, scalars %#x, %u bytes", out0.handle, out0.scalars,
			    out0.request_length);
			answer = AEE_EBADPARM;
		} else {
			answer = rpc_serve(out0.handle, out0.scalars, ins, outs);
		}
		rpc_count.served++;
		rpc_count.failed += answer != 0;
		reply_length = rpc_encode(outs, answer == 0 ? RPC_OUTBUFS(out0.scalars) : 0, reply, sizeof(reply));
	}
	rpc_listening = false;
	thread_terminate(current_thread());
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("slpi: link: THE SENSOR DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_FASTRPC) {
				rpc_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	rpc_end();                      /* (a call of ours that waits for an answer, next2 above all, gives up) */
	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("slpi: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; "
		    "its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the sensor DSP has put into its own");
	if (!glink_find()) {
		printf("slpi: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("slpi: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("slpi: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the sensor DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("slpi: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("slpi: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("slpi_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("slpi: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: FastRPC to the sensors' process ------------------------------------------------------- */

static bool
step_rpc(void)
{
	struct glink_channel *channel = &glink.channel[CHANNEL_FASTRPC];
	int32_t result = -1;
	int error;

	announce("FastRPC: the channel \"fastrpcglink-apps-dsp\" (the DSP's opening it waited for, else ours), a buffer of ours behind "
	    "the DSP's stream 0x541 of the SMMU, and our call to attach to its sensors' process (INIT_ATTACH_SNS)");
	rpc_gone = false;
	rpc_lines = 0;
	bzero(&rpc_count, sizeof(rpc_count));
	for (unsigned ms = 0; ms < 3000 && !channel->open; ms++) {
		sleep_ms(1);
	}
	if (!channel->open) {
		uint32_t size = (uint32_t)strlen(channel->name) + 1;
		char name[40];

		printf("slpi: %u: the DSP has not opened \"%s\": we open it\n", step, channel->name);
		bzero(name, sizeof(name));
		strlcpy(name, channel->name, sizeof(name));
		lck_mtx_lock(&glink_lock);
		if (!channel->asked) {
			glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
			channel->asked = true;
		}
		lck_mtx_unlock(&glink_lock);
		for (unsigned ms = 0; ms < 3000 && !channel->open; ms++) {
			sleep_ms(1);
		}
	}
	if (!channel->open) {
		printf("slpi: %u: THE CHANNEL \"%s\" DID NOT OPEN (acknowledged %d, the DSP's number %u)\n", step, channel->name,
		    channel->acknowledged, channel->theirs);
		return false;
	}
	if (rpc_buffer == 0 && kmem_alloc(kernel_map, &rpc_buffer, RPC_MESSAGE_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		rpc_buffer = 0;
		printf("slpi: %u: no memory for the calls' buffer\n", step);
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (!rpc_space.found && !rpc_space.passed && !rpc_space_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (!rpc_space.passed && !rpc_space.on && !rpc_space_on()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		rpc_space_release();
		return false;
	}
	rpc_buffer_address = rpc_space_map(rpc_buffer, RPC_MESSAGE_BYTES);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (rpc_buffer_address == 0) {
		return false;
	}
	printf("slpi: %u: the calls' buffer: %u KB, at %#llx for the DSP%s\n", step, RPC_MESSAGE_BYTES >> 10, rpc_buffer_address,
	    rpc_space.passed ? " (the stream goes untranslated)" : "");
	{
		int32_t pid = RPC_PID;
		struct rpc_arg args[1] = { { &pid, sizeof(pid) } };

		error = rpc_call(RPC_INIT_HANDLE, RPC_SCALARS(0, 1, 0), args, true, &result);   /* FASTRPC_RMID_INIT_ATTACH */
	}
	printf("slpi: %u: attach to the sensors' process: %s (%d, the DSP's result %d)\n", step, error == 0 && result == 0 ?
	    "ATTACHED" : "NOT ATTACHED", error, result);
	return error == 0 && result == 0;
}

/* ---- step 12: the DSP's calls to us, answered ------------------------------------------------------- */

/* Ours to the DSP's remotectl (handle 0): open(name, rout handle, rout dlerror, rout error). */
static bool
rpc_remote_open(const char *name, uint32_t *handle)
{
	struct {
		uint32_t name_length, dlerror_length;
	} in0 = { (uint32_t)strlen(name) + 1, 256 };
	struct {
		uint32_t handle, error;
	} out0 = { 0, 0 };
	char dlerror[256];
	struct rpc_arg args[4] = { { &in0, sizeof(in0) }, { (void *)(uintptr_t)name, in0.name_length }, { &out0, sizeof(out0) },
		                   { dlerror, sizeof(dlerror) } };
	int32_t result = -1;
	int error = rpc_call(0, RPC_SCALARS(0, 2, 2), args, false, &result);

	dlerror[sizeof(dlerror) - 1] = 0;
	printf("slpi: %u: the DSP's \"%s\": %s (%d, %d, %#x%s%s)\n", step, name, error == 0 && result == 0 && out0.error == 0 ?
	    "OPENED" : "NOT OPENED", error, result, out0.error, dlerror[0] ? ": " : "", dlerror);
	*handle = out0.handle;
	return error == 0 && result == 0 && out0.error == 0;
}

static void
rpc_remote_close(uint32_t handle)
{
	struct {
		uint32_t handle, dlerror_length;
	} in0 = { handle, 256 };
	uint32_t out0 = 0;
	char dlerror[256];
	struct rpc_arg args[3] = { { &in0, sizeof(in0) }, { &out0, sizeof(out0) }, { dlerror, sizeof(dlerror) } };
	int32_t result = -1;

	(void)rpc_call(0, RPC_SCALARS(1, 1, 2), args, false, &result);
}

static bool
step_listen(void)
{
	struct rpc_arg none[1];
	uint32_t listener = 0, seconds = 8;
	int32_t result = -1;
	thread_t thread;
	int error;

	announce("FastRPC: register us as the DSP's default listener (adsp_default_listener), then answer its calls -- the "
	    "configuration it reads -- from a thread of ours, from here on");
	if (!rpc_remote_open("adsp_default_listener", &listener)) {
		return false;
	}
	error = rpc_call(listener, RPC_SCALARS(0, 0, 0), none, false, &result);   /* adsp_default_listener register */
	printf("slpi: %u: registered as its listener: %s (%d, %d)\n", step, error == 0 && result == 0 ? "YES" : "NO", error, result);
	rpc_remote_close(listener);
	if (error != 0 || result != 0) {
		return false;
	}
	rpc_listening = true;
	if (kernel_thread_start(rpc_listener, NULL, &thread) != KERN_SUCCESS) {
		rpc_listening = false;
		printf("slpi: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	/* boot-arg slpi_rpc_wait=N: how long the step looks on (8 s), counting the DSP's calls and services */
	PE_parse_boot_argn("slpi_rpc_wait", &seconds, sizeof(seconds));
	for (unsigned s = 1; s <= seconds && rpc_listening; s++) {
		unsigned services = 0;

		sleep_ms(1000);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		printf("slpi: %u: %u s on: %u calls of ours, %u of the DSP's answered (%u with an error, %u not known here); %u services\n",
		    step, s, rpc_count.calls, rpc_count.served, rpc_count.failed, rpc_count.unknown, services);
	}
	return rpc_listening;
}

/* ---- step 13: the sensors themselves, the ambient light first --------------------------------------- */

/*
 * The sensors' service (SEE's "sensor client", QMI service 400 over QRTR): one request message, SNS_CLIENT_REQ
 * (0x20), whose one field is a protocol buffer, sns_client_request_msg, for a sensor named by its SUID; what
 * comes back are indications (0x21, 0x22 the long ones) carrying sns_client_event_msg buffers. A sensor's SUID
 * is asked of the "lookup" sensor (SUID 0xabab...ab) by its data type (msg 512, sns_suid_req; the answer msg
 * 768, sns_suid_event); an on-change sensor such as the light's is switched on by msg 514, a streaming one by
 * msg 513 (sns_std_sensor_config: a rate); its samples come as msg 1025, sns_std_sensor_event's floats -- the
 * light's first one is lux. (Qualcomm's sns_client.proto, sns_std.proto, sns_suid.proto, as libssc has them.)
 * What it reads: sysctl kern.hakios.als_millilux (and als_events, how many), for the system's light sensor.
 */
#define SEE_SERVICE             400
#define SEE_REQUEST             0x20
#define SEE_REPORT              0x21
#define SEE_JUMBO_REPORT        0x22
#define SEE_SUID_LOOKUP         0xababababababababull
#define SEE_MSG_SUID_REQUEST    512
#define SEE_MSG_ON_CHANGE       514
#define SEE_MSG_SUID_EVENT      768
#define SEE_MSG_SENSOR_EVENT    1025

SYSCTL_DECL(_kern_hakios);
static int see_millilux = -1, see_events;
SYSCTL_INT(_kern_hakios, OID_AUTO, als_millilux, CTLFLAG_RD | CTLFLAG_LOCKED, &see_millilux, 0,
    "the ambient light, in thousandths of a lux (the sensor DSP's; -1: none yet)");
SYSCTL_INT(_kern_hakios, OID_AUTO, als_events, CTLFLAG_RD | CTLFLAG_LOCKED, &see_events, 0, "how many readings of it so far");

static struct {
	uint32_t node, port;            /* the service's */
	uint16_t transaction;
	volatile bool answered;
	volatile uint32_t result, error;
	uint64_t client;                /* its number for us */
	uint64_t suid[4][2];            /* the light sensors it named */
	volatile uint32_t suids;
	uint32_t reports, shown;
} see;

/* protocol buffers, the few kinds these messages have */
struct pb {
	uint8_t *data;
	uint32_t at, room;
	bool full;
};

static void
pb_byte(struct pb *b, uint8_t value)
{
	if (b->at < b->room) {
		b->data[b->at++] = value;
	} else {
		b->full = true;
	}
}

static void
pb_varint(struct pb *b, uint64_t value)
{
	do {
		pb_byte(b, (uint8_t)((value & 0x7f) | (value > 0x7f ? 0x80 : 0)));
		value >>= 7;
	} while (value != 0);
}

static void
pb_fixed(struct pb *b, unsigned field, uint64_t value, unsigned bytes)
{
	pb_varint(b, (uint64_t)field << 3 | (bytes == 8 ? 1 : 5));
	for (unsigned i = 0; i < bytes; i++) {
		pb_byte(b, (uint8_t)(value >> (8 * i)));
	}
}

static void
pb_bytes(struct pb *b, unsigned field, const void *data, uint32_t length)
{
	pb_varint(b, (uint64_t)field << 3 | 2);
	pb_varint(b, length);
	for (uint32_t i = 0; i < length; i++) {
		pb_byte(b, ((const uint8_t *)data)[i]);
	}
}

static void
pb_bool(struct pb *b, unsigned field, bool value)
{
	pb_varint(b, (uint64_t)field << 3);
	pb_varint(b, value);
}

struct pb_field {
	unsigned number, wire;
	uint64_t value;                 /* a varint or fixed one */
	const uint8_t *data;            /* a length-delimited one */
	uint32_t length;
};

/* The next field of a buffer (at *at): false at its end or where it does not parse. */
static bool
pb_next(const uint8_t *data, uint32_t length, uint32_t *at, struct pb_field *f)
{
	uint64_t key = 0, value = 0;
	unsigned shift = 0;

	if (*at >= length) {
		return false;
	}
	for (;;) {
		if (*at >= length || shift > 63) {
			return false;
		}
		key |= (uint64_t)(data[*at] & 0x7f) << shift;
		shift += 7;
		if (!(data[(*at)++] & 0x80)) {
			break;
		}
	}
	f->number = (unsigned)(key >> 3);
	f->wire = (unsigned)(key & 7);
	f->data = NULL;
	f->length = 0;
	switch (f->wire) {
	case 0:
	case 2:
		shift = 0;
		for (;;) {
			if (*at >= length || shift > 63) {
				return false;
			}
			value |= (uint64_t)(data[*at] & 0x7f) << shift;
			shift += 7;
			if (!(data[(*at)++] & 0x80)) {
				break;
			}
		}
		if (f->wire == 2) {
			if (value > length - *at) {
				return false;
			}
			f->data = data + *at;
			f->length = (uint32_t)value;
			*at += (uint32_t)value;
		}
		f->value = value;
		return true;
	case 1:
	case 5: {
		unsigned bytes = f->wire == 1 ? 8 : 4;

		if (length - *at < bytes) {
			return false;
		}
		for (unsigned i = 0; i < bytes; i++) {
			value |= (uint64_t)data[*at + i] << (8 * i);
		}
		*at += bytes;
		f->value = value;
		return true;
	}
	default:
		return false;
	}
}

/* One request to a sensor: msg_id with `payload` (a buffer of its own, or none), for the AP's processor, woken. */
static int
see_request(uint64_t suid_low, uint64_t suid_high, uint32_t msg_id, const uint8_t *payload, uint32_t payload_length)
{
	uint8_t suid[32], suspend[16], request[256], message[400], packet[420];
	struct pb s = { suid, 0, sizeof(suid), false }, p = { suspend, 0, sizeof(suspend), false };
	struct pb r = { request, 0, sizeof(request), false }, m = { message, 0, sizeof(message), false };
	uint32_t at = 0;

	pb_fixed(&s, 1, suid_low, 8);
	pb_fixed(&s, 2, suid_high, 8);
	pb_varint(&p, 1 << 3);
	pb_varint(&p, 1);               /* client_proc_type: SNS_STD_CLIENT_PROCESSOR_APSS */
	pb_varint(&p, 2 << 3);
	pb_varint(&p, 0);               /* delivery_type: SNS_CLIENT_DELIVERY_WAKEUP */
	if (payload_length != 0) {
		pb_bytes(&r, 2, payload, payload_length);
	}
	pb_bytes(&m, 1, suid, s.at);
	pb_fixed(&m, 2, msg_id, 4);
	pb_bytes(&m, 3, suspend, p.at);
	pb_bytes(&m, 4, request, r.at);
	if (s.full || p.full || r.full || m.full || m.at > 1000) {
		return EMSGSIZE;
	}
	/* QMI: type 0 (a request), transaction, message 0x20, length; TLV 1: the buffer, with a 16-bit length of its own */
	see.transaction = see.transaction % 0xfff0 + 1;
	packet[at++] = 0;
	packet[at++] = (uint8_t)see.transaction;
	packet[at++] = (uint8_t)(see.transaction >> 8);
	packet[at++] = SEE_REQUEST;
	packet[at++] = 0;
	packet[at++] = (uint8_t)(m.at + 5);
	packet[at++] = (uint8_t)((m.at + 5) >> 8);
	packet[at++] = 1;
	packet[at++] = (uint8_t)(m.at + 2);
	packet[at++] = (uint8_t)((m.at + 2) >> 8);
	packet[at++] = (uint8_t)m.at;
	packet[at++] = (uint8_t)(m.at >> 8);
	memcpy(packet + at, message, m.at);
	at += m.at;
	see.answered = false;
	return qrtr_out_packet(QRTR_DATA, SEE_PORT, see.node, see.port, packet, at);
}

/* A light reading's lux (as a float's bits) to thousandths of a lux, without the FPU. */
static int
see_lux_milli(uint32_t bits)
{
	int exponent = (int)(bits >> 23 & 0xff) - 127;
	uint64_t mantissa = (bits & 0x7fffff) | 0x800000, milli;

	if ((bits >> 31) || (bits >> 23 & 0xff) == 0 || exponent < -20) {
		return 0;
	}
	if (exponent > 20) {
		return 2000000000;
	}
	/* value = mantissa * 2^(exponent - 23); times 1000 */
	milli = mantissa * 1000;
	milli = exponent >= 23 ? milli << (exponent - 23) : milli >> (23 - exponent);
	return milli > 2000000000 ? 2000000000 : (int)milli;
}

static void
see_event(const uint8_t *data, uint32_t length)
{
	struct pb_field f, e, g;
	uint32_t at = 0;

	while (pb_next(data, length, &at, &f)) {
		uint32_t msg_id = 0, eat = 0;
		const uint8_t *payload = NULL;
		uint32_t payload_length = 0;

		if (f.number != 2 || f.wire != 2) {
			continue;       /* (field 1: the sensor's SUID) */
		}
		while (pb_next(f.data, f.length, &eat, &e)) {
			if (e.number == 1 && e.wire == 5) {
				msg_id = (uint32_t)e.value;
			} else if (e.number == 3 && e.wire == 2) {
				payload = e.data;
				payload_length = e.length;
			}
		}
		see.reports++;
		if (msg_id == SEE_MSG_SUID_EVENT && payload != NULL) {
			uint32_t pat = 0;

			while (pb_next(payload, payload_length, &pat, &g)) {
				struct pb_field h;
				uint32_t hat = 0;
				uint64_t low = 0, high = 0;

				if (g.number == 1 && g.wire == 2) {
					printf("slpi: see: sensors of the type \"%.*s\":\n", (int)(g.length < 40 ? g.length : 40), g.data);
				}
				if (g.number != 2 || g.wire != 2) {
					continue;
				}
				while (pb_next(g.data, g.length, &hat, &h)) {
					if (h.number == 1) {
						low = h.value;
					} else if (h.number == 2) {
						high = h.value;
					}
				}
				printf("slpi: see:   SUID %016llx%016llx\n", high, low);
				if (see.suids < 4) {
					see.suid[see.suids][0] = low;
					see.suid[see.suids][1] = high;
					see.suids++;
				}
			}
		} else if (msg_id == SEE_MSG_SENSOR_EVENT && payload != NULL) {
			uint32_t pat = 0, first = 0, floats = 0;

			while (pb_next(payload, payload_length, &pat, &g)) {
				if (g.number == 1 && g.wire == 5 && floats++ == 0) {
					first = (uint32_t)g.value;
				} else if (g.number == 1 && g.wire == 2 && g.length >= 4 && floats++ == 0) {
					first = (uint32_t)g.data[0] | (uint32_t)g.data[1] << 8 | (uint32_t)g.data[2] << 16 | (uint32_t)g.data[3] << 24;
				}
			}
			if (floats != 0) {
				see_millilux = see_lux_milli(first);
				see_events++;
				if (see.shown++ < 20) {
					printf("slpi: see: THE LIGHT: %d.%03d lux\n", see_millilux / 1000, see_millilux % 1000);
				}
			}
		} else if (see.shown++ < 20) {
			printf("slpi: see: an event, message %u, %u bytes\n", msg_id, payload_length);
		}
	}
}

/* A QMI message from the sensors' service to our port (the link's thread). */
static void
see_input(const uint8_t *data, uint32_t length)
{
	uint16_t message, total;
	uint32_t at = 7;

	if (length < 7) {
		return;
	}
	message = (uint16_t)(data[3] | data[4] << 8);
	total = (uint16_t)(data[5] | data[6] << 8);
	if (7u + total > length) {
		return;
	}
	if (data[0] == 2 && message == SEE_REQUEST) {   /* the response */
		while (at + 3 <= 7u + total) {
			uint8_t type = data[at];
			uint16_t size = (uint16_t)(data[at + 1] | data[at + 2] << 8);
			const uint8_t *v = data + at + 3;

			if (at + 3 + size > 7u + total) {
				break;
			}
			if (type == 2 && size >= 4) {
				see.result = (uint32_t)(v[0] | v[1] << 8);
				see.error = (uint32_t)(v[2] | v[3] << 8);
			} else if (type == 0x10 && size >= 8) {
				memcpy(&see.client, v, 8);
			}
			at += 3 + size;
		}
		__asm__ volatile ("dmb ish" ::: "memory");
		see.answered = true;
		return;
	}
	if (data[0] == 4 && (message == SEE_REPORT || message == SEE_JUMBO_REPORT)) {
		while (at + 3 <= 7u + total) {
			uint8_t type = data[at];
			uint16_t size = (uint16_t)(data[at + 1] | data[at + 2] << 8);
			const uint8_t *v = data + at + 3;

			if (at + 3 + size > 7u + total) {
				break;
			}
			if (type == 2 && size >= 2) {
				uint16_t inner = (uint16_t)(v[0] | v[1] << 8);

				if (2u + inner <= size) {
					see_event(v + 2, inner);
				}
			}
			at += 3 + size;
		}
	}
}

static bool
see_wait_answer(void)
{
	for (unsigned ms = 0; ms < 3000 && !see.answered; ms++) {
		sleep_ms(1);
	}
	return see.answered && see.result == 0;
}

static bool
step_see(void)
{
	uint8_t lookup[64];
	struct pb q = { lookup, 0, sizeof(lookup), false };
	unsigned found = ~0u;
	int error;

	announce("the sensors' service (QMI 400): the light sensors' SUIDs asked of its lookup, the first switched on (its readings "
	    "are sysctl kern.hakios.als_millilux)");
	for (unsigned waited = 0; waited < 20000 && found == ~0u; waited++) {
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].service == SEE_SERVICE) {
				found = i;
			}
		}
		if (found == ~0u) {
			sleep_ms(1);
		}
	}
	if (found == ~0u) {
		printf("slpi: %u: THE SENSORS' SERVICE (400) DID NOT APPEAR in 20 s\n", step);
		return false;
	}
	see.node = qrtr_servers[found].node;
	see.port = qrtr_servers[found].port;
	see.suids = 0;
	pb_bytes(&q, 1, "ambient_light", 13);
	pb_bool(&q, 2, false);          /* default_only: every one there is */
	pb_bool(&q, 3, true);           /* register_updates */
	error = see_request(SEE_SUID_LOOKUP, SEE_SUID_LOOKUP, SEE_MSG_SUID_REQUEST, lookup, q.at);
	printf("slpi: %u: the lookup for \"ambient_light\", to node %u port %u: %s (%d)\n", step, see.node, see.port, error == 0 ?
	    "sent" : "NOT SENT", error);
	if (error != 0) {
		return false;
	}
	if (!see_wait_answer()) {
		printf("slpi: %u: the service %s (result %u, error %u)\n", step, see.answered ? "REFUSED IT" : "DID NOT ANSWER", see.result,
		    see.error);
		return false;
	}
	printf("slpi: %u: the service takes it: we are its client %llu\n", step, see.client);
	for (unsigned ms = 0; ms < 3000 && see.suids == 0; ms++) {
		sleep_ms(1);
	}
	if (see.suids == 0) {
		printf("slpi: %u: NO LIGHT SENSOR NAMED in 3 s (%u reports)\n", step, see.reports);
		return false;
	}
	error = see_request(see.suid[0][0], see.suid[0][1], SEE_MSG_ON_CHANGE, NULL, 0);
	if (error != 0 || !see_wait_answer()) {
		printf("slpi: %u: the light sensor %016llx%016llx NOT SWITCHED ON (%d, result %u, error %u)\n", step, see.suid[0][1],
		    see.suid[0][0], error, see.result, see.error);
		return false;
	}
	for (unsigned ms = 0; ms < 3000 && see_events == 0; ms++) {
		sleep_ms(1);
	}
	printf("slpi: %u: the light sensor is on: %d readings in the first 3 s%s\n", step, see_events, see_events ? "" :
	    " (an on-change sensor says nothing while the light stays the same)");
	return true;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_rpc, step_listen, step_see,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (slpi_keep, slpi=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("slpi: THE SENSOR DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_slpi_stop(void)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("slpi: the sensor DSP is off: it had been up%s (node %u, %u services), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_slpi_quiesce(void)
{
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("slpi: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("slpi: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("slpi: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("slpi: THE SENSOR DSP IS UP AND LINKED, %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
	} else {
		tell("slpi: THE SENSOR DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (bsd/dev/hakios.input.c), after the Wi-Fi's start has been set going. */
void
hakios_slpi_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("slpi_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("slpi", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		printf("slpi: slpi=1 and slpi_step=N are for the phone's sensor DSP, and this is not the phone\n");
		return;
	}
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("slpi: no thread: the sensor DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("slpi: the sensor DSP is being brought up beside the system's start (slpi=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("slpi_keep");
	new_from = last_step;
	PE_parse_boot_argn("slpi_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("slpi_pause", &pause_ms, sizeof(pause_ms));
	printf("slpi: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("slpi_return")) {
		printf("slpi: back to the loader (slpi_return)\n");
		loader_return();
	}
	printf("slpi: the system goes on starting\n");
}
