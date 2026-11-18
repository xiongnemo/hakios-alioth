/*
 * HakiosGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/hakios.adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/hakios-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Not here yet: interrupts (waiting is asking again, twenty times a millisecond at first and less
 * often the longer it goes on: gpuWait()), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), recovery of a GPU that has stopped answering (it is reported, see
 * hakios_gpu_report(), and stays stopped), fences and buffers as file descriptors.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/hakios.adreno.c ("the GPU kept on"; declared in bsd/sys/hakios.input.h, which is not seen from here) */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void * start, uint64_t size);
void hakios_gpu_from_memory(void * start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
/* a client's own address space (boot-arg gpu_spaces=1; NULL: the shared one) */
void * hakios_gpu_space_new(void);
void hakios_gpu_space_free(void * space);
bool hakios_gpu_map_in(void * space, uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped_in(void * space);
void hakios_gpu_unmap_in(void * space, uint64_t address, uint64_t size);
uint32_t hakios_gpu_submit_in(void * space, const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
/* (11-19) the 3D core's interrupt, boot-arg gpu_irq=1: whether the waits may sleep until it, on what, and its counts */
bool hakios_gpu_irq_ready(void);
uint64_t hakios_gpu_irq_last(void);
int hakios_gpu_irq_wait(uint32_t fence, uint64_t deadline, lck_mtx_t * held);
void hakios_gpu_irq_counts(uint64_t * arrived, uint64_t * empty);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
void hakios_gpu_stop(void);
int hakios_gpu_sleep(void);
bool hakios_gpu_wake(void);
bool hakios_gpu_hung(void);
bool hakios_gpu_recover(void);
bool hakios_gpu_fence_failed(uint32_t fence);
bool hakios_gpu_space_faulted(void * space);
void hakios_gpu_test_hang(void);
extern volatile bool hakios_screen_off;         /* bsd/dev/hakios.cpufreq.c */

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void hakios_gpu_screen(bool on);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_BO_WRITE     0x0002
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        50              /* while waiting for the core: how long between two looks at how far it is
	                                                 * (200 put idle frames on two peaks 220 us apart: 0.1 ms on average
	                                                 * of every frame's way to the flip, work/research/gpu-120hz.md) */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hakios_gpu_...) */
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
	uint32_t invalidated;                   /* surfaces the GPU wrote, out of this processor's cache before they are shown */
	uint64_t invalidating;                  /* ... and the time that took */
	uint32_t pushes;                        /* (11-17) surfaces pushed out of the cache for a submission ... */
	uint64_t biggest_push;                  /* ... and the biggest of them, bytes */
	uint32_t irq_woken, irq_timed;          /* (11-19, gpu_irq=1) waits' sleeps ended by the interrupt / by the 2 ms */
	uint32_t irq_missed;                    /* ... of the latter, those that found the fence done (an interrupt lost) */
	uint32_t irq_soon, irq_soon_done;       /* (11-19) 0.2 ms looks after a wake without the fence, and those that found it */
	uint32_t push_n[4];                     /* (11-19) surfaces pushed for: the seed moved, mapped here, written, interval */
	uint64_t push_bytes[4];                 /* ... and their bytes */
	uint32_t gpuonly_skips;                 /* (11-19) interval pushes left out for surfaces said GPU-only (gpu_push_gpuonly) */
	uint64_t gpuonly_bytes;
	uint32_t wrapped;                       /* (11-19) surfaces around a program's memory made GPU buffers (gpu_wrap=1) */
	uint32_t scan_held, scan_gave_up;       /* (11-19, fb_swap_async) submissions held for the surface on the panel */
	uint64_t scan_holding;                  /* ... and for how long together (processor's clock) */
} gGPUStat;
static uint64_t gIrqSeen, gIrqEmptySeen;        /* (11-19) the interrupt's counts at the last 5-s line */
static TUNABLE(uint32_t, gInvalScan, "gpu_inval_scan", 0);
/*
 * (the K40's, 11-17) Test roads for the cache pass of a submission (submit()). It pushes a surface's every line out of
 * this processor's cache when its seed moved, when this program has the buffer mapped, when it was last pushed
 * gpu_push_ms ago or more -- and whenever the submission names it for WRITING. freedreno names every buffer for
 * reading and writing alike, so that last reason is every submission: 3-3.5 MB pushed a submission while Flutter
 * animates, 94-162 us each, 215-370 ms of the submitting threads' time every 5 s (rounds b12/b13), for surfaces only
 * the GPU writes -- a drawable, CoreAnimation's frames -- whose lines this processor never dirtied.
 * gpu_push_write=0: naming a surface for writing is no reason by itself. gpu_push_ms=N: the again-interval.
 */
static TUNABLE(uint32_t, gPushWrite, "gpu_push_write", 1);
static TUNABLE(uint32_t, gPushAgainMs, "gpu_push_ms", GPU_PUSH_AGAIN_MS);
/* (11-19) the interval rule left out for surfaces said GPU-only (MiniIOSurface.h gpu_only; submit() says why) */
static TUNABLE(uint32_t, gPushGPUOnly, "gpu_push_gpuonly", 0);
/* (11-19) a surface around a program's memory made a GPU buffer when its rows and start allow (surfaceBuffer()) */
static TUNABLE(uint32_t, gWrapImport, "gpu_wrap", 0);
/*
 * (11-19) ... and its own interval: census c6 (gpu_wrap=1) moved the compositor's cost from copying and hashing those
 * surfaces to pushing them -- 3-5 GB in 5 s in home flicks, Control Center and the app switcher on the 4 ms rule. They
 * are pictures (decoded images, the apps' snapshots) seldom drawn again; one drawn again without a seed shows that
 * much later at most. 0: gpu_push_ms's.
 */
static TUNABLE(uint32_t, gWrapAgainMs, "gpu_wrap_ms", 0);

/*
 * (11-19) fb_swap_async=1 (MiniIOMobileFramebuffer.cpp): CoreAnimation is back from SwapEnd before the GPU has drawn its
 * frame, and may have the next one drawn into the surface still on the panel (it draws into two or three in turn). So
 * while a newer frame is on its way there, the display's driver names the surface on the panel here (0: none), and a
 * submission that names it waits in submit() until that is over -- the newer frame's flip taken -- SCAN_GUARD_MS at most.
 * (freedreno names every buffer for reading and writing alike: a submission that only reads it waits too; none should:
 * a frame reads the one before it, not the one before that.)
 */
#define SCAN_GUARD_MS           20
static uint32_t gScanGuard;
static IOLock * gScanGuardLock;

extern "C" void
hakios_gpu_scan_guard(uint32_t sid)
{
	if (gScanGuardLock == NULL || __atomic_load_n(&gScanGuard, __ATOMIC_RELAXED) == sid) {
		return;
	}
	IOLockLock(gScanGuardLock);
	__atomic_store_n(&gScanGuard, sid, __ATOMIC_RELEASE);
	IOLockWakeup(gScanGuardLock, &gScanGuard, false);
	IOLockUnlock(gScanGuardLock);
}

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;
	bool soon = false;                      /* (11-19) the next sleep a short one: woken, and the fence not there yet */

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		/*
		 * (the K40's) Asked again every 50 us for the first millisecond -- where the display's own waits end, and
		 * where 200 us cost every frame 0.1 ms (GPU_ASK_AGAIN_US) -- then every 200 us to 4 ms, and every 500 us
		 * after. A client waiting for a whole frame of GPU work (an application's command buffer, 10-20 ms) woke
		 * this processor up to 20000 times a second: Flutter Check 7500 a second, "caught waking the CPU"
		 * (2026-11-10). A wait that long now learns of its end at most 0.5 ms late, 0.25 ms on average.
		 */
		uint64_t waited_ns;

		/*
		 * (11-19, boot-arg gpu_irq=1) Or asleep until the 3D core's interrupt says that a submission has ended
		 * (bsd/dev/hakios.adreno.c, gpu_irq_arrived()): no processor kept busy for it. At most 2 ms at a time, so
		 * that a lost interrupt costs that and no more, and never past the patience.
		 */
		if (hakios_gpu_irq_ready()) {
			uint64_t now = mach_absolute_time(), step, deadline, recent;

			/*
			 * (11-19) b117 / census c2: in Metal apps' windows most 2 ms looks found the fence done (270 of 334) --
			 * waits that BEGAN just after an interrupt, when the fence's word was not yet in memory: they slept, and
			 * no interrupt more came for it. (a27b630's look 0.2 ms after a WAKE found the fence 11 times in 53047:
			 * not that road.) So a wait that begins within 0.3 ms of an interrupt looks again 0.3 ms on.
			 */
			nanoseconds_to_absolutetime(300 * NSEC_PER_USEC, &recent);
			soon = now - hakios_gpu_irq_last() < recent;
			bool was_soon = soon;

			nanoseconds_to_absolutetime(soon ? 300 * NSEC_PER_USEC : 2 * NSEC_PER_MSEC, &step);
			deadline = MIN(now + step, began + patience);
			int woken = hakios_gpu_irq_wait(fence, deadline, IOLockGetMachLock(gGPULock));

			if (woken > 0) {
				gGPUStat.irq_woken++;
			} else if (woken == 0 && was_soon) {
				gGPUStat.irq_soon++;
				if (hakios_gpu_done(fence)) {
					gGPUStat.irq_soon_done++;       /* (the fence came after its interrupt: caught 0.2 ms on) */
				}
			} else if (woken == 0) {
				gGPUStat.irq_timed++;
				/* (a look that finds the fence done: the interrupt that should have said so did not come) */
				if (hakios_gpu_done(fence)) {
					gGPUStat.irq_missed++;
				}
			}
			continue;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &waited_ns);
		uint32_t again = waited_ns < 1000000 ? GPU_ASK_AGAIN_US : waited_ns < 4000000 ? 200 : 500;

		IOLockUnlock(gGPULock);
		delay_for_interval(again, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	if (milliseconds >= 1000 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("no answer");
	}
	return false;
}

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	bool done = true;

	if (gGPULock != NULL) {
		IOLockLock(gGPULock);
		uint32_t fence = surface->drawn;
		bool ready = hakios_gpu_ready();
		bool stable = fence == 0 || !ready;
		done = stable;
		/* Waiting drops gGPULock; if the surface was submitted again, wait for its fence too. */
		for (unsigned retry = 0; ready && fence != 0 && retry < 4; retry++) {
			done = gpuWait(fence, milliseconds);
			if (!done) break;
			uint32_t latest = surface->drawn;
			if (latest == fence) {
				stable = true;
				break;
			}
			fence = latest;
			if (fence == 0) {
				stable = true;
				break;
			}
		}
		if (done && ready && !stable) done = false;
		if (done && ready && stable && surface->gpu_written) {
			/*
			 * Not for a surface in a scan-out slot: that memory is write-combined wherever it is mapped (here and
			 * in every program, MiniIOSurface.cpp), so no processor's cache holds a line of it, and the 170,000
			 * lines of a display-sized one were gone through on the way of every frame to the panel.
			 * Boot-arg gpu_inval_scan=1: as before.
			 */
			if (surface->scanPhys == 0 || gInvalScan) {
				uint64_t began = mach_absolute_time();

				hakios_gpu_from_memory(surface->kpixels, surface->info.allocSize);
				gGPUStat.invalidating += mach_absolute_time() - began;
				gGPUStat.invalidated++;
			}
			surface->gpu_written = false;
		}
		IOLockUnlock(gGPULock);
	}
	return done;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left, end;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	/* (11-16, oosexgang 7822f30) the seconds are the caller's: a number too big to be nanoseconds is a time far
	 * ahead (or behind), not a wrapped one */
	if (__builtin_mul_overflow(when->seconds, (int64_t) NSEC_PER_SEC, &end) ||
	    __builtin_add_overflow(end, when->nanoseconds, &end)) {
		return when->seconds > 0 && when->nanoseconds >= 0 ? GPU_PATIENCE_MS : 0;
	}
	if (end <= 0) {
		return 0;
	}
	left = end - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	registerService();
	return true;
}

static thread_call_t gGPUSleepCall, gGPUWakeCall;      /* (the GPU asleep while the screen is off: hakios_gpu_screen()) */
static void gpuSleepWork(thread_call_param_t, thread_call_param_t);
static void gpuWakeWork(thread_call_param_t, thread_call_param_t);

/*
 * A hung GPU found and brought back (boot-arg gpu_recover=1; hakios.adreno.c hakios_gpu_recover()): a look every
 * quarter of a second while the screen is on, every two while it is off; after a recovery the waits are waited for
 * again (gGPUReported). Boot-arg gpu_test_hang=S: S seconds after the start, the core is given a wait that never ends.
 */
static thread_call_t gGPUWatchCall, gGPUTestHangCall;

static void
gpuWatchWork(thread_call_param_t, thread_call_param_t)
{
	uint64_t deadline;

	IOLockLock(gGPULock);
	if (hakios_gpu_hung() && hakios_gpu_recover()) {
		gGPUReported = false;
	}
	IOLockUnlock(gGPULock);
	clock_interval_to_deadline(hakios_screen_off ? 2000 : 250, kMillisecondScale, &deadline);
	thread_call_enter_delayed(gGPUWatchCall, deadline);
}

static void
gpuTestHangWork(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(gGPULock);
	hakios_gpu_test_hang();
	IOLockUnlock(gGPULock);
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	gScanGuardLock = IOLockAlloc();
	uint32_t sleepWanted = 0;
	if (gGPULock != NULL && PE_parse_boot_argn("gpu_sleep", &sleepWanted, sizeof(sleepWanted)) && sleepWanted != 0) {
		gGPUSleepCall = thread_call_allocate_with_options(gpuSleepWork, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		gGPUWakeCall = thread_call_allocate_with_options(gpuWakeWork, NULL, THREAD_CALL_PRIORITY_HIGH, 0);
		IOLog("HakiosGPU: the GPU sleeps while the screen is off (gpu_sleep=1)\n");
	}
	uint32_t recover = 0, testHang = 0;
	if (gGPULock != NULL && PE_parse_boot_argn("gpu_recover", &recover, sizeof(recover)) && recover != 0) {
		uint64_t deadline;

		gGPUWatchCall = thread_call_allocate_with_options(gpuWatchWork, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gGPUWatchCall, deadline);
		IOLog("HakiosGPU: a hung GPU is brought back (gpu_recover=1)\n");
		if (PE_parse_boot_argn("gpu_test_hang", &testHang, sizeof(testHang)) && testHang != 0) {
			gGPUTestHangCall = thread_call_allocate_with_options(gpuTestHangWork, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
			clock_interval_to_deadline(testHang, kSecondScale, &deadline);
			thread_call_enter_delayed(gGPUTestHangCall, deadline);
		}
	}
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
}

/* The run is ending (hakios.adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

/*
 * The GPU asleep while the screen is off (boot-arg gpu_sleep=1; hakios.adreno.c gpu_sleep_core()): two seconds
 * after the display went off, if the 3D core has done all it was given (else once a second after that); awake when
 * the display comes back -- early, the panel itself takes 120 ms -- or at the first submission meanwhile
 * (hakios_gpu_submit(), which wakes it under gGPULock). Called by MiniIOMobileFramebuffer's display power.
 */
static void
gpuSleepWork(thread_call_param_t, thread_call_param_t)
{
	uint64_t deadline;
	int slept;

	IOLockLock(gGPULock);
	slept = hakios_gpu_sleep();
	IOLockUnlock(gGPULock);
	if (slept == 0) {
		clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gGPUSleepCall, deadline);
	}
}

static void
gpuWakeWork(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(gGPULock);
	(void) hakios_gpu_wake();
	IOLockUnlock(gGPULock);
}

void
hakios_gpu_screen(bool on)
{
	uint64_t deadline;

	if (gGPUSleepCall == NULL || gGPUWakeCall == NULL) {
		return;                 /* (no GPU kept, or no gpu_sleep=1) */
	}
	if (on) {
		thread_call_cancel(gGPUSleepCall);
		thread_call_enter(gGPUWakeCall);
	} else {
		clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gGPUSleepCall, deadline);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */
/* boot-arg gpu_test_nomem=S (a test): from second S of the run on, 20 s of buffers refused as if there were no room */
static TUNABLE(uint32_t, gGPUTestNoMem, "gpu_test_nomem", 0);
extern "C" void hakios_data_range(uint32_t *size_mb, uint32_t *used_mb, uint32_t *hole_mb);

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;
	void *                  fSpace;                 /* its own GPU address space; NULL (gpu_spaces=0): the shared one */

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	IOLockLock(gGPULock);
	fSpace = hakios_gpu_space_new();
	IOLockUnlock(gGPULock);
	return fBuffers != NULL;
}

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * slot)
{
	/*
	 * (11-16, oosexgang 7822f30) gpuWait() below lets go of the lock: the buffer is taken out of its handle FIRST and
	 * worked on as a copy, so that a second close of the same handle (or the client's closing) meanwhile cannot free the
	 * same memory and address twice -- nor find the surface's buffer for an import and get a handle being freed.
	 */
	struct gpu_buffer saved = *slot;
	struct gpu_buffer * b = &saved;

	slot->memory = NULL;
	slot->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;

	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			if (b->surface == NULL) {
				b->memory->retain();
			} else if (b->surface->client != NULL && b->surface->scanPhys == 0) {
				/* (11-19, gpu_wrap=1: the GPU reads a program's memory, held in place by the surface: the surface is kept
				 * for good, so that the memory under it stays held while the GPU may still read it) */
				b->surface = NULL;
			} else {
				b->surface->memory->retain();
			}
			if (said++ < 8) {
				IOLog("HakiosGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && hakios_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap_in(fSpace, b->address, b->size);
			hakios_gpu_address_free(b->address, b->size);
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
HakiosGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hakios_gpu_ready() && !hakios_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap_in(fSpace, left->address, left->size);
			hakios_gpu_address_free(left->address, left->size);
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (hakios_gpu_ready()) {
			gpuWait(hakios_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("HakiosGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		} else if (fSpace != NULL) {
			hakios_gpu_space_free(fSpace);          /* (the GPU is done with it: waited for above) */
		}
		fSpace = NULL;
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		hakios_tl("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_gpu_map_in(fSpace, b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped_in(fSpace);
	if (!mapped) {
		hakios_gpu_unmap_in(fSpace, b->address, size);
		hakios_gpu_address_free(b->address, size);
		hakios_tl("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_unmap_in(fSpace, k->address, k->size);
		hakios_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	if (gGPUTestNoMem != 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(began, &ns);
		if (ns / NSEC_PER_SEC >= gGPUTestNoMem && ns / NSEC_PER_SEC < gGPUTestNoMem + 20ull) {
			hakios_tl("HakiosGPU: gpu_test_nomem: a buffer of %llu bytes refused, as if there were no room for it\n", size);
			return kIOReturnNoMemory;
		}
	}
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		/*
		 * No room in the kernel's range for big data buffers (not no memory: 2026-11-03 00:01 it was refused with 3 GB
		 * free). This client's kept buffers are in that range too: they are let go, and it is tried once more.
		 */
		static uint32_t refused;
		uint32_t range = 0, taken = 0, hole = 0;
		uint64_t kept;

		IOLockLock(gGPULock);
		kept = fKeptBytes;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		IOLockUnlock(gGPULock);
		if (kept != 0) {
			memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
			    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
		}
		if (refused < 8 || refused % 100 == 0) {
			hakios_data_range(&range, &taken, &hole);
			hakios_tl("HakiosGPU: no room for a buffer of %llu bytes (%u buffers, %llu MB with this client): %s %llu MB kept "
			    "were let go; the kernel's range for big data buffers %u MB, %u taken, the biggest hole %u MB\n", size,
			    fBufferCount, fBufferBytes >> 20, memory != NULL ? "made once this client's" : "refused although its",
			    kept >> 20, range, taken, hole);
		}
		refused++;
		if (memory == NULL) {
			return kIOReturnNoMemory;
		}
	}
	if (gGPUCached) {
		hakios_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHakiosDescribe), no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	/*
	 * (11-19, boot-arg gpu_wrap=1) ... unless it begins on a 4 KB boundary (the GPU's pages: this kernel's are 16 KB,
	 * so a 16 KB page's 4, 8 and 12 KB marks are boundaries too) and its rows are a multiple of 64 bytes (what freedreno
	 * takes): census c4 found 13 of the 22 refused so -- the home screen's 87x87 icons, and the 1326 x 1160 / 1326 x 1326
	 * pictures of the app switcher, which the compositor copied and hashed every frame. The GPU then reads the program's
	 * memory in place (held by the surface for its life, prepare()d where it was made, MiniIOSurface.cpp), read-only
	 * if that is how it was held; submit()'s cache pass pushes it like any other surface (the seed, the interval).
	 */
	if (s->client != NULL && s->scanPhys == 0 && gWrapImport && (s->clientAddress & 4095) == 0 &&
	    s->info.bytesPerRow % 64 == 0 && s->info.planeCount == 0) {
		uint64_t wsize = ((uint64_t) s->info.allocSize + 4095) & ~4095ULL;
		uint32_t wflags = s->client->getDirection() == kIODirectionOut ? MSM_BO_GPU_READONLY : 0;

		request->size = wsize;
		request->offset = s->info.offset;
		request->rowBytes = s->info.bytesPerRow;
		request->width = s->info.width;
		request->height = s->info.height;
		request->pixelFormat = s->info.pixelFormat;
		request->planes = s->info.planeCount;
		if (request->handle == kHakiosDescribe) {
			request->handle = 0;
			MiniIOSurfaceDrop(s);
			return kIOReturnSuccess;
		}
		IOLockLock(gGPULock);
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
				b = &fBuffers[i];
				request->handle = i + 1;
				break;
			}
		}
		if (b != NULL) {
			MiniIOSurfaceDrop(s);
		} else if ((memory = IOSubMemoryDescriptor::withSubRange(s->client, 0, s->info.allocSize,
		    s->client->getDirection())) != NULL && (b = newBuffer(s->client, 0, wsize, wflags, &request->handle)) != NULL) {
			b->memory = memory;
			b->kernel = s->kpixels;
			b->surface = s;
			gGPUStat.surfaces++;
			gGPUStat.wrapped++;
		}
		gGPUStat.taking += mach_absolute_time() - began;
		IOLockUnlock(gGPULock);
		if (b == NULL) {
			OSSafeReleaseNULL(memory);
			MiniIOSurfaceDrop(s);
			return kIOReturnNoMemory;
		}
		return kIOReturnSuccess;
	}
	if (s->client != NULL && s->scanPhys == 0) {
		/*
		 * (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes)
		 * (11-19) said, the first 16 and then one every 30 s: the compositor copies and hashes every such surface with
		 * the processor (eagl.c surface_to_texture / surface_changed: 18% of CA's display thread in the app switcher,
		 * 13% in home flicks, census c1) -- how many of them the GPU could take in place, rows of a multiple of 64 bytes
		 * from wherever in a page they begin, is what this is for.
		 */
		static uint32_t told;
		static uint64_t told_at;
		uint64_t now = mach_absolute_time(), thirty;

		nanoseconds_to_absolutetime(30 * NSEC_PER_SEC, &thirty);
		if (told < 16 || now - told_at >= thirty) {
			told++;
			told_at = now;
			IOLog("HakiosGPU: surface %u (%u x %u, format %#x) wraps a program's memory, %u bytes from a page's start, "
			    "%u bytes a row (%s 64; %u for its pixels): no GPU buffer of it (%u asked so far)\n", s->sid,
			    s->info.width, s->info.height, s->info.pixelFormat, (uint32_t) (s->clientAddress & PAGE_MASK),
			    s->info.bytesPerRow, s->info.bytesPerRow % 64 == 0 ? "a multiple of" : "NOT a multiple of",
			    s->info.width * 4, told);
		}
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	/* (a scan-out surface's pixels are a piece of the display's memory of their own, from its start: MiniIOSurface.cpp) */
	IOMemoryDescriptor * pixels = s->scanPhys != 0 ? s->client : s->memory;
	uint64_t from = s->scanPhys != 0 ? 0 : PAGE_SIZE;
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(pixels, from, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(pixels, from, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kpixels;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	if (hakios_gpu_space_faulted(fSpace)) {
		/* (this client's work hung the GPU, which was brought back without it: hakios_gpu_recover()) */
		IOLockUnlock(gGPULock);
		ret = kIOReturnNotResponding;
		goto out;
	}
	if (__atomic_load_n(&gScanGuard, __ATOMIC_ACQUIRE) != 0) {
		/* (fb_swap_async) not into the surface on the panel while a newer frame is on its way: see gScanGuard */
		uint32_t guard = __atomic_load_n(&gScanGuard, __ATOMIC_ACQUIRE);
		bool names = false;

		for (uint32_t i = 0; guard != 0 && !names && i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			names = b != NULL && b->surface != NULL && b->surface->sid == guard;
		}
		if (names) {
			uint64_t held = mach_absolute_time();
			AbsoluteTime until;
			bool gaveUp = false;

			IOLockUnlock(gGPULock);
			clock_interval_to_deadline(SCAN_GUARD_MS, kMillisecondScale, &until);
			IOLockLock(gScanGuardLock);
			while (__atomic_load_n(&gScanGuard, __ATOMIC_ACQUIRE) == guard) {
				if (IOLockSleepDeadline(gScanGuardLock, &gScanGuard, until, THREAD_UNINT) == THREAD_TIMED_OUT) {
					gaveUp = __atomic_load_n(&gScanGuard, __ATOMIC_ACQUIRE) == guard;
					break;
				}
			}
			IOLockUnlock(gScanGuardLock);
			IOLockLock(gGPULock);
			gGPUStat.scan_held++;
			gGPUStat.scan_gave_up += gaveUp;
			gGPUStat.scan_holding += mach_absolute_time() - held;
			if (hakios_gpu_space_faulted(fSpace)) {
				IOLockUnlock(gGPULock);
				ret = kIOReturnNotResponding;
				goto out;
			}
		}
	}
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime((uint64_t) gPushAgainMs * NSEC_PER_MSEC, &again);
	uint64_t wrapAgain;                     /* (no initializer: the gotos to `out` above jump past it) */

	wrapAgain = 0;
	if (gWrapAgainMs != 0) {
		nanoseconds_to_absolutetime((uint64_t) gWrapAgainMs * NSEC_PER_MSEC, &wrapAgain);
	}
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL && b->surface->scanPhys != 0) {
			/*
			 * A surface in a scan-out slot (CoreAnimation's frames, the display's engine scans them out
			 * as they are) is write-combined wherever it is mapped (MiniIOSurface addRef()): nothing of
			 * it is ever in this processor's cache, and pushing its 11 MB out line by line on every
			 * submission was the most of the render server's time in the kernel (prof=1).
			 */
			continue;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;
			/* (11-19) a surface around a program's memory, a GPU buffer since gpu_wrap=1: gpu_wrap_ms's interval */
			uint64_t again_here = b->surface->client != NULL && wrapAgain != 0 ? wrapAgain : again;

			if (seed == b->seed && !b->seen && began - b->pushed < again_here &&
			    (!gPushWrite || !(bos[i].flags & MSM_SUBMIT_BO_WRITE))) {
				continue;
			}
			/*
			 * (11-19, boot-arg gpu_push_gpuonly=1) A surface said to be touched by GPUs alone (MiniIOSurface.h gpu_only: a
			 * CAMetalLayer drawable, said by libhakios.metal) has no writer that keeps quiet: the interval is for those,
			 * and is not its reason. Its seed moving and this program mapping it still are.
			 */
			if (gPushGPUOnly && b->surface->gpu_only && seed == b->seed && !b->seen &&
			    (!gPushWrite || !(bos[i].flags & MSM_SUBMIT_BO_WRITE))) {
				gGPUStat.gpuonly_skips++;
				gGPUStat.gpuonly_bytes += b->size;
				continue;
			}
			/* (11-19) why, the first reason that holds: the seed moved, mapped by this program, the interval, named for
			 * writing -- for the 5-s line, to know which rule the pushes that are left come from */
			unsigned why = seed != b->seed ? 0 : b->seen ? 1 : began - b->pushed >= again_here ? 3 : 2;

			gGPUStat.push_n[why]++;
			gGPUStat.push_bytes[why] += b->size;
			b->seed = seed;
			b->pushed = began;
			gGPUStat.pushes++;
			if (b->size > gGPUStat.biggest_push) {
				gGPUStat.biggest_push = b->size;
			}
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		hakios_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = hakios_gpu_submit_in(fSpace, addresses, words, count);
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
				if (bos[i].flags & MSM_SUBMIT_BO_WRITE) {
					b->surface->gpu_written = true;
				}
			}
		}
	}
	reap();
	if (request->fence == 0 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("the ring is full");
	}
	if (fSubmits++ < 4) {
		/* (addresses and words are only set up to `count`: with no command buffer, the first would be stack garbage) */
		IOLog("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    count != 0 ? addresses[0] : 0ull, count != 0 ? words[0] : 0u);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		uint64_t invalidating;

		absolutetime_to_nanoseconds(gGPUStat.invalidating, &invalidating);
		IOLog("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept); %u surfaces out of the "
		    "cache before they were shown, in %llu us each; %u surfaces pushed for submissions, the biggest %llu KB\n",
		    gGPUStat.made, gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again,
		    fKeptBytes >> 20, gGPUStat.invalidated,
		    gGPUStat.invalidated ? invalidating / NSEC_PER_USEC / gGPUStat.invalidated : 0, gGPUStat.pushes,
		    gGPUStat.biggest_push >> 10);
		/* (11-19) which rule pushed them: the seed moved / mapped by the program / gpu_push_ms's interval / named for writing */
		IOLog("HakiosGPU: surfaces pushed for: the seed moved %u (%llu MB), mapped by the program %u (%llu MB), the %u ms "
		    "interval %u (%llu MB), named for writing %u (%llu MB); interval pushes left out, GPU-only: %u (%llu MB); "
		    "surfaces around a program's memory taken as buffers: %u\n",
		    gGPUStat.push_n[0], gGPUStat.push_bytes[0] >> 20, gGPUStat.push_n[1], gGPUStat.push_bytes[1] >> 20, gPushAgainMs,
		    gGPUStat.push_n[3], gGPUStat.push_bytes[3] >> 20, gGPUStat.push_n[2], gGPUStat.push_bytes[2] >> 20,
		    gGPUStat.gpuonly_skips, gGPUStat.gpuonly_bytes >> 20, gGPUStat.wrapped);
		if (gGPUStat.scan_held != 0) {
			uint64_t holding;

			absolutetime_to_nanoseconds(gGPUStat.scan_holding, &holding);
			IOLog("HakiosGPU: %u submissions waited for the surface on the panel to be replaced (fb_swap_async), %llu us "
			    "each; %u gave up after %u ms\n", gGPUStat.scan_held, holding / NSEC_PER_USEC / gGPUStat.scan_held,
			    gGPUStat.scan_gave_up, SCAN_GUARD_MS);
		}
		IOLog("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		uint64_t irqs, empty;

		hakios_gpu_irq_counts(&irqs, &empty);
		if (irqs != gIrqSeen || gGPUStat.irq_woken != 0 || gGPUStat.irq_timed != 0) {
			IOLog("HakiosGPU: the 3D core's interrupt: %llu arrived (%llu without a submission's end); the waits slept %u times "
			    "until it and %u times until their 2 ms look (%u of those found the fence done: interrupts lost); %u looks "
			    "0.3 ms on, for waits begun just after an interrupt, %u of them found the fence%s\n",
			    irqs - gIrqSeen, empty - gIrqEmptySeen, gGPUStat.irq_woken, gGPUStat.irq_timed, gGPUStat.irq_missed,
			    gGPUStat.irq_soon, gGPUStat.irq_soon_done,
			    hakios_gpu_irq_ready() ? "" : "; NOW LOOKING AS BEFORE (the line was let go of)");
			gIrqSeen = irqs;
			gIrqEmptySeen = empty;
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	/* (11-16, oosexgang 7822f30) rowBytes is the caller's own number: no more than a buffer may be */
	if (size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_show show;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow) {
		static uint32_t unready;

		if (unready++ < 8) {
			hakios_tl("HakiosGPU: request %#x while the GPU is not ready: refused\n", selector);
		}
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hakios_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hakios_gpu_last());
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		/* (a fence not yet given out is never reached: refused, not waited for -- the wait would end in a report) */
		if ((int32_t)(r.waitFence.fence - hakios_gpu_last()) > 0) {
			IOLockUnlock(gGPULock);
			ret = kIOReturnBadArgument;
			break;
		}
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		} else if (hakios_gpu_fence_failed(r.waitFence.fence)) {
			ret = kIOReturnNotResponding;   /* (lost when the hung GPU was brought back) */
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret != kIOReturnSuccess && selector != kMsmGemCpuPrep && selector != kMsmWaitFence) {
		static uint32_t refusals;

		/* (a refusal freedreno does not expect is a crash in the program: backboardd's fd_bo_ref(NULL), 2026-10-30) */
		if (refusals++ < 32) {
			char name[32];

			proc_selfname(name, sizeof(name));
			hakios_tl("HakiosGPU: %s: request %#x refused (%#x)%s%llu bytes, flags %#x; %u buffers, %llu MB\n", name, selector, ret,
			    selector == kMsmGemNew ? ": a new buffer of " : ": ", selector == kMsmGemNew ? (unsigned long long) r.gemNew.size : 0ULL,
			    selector == kMsmGemNew ? r.gemNew.flags : 0, fBufferCount, fBufferBytes >> 20);
		}
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
