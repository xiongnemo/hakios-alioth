/*
 * PVRTC1 decoding by the processor, shared by the GL library (eagl.c: glCompressedTexImage2D) and the Metal driver
 * (alioth_metal.m: -replaceRegion: of a PVRTC texture), which both store the texels as RGBA8 for an Adreno that
 * samples no PVRTC. Static functions: each includer gets its own copy.
 */
#ifndef HAKIOS_PVRTC_H
#define HAKIOS_PVRTC_H

#include <stddef.h>
#include <stdint.h>

/* ---- PVRTC (PowerVR's compressed textures, which every iPhone's GPU samples and an Adreno does not) ---------- */

/*
 * PVRTC1, 4 and 2 bits a texel, decoded by the processor into RGBA8 for an application's glCompressedTexImage2D.
 * A 64-bit word a block (4x4 texels, 8x4 at 2 bits): 32 bits of modulation, then two endpoint colours A (low half)
 * and B (high half), each opaque (RGB 554/555) or translucent (ARGB 3443/3444) by its top bit; bit 0 of the colour
 * word chooses the modulation mode. The blocks are in Morton order (Y in the even bits, X in the odd, the larger
 * dimension's extra bits above). A texel's colour: A and B each upscaled bilinearly between the centres of the four
 * blocks around it, mixed by its modulation weight /8. (Imagination's published format, as its PowerVR SDK's
 * PVRTDecompress and Basis Universal's PVRTC1 code read it.)
 */
struct pvr_colour {
	int r, g, b, a;                 /* 5-bit colour, 4-bit alpha */
};

static uint32_t
pvr_morton(uint32_t xblocks, uint32_t yblocks, uint32_t x, uint32_t y)
{
	uint32_t smaller = xblocks < yblocks ? xblocks : yblocks, rest = xblocks < yblocks ? y : x, out = 0, shift = 0;

	for (uint32_t bit = 1, to = 1; bit < smaller; bit <<= 1, to <<= 2, shift++) {
		out |= (y & bit) ? to : 0;
		out |= (x & bit) ? to << 1 : 0;
	}
	return out | (rest >> shift) << (2 * shift);
}

static struct pvr_colour
pvr_colour_a(uint32_t c)
{
	if (c & 0x8000) {
		return (struct pvr_colour){ (int)(c >> 10 & 31), (int)(c >> 5 & 31), (int)((c & 0x1e) | (c & 0x1e) >> 4), 15 };
	}
	return (struct pvr_colour){ (int)((c >> 7 & 0x1e) | (c >> 11 & 1)), (int)((c >> 3 & 0x1e) | (c >> 7 & 1)),
	    (int)((c & 0xe) << 1 | (c & 0xe) >> 2), (int)(c >> 11 & 0xe) };
}

static struct pvr_colour
pvr_colour_b(uint32_t c)
{
	if (c & 0x80000000u) {
		return (struct pvr_colour){ (int)(c >> 26 & 31), (int)(c >> 21 & 31), (int)(c >> 16 & 31), 15 };
	}
	return (struct pvr_colour){ (int)((c >> 23 & 0x1e) | (c >> 27 & 1)), (int)((c >> 19 & 0x1e) | (c >> 23 & 1)),
	    (int)((c >> 15 & 0x1e) | (c >> 19 & 1)), (int)(c >> 27 & 0xe) };
}

/* the four blocks' colours upscaled over the w x 4 texels between their centres, 8 bits a channel */
static void
pvr_upscale(struct pvr_colour p, struct pvr_colour q, struct pvr_colour r, struct pvr_colour s, int w, int out[32][4])
{
	int hp[4] = { p.r * w, p.g * w, p.b * w, p.a * w }, hr[4] = { r.r * w, r.g * w, r.b * w, r.a * w };
	int qp[4] = { q.r - p.r, q.g - p.g, q.b - p.b, q.a - p.a }, sr[4] = { s.r - r.r, s.g - r.g, s.b - r.b, s.a - r.a };

	for (int x = 0; x < w; x++) {
		int v[4], dy[4];

		for (int c = 0; c < 4; c++) {
			v[c] = 4 * hp[c];
			dy[c] = hr[c] - hp[c];
		}
		for (int y = 0; y < 4; y++) {
			for (int c = 0; c < 4; c++) {
				/* (v is the 5-bit colour, 4-bit alpha times 16 at 4 bpp, 32 at 2: widened to 8 bits) */
				out[y * w + x][c] = w == 4 ? (c < 3 ? (v[c] >> 6) + (v[c] >> 1) : (v[c] >> 4) + v[c]) :
				    (c < 3 ? (v[c] >> 7) + (v[c] >> 2) : (v[c] >> 5) + (v[c] >> 1));
				v[c] += dy[c];
			}
		}
		for (int c = 0; c < 4; c++) {
			hp[c] += qp[c];
			hr[c] += sr[c];
		}
	}
}

/* a block's modulation into mod[][] at (ox, oy): weights /8 (4 bpp; +10: punch-through alpha), or 2-bit codes (2 bpp) */
static void
pvr_modulation(uint32_t bits, uint32_t colour, int two, int ox, int oy, int mod[8][16], int mode[8][16])
{
	int m = (int)(colour & 1);

	if (!two) {
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++, bits >>= 2) {
				static const int standard[4] = { 0, 3, 5, 8 }, punch[4] = { 0, 4, 14, 8 };

				mod[oy + y][ox + x] = m ? punch[bits & 3] : standard[bits & 3];
			}
		}
		return;
	}
	if (m == 0) {
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 8; x++, bits >>= 1) {
				mode[oy + y][ox + x] = 0;
				mod[oy + y][ox + x] = bits & 1 ? 3 : 0;
			}
		}
		return;
	}
	if (bits & 1) {
		m = bits & 1u << 20 ? 3 : 2;            /* vertical only : horizontal only */
		bits = bits & 1u << 21 ? bits | 1u << 20 : bits & ~(1u << 20);
	}
	bits = bits & 2 ? bits | 1 : bits & ~1u;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 8; x++) {
			mode[oy + y][ox + x] = m;
			if (((x ^ y) & 1) == 0) {
				mod[oy + y][ox + x] = (int)(bits & 3);
				bits >>= 2;
			}
		}
	}
}

static int
pvr_weight(int mod[8][16], int mode[8][16], int x, int y, int two)
{
	static const int code[4] = { 0, 3, 5, 8 };

	if (!two) {
		return mod[y][x];
	}
	if (mode[y][x] == 0 || ((x ^ y) & 1) == 0) {
		return code[mod[y][x]];
	}
	if (mode[y][x] == 1) {
		return (code[mod[y - 1][x]] + code[mod[y + 1][x]] + code[mod[y][x - 1]] + code[mod[y][x + 1]] + 2) / 4;
	}
	if (mode[y][x] == 2) {
		return (code[mod[y][x - 1]] + code[mod[y][x + 1]] + 1) / 2;
	}
	return (code[mod[y - 1][x]] + code[mod[y + 1][x]] + 1) / 2;
}

/* the whole image (the block grid at least 2 x 2 blocks), RGBA8 into out (width x height of the padded size) */
static void
pvr_decode(const uint8_t *data, int two, int width, int height, uint8_t *out)
{
	int bw = two ? 8 : 4, xblocks = width / bw, yblocks = height / 4;

	for (int by = 0; by < yblocks; by++) {
		for (int bx = 0; bx < xblocks; bx++) {
			int at[4][2] = { { bx, by }, { (bx + 1) % xblocks, by }, { bx, (by + 1) % yblocks },
			    { (bx + 1) % xblocks, (by + 1) % yblocks } };
			uint32_t words[4][2];
			int mod[8][16] = { { 0 } }, mode[8][16] = { { 0 } }, a[32][4], b[32][4];

			for (int i = 0; i < 4; i++) {
				const uint8_t *word = data + 8 * (size_t)pvr_morton((uint32_t)xblocks, (uint32_t)yblocks,
				    (uint32_t)at[i][0], (uint32_t)at[i][1]);

				words[i][0] = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
				words[i][1] = (uint32_t)word[4] | (uint32_t)word[5] << 8 | (uint32_t)word[6] << 16 | (uint32_t)word[7] << 24;
				pvr_modulation(words[i][0], words[i][1], two, (i & 1) * bw, (i >> 1) * 4, mod, mode);
			}
			pvr_upscale(pvr_colour_a(words[0][1]), pvr_colour_a(words[1][1]), pvr_colour_a(words[2][1]),
			    pvr_colour_a(words[3][1]), bw, a);
			pvr_upscale(pvr_colour_b(words[0][1]), pvr_colour_b(words[1][1]), pvr_colour_b(words[2][1]),
			    pvr_colour_b(words[3][1]), bw, b);
			/* the texels from this block's centre to the next ones' (wrapping at the edges) */
			for (int y = 0; y < 4; y++) {
				for (int x = 0; x < bw; x++) {
					int w = pvr_weight(mod, mode, x + bw / 2, y + 2, two), punch = w > 10;
					uint8_t *px = out + 4 * ((size_t)((by * 4 + 2 + y) % height) * (size_t)width +
					    (size_t)((bx * bw + bw / 2 + x) % width));

					w -= punch ? 10 : 0;
					for (int c = 0; c < 4; c++) {
						px[c] = (uint8_t)((a[y * bw + x][c] * (8 - w) + b[y * bw + x][c] * w) / 8);
					}
					px[3] = punch ? 0 : px[3];
				}
			}
		}
	}
}

#endif /* HAKIOS_PVRTC_H */
