/*
 * camera_isp.c's preview render timed, on the phone or on a computer (2026-11-15): a made-up 4000x3000 RAW10 frame
 * (smooth gradients, edges and noise, the K40's wide camera's size), a white balance, a colour matrix and a lens
 * shading grid like a real tuning's, rendered as the camera cell of Nagram's picker asks (1920x1080 420v), and the
 * same turned, smaller, zoomed (the magnified road) and as BGRA. Prints the time per picture (one thread) and writes
 * the pictures to DIR, to compare byte by byte with another build's or the other road's (the frame is the same
 * everywhere: a fixed generator). "plain": the preview's pixels the way before 2026-11-15 (isp.plain_preview).
 * usage: isp_bench DIR [ITERATIONS] [plain]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "camera_isp.h"

#define FW 4000
#define FH 3000

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void
put_raw10(uint8_t *line, uint32_t x, uint32_t v)
{
	uint8_t *g = line + (x / 4) * 5;
	unsigned k = x & 3;

	g[k] = (uint8_t)(v >> 2);
	g[4] = (uint8_t)((g[4] & ~(3u << (k * 2))) | (v & 3) << (k * 2));
}

static void
save(const char *dir, const char *name, const void *p, size_t n)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((f = fopen(path, "wb")) != NULL) {
		fwrite(p, 1, n, f);
		fclose(f);
	}
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	int iterations = argc > 2 ? atoi(argv[2]) : 10;
	size_t stride = FW * 5 / 4;
	struct hakios_camera_frame *f = calloc(1, sizeof(*f) + stride * FH);
	uint8_t *raw = (uint8_t *)(f + 1);
	struct isp isp;
	uint32_t seed = 12345;

	memcpy(f->magic, "HKCAMFR", 8);
	f->header_bytes = sizeof(*f);
	f->width = FW, f->height = FH, f->stride = (uint32_t)stride, f->format = 1, f->bayer = 0, f->black = 64;
	for (uint32_t y = 0; y < FH; y++) {
		for (uint32_t x = 0; x < FW; x++) {
			int colour = (y & 1) * 2 + (x & 1);             /* RGGB */
			double u = (double)x / FW, v = (double)y / FH;
			double level = 300 + 250 * sin(u * 7 + colour) * cos(v * 5) + ((x / 97 + y / 61) & 1 ? 180 : 0);

			seed = seed * 1103515245u + 12345u;
			level += (int)(seed >> 16 & 63) - 32;
			level *= 1 - 0.35 * ((u - .5) * (u - .5) + (v - .5) * (v - .5)) * 4;     /* (vignetting) */
			put_raw10(raw + (size_t)y * stride, x, (uint32_t)(level < 0 ? 64 : level + 64 > 1023 ? 1023 : level + 64));
		}
	}
	isp_init(&isp);
	isp.plain_preview = argc > 3 && strcmp(argv[3], "plain") == 0;
	isp.wb[0] = 1.85f, isp.wb[2] = 1.52f, isp.digital = 1.15f;
	isp_set_ccm(&isp, (const float[9]){ 1.62f, -0.43f, -0.19f, -0.27f, 1.49f, -0.22f, -0.06f, -0.55f, 1.61f });
	isp.lsc.on = true;
	for (int c = 0; c < 3; c++) {
		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				double du = (double)gx / (LSC_W - 1) - .5, dv = (double)gy / (LSC_H - 1) - .5;

				isp.lsc.gain[c][gy][gx] = (float)(1 + (1.4 + 0.2 * c) * (du * du + dv * dv));
			}
		}
	}
	struct { const char *name; uint32_t w, h; float zoom; bool turned, bgra; } cases[] = {
		{ "nv12-1920x1080", 1920, 1080, 1, false, false },
		{ "nv12-1920x1080-turned", 1920, 1080, 1, true, false },
		{ "nv12-1280x960", 1280, 960, 1, false, false },
		{ "nv12-1920x1080-zoom2.5", 1920, 1080, 2.5f, false, false },
		{ "bgra-1920x1080", 1920, 1080, 1, false, true },
	};
	for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		uint32_t w = cases[k].w, h = cases[k].h;
		size_t bytes = cases[k].bgra ? (size_t)w * 4 * h : (size_t)w * h * 3 / 2;
		uint8_t *out = calloc(1, bytes);
		double t0, t;

		isp.zoom = cases[k].zoom;
		isp.turned = cases[k].turned;
		t0 = now();
		for (int i = 0; i < iterations; i++) {
			if (cases[k].bgra) {
				isp_render_bgra(&isp, f, raw, out, (size_t)w * 4, w, h, 0, h);
			} else {
				isp_render_nv12(&isp, f, raw, out, w, out + (size_t)w * h, w, w, h, 0, h, true);
			}
		}
		t = (now() - t0) / iterations;
		printf("%-26s %7.2f ms a picture (%.1f Mpixel/s, one thread)\n", cases[k].name, t * 1000, w * h / t / 1e6);
		save(dir, cases[k].name, out, bytes);
		free(out);
	}
	free(f);
	return 0;
}
