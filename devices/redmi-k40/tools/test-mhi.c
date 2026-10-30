/*
 * The modem's MHI host (overlays/xnu/files/bsd/dev/alioth_mhi.c) and Sahara (sys/alioth_sahara.h) against a pretend
 * SDX55, on the build machine. The pretend modem has a BAR of registers and reaches the host's memory as a PCIe
 * device would (device addresses in kona-mhi.dtsi's window), and checks what it is given: the SBL through BHI,
 * READY, the contexts and event rings at M0, START/RESET commands, transfer rings and doorbells. Its SBL then asks
 * for two images over SAHARA as Qualcomm's does (HELLO, READ_DATA / READ_DATA64, END_OF_IMAGE, DONE_RESP; a new
 * HELLO per image), one read of 5 MB being more than the ring holds, and goes to mission mode. A second run has
 * the modem fail (SYS_ERR) in the middle: the host must stop, not hang.
 * usage (WSL, in devices/redmi-k40/tools):
 *   cc -std=c11 -O1 -Wall -Wextra -I ../overlays/xnu/files/bsd -o /tmp/test-mhi test-mhi.c \
 *       ../overlays/xnu/files/bsd/dev/alioth_mhi.c && /tmp/test-mhi
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/alioth_mhi.h>
#include <sys/alioth_sahara.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)
#define DEVICE_FAIL(...) do { failures++; printf("DEVICE OBJECTS: "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* the host's memory, as the modem reaches it: device address WINDOW + offset */
#define WINDOW          0x20000000ull
#define WINDOW_LAST     0x3fffffffull
#define ARENA           (32u << 20)
static uint8_t *arena;
static size_t arena_used;

static int
allocate(void *context, size_t size, struct alioth_mhi_dma *dma)
{
	(void)context;
	size = (size + 4095) & ~(size_t)4095;
	if (arena_used + size > ARENA) {
		return ENOMEM;
	}
	dma->bytes = arena + arena_used;
	dma->address = WINDOW + arena_used;
	dma->size = size;
	memset(dma->bytes, 0, size);
	arena_used += size;
	return 0;
}

static uint8_t *
device_memory(uint64_t address, uint64_t length)
{
	if (address < WINDOW || address - WINDOW > arena_used || length > arena_used - (address - WINDOW)) {
		DEVICE_FAIL("an access outside what the host gave: %#llx + %llu", (unsigned long long)address,
		    (unsigned long long)length);
		exit(2);
	}
	return arena + (address - WINDOW);
}

static uint64_t
words64(const uint32_t *w, unsigned at)
{
	return w[at] | (uint64_t)w[at + 1] << 32;
}

/* ---- the pretend SDX55 ---- */
#define BAR             4096
#define BHI             0x100
#define CHDB            0x400
#define ERDB            0x800
#define NCH             128
static uint32_t regs[BAR / 4];

static const uint8_t *sbl;
static size_t sbl_size;
static uint8_t *image13, *image21;
#define IMAGE13_SIZE    (6u << 20)
#define IMAGE21_SIZE    100000u

struct ring { bool running; uint64_t base, length, rp, wp; };
static struct {
	uint64_t us, ready_at;
	bool m0, syserr_after_hello;
	uint64_t command_rp;
	uint64_t event_rp[3], event_host_wp[3], event_base[3], event_length[3];
	bool event_doorbell[3];
	struct ring ch[NCH];
	unsigned events, resets;
	/* the transfer being received on channel 2 */
	uint8_t *transfer;
	size_t transfer_length;
	/* packets for channel 3 */
	uint8_t out[16][64];
	uint32_t out_length[16];
	unsigned out_head, out_tail;
} dev;

/* its SBL's Sahara side: a script */
enum step_kind { HELLO, READ, READ64, EOI, DONE_PENDING, DONE_COMPLETE };
struct step { enum step_kind kind; uint32_t image; uint64_t offset, length; };
static const struct step script[] = {
	{ HELLO, 0, 0, 0 },
	{ READ, 21, 0, 80 }, { READ, 21, 80, 40000 }, { READ, 21, 40080, 40000 }, { READ, 21, 80080, IMAGE21_SIZE - 80080 },
	{ EOI, 21, 0, 0 }, { DONE_PENDING, 0, 0, 0 },
	{ HELLO, 0, 0, 0 },
	{ READ64, 13, 0, 0x34 }, { READ64, 13, 0x1000, 5u << 20 }, { READ64, 13, 0x1000 + (5u << 20), IMAGE13_SIZE - 0x1000 - (5u << 20) },
	{ EOI, 13, 0, 0 }, { DONE_COMPLETE, 0, 0, 0 },
};
enum chip_wait { NOTHING, HELLO_RESP, DATA, DONE, FINISHED };
static struct {
	unsigned step;
	enum chip_wait waiting;
	uint64_t received;                      /* of the read asked for */
	unsigned transfers, most_transfers;     /* MHI transfers one read came in */
	bool said_hello;
} chip;

static uint32_t *
channel_context(unsigned ch)
{
	return (uint32_t *)device_memory(words64(regs, 0x58 / 4) + ch * 44, 44);
}

static uint32_t *
event_context(unsigned r)
{
	return (uint32_t *)device_memory(words64(regs, 0x60 / 4) + r * 44, 44);
}

static uint32_t *
command_context(void)
{
	return (uint32_t *)device_memory(words64(regs, 0x68 / 4), 44);
}

static void
post_event(unsigned r, uint64_t pointer, uint32_t w0, uint32_t w1)
{
	uint32_t *c = event_context(r), *e;

	if (dev.event_rp[r] == dev.event_host_wp[r]) {
		DEVICE_FAIL("event ring %u is full: the host gives no elements back", r);
		return;
	}
	e = (uint32_t *)device_memory(dev.event_rp[r], 16);
	e[0] = (uint32_t)pointer;
	e[1] = (uint32_t)(pointer >> 32);
	e[2] = w0;
	e[3] = w1;
	dev.event_rp[r] += 16;
	if (dev.event_rp[r] == dev.event_base[r] + dev.event_length[r]) {
		dev.event_rp[r] = dev.event_base[r];
	}
	c[7] = (uint32_t)dev.event_rp[r];
	c[8] = (uint32_t)(dev.event_rp[r] >> 32);
	dev.events++;
}

static uint64_t
ring_next(const struct ring *r, uint64_t p)
{
	p += 16;
	return p == r->base + r->length ? r->base : p;
}

static void dl_flush(void);

static void
chip_send(const uint32_t *words, unsigned count)
{
	unsigned slot = dev.out_tail % 16;

	if (dev.out_tail - dev.out_head == 16) {
		DEVICE_FAIL("the chip's own queue is full");
		return;
	}
	memcpy(dev.out[slot], words, count * 4);
	dev.out_length[slot] = count * 4;
	dev.out_tail++;
	dl_flush();
}

static void
chip_next(void)
{
	while (chip.step < sizeof(script) / sizeof(script[0])) {
		const struct step *s = &script[chip.step++];

		switch (s->kind) {
		case HELLO: {
			uint32_t p[12] = { SAHARA_HELLO, SAHARA_HELLO_LENGTH, 2, 1, 0x400, SAHARA_MODE_PENDING };

			chip_send(p, 12);
			chip.waiting = HELLO_RESP;
			if (dev.syserr_after_hello) {
				regs[0x48 / 4] |= 4;    /* SYS_ERR */
			}
			return;
		}
		case READ: {
			uint32_t p[5] = { SAHARA_READ_DATA, SAHARA_READ_LENGTH, s->image, (uint32_t)s->offset, (uint32_t)s->length };

			chip_send(p, 5);
			chip.waiting = DATA;
			chip.received = 0;
			chip.transfers = 0;
			return;
		}
		case READ64: {
			uint32_t p[8] = { SAHARA_READ_DATA64, SAHARA_READ64_LENGTH, s->image, 0, (uint32_t)s->offset,
				(uint32_t)(s->offset >> 32), (uint32_t)s->length, (uint32_t)(s->length >> 32) };

			chip_send(p, 8);
			chip.waiting = DATA;
			chip.received = 0;
			chip.transfers = 0;
			return;
		}
		case EOI: {
			uint32_t p[4] = { SAHARA_END_OF_IMAGE, SAHARA_EOI_LENGTH, s->image, 0 };

			chip_send(p, 4);
			chip.waiting = DONE;
			return;
		}
		case DONE_PENDING:
		case DONE_COMPLETE: {
			uint32_t p[3] = { SAHARA_DONE_RESP, SAHARA_DONE_RESP_LENGTH,
				s->kind == DONE_COMPLETE ? SAHARA_MODE_COMPLETE : SAHARA_MODE_PENDING };

			chip_send(p, 3);
			if (s->kind == DONE_COMPLETE) {
				/* the SBL hands over to the modem's own image: mission mode */
				chip.waiting = FINISHED;
				regs[(BHI + 0x28) / 4] = ALIOTH_MHI_EE_AMSS;
				post_event(0, 0, ALIOTH_MHI_EE_AMSS << 24, 0x40u << 16);
				return;
			}
			break;
		}
		}
	}
}

/* one whole transfer of the host's on channel 2 */
static void
chip_transfer(const uint8_t *p, size_t length)
{
	const struct step *s = &script[chip.step - 1];

	switch (chip.waiting) {
	case HELLO_RESP:
		if (length != SAHARA_HELLO_LENGTH || sahara_le32(p) != SAHARA_HELLO_RESP || sahara_le32(p + 8) != 2 ||
		    sahara_le32(p + 16) != 0 || sahara_le32(p + 20) != SAHARA_MODE_PENDING) {
			DEVICE_FAIL("not the HELLO_RESP wanted (%zu bytes, command %u)", length, length >= 4 ? sahara_le32(p) : 0);
			return;
		}
		chip_next();
		return;
	case DATA: {
		const uint8_t *image = s->image == 13 ? image13 : image21;

		chip.transfers++;
		if (chip.received + length > s->length ||
		    memcmp(p, image + s->offset + chip.received, length) != 0) {
			DEVICE_FAIL("the bytes of image %u at %llu are not the image's (%zu bytes)", s->image,
			    (unsigned long long)(s->offset + chip.received), length);
			chip.waiting = NOTHING;
			return;
		}
		chip.received += length;
		if (chip.received == s->length) {
			if (chip.transfers > chip.most_transfers) {
				chip.most_transfers = chip.transfers;
			}
			chip_next();
		}
		return;
	}
	case DONE:
		if (length != SAHARA_DONE_LENGTH || sahara_le32(p) != SAHARA_DONE) {
			DEVICE_FAIL("not the DONE wanted");
			return;
		}
		chip_next();
		return;
	default:
		DEVICE_FAIL("a transfer of %zu bytes when none was asked for (command %u)", length, length >= 4 ? sahara_le32(p) : 0);
		return;
	}
}

static void
maybe_hello(void)
{
	if (!chip.said_hello && dev.ch[2].running && dev.ch[3].running && dev.ch[3].rp != dev.ch[3].wp) {
		chip.said_hello = true;
		/* a link speed request in passing, as the SDX55 makes them */
		post_event(0, 0, 0, 0x50u << 16);
		chip_next();
	}
}

static void
ul_process(void)
{
	struct ring *r = &dev.ch[2];

	while (r->rp != r->wp) {
		uint32_t *t = (uint32_t *)device_memory(r->rp, 16);
		uint64_t pointer = words64(t, 0);
		uint32_t length = t[2], flags = t[3];

		if (((flags >> 16) & 0xff) != 2 || length == 0 || length > 0xffff) {
			DEVICE_FAIL("a bad transfer element: %#x %u", flags, length);
			return;
		}
		if (dev.transfer_length + length > (8u << 20)) {
			DEVICE_FAIL("a transfer longer than 8 MB");
			return;
		}
		memcpy(dev.transfer + dev.transfer_length, device_memory(pointer, length), length);
		dev.transfer_length += length;
		if (flags & (1u << 9)) {
			post_event(1, r->rp, 2u << 24 | length, 2u << 24 | 0x22u << 16);
		}
		r->rp = ring_next(r, r->rp);
		if (!(flags & 1)) {
			size_t n = dev.transfer_length;

			if (!(flags & (1u << 9))) {
				DEVICE_FAIL("a transfer ended without asking for its completion");
			}
			dev.transfer_length = 0;
			chip_transfer(dev.transfer, n);
		}
	}
}

static void
dl_flush(void)
{
	struct ring *r = &dev.ch[3];

	while (dev.out_head != dev.out_tail && r->running && r->rp != r->wp) {
		uint32_t *t = (uint32_t *)device_memory(r->rp, 16);
		unsigned slot = dev.out_head % 16;
		uint32_t n = dev.out_length[slot];

		if (t[2] < n || ((t[3] >> 16) & 0xff) != 2 || !(t[3] & (1u << 9))) {
			/* (without IEOT a real modem would fill it and say nothing) */
			DEVICE_FAIL("a receive buffer too small, not a transfer element or asking for no completion");
			return;
		}
		memcpy(device_memory(words64(t, 0), n), dev.out[slot], n);
		post_event(1, r->rp, 2u << 24 | n, 3u << 24 | 0x22u << 16);
		r->rp = ring_next(r, r->rp);
		dev.out_head++;
	}
}

static uint32_t
start_channel(unsigned ch)
{
	uint32_t *c = channel_context(ch);
	unsigned want = ch == 2 ? 1 : ch == 3 ? 2 : 0;
	struct ring *r = &dev.ch[ch];

	if (want == 0 || regs[(BHI + 0x28) / 4] != ALIOTH_MHI_EE_SBL) {
		DEVICE_FAIL("START of channel %u, not one of SBL's", ch);
		return 0x10;
	}
	r->base = words64(c, 3);
	r->length = words64(c, 5);
	if ((c[0] & 0xff) != 1 || c[1] != want || c[2] != 1 || (r->base & 15) || r->length == 0 || (r->length & 15) ||
	    words64(c, 7) != r->base || words64(c, 9) != r->base) {
		DEVICE_FAIL("channel %u's context is not what START wants: %#x %u %u", ch, c[0], c[1], c[2]);
		return 0x10;
	}
	device_memory(r->base, r->length);
	r->rp = r->wp = r->base;
	r->running = true;
	c[0] = (c[0] & ~0xffu) | 2;             /* running */
	return 1;
}

static void
command_doorbell(void)
{
	uint32_t *c = command_context();
	uint64_t base = words64(c, 3), length = words64(c, 5), wp = words64(regs, 0x70 / 4);

	while (dev.command_rp != wp) {
		uint32_t *t = (uint32_t *)device_memory(dev.command_rp, 16);
		unsigned type = (t[3] >> 16) & 0xff, ch = t[3] >> 24;
		uint32_t code = 1;

		if (type == 0x12) {
			code = start_channel(ch);
		} else if (type == 0x10) {
			dev.ch[ch].running = false;
			dev.resets++;
		} else {
			DEVICE_FAIL("a command of type %#x", type);
			code = 0x10;
		}
		post_event(0, dev.command_rp, code << 24, 0x21u << 16);
		dev.command_rp += 16;
		if (dev.command_rp == base + length) {
			dev.command_rp = base;
		}
		c[7] = (uint32_t)dev.command_rp;
		c[8] = (uint32_t)(dev.command_rp >> 32);
	}
}

static void
control(uint32_t value)
{
	uint32_t *c;

	if (((value >> 8) & 0xff) != 2 || dev.m0) {
		return;
	}
	if (((regs[0x48 / 4] >> 8) & 0xff) != 1) {
		DEVICE_FAIL("M0 asked for before READY");
		return;
	}
	if (((regs[0x10 / 4] >> 16) & 0xff) != 3 || (regs[0x10 / 4] & 0xff) != NCH) {
		DEVICE_FAIL("MHICFG %#x: not three event rings, or NCH changed", regs[0x10 / 4]);
	}
	if (words64(regs, 0x80 / 4) != WINDOW || words64(regs, 0x88 / 4) != WINDOW_LAST ||
	    words64(regs, 0x98 / 4) != WINDOW || words64(regs, 0xa0 / 4) != WINDOW_LAST) {
		DEVICE_FAIL("the control and data windows are not the ones allocations come from");
	}
	for (unsigned r = 0; r < 3; r++) {
		c = event_context(r);
		dev.event_base[r] = words64(c, 3);
		dev.event_length[r] = words64(c, 5);
		dev.event_rp[r] = words64(c, 7);
		device_memory(dev.event_base[r], dev.event_length[r]);
		if (c[1] != 1 || dev.event_rp[r] != dev.event_base[r] || !dev.event_doorbell[r] ||
		    dev.event_host_wp[r] != dev.event_base[r] + dev.event_length[r] - 16 || words64(c, 9) != dev.event_host_wp[r]) {
			DEVICE_FAIL("event ring %u is not set up as Linux sets it up", r);
		}
	}
	c = channel_context(20);
	if (c[0] != 2u << 8 || c[1] != 1 || c[2] != 2) {
		DEVICE_FAIL("channel 20 (IPCR) was not described before M0");
	}
	c = command_context();
	dev.command_rp = words64(c, 3);
	if (words64(c, 7) != dev.command_rp || words64(c, 9) != dev.command_rp || words64(c, 5) == 0) {
		DEVICE_FAIL("the command ring is not set up");
	}
	dev.m0 = true;
	regs[0x48 / 4] = 1 | 2u << 8;
	post_event(0, 0, 2u << 24, 0x20u << 16);                         /* state M0 */
	post_event(0, 0, ALIOTH_MHI_EE_SBL << 24, 0x40u << 16);           /* in SBL */
}

static void
bhi_doorbell(uint32_t sequence)
{
	uint64_t address = words64(regs, (BHI + 8) / 4);
	uint32_t size = regs[(BHI + 0x10) / 4];

	if (regs[(BHI + 0x28) / 4] != ALIOTH_MHI_EE_PBL || size != sbl_size || memcmp(device_memory(address, size), sbl, size) != 0) {
		regs[(BHI + 0x2c) / 4] = 3u << 30;
		return;
	}
	regs[(BHI + 0x2c) / 4] = 2u << 30 | (sequence & 0x3fffffff);
	regs[(BHI + 0x28) / 4] = ALIOTH_MHI_EE_SBL;
	dev.ready_at = dev.us + 5000;           /* the SBL takes its time to come up */
}

static uint32_t
read32(void *context, uint32_t offset)
{
	(void)context;
	if (offset >= BAR || (offset & 3)) {
		DEVICE_FAIL("a register read at %#x", offset);
		return UINT32_MAX;
	}
	return regs[offset / 4];
}

static void
write32(void *context, uint32_t offset, uint32_t value)
{
	(void)context;
	if (offset >= BAR || (offset & 3)) {
		DEVICE_FAIL("a register write at %#x", offset);
		return;
	}
	regs[offset / 4] = value;
	if (offset == BHI + 0x18) {
		bhi_doorbell(value);
	} else if (offset == 0x38) {
		control(value);
	} else if (offset == 0x70) {
		if (!dev.m0) {
			DEVICE_FAIL("a command doorbell before M0");
		} else {
			command_doorbell();
		}
	} else if (offset >= CHDB && offset < CHDB + NCH * 8 && !(offset & 4)) {
		unsigned ch = (offset - CHDB) / 8;
		struct ring *r = &dev.ch[ch];
		uint64_t wp = words64(regs, offset / 4);

		if (!r->running) {
			DEVICE_FAIL("a doorbell for channel %u, which is not running", ch);
			return;
		}
		if (wp < r->base || wp >= r->base + r->length || (wp & 15)) {
			DEVICE_FAIL("channel %u's doorbell outside its ring", ch);
			return;
		}
		r->wp = wp;
		if (ch == 2) {
			ul_process();
		} else if (ch == 3) {
			dl_flush();
		}
		maybe_hello();
	} else if (offset >= ERDB && offset < ERDB + 3 * 8 && !(offset & 4)) {
		unsigned r = (offset - ERDB) / 8;

		dev.event_host_wp[r] = words64(regs, offset / 4);
		dev.event_doorbell[r] = true;
	}
}

static void
delay_us(void *context, unsigned us)
{
	(void)context;
	dev.us += us;
	if (dev.ready_at != 0 && dev.us >= dev.ready_at) {
		dev.ready_at = 0;
		regs[0x48 / 4] = 1 | 1u << 8;   /* READY */
	}
	if (dev.us > 600ull * 1000 * 1000) {
		printf("FAIL: ten minutes of pretend time\n");
		exit(2);
	}
}

static int quiet_say;

static void
say(void *context, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	(void)context;
	if (!quiet_say) {
		printf("  %s %#llx %#llx %#llx\n", what, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
	}
}

static const uint8_t *
image_of(void *context, uint64_t id, uint64_t *size)
{
	(void)context;
	if (id == 13) {
		*size = IMAGE13_SIZE;
		return image13;
	}
	if (id == 21) {
		*size = IMAGE21_SIZE;
		return image21;
	}
	return NULL;
}

static void
modem_reset(void)
{
	memset(regs, 0, sizeof(regs));
	memset(&dev.ch, 0, sizeof(dev.ch));
	memset(&chip, 0, sizeof(chip));
	dev.us = dev.ready_at = 0;
	dev.m0 = false;
	dev.events = dev.resets = 0;
	dev.transfer_length = 0;
	dev.out_head = dev.out_tail = 0;
	memset(dev.event_doorbell, 0, sizeof(dev.event_doorbell));
	regs[0x00 / 4] = 0x100;                 /* MHIREGLEN */
	regs[0x08 / 4] = 0x01000000;            /* MHIVER */
	regs[0x10 / 4] = NCH;                   /* MHICFG */
	regs[0x18 / 4] = CHDB;
	regs[0x20 / 4] = ERDB;
	regs[0x28 / 4] = BHI;
	regs[(BHI + 4) / 4] = 1;                /* BHI major version */
	regs[(BHI + 0x28) / 4] = ALIOTH_MHI_EE_PBL;
	arena_used = 0;
}

static const struct alioth_mhi_known known[] = {
	{ 2, ALIOTH_MHI_TO_DEVICE, 1 }, { 3, ALIOTH_MHI_FROM_DEVICE, 1 }, { 20, ALIOTH_MHI_TO_DEVICE, 2 },
	{ 21, ALIOTH_MHI_FROM_DEVICE, 2 }, { 25, ALIOTH_MHI_FROM_DEVICE, 1 },
};

int
main(void)
{
	const struct alioth_mhi_io io = { NULL, BAR, WINDOW, WINDOW_LAST, read32, write32, allocate, delay_us, say };
	static uint8_t sbl_bytes[700000];
	struct alioth_mhi host;
	int error;

	arena = aligned_alloc(4096, ARENA);
	dev.transfer = malloc(8u << 20);
	image13 = malloc(IMAGE13_SIZE);
	image21 = malloc(IMAGE21_SIZE);
	for (size_t i = 0; i < sizeof(sbl_bytes); i++) sbl_bytes[i] = (uint8_t)(i * 31 + 7);
	for (size_t i = 0; i < IMAGE13_SIZE; i++) image13[i] = (uint8_t)(i * 13 + (i >> 16));
	for (size_t i = 0; i < IMAGE21_SIZE; i++) image21[i] = (uint8_t)(i * 7 + 3);
	sbl = sbl_bytes;
	sbl_size = sizeof(sbl_bytes);

	printf("run 1: a whole boot\n");
	modem_reset();
	memset(&host, 0, sizeof(host));
	CHECK(alioth_mhi_probe(&host, &io) == 0, "probe");
	CHECK(host.ee == ALIOTH_MHI_EE_PBL && host.channels == NCH, "probe found PBL, %u channels", host.channels);
	CHECK(alioth_mhi_ready(&host, known, 5, 50) == ETIMEDOUT, "READY before the SBL is in should time out");
	CHECK((error = alioth_mhi_bhi(&host, sbl, sbl_size, 2000)) == 0, "BHI: %d", error);
	CHECK(alioth_mhi_bhi(&host, sbl, sbl_size, 2000) == EALREADY, "a second BHI");
	CHECK((error = alioth_mhi_ready(&host, known, 5, 2000)) == 0, "READY, M0: %d", error);
	CHECK(host.ee == ALIOTH_MHI_EE_SBL && host.state == 2, "SBL and M0 seen as events (%u, %u)", host.ee, host.state);
	CHECK((error = alioth_mhi_sahara(&host, image_of, NULL, 2000)) == 0, "Sahara: %d", error);
	CHECK(chip.waiting == FINISHED && chip.step == sizeof(script) / sizeof(script[0]), "the SBL got through its script (step %u)",
	    chip.step);
	CHECK(chip.most_transfers >= 2, "the 5 MB read came in %u transfer(s), should be split", chip.most_transfers);
	CHECK(host.ee == ALIOTH_MHI_EE_AMSS, "mission mode");
	CHECK(dev.resets == 2 && !dev.ch[2].running && !dev.ch[3].running, "SBL's channels reset (%u)", dev.resets);
	CHECK(host.bandwidth_requests == 1, "the link speed request seen (%u)", host.bandwidth_requests);
	CHECK(!host.poisoned, "not poisoned");
	printf("run 1: %u events, %zu KB of host memory, %llu ms pretend\n", dev.events, arena_used >> 10,
	    (unsigned long long)(dev.us / 1000));

	printf("run 2: the modem fails after its HELLO\n");
	quiet_say = 0;
	modem_reset();
	dev.syserr_after_hello = true;
	memset(&host, 0, sizeof(host));
	CHECK(alioth_mhi_probe(&host, &io) == 0, "probe");
	CHECK(alioth_mhi_bhi(&host, sbl, sbl_size, 2000) == 0, "BHI");
	CHECK(alioth_mhi_ready(&host, known, 5, 2000) == 0, "READY, M0");
	CHECK((error = alioth_mhi_sahara(&host, image_of, NULL, 2000)) == EIO, "Sahara should fail with EIO: %d", error);
	CHECK(host.poisoned, "poisoned");
	CHECK(alioth_mhi_send(&host, 2, "x", 1, true, 10) == EIO, "nothing more is sent");

	printf("run 3: an SBL the modem's PBL does not take\n");
	modem_reset();
	dev.syserr_after_hello = false;
	memset(&host, 0, sizeof(host));
	CHECK(alioth_mhi_probe(&host, &io) == 0, "probe");
	sbl_size--;
	CHECK(alioth_mhi_bhi(&host, sbl, sbl_size + 1, 2000) == EIO, "BHI should be refused");
	sbl_size++;

	printf("test-mhi: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
