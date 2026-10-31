/*
 * The modem's MHI host (sys/alioth_mhi.h). Register offsets and element formats are the MHI specification's, as
 * Linux's drivers/bus/mhi/common.h spells them and umi_mhi.c already uses them against a Qualcomm device here.
 * Everything is polled: no MSI; an event ring is read up to where the device has written it.
 */
#if KERNEL
#include <libkern/libkern.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include <sys/alioth_mhi.h>
#include <sys/alioth_sahara.h>

#define MHIREGLEN               0x00
#define MHIVER                  0x08
#define MHICFG                  0x10    /* NHWER 31:24, NER 23:16, NHWCH 15:8, NCH 7:0 */
#define CHDBOFF                 0x18
#define ERDBOFF                 0x20
#define BHIOFF                  0x28
#define MHICTRL                 0x38
#define MHISTATUS               0x48
#define CCABAP                  0x58
#define ECABAP                  0x60
#define CRCBAP                  0x68
#define CRDB                    0x70
#define MHICTRLBASE             0x80
#define MHICTRLLIMIT            0x88
#define MHIDATABASE             0x98
#define MHIDATALIMIT            0xa0

#define BHI_VERSION_MAJOR       0x04
#define BHI_IMGADDR             0x08
#define BHI_IMGSIZE             0x10
#define BHI_IMGTXDB             0x18
#define BHI_INTVEC              0x20
#define BHI_EXECENV             0x28
#define BHI_STATUS              0x2c    /* 31:30: 2 done, 3 failed */
#define BHI_ERRCODE             0x30
#define BHI_ERRDBG1             0x34

#define STATUS_READY            (1u << 0)
#define STATUS_SYSERR           (1u << 2)
#define CTRL_RESET              (1u << 1)
#define STATE_OF(r)             (((r) >> 8) & 0xff)
#define STATE_READY             1
#define STATE_M0                2

#define TYPE_TRANSFER           0x02
#define TYPE_RESET_CHANNEL      0x10
#define TYPE_START_CHANNEL      0x12
#define TYPE_STATE              0x20
#define TYPE_COMMAND_DONE       0x21
#define TYPE_TRANSFER_DONE      0x22
#define TYPE_EE                 0x40
#define TYPE_BANDWIDTH          0x50

#define CODE_SUCCESS            1
#define CODE_EOT                2
#define CODE_OVERFLOW           3
#define CODE_EOB                4
#define CODE_OOB                5
#define CODE_DB_MODE            6

#define TRE_CHAIN               (1u << 0)
#define TRE_IEOT                (1u << 9)

#define CONTEXT_WORDS           11      /* 44 bytes, 4-byte aligned 64-bit fields */
#define COMMAND_ELEMENTS        128
#define CHANNEL_ENABLED         1
#define CHANNEL_BURST_OFF       (2u << 8)
#define COMMAND_MS              2000    /* kona-mhi.dtsi: mhi,timeout */

static const uint32_t event_elements[ALIOTH_MHI_EVENT_RINGS] = { 32, 256, 256 };
static const uint32_t event_vectors[ALIOTH_MHI_EVENT_RINGS] = { 1, 2, 3 };       /* mhi,msi (polled all the same) */

static void
write_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshst" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static void
read_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshld" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

static uint32_t
rd(struct alioth_mhi *host, uint32_t offset)
{
	return host->io->read32(host->io->context, offset);
}

static void
wr(struct alioth_mhi *host, uint32_t offset, uint32_t value)
{
	host->io->write32(host->io->context, offset, value);
}

/* high half first: the low half is what a doorbell acts on */
static void
wr64(struct alioth_mhi *host, uint32_t offset, uint64_t value)
{
	wr(host, offset + 4, (uint32_t)(value >> 32));
	wr(host, offset, (uint32_t)value);
}

static void
say(struct alioth_mhi *host, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	if (host->io->say != NULL) {
		host->io->say(host->io->context, what, a, b, c);
	}
}

static bool
in_bar(struct alioth_mhi *host, uint32_t base, uint32_t length)
{
	return !(base & 3) && base <= host->io->bar_size && length <= host->io->bar_size - base;
}

static uint64_t
get64(const volatile uint32_t *words, unsigned at)
{
	return words[at] | (uint64_t)words[at + 1] << 32;
}

static void
set64(volatile uint32_t *words, unsigned at, uint64_t value)
{
	words[at] = (uint32_t)value;
	words[at + 1] = (uint32_t)(value >> 32);
}

static volatile uint32_t *
context_of(struct alioth_mhi_dma *table, unsigned index)
{
	return (volatile uint32_t *)table->bytes + index * CONTEXT_WORDS;
}

static uint64_t
ring_length(const struct alioth_mhi_ring *ring)
{
	return (uint64_t)ring->elements * 16;
}

static bool
in_ring(const struct alioth_mhi_ring *ring, uint64_t pointer)
{
	return pointer >= ring->dma.address && pointer < ring->dma.address + ring_length(ring) &&
	       !((pointer - ring->dma.address) & 15);
}

static uint64_t
ring_next(const struct alioth_mhi_ring *ring, uint64_t pointer)
{
	pointer += 16;
	return pointer == ring->dma.address + ring_length(ring) ? ring->dma.address : pointer;
}

static uint32_t
ring_index(const struct alioth_mhi_ring *ring, uint64_t pointer)
{
	return (uint32_t)((pointer - ring->dma.address) / 16);
}

static volatile uint32_t *
ring_element(const struct alioth_mhi_ring *ring, uint64_t pointer)
{
	return (volatile uint32_t *)((volatile uint8_t *)ring->dma.bytes + (size_t)(pointer - ring->dma.address));
}

/* elements the host may still write: one is always left empty, so that full and empty differ */
static uint32_t
ring_room(const struct alioth_mhi_ring *ring)
{
	uint64_t length = ring_length(ring);

	return (uint32_t)(((ring->rp + length - ring->wp - 16) % length) / 16);
}

/* whether `pointer` is one of the elements given to the device and not yet back, [rp, wp) */
static bool
ring_given(const struct alioth_mhi_ring *ring, uint64_t pointer)
{
	uint64_t length = ring_length(ring);

	return in_ring(ring, pointer) && (pointer + length - ring->rp) % length < (ring->wp + length - ring->rp) % length;
}

static void
sync(struct alioth_mhi *host, const struct alioth_mhi_dma *dma)
{
	if (host->io->sync != NULL && dma->bytes != NULL) {
		host->io->sync(host->io->context, dma);
	}
}

static int
allocate(struct alioth_mhi *host, size_t size, struct alioth_mhi_dma *dma)
{
	int error = host->io->allocate(host->io->context, size, dma);

	if (error != 0) {
		return error;
	}
	if (dma->bytes == NULL || ((uintptr_t)dma->bytes & 7) || dma->size < size || (dma->address & 4095) ||
	    dma->address < host->io->window_base || dma->address > host->io->window_last ||
	    size - 1 > host->io->window_last - dma->address) {
		say(host, "MHI: memory outside the device's window: address, size", dma->address, size, 0);
		return EINVAL;
	}
	return 0;
}

static int
ring_allocate(struct alioth_mhi *host, struct alioth_mhi_ring *ring, uint32_t elements)
{
	int error = allocate(host, (size_t)elements * 16, &ring->dma);

	if (error == 0) {
		memset(ring->dma.bytes, 0, (size_t)elements * 16);
		ring->elements = elements;
		ring->rp = ring->wp = ring->dma.address;
	}
	return error;
}

int
alioth_mhi_probe(struct alioth_mhi *host, const struct alioth_mhi_io *io)
{
	uint32_t length, config;

	if (host == NULL || io == NULL || io->read32 == NULL || io->write32 == NULL || io->allocate == NULL ||
	    io->delay_us == NULL || io->bar_size < 4096 || io->window_last <= io->window_base) {
		return EINVAL;
	}
	if (host->io != NULL) {
		return EBUSY;
	}
	host->io = io;
	length = rd(host, MHIREGLEN);
	config = rd(host, MHICFG);
	host->bhi = rd(host, BHIOFF);
	host->channel_db = rd(host, CHDBOFF);
	host->event_db = rd(host, ERDBOFF);
	host->channels = config & 0xff;
	say(host, "MHI: register length, version, configuration", length, rd(host, MHIVER), config);
	say(host, "MHI: BHI, channel doorbells, event doorbells", host->bhi, host->channel_db, host->event_db);
	if (length == UINT32_MAX || length < 0xa8 || length > io->bar_size || !in_bar(host, host->bhi, 0x40) ||
	    !in_bar(host, host->channel_db, host->channels * 8) ||
	    !in_bar(host, host->event_db, ALIOTH_MHI_EVENT_RINGS * 8) || rd(host, host->bhi + BHI_VERSION_MAJOR) == UINT32_MAX) {
		host->io = NULL;
		return ENODEV;
	}
	host->sequence = 0x1d00;                /* anything not 0 */
	host->command_pending = UINT64_MAX;
	host->ee = rd(host, host->bhi + BHI_EXECENV);
	host->state = STATE_OF(rd(host, MHISTATUS));
	say(host, "MHI: execution environment, state, BHI major version", host->ee, host->state,
	    rd(host, host->bhi + BHI_VERSION_MAJOR));
	return 0;
}

int
alioth_mhi_execution_environment(struct alioth_mhi *host, uint32_t *ee)
{
	uint32_t value;

	if (host == NULL || host->io == NULL || ee == NULL) {
		return EINVAL;
	}
	value = rd(host, host->bhi + BHI_EXECENV);
	if (value > 7) {
		return EIO;
	}
	*ee = value;
	return 0;
}

int
alioth_mhi_bhi(struct alioth_mhi *host, const void *image, size_t size, unsigned timeout_ms)
{
	uint32_t ee, status = 0, sequence;
	int error;

	if (host == NULL || host->io == NULL || image == NULL || size == 0 || size > (64u << 20)) {
		return EINVAL;
	}
	if (host->poisoned) {
		return EIO;
	}
	if ((error = alioth_mhi_execution_environment(host, &ee)) != 0) {
		return error;
	}
	if (ee != ALIOTH_MHI_EE_PBL || host->image.bytes != NULL) {
		say(host, "MHI: BHI wanted in PBL; execution environment", ee, 0, 0);
		return ee == ALIOTH_MHI_EE_SBL || ee == ALIOTH_MHI_EE_AMSS ? EALREADY : EBUSY;
	}
	if ((error = allocate(host, size, &host->image)) != 0) {
		return error;
	}
	memcpy(host->image.bytes, image, size);
	{
		/* what the device will read, as it lies in the memory given it (FNV-1a, to set beside the file's) */
		const volatile uint8_t *b = host->image.bytes;
		uint32_t hash = 2166136261u;

		for (size_t i = 0; i < size; i++) {
			hash = (hash ^ b[i]) * 16777619u;
		}
		say(host, "MHI: BHI image as given: bytes, FNV-1a, first word", size, hash,
		    (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
	}
	sync(host, &host->image);
	sequence = host->sequence++ & 0x3fffffff;
	write_barrier();
	wr(host, host->bhi + BHI_STATUS, 0);
	wr(host, host->bhi + BHI_INTVEC, 0);
	if (host->bhi_probe != 0) {
		say(host, "MHI: BHI POINTED AT AN ADDRESS WITH NOTHING BEHIND IT (diagnosis): address", host->bhi_probe, 0, 0);
	}
	wr64(host, host->bhi + BHI_IMGADDR, host->bhi_probe != 0 ? host->bhi_probe : host->image.address);
	wr(host, host->bhi + BHI_IMGSIZE, (uint32_t)size);
	write_barrier();
	wr(host, host->bhi + BHI_IMGTXDB, sequence);
	{
		uint32_t last = rd(host, host->bhi + BHI_STATUS), changes = 0;

		say(host, "MHI: BHI rung: image at, bytes, status right after", host->image.address, size, last);
		for (unsigned ms = 0; ms < timeout_ms; ms++) {
			status = rd(host, host->bhi + BHI_STATUS);
			if (status != last && changes++ < 16) {
				/* (every change said: all ones from a register is not always the link -- 2026-10-31 the SDX55's BHI
				 * status read 0xffffffff while MHISTATUS read 0) */
				say(host, "MHI: BHI status now: status, ms, MHI status", status, ms, rd(host, MHISTATUS));
				last = status;
			}
			if (status >> 30 == 2 && status != UINT32_MAX) {
				say(host, "MHI: BHI took the image: bytes, status, ms", size, status, ms);
				return 0;
			}
			if (status >> 30 == 3 && status != UINT32_MAX) {
				break;
			}
			host->io->delay_us(host->io->context, 1000);
		}
	}
	say(host, "MHI: BHI DID NOT TAKE THE IMAGE: status, error code, execution environment", status,
	    rd(host, host->bhi + BHI_ERRCODE), rd(host, host->bhi + BHI_EXECENV));
	say(host, "MHI: BHI's debug registers 1, 2, 3", rd(host, host->bhi + BHI_ERRDBG1), rd(host, host->bhi + BHI_ERRDBG1 + 4),
	    rd(host, host->bhi + BHI_ERRDBG1 + 8));
	say(host, "MHI: MHI status, control, version", rd(host, MHISTATUS), rd(host, MHICTRL), rd(host, MHIVER));
	host->poisoned = true;
	return status >> 30 == 3 && status != UINT32_MAX ? EIO : ETIMEDOUT;
}

int
alioth_mhi_ready(struct alioth_mhi *host, const struct alioth_mhi_known *known, unsigned count, unsigned timeout_ms)
{
	uint32_t status = 0, control = 0, config;
	unsigned ms;
	int error;

	if (host == NULL || host->io == NULL || (count != 0 && known == NULL)) {
		return EINVAL;
	}
	if (host->poisoned) {
		return EIO;
	}
	if (host->ready) {
		return EALREADY;
	}
	for (ms = 0; ms < timeout_ms; ms++) {
		status = rd(host, MHISTATUS);
		control = rd(host, MHICTRL);
		if (status == UINT32_MAX || control == UINT32_MAX || (status & STATUS_SYSERR)) {
			say(host, "MHI: SYSTEM ERROR (or the link is down) waiting for READY: status, control", status, control, 0);
			host->poisoned = true;
			return EIO;
		}
		if ((status & STATUS_READY) && !(control & CTRL_RESET) && STATE_OF(status) == STATE_READY) {
			break;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	if (ms == timeout_ms) {
		say(host, "MHI: not READY: status, control, execution environment", status, control, rd(host, host->bhi + BHI_EXECENV));
		return ETIMEDOUT;
	}
	say(host, "MHI: READY after ms; execution environment", ms, rd(host, host->bhi + BHI_EXECENV), 0);

	if ((error = allocate(host, ALIOTH_MHI_CONTEXTS * CONTEXT_WORDS * 4, &host->channel_contexts)) != 0 ||
	    (error = allocate(host, ALIOTH_MHI_EVENT_RINGS * CONTEXT_WORDS * 4, &host->event_contexts)) != 0 ||
	    (error = allocate(host, CONTEXT_WORDS * 4, &host->command_context)) != 0 ||
	    (error = ring_allocate(host, &host->commands, COMMAND_ELEMENTS)) != 0) {
		return error;
	}
	memset(host->channel_contexts.bytes, 0, ALIOTH_MHI_CONTEXTS * CONTEXT_WORDS * 4);
	memset(host->event_contexts.bytes, 0, ALIOTH_MHI_EVENT_RINGS * CONTEXT_WORDS * 4);
	memset(host->command_context.bytes, 0, CONTEXT_WORDS * 4);
	for (unsigned i = 0; i < count; i++) {
		volatile uint32_t *c;

		if (known[i].event_ring >= ALIOTH_MHI_EVENT_RINGS) {
			return EINVAL;
		}
		c = context_of(&host->channel_contexts, known[i].number);
		c[0] = CHANNEL_BURST_OFF;       /* disabled */
		c[1] = known[i].direction;
		c[2] = known[i].event_ring;
	}
	for (unsigned i = 0; i < ALIOTH_MHI_EVENT_RINGS; i++) {
		struct alioth_mhi_ring *ring = &host->event[i];
		volatile uint32_t *c = context_of(&host->event_contexts, i);

		if ((error = ring_allocate(host, ring, event_elements[i])) != 0) {
			return error;
		}
		/* all but one element free for the device (Linux: mhi_ready_state_transition) */
		ring->wp = ring->dma.address + ring_length(ring) - 16;
		c[0] = 0;                       /* no interrupt moderation */
		c[1] = 1;                       /* a valid event ring */
		c[2] = event_vectors[i];
		set64(c, 3, ring->dma.address);
		set64(c, 5, ring_length(ring));
		set64(c, 7, ring->dma.address);
		set64(c, 9, ring->wp);
	}
	{
		volatile uint32_t *c = host->command_context.bytes;

		set64(c, 3, host->commands.dma.address);
		set64(c, 5, ring_length(&host->commands));
		set64(c, 7, host->commands.dma.address);
		set64(c, 9, host->commands.dma.address);
	}
	sync(host, &host->channel_contexts);
	sync(host, &host->event_contexts);
	sync(host, &host->command_context);
	write_barrier();
	config = rd(host, MHICFG);
	wr(host, MHICFG, (config & 0xffff) | (uint32_t)ALIOTH_MHI_EVENT_RINGS << 16);
	wr64(host, CCABAP, host->channel_contexts.address);
	wr64(host, ECABAP, host->event_contexts.address);
	wr64(host, CRCBAP, host->command_context.address);
	wr64(host, MHICTRLBASE, host->io->window_base);
	wr64(host, MHICTRLLIMIT, host->io->window_last);
	wr64(host, MHIDATABASE, host->io->window_base);
	wr64(host, MHIDATALIMIT, host->io->window_last);
	write_barrier();
	for (unsigned i = 0; i < ALIOTH_MHI_EVENT_RINGS; i++) {
		wr64(host, host->event_db + i * 8, host->event[i].wp);
	}
	host->ready = true;
	wr(host, MHICTRL, (control & ~0xff00u) | STATE_M0 << 8);
	for (ms = 0; ms < timeout_ms; ms++) {
		status = rd(host, MHISTATUS);
		if (status == UINT32_MAX || (status & STATUS_SYSERR)) {
			break;
		}
		if (STATE_OF(status) == STATE_M0) {
			say(host, "MHI: M0 after ms", ms, 0, 0);
			return alioth_mhi_poll(host);
		}
		host->io->delay_us(host->io->context, 1000);
	}
	say(host, "MHI: NO M0: status", status, 0, 0);
	host->poisoned = true;
	return status == UINT32_MAX || (status & STATUS_SYSERR) ? EIO : ETIMEDOUT;
}

static struct alioth_mhi_channel *
channel_of(struct alioth_mhi *host, unsigned number)
{
	for (unsigned i = 0; i < ALIOTH_MHI_OPEN; i++) {
		if (host->open[i].open && host->open[i].number == number) {
			return &host->open[i];
		}
	}
	return NULL;
}

static void
channel_doorbell(struct alioth_mhi *host, struct alioth_mhi_channel *c)
{
	set64(context_of(&host->channel_contexts, c->number), 9, c->ring.wp);
	sync(host, &c->ring.dma);
	sync(host, &host->channel_contexts);
	write_barrier();
	wr64(host, host->channel_db + c->number * 8, c->ring.wp);
}

/* an empty buffer for the device at the ring's write end: the slot of the same index */
static void
post_buffer(struct alioth_mhi_channel *c)
{
	uint32_t index = ring_index(&c->ring, c->ring.wp);
	volatile uint32_t *tre = ring_element(&c->ring, c->ring.wp);

	set64(tre, 0, c->buffers.address + (uint64_t)index * c->slot);
	tre[2] = c->slot;
	tre[3] = TYPE_TRANSFER << 16 | TRE_IEOT;
	c->ring.wp = ring_next(&c->ring, c->ring.wp);
}

static int
transfer_done(struct alioth_mhi *host, unsigned number, uint64_t pointer, unsigned code, uint32_t length)
{
	struct alioth_mhi_channel *c = channel_of(host, number);
	bool good = code == CODE_EOT || code == CODE_EOB || code == CODE_OVERFLOW;

	if (c == NULL) {
		say(host, "MHI: an event for a channel not open: channel, code", number, code, 0);
		return 0;
	}
	if (code == CODE_OOB || code == CODE_DB_MODE) {
		/* the device went back to waiting for doorbells: ring again if it has work */
		if (c->ring.wp != c->ring.rp) {
			channel_doorbell(host, c);
		}
		return 0;
	}
	if (!ring_given(&c->ring, pointer)) {
		say(host, "MHI: A TRANSFER EVENT OUTSIDE WHAT WAS GIVEN: channel, pointer, code", number, pointer, code);
		return EIO;
	}
	if (!good) {
		c->failed++;
		say(host, "MHI: a transfer failed: channel, code, length", number, code, length);
	}
	if (c->direction == ALIOTH_MHI_TO_DEVICE) {
		/* everything up to the element reported is the device's no more */
		c->ring.rp = ring_next(&c->ring, pointer);
		c->done++;
		return good ? 0 : EIO;
	}
	for (;;) {
		uint64_t at = c->ring.rp;
		bool last = at == pointer;
		uint32_t index = ring_index(&c->ring, at);
		uint32_t n = !last ? c->slot : length < c->slot ? length : c->slot;

		if (good && c->receive != NULL) {
			c->receive(c->receive_context, number, (const uint8_t *)c->buffers.bytes + (size_t)index * c->slot, n,
			    last && code == CODE_EOT);
		}
		c->ring.rp = ring_next(&c->ring, at);
		post_buffer(c);
		c->done++;
		if (last) {
			break;
		}
	}
	channel_doorbell(host, c);
	return good ? 0 : EIO;
}

static int
event(struct alioth_mhi *host, uint64_t pointer, uint32_t w0, uint32_t w1)
{
	unsigned type = (w1 >> 16) & 0xff;

	switch (type) {
	case TYPE_STATE:
		host->state = w0 >> 24;
		say(host, "MHI: the device is in state", host->state, 0, 0);
		return 0;
	case TYPE_EE:
		host->ee = w0 >> 24;
		say(host, "MHI: the device is in execution environment", host->ee, 0, 0);
		return 0;
	case TYPE_COMMAND_DONE:
		if (pointer != host->command_pending) {
			say(host, "MHI: a command completion for no command: pointer, code", pointer, w0 >> 24, 0);
			return 0;
		}
		host->command_code = (int)(w0 >> 24);
		return 0;
	case TYPE_TRANSFER_DONE:
		return transfer_done(host, w1 >> 24, pointer, w0 >> 24, w0 & 0xffff);
	case TYPE_BANDWIDTH:
		host->bandwidth_requests++;     /* (a link speed request: the link stays as it is) */
		return 0;
	default:
		say(host, "MHI: an event we do not use: type, words", type, w0, w1);
		return 0;
	}
}

static int
service(struct alioth_mhi *host, unsigned r)
{
	struct alioth_mhi_ring *ring = &host->event[r];
	volatile uint32_t *c = context_of(&host->event_contexts, r);
	uint64_t producer;
	unsigned handled = 0;

	read_barrier();
	producer = get64(c, 7);
	if (!in_ring(ring, producer)) {
		say(host, "MHI: AN EVENT RING POINTER OUTSIDE ITS RING: ring, pointer", r, producer, 0);
		return EIO;
	}
	while (ring->rp != producer && handled < ring->elements) {
		volatile uint32_t *e = ring_element(ring, ring->rp);
		uint64_t pointer;
		uint32_t w0, w1;
		int error;

		read_barrier();
		pointer = get64(e, 0);
		w0 = e[2];
		w1 = e[3];
		ring->rp = ring_next(ring, ring->rp);
		ring->wp = ring_next(ring, ring->wp);
		handled++;
		if ((error = event(host, pointer, w0, w1)) != 0) {
			return error;
		}
	}
	if (handled != 0) {
		host->events += handled;
		set64(c, 9, ring->wp);
		sync(host, &host->event_contexts);
		write_barrier();
		wr64(host, host->event_db + r * 8, ring->wp);
	}
	return 0;
}

int
alioth_mhi_poll(struct alioth_mhi *host)
{
	uint32_t status;

	if (host == NULL || host->io == NULL) {
		return EINVAL;
	}
	if (host->poisoned) {
		return EIO;
	}
	if (!host->ready) {
		return EBUSY;
	}
	status = rd(host, MHISTATUS);
	if (status == UINT32_MAX || (status & STATUS_SYSERR)) {
		say(host, "MHI: SYSTEM ERROR (or the link is down): status, execution environment", status,
		    rd(host, host->bhi + BHI_EXECENV), 0);
		host->poisoned = true;
		return EIO;
	}
	for (unsigned r = 0; r < ALIOTH_MHI_EVENT_RINGS; r++) {
		int error = service(host, r);

		if (error != 0) {
			host->poisoned = true;
			return error;
		}
	}
	return 0;
}

static int
command(struct alioth_mhi *host, unsigned type, unsigned number)
{
	uint64_t at = host->commands.wp, after = ring_next(&host->commands, at);
	volatile uint32_t *tre = ring_element(&host->commands, at);

	if (ring_room(&host->commands) == 0) {
		return EBUSY;
	}
	tre[0] = tre[1] = tre[2] = 0;
	tre[3] = number << 24 | type << 16;
	host->commands.wp = after;
	host->command_pending = at;
	host->command_code = -1;
	set64(host->command_context.bytes, 9, after);
	sync(host, &host->commands.dma);
	sync(host, &host->command_context);
	sync(host, &host->channel_contexts);    /* (START/RESET read the channel's context) */
	write_barrier();
	wr64(host, CRDB, after);
	for (unsigned ms = 0; ms < COMMAND_MS; ms++) {
		int error = alioth_mhi_poll(host);

		if (error != 0) {
			return error;
		}
		if (host->command_code >= 0) {
			host->commands.rp = after;
			host->command_pending = UINT64_MAX;
			if (host->command_code != CODE_SUCCESS) {
				say(host, "MHI: A COMMAND FAILED: type, channel, code", type, number, (uint64_t)host->command_code);
				return EIO;
			}
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	say(host, "MHI: A COMMAND WAS NOT ANSWERED: type, channel, status", type, number, rd(host, MHISTATUS));
	host->poisoned = true;
	return ETIMEDOUT;
}

int
alioth_mhi_open(struct alioth_mhi *host, unsigned number, unsigned direction, unsigned event_ring, unsigned elements,
    uint32_t slot, alioth_mhi_receive receive, void *receive_context)
{
	struct alioth_mhi_channel *c = NULL;
	volatile uint32_t *context;
	int error;

	if (host == NULL || host->io == NULL || number >= ALIOTH_MHI_CONTEXTS || number >= host->channels ||
	    (direction != ALIOTH_MHI_TO_DEVICE && direction != ALIOTH_MHI_FROM_DEVICE) || event_ring >= ALIOTH_MHI_EVENT_RINGS ||
	    elements < 2 || elements > 1024 || slot == 0 || slot > 0xffff) {
		return EINVAL;
	}
	if (host->poisoned) {
		return EIO;
	}
	if (!host->ready) {
		return EBUSY;
	}
	if (channel_of(host, number) != NULL) {
		return EALREADY;
	}
	for (unsigned i = 0; i < ALIOTH_MHI_OPEN && c == NULL; i++) {
		if (!host->open[i].open) {
			c = &host->open[i];
		}
	}
	if (c == NULL) {
		return ENOSPC;
	}
	memset(c, 0, sizeof(*c));
	if ((error = ring_allocate(host, &c->ring, elements)) != 0 ||
	    (error = allocate(host, (size_t)elements * slot, &c->buffers)) != 0) {
		return error;
	}
	c->number = (uint8_t)number;
	c->direction = (uint8_t)direction;
	c->event_ring = (uint8_t)event_ring;
	c->slot = slot;
	c->receive = receive;
	c->receive_context = receive_context;
	context = context_of(&host->channel_contexts, number);
	for (unsigned i = 0; i < CONTEXT_WORDS; i++) {
		context[i] = 0;
	}
	context[0] = CHANNEL_ENABLED | CHANNEL_BURST_OFF;
	context[1] = direction;
	context[2] = event_ring;
	set64(context, 3, c->ring.dma.address);
	set64(context, 5, ring_length(&c->ring));
	set64(context, 7, c->ring.dma.address);
	set64(context, 9, c->ring.dma.address);
	c->open = true;
	write_barrier();
	/* as Linux: started empty, the buffers for the device's bytes given after */
	if ((error = command(host, TYPE_START_CHANNEL, number)) != 0) {
		c->open = false;
		return error;
	}
	if (direction == ALIOTH_MHI_FROM_DEVICE) {
		while (ring_room(&c->ring) > 0) {
			post_buffer(c);
		}
		channel_doorbell(host, c);
	}
	say(host, "MHI: channel open: number, direction, elements", number, direction, elements);
	return 0;
}

int
alioth_mhi_close(struct alioth_mhi *host, unsigned number)
{
	struct alioth_mhi_channel *c;
	volatile uint32_t *context;
	int error;

	if (host == NULL || host->io == NULL || (c = channel_of(host, number)) == NULL) {
		return EINVAL;
	}
	error = command(host, TYPE_RESET_CHANNEL, number);
	context = context_of(&host->channel_contexts, number);
	context[0] = CHANNEL_BURST_OFF;         /* disabled; its direction and event ring stay */
	for (unsigned i = 3; i < CONTEXT_WORDS; i++) {
		context[i] = 0;
	}
	write_barrier();
	c->open = false;                        /* (its memory stays the device's until the endpoint is reset) */
	return error;
}

int
alioth_mhi_send(struct alioth_mhi *host, unsigned number, const void *bytes, uint32_t length, bool last,
    unsigned timeout_ms)
{
	struct alioth_mhi_channel *c;
	const uint8_t *p = bytes;

	if (host == NULL || host->io == NULL || (c = channel_of(host, number)) == NULL ||
	    c->direction != ALIOTH_MHI_TO_DEVICE || bytes == NULL || length == 0) {
		return EINVAL;
	}
	if (host->poisoned) {
		return EIO;
	}
	while (length > 0) {
		uint32_t n = length < c->slot ? length : c->slot, index, flags;
		volatile uint32_t *tre;
		unsigned ms = 0;
		int error;

		while (ring_room(&c->ring) == 0) {
			if ((error = alioth_mhi_poll(host)) != 0) {
				return error;
			}
			if (ring_room(&c->ring) != 0) {
				break;
			}
			if (ms++ == timeout_ms) {
				say(host, "MHI: THE DEVICE TAKES NOTHING MORE: channel, done", number, c->done, 0);
				host->poisoned = true;
				return ETIMEDOUT;
			}
			host->io->delay_us(host->io->context, 1000);
		}
		index = ring_index(&c->ring, c->ring.wp);
		memcpy((uint8_t *)c->buffers.bytes + (size_t)index * c->slot, p, n);
		{
			const struct alioth_mhi_dma piece = { (uint8_t *)c->buffers.bytes + (size_t)index * c->slot,
				                              c->buffers.address + (uint64_t)index * c->slot, n };

			sync(host, &piece);
		}
		/*
		 * A transfer goes on (CHAIN) until its last bytes (EOT); when the ring is about to be full it ends early
		 * (EOT), as Android's mhi_uci does, so that the device gives elements back and the rest can follow.
		 */
		flags = (n == length && last) || ring_room(&c->ring) == 1 ? TRE_IEOT : TRE_CHAIN;
		tre = ring_element(&c->ring, c->ring.wp);
		set64(tre, 0, c->buffers.address + (uint64_t)index * c->slot);
		tre[2] = n;
		tre[3] = TYPE_TRANSFER << 16 | flags;
		c->ring.wp = ring_next(&c->ring, c->ring.wp);
		channel_doorbell(host, c);
		p += n;
		length -= n;
	}
	return 0;
}

int
alioth_mhi_wait_ee(struct alioth_mhi *host, uint32_t ee, unsigned timeout_ms)
{
	for (unsigned ms = 0; ms <= timeout_ms; ms++) {
		int error = alioth_mhi_poll(host);

		if (error != 0) {
			return error;
		}
		if (host->ee == ee || rd(host, host->bhi + BHI_EXECENV) == ee) {
			host->ee = ee;
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	say(host, "MHI: the device did not reach execution environment: wanted, reported, register", ee, host->ee,
	    rd(host, host->bhi + BHI_EXECENV));
	return ETIMEDOUT;
}

/* SAHARA: kona-mhi.dtsi's channels 2 and 3, event ring 1, 128 elements, mhi,buffer-len 0x8000 */
#define SAHARA_TO_DEVICE        2
#define SAHARA_FROM_DEVICE      3
#define SAHARA_EVENT_RING       1
#define SAHARA_ELEMENTS         128
#define SAHARA_SLOT             0x8000
#define SAHARA_IN_ELEMENTS      16
#define SAHARA_IN_SLOT          0x1000
#define SAHARA_QUEUE            8
#define SAHARA_PACKET           256             /* the longest the SBL sends is HELLO's 0x30 */
#define SAHARA_SEND_MS          2000
#define SAHARA_MISSION_MS       30000

static struct sahara_link {
	struct alioth_mhi *host;
	const uint8_t *(*image)(void *, uint64_t, uint64_t *);
	void *context;
	unsigned head, tail;                    /* the device's packets, in the order sent */
	uint32_t partial, dropped;
	bool skipping;
	uint32_t length[SAHARA_QUEUE];
	uint8_t packet[SAHARA_QUEUE][SAHARA_PACKET];
} sahara_link;

/* (from the poll: only queued here, answered by alioth_mhi_sahara's loop) */
static void
sahara_received(void *context, unsigned channel, const uint8_t *bytes, uint32_t length, bool end)
{
	struct sahara_link *l = context;
	unsigned slot = l->tail % SAHARA_QUEUE;

	(void)channel;
	if (l->partial == 0 && !l->skipping && l->tail - l->head == SAHARA_QUEUE) {
		l->skipping = true;
	}
	if (!l->skipping) {
		if (l->partial + length <= SAHARA_PACKET) {
			memcpy(l->packet[slot] + l->partial, bytes, length);
		}
		l->partial += length;
	}
	if (!end) {
		return;
	}
	if (l->skipping) {
		l->dropped++;
		l->skipping = false;
		return;
	}
	/* one too long is passed on empty: Sahara answers it with RESET */
	l->length[slot] = l->partial <= SAHARA_PACKET ? l->partial : 0;
	l->partial = 0;
	l->tail++;
}

static int
sahara_send(void *context, const void *bytes, uint32_t length, bool last)
{
	struct sahara_link *l = context;

	return alioth_mhi_send(l->host, SAHARA_TO_DEVICE, bytes, length, last, SAHARA_SEND_MS);
}

static const uint8_t *
sahara_image(void *context, uint64_t id, uint64_t *size)
{
	struct sahara_link *l = context;

	return l->image(l->context, id, size);
}

static void
sahara_say_it(void *context, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	struct sahara_link *l = context;

	say(l->host, what, a, b, c);
}

int
alioth_mhi_sahara(struct alioth_mhi *host, const uint8_t *(*image)(void *, uint64_t, uint64_t *), void *context,
    unsigned timeout_ms)
{
	struct sahara_link *l = &sahara_link;
	const struct sahara_ops ops = { l, sahara_send, sahara_image, sahara_say_it };
	struct sahara session;
	unsigned quiet = 0;
	int error;

	if (host == NULL || host->io == NULL || image == NULL) {
		return EINVAL;
	}
	memset(l, 0, sizeof(*l));
	l->host = host;
	l->image = image;
	l->context = context;
	sahara_init(&session, &ops);
	if ((error = alioth_mhi_open(host, SAHARA_TO_DEVICE, ALIOTH_MHI_TO_DEVICE, SAHARA_EVENT_RING, SAHARA_ELEMENTS,
	    SAHARA_SLOT, NULL, NULL)) != 0 ||
	    (error = alioth_mhi_open(host, SAHARA_FROM_DEVICE, ALIOTH_MHI_FROM_DEVICE, SAHARA_EVENT_RING, SAHARA_IN_ELEMENTS,
	    SAHARA_IN_SLOT, sahara_received, l)) != 0) {
		return error;
	}
	while (session.state != SAHARA_FINISHED) {
		unsigned slot;

		if ((error = alioth_mhi_poll(host)) != 0) {
			break;
		}
		if (l->head == l->tail) {
			if (quiet++ == timeout_ms) {
				say(host, "sahara: the modem has said nothing for ms; images done", timeout_ms, session.images_done, 0);
				error = ETIMEDOUT;
				break;
			}
			host->io->delay_us(host->io->context, 1000);
			continue;
		}
		quiet = 0;
		slot = l->head % SAHARA_QUEUE;
		error = sahara_input(&session, l->packet[slot], l->length[slot]);
		l->head++;
		if (error != 0 || session.state == SAHARA_FAILED) {
			error = error != 0 ? error : EIO;
			break;
		}
	}
	say(host, "sahara: images done, resets, bytes sent", session.images_done, session.resets, session.bytes_sent);
	if (l->dropped != 0) {
		say(host, "sahara: packets of the modem's dropped (no room)", l->dropped, 0, 0);
	}
	if (error != 0) {
		return error;
	}
	if ((error = alioth_mhi_wait_ee(host, ALIOTH_MHI_EE_AMSS, SAHARA_MISSION_MS)) != 0) {
		return error;
	}
	say(host, "sahara: the modem is in mission mode", 0, 0, 0);
	/* as Linux when SBL gives way to AMSS: SBL's channels are reset */
	error = alioth_mhi_close(host, SAHARA_TO_DEVICE);
	if (error == 0) {
		error = alioth_mhi_close(host, SAHARA_FROM_DEVICE);
	}
	return error;
}
