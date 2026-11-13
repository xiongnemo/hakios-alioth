/*
 * Sahara, the protocol a Qualcomm chip's boot loader asks its host for images with (the SDX55's, over its MHI
 * channel 2, "SAHARA", once the boot ROM has run what BHI gave it: work/research/gps-sdx55.md). The host's side
 * only, independent of the transport: a packet the chip sent in, the packets to send back out through
 * `send`; images by their Sahara id through `image`. Header-only, so that the kernel (bsd/dev/alioth_mdm.c) and a
 * test on the build machine (devices/redmi-k40/tools/test-sahara.c, against a pretend chip) compile the same code.
 *
 * What it answers (Linux's drivers/accel/qaic/sahara.c for the packets of version 2; READ_DATA64, which Android's
 * ks serves too, for images past 4 GB or chips that ask that way):
 *   HELLO (1)             HELLO_RESP (2): our version 2, its mode
 *   READ_DATA (3), READ_DATA64 (0x12)
 *                         the bytes asked for, in packets of at most SAHARA_PACKET_MAX; RESET (7) when the image
 *                         is not there or the request goes past its end
 *   END_OF_IMAGE (4)      DONE (5) if the chip says it went well; the image is let go either way
 *   DONE_RESP (6)         its mode: 1 (image transfer complete) ends the session, 0 means more images follow
 *   RESET_RESP (8)        nothing
 *   CMD_READY (0x0b)      CMD_SWITCH_MODE (0x0c) to mode 0, image transfer: the SDX55's SBL says HELLO in command
 *                         mode (3) once in the middle of its images, after mdmddr (the OnePlus 8 Pro port's
 *                         sahara_packet, github.com/dekefake/hackios-op8pro, which boots the same SDX55)
 *   MEM_DEBUG (9, 0x10)   RESET: the modem crashed and offers its memory; not taken, it starts again
 */
#ifndef _SYS_ALIOTH_SAHARA_H_
#define _SYS_ALIOTH_SAHARA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAHARA_HELLO            0x01
#define SAHARA_HELLO_RESP       0x02
#define SAHARA_READ_DATA        0x03
#define SAHARA_END_OF_IMAGE     0x04
#define SAHARA_DONE             0x05
#define SAHARA_DONE_RESP        0x06
#define SAHARA_RESET            0x07
#define SAHARA_RESET_RESP       0x08
#define SAHARA_MEM_DEBUG        0x09
#define SAHARA_CMD_READY        0x0b
#define SAHARA_CMD_SWITCH_MODE  0x0c
#define SAHARA_MEM_DEBUG64      0x10
#define SAHARA_READ_DATA64      0x12

#define SAHARA_VERSION          2
#define SAHARA_HELLO_LENGTH     0x30
#define SAHARA_READ_LENGTH      0x14
#define SAHARA_READ64_LENGTH    0x20
#define SAHARA_EOI_LENGTH       0x10
#define SAHARA_DONE_LENGTH      0x08
#define SAHARA_DONE_RESP_LENGTH 0x0c
#define SAHARA_RESET_LENGTH     0x08
#define SAHARA_CMD_READY_LENGTH 0x08
#define SAHARA_SWITCH_LENGTH    0x0c
#define SAHARA_MODE_PENDING     0               /* images to transfer */
#define SAHARA_MODE_COMPLETE    1               /* all transferred */
#define SAHARA_MODE_COMMAND     3               /* the chip wants commands first: switched back to PENDING */
/* what one packet of an image's bytes carries -- each packet is a transfer of its own (`last` true: MHI's IEOT), one
 * element of the SDX55's Sahara channel (alioth_mhi.c SAHARA_SLOT 0x8000). 2026-11-13: with 0xffff-byte packets
 * chained (CHAIN, IEOT only at the read's end or a full ring) the SBL took ten images and then stopped receiving
 * qdsp6sw.mbn's 84 MB (read in one request) for minutes; the OnePlus 8 Pro port, the same SDX55, sends 8 KB
 * elements each with IEOT and moves 88 MB in ~30 s (dekefake/hackios-op8pro hk_mhi_send). */
#define SAHARA_PACKET_MAX       0x8000u
#define SAHARA_PROGRESS         (8u << 20)      /* a line every this many bytes of one read */
/* the most one READ_DATA may ask for: the SDX55's SBL asks for the rest of qdsp6sw.mbn in ONE read (0x501e08c bytes
 * at 0x3000, 2026-11-13; 8 MB here reset the session at image 8) -- the bytes still go out in SAHARA_PACKET_MAX
 * pieces, and the image's own size bounds the read (sahara_read) */
#define SAHARA_REQUEST_MAX      (256u << 20)
#define SAHARA_NO_IMAGE         UINT64_MAX

enum sahara_state { SAHARA_WAITING, SAHARA_TRANSFERRING, SAHARA_FINISHED, SAHARA_FAILED };

struct sahara_ops {
	void *context;
	/* one packet out; `last` ends a transfer (MHI's EOT rather than CHAIN); 0 when it went */
	int (*send)(void *context, const void *bytes, uint32_t length, bool last);
	/* image `id`: its bytes and size, or NULL if there is none */
	const uint8_t *(*image)(void *context, uint64_t id, uint64_t *size);
	void (*say)(void *context, const char *what, uint64_t a, uint64_t b, uint64_t c);
};

struct sahara {
	const struct sahara_ops *ops;
	enum sahara_state state;
	uint64_t active;                        /* the image being read, SAHARA_NO_IMAGE when none */
	const uint8_t *data;
	uint64_t size;
	uint32_t mode;                          /* the chip's, from its HELLO */
	uint32_t images_done, resets, packets;
	uint64_t bytes_sent;
	uint8_t out[SAHARA_HELLO_LENGTH];       /* our own short packets */
};

static inline uint32_t
sahara_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
sahara_le64(const uint8_t *p)
{
	return (uint64_t)sahara_le32(p) | (uint64_t)sahara_le32(p + 4) << 32;
}

static inline void
sahara_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static inline void
sahara_say(struct sahara *s, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	if (s->ops->say != NULL) {
		s->ops->say(s->ops->context, what, a, b, c);
	}
}

static inline void
sahara_init(struct sahara *s, const struct sahara_ops *ops)
{
	s->ops = ops;
	s->state = SAHARA_WAITING;
	s->active = SAHARA_NO_IMAGE;
	s->data = NULL;
	s->size = 0;
	s->mode = 0;
	s->images_done = s->resets = s->packets = 0;
	s->bytes_sent = 0;
}

static inline int
sahara_send_short(struct sahara *s, uint32_t cmd, uint32_t length)
{
	for (uint32_t i = 0; i < sizeof(s->out); i++) {
		s->out[i] = 0;
	}
	sahara_put32(s->out, cmd);
	sahara_put32(s->out + 4, length);
	if (cmd == SAHARA_HELLO_RESP) {
		sahara_put32(s->out + 8, SAHARA_VERSION);
		sahara_put32(s->out + 12, 1);               /* version_compat: 1, as the OnePlus 8 Pro port answers its SDX55 */
		sahara_put32(s->out + 16, 0);               /* status: success */
		sahara_put32(s->out + 20, s->mode);
	} else if (cmd == SAHARA_CMD_SWITCH_MODE) {
		sahara_put32(s->out + 8, SAHARA_MODE_PENDING);
	}
	s->packets++;
	return s->ops->send(s->ops->context, s->out, length, true);
}

static inline int
sahara_reset(struct sahara *s, const char *why, uint64_t a, uint64_t b)
{
	sahara_say(s, why, a, b, s->active);
	s->resets++;
	return sahara_send_short(s, SAHARA_RESET, SAHARA_RESET_LENGTH);
}

/* the bytes [offset, offset + length) of image `id` */
static inline int
sahara_read(struct sahara *s, uint64_t id, uint64_t offset, uint64_t length)
{
	uint64_t sent = 0;

	if (id != s->active) {
		uint64_t size = 0;
		const uint8_t *data;

		if (s->active != SAHARA_NO_IMAGE) {
			return sahara_reset(s, "sahara: an image asked for while another is being read", id, s->active);
		}
		if ((data = s->ops->image(s->ops->context, id, &size)) == NULL) {
			return sahara_reset(s, "sahara: no such image", id, 0);
		}
		s->active = id;
		s->data = data;
		s->size = size;
		sahara_say(s, "sahara: image", id, size, 0);
	}
	if (length == 0 || length > SAHARA_REQUEST_MAX || offset >= s->size || length > s->size - offset) {
		return sahara_reset(s, "sahara: a read past the image or too long", offset, length);
	}
	s->state = SAHARA_TRANSFERRING;
	while (length > 0) {
		uint32_t n = length > SAHARA_PACKET_MAX ? SAHARA_PACKET_MAX : (uint32_t)length;
		int error = s->ops->send(s->ops->context, s->data + offset, n, true);

		if (error != 0) {
			sahara_say(s, "sahara: a send failed: image, bytes of this read sent, error", id, sent, (uint64_t)error);
			s->state = SAHARA_FAILED;
			return error;
		}
		s->packets++;
		s->bytes_sent += n;
		offset += n;
		length -= n;
		sent += n;
		if (length > 0 && sent % SAHARA_PROGRESS < n) {
			sahara_say(s, "sahara: a long read: image, bytes sent, bytes left", id, sent, length);
		}
	}
	return 0;
}

/* One packet from the chip; 0, or an error of `send` (the session is then SAHARA_FAILED). */
static inline int
sahara_input(struct sahara *s, const uint8_t *p, uint32_t length)
{
	uint32_t cmd, plength;

	if (length < 8) {
		return sahara_reset(s, "sahara: a packet shorter than its header", length, 0);
	}
	cmd = sahara_le32(p);
	plength = sahara_le32(p + 4);
	if (plength != length) {
		return sahara_reset(s, "sahara: a packet whose length is not what it says", plength, length);
	}
	switch (cmd) {
	case SAHARA_HELLO:
		if (length != SAHARA_HELLO_LENGTH) {
			return sahara_reset(s, "sahara: a HELLO of the wrong length", length, 0);
		}
		s->mode = sahara_le32(p + 20);
		sahara_say(s, "sahara: HELLO: version, compatible, mode", sahara_le32(p + 8), sahara_le32(p + 12), s->mode);
		if (sahara_le32(p + 12) > SAHARA_VERSION || (s->mode != SAHARA_MODE_PENDING && s->mode != SAHARA_MODE_COMPLETE &&
		    s->mode != SAHARA_MODE_COMMAND)) {
			return sahara_reset(s, "sahara: a HELLO we cannot answer (version, mode)", sahara_le32(p + 12), s->mode);
		}
		s->state = SAHARA_TRANSFERRING;
		return sahara_send_short(s, SAHARA_HELLO_RESP, SAHARA_HELLO_LENGTH);
	case SAHARA_READ_DATA:
		if (length != SAHARA_READ_LENGTH) {
			return sahara_reset(s, "sahara: a READ_DATA of the wrong length", length, 0);
		}
		return sahara_read(s, sahara_le32(p + 8), sahara_le32(p + 12), sahara_le32(p + 16));
	case SAHARA_READ_DATA64:
		if (length != SAHARA_READ64_LENGTH) {
			return sahara_reset(s, "sahara: a READ_DATA64 of the wrong length", length, 0);
		}
		return sahara_read(s, sahara_le64(p + 8), sahara_le64(p + 16), sahara_le64(p + 24));
	case SAHARA_END_OF_IMAGE: {
		uint32_t id, status;

		if (length != SAHARA_EOI_LENGTH) {
			return sahara_reset(s, "sahara: an END_OF_IMAGE of the wrong length", length, 0);
		}
		id = sahara_le32(p + 8);
		status = sahara_le32(p + 12);
		if (s->active != SAHARA_NO_IMAGE && id != s->active) {
			return sahara_reset(s, "sahara: END_OF_IMAGE of another image", id, s->active);
		}
		s->active = SAHARA_NO_IMAGE;
		s->data = NULL;
		s->size = 0;
		sahara_say(s, "sahara: END_OF_IMAGE: image, status", id, status, 0);
		if (status != 0) {
			return 0;                       /* (the chip says what it does next) */
		}
		s->images_done++;
		return sahara_send_short(s, SAHARA_DONE, SAHARA_DONE_LENGTH);
	}
	case SAHARA_DONE_RESP:
		if (length != SAHARA_DONE_RESP_LENGTH) {
			return sahara_reset(s, "sahara: a DONE_RESP of the wrong length", length, 0);
		}
		s->mode = sahara_le32(p + 8);
		sahara_say(s, "sahara: DONE_RESP: mode, images so far", s->mode, s->images_done, 0);
		if (s->mode == SAHARA_MODE_COMPLETE) {
			s->state = SAHARA_FINISHED;
		}
		return 0;
	case SAHARA_RESET_RESP:
		return 0;
	case SAHARA_CMD_READY:
		/* (a chip that said HELLO in command mode: no commands are sent, it is switched to image transfer) */
		sahara_say(s, "sahara: CMD_READY: switched to image transfer", s->mode, 0, 0);
		s->mode = SAHARA_MODE_PENDING;
		return sahara_send_short(s, SAHARA_CMD_SWITCH_MODE, SAHARA_SWITCH_LENGTH);
	case SAHARA_MEM_DEBUG:
	case SAHARA_MEM_DEBUG64:
		return sahara_reset(s, "sahara: the modem crashed and offers its memory: not taken, reset", cmd, length);
	default:
		return sahara_reset(s, "sahara: a command we do not know", cmd, length);
	}
}

#endif
