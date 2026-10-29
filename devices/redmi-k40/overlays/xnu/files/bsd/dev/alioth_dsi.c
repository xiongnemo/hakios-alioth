/*
 * Redmi K40 (alioth): DCS commands to the panel through DSI0, and the panel's refresh rate.
 *
 * The panel is "K11A 38 08 0a" (qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd), 1080 x 2400, DSI command
 * mode with DSC, its TE on a dedicated pin. The UEFI (XBL DisplayDxe, Panel_K11A_38_08_0a_amoled_
 * dsc_cmd.xml) brings it up at 60 Hz (DSIRefreshRate 0x3C0000; its init sequence ends in "60 00")
 * and leaves the MDP's INTF_1 auto-refresh on with one frame per TE: the panel is sent the
 * framebuffer at every TE and nothing in the kernel kicks frames.
 *
 * The phone's own device tree has three timings for the panel, 60, 90 and 120 Hz, each with the
 * DCS set Android sends to switch to it (qcom,mdss-dsi-timing-switch-command, LP mode):
 *     120 Hz: 60 10; F0 5A 5A; FC 5A 5A; B0 16; D1 2E; FC A5 A5; F0 A5 A5
 *      90 Hz: 60 10; F0 5A 5A; FC 5A 5A; B0 16; D1 6E; FC A5 A5; F0 A5 A5
 *      60 Hz: 60 00; F0 5A 5A; FC 5A 5A; B0 16; D1 2E; FC A5 A5; F0 A5 A5
 * After the switch the panel's TE comes at the new rate. The INTF tear check counts lines between
 * TEs by its vsync_count (19.2 MHz clocks a line, sde_encoder_phys_cmd_tearcheck_config():
 * vsync_count = 19.2 MHz / (vtotal * fps)); with the 60 Hz count left in place a 120 Hz frame would
 * end half way through its lines, so it is reprogrammed for the new timing as Android does
 * (60 Hz: vtotal 3592; 90 Hz: 2996; 120 Hz: 2416, from the timings' porches).
 *
 * DCS writes go through the DSI0 controller's command DMA in FIFO mode (the test-pattern
 * generator's DMA FIFO: no buffer in RAM, no SMMU), as Linux dsi_ctrl_hw_cmn_kickoff_fifo_command
 * and the OnePlus 10 Pro port (iOSPlus10, patch 0250, NegroniBacklight.cpp) do. A write waits for
 * the MDP's frame transfer to finish (DSI STATUS) so it goes between two frames.
 *
 * Boot-args: panel_hz=60|90|120 (default 120; 0 = leave the panel as the UEFI set it),
 *            panel_dcs=0 (no DCS at all).
 * Registers: Linux drivers/gpu/drm/msm/dsi/dsi.xml.h (DSI 6G/2.x offsets), techpack display
 * dsi_ctrl_reg.h; INTF tear: sde_hw_intf.c. DSI0 at 0xae94000 (live DT qcom,mdss_dsi_ctrl0@ae94000,
 * "qcom,dsi-ctrl-hw-v2.4"), INTF_1 at 0xae6b800 (qcom,sde-intf-off[1], type dsi).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.board.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_tl(const char *, ...) __printflike(1, 2);
#define printf hakios_tl

#define DSI0_PHYS               0x0ae94000UL
#define DSI0_SIZE               0x400
#define INTF1_PHYS              0x0ae6b800UL

#define DSI_HW_VERSION          0x000
#define DSI_CTRL                0x004   /* bit0 ENABLE, bit2 CMD_MODE_EN */
#define DSI_STATUS              0x008   /* bit1 CMD_MODE_DMA_BUSY, bit2 CMD_MODE_MDP_BUSY */
#define DSI_CMD_DMA_CTRL        0x03c   /* 31 BROADCAST_EN, 30 MASTER, 28 EMBEDDED, 26 LOW_POWER */
#define DSI_DMA_CMD_LENGTH      0x04c
#define DSI_TRIG_CTRL           0x084   /* [3:0] DMA trigger, 4 = software */
#define DSI_CMD_DMA_SW_TRIGGER  0x090
#define DSI_INT_CTRL            0x110   /* even bits status (write 1 clears), odd bits enables */
#define DSI_TPG_CTRL            0x15c
#define DSI_TPG_CMD_DMA_INIT    0x17c   /* the FIFO port */
#define DSI_TPG_DMA_FIFO_RESET  0x1ec

#define STATUS_DMA_BUSY         (1u << 1)
#define STATUS_MDP_BUSY         (1u << 2)
#define INT_DMA_DONE            (1u << 0)
#define INT_ENABLE_BITS         0xaaaaaaaau
#define TPG_FIFO_ON             (3u << 16 | 1u << 2 | 1u << 1)
#define TRIGGER_SW              4u
#define MAX_PAYLOAD             60      /* bytes: the FIFO holds 64 with the header */

/* INTF tear check (sde_hw_intf.c) */
#define TEAR_SYNC_CONFIG_VSYNC  0x288   /* [18:0] vsync_count, 19 counter enable, 20 TE from pin */
#define TEAR_SYNC_CONFIG_HEIGHT 0x28c
#define TEAR_VSYNC_INIT_VAL     0x294
#define TEAR_INT_COUNT_VAL      0x298
#define TEAR_AUTOREFRESH_CONFIG 0x2b4

static volatile uint32_t *dsi;
static vm_offset_t intf;
static lck_grp_t *dsi_grp;
static lck_mtx_t *dsi_lock;
static bool dsi_dead;
static unsigned dsi_failures, mdp_waits;
static uint32_t panel_rate;              /* what the panel was last switched to, 0 = the UEFI's */

static inline uint32_t dsi_rd(uint32_t off) { return dsi[off / 4]; }
static inline void dsi_wr(uint32_t off, uint32_t v) { dsi[off / 4] = v; }
static inline uint32_t intf_rd(uint32_t off) { return *(volatile uint32_t *)(intf + off); }
static inline void
intf_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(intf + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Map DSI0 and check that it is a running command-mode DSI 2.x controller. */
static bool
dsi_init(void)
{
	if (dsi != NULL) return true;
	if (dsi_dead) return false;
	vm_offset_t va = ml_io_map(DSI0_PHYS, DSI0_SIZE);
	intf = ml_io_map(INTF1_PHYS, 0x1000);
	if (va == 0 || intf == 0) {
		printf("alioth dsi: DSI0/INTF1 not mapped\n");
		dsi_dead = true;
		return false;
	}
	volatile uint32_t *d = (volatile uint32_t *)va;
	uint32_t ver = d[DSI_HW_VERSION / 4], ctrl = d[DSI_CTRL / 4];
	printf("alioth dsi: DSI0 version %#x ctrl %#x status %#x dma_ctrl %#x trig %#x tpg %#x\n",
	    ver, ctrl, d[DSI_STATUS / 4], d[DSI_CMD_DMA_CTRL / 4], d[DSI_TRIG_CTRL / 4], d[DSI_TPG_CTRL / 4]);
	if ((ver >> 28) != 2 || (ctrl & 0x5u) != 0x5u) {
		printf("alioth dsi: DSI0 is not a running command-mode DSI 2.x controller: no DCS\n");
		dsi_dead = true;
		return false;
	}
	dsi_grp = lck_grp_alloc_init("alioth_dsi", LCK_GRP_ATTR_NULL);
	dsi_lock = lck_mtx_alloc_init(dsi_grp, LCK_ATTR_NULL);
	dsi = d;
	return true;
}

static void
spin_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/*
 * One DCS write to the panel on virtual channel 0 (buf[0] = command, then its parameters):
 * short write without (0x05) or with one parameter (0x15), else a long write (0x39); or,
 * with force_type, that data type (the DT's packets say which). lp = low-power transmission.
 * 0 when the controller reported the DMA done. After 3 failures DCS is left alone for good.
 */
int
alioth_dsi_dcs_write(const uint8_t *buf, uint32_t len, bool lp, uint8_t force_type)
{
	uint32_t words[2 + MAX_PAYLOAD / 4];
	uint32_t n = 0, i;
	int rc = 0;

	if (!dsi_init() || len == 0 || len > MAX_PAYLOAD) return -1;
	uint8_t dt = force_type ? force_type : len == 1 ? 0x05 : len == 2 ? 0x15 : 0x39;
	/* Header in embedded mode: [7:0] data0/WC lo, [15:8] data1/WC hi, [21:16] DT, [23:22] VC, 30 long, 31 last */
	if (dt == 0x05 || dt == 0x15) {
		words[n++] = 1u << 31 | (uint32_t)dt << 16 | (len >= 2 ? (uint32_t)buf[1] << 8 : 0u) | buf[0];
	} else {
		words[n++] = 1u << 31 | 1u << 30 | (uint32_t)dt << 16 | len;
		for (i = 0; i < len; i += 4) {
			uint32_t w = 0;
			for (uint32_t b = 0; b < 4 && i + b < len; b++) w |= (uint32_t)buf[i + b] << (8 * b);
			words[n++] = w;
		}
	}
	uint32_t bytes = n * 4;
	if (n & 1u) words[n++] = 0;     /* the FIFO takes an even number of words */

	lck_mtx_lock(dsi_lock);
	if (dsi_dead) {
		lck_mtx_unlock(dsi_lock);
		return -1;
	}
	for (i = 0; i < 250 && (dsi_rd(DSI_STATUS) & STATUS_DMA_BUSY); i++) spin_us(20);
	if (dsi_rd(DSI_STATUS) & STATUS_DMA_BUSY) {
		rc = -2;                /* someone else's command never finished: do not touch it */
		goto out;
	}
	/* between frames: the auto-refresh sends one per TE (about 5 ms of a 16.7 or 8.3 ms frame) */
	for (i = 0; i < 1000 && (dsi_rd(DSI_STATUS) & STATUS_MDP_BUSY); i++) spin_us(20);
	if (i == 1000 && mdp_waits++ < 4) printf("alioth dsi: MDP stream busy for 20 ms; sending anyway\n");
	{
		uint32_t tpg = dsi_rd(DSI_TPG_CTRL), dma = dsi_rd(DSI_CMD_DMA_CTRL), trig = dsi_rd(DSI_TRIG_CTRL);
		bool done = false;

		if ((trig & 0xfu) != TRIGGER_SW) dsi_wr(DSI_TRIG_CTRL, (trig & ~0xfu) | TRIGGER_SW);
		dsi_wr(DSI_INT_CTRL, (dsi_rd(DSI_INT_CTRL) & INT_ENABLE_BITS) | INT_DMA_DONE);
		dsi_wr(DSI_TPG_CTRL, TPG_FIFO_ON);
		for (uint32_t w = 0; w < n; w++) dsi_wr(DSI_TPG_CMD_DMA_INIT, words[w]);
		dsi_wr(DSI_CMD_DMA_CTRL, (dma & ~(1u << 31 | 1u << 30 | 1u << 26)) | 1u << 28 | (lp ? 1u << 26 : 0u));
		dsi_wr(DSI_DMA_CMD_LENGTH, bytes);
		__builtin_arm_dsb(0xf);
		dsi_wr(DSI_CMD_DMA_SW_TRIGGER, 1);
		__builtin_arm_dsb(0xf);
		for (i = 0; i < 1250 && !done; i++) {          /* 25 ms */
			done = (dsi_rd(DSI_INT_CTRL) & INT_DMA_DONE) != 0;
			if (!done) spin_us(20);
		}
		dsi_wr(DSI_INT_CTRL, (dsi_rd(DSI_INT_CTRL) & INT_ENABLE_BITS) | INT_DMA_DONE);
		/* dsi_ctrl_hw_cmn_reset_cmd_fifo, then the registers as found */
		dsi_wr(DSI_TPG_CTRL, 0);
		dsi_wr(DSI_TPG_DMA_FIFO_RESET, 1);
		__builtin_arm_dsb(0xf);
		spin_us(1);
		dsi_wr(DSI_TPG_DMA_FIFO_RESET, 0);
		dsi_wr(DSI_TPG_CTRL, tpg);
		dsi_wr(DSI_CMD_DMA_CTRL, dma);
		if ((trig & 0xfu) != TRIGGER_SW) dsi_wr(DSI_TRIG_CTRL, trig);
		__builtin_arm_dsb(0xf);
		if (!done) rc = -3;
	}
out:
	if (rc != 0) {
		dsi_failures++;
		printf("alioth dsi: DCS %#x (%u bytes) failed %d, status %#x int %#x (failure %u)\n",
		    buf[0], len, rc, dsi_rd(DSI_STATUS), dsi_rd(DSI_INT_CTRL), dsi_failures);
		if (dsi_failures >= 3) {
			dsi_dead = true;
			printf("alioth dsi: DCS disabled after 3 failures\n");
		}
	} else {
		dsi_failures = 0;
	}
	lck_mtx_unlock(dsi_lock);
	return rc;
}

/* The panel's timings (the phone's DT, qcom,mdss-dsi-display-timings of the K11A panel). */
struct k11a_timing {
	uint32_t hz, vtotal;
	uint8_t rate_60, d1;            /* "60 xx" and "D1 xx" of its timing-switch set */
};
static const struct k11a_timing k11a_timings[] = {
	{ 60, 2400 + 600 + 560 + 32, 0x00, 0x2e },      /* timing@0: v fp 600, bp 560, pw 32 */
	{ 90, 2400 + 300 + 280 + 16, 0x10, 0x6e },      /* timing@1: v fp 300, bp 280, pw 16 */
	{ 120, 2400 + 4 + 8 + 4, 0x10, 0x2e },          /* timing@2: v fp 4, bp 8, pw 4 */
};

static int
send_switch(const struct k11a_timing *t)
{
	const uint8_t c60[] = { 0x60, t->rate_60 };
	const uint8_t f0_on[] = { 0xf0, 0x5a, 0x5a }, fc_on[] = { 0xfc, 0x5a, 0x5a };
	const uint8_t b0[] = { 0xb0, 0x16 }, d1[] = { 0xd1, t->d1 };
	const uint8_t fc_off[] = { 0xfc, 0xa5, 0xa5 }, f0_off[] = { 0xf0, 0xa5, 0xa5 };
	const struct { const uint8_t *b; uint32_t n; } set[] = {
		{ c60, 2 }, { f0_on, 3 }, { fc_on, 3 }, { b0, 2 }, { d1, 2 }, { fc_off, 3 }, { f0_off, 3 },
	};
	for (unsigned i = 0; i < sizeof(set) / sizeof(set[0]); i++) {
		/* the DT sends every packet of the set as a long write (data type 0x39) */
		int rc = alioth_dsi_dcs_write(set[i].b, set[i].n, true, 0x39);
		if (rc != 0) return rc;
	}
	return 0;
}

/*
 * Switch the panel to `hz` (60, 90, 120) and the tear check with it. 1 when done. Called with the
 * display's bus clocks running (hakios.display.c, before its TE calibration).
 */
int
alioth_panel_set_rate(uint32_t hz)
{
	const struct k11a_timing *t = NULL, *cur = NULL;

	for (unsigned i = 0; i < sizeof(k11a_timings) / sizeof(k11a_timings[0]); i++) {
		if (k11a_timings[i].hz == hz) t = &k11a_timings[i];
		if (k11a_timings[i].hz == (panel_rate ? panel_rate : 60)) cur = &k11a_timings[i];
	}
	if (t == NULL || cur == NULL || !dsi_init()) return 0;
	uint32_t sync = intf_rd(TEAR_SYNC_CONFIG_VSYNC), count = sync & 0x7ffff;
	uint32_t want = (uint32_t)((19200000ULL + (uint64_t)t->vtotal * hz / 2) / ((uint64_t)t->vtotal * hz));
	printf("alioth panel: tear check sync %#x (vsync_count %u, %s), height %#x, init %u, count %#x, autorefresh %#x\n",
	    sync, count, (sync & (1u << 20)) ? "TE pin" : "internal", intf_rd(TEAR_SYNC_CONFIG_HEIGHT),
	    intf_rd(TEAR_VSYNC_INIT_VAL), intf_rd(TEAR_INT_COUNT_VAL), intf_rd(TEAR_AUTOREFRESH_CONFIG));
	if (!(sync & (1u << 19)) || count == 0) {
		printf("alioth panel: the tear check is not counting: refresh left alone\n");
		return 0;
	}
	int rc = send_switch(t);
	if (rc != 0) {
		printf("alioth panel: switch to %u Hz failed (%d); panel left at %u Hz\n", hz, rc, cur->hz);
		return 0;
	}
	intf_wr(TEAR_SYNC_CONFIG_VSYNC, (sync & ~0x7ffffu) | (want & 0x7ffff));
	panel_rate = hz;
	printf("alioth panel: switched to %u Hz (vtotal %u): vsync_count %u -> %u\n", hz, t->vtotal, count, want);
	return 1;
}

/*
 * Read-only look at the display engine as the UEFI left it (kona DPU 6.0, live DT qcom,sde-*-off relative to
 * MDSS 0xae00000): which source pipe scans the framebuffer, its stride and size, and the control path's
 * flush and layer state -- what a page flip (SRC0_ADDR + CTL_FLUSH) would have to touch. Registers:
 * sde_hw_sspp.c (SRC_SIZE 0x00, SRC_XY 0x08, OUT_SIZE 0x0c, SRC0_ADDR 0x14, YSTRIDE0 0x24, FORMAT 0x30),
 * sde_hw_ctl.c (LAYER0 0x00, FLUSH 0x18, START 0x1c, TOP 0x14), sde_hw_lm.c (OP_MODE 0x00, OUT_SIZE 0x04).
 */
#define MDSS_PHYS               0x0ae00000UL
static const struct { const char *name; uint32_t off; } dpu_sspp[] = {
	{ "VIG0", 0x5000 }, { "VIG1", 0x7000 }, { "VIG2", 0x9000 }, { "VIG3", 0xb000 },
	{ "DMA0", 0x25000 }, { "DMA1", 0x27000 }, { "DMA2", 0x29000 }, { "DMA3", 0x2b000 },
};
static const uint32_t dpu_ctl[] = { 0x2000, 0x2200, 0x2400, 0x2600, 0x2800, 0x2a00 };
static const uint32_t dpu_lm[] = { 0x45000, 0x46000 };

static void
alioth_dpu_probe(void)
{
	vm_offset_t m = ml_io_map(MDSS_PHYS, 0x90000);
	uint32_t on = 1;

	PE_parse_boot_argn("dpu_probe", &on, sizeof(on));
	if (!on || m == 0) return;
#define MD(o) (*(volatile uint32_t *)(m + (o)))
	printf("alioth dpu: MDSS hw %#x, MDP top %#x\n", MD(0x0), MD(0x1000));
	for (unsigned i = 0; i < sizeof(dpu_sspp) / sizeof(dpu_sspp[0]); i++) {
		uint32_t b = dpu_sspp[i].off;
		uint32_t size = MD(b + 0x00), addr = MD(b + 0x14);
		if (size == 0 && addr == 0) continue;
		printf("alioth dpu: %s src %ux%u at %u,%u -> out %ux%u at %u,%u; addr %#x %#x stride %#x %#x format %#x unpack %#x op %#x\n",
		    dpu_sspp[i].name, size & 0xffff, size >> 16, MD(b + 0x08) & 0xffff, MD(b + 0x08) >> 16,
		    MD(b + 0x0c) & 0xffff, MD(b + 0x0c) >> 16, MD(b + 0x10) & 0xffff, MD(b + 0x10) >> 16,
		    addr, MD(b + 0x18), MD(b + 0x24), MD(b + 0x28), MD(b + 0x30), MD(b + 0x34), MD(b + 0x38));
	}
	for (unsigned i = 0; i < sizeof(dpu_ctl) / sizeof(dpu_ctl[0]); i++) {
		uint32_t b = dpu_ctl[i];
		uint32_t layer0 = MD(b + 0x00), top = MD(b + 0x14);
		if (layer0 == 0 && top == 0) continue;
		printf("alioth dpu: CTL%u layer0 %#x layer1 %#x top %#x flush %#x intf_active %#x\n",
		    i, layer0, MD(b + 0x04), top, MD(b + 0x18), MD(b + 0xf4));
	}
	for (unsigned i = 0; i < sizeof(dpu_lm) / sizeof(dpu_lm[0]); i++) {
		uint32_t b = dpu_lm[i];
		printf("alioth dpu: LM%u op %#x out %ux%u border %#x\n", i, MD(b), MD(b + 4) & 0xffff, MD(b + 4) >> 16, MD(b + 8));
	}
#undef MD
}

/* At display start (hakios.display.c): the rate asked for by panel_hz=, 120 by default. */
void
alioth_panel_start(void)
{
	uint32_t hz = 120, dcs = 1;

	if (!hakios_board_is_alioth()) return;
	alioth_dpu_probe();
	PE_parse_boot_argn("panel_dcs", &dcs, sizeof(dcs));
	PE_parse_boot_argn("panel_hz", &hz, sizeof(hz));
	if (!dcs || hz == 0) {
		printf("alioth panel: refresh left as the UEFI set it (panel_hz=%u panel_dcs=%u)\n", hz, dcs);
		return;
	}
	if (hz == 60 && panel_rate == 0) return;        /* the UEFI's */
	if (alioth_panel_set_rate(hz)) {
		/* let a few TEs of the new timing pass before anyone measures them */
		delay_for_interval(50, NSEC_PER_MSEC);
	}
}

uint32_t
alioth_panel_rate(void)
{
	return panel_rate ? panel_rate : 60;
}
