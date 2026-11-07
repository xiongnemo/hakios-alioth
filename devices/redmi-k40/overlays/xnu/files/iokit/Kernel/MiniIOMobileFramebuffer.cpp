/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void hakios_screen_power(bool on);           /* bsd/dev/hakios.cpufreq.c */
extern "C" void hakios_gpu_screen(bool on);             /* iokit/Kernel/hakios.gpu.cpp: the GPU asleep while dark (gpu_sleep=1) */
extern "C" int alioth_panel_power(bool on);             /* bsd/dev/alioth_dsi.c */
extern "C" void alioth_refresh_activity(void);          /* (a swap: a frame every TE again, if a still screen had fewer) */
/* the panel's scan (bsd/dev/hakios.display.c; sys/hakios.display.h, which iokit/ does not see) */
struct hakios_display_scan {
	int ok;
	uint32_t lines, shown, millihz;
	uint64_t line_ns, frame_ns;
};
extern "C" int hakios_display_scan_start(void);
extern "C" const struct hakios_display_scan * hakios_display_scan(void);
extern "C" int hakios_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns);
extern "C" uint64_t hakios_display_next_frame_ns(void);
extern "C" int hakios_display_command_mode(void);
extern "C" int hakios_display_wait_copy(uint64_t copy_ns, uint64_t limit_ns);
/* Redmi K40 page flips and refresh rate (bsd/dev/alioth_dsi.c) */
extern "C" int alioth_flip_probe(uint64_t fb_phys, uint32_t stride, uint32_t width, uint32_t height);
extern "C" int alioth_flip_to(uint64_t phys, uint32_t timeout_us);
extern "C" uint32_t alioth_flip_misr(unsigned frames);
extern "C" void alioth_flip_misr_off(void);
extern "C" void alioth_flip_set_ready(bool on);
extern "C" uint32_t alioth_flip_latch_max_us(void);
extern "C" void alioth_flip_issue(uint64_t phys);
extern "C" int alioth_flip_wait(uint32_t timeout_us);
extern "C" void alioth_panel_start(void);
extern "C" uint32_t alioth_panel_rate(void);
extern "C" uint64_t alioth_panel_frame_ns(void);
extern "C" void alioth_scan_source(uint64_t phys, uint32_t w, uint32_t h, uint32_t stride);
extern "C" uint32_t alioth_panel_line(void);
extern "C" void alioth_pcc_set(const int32_t m[3][3], const int32_t c[3], int on);
extern "C" void alioth_pcc_test(unsigned mode);
extern "C" void hakios_gpu_to_memory(void * start, uint64_t size);
extern "C" uint64_t gPhysBase, gPhysSize;       /* (osfmk/arm: the physical memory XNU has) */

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSetColorRemapMode      = 51,       /* (work/research/color-matrix.md: these from the dyld cache) */
	kFBMethodSwapCancel             = 52,
	kFBMethodGetMatrix              = 54,
	kFBMethodSetMatrix              = 55,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodSwapSetColorMatrix     = 69,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);
/* Boot-arg fb_vsync=2: the vsync notification as {count, time, interval} (iOSPlus10), else {time, time, count}. */
static TUNABLE(uint32_t, gVSyncLayout, "fb_vsync", 1);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
/* fb_beat: 2 (default) the beat below unless frames are page-flipped, 1 always, 0 never */
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 2);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_bind=0: the helpers run wherever the scheduler puts them. By default the first three are
 * bound to XNU's cpus 1-3 (the A77s; cpu 0 is the prime A77, cpus 4-6 the A55s): woken, a helper left
 * to the scheduler often started only once the presenter had scaled most of the frame alone (~9 ms).
 */
static TUNABLE(uint32_t, gBind, "fb_bind", 1);
extern "C" processor_t thread_bind(processor_t processor);
extern "C" processor_t cpu_to_processor(int cpu);
extern "C" unsigned int processor_count;
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static uint32_t gScanHz;

static inline uint32_t
refreshHz(void)
{
	if (gScanHz != 0) return gScanHz;
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}

/*
 * The panel's own refresh, measured (alioth flips): its TE comes every 8.20-8.25 ms at "120 Hz", not
 * 8.33. Refreshes told at 120.0 a second drift through the panel's by a tenth of a millisecond a frame,
 * and CoreAnimation's frames, which start on those, landed now early in a TE period and now late --
 * fitting before the next TE or missing it by turns (2026-10-29). So the period told is the measured one,
 * and the refresh notifications keep to the TEs the flips are seen to be taken at (gTEAt).
 */
static uint64_t gTENs;                  /* the TE period, ns (0: not measured: 1 s / refreshHz()) */
static TUNABLE(uint32_t, gVSyncLead, "fb_vsync_lead", 3000);   /* us: refreshes told this much before the TE (armVSync()) */
static uint64_t gTEAt;                  /* a moment a TE came (a flip taken just then), mach time; 0: none seen */

static inline uint64_t
refreshNs(void)
{
	return gTENs != 0 ? gTENs : NSEC_PER_SEC / refreshHz();
}

/*
 * The panel's next TE after `now` (mach time), by the last one a flip was seen taken at and the measured period;
 * 0 when none has been seen yet. For the touch path (bsd/dev/hakios.input.c), which hands finger moves on in step
 * with the panel so that what they move is committed before CoreAnimation's next frame begins.
 */
extern "C" uint64_t hakios_display_next_te(uint64_t now);

extern "C" uint64_t
hakios_display_next_te(uint64_t now)
{
	uint64_t at = gTEAt, period;

	if (at == 0 || at > now) {
		return 0;
	}
	nanoseconds_to_absolutetime(refreshNs(), &period);
	return period == 0 ? 0 : at + ((now - at) / period + 1) * period;
}

/* A frame is on the panel (its flip taken at `when`): to whoever asked for frame info (the user client, below). */
static void frameInfoPresented(uint32_t swapID, uint64_t when);
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	IOBufferMemoryDescriptor * fComposeMemory;
	uint8_t *       fComposeBase;           /* command-mode composition, away from display DMA */
	bool            fFlip;                  /* alioth: frames drawn into the buffer not shown, then flipped */
	unsigned        fFlipFront;             /* which of fFlipBase the display engine scans */
	uint8_t *       fFlipBase[2];
	uint64_t        fFlipPhys[2];
	IOMemoryMap *   fFlipMap, * fFlipMap0;
	/* direct scan-out (aliothDirectTest()): the engine reads CoreAnimation's surface and scales it */
	bool            fDirect, fDirectOn;     /* allowed (tested); VIG0 is on a surface now */
	bool            fSingle;                /* (with fDirect) frames that are drawn here go into the first buffer only */
	IOMemoryMap *   fSlotMap;               /* the display's memory past the first buffer, for the test */
	MiniIOSurface * fDirectHeld;            /* the surface being scanned out: not CoreAnimation's to draw into */
	uint32_t        fDirectStride, fDirectWidth, fDirectHeight;     /* the source geometry VIG0 has now */
	uint32_t        fPendingSwapID;         /* the swap whose (drawn) frame the pending flip shows */
	uint32_t        fStatDirect;
	uint32_t        fEarlyPrev[2];          /* (fb_swap_early) the surfaces of the two direct frames before this one */
	uint32_t        fEarlyShown;            /* ... the surface on the panel when the last swap was over early */
	uint32_t        fDirectHeldID;          /* the swap fDirectHeld came in (fb_swap_wait: done when it is let go) */
	uint32_t        fStatEarly, fStatEarlyRisk;     /* swaps over early; frames that came in fEarlyShown */
	bool swapEarly(uint32_t sid);
	IOMemoryMap *   fMoreMap;               /* (fb_slots=3/4) the slots the loader took from the firmware's memory */
	bool aliothDirectTest(void);
	void aliothMoreSlots(void);
	bool aliothSlotSeen(uint64_t phys, uint8_t * pixels);
	MiniIOSurface * directSurface(const uint8_t * swap, MiniIOSurface ** surfaces);
	uint32_t        fFlipFlips, fFlipMissed;
	bool            fFlipPending;           /* a flip issued, not yet seen taken: fComposeBase is still scanned */
	bool aliothFlipStart(void);
	uint64_t        fCopyNs;
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	uint64_t        fNextTicket, fCompletedTicket;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 * Not with page flips (the K40's): the engine takes a flip at the panel's own TE whenever it is issued
	 * before it, and the beat -- a clock of this driver's, not the TE's -- only held frames back, at times
	 * past their TE: a drag's frame intervals at the 120 Hz pace 48% with it, 70% without (2026-10-30).
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fDrawNs;
	uint32_t        fStatScanWaits, fStatScanMissed;
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void completeSwap(void);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	volatile SInt32 fRowNext;               /* the next chunk of rows nobody has taken yet (takeRows()) */
	void takeRows(const struct RowJob * job, uint32_t * blendRow, unsigned drawer);
	uint64_t        fJobBegan;              /* (touch_lat=1: of the last frame, each drawer's chunks and when it began) */
	uint16_t        fJobChunks[8], fJobStartUs[8];
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	int32_t         fHoleX0, fHoleY0, fHoleX1, fHoleY1;     /* this frame's layers drawn so far, on the panel (drawLayer()) */
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

/*
 * Redmi K40 (alioth): page flips. The panel's transfer is held to its scan for the whole frame
 * (bsd/dev/alioth_dsi.c), so a frame copied into the buffer being scanned always tears; at 120 Hz there
 * is no idle moment at all. So the frame is drawn into a second buffer and the display engine is pointed
 * at it (alioth_flip_to(): VIG0's address, CTL0's flush, taken at the next frame). Both buffers lie in the
 * memory the bootloader reserved for the display (cont_splash_region 0x9c000000 + 0x2300000).
 *
 * Before any of that is relied on it is tested: the interface's signature (MISR) of an exact copy of the
 * shown frame must be the same, and of a copy with one pixel changed different. Then the panel goes to
 * its 120 Hz timing (panel_hz=, default 120) and CoreAnimation is told that rate. fb_flip=0: none of it.
 */
/*
 * The memory the bootloader keeps for the display (live DT cont_splash_region): the first buffer (the UEFI's
 * picture) at its start, the second 10 MB on -- or, with direct scan-out, from there two slots for the
 * surfaces CoreAnimation draws into (MiniIOSurfaceSetScanout()).
 */
static const uint64_t kDisplayPhys = 0x9C000000ULL, kDisplayBytes = 0x2300000ULL;
static const uint64_t kSlotsPhys = 0x9CA00000ULL, kSlotBytes = 0xC80000ULL;

bool
IOMobileFramebuffer::aliothFlipStart(void)
{
	const uint64_t phys0 = 0x9C000000ULL, phys1 = 0x9CA00000ULL, room = 0xA00000ULL;
	uint32_t on = 1;
	size_t bytes = (size_t) fRowBytes * fPanelHeight;

	PE_parse_boot_argn("fb_flip", &on, sizeof(on));
	if (!on || bytes > room || !alioth_flip_probe(phys0, fRowBytes, fPanelWidth, fPanelHeight)) {
		return false;
	}
	IOMemoryDescriptor * md = IOMemoryDescriptor::withPhysicalAddress(phys1, room, kIODirectionInOut);
	if (md == NULL) {
		return false;
	}
	fFlipMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	md->release();
	if (fFlipMap == NULL) {
		hakios_tl("alioth flip: the second buffer (%#llx) cannot be mapped\n", phys1);
		return false;
	}
	/*
	 * The first buffer too through a write-combined mapping of its own: the boot console's (fBase) is device
	 * memory, and every other frame -- the ones drawn into it -- took 7-9 ms to scale instead of 1-2 ms
	 * (2026-10-29). The console draws no more once a picture is up.
	 */
	fFlipBase[0] = fBase;
	md = IOMemoryDescriptor::withPhysicalAddress(phys0, room, kIODirectionInOut);
	if (md != NULL) {
		fFlipMap0 = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
		md->release();
	}
	if (fFlipMap0 != NULL) {
		fFlipBase[0] = (uint8_t *) fFlipMap0->getVirtualAddress();
	}
	fFlipPhys[0] = phys0;
	fFlipBase[1] = (uint8_t *) fFlipMap->getVirtualAddress();
	fFlipPhys[1] = phys1;

	memcpy(fFlipBase[1], fFlipBase[0], bytes);
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t a = alioth_flip_misr(2);
	int t1 = alioth_flip_to(phys1, 100000);
	uint32_t b = alioth_flip_misr(2);
	uint32_t * px = (uint32_t *) (fFlipBase[0] + (size_t) (fPanelHeight / 2) * fRowBytes + (fPanelWidth / 2) * 4);
	uint32_t saved = *px;
	*px = saved ^ 0x00ffffffu;                      /* one pixel in the middle, in the buffer not shown */
	__asm__ volatile ("dsb sy" ::: "memory");
	int t2 = alioth_flip_to(phys0, 100000);
	uint32_t c = alioth_flip_misr(2);
	*px = saved;
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t d = alioth_flip_misr(2);
	bool pass = t1 && t2 && a != 0 && a == b && c != a && d == a;
	hakios_tl("alioth flip: test: signature shown %#x, copy %#x (%s), changed copy %#x (%s), restored %#x; flips %s; %s\n",
	    a, b, a == b ? "same" : "DIFFERENT", c, c != a ? "different" : "SAME", d, t1 && t2 ? "taken" : "NOT TAKEN",
	    pass ? "the display engine reads our buffers: frames are flipped" : "no flips");
	alioth_flip_misr_off();
	if (pass) {
		uint32_t pcc_test = 0;

		/* (boot-arg pcc_test=1/2: is DSPP_0's colour correction in this path, and in which numbers -- alioth_dsi.c) */
		if (PE_parse_boot_argn("pcc_test", &pcc_test, sizeof(pcc_test)) && pcc_test != 0 && pcc_test != 3) {
			alioth_pcc_test(pcc_test);
		}
		/*
		 * pcc_test=3: what the PCC's constant term counts in. The coefficients' 1.0 is 0x8000 (pcc_test=1), but an
		 * invert made with constant 0x8000 showed a black screen (2026-10-31 19:53). So: the signature of a white
		 * frame, then a black frame with all coefficients 0 and only a constant, for constants doubling from 0x40:
		 * the smallest that gives white's signature is the constant's full scale (or more, clamped). Under 1 s.
		 */
		if (pcc_test == 3) {
			static const int32_t zero[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
			uint32_t * p = (uint32_t *) fFlipBase[1], white, black, seen;
			size_t n = bytes / 4;

			for (size_t i = 0; i < n; i++) {
				p[i] = 0xffffffffu;
			}
			__asm__ volatile ("dsb sy" ::: "memory");
			(void) alioth_flip_to(phys1, 100000);
			white = alioth_flip_misr(2);
			for (size_t i = 0; i < n; i++) {
				p[i] = 0xff000000u;
			}
			__asm__ volatile ("dsb sy" ::: "memory");
			black = alioth_flip_misr(2);
			hakios_tl("alioth pcc: constant probe: white %#x, black %#x\n", white, black);
			for (int32_t c = 0x40; c <= 0x20000; c <<= 1) {
				int32_t k = c > 0x1ffff ? 0x1ffff : c, cs[3] = { k, k, k };

				alioth_pcc_set(zero, cs, 1);
				seen = alioth_flip_misr(2);
				hakios_tl("alioth pcc: constant %#x alone: %#x (%s)\n", k, seen, seen == white ? "WHITE" : seen == black ?
				    "black" : "between, or something else");
				k = k - 1;      /* (and one under: full scale may be 2^n - 1) */
				cs[0] = cs[1] = cs[2] = k;
				alioth_pcc_set(zero, cs, 1);
				seen = alioth_flip_misr(2);
				hakios_tl("alioth pcc: constant %#x alone: %#x (%s)\n", k, seen, seen == white ? "WHITE" : seen == black ?
				    "black" : "between, or something else");
			}
			alioth_pcc_set(zero, zero[0], 0);
			alioth_flip_misr_off();
			memcpy(fFlipBase[1], fFlipBase[0], bytes);
			__asm__ volatile ("dsb sy" ::: "memory");
			(void) alioth_flip_to(phys0, 100000);
		}
	}
	if (!pass) {
		alioth_flip_to(phys0, 100000);
		fFlipMap->release();
		fFlipMap = NULL;
		OSSafeReleaseNULL(fFlipMap0);
		return false;
	}
	alioth_flip_set_ready(true);
	alioth_panel_start();
	uint64_t frame_ns = alioth_panel_frame_ns();
	if (frame_ns >= 6000000 && frame_ns <= 20000000) {
		gTENs = frame_ns;               /* (refreshNs(): the period told, by CoreAnimation's mode and refresh notifications) */
	}
	/* how fast each buffer takes a frame's worth of writes (the one not shown; then the picture back in it) */
	uint64_t w1 = mach_absolute_time(), w0;
	memset(fFlipBase[1], 0, bytes);
	w1 = mach_absolute_time() - w1;
	memcpy(fFlipBase[1], fFlipBase[0], bytes);
	int t3 = alioth_flip_to(phys1, 100000);
	w0 = mach_absolute_time();
	memset(fFlipBase[0], 0, bytes);
	w0 = mach_absolute_time() - w0;
	memcpy(fFlipBase[0], fFlipBase[1], bytes);
	__asm__ volatile ("dsb sy" ::: "memory");
	hakios_tl("alioth flip: buffers at %p (%s) and %p (boot console's %p): a frame of writes takes %llu us and %llu us\n",
	    fFlipBase[0], fFlipMap0 != NULL ? "own mapping" : "the console's", fFlipBase[1], fBase, micros(w0), micros(w1));
	fFlipFront = 1;
	fComposeBase = fFlipBase[0];
	fFlip = true;
	hakios_tl("alioth flip: on: panel at %u Hz (TE every %llu us), flip at that rate %s, longest wait %u us\n",
	    alioth_panel_rate(), frame_ns / 1000, t3 ? "taken" : "NOT TAKEN", alioth_flip_latch_max_us());
	if (aliothDirectTest()) {
		/* the display's memory past the first buffer: two slots for CoreAnimation's surfaces of the display's size */
		fDirect = fSingle = true;
		fFlipFront = 0;
		fComposeBase = fFlipBase[0];
		MiniIOSurfaceSetScanout(fWidth, fHeight, kSlotsPhys, kSlotBytes, 2);
		aliothMoreSlots();
	}
	return true;
}

/*
 * fb_slots=4 (the default; 3: one more; 2: as before 2026-11-03): more slots for CoreAnimation's surfaces. It asks for
 * three of the display's size, and one came in ordinary memory ("all 2 scan-out slots taken"): every frame drawn into
 * that one was then scaled by the processors instead of the engine -- half the frames of a game (logs/230748-play.log).
 * The bootloader's display memory holds two. XNU's own memory has no piece of 12.5 MB below 4 GB (the engine's source
 * address has 32 bits; the window begins at 0xc0000000 and the system image fills it from 0xc8000000: fb_slots=3 asked
 * it in vain, 2026-11-03), but the firmware has: the loader takes up to two once a session, below 0xc0000000 and so
 * outside XNU's memory like the display's own, and says where (fb_slots_pa=, fb_slots_more=; loader.c scan_slots_take).
 * Each is used only once the engine is seen to read it, by the same signature test as aliothDirectTest()'s.
 */
static void fillPixels(uint8_t * base, uint32_t rowBytes, uint32_t width, uint32_t height, uint32_t value);

void
IOMobileFramebuffer::aliothMoreSlots(void)
{
	uint32_t slots = 4, more = 0, good = 0;
	uint64_t at = 0;

	PE_parse_boot_argn("fb_slots", &slots, sizeof(slots));
	PE_parse_boot_argn("fb_slots_pa", &at, sizeof(at));
	PE_parse_boot_argn("fb_slots_more", &more, sizeof(more));
	if (slots <= 2 || fSlotMap == NULL) {
		return;
	}
	if (at == 0 || more == 0) {
		hakios_tl("alioth scan: the loader took no more slots (an older loader, or no room below 4 GB): two slots\n");
		return;
	}
	more = more > slots - 2 ? slots - 2 : more;
	more = more > 2 ? 2 : more;
	if ((at & PAGE_MASK) != 0 || at + more * kSlotBytes > 0x100000000ULL) {
		hakios_tl("alioth scan: the loader's slots at %#llx (%u) are not below 4 GB in whole pages: two slots\n", at, more);
		return;
	}
	IOMemoryDescriptor * md = IOMemoryDescriptor::withPhysicalAddress(at, more * kSlotBytes, kIODirectionInOut);
	if (md != NULL) {
		fMoreMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
		md->release();
	}
	if (fMoreMap == NULL) {
		hakios_tl("alioth scan: the loader's slots at %#llx cannot be mapped: two slots\n", at);
		return;
	}
	uint8_t * base = (uint8_t *) fMoreMap->getVirtualAddress();
	for (uint32_t i = 0; i < more; i++) {
		uint64_t phys = at + i * kSlotBytes;

		if (aliothSlotSeen(phys, base + i * kSlotBytes)) {
			bzero(base + i * kSlotBytes, kSlotBytes);
			__asm__ volatile ("dsb sy" ::: "memory");
			MiniIOSurfaceAddScanoutSlot(phys);
			good++;
		}
	}
	hakios_tl("alioth scan: %u slots for CoreAnimation's surfaces (%u of the loader's %u)\n", 2 + good, good, more);
}

/* Does the engine read a slot at `phys` (`pixels` mapped)? The panel's size all of one colour in the first slot, shown as it
 * is; then the slot, the display's size, scaled: the same signature; of another colour another. */
bool
IOMobileFramebuffer::aliothSlotSeen(uint64_t phys, uint8_t * pixels)
{
	const uint32_t colour = 0xff3a6ea5, other = 0xffa53a6e;
	uint32_t stride = (fWidth * 4 + 63) & ~63u;
	uint8_t * ref = (uint8_t *) fSlotMap->getVirtualAddress();

	fillPixels(ref, fRowBytes, fPanelWidth, fPanelHeight, colour);
	__asm__ volatile ("dsb sy" ::: "memory");
	alioth_flip_to(kSlotsPhys, 100000);
	uint32_t a = alioth_flip_misr(2);
	fillPixels(pixels, stride, fWidth, fHeight, colour);
	__asm__ volatile ("dsb sy" ::: "memory");
	alioth_scan_source(phys, fWidth, fHeight, stride);
	int taken = alioth_flip_wait(100000);
	uint32_t b = alioth_flip_misr(2);
	fillPixels(pixels, stride, fWidth, fHeight, other);
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t c = alioth_flip_misr(2);
	alioth_scan_source(fFlipPhys[0], 0, 0, 0);
	int back_taken = alioth_flip_wait(100000);
	alioth_flip_misr_off();
	bool pass = taken && back_taken && a != 0 && b == a && c != a;
	hakios_tl("alioth scan: a slot at %#llx: panel-size %#x, scaled %#x (%s), other colour %#x (%s); %s\n", phys, a, b,
	    b == a ? "same" : "DIFFERENT", c, c != a ? "different" : "SAME", pass ? "used" : "NOT USED");
	return pass;
}

static void
fillPixels(uint8_t * base, uint32_t rowBytes, uint32_t width, uint32_t height, uint32_t value)
{
	for (uint32_t y = 0; y < height; y++) {
		uint32_t * row = (uint32_t *) (base + (size_t) y * rowBytes);

		for (uint32_t x = 0; x < width; x++) {
			row[x] = value;
		}
	}
}

/*
 * Direct scan-out (boot-arg fb_direct=0: none). Scaling each frame to the panel's size on the processors cost
 * 1-2 ms, and every other frame 7-9 ms (2026-10-29): the display engine can do it, reading the surface
 * CoreAnimation drew into where it is (MiniIOSurface gives surfaces of the display's size memory in one
 * piece below 4 GB) and scaling it with VIG0's scaler (alioth_scan_source()). Tested before it is used,
 * by the interface's signature: a buffer of the display's size all of one colour, scaled, must give the
 * signature of the panel's size all of that colour; of another colour another.
 */
bool
IOMobileFramebuffer::aliothDirectTest(void)
{
	const uint32_t colour = 0xff3a6ea5, other = 0xffa53a6e;
	uint32_t on = 1, stride = (fWidth * 4 + 63) & ~63u;
	size_t bytes = (size_t) stride * fHeight, panel = (size_t) fRowBytes * fPanelHeight;

	PE_parse_boot_argn("fb_direct", &on, sizeof(on));
	if (!on || bytes > kSlotBytes || panel > kSlotBytes || kSlotsPhys + 2 * kSlotBytes > kDisplayPhys + kDisplayBytes) {
		return false;
	}
	/* the picture into the first buffer and that shown: the second one's place is the slots' */
	memcpy(fFlipBase[0], fFlipBase[fFlipFront], panel);
	__asm__ volatile ("dsb sy" ::: "memory");
	alioth_flip_to(fFlipPhys[0], 100000);
	fFlipFront = 0;
	IOMemoryDescriptor * md = IOMemoryDescriptor::withPhysicalAddress(kSlotsPhys, 2 * kSlotBytes, kIODirectionInOut);
	if (md != NULL) {
		fSlotMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
		md->release();
	}
	if (fSlotMap == NULL) {
		hakios_tl("alioth scan: the display's memory past the first buffer cannot be mapped: no direct scan-out\n");
		return false;
	}
	uint8_t * ref = (uint8_t *) fSlotMap->getVirtualAddress(), * src = ref + kSlotBytes;

	/* the panel's size all of one colour, shown as it is */
	fillPixels(ref, fRowBytes, fPanelWidth, fPanelHeight, colour);
	__asm__ volatile ("dsb sy" ::: "memory");
	alioth_flip_to(kSlotsPhys, 100000);
	uint32_t a = alioth_flip_misr(2);
	/* the display's size all of that colour, scaled by the engine */
	fillPixels(src, stride, fWidth, fHeight, colour);
	__asm__ volatile ("dsb sy" ::: "memory");
	alioth_scan_source(kSlotsPhys + kSlotBytes, fWidth, fHeight, stride);
	int taken = alioth_flip_wait(100000);
	uint32_t b = alioth_flip_misr(2);
	fillPixels(src, stride, fWidth, fHeight, other);
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t c = alioth_flip_misr(2);
	/* back to the picture, as it was */
	alioth_scan_source(fFlipPhys[0], 0, 0, 0);
	int back_taken = alioth_flip_wait(100000);
	uint32_t d = alioth_flip_misr(2);
	alioth_flip_misr_off();
	bool pass = taken && back_taken && a != 0 && b == a && c != a && d != a;
	if (!pass) {
		/* (the second buffer is drawn into again, for the next frame, as before) */
		fComposeBase = fFlipBase[1];
	}
	hakios_tl("alioth scan: test: one colour %ux%u -> %ux%u: panel-size %#x, scaled %#x (%s), other colour %#x (%s), back %#x; %s\n",
	    fWidth, fHeight, fPanelWidth, fPanelHeight, a, b, b == a ? "same" : "DIFFERENT", c, c != a ? "different" : "SAME", d,
	    pass ? "frames are scanned out of CoreAnimation's surfaces" : "no direct scan-out");
	return pass;
}

/* The swap's surface, if it can be scanned out as it is: one layer, of the display's size, all of it shown. */
MiniIOSurface *
IOMobileFramebuffer::directSurface(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];

	if (!fDirect || fRecBase != NULL || fDiff || s == NULL || s->scanPhys == 0) {
		return NULL;
	}
	for (unsigned layer = 1; layer < SWAP_LAYERS; layer++) {
		if (swapWord(swap, SWAP_SURFACE(layer)) != 0) {
			return NULL;
		}
	}
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	if (s->info.width != fWidth || s->info.height != fHeight || s->info.bytesPerElement != 4 || s->info.planeCount != 0 ||
	    (src[2] > 0 && (src[0] != 0 || src[1] != 0 || (uint32_t) src[2] != fWidth || (uint32_t) src[3] != fHeight)) ||
	    (dst[2] > 0 && (dst[0] != 0 || dst[1] != 0 || (uint32_t) dst[2] != fWidth || (uint32_t) dst[3] != fHeight))) {
		return NULL;
	}
	return s;
}

/*
 * fb_swap_early (default 2 since 2026-11-07: measured, CoreAnimation does draw the next frame into the surface shown
 * until the TE -- every frame came in it, logs/231648-fpsq2e2b.log -- but the owner, scrolling and flicking pages, saw no
 * tearing, and frames that missed their TE went from 18% to 2%; 1 or 0 to go back):
 * the swap of a frame scanned out of its surface is over -- CoreAnimation back from SwapEnd
 * -- once the flip to it is issued, not once the engine has taken it at the next TE. Waiting held CoreAnimation up to a
 * refresh at every frame, its next one begun only after it: 231 of 379 swaps came a refresh late in a scroll
 * (logs/release-1104.log). Over early, the surface on the panel until that TE might be the one it draws into next. So,
 * with 1, only when its last three frames came in three different surfaces: it draws into them in turn, and the next
 * is then the one shown two frames ago, which the engine no longer reads. 2: always (the surfaces on the panel read as
 * in use, MiniIOSurfaceUse(), to a drawer that asks IOSurfaceIsInUse()); 0: never, as before. The statistics say how
 * many frames still came in the surface that was on the panel as the swap before was over ("fEarlyShown").
 */
static TUNABLE(uint32_t, gSwapEarly, "fb_swap_early", 2);

/*
 * fb_swap_wait=1 (2026-11-07; 0 the default until measured): SwapWait (selector 6) tells the truth. CoreAnimation asks it
 * of every swap it means to draw into again -- QuartzCore's swap_wait_timeout() calls IOMobileFramebufferSwapWait, and
 * while that is not 0 sleeps 100 us and asks again, within a time of its own -- and IOMFBDisplay::can_update() keeps a
 * second swap in flight ("triple_buffered", its third framebuffer: AppleDisplay makes three) only while one is still
 * pending. Answered "done" at once, as before, it never was: two surfaces in turn, and with fb_swap_early it drew into
 * the one on the panel. A swap is done once its surfaces are read no more: scanned out, when the next flip is taken
 * (the surface held until then, fDirectHeld); drawn here, when that is over. Only one of the last 8 swaps made and not
 * done is answered busy (kIOReturnBusy): anything else is done, so that a count gone wrong cannot hold CoreAnimation.
 */
static TUNABLE(uint32_t, gSwapWaitTruth, "fb_swap_wait", 0);
static uint32_t gSwapDoneID, gSwapMadeID;      /* the newest swap done, the newest made (SwapEnd) */
static uint32_t gStatSwapWaitBusy, gStatSwapWaitAsked;

static void
swapDone(uint32_t id)
{
	uint32_t was = __atomic_load_n(&gSwapDoneID, __ATOMIC_RELAXED);

	if (id != 0 && (was == 0 || (int32_t) (id - was) > 0)) {
		__atomic_store_n(&gSwapDoneID, id, __ATOMIC_RELEASE);
	}
}

static bool
swapPending(uint32_t id)
{
	uint32_t done = __atomic_load_n(&gSwapDoneID, __ATOMIC_ACQUIRE), made = __atomic_load_n(&gSwapMadeID, __ATOMIC_ACQUIRE);

	return gSwapWaitTruth != 0 && id != 0 && done != 0 && (int32_t) (id - done) > 0 && (int32_t) (id - done) <= 8 &&
	       (int32_t) (made - id) >= 0;
}

bool
IOMobileFramebuffer::swapEarly(uint32_t sid)
{
	bool turn = sid != fEarlyPrev[0] && sid != fEarlyPrev[1] && fEarlyPrev[0] != fEarlyPrev[1] && fEarlyPrev[1] != 0;

	fEarlyPrev[1] = fEarlyPrev[0];
	fEarlyPrev[0] = sid;
	return gSwapEarly >= 2 || (gSwapEarly == 1 && turn);
}

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = (uint32_t) ((65536ULL * NSEC_PER_SEC + refreshNs() / 2) / refreshNs());     /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	hakios_tl("display: mode for CoreAnimation: %ux%u at %u Hz (SyncRate %#x)\n", fWidth, fHeight, refreshHz(), rate);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;
	fComposeMemory = NULL;
	fComposeBase = NULL;
	fFlip = false;
	fFlipMap = NULL;
	fFlipFlips = fFlipMissed = 0;
	fFlipPending = false;
	fCopyNs = 4000000;
	fNextTicket = fCompletedTicket = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	/* the panel's scan, if it can be read: its rate is then the one published, unless fb_hz= is given */
	if (aliothFlipStart()) {
		uint32_t hz;

		if (!PE_parse_boot_argn("fb_hz", &hz, sizeof(hz))) {
			gScanHz = alioth_panel_rate();
		}
		IOLog("MiniIOMobileFramebuffer: frames are flipped, at %u a second\n", refreshHz());
	} else if (hakios_display_scan_start()) {
		uint32_t hz;
		const struct hakios_display_scan * scan = hakios_display_scan();

		if (!PE_parse_boot_argn("fb_hz", &hz, sizeof(hz)) && scan != NULL) {
			gScanHz = (scan->millihz + 500) / 1000;
		}
		IOLog("MiniIOMobileFramebuffer: frames follow the panel's scan, at %u a second\n", refreshHz());
		if (hakios_display_command_mode()) {
			uint64_t bytes = (uint64_t)fRowBytes * fPanelHeight;
			if (bytes <= 64 * 1024 * 1024)
				fComposeMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
				    kIODirectionInOut, bytes, PAGE_SIZE);
			if (fComposeMemory != NULL) {
				fComposeBase = (uint8_t *)fComposeMemory->getBytesNoCopy();
				memcpy(fComposeBase, fBase, bytes);
				hakios_tl("display: command-mode staging buffer: %llu bytes\n", bytes);
			} else hakios_tl("display: no staging buffer; command-mode presentation remains unsynchronized\n");
		}
	}
	publishModes();
	{
		/* CoreAnimation's heartbeat: one refresh, in mach time (boot-arg fb_carefresh=0: none, 1/60 s as before) */
		uint32_t on = 1;
		uint64_t step = 0;
		OSDictionary * refresh = OSDictionary::withCapacity(2);

		PE_parse_boot_argn("fb_carefresh", &on, sizeof(on));
		nanoseconds_to_absolutetime(refreshNs(), &step);
		if (on && refresh != NULL) {
			OSNumber * n = OSNumber::withNumber(step, 64);
			OSNumber * m = OSNumber::withNumber((refreshNs() << 32) / NSEC_PER_SEC, 32);  /* seconds in 2^-32: the field CA uses (fpsq, 2026-10-29) */
			if (n != NULL && m != NULL) {
				refresh->setObject("displayRefreshStepMachTime", n);
				refresh->setObject("displayMinRefreshInterval", m);
				setProperty("IOMFBDisplayRefresh", refresh);
				hakios_tl("display: IOMFBDisplayRefresh: a refresh is %llu ticks (%u Hz)\n", step, refreshHz());
			}
			OSSafeReleaseNULL(n);
			OSSafeReleaseNULL(m);
		}
		OSSafeReleaseNULL(refresh);
	}
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
	int32_t         hx0, hy0, hx1, hy1;     /* where the layers below were drawn: this one goes over them there */
};

/*
 * One premultiplied pixel over another (source over, as CoreAnimation means its layers). A layer above another --
 * CoreAnimation's frame over an application's CAMetalLayer, which it hands the display as a layer of its own and leaves
 * a transparent hole for (2026-11-08) -- was copied over it, and the hole showed black.
 */
static inline uint32_t
pixel_over(uint32_t s, uint32_t d)
{
	uint32_t ia = 255 - (s >> 24), rb, ag;

	if (ia == 0) {
		return s;
	}
	if (ia == 255) {
		return d;               /* (alpha 0: nothing of it) */
	}
	rb = (d & 0x00ff00ff) * ia;
	ag = ((d >> 8) & 0x00ff00ff) * ia;
	rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00ff00ff)) >> 8) & 0x00ff00ff;
	ag = (ag + 0x00800080 + ((ag >> 8) & 0x00ff00ff)) & 0xff00ff00;
	return s + (rb | ag);
}

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;
		/* (over the layers below, where they are: [h0, h1) of this row; elsewhere copied, as always) */
		int32_t h0 = job->hx0 > cx0 ? job->hx0 : cx0, h1 = job->hx1 < cx1 ? job->hx1 : cx1;
		bool holed = y >= job->hy0 && y < job->hy1 && h1 > h0;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) ((fComposeBase != NULL ? fComposeBase : fBase) + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			const uint32_t * row = (const uint32_t *) (pixels + (size_t) (sy + y - py0) * stride) + sx - px0;

			if (!holed) {
				memcpy(to + px0, row + px0, (size_t) sw * 4);
				continue;
			}
			memcpy(to + px0, row + px0, (size_t) (h0 - px0) * 4);
			for (int32_t x = h0; x < h1; x++) {
				to[x] = pixel_over(row[x], to[x]);
			}
			memcpy(to + h1, row + h1, (size_t) (px1 - h1) * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (holed) {
			/* a row over the layers below: the plain loops, with the stretch over them blended */
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? (fColumnWeight[x] == 0 || from[fColumn[x]] == from[fColumn[x] + 1] ?
				    from[fColumn[x]] : blend(from[fColumn[x]], from[fColumn[x] + 1], fColumnWeight[x])) : from[fColumn[x]];

				to[x] = x >= h0 && x < h1 ? pixel_over(value, to[x]) : value;
			}
		} else if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/*
 * The rows of a job in chunks, taken in turn by whoever of the drawers is free. Fixed shares made a
 * frame wait for the slowest of them: of the phone's seven processors three are A55s (XNU's cpus 4-6,
 * 1.8 GHz), and a share that landed on one took the scaling from 2-3 ms to 8-11 ms (2026-10-29).
 */
#define ROW_CHUNK       48

void
IOMobileFramebuffer::takeRows(const RowJob * job, uint32_t * blendRow, unsigned drawer)
{
	uint16_t chunks = 0;

	for (;;) {
		int32_t y = OSAddAtomic(ROW_CHUNK, &fRowNext);

		if (y >= job->py1) {
			break;
		}
		if (chunks++ == 0 && drawer < 8) {
			fJobStartUs[drawer] = (uint16_t) micros(mach_absolute_time() - fJobBegan);
		}
		drawRows(job, y, y + ROW_CHUNK < job->py1 ? y + ROW_CHUNK : job->py1, blendRow);
	}
	if (drawer < 8) {
		fJobChunks[drawer] = chunks;
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	if (gBind && index < 3 && index + 1 < processor_count) {
		thread_bind(cpu_to_processor((int) index + 1));
		thread_block(THREAD_CONTINUE_NULL);
	}
	for (;;) {
		const RowJob * job;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		fb->takeRows(job, fb->fHelperBlend[index], index + 1);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	/* over what the layers below this one drew in this frame (none for the bottom one: show() empties it) */
	job.hx0 = fHoleX0;
	job.hy0 = fHoleY0;
	job.hx1 = fHoleX1;
	job.hy1 = fHoleY1;
	if (fHoleX1 <= fHoleX0) {
		fHoleX0 = cx0, fHoleY0 = py0, fHoleX1 = cx1, fHoleY1 = py1;
	} else {
		fHoleX0 = cx0 < fHoleX0 ? cx0 : fHoleX0;
		fHoleY0 = py0 < fHoleY0 ? py0 : fHoleY0;
		fHoleX1 = cx1 > fHoleX1 ? cx1 : fHoleX1;
		fHoleY1 = py1 > fHoleY1 ? py1 : fHoleY1;
	}
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: this thread and the helpers take chunks of rows until none are left, then it waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fRowNext = py0;
	fJobBegan = mach_absolute_time();
	bzero(fJobChunks, sizeof(fJobChunks));
	bzero(fJobStartUs, sizeof(fJobStartUs));
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	takeRows(&job, fBlendRow, 0);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(refreshNs() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	alioth_refresh_activity();
	__atomic_store_n(&gSwapMadeID, swapWord(swap, SWAP_ID), __ATOMIC_RELEASE);
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			swapDone(swapWord(swap, SWAP_ID));      /* (never shown: nothing of it is read, fb_swap_wait) */
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	uint64_t ticket = ++fNextTicket;
	IOLockWakeup(fSwapLock, fSwap, true);
	/* A source surface must not be handed back to CoreAnimation while the CPU
	 * is still scaling it into the command-mode staging buffer. Once staged,
	 * the panel wait no longer needs the source and can overlap the next render.
	 */
	while (fComposeBase != NULL && fCompletedTicket < ticket) {
		if (IOLockSleepDeadline(fSwapLock, &fCompletedTicket, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fCompletedTicket < ticket) {
			hakios_tl("display: staged swap completion timed out\n");
			break;
		}
	}
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::completeSwap(void)
{
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	fCompletedTicket++;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockWakeup(fSwapLock, &fCompletedTicket, false);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report, stagedReleased = false;

	if (fFlip && fFlipPending) {
		/* the buffer about to be drawn into is the one the last flip moved away from: once the engine has
		 * taken that flip (at the next frame's start) it reads this one no more */
		uint64_t asked = mach_absolute_time();

		if (alioth_flip_wait(40000)) {
			uint64_t taken = mach_absolute_time();

			fFlipFlips++;
			if (micros(taken - asked) > 150) {
				gTEAt = taken;
			}
			frameInfoPresented(fPendingSwapID, taken);      /* (the frame drawn here last: now on the panel) */
		} else {
			fFlipMissed++;
			fStatScanMissed++;
		}
		fFlipPending = false;
	}

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	uint32_t line_ready = alioth_panel_line();      /* (touch_lat: where the engine's transfer was as the scaling began) */
	if (hakios_display_scan() != NULL && !hakios_display_command_mode()) {
		/*
		 * Where the copy may start (fb_scan): every band of rows (fThreads of them, drawn side by side, each top
		 * to bottom in about fDrawNs) has to stay behind the scan in this refresh -- written after the scan has
		 * passed it, so it is first shown in the next -- and to be done before the next refresh's scan gets to
		 * it. The last band starts below the scan and must not catch it up; the first must be done before the
		 * scan comes round to its last row. That is a window of lines for the scan to be in when the copy starts:
		 *   from  max(shown (n-1)/n, shown - D/line) and   up to  lines + shown/n - D/line.
		 * Waited for, a frame and a half at most: a frame is never held longer than that for it.
		 */
		const struct hakios_display_scan * scan = hakios_display_scan();
		uint64_t d = fDrawNs != 0 ? fDrawNs : scan->frame_ns / 3, dl = d / scan->line_ns;
		uint32_t n = fThreads != 0 ? fThreads : 1, S = scan->shown, T = scan->lines;
		uint64_t lo = (uint64_t) S * (n - 1) / n, hi = (uint64_t) T + S / n;

		lo = S > dl && S - dl > lo ? S - dl : lo;
		lo += 16;
		hi = hi > dl + 16 ? hi - dl - 16 : 0;
		if (hi > T) {
			hi = T;
		}
		if (lo >= hi) {
			lo = S;                         /* (a copy too slow for any window: at least start in the blanking) */
			hi = T;
		}
		fStatScanWaits++;
		if (!hakios_display_wait_lines((uint32_t) lo, (uint32_t) hi, scan->frame_ns * 3 / 2)) {
			fStatScanMissed++;
		}
		fBeat = began = mach_absolute_time();
	} else if (!hakios_display_command_mode() && (gOnTheBeat == 1 || (gOnTheBeat == 2 && !fFlip))) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/*
	 * Direct scan-out: the engine is pointed at the surface itself (scaled by it), and the swap is over once it
	 * has taken it -- from then on the surface shown before is read no more, and CoreAnimation, which draws into
	 * its surfaces in turn, may draw into that one again. Nothing is drawn here.
	 */
	MiniIOSurface * direct = directSurface(swap, surfaces);
	if (direct != NULL) {
		/*
		 * The engine's source geometry is the surface's own: surfaces of the display's size come with rows of
		 * different lengths (the framebuffer's default one 4608 bytes, CoreAnimation's 4544), and a flip of the
		 * address alone onto a surface of other rows showed it sheared (2026-10-29).
		 */
		if (!fDirectOn || direct->info.bytesPerRow != fDirectStride || direct->info.width != fDirectWidth ||
		    direct->info.height != fDirectHeight) {
			alioth_scan_source(direct->scanPhys, direct->info.width, direct->info.height, direct->info.bytesPerRow);
			fDirectOn = true;
			fDirectStride = direct->info.bytesPerRow;
			fDirectWidth = direct->info.width;
			fDirectHeight = direct->info.height;
		} else {
			alioth_flip_issue(direct->scanPhys);
		}
		/* (in use from now: the engine reads it from the next TE until the flip after this one is taken; held from before
		 * the swap may be over, its maker free to let go of it then) */
		MiniIOSurfaceUse(direct, 1);
		MiniIOSurface * held = MiniIOSurfaceCopy(direct->sid);
		fStatEarlyRisk += fEarlyShown != 0 && direct->sid == fEarlyShown;
		fEarlyShown = 0;
		bool early = swapEarly(direct->sid);
		if (early) {
			fEarlyShown = fDirectHeld != NULL ? fDirectHeld->sid : 0;
			completeSwap();
			stagedReleased = true;
			fStatEarly++;
		}
		shown = mach_absolute_time();
		if (alioth_flip_wait(40000)) {
			uint64_t taken = mach_absolute_time();

			fFlipFlips++;
			if (micros(taken - shown) > 150) {
				gTEAt = taken;          /* (taken while waited for: a TE came just now) */
			}
			frameInfoPresented(swapWord(swap, SWAP_ID), taken);
		} else {
			fFlipMissed++;
			fStatScanMissed++;
		}
		if (fDirectHeld != NULL) {
			MiniIOSurfaceUse(fDirectHeld, -1);
			MiniIOSurfaceDrop(fDirectHeld);
			swapDone(fDirectHeldID);        /* (its surface read no more: the flip after it is taken) */
		}
		fDirectHeld = held;
		fDirectHeldID = swapWord(swap, SWAP_ID);
		/* the frames' surfaces, here too: how many CoreAnimation turns (fb_swap_wait) */
		for (unsigned i = 0; i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == direct->sid) {
				fStatSurfaces[i] = direct->sid;
				break;
			}
		}
		if (!early) {
			completeSwap();
			stagedReleased = true;
		}
		fStatDirect++;
		bzero(fJobChunks, sizeof(fJobChunks));
		bzero(fJobStartUs, sizeof(fJobStartUs));
	}
	/* Staged sources stay owned until the CPU has consumed every layer. */
	if (fComposeBase == NULL && direct == NULL) completeSwap();
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	{
		/* (and whenever the layers in use change: each layer's surface, its size and where it goes -- an application's
		 * CAMetalLayer given a layer of its own, 2026-11-08, showed black there) */
		uint32_t now = 0;

		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			now |= swapWord(swap, SWAP_SURFACE(layer)) != 0 ? 1u << layer : 0;
		}
		report = report || (direct == NULL && now != fLayersShown && now > 1);
	}
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	layers = direct != NULL ? 1u : 0u;
	fHoleX0 = fHoleY0 = fHoleX1 = fHoleY1 = 0;      /* (nothing below the first layer drawn) */
	for (unsigned layer = 0; direct == NULL && layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero((fComposeBase != NULL ? fComposeBase : fBase) + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	if (fComposeBase != NULL && direct == NULL) {
		/* Only this presenter writes fComposeBase. All source pixels have now
		 * been consumed, so CoreAnimation may render its next frame while we
		 * wait for TE and copy this private buffer to the panel. Recordings
		 * still sample source pixels below, so keep their old ownership span.
		 */
		if (fRecBase == NULL) {
			completeSwap();
			stagedReleased = true;
		}
		const struct hakios_display_scan * scan = hakios_display_scan();
		fStatScanWaits++;
		if (fFlip) {
			/* (fSingle: the second buffer's place is the scan-out slots'; frames drawn here all go into the first) */
			unsigned back = fSingle ? 0 : fFlipFront ^ 1;

			shown = mach_absolute_time();
			if (fDirectOn) {
				/* from a surface back to our own buffers: the engine's own setting again, and the surface let go once taken */
				alioth_scan_source(fFlipPhys[back], 0, 0, 0);
				fDirectOn = false;
				(void) alioth_flip_wait(40000);
				fEarlyPrev[0] = fEarlyPrev[1] = fEarlyShown = 0;
				if (fDirectHeld != NULL) {
					MiniIOSurfaceUse(fDirectHeld, -1);
					MiniIOSurfaceDrop(fDirectHeld);
					swapDone(fDirectHeldID);
					fDirectHeld = NULL;
				}
				fFlipPending = false;
			} else {
				alioth_flip_issue(fFlipPhys[back]);
				fFlipPending = true;
				fPendingSwapID = swapWord(swap, SWAP_ID);
			}
			fFlipFront = back;
			fComposeBase = fFlipBase[fSingle ? 0 : back ^ 1];
			if (fSwapsShown < 3 || (fFlipFlips + fFlipMissed) % 1200 == 0) {
				hakios_tl("display: flip %u: %u taken, %u not, longest wait %u us\n", fSwapsShown, fFlipFlips,
				    fFlipMissed, alioth_flip_latch_max_us());
			}
		} else if (scan != NULL && hakios_display_wait_copy(fCopyNs, scan->frame_ns * 3 / 2)) {
			uint64_t copy_start = mach_absolute_time(), copy_ns;
			shown = copy_start;
			memcpy(fBase, fComposeBase, (size_t)fRowBytes * fPanelHeight);
			__asm__ volatile("dsb oshst" ::: "memory");
			absolutetime_to_nanoseconds(mach_absolute_time() - copy_start, &copy_ns);
			fCopyNs = copy_ns > fCopyNs ? copy_ns : (fCopyNs * 31 + copy_ns) / 32;
			if (fSwapsShown < 5)
				hakios_tl("display: staged frame %u copied in %llu us\n", fSwapsShown, copy_ns / 1000);
		} else {
			/* Keep the previous complete picture if this frame misses its window. */
			fStatScanMissed++;
			fCopyNs = fCopyNs > 4000000 ? fCopyNs * 7 / 8 : fCopyNs;
			if (fSwapsShown < 5) hakios_tl("display: staged frame %u missed its idle window\n", fSwapsShown);
		}
	}
	drawn = mach_absolute_time();
	absolutetime_to_nanoseconds(drawn - began, &nanos);
	fDrawNs = fDrawNs == 0 ? nanos : (fDrawNs * 7 + nanos) / 8;
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		/* (and the surface, and of the scaling: each drawer's chunks of rows @ when it took its first, us) */
		hakios_tl("tl: v %u ready %llu shown %llu done %llu surface %u rows %u@%u %u@%u %u@%u %u@%u line %u %u\n",
		    swapWord(swap, SWAP_ID), micros(ready), micros(shown), micros(drawn), swapWord(swap, SWAP_SURFACE(0)), fJobChunks[0],
		    fJobStartUs[0], fJobChunks[1], fJobStartUs[1], fJobChunks[2], fJobStartUs[2], fJobChunks[3], fJobStartUs[3],
		    line_ready, alioth_panel_line());
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves; %u scanned out of the surface itself\n", fStatFrames,
		    nanos / NSEC_PER_MSEC, fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate, fStatDirect);
		fStatSwapsLate = 0;
		fStatDirect = 0;
		if (fStatEarly != 0 || fStatEarlyRisk != 0 || gSwapWaitTruth != 0) {
			uint32_t asked = __atomic_exchange_n(&gStatSwapWaitAsked, 0, __ATOMIC_RELAXED);
			uint32_t busy = __atomic_exchange_n(&gStatSwapWaitBusy, 0, __ATOMIC_RELAXED);

			hakios_tl("display: %u swaps over as their flip was issued (fb_swap_early=%u); %u frames came in the surface that "
			    "was on the panel then; SwapWait asked %u times, %u busy (fb_swap_wait=%u); surfaces %u %u %u %u %u\n",
			    fStatEarly, gSwapEarly, fStatEarlyRisk, asked, busy, gSwapWaitTruth, fStatSurfaces[0], fStatSurfaces[1],
			    fStatSurfaces[2], fStatSurfaces[3], fStatSurfaces[4]);
			fStatEarly = fStatEarlyRisk = 0;
		}
		if (fStatScanWaits != 0) {
			IOLog("MiniIOMobileFramebuffer: the scan (fb_scan): %u frames started where it let them land whole, %u did not "
			    "get there in time; %llu us to put a frame into the panel's memory\n", fStatScanWaits - fStatScanMissed,
			    fStatScanMissed, fDrawNs / NSEC_PER_USEC);
			fStatScanWaits = fStatScanMissed = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
	if (fComposeBase != NULL && !stagedReleased) completeSwap();
	if (direct == NULL) {
		swapDone(swapWord(swap, SWAP_ID));      /* (drawn here: its surfaces are read no more, fb_swap_wait) */
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static void backlightDisplayPower(bool on);     /* (the panel's brightness: AppleARMBacklight, below) */
static IOSimpleLock * gVSyncParkLock;           /* (the refreshes stopped while the display is off: vsyncPark(), below) */
static volatile bool gDisplayDark;

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;
	uint32_t                fFrameInfoMask;         /* the frame info tags asked for (kFrameInfoTags, by bit) */

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	void resumeVSync(void);
	void frameInfo(uint32_t swapID, uint64_t when, uint64_t delta);
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

/*
 * Frame info (IOMobileFramebufferFrameInfo, as IOMobileFramebuffer.framework asks for it): which tags there
 * are -- selector 74, one at a time by index: their number, and the name in 64 bytes --, which of them a
 * client wants -- selector 73: {3, a mask of their indexes} --, and for every frame put on the panel a
 * notification of type 3: {mask, swap ID, a 64-bit value for every tag in the mask, in index order}, which
 * the framework makes a dictionary of (FrameInfoNotifyFunc). CoreAnimation's render server asks for these
 * eight and takes Presentation_time for the phase of its frame timer (IOMFBServer::frame_info_callback ->
 * IOMFBDisplay::add_timing): without them that timer kept to no refresh of the panel's at all, and its
 * frames started anywhere in a TE period (2026-10-29). Times are mach absolute time, periods mach ticks.
 */
#define kFBNotifyFrameInfo      3
static const char * const kFrameInfoTags[] = {
	"Presentation_time", "Presentation_delta", "Requested_presentation", "Performance_headroom",
	"Thermal_throttle", "Min_FrameTime", "Max_FrameTime", "Vbl_FrameTime",
};
#define kFrameInfoTagCount      (sizeof(kFrameInfoTags) / sizeof(kFrameInfoTags[0]))
static IOLock * gFrameInfoLock;
static IOMobileFramebufferUserClient * gFrameInfoClient;        /* (retained) the client that asked last */
static uint64_t gFrameInfoLast;                                 /* (presenter thread) when the last frame was put up */

/*
 * Boot-arg fb_present_lead=US (default 0): frames are said to have gone up that much before they did. With frame
 * info CoreAnimation starts its frames 1-3 ms after one went up (its timer keeps to Presentation_time); told
 * earlier, it starts sooner, with that much more of the TE period left for drawing (an experiment).
 */
static TUNABLE(uint32_t, gPresentLead, "fb_present_lead", 0);

static void
frameInfoPresented(uint32_t swapID, uint64_t when)
{
	IOMobileFramebufferUserClient * client;
	uint64_t delta = gFrameInfoLast != 0 && when > gFrameInfoLast ? when - gFrameInfoLast : 0;

	if (gPresentLead != 0) {
		uint64_t lead;

		nanoseconds_to_absolutetime((uint64_t) gPresentLead * NSEC_PER_USEC, &lead);
		when = when > lead ? when - lead : when;
	}

	gFrameInfoLast = when;
	if (gFrameInfoLock == NULL) {
		return;
	}
	IOLockLock(gFrameInfoLock);
	client = gFrameInfoClient;
	if (client != NULL) {
		client->retain();
	}
	IOLockUnlock(gFrameInfoLock);
	if (client != NULL) {
		client->frameInfo(swapID, when, delta);
		client->release();
	}
}

void
IOMobileFramebufferUserClient::frameInfo(uint32_t swapID, uint64_t when, uint64_t delta)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[2 + kFrameInfoTagCount];
	uint32_t n = 0;
	uint64_t period;
	bool send = false;

	nanoseconds_to_absolutetime(refreshNs(), &period);
	IOLockLock(fLock);
	if (!fClosed && fFrameInfoMask != 0 && fCallbacks[kFBNotifyFrameInfo] != 0 && fPorts[kFBNotifyFrameInfo] != NULL) {
		setAsyncReference64(ref, fPorts[kFBNotifyFrameInfo], fCallbacks[kFBNotifyFrameInfo], fRefcons[kFBNotifyFrameInfo], fTask);
		args[n++] = fFrameInfoMask;
		args[n++] = swapID;
		for (uint32_t tag = 0; tag < kFrameInfoTagCount; tag++) {
			static const uint8_t kind[kFrameInfoTagCount] = { 0, 1, 0, 2, 2, 3, 3, 3 };   /* when, delta, 0, a period */

			if (fFrameInfoMask & (1u << tag)) {
				args[n++] = kind[tag] == 0 ? when : kind[tag] == 1 ? delta : kind[tag] == 3 ? period : 0;
			}
		}
		send = true;
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, n, kIOUserNotifyOptionCanDrop);
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	if (gVSyncParkLock == NULL) {
		IOSimpleLock * lock = IOSimpleLockAlloc();

		if (lock != NULL && !OSCompareAndSwapPtr(NULL, lock, &gVSyncParkLock)) {
			IOSimpleLockFree(lock);
		}
	}
	if (gFrameInfoLock == NULL) {
		IOLock * lock = IOLockAlloc();

		if (lock != NULL && !OSCompareAndSwapPtr(NULL, lock, &gFrameInfoLock)) {
			IOLockFree(lock);
		}
	}
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (gFrameInfoLock != NULL) {
		IOLockLock(gFrameInfoLock);
		if (gFrameInfoClient == this) {
			gFrameInfoClient = NULL;
			release();
		}
		IOLockUnlock(gFrameInfoLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(refreshNs(), &period);
	if (gTEAt != 0 && gTEAt <= now && period != 0) {
		/*
		 * The panel's TEs, as the last flip taken showed one: the next of them (gTENs) -- told early by
		 * fb_vsync_lead microseconds (default 3000). CoreAnimation starts a frame so as to have it done just
		 * before the refresh it is told of, by its own guess of what drawing takes; the GPU's part and the
		 * flip, which must be in before the TE, it does not count, and frames started 3.5-6 ms after a flip
		 * was taken missed the next TE by turns (2026-10-29).
		 */
		uint64_t lead = 0, base = gTEAt;

		nanoseconds_to_absolutetime((uint64_t) gVSyncLead * NSEC_PER_USEC, &lead);
		lead %= period;
		base = gTEAt > lead ? gTEAt - lead : gTEAt;
		fVSyncTime = base + ((now - base) / period + 1) * period;
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
	if (hakios_display_scan() != NULL) {
		/* fb_scan: when the panel's next frame begins, by its scan -- locked to it, never drifting */
		uint64_t ahead;

		nanoseconds_to_absolutetime(hakios_display_next_frame_ns(), &ahead);
		fVSyncTime = now + (ahead != 0 ? ahead : period);
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * With the display off its refreshes stop (boot-arg fb_vsync_dark=0: they go on, as before): told 120 times a second,
 * the window server woke as often for nothing on a dark screen. A client whose refresh comes due then is parked here
 * (retained) and started again the moment the display is back (backlightDisplayPower(): resumeVSync()), on the
 * panel's TE as armVSync() always places it.
 */
static TUNABLE(uint32_t, gVSyncDark, "fb_vsync_dark", 1);
#define VSYNC_PARKED    8
static IOMobileFramebufferUserClient * gVSyncParked[VSYNC_PARKED];
static uint32_t gVSyncParks, gVSyncResumes;

static bool
vsyncPark(IOMobileFramebufferUserClient * client)
{
	bool parked = false;

	if (gVSyncParkLock == NULL) {
		return false;
	}
	IOSimpleLockLock(gVSyncParkLock);
	/* (looked at again under the lock: a display come back since is not to find a client parked after its resume) */
	if (!gDisplayDark) {
		IOSimpleLockUnlock(gVSyncParkLock);
		return false;
	}
	for (unsigned i = 0; i < VSYNC_PARKED && !parked; i++) {
		if (gVSyncParked[i] == client) {
			parked = true;
		}
	}
	for (unsigned i = 0; i < VSYNC_PARKED && !parked; i++) {
		if (gVSyncParked[i] == NULL) {
			client->retain();
			gVSyncParked[i] = client;
			parked = true;
			gVSyncParks++;
		}
	}
	IOSimpleLockUnlock(gVSyncParkLock);
	return parked;
}

static void
vsyncResumeAll(void)
{
	IOMobileFramebufferUserClient * clients[VSYNC_PARKED];
	unsigned n = 0;

	if (gVSyncParkLock == NULL) {
		return;
	}
	IOSimpleLockLock(gVSyncParkLock);
	for (unsigned i = 0; i < VSYNC_PARKED; i++) {
		if (gVSyncParked[i] != NULL) {
			clients[n++] = gVSyncParked[i];
			gVSyncParked[i] = NULL;
		}
	}
	IOSimpleLockUnlock(gVSyncParkLock);
	for (unsigned i = 0; i < n; i++) {
		clients[i]->resumeVSync();
		clients[i]->release();
	}
	if (n != 0 && gVSyncResumes++ < 8) {
		hakios_tl("framebuffer: the display is back: the refreshes of %u client(s) start again (%u parked so far)\n", n,
		    gVSyncParks);
	}
}

void
IOMobileFramebufferUserClient::resumeVSync(void)
{
	IOLockLock(fLock);
	if (!fClosed && !fVSyncRunning && fCallbacks[kFBNotifyVSync] != 0 && fPorts[kFBNotifyVSync] != NULL) {
		fVSyncRunning = true;
		armVSync();
	}
	IOLockUnlock(fLock);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else if (gVSyncDark && gDisplayDark && vsyncPark(this)) {
		/* (the display off: no refresh, none armed; resumeVSync() when it is back) */
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		if (gVSyncLayout == 2) {
			uint64_t interval;

			nanoseconds_to_absolutetime(refreshNs(), &interval);
			args[0] = (uint32_t) fVSyncCount;
			args[1] = fVSyncCount >> 32;
			args[2] = (uint32_t) now;
			args[3] = now >> 32;
			args[4] = (uint32_t) interval;
			args[5] = interval >> 32;
		} else {
			args[0] = (uint32_t) now;
			args[1] = now >> 32;
			args[2] = (uint32_t) now;
			args[3] = now >> 32;
			args[4] = (uint32_t) fVSyncCount;
			args[5] = fVSyncCount >> 32;
		}
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

/*
 * The whole picture's colour, as a real iPhone's display hardware applies it (iOS 16.5's calls, read from the dyld
 * cache: work/research/color-matrix.md): the colour remap mode (51; backboardd for Classic and Smart Invert: 1
 * inverted, 3 grayscale, 4 both, 0 neither), Night Shift's matrix (55, CoreBrightness, index 9) and the colour
 * filters' / Reduce White Point's (69, Accessibility: 0x88 bytes, byte 0 "disabled", the 3x3 at 0x20 row by row, the
 * offsets after it at 0x68). iOS's numbers are s31.32; an all-zero matrix is no matrix. Put together, Night Shift
 * first, then the filter, then grayscale (Rec. 709) and invert (1 - x), into one matrix for DSPP_0's PCC
 * (alioth_dsi.c alioth_pcc_set, 1.0 = 0x8000); none of them: the PCC off.
 */
static IOSimpleLock * gColorLock;
static uint32_t gRemapMode;
static bool gNightShiftSet, gColorFilterSet;
static int64_t gNightShift[9], gColorFilter[9], gColorFilterPost[3];       /* s31.32, as iOS gave them */

static void
colorMultiply(int64_t r[3][3], const int64_t a[3][3], const int64_t b[3][3])   /* r = a b, in s15.16 */
{
	int64_t t[3][3];

	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			t[i][j] = (a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]) >> 16;
		}
	}
	memcpy(r, t, sizeof(t));
}

static int32_t
colorToPCC(int64_t v)                                   /* s15.16 -> the PCC's 1.0 = 0x8000, within its 18 bits */
{
	v >>= 1;
	return (int32_t) (v < -0x20000 ? -0x20000 : v > 0x1ffff ? 0x1ffff : v);
}

static int32_t
colorConstantToPCC(int64_t v)                           /* the constant: the same units in 16 bits, white 0x7fff */
{
	v >>= 1;
	return (int32_t) (v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v);
}

static void
colorApply(void)
{
	const int64_t one = 1 << 16;
	int64_t m[3][3] = { { one, 0, 0 }, { 0, one, 0 }, { 0, 0, one } }, s[3][3], c[3] = { 0, 0, 0 };
	uint32_t mode;
	bool night, filter, gray, invert, identity = true;
	int32_t pm[3][3], pc[3];

	IOSimpleLockLock(gColorLock);
	mode = gRemapMode;
	night = gNightShiftSet;
	filter = gColorFilterSet;
	if (night) {
		for (int i = 0; i < 9; i++) {
			s[i / 3][i % 3] = gNightShift[i] >> 16;
		}
		colorMultiply(m, s, m);
	}
	if (filter) {
		for (int i = 0; i < 9; i++) {
			s[i / 3][i % 3] = gColorFilter[i] >> 16;
		}
		colorMultiply(m, s, m);
		for (int i = 0; i < 3; i++) {
			c[i] = gColorFilterPost[i] >> 16;
		}
	}
	IOSimpleLockUnlock(gColorLock);
	gray = mode == 3 || mode == 4;
	invert = mode == 1 || mode == 4;
	if (gray) {
		static const int64_t w[3] = { 13933, 46871, 4732 };     /* 0.2126, 0.7152, 0.0722 in 1/65536 */
		int64_t y[3][3], cy = (w[0] * c[0] + w[1] * c[1] + w[2] * c[2]) >> 16;

		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				y[i][j] = w[j];
			}
			c[i] = cy;
		}
		colorMultiply(m, y, m);
	}
	if (invert) {
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				m[i][j] = -m[i][j];
			}
			c[i] = one - c[i];
		}
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			pm[i][j] = colorToPCC(m[i][j]);
			identity = identity && pm[i][j] == (i == j ? 0x8000 : 0);
		}
		pc[i] = colorConstantToPCC(c[i]);
		identity = identity && pc[i] == 0;
	}
	alioth_pcc_set(pm, pc, !identity);
	{
		/* (said when what is on changes: Night Shift's ramps send a matrix every frame, 250 of them in 2 s) */
		static uint32_t said = ~0u;
		uint32_t now = mode << 3 | (night ? 4 : 0) | (filter ? 2 : 0) | (identity ? 1 : 0);

		if (now != said) {
			said = now;
			hakios_tl("alioth colour: remap mode %u, night shift %s, filter %s -> %s (R from R G B %#x %#x %#x, constant %#x)\n",
			    mode, night ? "on" : "off", filter ? "on" : "off", identity ? "the PCC off" : "the PCC on",
			    (uint32_t) pm[0][0] & 0x3ffff, (uint32_t) pm[0][1] & 0x3ffff, (uint32_t) pm[0][2] & 0x3ffff,
			    (uint32_t) pc[0] & 0xffff);
		}
	}
}

/* One of the colour calls; true when it was one (its answer in `result`). */
static bool
colorCall(uint32_t selector, IOExternalMethodArguments * args, IOReturn * result)
{
	const uint64_t * in = args->scalarInput;
	bool any;

	if (gColorLock == NULL) {
		IOSimpleLock * lock = IOSimpleLockAlloc();

		if (!OSCompareAndSwapPtr(NULL, lock, (void * volatile *) &gColorLock)) {
			IOSimpleLockFree(lock);
		}
	}
	*result = kIOReturnSuccess;
	switch (selector) {
	case kFBMethodSetColorRemapMode:                /* in: the mode */
		if (args->scalarInputCount < 1) {
			*result = kIOReturnBadArgument;
			return true;
		}
		IOSimpleLockLock(gColorLock);
		gRemapMode = (uint32_t) in[0];
		IOSimpleLockUnlock(gColorLock);
		colorApply();
		return true;

	case kFBMethodGetColorRemapMode:                /* out: the mode */
		if (args->scalarOutputCount < 1) {
			*result = kIOReturnBadArgument;
			return true;
		}
		args->scalarOutput[0] = gRemapMode;
		args->scalarOutputCount = 1;
		return true;

	case kFBMethodSetMatrix:                        /* in: which (9: Night Shift's), 3x3 s31.32 */
		if (args->scalarInputCount < 1 || args->structureInput == NULL || args->structureInputSize < 72) {
			*result = kIOReturnBadArgument;
			return true;
		}
		if (in[0] != 9) {
			return true;                    /* (another stage: nothing iOS 16.5 was seen to use) */
		}
		IOSimpleLockLock(gColorLock);
		memcpy(gNightShift, args->structureInput, sizeof(gNightShift));
		any = false;
		for (int i = 0; i < 9; i++) {
			any = any || gNightShift[i] != 0;
		}
		gNightShiftSet = any;
		IOSimpleLockUnlock(gColorLock);
		colorApply();
		return true;

	case kFBMethodGetMatrix:                        /* in: which; out: its 3x3 */
		if (args->structureOutput != NULL && args->structureOutputSize != 0) {
			bzero(args->structureOutput, args->structureOutputSize);
			if (args->scalarInputCount >= 1 && in[0] == 9 && args->structureOutputSize >= 72) {
				IOSimpleLockLock(gColorLock);
				memcpy(args->structureOutput, gNightShift, sizeof(gNightShift));
				IOSimpleLockUnlock(gColorLock);
			}
		}
		return true;

	case kFBMethodSwapSetColorMatrix:               /* in: 0x88 bytes (above) */
		if (args->structureInput == NULL || args->structureInputSize < 0x80) {
			*result = kIOReturnBadArgument;
			return true;
		}
		{
			const uint8_t * b = (const uint8_t *) args->structureInput;

			IOSimpleLockLock(gColorLock);
			memcpy(gColorFilter, b + 0x20, sizeof(gColorFilter));
			memcpy(gColorFilterPost, b + 0x68, sizeof(gColorFilterPost));
			any = false;
			for (int i = 0; i < 9; i++) {
				any = any || gColorFilter[i] != 0;
			}
			gColorFilterSet = b[0] == 0 && any;
			IOSimpleLockUnlock(gColorLock);
		}
		colorApply();
		return true;

	default:
		return false;
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* {swap, flags, timeout}: is that swap done? (fb_swap_wait; else: yes, at once) */
	{
		bool busy = args->scalarInputCount > 0 && swapPending((uint32_t) in[0]);

		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu%s\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()),
			    busy ? " busy" : "");
		}
		__atomic_add_fetch(&gStatSwapWaitAsked, 1, __ATOMIC_RELAXED);
		if (busy) {
			__atomic_add_fetch(&gStatSwapWaitBusy, 1, __ATOMIC_RELAXED);
			return kIOReturnBusy;
		}
		return kIOReturnSuccess;
	}
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
		return kIOReturnSuccess;
	case kFBMethodRequestPowerChange:               /* in: the display's power, 0 off (iOSPlus10's reading) */
		if (args->scalarInputCount >= 1) {
			backlightDisplayPower(in[0] != 0);
		}
		return kIOReturnSuccess;

	case kFBMethodChangeFrameInfo:                  /* in: notification type (3), the mask of frame info tags wanted */
		if (args->scalarInputCount >= 2 && in[0] == kFBNotifyFrameInfo) {
			uint32_t mask = (uint32_t) in[1] & ((1u << kFrameInfoTagCount) - 1);

			IOLockLock(fLock);
			fFrameInfoMask = mask;
			IOLockUnlock(fLock);
			if (gFrameInfoLock != NULL) {
				IOMobileFramebufferUserClient * old = NULL;

				IOLockLock(gFrameInfoLock);
				if (mask != 0 && gFrameInfoClient != this) {
					old = gFrameInfoClient;
					retain();
					gFrameInfoClient = this;
				} else if (mask == 0 && gFrameInfoClient == this) {
					old = this;
					gFrameInfoClient = NULL;
				}
				IOLockUnlock(gFrameInfoLock);
				OSSafeReleaseNULL(old);
			}
			IOLog("MiniIOMobileFramebuffer: frame info: tags %#x asked for\n", mask);
		}
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: a tag's index; out: how many tags, and that one's name (64 bytes) */
		if (args->scalarOutputCount >= 1) {
			out[0] = kFrameInfoTagCount;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			uint64_t index = args->scalarInputCount >= 1 ? in[0] : 0;

			bzero(args->structureOutput, args->structureOutputSize);
			if (index < kFrameInfoTagCount && args->structureOutputSize > 0) {
				strlcpy((char *) args->structureOutput, kFrameInfoTags[index], args->structureOutputSize);
			}
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		{
			IOReturn result;

			if (colorCall(selector, args, &result)) {
				return result;
			}
		}
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * alioth: the panel's brightness for iOS. CoreBrightness (in backboardd) looks for a service that
 * conforms to AppleARMBacklight and has "backlight-control"; without "new-backlight-architecture" it
 * takes the legacy path and sets "brightness" (a device level within IODisplayParameters' min..max) and
 * "FadePeriod" (seconds, 16.16) through IORegistryEntrySetCFProperties -- as the OnePlus 10 Pro port
 * found and does (iOSPlus10, patch 0250, NegroniBacklight.cpp; its notes: never block in setProperties,
 * and never publish "digital-dimming-min-nits", which turns on per-frame dimming). The level is the
 * K11A panel's DBV, DCS 0x51 with two bytes high first (the phone's DT: bl-dcs big-endian, 1..0x7ff,
 * 0x133 at power-up), written through DSI0's command DMA (alioth_dsi.c) from a work loop of its own;
 * a fade is steps of 33 ms (a DCS write waits for the MDP's frame transfer to end: up to 20 ms at
 * 120 Hz). The display's power (the framebuffer's RequestPowerChange) takes the panel to 0 and back.
 * Boot-args: panel_bl=0 (none), panel_bl_init=N (the level set at start, iOS's scale; 1023),
 * panel_bl_curve=1 (iOS's level to the panel as it is, not squared).
 */
extern "C" int alioth_dsi_dcs_write(const uint8_t * buf, uint32_t len, bool lp, uint8_t force_type);
extern "C" bool hakios_board_is_alioth(void);

#define ALIOTH_BL_MAX           0x7ff
#define ALIOTH_BL_INIT          1023            /* (iOS's scale: half way, a quarter of the panel's light squared) */
#define ALIOTH_BL_STEP_MS       33
#define ALIOTH_BL_NITS_MAX      500             /* (the K11A's light at DBV 0x7ff, outside its high-brightness mode) */
#define ALIOTH_BL_FADE_MAX_MS   5000

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

	IOWorkLoop *            fWL;
	IOTimerEventSource *    fTimer;
	IOSimpleLock *          fState;         /* the requests below */
	uint32_t                fTarget, fFadeFrom, fLastLit;
	uint64_t                fFadeStart, fFadePeriod;
	bool                    fDisplayOn, fPowerEdge;
	uint32_t                fLogged;
	/* the work loop's */
	bool                    fKnown, fBctrl;
	uint32_t                fLevel, fChanges, fWrites;
	uint64_t                fLastLog;

	static void timerFired(OSObject * owner, IOTimerEventSource * sender);
	void step(void);
	void apply(uint32_t level, bool final);
	void publish(uint32_t level);

public:
	bool                    fCurve;

	bool startBacklight(uint32_t initial);
	IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void displayPower(bool on);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static AppleARMBacklight * gBacklight;

void
AppleARMBacklight::publish(uint32_t level)
{
	OSDictionary * b = OSDictionary::withCapacity(3), * p = OSDictionary::withCapacity(1);

	if (b != NULL && p != NULL) {
		OSNumber * n;

		/* (min 0: with 1 there, CoreBrightness sent nothing but "brightness -1") */
		n = OSNumber::withNumber(0ULL, 32); b->setObject("min", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(ALIOTH_BL_MAX, 32); b->setObject("max", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(level, 32); b->setObject("value", n); OSSafeReleaseNULL(n);
		p->setObject("brightness", b);
		setProperty("IODisplayParameters", p);
	}
	OSSafeReleaseNULL(b);
	OSSafeReleaseNULL(p);
}

bool
AppleARMBacklight::startBacklight(uint32_t initial)
{
	fState = IOSimpleLockAlloc();
	fWL = IOWorkLoop::workLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &AppleARMBacklight::timerFired);
	if (fState == NULL || fWL == NULL || fTimer == NULL || fWL->addEventSource(fTimer) != kIOReturnSuccess) {
		return false;
	}
	fDisplayOn = true;
	fTarget = fLastLit = fFadeFrom = initial;
	setProperty("backlight-control", true);
	/*
	 * The legacy path: CoreBrightness sends the slider's level ("brightness"). Its auto-brightness ramps, in nits,
	 * end in its own model there (every step "brightness 0.5", 2026-10-30): backboardd hands them to us itself, as
	 * "brightness-nits" (src/inject.c, ramp_to()). Boot-arg panel_bl_nits=1 publishes "new-backlight-architecture"
	 * instead, on which CoreBrightness would send nits for everything -- but then it waits for display hardware this
	 * phone has not ("Modules not ready") and the screen stopped drawing at the home screen (2026-10-30).
	 */
	uint32_t nits_path = 0;
	PE_parse_boot_argn("panel_bl_nits", &nits_path, sizeof(nits_path));
	if (nits_path) {
		setProperty("new-backlight-architecture", true);
	}
	publish(initial);
	/*
	 * What the panel gives in nits, for CoreBrightness (_DisplayCreateWithID reads this dictionary off the
	 * backlight, as floats): without it the panel's light is taken to be 0, 0.5 and 1 nit at the slider's bottom,
	 * middle and top, hardwareAccessibleMaxNits is 1, and every auto-brightness target came out 1 nit whatever the
	 * light -- under a torch at 4000 lux as in the dark (2026-10-30). The slider's level goes to the panel squared
	 * (step() below), and the DBV is about linear in light: 500 nits at the top, a quarter of that half way, the
	 * dimmest step about 2. The currents are only for CoreBrightness's power estimate (nits = mA x the factor).
	 */
	OSDictionary * cal = OSDictionary::withCapacity(10);
	if (cal != NULL) {
		static const struct { const char * key; uint32_t value; } values[] = {
			{ "lMaxPanel", ALIOTH_BL_NITS_MAX }, { "lMidPanel", ALIOTH_BL_NITS_MAX / 4 }, { "lMinPanel", 2 },
			{ "lMaxProduct", ALIOTH_BL_NITS_MAX }, { "lMidProduct", ALIOTH_BL_NITS_MAX / 4 }, { "lMinProduct", 2 },
			{ "current-for-max-backlight", 20 }, { "current-for-mid-backlight", 5 },
			{ "hardware-max-current-limit", 25 }, { "milliAmps2NitsScaleFactor", ALIOTH_BL_NITS_MAX / 20 },
		};
		/* (in 16.16: it divides what it reads by 65536 -- 500 plain was 0.0076 nits) */
		for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
			OSNumber * n = OSNumber::withNumber((unsigned long long) values[i].value << 16, 32);
			cal->setObject(values[i].key, n);
			OSSafeReleaseNULL(n);
		}
		setProperty("backlight-calibration-parameters", cal);
		OSSafeReleaseNULL(cal);
	}
	registerService();
	hakios_tl("backlight: AppleARMBacklight up: the panel's DBV 0..%u by DCS 0x51; %u to start with (iOS's scale)\n", ALIOTH_BL_MAX,
	    initial);
	/*
	 * Set now: CoreBrightness asks for no level of its own until the slider is moved ("brightness -1"), and
	 * the UEFI leaves the panel bright -- too bright in a dark room.
	 */
	fTimer->setTimeoutUS(1);
	return true;
}

/* (the work loop) final: the end of a fade */
void
AppleARMBacklight::apply(uint32_t level, bool final)
{
	if (fKnown && level == fLevel) {
		return;
	}
	if (!fBctrl) {
		/* WRITE_CTRL_DISPLAY: brightness control on (BCTRL), as the panel's own power-up has it */
		const uint8_t c53[2] = { 0x53, 0x20 };

		fBctrl = alioth_dsi_dcs_write(c53, 2, true, 0) == 0;
	}
	const uint8_t cmd[3] = { 0x51, (uint8_t)(level >> 8), (uint8_t)level };
	int rc = alioth_dsi_dcs_write(cmd, 3, true, 0);
	uint64_t now = mach_absolute_time(), second;

	fChanges++;
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	if (rc != 0 || !fKnown || fWrites++ < 8 || (final && now - fLastLog >= second / 4) || now - fLastLog >= second) {
		hakios_tl("backlight: panel DBV %u%s (%u writes since the last line)\n", level, rc == 0 ? "" : " FAILED", fChanges);
		fLastLog = now;
		fChanges = 0;
	}
	if (rc == 0) {
		fKnown = true;
		fLevel = level;
	}
}

void
AppleARMBacklight::step(void)
{
	uint64_t now = mach_absolute_time();
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(fState);
	bool on = fDisplayOn, edge = fPowerEdge;
	uint32_t to = fTarget, from = fFadeFrom, lit = fLastLit;
	uint64_t t = now - fFadeStart, period = fFadePeriod;

	fPowerEdge = false;
	IOSimpleLockUnlockEnableInterrupt(fState, is);

	if (!on) {
		if (edge) {
			apply(0, true);
			hakios_tl("backlight: the display off: panel DBV 0\n");
		}
		return;
	}
	uint32_t level = to;
	bool more = false;

	if (period != 0 && t < period) {
		level = (uint32_t)((int64_t)from + ((int64_t)to - (int64_t)from) * (int64_t)t / (int64_t)period);
		more = true;
	}
	if (edge) {
		level = to != 0 ? to : lit;     /* the level from before the display went off, until iOS sets one */
		more = false;
		hakios_tl("backlight: the display on: panel DBV %u\n", level);
	}
	/*
	 * iOS's level to the panel's: squared (boot-arg panel_bl_curve=1: as it is). The data volume is new at
	 * every boot, so iOS starts at its default, half way (1055): linear that was too bright in a dark
	 * room; squared it is a quarter of the panel's light, and the slider's lower half has finer steps.
	 */
	if (level != 0 && fCurve) {
		level = (uint32_t)((uint64_t)level * level / ALIOTH_BL_MAX);
	}
	level = level == 0 ? 1 : level;         /* (the display is on: never dark) */
	apply(level, !more);
	if (more) {
		fTimer->setTimeoutMS(ALIOTH_BL_STEP_MS);
	}
}

void
AppleARMBacklight::timerFired(OSObject * owner, IOTimerEventSource * sender)
{
	(void) sender;
	((AppleARMBacklight *) owner)->step();
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * d = OSDynamicCast(OSDictionary, properties);

	if (d == NULL) {
		return kIOReturnBadArgument;
	}
	OSNumber * b = OSDynamicCast(OSNumber, d->getObject("brightness"));
	OSNumber * f = OSDynamicCast(OSNumber, d->getObject("FadePeriod"));
	OSNumber * nits = OSDynamicCast(OSNumber, d->getObject("brightness-nits"));

	if (fLogged < 24) {
		char name[32];

		fLogged++;
		proc_selfname(name, sizeof(name));
		hakios_tl("backlight: %s sets %u keys: brightness %d FadePeriod %#x brightness-nits %#x\n", name, d->getCount(),
		    b ? (int) b->unsigned32BitValue() : -1, f ? f->unsigned32BitValue() : 0, nits ? nits->unsigned32BitValue() : 0);
	}
	uint32_t to;
	int64_t asked_nits = nits != NULL ? (int32_t) nits->unsigned32BitValue() : 0;

	if (asked_nits > 0) {
		/*
		 * nits in 16.16 (CoreBrightness's _SetBLDriverNitsValue): the DBV is about linear in light, the panel's
		 * 500 at the top (the calibration above); as a level of iOS's scale, which step() squares, its root
		 */
		uint64_t dbv = MIN((uint64_t) asked_nits * ALIOTH_BL_MAX / ((uint64_t) ALIOTH_BL_NITS_MAX << 16), (uint64_t) ALIOTH_BL_MAX);
		uint64_t root = 0;

		if (fCurve) {
			for (uint64_t bit = 1ULL << 22; bit != 0; bit >>= 1) {
				if ((root + bit) * (root + bit) <= dbv * ALIOTH_BL_MAX) {
					root += bit;
				}
			}
			dbv = root;
		}
		to = dbv == 0 ? 1 : (uint32_t) MIN(dbv, (uint64_t) ALIOTH_BL_MAX);
	} else if (b != NULL) {
		int32_t v = (int32_t) b->unsigned32BitValue();

		if (v < 0) {
			return kIOReturnSuccess;        /* ("brightness -1": no level asked for) */
		}
		/* (the slider's bottom is the dimmest light, not none: 0 is for the display going off) */
		to = v < 1 ? 1 : MIN((uint32_t) v, (uint32_t) ALIOTH_BL_MAX);
	} else {
		return kIOReturnSuccess;
	}
	uint64_t ms = f != NULL ? ((uint64_t) f->unsigned32BitValue() * 1000) >> 16 : 0, period, now = mach_absolute_time();

	nanoseconds_to_absolutetime(MIN(ms, (uint64_t) ALIOTH_BL_FADE_MAX_MS) * NSEC_PER_MSEC, &period);
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(fState);
	uint32_t cur = fTarget;

	if (fFadePeriod != 0 && now - fFadeStart < fFadePeriod) {
		cur = (uint32_t)((int64_t)fFadeFrom + ((int64_t)fTarget - (int64_t)fFadeFrom) * (int64_t)(now - fFadeStart) /
		    (int64_t)fFadePeriod);
	}
	fFadeFrom = cur;
	fTarget = to;
	fFadeStart = now;
	fFadePeriod = period;
	if (to != 0 && fDisplayOn) {
		fLastLit = to;
	}
	IOSimpleLockUnlockEnableInterrupt(fState, is);
	publish(to);
	fTimer->setTimeoutUS(1);
	return kIOReturnSuccess;
}

void
AppleARMBacklight::displayPower(bool on)
{
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(fState);
	bool changed = fDisplayOn != on;

	if (changed) {
		fDisplayOn = on;
		fPowerEdge = true;
	}
	IOSimpleLockUnlockEnableInterrupt(fState, is);
	if (changed) {
		fTimer->setTimeoutUS(1);
	}
}

static void
publishBacklight(IOService * provider)
{
	uint32_t on = 1, initial = ALIOTH_BL_INIT, curve = 2;

	PE_parse_boot_argn("panel_bl", &on, sizeof(on));
	PE_parse_boot_argn("panel_bl_init", &initial, sizeof(initial));
	PE_parse_boot_argn("panel_bl_curve", &curve, sizeof(curve));
	if (!hakios_board_is_alioth() || !on || gBacklight != NULL) {
		return;
	}
	AppleARMBacklight * bl = OSTypeAlloc(AppleARMBacklight);

	if (bl == NULL || !bl->init() || !bl->attach(provider)) {
		OSSafeReleaseNULL(bl);
		return;
	}
	bl->fCurve = curve != 1;
	if (!bl->startBacklight(MIN(initial, (uint32_t) ALIOTH_BL_MAX))) {
		hakios_tl("backlight: not set up\n");
		bl->detach(provider);
		bl->release();
		return;
	}
	gBacklight = bl;                /* (keeps the reference) */
}

static void
backlightDisplayPower(bool on)
{
	static uint32_t logged;

	if (logged++ < 16) {
		hakios_tl("backlight: the framebuffer's power change: %s\n", on ? "on" : "off");
	}
	/*
	 * (the panel awake before its brightness comes back, asleep after it has gone: bsd/dev/alioth_dsi.c; and the cores
	 * and the memory's bandwidth up before the panel takes frames again -- with the display off the memory's vote may be
	 * far under what scanning out at 120 Hz needs, ddr_off)
	 */
	if (on) {
		hakios_screen_power(true);
		hakios_gpu_screen(true);        /* (on a thread of its own: it wakes while the panel takes its 120 ms) */
		alioth_panel_power(true);
		/* (the refreshes again, those that stopped while it was dark: see vsyncPark()) */
		__atomic_store_n(&gDisplayDark, false, __ATOMIC_RELEASE);
		vsyncResumeAll();
	}
	if (gBacklight != NULL) {
		gBacklight->displayPower(on);
	}
	if (!on) {
		alioth_panel_power(false);
		__atomic_store_n(&gDisplayDark, true, __ATOMIC_RELEASE);
		/* (the cores and the memory slower while nobody looks: there is nothing else that lowers them) */
		hakios_screen_power(false);
		hakios_gpu_screen(false);       /* (asleep in two seconds, if it has done all it was given) */
	}
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
		publishBacklight(fb);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
