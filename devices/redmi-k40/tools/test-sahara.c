/*
 * The host's side of Sahara (devices/redmi-k40/overlays/xnu/files/bsd/sys/alioth_sahara.h) against a pretend
 * chip, on the build machine: a whole session of three images (one asked for in 32-bit reads, one in 64-bit reads
 * and pieces bigger than a packet, one missing), what comes out compared byte for byte, and the malformed
 * packets that must be answered with RESET.
 * usage (WSL, in devices/redmi-k40/tools):
 *   cc -std=c11 -Wall -Wextra -I ../overlays/xnu/files/bsd/sys -o /tmp/test-sahara test-sahara.c && /tmp/test-sahara
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alioth_sahara.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* what the host sent, for the pretend chip to look at */
static uint8_t sent[1 << 22];
static uint32_t sent_length, sends, lasts;
static uint32_t last_cmd;

static int
send_out(void *context, const void *bytes, uint32_t length, bool last)
{
	(void)context;
	if (sent_length + length > sizeof(sent)) {
		return 1;
	}
	memcpy(sent + sent_length, bytes, length);
	sent_length += length;
	sends++;
	lasts += last;
	if (length >= 4) {
		last_cmd = sahara_le32(bytes);
	}
	return 0;
}

static uint8_t image1[100000], image5[300000];

static const uint8_t *
image_of(void *context, uint64_t id, uint64_t *size)
{
	(void)context;
	if (id == 1) {
		*size = sizeof(image1);
		return image1;
	}
	if (id == 5) {
		*size = sizeof(image5);
		return image5;
	}
	return NULL;
}

static void
say(void *context, const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	(void)context;
	printf("  %s %#llx %#llx %#llx\n", what, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}

static void
put32(uint8_t *p, uint32_t v)
{
	sahara_put32(p, v);
}

static void
put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static int
chip(struct sahara *s, uint32_t cmd, uint32_t length, const uint32_t *words, unsigned n)
{
	uint8_t p[64] = { 0 };

	put32(p, cmd);
	put32(p + 4, length);
	for (unsigned i = 0; i < n; i++) {
		put32(p + 8 + 4 * i, words[i]);
	}
	sent_length = 0;
	return sahara_input(s, p, length);
}

int
main(void)
{
	struct sahara_ops ops = { NULL, send_out, image_of, say };
	struct sahara s;
	uint8_t p[64];

	for (size_t i = 0; i < sizeof(image1); i++) image1[i] = (uint8_t)(i * 7 + 1);
	for (size_t i = 0; i < sizeof(image5); i++) image5[i] = (uint8_t)(i * 13 + 5);
	sahara_init(&s, &ops);

	/* HELLO: version 2, compatible 1, mode pending -> HELLO_RESP with mode pending */
	{ uint32_t w[] = { 2, 1, 0x400, SAHARA_MODE_PENDING }; CHECK(chip(&s, SAHARA_HELLO, SAHARA_HELLO_LENGTH, w, 4) == 0, "hello"); }
	CHECK(sent_length == SAHARA_HELLO_LENGTH && sahara_le32(sent) == SAHARA_HELLO_RESP && sahara_le32(sent + 4) == 0x30 &&
	    sahara_le32(sent + 8) == 2 && sahara_le32(sent + 16) == 0 && sahara_le32(sent + 20) == 0, "hello resp");

	/* image 1 in 32-bit reads: the first 80 bytes (its header), then all of it in pieces of 40000 */
	{ uint32_t w[] = { 1, 0, 80 }; CHECK(chip(&s, SAHARA_READ_DATA, SAHARA_READ_LENGTH, w, 3) == 0, "read 1"); }
	CHECK(sent_length == 80 && memcmp(sent, image1, 80) == 0, "image 1 header bytes");
	for (uint32_t off = 0; off < sizeof(image1); off += 40000) {
		uint32_t len = sizeof(image1) - off < 40000 ? (uint32_t)(sizeof(image1) - off) : 40000;
		uint32_t w[] = { 1, off, len };

		CHECK(chip(&s, SAHARA_READ_DATA, SAHARA_READ_LENGTH, w, 3) == 0, "read 1 at %u", off);
		CHECK(sent_length == len && memcmp(sent, image1 + off, len) == 0, "image 1 bytes at %u", off);
	}
	/* a read past its end: RESET */
	{ uint32_t w[] = { 1, 99990, 20 }; chip(&s, SAHARA_READ_DATA, SAHARA_READ_LENGTH, w, 3); }
	CHECK(last_cmd == SAHARA_RESET && sent_length == SAHARA_RESET_LENGTH, "read past the end -> reset");
	/* another image while image 1 is open: RESET */
	{ uint32_t w[] = { 5, 0, 10 }; chip(&s, SAHARA_READ_DATA, SAHARA_READ_LENGTH, w, 3); }
	CHECK(last_cmd == SAHARA_RESET, "second image while one is open -> reset");
	/* END_OF_IMAGE of image 1, success -> DONE */
	{ uint32_t w[] = { 1, 0 }; CHECK(chip(&s, SAHARA_END_OF_IMAGE, SAHARA_EOI_LENGTH, w, 2) == 0, "eoi 1"); }
	CHECK(last_cmd == SAHARA_DONE && sent_length == SAHARA_DONE_LENGTH, "eoi -> done");
	/* DONE_RESP, mode pending: more images */
	{ uint32_t w[] = { SAHARA_MODE_PENDING }; CHECK(chip(&s, SAHARA_DONE_RESP, SAHARA_DONE_RESP_LENGTH, w, 1) == 0, "done resp"); }
	CHECK(s.state != SAHARA_FINISHED, "not finished after a pending DONE_RESP");

	/* image 5 in 64-bit reads, one of 200000 bytes: more than a packet, so 4 packets */
	memset(p, 0, sizeof(p));
	put32(p, SAHARA_READ_DATA64);
	put32(p + 4, SAHARA_READ64_LENGTH);
	put64(p + 8, 5);
	put64(p + 16, 1000);
	put64(p + 24, 200000);
	sent_length = 0;
	sends = lasts = 0;
	CHECK(sahara_input(&s, p, SAHARA_READ64_LENGTH) == 0, "read64");
	CHECK(sent_length == 200000 && memcmp(sent, image5 + 1000, 200000) == 0, "image 5 bytes");
	CHECK(sends == 4 && lasts == 1, "200000 bytes in %u packets (%u ending a transfer), not 4 (1)", sends, lasts);

	/* a missing image: RESET (after closing 5) */
	{ uint32_t w[] = { 5, 0 }; chip(&s, SAHARA_END_OF_IMAGE, SAHARA_EOI_LENGTH, w, 2); }
	{ uint32_t w[] = { 9, 0, 16 }; chip(&s, SAHARA_READ_DATA, SAHARA_READ_LENGTH, w, 3); }
	CHECK(last_cmd == SAHARA_RESET, "missing image -> reset");

	/* malformed: a length field that lies, a short packet, an unknown command */
	{ uint8_t q[0x14] = { 0 }; put32(q, SAHARA_READ_DATA); put32(q + 4, 0x18);
	  sent_length = 0; sahara_input(&s, q, sizeof(q)); CHECK(last_cmd == SAHARA_RESET, "lying length -> reset"); }
	{ uint8_t q[4] = { 1, 0, 0, 0 }; sent_length = 0; sahara_input(&s, q, 4); CHECK(last_cmd == SAHARA_RESET, "short -> reset"); }
	{ CHECK(chip(&s, 0x55, 8, NULL, 0) == 0 && last_cmd == SAHARA_RESET, "unknown -> reset"); }

	/* a HELLO asking for a version we do not speak: RESET */
	{ uint32_t w[] = { 3, 3, 0x400, 0 }; chip(&s, SAHARA_HELLO, SAHARA_HELLO_LENGTH, w, 4); CHECK(last_cmd == SAHARA_RESET, "version 3 -> reset"); }

	/* the end: DONE_RESP with mode complete finishes the session */
	{ uint32_t w[] = { SAHARA_MODE_COMPLETE }; chip(&s, SAHARA_DONE_RESP, SAHARA_DONE_RESP_LENGTH, w, 1); }
	CHECK(s.state == SAHARA_FINISHED, "finished");
	printf("sahara_test: %s (%u images done, %u resets, %llu bytes sent)\n", failures ? "FAILED" : "all passed",
	    s.images_done, s.resets, (unsigned long long)s.bytes_sent);
	return failures != 0;
}
