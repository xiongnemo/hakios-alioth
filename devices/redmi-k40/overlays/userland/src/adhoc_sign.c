/*
 * adhoc_sign.c: an ad hoc code signature for an arm64 Mach-O (see adhoc_sign.h for why).
 *
 * What is written is what codesign/ldid write for an ad hoc signature, in their layout, so that the kernel's code
 * signing reads it as theirs: at the end of __LINKEDIT, 16-aligned, an embedded-signature SuperBlob (0xfade0cc0) of
 *   slot 0       CodeDirectory (0xfade0c02) version 0x20400: ad hoc flag, SHA-256, 4 KB pages; execSeg = the
 *                executable segments' file range, MAIN_BINARY for an executable (and the entitlements' own bits);
 *                special slots: 1 the __TEXT,__info_plist section if any, 2 the requirements, 5 the XML
 *                entitlements, 7 the DER entitlements
 *   slot 2       Requirements (0xfade0c01): an empty set, as codesign's ad hoc
 *   slot 5       Entitlements (0xfade7171): the XML plist as given
 *   slot 7       DER entitlements (0xfade7172): a SET of SEQUENCE { UTF8String key, value }, sorted by their bytes
 * LC_CODE_SIGNATURE is the last load command; __LINKEDIT grows over the signature (vmsize to 16 KB pages). The
 * computer's ldid (Procursus 2.1.5) was the reference for the layout and the DER bytes; no code of it is here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "adhoc_sign.h"

#define MH_MAGIC_64           0xfeedfacfu
#define FAT_MAGIC             0xcafebabeu
#define FAT_MAGIC_64          0xcafebabfu
#define CPU_TYPE_ARM64        0x0100000cu
#define MH_EXECUTE            2u
#define LC_SYMTAB             0x2u
#define LC_SEGMENT_64         0x19u
#define LC_CODE_SIGNATURE     0x1du
#define LC_ENCRYPTION_INFO_64 0x2cu
#define VM_PROT_EXECUTE       4u
#define PAGE                  4096u
#define SEG_ALIGN             0x4000u            /* (arm64: 16 KB pages) */

#define CSMAGIC_REQUIREMENTS      0xfade0c01u
#define CSMAGIC_CODEDIRECTORY     0xfade0c02u
#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0u
#define CSMAGIC_ENTITLEMENTS      0xfade7171u
#define CSMAGIC_DER_ENTITLEMENTS  0xfade7172u
#define CS_ADHOC                  0x2u
#define CD_SIZE                   88u            /* (version 0x20400, with the blob header) */

#define EXECSEG_MAIN_BINARY    0x1u
#define EXECSEG_ALLOW_UNSIGNED 0x10u
#define EXECSEG_DEBUGGER       0x20u
#define EXECSEG_JIT            0x40u
#define EXECSEG_SKIP_LV        0x80u
#define EXECSEG_CAN_LOAD_CDHASH 0x100u
#define EXECSEG_CAN_EXEC_CDHASH 0x200u

static void
say_err(char *err, size_t errlen, const char *fmt, const char *arg)
{
	if (err != NULL && errlen > 0) {
		snprintf(err, errlen, fmt, arg ? arg : "");
	}
}

/* ---------------------------------------------------------------- SHA-256 (FIPS 180-4) */

typedef struct {
	uint32_t h[8];
	uint64_t bits;
	unsigned char block[64];
	size_t used;
} sha256_ctx;

static const uint32_t sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(sha256_ctx *c, const unsigned char *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
	for (int i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
	}
	c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d;
	c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

static void
sha256(const void *data, size_t len, unsigned char out[32])
{
	static const uint32_t init[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	sha256_ctx c;
	const unsigned char *p = data;
	unsigned char pad[128] = { 0x80 };
	size_t full = len & ~(size_t)63, rest = len - full, padlen;

	memcpy(c.h, init, sizeof(init));
	for (size_t i = 0; i < full; i += 64) {
		sha256_block(&c, p + i);
	}
	padlen = rest < 56 ? 64 : 128;
	memset(pad, 0, sizeof(pad));
	memcpy(pad, p + full, rest);
	pad[rest] = 0x80;
	c.bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		pad[padlen - 1 - i] = (unsigned char)(c.bits >> (8 * i));
	}
	sha256_block(&c, pad);
	if (padlen == 128) {
		sha256_block(&c, pad + 64);
	}
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)(c.h[i] >> 24), out[4 * i + 1] = (unsigned char)(c.h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c.h[i] >> 8), out[4 * i + 3] = (unsigned char)c.h[i];
	}
}

/* ---------------------------------------------------------------- byte buffers */

struct buf {
	unsigned char *p;
	size_t n, cap;
	int failed;
};

static void
buf_put(struct buf *b, const void *data, size_t n)
{
	if (b->failed) {
		return;
	}
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		unsigned char *p;

		while (cap < b->n + n) {
			cap *= 2;
		}
		p = realloc(b->p, cap);
		if (p == NULL) {
			b->failed = 1;
			return;
		}
		b->p = p, b->cap = cap;
	}
	if (n > 0) {
		memcpy(b->p + b->n, data, n);
	}
	b->n += n;
}

static void
buf_byte(struct buf *b, unsigned value)
{
	unsigned char c = (unsigned char)value;

	buf_put(b, &c, 1);
}

static void
buf_be32(struct buf *b, uint32_t v)
{
	unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };

	buf_put(b, x, 4);
}

static void
buf_be64(struct buf *b, uint64_t v)
{
	buf_be32(b, (uint32_t)(v >> 32));
	buf_be32(b, (uint32_t)v);
}

static unsigned
nbytes(uint64_t v)
{
	unsigned n = 1;

	while (n < 8 && (v >> (8 * n)) != 0) {
		n++;
	}
	return n;
}

/* a DER length: short form below 128, else 0x80|count and the count's bytes, big-endian */
static void
der_len(struct buf *b, uint64_t len)
{
	if (len < 128) {
		buf_byte(b, (unsigned)len);
		return;
	}
	unsigned k = nbytes(len);

	buf_byte(b, 0x80 | k);
	while (k-- > 0) {
		buf_byte(b, (unsigned)(len >> (8 * k)));
	}
}

static void
der_tlv(struct buf *b, unsigned tag, const void *data, size_t n)
{
	buf_byte(b, tag);
	der_len(b, n);
	buf_put(b, data, n);
}

/* ---------------------------------------------------------------- XML plist -> DER (entitlements' subset) */

struct parse {
	const char *p, *end;
	char *err;
	size_t errlen;
	int depth;
	uint64_t execseg;       /* bits from top-level true booleans (ldid's list) */
	int bad;
};

static void
fail(struct parse *ps, const char *what)
{
	if (!ps->bad) {
		say_err(ps->err, ps->errlen, "entitlements: %s", what);
	}
	ps->bad = 1;
}

static int
starts(struct parse *ps, const char *s)
{
	size_t n = strlen(s);

	return (size_t)(ps->end - ps->p) >= n && memcmp(ps->p, s, n) == 0;
}

/* whitespace, comments, <?...?> and <!DOCTYPE ...> */
static void
skip_ws(struct parse *ps)
{
	for (;;) {
		while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n')) {
			ps->p++;
		}
		const char *close = NULL;

		if (starts(ps, "<!--")) {
			close = "-->";
		} else if (starts(ps, "<?")) {
			close = "?>";
		} else if (starts(ps, "<!")) {
			close = ">";
		} else {
			return;
		}
		const char *q = ps->p + 2;
		size_t cn = strlen(close);

		while (q + cn <= ps->end && memcmp(q, close, cn) != 0) {
			q++;
		}
		if (q + cn > ps->end) {
			fail(ps, "unterminated markup");
			ps->p = ps->end;
			return;
		}
		ps->p = q + cn;
	}
}

/* "<NAME>" or "<NAME/>" (attributes skipped) at the cursor: the name into NAME, *selfclose; 0 when not a tag */
static int
open_tag(struct parse *ps, char *name, size_t max, int *selfclose)
{
	const char *q;
	size_t n = 0;

	skip_ws(ps);
	if (ps->p >= ps->end || *ps->p != '<' || (ps->p + 1 < ps->end && ps->p[1] == '/')) {
		return 0;
	}
	q = ps->p + 1;
	while (q < ps->end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) && n + 1 < max) {
		name[n++] = *q++;
	}
	name[n] = 0;
	while (q < ps->end && *q != '>') {
		q++;
	}
	if (q >= ps->end) {
		fail(ps, "unterminated tag");
		return 0;
	}
	*selfclose = q[-1] == '/';
	ps->p = q + 1;
	return 1;
}

static int
close_tag(struct parse *ps, const char *name)
{
	char want[32];

	skip_ws(ps);
	snprintf(want, sizeof(want), "</%s>", name);
	if (!starts(ps, want)) {
		return 0;
	}
	ps->p += strlen(want);
	return 1;
}

static void
put_utf8(struct buf *b, unsigned long c)
{
	if (c < 0x80) {
		buf_byte(b, (unsigned)c);
	} else if (c < 0x800) {
		buf_byte(b, 0xc0 | (unsigned)(c >> 6)), buf_byte(b, 0x80 | (unsigned)(c & 0x3f));
	} else if (c < 0x10000) {
		buf_byte(b, 0xe0 | (unsigned)(c >> 12)), buf_byte(b, 0x80 | (unsigned)((c >> 6) & 0x3f));
		buf_byte(b, 0x80 | (unsigned)(c & 0x3f));
	} else {
		buf_byte(b, 0xf0 | (unsigned)(c >> 18)), buf_byte(b, 0x80 | (unsigned)((c >> 12) & 0x3f));
		buf_byte(b, 0x80 | (unsigned)((c >> 6) & 0x3f)), buf_byte(b, 0x80 | (unsigned)(c & 0x3f));
	}
}

/* the text up to "</NAME>", entities decoded, into OUT (the closing tag consumed) */
static void
text_until(struct parse *ps, const char *name, struct buf *out)
{
	char want[32];

	snprintf(want, sizeof(want), "</%s>", name);
	while (ps->p < ps->end && !starts(ps, want)) {
		if (*ps->p != '&') {
			buf_byte(out, (unsigned char)*ps->p++);
			continue;
		}
		const char *semi = memchr(ps->p, ';', (size_t)(ps->end - ps->p));

		if (semi == NULL || semi - ps->p > 12) {
			fail(ps, "a bad entity");
			return;
		}
		size_t n = (size_t)(semi - ps->p - 1);
		const char *e = ps->p + 1;

		if (n == 2 && memcmp(e, "lt", 2) == 0) {
			buf_byte(out, '<');
		} else if (n == 2 && memcmp(e, "gt", 2) == 0) {
			buf_byte(out, '>');
		} else if (n == 3 && memcmp(e, "amp", 3) == 0) {
			buf_byte(out, '&');
		} else if (n == 4 && memcmp(e, "quot", 4) == 0) {
			buf_byte(out, '"');
		} else if (n == 4 && memcmp(e, "apos", 4) == 0) {
			buf_byte(out, '\'');
		} else if (n >= 2 && e[0] == '#') {
			char num[16];

			memcpy(num, e + 1, n - 1);
			num[n - 1] = 0;
			put_utf8(out, num[0] == 'x' ? strtoul(num + 1, NULL, 16) : strtoul(num, NULL, 10));
		} else {
			fail(ps, "an unknown entity");
			return;
		}
		ps->p = semi + 1;
	}
	if (!starts(ps, want)) {
		fail(ps, "a tag not closed");
		return;
	}
	ps->p += strlen(want);
}

static int
b64_value(int c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	} else if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	} else if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	} else if (c == '+') {
		return 62;
	} else if (c == '/') {
		return 63;
	}
	return -1;
}

struct pair {
	unsigned char *p;
	size_t n;
};

static int
pair_cmp(const void *a, const void *b)
{
	const struct pair *x = a, *y = b;
	size_t n = x->n < y->n ? x->n : y->n;
	int c = memcmp(x->p, y->p, n);

	return c != 0 ? c : (x->n > y->n) - (x->n < y->n);
}

static void parse_value(struct parse *ps, struct buf *out, int *is_true);

static void
parse_dict(struct parse *ps, struct buf *out)
{
	struct pair *pairs = NULL;
	size_t count = 0, cap = 0;
	char name[16];
	int selfclose;
	struct buf all = { 0 };

	ps->depth++;
	while (!ps->bad && !close_tag(ps, "dict")) {
		struct buf key = { 0 }, value = { 0 }, pair = { 0 }, seq = { 0 };
		int is_true = 0;

		if (!open_tag(ps, name, sizeof(name), &selfclose) || strcmp(name, "key") != 0) {
			fail(ps, "a dict entry without <key>");
			break;
		}
		if (!selfclose) {
			text_until(ps, "key", &key);
		}
		parse_value(ps, &value, &is_true);
		if (ps->depth == 1 && is_true) {
			static const struct { const char *key; uint64_t bit; } bits[] = {
				{ "get-task-allow", EXECSEG_ALLOW_UNSIGNED }, { "run-unsigned-code", EXECSEG_ALLOW_UNSIGNED },
				{ "com.apple.private.cs.debugger", EXECSEG_DEBUGGER }, { "dynamic-codesigning", EXECSEG_JIT },
				{ "com.apple.private.skip-library-validation", EXECSEG_SKIP_LV },
				{ "com.apple.private.amfi.can-load-cdhash", EXECSEG_CAN_LOAD_CDHASH },
				{ "com.apple.private.amfi.can-execute-cdhash", EXECSEG_CAN_EXEC_CDHASH },
			};

			for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
				if (key.n == strlen(bits[i].key) && memcmp(key.p, bits[i].key, key.n) == 0) {
					ps->execseg |= bits[i].bit;
				}
			}
		}
		der_tlv(&pair, 0x0c, key.p, key.n);
		buf_put(&pair, value.p, value.n);
		der_tlv(&seq, 0x30, pair.p, pair.n);
		free(key.p), free(value.p), free(pair.p);
		if (seq.failed || key.failed || value.failed) {
			fail(ps, "out of memory");
		}
		if (count == cap) {
			cap = cap ? cap * 2 : 16;
			pairs = realloc(pairs, cap * sizeof(*pairs));
			if (pairs == NULL) {
				fail(ps, "out of memory");
				free(seq.p);
				break;
			}
		}
		pairs[count].p = seq.p, pairs[count].n = seq.n;
		count++;
	}
	ps->depth--;
	if (count > 0) {
		qsort(pairs, count, sizeof(*pairs), pair_cmp);
	}
	for (size_t i = 0; i < count; i++) {
		buf_put(&all, pairs[i].p, pairs[i].n);
		free(pairs[i].p);
	}
	free(pairs);
	der_tlv(out, 0x31, all.p, all.n);
	free(all.p);
}

static void
parse_value(struct parse *ps, struct buf *out, int *is_true)
{
	char name[16];
	int selfclose;

	*is_true = 0;
	if (!open_tag(ps, name, sizeof(name), &selfclose)) {
		fail(ps, "a value expected");
		return;
	}
	if (strcmp(name, "dict") == 0) {
		if (selfclose) {
			der_tlv(out, 0x31, NULL, 0);
		} else {
			parse_dict(ps, out);
		}
	} else if (strcmp(name, "array") == 0) {
		struct buf all = { 0 };

		while (!selfclose && !ps->bad && !close_tag(ps, "array")) {
			struct buf item = { 0 };
			int t;

			parse_value(ps, &item, &t);
			buf_put(&all, item.p, item.n);
			free(item.p);
		}
		der_tlv(out, 0x30, all.p, all.n);
		free(all.p);
	} else if (strcmp(name, "string") == 0) {
		struct buf s = { 0 };

		if (!selfclose) {
			text_until(ps, "string", &s);
		}
		der_tlv(out, 0x0c, s.p, s.n);
		free(s.p);
	} else if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
		unsigned char v = name[0] == 't';

		if (!selfclose && !close_tag(ps, name)) {
			fail(ps, "a boolean with content");
		}
		der_tlv(out, 0x01, &v, 1);
		*is_true = v;
	} else if (strcmp(name, "integer") == 0) {
		struct buf s = { 0 };
		unsigned char be[8];
		unsigned long long v;
		unsigned k;

		if (!selfclose) {
			text_until(ps, "integer", &s);
		}
		buf_byte(&s, 0);
		if (s.p == NULL || s.p[0] == '-') {
			fail(ps, "a negative or empty integer");
			free(s.p);
			return;
		}
		v = strtoull((char *)s.p, NULL, 0);
		free(s.p);
		k = nbytes(v);
		for (unsigned i = 0; i < k; i++) {
			be[i] = (unsigned char)(v >> (8 * (k - 1 - i)));
		}
		der_tlv(out, 0x02, be, k);
	} else if (strcmp(name, "data") == 0) {
		struct buf s = { 0 }, bytes = { 0 };
		unsigned acc = 0;
		int bits = 0;

		if (!selfclose) {
			text_until(ps, "data", &s);
		}
		for (size_t i = 0; i < s.n; i++) {
			int v = b64_value(s.p[i]);

			if (v < 0) {
				continue;       /* (whitespace and '=') */
			}
			acc = acc << 6 | (unsigned)v, bits += 6;
			if (bits >= 8) {
				bits -= 8;
				buf_byte(&bytes, (acc >> bits) & 0xff);
			}
		}
		der_tlv(out, 0x04, bytes.p, bytes.n);
		free(s.p), free(bytes.p);
	} else {
		fail(ps, "a value type entitlements do not have (real, date)");
	}
}

static int
der_parse(const char *xml, size_t len, struct buf *out, uint64_t *execseg, char *err, size_t errlen)
{
	struct parse ps = { xml, xml + len, err, errlen, 0, 0, 0 };
	char name[16];
	int selfclose, t, wrapped = 0;
	const char *save;

	skip_ws(&ps);
	save = ps.p;
	if (open_tag(&ps, name, sizeof(name), &selfclose) && strcmp(name, "plist") == 0) {
		wrapped = 1;
	} else {
		ps.p = save;
	}
	parse_value(&ps, out, &t);
	if (!ps.bad && wrapped && !close_tag(&ps, "plist")) {
		fail(&ps, "no </plist>");
	}
	if (!ps.bad && out->failed) {
		fail(&ps, "out of memory");
	}
	if (!ps.bad && (out->n == 0 || out->p[0] != 0x31)) {
		fail(&ps, "not a dict");
	}
	if (execseg != NULL) {
		*execseg = ps.execseg;
	}
	return ps.bad ? -1 : 0;
}

int
adhoc_der(const char *xml, size_t len, unsigned char **der, size_t *der_len_out, char *err, size_t errlen)
{
	struct buf out = { 0 };

	if (der_parse(xml, len, &out, NULL, err, errlen) != 0) {
		free(out.p);
		return -1;
	}
	*der = out.p, *der_len_out = out.n;
	return 0;
}

/* ---------------------------------------------------------------- Mach-O */

static uint32_t
rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
rd64(const unsigned char *p)
{
	return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void
wr32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

static void
wr64(unsigned char *p, uint64_t v)
{
	wr32(p, (uint32_t)v);
	wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t
be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
be64(const unsigned char *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static int
read_file(const char *path, unsigned char **data, size_t *len, mode_t *mode)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	unsigned char *p;
	size_t got = 0;

	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, &st) != 0 || st.st_size < 32) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	p = malloc((size_t)st.st_size);
	if (p == NULL) {
		close(fd);
		return -1;
	}
	while (got < (size_t)st.st_size) {
		ssize_t r = read(fd, p + got, (size_t)st.st_size - got);

		if (r <= 0) {
			free(p);
			close(fd);
			errno = EIO;
			return -1;
		}
		got += (size_t)r;
	}
	close(fd);
	*data = p, *len = got;
	if (mode != NULL) {
		*mode = st.st_mode & 07777;
	}
	return 0;
}

/* the arm64 slice of a fat file moved to the start (the file becomes thin); 0, or -1 when there is none */
static int
thin_arm64(unsigned char *data, size_t *len)
{
	uint32_t magic = be32(data), n = be32(data + 4);
	int wide = magic == FAT_MAGIC_64;
	size_t entry = wide ? 32 : 20;
	uint64_t pick_off = 0, pick_size = 0;
	int found = 0;

	if (magic != FAT_MAGIC && magic != FAT_MAGIC_64) {
		return 0;
	}
	if (n == 0 || n > 32 || 8 + n * entry > *len) {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		const unsigned char *a = data + 8 + i * entry;
		uint32_t type = be32(a), sub = be32(a + 4) & 0x00ffffff;
		uint64_t off = wide ? be64(a + 8) : be32(a + 8), size = wide ? be64(a + 16) : be32(a + 12);

		/* (plain arm64 first: this phone has no pointer authentication for arm64e) */
		if (type == CPU_TYPE_ARM64 && (!found || sub == 0) && off + size <= *len) {
			pick_off = off, pick_size = size, found = 1 + (sub == 0);
		}
	}
	if (!found || found == 1) {
		return found ? 0 : -1;   /* (only arm64e: kept as found, the caller's checks decide) */
	}
	memmove(data, data + pick_off, (size_t)pick_size);
	*len = (size_t)pick_size;
	return 0;
}

struct macho {
	unsigned char *d;
	size_t len;
	uint32_t ncmds, sizeofcmds, filetype;
	size_t linkedit;        /* offset of __LINKEDIT's segment command, 0 if none */
	size_t cs;              /* offset of LC_CODE_SIGNATURE, 0 if none */
	uint32_t cryptid;
	uint64_t strend;        /* end of the string table */
	uint64_t min_sect;      /* the lowest file offset of a section: where the load commands must end */
	uint64_t exec_left, exec_right;
	uint64_t info_off, info_size;
};

static int
macho_scan(struct macho *m, char *err, size_t errlen)
{
	const unsigned char *d = m->d;
	size_t at = 32;

	if (m->len < 32 || rd32(d) != MH_MAGIC_64 || rd32(d + 4) != CPU_TYPE_ARM64) {
		say_err(err, errlen, "not an arm64 Mach-O%s", "");
		return -1;
	}
	m->filetype = rd32(d + 12), m->ncmds = rd32(d + 16), m->sizeofcmds = rd32(d + 20);
	m->linkedit = m->cs = 0, m->cryptid = 0, m->strend = 0, m->min_sect = UINT64_MAX;
	m->exec_left = UINT64_MAX, m->exec_right = 0, m->info_off = m->info_size = 0;
	if (32 + (size_t)m->sizeofcmds > m->len) {
		say_err(err, errlen, "load commands past the end%s", "");
		return -1;
	}
	for (uint32_t i = 0; i < m->ncmds; i++) {
		uint32_t cmd, size;

		if (at + 8 > 32 + (size_t)m->sizeofcmds) {
			say_err(err, errlen, "a damaged load command%s", "");
			return -1;
		}
		cmd = rd32(d + at), size = rd32(d + at + 4);
		if (size < 8 || at + size > 32 + (size_t)m->sizeofcmds) {
			say_err(err, errlen, "a damaged load command%s", "");
			return -1;
		}
		if (cmd == LC_SEGMENT_64 && size >= 72) {
			uint64_t fileoff = rd64(d + at + 40), filesize = rd64(d + at + 48);
			uint32_t initprot = rd32(d + at + 60), nsects = rd32(d + at + 64);

			if (memcmp(d + at + 8, "__LINKEDIT", 11) == 0) {
				m->linkedit = at;
			}
			if (initprot & VM_PROT_EXECUTE) {
				if (fileoff < m->exec_left) {
					m->exec_left = fileoff;
				}
				if (fileoff + filesize > m->exec_right) {
					m->exec_right = fileoff + filesize;
				}
			}
			for (uint32_t s = 0; s < nsects && 72 + (s + 1) * 80 <= size; s++) {
				const unsigned char *sec = d + at + 72 + s * 80;
				uint32_t offset = rd32(sec + 48), type = rd32(sec + 64) & 0xff;

				if (offset != 0 && type != 0x1 && type != 0xc && type != 0x12 && offset < m->min_sect) {
					m->min_sect = offset;      /* (not a zero-fill section: one with bytes in the file) */
				}
				if (memcmp(sec, "__info_plist", 13) == 0 && memcmp(sec + 16, "__TEXT", 7) == 0) {
					m->info_off = offset, m->info_size = rd64(sec + 40);
				}
			}
		} else if (cmd == LC_CODE_SIGNATURE && size >= 16) {
			m->cs = at;
		} else if (cmd == LC_ENCRYPTION_INFO_64 && size >= 20) {
			m->cryptid = rd32(d + at + 16);
		} else if (cmd == LC_SYMTAB && size >= 24) {
			m->strend = (uint64_t)rd32(d + at + 16) + rd32(d + at + 20);
		}
		at += size;
	}
	if (m->exec_left == UINT64_MAX) {
		m->exec_left = m->exec_right = 0;
	}
	return 0;
}

static int
load(const char *path, struct macho *m, mode_t *mode, char *err, size_t errlen)
{
	memset(m, 0, sizeof(*m));
	if (read_file(path, &m->d, &m->len, mode) != 0) {
		say_err(err, errlen, "cannot read it: %s", strerror(errno));
		return -1;
	}
	if (thin_arm64(m->d, &m->len) != 0) {
		say_err(err, errlen, "a fat file without arm64%s", "");
		free(m->d);
		m->d = NULL;
		return -1;
	}
	if (macho_scan(m, err, errlen) != 0) {
		free(m->d);
		m->d = NULL;
		return -1;
	}
	return 0;
}

int
adhoc_is_macho(const char *path)
{
	unsigned char h[8];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		return 0;
	}
	n = read(fd, h, sizeof(h));
	close(fd);
	if (n < 8) {
		return 0;
	}
	if (rd32(h) == MH_MAGIC_64 || rd32(h) == 0xfeedfaceu) {
		return 1;
	}
	/* (fat -- and not a Java class, whose magic is the same: a fat file has few architectures) */
	return (be32(h) == FAT_MAGIC || be32(h) == FAT_MAGIC_64) && be32(h + 4) > 0 && be32(h + 4) < 32;
}

int
adhoc_is_encrypted(const char *path)
{
	struct macho m;
	int encrypted;

	if (load(path, &m, NULL, NULL, 0) != 0) {
		return 0;
	}
	encrypted = m.cryptid != 0;
	free(m.d);
	return encrypted;
}

char *
adhoc_entitlements(const char *path, size_t *len)
{
	struct macho m;
	char *out = NULL;

	if (load(path, &m, NULL, NULL, 0) != 0) {
		return NULL;
	}
	if (m.cs != 0) {
		uint64_t off = rd32(m.d + m.cs + 8), size = rd32(m.d + m.cs + 12);

		if (off + size <= m.len && size >= 12 && be32(m.d + off) == CSMAGIC_EMBEDDED_SIGNATURE) {
			const unsigned char *sb = m.d + off;
			uint32_t count = be32(sb + 8);

			for (uint32_t i = 0; i < count && 12 + (i + 1) * 8 <= size; i++) {
				uint32_t type = be32(sb + 12 + i * 8), boff = be32(sb + 16 + i * 8);

				if (type == 5 && boff + 8 <= size && be32(sb + boff) == CSMAGIC_ENTITLEMENTS) {
					uint32_t blen = be32(sb + boff + 4);

					if (blen >= 8 && boff + blen <= size && (out = malloc(blen - 8 + 1)) != NULL) {
						memcpy(out, sb + boff + 8, blen - 8);
						out[blen - 8] = 0;
						*len = blen - 8;
					}
				}
			}
		}
	}
	free(m.d);
	return out;
}

int
adhoc_sign(const char *path, const char *identifier, const char *entitlements, size_t ents_len, char *err, size_t errlen)
{
	struct macho m;
	mode_t mode = 0755;
	struct buf req = { 0 }, xml = { 0 }, der = { 0 }, dir = { 0 }, sig = { 0 };
	uint64_t execseg = 0, size, limit, alloc;
	uint32_t special, ncode, idlen, count;
	unsigned char *grown;
	char tmp[1024];
	int fd, ret = -1;

	if (load(path, &m, &mode, err, errlen) != 0) {
		return -1;
	}
	if (m.cryptid != 0) {
		say_err(err, errlen, "still encrypted (an App Store binary not decrypted)%s", "");
		goto out;
	}
	if (m.linkedit == 0) {
		say_err(err, errlen, "no __LINKEDIT segment%s", "");
		goto out;
	}
	/* where the signature goes: where the old one was, or the end -- not before the string table's end */
	size = m.cs != 0 ? rd32(m.d + m.cs + 8) : m.len;
	if (size > m.len) {
		say_err(err, errlen, "its signature points past the end%s", "");
		goto out;
	}
	if (m.strend != 0 && m.strend <= size) {
		size = m.strend;
	}
	limit = (size + 15) & ~(uint64_t)15;

	/* the blobs: requirements (an empty set), entitlements as XML and as DER */
	buf_be32(&req, CSMAGIC_REQUIREMENTS), buf_be32(&req, 12), buf_be32(&req, 0);
	if (entitlements != NULL && ents_len > 0) {
		struct buf body = { 0 };

		if (der_parse(entitlements, ents_len, &body, &execseg, err, errlen) != 0) {
			free(body.p);
			goto out;
		}
		buf_be32(&xml, CSMAGIC_ENTITLEMENTS), buf_be32(&xml, (uint32_t)(8 + ents_len));
		buf_put(&xml, entitlements, ents_len);
		buf_be32(&der, CSMAGIC_DER_ENTITLEMENTS), buf_be32(&der, (uint32_t)(8 + body.n));
		buf_put(&der, body.p, body.n);
		free(body.p);
	}
	if (m.filetype == MH_EXECUTE) {
		execseg |= EXECSEG_MAIN_BINARY;
	}
	special = xml.n ? 7 : 2;
	ncode = (uint32_t)((limit + PAGE - 1) / PAGE);
	idlen = (uint32_t)strlen(identifier) + 1;
	count = xml.n ? 4 : 2;
	{
		uint64_t cd = CD_SIZE + idlen + (uint64_t)(special + ncode) * 32;

		alloc = (12 + count * 8 + req.n + xml.n + der.n + cd + 15) & ~(uint64_t)15;
		if (limit + alloc > UINT32_MAX) {
			say_err(err, errlen, "4 GB or more%s", "");
			goto out;
		}
	}

	/* the file grown to its new end; nothing but zeros between the code and the signature */
	grown = realloc(m.d, (size_t)(limit + alloc));
	if (grown == NULL) {
		say_err(err, errlen, "out of memory%s", "");
		goto out;
	}
	m.d = grown;
	memset(m.d + size, 0, (size_t)(limit + alloc - size));

	/* the load commands: the old LC_CODE_SIGNATURE out, a new one last; __LINKEDIT over the signature */
	{
		size_t end = 32 + m.sizeofcmds;

		if (m.cs != 0) {
			memmove(m.d + m.cs, m.d + m.cs + 16, end - (m.cs + 16));
			memset(m.d + end - 16, 0, 16);
			m.sizeofcmds -= 16, m.ncmds--, end -= 16;
			if (m.linkedit > m.cs) {
				m.linkedit -= 16;
			}
		}
		if (end + 16 > m.min_sect || end + 16 > limit) {
			say_err(err, errlen, "no room for a signature command after the load commands%s", "");
			goto out;
		}
		for (size_t i = end; i < end + 16; i++) {
			if (m.d[i] != 0) {
				say_err(err, errlen, "no room for a signature command after the load commands%s", "");
				goto out;
			}
		}
		wr32(m.d + end, LC_CODE_SIGNATURE), wr32(m.d + end + 4, 16);
		wr32(m.d + end + 8, (uint32_t)limit), wr32(m.d + end + 12, (uint32_t)alloc);
		m.ncmds++, m.sizeofcmds += 16;
		wr32(m.d + 16, m.ncmds), wr32(m.d + 20, m.sizeofcmds);

		uint64_t lfileoff = rd64(m.d + m.linkedit + 40), lsize;

		if (lfileoff > limit) {
			say_err(err, errlen, "__LINKEDIT starts after the code's end%s", "");
			goto out;
		}
		lsize = limit + alloc - lfileoff;
		wr64(m.d + m.linkedit + 48, lsize);
		wr64(m.d + m.linkedit + 32, (lsize + SEG_ALIGN - 1) & ~(uint64_t)(SEG_ALIGN - 1));
	}

	/* the CodeDirectory: header, identifier, special slots (-7 .. -1), the pages' hashes */
	{
		uint32_t hash_off = CD_SIZE + idlen + special * 32, cd_len = hash_off + ncode * 32;
		unsigned char *slots = calloc(special, 32), h[32];

		if (slots == NULL) {
			say_err(err, errlen, "out of memory%s", "");
			goto out;
		}
		if (m.info_size != 0 && m.info_off + m.info_size <= limit) {
			sha256(m.d + m.info_off, (size_t)m.info_size, slots + (special - 1) * 32);
		}
		sha256(req.p, req.n, slots + (special - 2) * 32);
		if (xml.n) {
			sha256(xml.p, xml.n, slots + (special - 5) * 32);
			sha256(der.p, der.n, slots + (special - 7) * 32);
		}
		buf_be32(&dir, CSMAGIC_CODEDIRECTORY), buf_be32(&dir, cd_len), buf_be32(&dir, 0x20400);
		buf_be32(&dir, CS_ADHOC), buf_be32(&dir, hash_off), buf_be32(&dir, CD_SIZE), buf_be32(&dir, special);
		buf_be32(&dir, ncode), buf_be32(&dir, (uint32_t)limit);
		buf_byte(&dir, 32), buf_byte(&dir, 2), buf_byte(&dir, 0), buf_byte(&dir, 12);      /* SHA-256, 4 KB */
		buf_be32(&dir, 0), buf_be32(&dir, 0), buf_be32(&dir, 0), buf_be32(&dir, 0);         /* spare2 scatter team spare3 */
		buf_be64(&dir, 0);                                                                   /* codeLimit64 */
		buf_be64(&dir, m.exec_left), buf_be64(&dir, m.exec_right - m.exec_left), buf_be64(&dir, execseg);
		buf_put(&dir, identifier, idlen);
		buf_put(&dir, slots, special * 32);
		free(slots);
		for (uint32_t i = 0; i < ncode; i++) {
			uint64_t at = (uint64_t)i * PAGE, n = limit - at < PAGE ? limit - at : PAGE;

			sha256(m.d + at, (size_t)n, h);
			buf_put(&dir, h, 32);
		}
	}

	/* the SuperBlob, its index in slot order: 0 CodeDirectory, 2 requirements, 5 XML, 7 DER */
	{
		uint32_t at = 12 + count * 8;
		uint32_t total = at + (uint32_t)(dir.n + req.n + xml.n + der.n);

		buf_be32(&sig, CSMAGIC_EMBEDDED_SIGNATURE), buf_be32(&sig, total), buf_be32(&sig, count);
		buf_be32(&sig, 0), buf_be32(&sig, at), at += (uint32_t)dir.n;
		buf_be32(&sig, 2), buf_be32(&sig, at), at += (uint32_t)req.n;
		if (xml.n) {
			buf_be32(&sig, 5), buf_be32(&sig, at), at += (uint32_t)xml.n;
			buf_be32(&sig, 7), buf_be32(&sig, at);
		}
		buf_put(&sig, dir.p, dir.n), buf_put(&sig, req.p, req.n), buf_put(&sig, xml.p, xml.n), buf_put(&sig, der.p, der.n);
		if (sig.failed || dir.failed || req.failed || xml.failed || der.failed || sig.n > alloc) {
			say_err(err, errlen, "the signature did not come together%s", "");
			goto out;
		}
		memcpy(m.d + limit, sig.p, sig.n);
	}

	/* written beside it, then put in its place */
	snprintf(tmp, sizeof(tmp), "%s.adhoc-tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		say_err(err, errlen, "cannot write: %s", strerror(errno));
		goto out;
	}
	{
		size_t total = (size_t)(limit + alloc), done = 0;

		while (done < total) {
			ssize_t w = write(fd, m.d + done, total - done);

			if (w <= 0) {
				break;
			}
			done += (size_t)w;
		}
		if (done != total || fchmod(fd, mode) != 0 || close(fd) != 0) {
			if (done == total) {
				say_err(err, errlen, "cannot finish writing: %s", strerror(errno));
			} else {
				say_err(err, errlen, "short write%s", "");
				close(fd);
			}
			unlink(tmp);
			goto out;
		}
	}
	if (rename(tmp, path) != 0) {
		say_err(err, errlen, "cannot replace it: %s", strerror(errno));
		unlink(tmp);
		goto out;
	}
	ret = 0;
out:
	free(m.d), free(req.p), free(xml.p), free(der.p), free(dir.p), free(sig.p);
	return ret;
}
