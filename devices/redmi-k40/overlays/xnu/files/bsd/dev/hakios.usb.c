/*
 * The Redmi K40's USB controller (SM8250 usb_1: a Synopsys DWC3 at 0x0a600000 behind the apps
 * SMMU as stream 0, its Qualcomm wrapper at 0x0a6f8800), for iOS.
 *
 *   usb=1   a look, reading only: the controller's clocks and power (GCC), the wrapper's VBUS
 *           override, the DWC3's identity, role, device state and event buffer, and what the
 *           apps SMMU does with stream 0 (which match, which context, bypass or translation).
 *           Safe while the loader serves its own USB link on another core (bootcpu=7 cpus=7):
 *           nothing is written.
 *   usb=2   the controller is the kernel's: a USB serial port (CDC ACM, 1209:316d, named "HakiOS
 *           K40" or usb_name=Your_Name with '_' for spaces, serial hakios-alioth) that carries the
 *           kernel's log (what the loader's log gets) to the computer, and /dev/hakios-usb, through
 *           which a program (userland usbsh) shares the port: the log flows until the computer
 *           sends something, which goes to the program, and the log stops until the program
 *           asks for it again (ioctl HAKIOS_USB_LOG) or lets the port go. Only when
 *           the controller is free: the kernel has all the cores (bootcpu=0), or the loader on
 *           its own core has stopped its link and lent the controller (it then adds usbfree=1);
 *           otherwise it is the loader's link and is left alone. At the run's end the controller
 *           is stopped (hakios_usb_quiesce()) and the loader restarts its own link
 *           (devices/redmi-k40/loader: alioth_usb_take_back()).
 *   usb=3   the same controller, but what an iPhone is to a computer: 05ac:12a8 "iPhone" by "Apple Inc.",
 *           its serial the UDID, and Apple's usbmux interface (class 0xff/0xfe/2: hakios.dwc3.c), which
 *           the computer's usbmuxd -- Apple Mobile Device Service on Windows, usbmuxd elsewhere -- talks to.
 *           Nothing shows until a program (userland usbmux, which speaks the device's side of usbmux and
 *           hands its connections to lockdownd and the rest on 127.0.0.1) opens /dev/hakios-usb and gives
 *           the UDID (ioctl HAKIOS_USB_IPHONE); from then on the port carries its packets only: read()
 *           gives what the computer sent as it came, each write() is one packet, one USB transfer.
 *
 * Addresses: Linux sm8250.dtsi (usb_1, usb_1_dwc3, apps_smmu), gcc-sm8250.c (USB30_PRIM
 * branches and GDSC), dwc3-qcom.c (QSCRATCH), drivers/usb/dwc3/core.h (the DWC3's own
 * registers); FFT-infra/QcomXnuUsbDebug soc/Sm8250.h has the same values for this phone.
 * The device side (EP0, CDC ACM, events, TRBs) is QcomXnuUsbDebug's shared core
 * (hakios.dwc3.c, from Asahi Linux m1n1). Its DMA: the firmware leaves stream 0 matched to
 * a context bank with translation off (usb=1 shows SMR5 -> CB5, SCTLR.M 0: the firmware's
 * own USB driver uses physical addresses), so the controller is given physical addresses of
 * memory in the kernel's image, and the SMMU is not touched.
 */
#include <stdarg.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/vnode.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>
#include <mach/mach_types.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.board.h>
#include "hakios.dwc3.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern kern_return_t thread_terminate(thread_t);
extern lck_mtx_t hakios_smmu_lock;
extern char loader_log_tee[];
extern volatile uint64_t loader_log_tee_total;
#define LOADER_LOG_TEE          (64 * 1024)     /* (osfmk/arm64/loader_return.c) */

#define GCC_BASE                0x00100000ull
#define GCC_USB30_PRIM_BCR      0xf000
#define GCC_USB30_PRIM_GDSCR    0xf004
#define DWC3_BASE               0x0a600000ull   /* the core's window; the wrapper (QSCRATCH) at +0xf8800 */
#define QSCRATCH_HS_PHY_CTRL    0xf8810
#define SMMU_BASE               0x15000000ull
#define USB_SID                 0x0u

/* drivers/usb/dwc3/core.h */
#define DWC3_GCTL               0xc110
#define DWC3_GSTS               0xc118
#define DWC3_GSNPSID            0xc120
#define DWC3_GHWPARAMS0         0xc140
#define DWC3_GUSB2PHYCFG0       0xc200
#define DWC3_GUSB3PIPECTL0      0xc2c0
#define DWC3_GEVNTADRLO0        0xc400
#define DWC3_GEVNTADRHI0        0xc404
#define DWC3_GEVNTSIZ0          0xc408
#define DWC3_GEVNTCOUNT0        0xc40c
#define DWC3_DCFG               0xc700
#define DWC3_DCTL               0xc704
#define DWC3_DEVTEN             0xc708
#define DWC3_DSTS               0xc70c
#define DWC3_DALEPENA           0xc720
#define DWC3_DEPCMDPAR2(n)      (0xc800 + 0x10 * (n))
#define DWC3_DEPCMD(n)          (0xc80c + 0x10 * (n))

extern void loader_log_putc(char c);

static uint32_t level;
static vm_offset_t gcc, dwc, smmu;

/*
 * Into the loader's log (what the host reads over the loader's link) as well as the console -- but not the
 * console at the run's end (hakios_usb_quiesce(), interrupts off): console output from there is a panic
 * ("Console I/O from interrupt-disabled context"), which then skips the rest of the way back.
 */
static volatile bool usb_leaving;
static void say(const char *format, ...) __printflike(1, 2);

/*
 * What was said, kept again in a ring of its own and said once more at the run's end (hakios_usb_quiesce()):
 * a long run's log keeps its first 256 KB and its last 1 MB, and the port's first seconds -- the computer
 * finding it -- fall in between (2026-10-30, usb=3). Each line here with the seconds since boot, and the mux's own
 * lines too (HAKIOS_USB_NOTE): a cable pulled minutes into a run fell in that gap, the mux's console lines with it,
 * and 8 KB without times held only the port's requests (2026-11-02).
 */
static char usb_history[128 * 1024];
static uint32_t usb_history_at;

static void
say(const char *format, ...)
{
	char line[400], when[24];
	va_list list;
	uint64_t ns;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	snprintf(when, sizeof(when), "[%5llu.%02llu] ", ns / NSEC_PER_SEC, ns % NSEC_PER_SEC / (NSEC_PER_SEC / 100));
	for (const char *c = when; *c != 0; c++) {
		usb_history[usb_history_at++ % sizeof(usb_history)] = *c;
	}
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
		usb_history[usb_history_at++ % sizeof(usb_history)] = *c;
	}
	if (!usb_leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	uint32_t v = *(volatile uint32_t *)(base + offset);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
look_power(void)
{
	static const struct { uint32_t offset; const char *name; } branches[] = {
		{ 0xf010, "usb30_prim_master" }, { 0xf018, "usb30_prim_sleep" }, { 0xf01c, "usb30_prim_mock_utmi" },
		{ 0xf054, "usb3_prim_phy_aux" }, { 0xf058, "usb3_prim_phy_com_aux" }, { 0xf05c, "usb3_prim_phy_pipe" },
		{ 0xf00c, "usb30_prim_master RCG cmd" }, { 0xf024, "usb30_prim_mock_utmi RCG cmd" },
	};
	uint32_t gdscr = rd(gcc, GCC_USB30_PRIM_GDSCR);

	say("usb: GCC USB30_PRIM GDSC %#x: %s%s; BCR %#x\n", gdscr, gdscr & (1u << 31) ? "powered" : "OFF",
	    gdscr & 1 ? " (software collapse asked)" : "", rd(gcc, GCC_USB30_PRIM_BCR));
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t v = rd(gcc, branches[i].offset);

		say("usb: GCC %s (%#x) = %#x%s\n", branches[i].name, branches[i].offset, v,
		    i < 6 ? (v & (1u << 31) ? ": clock OFF" : ": clock running") : "");
	}
}

/* Readable only with its power and the master/sleep clocks on (else an external abort): checked first. */
static bool
dwc_readable(void)
{
	return (rd(gcc, GCC_USB30_PRIM_GDSCR) & (1u << 31)) && !(rd(gcc, 0xf010) & (1u << 31)) && !(rd(gcc, 0xf018) & (1u << 31));
}

static void
look_controller(void)
{
	uint32_t gctl, dsts, dctl, evlo, evhi, evsiz, hs;

	if (!dwc_readable()) {
		say("usb: the controller's power or clocks are off: its registers are not read\n");
		return;
	}
	gctl = rd(dwc, DWC3_GCTL);
	dsts = rd(dwc, DWC3_DSTS);
	dctl = rd(dwc, DWC3_DCTL);
	evlo = rd(dwc, DWC3_GEVNTADRLO0);
	evhi = rd(dwc, DWC3_GEVNTADRHI0);
	evsiz = rd(dwc, DWC3_GEVNTSIZ0);
	hs = rd(dwc, QSCRATCH_HS_PHY_CTRL);
	say("usb: DWC3 GSNPSID %#x, GHWPARAMS0 %#x; GCTL %#x (port role %u: %s), GSTS %#x\n", rd(dwc, DWC3_GSNPSID),
	    rd(dwc, DWC3_GHWPARAMS0), gctl, (gctl >> 12) & 3,
	    ((gctl >> 12) & 3) == 1 ? "host" : ((gctl >> 12) & 3) == 2 ? "device" : ((gctl >> 12) & 3) == 3 ? "OTG" : "?", rd(dwc, DWC3_GSTS));
	say("usb: DCFG %#x (speed %u, address %u), DCTL %#x (%s%s), DEVTEN %#x, DSTS %#x (speed %u, link state %u%s), DALEPENA %#x\n",
	    rd(dwc, DWC3_DCFG), rd(dwc, DWC3_DCFG) & 7, (rd(dwc, DWC3_DCFG) >> 3) & 0x7f, dctl, dctl & (1u << 31) ? "run" : "stopped",
	    dctl & (1u << 30) ? ", soft reset" : "", rd(dwc, DWC3_DEVTEN), dsts, dsts & 7, (dsts >> 18) & 0xf,
	    dsts & (1u << 22) ? ", halted" : "", rd(dwc, DWC3_DALEPENA));
	say("usb: event buffer 0 at %#x:%08x, size %#x (interrupt %s), %u bytes of events pending\n", evhi, evlo, evsiz & 0xffff,
	    evsiz & (1u << 31) ? "masked" : "on", rd(dwc, DWC3_GEVNTCOUNT0) & 0xffff);
	say("usb: GUSB2PHYCFG %#x, GUSB3PIPECTL %#x, QSCRATCH HS_PHY_CTRL %#x (VBUS override %s)\n", rd(dwc, DWC3_GUSB2PHYCFG0),
	    rd(dwc, DWC3_GUSB3PIPECTL0), hs, (hs & ((1u << 20) | (1u << 28))) == ((1u << 20) | (1u << 28)) ? "on" : "off");
	for (unsigned ep = 0; ep < 6; ep++) {
		say("usb: endpoint %u%s: DEPCMD %#x, DEPCMDPAR2 %#x%s\n", ep / 2, ep & 1 ? " IN" : " OUT", rd(dwc, DWC3_DEPCMD(ep)),
		    rd(dwc, DWC3_DEPCMDPAR2(ep)), rd(dwc, DWC3_DALEPENA) & (1u << ep) ? " (enabled)" : "");
	}
}

/* The apps SMMU (SMMU-500, SMMUv2 programming model): which matches take stream 0, and where they send it. */
static void
look_smmu(void)
{
	uint32_t scr0 = rd(smmu, 0), id0 = rd(smmu, 0x20), id1 = rd(smmu, 0x24);
	unsigned page = (id1 >> 31) ? 0x10000 : 0x1000, pages = 2u << ((id1 >> 28) & 7);
	unsigned matches = id0 & 255, contexts = id1 & 255, found = 0;
	vm_offset_t gr1 = ml_io_map(SMMU_BASE + page, 0x1000);

	say("usb: SMMU sCR0 %#x (unmatched streams %s), IDR0 %#x (%u matches), IDR1 %#x (%u contexts)\n", scr0,
	    scr0 & (1u << 10) ? "FAULT" : "BYPASS", id0, matches, id1, contexts);
	lck_mtx_lock(&hakios_smmu_lock);
	for (unsigned i = 0; i < matches && i < 128; i++) {
		uint32_t smr = rd(smmu, 0x800 + 4 * i), s2cr = rd(smmu, 0xc00 + 4 * i);
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if (!(smr & (1u << 31)) || ((id ^ USB_SID) & ~mask)) {
			continue;
		}
		found++;
		say("usb: SMR%u %#x (id %#x mask %#x) takes stream 0: S2CR %#x, type %u (%s), context %u\n", i, smr, id, mask, s2cr,
		    (s2cr >> 16) & 3, ((s2cr >> 16) & 3) == 0 ? "translation" : ((s2cr >> 16) & 3) == 1 ? "BYPASS" : "FAULT", s2cr & 255);
		if (((s2cr >> 16) & 3) == 0 && (s2cr & 255) < contexts && gr1) {
			unsigned cb = s2cr & 255;
			vm_offset_t ctx = ml_io_map(SMMU_BASE + (uint64_t)(pages + cb) * page, 0x1000);

			say("usb:   CBAR%u %#x CBA2R %#x\n", cb, rd(gr1, 4 * cb), rd(gr1, 0x800 + 4 * cb));
			if (ctx) {
				say("usb:   CB%u SCTLR %#x TCR %#x TCR2 %#x TTBR0 %#x:%08x MAIR0 %#x FSR %#x FAR %#x:%08x\n", cb, rd(ctx, 0),
				    rd(ctx, 0x30), rd(ctx, 0x10), rd(ctx, 0x24), rd(ctx, 0x20), rd(ctx, 0x38), rd(ctx, 0x58), rd(ctx, 0x64),
				    rd(ctx, 0x60));
			}
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (found == 0) {
		say("usb: no SMMU match takes stream 0: the controller's DMA goes %s\n",
		    scr0 & (1u << 10) ? "NOWHERE (unmatched streams fault)" : "straight to physical addresses (bypass)");
	}
}

/* ---- usb=2: the controller is the kernel's ---------------------------------------------------- */

#define USB_VID                 0x1209u         /* pid.codes' test VID with the PID m1n1 and QcomXnuUsbDebug use */
#define USB_PID                 0x316du
#define POLL_US                 1000            /* the controller is polled (its interrupt stays masked) */
#define RX_RING                 65536           /* what the computer sent, kept for /dev/hakios-usb */
#define LOG_BACKLOG             (16 * 1024)     /* how much of the log a newly opened port gets first */

static uint8_t usb_dma[DWC3_DMA_BYTES] __attribute__((aligned(16384)));
static uint64_t usb_context[DWC3_CONTEXT_BYTES / sizeof(uint64_t)];
static struct dwc3 *usb_dev;
static uint64_t usb_dma_pa;
static LCK_GRP_DECLARE(usb_lck_grp, "hakios-usb");
static LCK_MTX_DECLARE(usb_lock, &usb_lck_grp);
static volatile bool usb_owned;         /* dwc3_init() done: the controller is ours until hakios_usb_quiesce() */
static volatile bool usb_stopping;
static uint64_t log_at;                 /* the next character of the log tee for the port */
static uint64_t log_skipped;            /* characters of the log that came faster than the port took them */
static uint32_t usb_failures;
/* /dev/hakios-usb */
static uint8_t rx_ring[RX_RING];
static uint32_t rx_head, rx_tail;       /* (under usb_lock; free-running counters) */
/*
 * Sessions of the port: one more at every bus reset (the cable out and in again, the computer resetting the port) and
 * at every restart of the controller. What the computer sent before is of a session that is over, and is dropped; the
 * program asks for the number (HAKIOS_USB_GENERATION) and starts its side again when it changes -- the usbmux did not,
 * kept half a packet of the old session in front of the new one's first, and the computer's usbmuxd, unanswered, let
 * the iPhone go: no Trust, no photos after the cable was pulled once (2026-11-01).
 */
static uint32_t usb_generation, seen_rx_generation;
static volatile UInt32 user_open;
static volatile bool log_mode = true;   /* the log goes to the port: until the computer sends something to the program that has it */
static struct selinfo usb_rsel, usb_wsel;
static int usb_wchan;
static char usb_name[32] = "HakiOS K40";        /* boot-arg usb_name=Your_Name: what the computer calls the port ('_' for a space) */

/* ioctl on /dev/hakios-usb: the log to the port again (until the computer next sends something) */
#define HAKIOS_USB_LOG          _IO('u', 1)
/* usb=3: show the computer an iPhone with this serial (the UDID) and its usbmux interface */
struct hakios_usb_iphone {
	char serial[48];
};
#define HAKIOS_USB_IPHONE       _IOW('u', 2, struct hakios_usb_iphone)
/* usb=3: the computer's usbmux has come (1) or gone (0) -- for AppleUSBDeviceMux's clients (iokit/Kernel/hakios.usbmux.cpp) */
#define HAKIOS_USB_HOST         _IOW('u', 3, int)
/* the port's session (see usb_generation) */
#define HAKIOS_USB_GENERATION   _IOR('u', 4, uint32_t)
/* usb=3: a line of the mux's own, into the loader's log and the port's history (see usb_history) */
struct hakios_usb_note {
	char text[160];
};
#define HAKIOS_USB_NOTE         _IOW('u', 5, struct hakios_usb_note)
extern int hakios_usbmux_publish(void);
extern void hakios_usbmux_host(bool connected);
static volatile bool usb_taken;         /* /dev/hakios-usb is there, the thread runs */
static volatile bool mux_asked;         /* usb=3: a program gave the serial: the device side may start */
static char mux_serial[48];
static volatile int mux_pid = -1;       /* ... that program, the phone's USB mux, while it has the port */

/*
 * The process that is this phone's USB mux (usb=3), or -1: bsd/netinet/tcp_usrreq.c tells lockdownd that a
 * connection of its is the kernel's (TCP_PEER_PID 0), as an iPhone's AppleUSBDeviceMux, in its kernel, is.
 */
int hakios_usb_mux_pid(void);

int
hakios_usb_mux_pid(void)
{
	return level == 3 ? mux_pid : -1;
}

static uint32_t
io_read(__unused void *cookie, uint32_t offset)
{
	return rd(dwc, offset);
}

static void
io_write(__unused void *cookie, uint32_t offset, uint32_t value)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(dwc + offset) = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* A busy wait: the shared core's waits are microseconds, and the run's end (other processors off) has no scheduler to lean on. */
static void
io_delay(__unused void *cookie, uint32_t usec)
{
	uint64_t span, start = mach_absolute_time();

	nanoseconds_to_absolutetime((uint64_t)usec * NSEC_PER_USEC, &span);
	while (mach_absolute_time() - start < span) {
		__asm__ volatile ("yield");
	}
}

/* The controller does not snoop the processors' caches: the event ring, TRBs and buffers are cleaned before it reads them and invalidated before we do. */
static void
io_sync(__unused void *cookie, void *cpu, size_t bytes, enum dwc3_sync_direction direction)
{
	uintptr_t p = (uintptr_t)cpu & ~(uintptr_t)63, end = (uintptr_t)cpu + bytes;

	for (; p < end; p += 64) {
		if (direction == DWC3_TO_DEVICE) {
			__asm__ volatile ("dc cvac, %0" :: "r" (p) : "memory");
		} else {
			__asm__ volatile ("dc civac, %0" :: "r" (p) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
io_barrier(__unused void *cookie)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static unsigned setups_said;

static void
io_trace(__unused void *cookie, enum dwc3_trace event, uint32_t a, uint32_t b)
{
	static const char *const devt[16] = { "disconnect", "USB reset", "connection done", "link state", "wake-up", "hibernation",
		                              "suspend", "start of frame", "?", "erratic error", "command complete", "event overflow",
		                              "?", "?", "?", "?" };

	static unsigned session_said, said_all;

	switch (event) {
	case DWC3_TRACE_DEVICE_EVENT:
		say("usb: event: %s (DSTS %#x)\n", devt[a & 15], b);
		if ((a & 15) == 1) {
			session_said = 0;       /* (a bus reset: the next session's first requests are said too) */
		}
		break;
	case DWC3_TRACE_STUCK:
		say("usb: the computer reset the port %u times %s: the device side from scratch\n", a,
		    (b & 0x100) ? "without ever configuring it" : "and got no request through");
		break;
	case DWC3_TRACE_SETUP:
		/* (40 a session: past the configuration, SET_CONFIGURATION being the 18th of a Windows enumeration) */
		if (setups_said++ < 24 || (session_said++ < 40 && said_all++ < 1500)) {
			say("usb: request %02x %02x value %#x index %#x length %u\n", a >> 24, (a >> 16) & 0xff, a & 0xffff, b >> 16, b & 0xffff);
		}
		break;
	case DWC3_TRACE_READY:
		say("usb: the computer has %s the port (configured %u)\n", b ? "OPENED" : "closed", a);
		break;
	case DWC3_TRACE_TIMEOUT:
		say("usb: TIMEOUT waiting on register %#x (%#x)\n", a, b);
		break;
	case DWC3_TRACE_COMMAND:
		say("usb: endpoint command %#x FAILED: %#x\n", a, b);
		break;
	case DWC3_TRACE_BAD_EVENT:
		say("usb: event %#x not understood (%u)\n", a, b);
		break;
	case DWC3_TRACE_UNSUPPORTED:
		if (setups_said < 24) {
			say("usb: request %02x %02x answered with a stall\n", a, b);
		}
		break;
	/* (the three below once a bus session each: the controller's count of resets, dwc3_rx_generation()) */
	case DWC3_TRACE_CONFIGURED: {
		static unsigned configured_said;

		if (configured_said++ < 200) {
			say("usb: bus session %u: the computer chose configuration %u%s\n", dwc3_rx_generation(usb_dev), a,
			    b ? " (iPhone mode)" : "");
		}
		break;
	}
	case DWC3_TRACE_ARMED: {
		static uint32_t armed_said = 0xffffffffu;

		if (armed_said != dwc3_rx_generation(usb_dev)) {
			armed_said = dwc3_rx_generation(usb_dev);
			say("usb: bus session %u: endpoint %u ready for what the computer sends (%u bytes in the ring)\n",
			    armed_said, a, b);
		}
		break;
	}
	case DWC3_TRACE_NOT_READY: {
		static uint32_t not_ready_said[2] = { 0xffffffffu, 0xffffffffu };
		uint32_t *said = &not_ready_said[a & 1];

		if (*said != dwc3_rx_generation(usb_dev)) {
			*said = dwc3_rx_generation(usb_dev);
			say("usb: bus session %u: the computer tried endpoint %u (%s) with nothing there: active %u, configured %u, "
			    "%u bytes in the ring\n", *said, a, (a & 1) ? "IN" : "OUT", b & 0xff, (b >> 8) & 0xff, b >> 16);
		}
		break;
	}
	default:
		break;
	}
}

/* The log into the port, while no program has it: what came since the last time, as much as the port's queue takes. */
static void
feed_log(void)
{
	uint64_t total = __atomic_load_n(&loader_log_tee_total, __ATOMIC_ACQUIRE);

	if (total - log_at > LOADER_LOG_TEE - 4096) {
		log_skipped += total - log_at - (LOADER_LOG_TEE - 4096);
		log_at = total - (LOADER_LOG_TEE - 4096);
	}
	if (!dwc3_ready(usb_dev)) {
		/* nobody reads the port: only a backlog's worth is kept for when someone does */
		if (total - log_at > LOG_BACKLOG) {
			log_at = total - LOG_BACKLOG;
		}
		return;
	}
	while (log_at < total) {
		uint32_t at = (uint32_t)(log_at % LOADER_LOG_TEE);
		uint32_t n = (uint32_t)MIN(total - log_at, (uint64_t)(LOADER_LOG_TEE - at));
		size_t taken = dwc3_queue(usb_dev, &loader_log_tee[at], n);

		log_at += taken;
		if (taken < n) {
			break;
		}
	}
}

/* The device side from scratch (the shared core's init and start), with the VBUS override; usb_lock held. */
static int
bring_up(void)
{
	struct dwc3_platform io = {
		.cookie = NULL, .read32 = io_read, .write32 = io_write, .delay_us = io_delay, .dma_sync = io_sync,
		.barrier = io_barrier, .trace = io_trace,
	};
	struct dwc3_identity identity = {
		.vid = USB_VID, .pid = USB_PID, .manufacturer = "HakiOS", .product = usb_name, .serial = "hakios-alioth",
	};
	if (level == 3) {
		/* an iPhone X (iPhone10,3: PID 0x12a8, as Apple's Windows driver lists it) */
		identity = (struct dwc3_identity){
			.vid = 0x05ac, .pid = 0x12a8, .manufacturer = "Apple Inc.", .product = "iPhone", .serial = mux_serial,
			.mux = 1, .bcd_device = 0x1006,
		};
	}
	int r = dwc3_init(usb_context, sizeof(usb_context), &io, &identity, usb_dma, usb_dma_pa, sizeof(usb_dma));

	if (r == 0) {
		/*
		 * VBUS as the controller sees it: with no OTG block telling it, the Qualcomm wrapper's override
		 * says the session is valid (dwc3-qcom.c dwc3_qcom_vbus_override_enable()). The firmware's link
		 * sets it, and its stop clears it (usb=1 then reads HS_PHY_CTRL 0): without it the device
		 * never pulls up and the computer sees nothing.
		 */
		uint32_t hs = rd(dwc, QSCRATCH_HS_PHY_CTRL), vbus = (1u << 20) | (1u << 28);

		if ((hs & vbus) != vbus) {
			io_write(NULL, QSCRATCH_HS_PHY_CTRL, hs | vbus);
			say("usb: the wrapper's VBUS override on: HS_PHY_CTRL %#x -> %#x\n", hs, rd(dwc, QSCRATCH_HS_PHY_CTRL));
		}
		usb_dev = (struct dwc3 *)(void *)usb_context;
		usb_owned = true;
		r = dwc3_start(usb_dev);
	}
	return r;
}

static void
usb_thread(__unused void *parameter, __unused wait_result_t result)
{
	int r;

	if (level == 3) {
		say("usb: usb=3: an iPhone to the computer once a program gives its UDID on /dev/hakios-usb\n");
		while (!mux_asked && !usb_stopping) {
			(void)tsleep((void *)&mux_asked, PZERO, "hakios-usb-mux", 100);       /* (ticks: a second or so) */
		}
		if (usb_stopping) {
			thread_terminate(current_thread());
			return;
		}
	}
	lck_mtx_lock(&usb_lock);
	r = bring_up();
	lck_mtx_unlock(&usb_lock);
	if (r != 0) {
		say("usb: the controller could not be made ours (%d): %s\n", r, usb_owned ? "left stopped" : "not touched past its stop");
		thread_terminate(current_thread());
		return;
	}
	if (level == 3) {
		say("usb: THE CONTROLLER IS THE KERNEL'S: an iPhone (05ac:12a8, serial %s) with Apple's usbmux interface; DMA at %#llx\n",
		    mux_serial, usb_dma_pa);
		log_mode = false;
		r = hakios_usbmux_publish();
		say("usb: AppleUSBDeviceMux for lockdownd: %s (%d)\n", r == 0 ? "published" : "NOT PUBLISHED", r);
	} else {
		say("usb: THE CONTROLLER IS THE KERNEL'S: a serial port (%04x:%04x \"%s\") from here on; its DMA at %#llx\n",
		    USB_VID, USB_PID, usb_name, usb_dma_pa);
	}
	log_at = loader_log_tee_total > LOG_BACKLOG ? loader_log_tee_total - LOG_BACKLOG : 0;
	/*
	 * Boot-arg usb_bounce=SECS (a test, usb_bounce_every=SECS, three times): the port leaves and comes back at that
	 * many seconds of uptime, as the computer sees a cable pulled and put back -- for what happens above the port
	 * then (the usbmux's new session) to be tried with nobody at the cable.
	 */
	uint32_t bounce_at = 0, bounce_every = 60, bounces = 0;
	PE_parse_boot_argn("usb_bounce", &bounce_at, sizeof(bounce_at));
	PE_parse_boot_argn("usb_bounce_every", &bounce_every, sizeof(bounce_every));
	while (!usb_stopping) {
		bool readable = false, writable, restarted = false, bounce = false;
		uint8_t buf[2048];
		size_t n;

		if (bounce_at != 0 && bounces < 3) {
			uint64_t ns;

			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			if (ns / NSEC_PER_SEC >= bounce_at + (uint64_t)bounces * bounce_every) {
				bounces++;
				bounce = true;
				say("usb: test (usb_bounce): the port leaves and comes back (%u of 3)\n", bounces);
			}
		}
		lck_mtx_lock(&usb_lock);
		if (usb_stopping) {
			lck_mtx_unlock(&usb_lock);
			break;
		}
		r = bounce ? -10 : dwc3_poll(usb_dev, 64);
		if (r < 0) {
			restarted = true;
			/*
			 * The device side from scratch: stopped, reset and started again (the computer sees the port
			 * leave and come back), not the shared core's dwc3_recover(), which reconfigures the endpoints
			 * but leaves its idea of what is armed as it was, and EP0 then never gets a SETUP again.
			 */
			(void)dwc3_stop(usb_dev);
			r = bring_up();
			if (++usb_failures <= 20) {
				say("usb: the controller failed, started again from scratch (%d; %u so far)\n", r, usb_failures);
			}
			if (r != 0) {
				usb_owned = false;
				lck_mtx_unlock(&usb_lock);
				say("usb: the controller does not start again: the port is gone until the run ends\n");
				break;
			}
		}
		if (restarted || dwc3_rx_generation(usb_dev) != seen_rx_generation) {
			static unsigned told;

			seen_rx_generation = dwc3_rx_generation(usb_dev);
			rx_head = rx_tail = 0;
			if (usb_generation++ != 0 && told++ < 200) {
				say("usb: a new session of the port (%u): what came before it is dropped\n", usb_generation);
			}
		}
		if (!user_open) {
			static uint32_t dropped_said;

			log_mode = level != 3;
			while ((n = dwc3_read(usb_dev, buf, sizeof(buf))) > 0) {
				/* what the computer sends with no program to take it is dropped */
				if (level == 3 && dropped_said != usb_generation) {
					dropped_said = usb_generation;
					say("usb: session %u: %zu bytes from the computer dropped: no program has the port\n", usb_generation, n);
				}
			}
		} else {
			/* usb=3: each session's first bytes for the mux (its version packet -- did it come at all?) */
			static uint32_t first_in_said;

			while (rx_head - rx_tail <= RX_RING - sizeof(buf) && (n = dwc3_read(usb_dev, buf, sizeof(buf))) > 0) {
				if (level == 3 && first_in_said != usb_generation) {
					char hex[3 * 16 + 1] = "";

					first_in_said = usb_generation;
					for (size_t i = 0; i < n && i < 16; i++) {
						snprintf(hex + 3 * i, sizeof(hex) - 3 * i, " %02x", buf[i]);
					}
					say("usb: session %u: the computer's first %zu bytes for the mux:%s\n", usb_generation, n, hex);
				}
				for (size_t i = 0; i < n; i++) {
					rx_ring[rx_head++ % RX_RING] = buf[i];
				}
				readable = true;
				/* the computer talks to the program: the log stops (the program can start it again) */
				log_mode = false;
			}
		}
		if (log_mode) {
			feed_log();
		} else {
			/* (what is not sent while the program has the port is not sent later either) */
			log_at = __atomic_load_n(&loader_log_tee_total, __ATOMIC_ACQUIRE);
		}
		writable = level == 3 ? dwc3_packet_room(usb_dev, DWC3_PACKET_MAX) : dwc3_queued(usb_dev) < DWC3_TX_BYTES / 2;
		/*
		 * Under the lock that usbdev_select() records the waiters under: selwakeup() of this XNU also tears the
		 * select queue down (waitq_deinit), and one that ran while the usbmux was in selrecord() met a link half
		 * made -- "waitq.c:1771 Assertion failed: waitq_sellinks.next == NULL", a panic the moment the cable was
		 * pulled, when the usbmux selects most often (2026-11-01 16:12).
		 */
		if (readable) {
			wakeup(rx_ring);
			selwakeup(&usb_rsel);
		}
		if (writable && user_open) {
			wakeup(&usb_wchan);
			selwakeup(&usb_wsel);
		}
		lck_mtx_unlock(&usb_lock);
		delay(POLL_US);
	}
	thread_terminate(current_thread());
}

static int
usbdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	/* (usb=3: before the device side is up too -- the program that opens it is what starts it) */
	if (!usb_owned && !(level == 3 && usb_taken && !usb_stopping)) {
		return ENXIO;
	}
	if (!OSCompareAndSwap(0, 1, &user_open)) {
		return EBUSY;
	}
	lck_mtx_lock(&usb_lock);
	rx_head = rx_tail = 0;
	lck_mtx_unlock(&usb_lock);
	say("usb: a program has the port (/dev/hakios-usb): the log goes on until the computer sends something\n");
	return 0;
}

static int
usbdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	user_open = 0;
	if (level == 3 && mux_pid >= 0) {
		hakios_usbmux_host(false);
	}
	mux_pid = -1;
	log_mode = level != 3;
	say("usb: the program let the port go%s\n", level == 3 ? "" : ": the log again");
	return 0;
}

static int
usbdev_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int error = 0;

	lck_mtx_lock(&usb_lock);
	while (rx_head == rx_tail) {
		if (usb_stopping || (!usb_owned && level != 3)) {
			lck_mtx_unlock(&usb_lock);
			return EIO;
		}
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&usb_lock);
			return EWOULDBLOCK;
		}
		error = msleep(rx_ring, &usb_lock, PZERO | PCATCH, "hakios-usb", NULL);
		if (error) {
			lck_mtx_unlock(&usb_lock);
			return error;
		}
	}
	while (rx_head != rx_tail && uio_resid(uio) > 0 && error == 0) {
		uint8_t buf[256];
		uint32_t n = 0;

		while (n < sizeof(buf) && rx_tail != rx_head && (user_ssize_t)n < uio_resid(uio)) {
			buf[n++] = rx_ring[rx_tail++ % RX_RING];
		}
		lck_mtx_unlock(&usb_lock);
		error = uiomove((caddr_t)buf, (int)n, uio);
		lck_mtx_lock(&usb_lock);
	}
	lck_mtx_unlock(&usb_lock);
	return error;
}

/* usb=3: the write is one packet, sent as one transfer (the computer's usbmux takes a transfer as a packet) */
static int
usbdev_write_packet(struct uio *uio, int ioflag)
{
	static uint8_t packet[DWC3_PACKET_MAX];         /* (under usb_write_lock) */
	static LCK_MTX_DECLARE(usb_write_lock, &usb_lck_grp);
	/* (said once a session: its first packet out, a packet the computer has not taken for 2 s) */
	static uint32_t first_out_said, wait_said;
	static unsigned reset_said;
	user_ssize_t n = uio_resid(uio);
	int error;
	unsigned waits = 0;

	uint32_t session;

	if (n <= 0 || n > (user_ssize_t)DWC3_PACKET_MAX) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&usb_write_lock);
	lck_mtx_lock(&usb_lock);
	session = usb_generation;
	lck_mtx_unlock(&usb_lock);
	error = uiomove((caddr_t)packet, (int)n, uio);
	while (error == 0) {
		bool sent;

		lck_mtx_lock(&usb_lock);
		if (!usb_owned || usb_stopping) {
			lck_mtx_unlock(&usb_lock);
			error = EIO;
			break;
		}
		/*
		 * A packet of a session that ended while it waited for room (the cable pulled with the queue full) is not
		 * sent into the next: it went out first there, half a TCP segment of the old session where the computer's
		 * usbmuxd waited for the answer to its version packet. The program learns of the new session from this.
		 */
		if (usb_generation != session) {
			uint32_t now = usb_generation;

			lck_mtx_unlock(&usb_lock);
			if (reset_said++ < 50) {
				say("usb: session %u: a packet of %lld bytes of session %u not sent (ENETRESET)\n", now, (long long)n, session);
			}
			error = ENETRESET;
			break;
		}
		sent = dwc3_send_packet(usb_dev, packet, (size_t)n) == 0;
		lck_mtx_unlock(&usb_lock);
		if (sent) {
			if (level == 3 && first_out_said != session) {
				first_out_said = session;
				say("usb: session %u: the mux's first packet to the computer, %lld bytes\n", session, (long long)n);
			}
			break;
		}
		/* (a tick a wait: 200 of them about 2 s) */
		if (++waits == 200 && wait_said != session) {
			wait_said = session;
			say("usb: session %u: a packet of %lld bytes has waited 2 s for the computer to take it\n", session, (long long)n);
		}
		if (ioflag & IO_NDELAY) {
			error = EWOULDBLOCK;
			break;
		}
		error = tsleep(&usb_wchan, PZERO | PCATCH, "hakios-usb-w", 1);
		error = error == EWOULDBLOCK ? 0 : error;
	}
	lck_mtx_unlock(&usb_write_lock);
	return error;
}

static int
usbdev_write(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int error = 0;

	if (level == 3) {
		return usbdev_write_packet(uio, ioflag);
	}
	while (uio_resid(uio) > 0 && error == 0) {
		uint8_t buf[512];
		size_t n = (size_t)MIN((user_ssize_t)sizeof(buf), uio_resid(uio)), done = 0;

		error = uiomove((caddr_t)buf, (int)n, uio);
		while (error == 0 && done < n) {
			lck_mtx_lock(&usb_lock);
			if (!usb_owned || usb_stopping) {
				error = EIO;
			} else {
				done += dwc3_queue(usb_dev, buf + done, n - done);
			}
			lck_mtx_unlock(&usb_lock);
			if (error == 0 && done < n) {
				/* the port's queue is full: wait for the computer to take some (a tick at most, then look again) */
				if (ioflag & IO_NDELAY) {
					return done > 0 ? 0 : EWOULDBLOCK;
				}
				error = tsleep(&usb_wchan, PZERO | PCATCH, "hakios-usb-w", 1);
				error = error == EWOULDBLOCK ? 0 : error;
			}
		}
	}
	return error;
}

static int
usbdev_select(__unused dev_t dev, int which, void *wql, struct proc *p)
{
	int ready = 0;

	lck_mtx_lock(&usb_lock);
	if (usb_stopping || (!usb_owned && level != 3)) {
		ready = 1;
	} else if (which == FREAD) {
		ready = rx_head != rx_tail;
		if (!ready) {
			selrecord(p, &usb_rsel, wql);
		}
	} else if (which == FWRITE) {
		ready = !usb_owned ? 0 : level == 3 ? dwc3_packet_room(usb_dev, DWC3_PACKET_MAX) :
		    dwc3_queued(usb_dev) < DWC3_TX_BYTES - 512;
		if (!ready) {
			selrecord(p, &usb_wsel, wql);
		}
	}
	lck_mtx_unlock(&usb_lock);
	return ready;
}

static int
usbdev_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		return 0;
	case HAKIOS_USB_LOG:
		lck_mtx_lock(&usb_lock);
		log_at = __atomic_load_n(&loader_log_tee_total, __ATOMIC_ACQUIRE);
		log_mode = true;
		lck_mtx_unlock(&usb_lock);
		return 0;
	case FIONREAD:
		lck_mtx_lock(&usb_lock);
		*(int *)data = (int)(rx_head - rx_tail);
		lck_mtx_unlock(&usb_lock);
		return 0;
	case HAKIOS_USB_GENERATION:
		lck_mtx_lock(&usb_lock);
		*(uint32_t *)(void *)data = usb_generation;
		lck_mtx_unlock(&usb_lock);
		return 0;
	case HAKIOS_USB_HOST: {
		/*
		 * Only the changes said, with how often the state before was said again: the mux says "come" at every
		 * connection to lockdownd, and 24 of a boot's first minute used up the 40 lines there were before the
		 * cable was ever pulled (2026-11-02).
		 */
		static int host_was = -1;
		static unsigned changes, again;
		int here = *(const int *)(const void *)data != 0;

		if (level != 3 || mux_pid != proc_selfpid()) {
			return level != 3 ? ENOTSUP : EPERM;
		}
		if (here != host_was) {
			if (changes++ < 400) {
				if (again != 0) {
					say("usb: session %u: the mux says the computer's usbmux has %s (\"%s\" said %u times more before)\n",
					    usb_generation, here ? "come" : "gone", here ? "gone" : "come", again);
				} else {
					say("usb: session %u: the mux says the computer's usbmux has %s\n", usb_generation, here ? "come" : "gone");
				}
			}
			host_was = here;
			again = 0;
		} else {
			again++;
		}
		hakios_usbmux_host(here);
		return 0;
	}
	case HAKIOS_USB_NOTE: {
		static unsigned notes;
		const struct hakios_usb_note *note = (const struct hakios_usb_note *)(const void *)data;

		if (level != 3 || mux_pid != proc_selfpid()) {
			return level != 3 ? ENOTSUP : EPERM;
		}
		if (notes++ < 3000) {
			say("usbmux: %.*s\n", (int)strnlen(note->text, sizeof(note->text)), note->text);
		}
		return 0;
	}
	case HAKIOS_USB_IPHONE: {
		const struct hakios_usb_iphone *ask = (const struct hakios_usb_iphone *)(void *)data;
		size_t len = strnlen(ask->serial, sizeof(ask->serial));

		if (level != 3) {
			return ENOTSUP;
		}
		if (mux_asked && len != 0 && len < sizeof(ask->serial) && strcmp(ask->serial, mux_serial) == 0) {
			mux_pid = proc_selfpid();       /* (the mux again, after it has ended and been started anew) */
			return EALREADY;
		}
		if (len == 0 || len >= sizeof(ask->serial) || mux_asked) {
			return mux_asked ? EALREADY : EINVAL;
		}
		for (size_t i = 0; i < len; i++) {
			if (ask->serial[i] < 0x21 || ask->serial[i] > 0x7e) {
				return EINVAL;
			}
		}
		strlcpy(mux_serial, ask->serial, sizeof(mux_serial));
		mux_pid = proc_selfpid();
		say("usb: the USB mux is process %d: its connections are the kernel's to lockdownd\n", mux_pid);
		mux_asked = true;
		wakeup((void *)&mux_asked);
		return 0;
	}
	default:
		return ENOTTY;
	}
}

static const struct cdevsw usb_cdevsw = {
	.d_open = usbdev_open, .d_close = usbdev_close, .d_read = usbdev_read, .d_write = usbdev_write,
	.d_ioctl = usbdev_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = usbdev_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The run's end (loader_return(), the other processors already off): the controller stopped. The lock is
 * not waited for (a processor that held it is gone); the shared core's stop ends the transfers and halts the
 * device side. Its event buffer is left where we put it, which is how the loader knows to restart its link.
 */
void hakios_usb_quiesce(void);

void
hakios_usb_quiesce(void)
{
	int r;

	usb_leaving = true;
	if (usb_history_at != 0) {
		uint32_t from = usb_history_at > sizeof(usb_history) ? usb_history_at - (uint32_t)sizeof(usb_history) : 0;
		static const char banner[] = "\n--- usb: what the port said during the run, again ---\n";

		for (const char *c = banner; *c != 0; c++) {
			loader_log_putc(*c);
		}
		for (uint32_t i = from; i < usb_history_at; i++) {
			loader_log_putc(usb_history[i % sizeof(usb_history)]);
		}
		usb_history_at = 0;
	}
	if (!usb_owned) {
		return;
	}
	usb_stopping = true;
	r = dwc3_stop(usb_dev);
	usb_owned = false;
	say("usb: the controller stopped%s; %llu characters of the log came faster than the port took them\n",
	    r == 0 ? "" : " (NOT CLEANLY)", log_skipped);
}

static void
take_controller(void)
{
	uint32_t bootcpu = 0, usbfree = 0;
	thread_t thread;
	int major;

	PE_parse_boot_argn("bootcpu", &bootcpu, sizeof(bootcpu));
	PE_parse_boot_argn("usbfree", &usbfree, sizeof(usbfree));
	if (bootcpu != 0 && usbfree == 0) {
		say("usb: the loader keeps a core of its own (bootcpu=%u) and serves its link on this controller: left to it\n", bootcpu);
		return;
	}
	if (bootcpu != 0) {
		say("usb: the loader on its own core has stopped its link and lent the controller for this run (usbfree=1)\n");
	}
	{
		char name[sizeof(usb_name)] = { 0 };
		bool good = PE_parse_boot_arg_str("usb_name", name, sizeof(name)) && name[0] != 0;

		/* printable ASCII only (a USB string descriptor here), '_' standing for the space a boot argument cannot hold */
		for (size_t i = 0; good && name[i] != 0; i++) {
			name[i] = name[i] == '_' ? ' ' : name[i];
			good = name[i] >= 0x20 && name[i] <= 0x7e;
		}
		if (good) {
			strlcpy(usb_name, name, sizeof(usb_name));
		}
	}
	if (!dwc_readable() || ((rd(dwc, DWC3_GCTL) >> 12) & 3) != 2) {
		say("usb: the controller is not powered, clocked and in device mode as the firmware leaves it: not taken\n");
		return;
	}
	usb_dma_pa = kvtophys((vm_offset_t)usb_dma);
	for (vm_offset_t off = PAGE_SIZE; off < sizeof(usb_dma); off += PAGE_SIZE) {
		if (kvtophys((vm_offset_t)usb_dma + off) != usb_dma_pa + off) {
			say("usb: the DMA memory is not contiguous: not taken\n");
			return;
		}
	}
	major = cdevsw_add(-1, &usb_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "hakios-usb") == NULL) {
		say("usb: /dev/hakios-usb cannot be made\n");
	}
	if (kernel_thread_start(usb_thread, NULL, &thread) != KERN_SUCCESS) {
		say("usb: no thread: not taken\n");
		return;
	}
	thread_deallocate(thread);
	usb_taken = true;
}

void hakios_usb_init(void);

void
hakios_usb_init(void)
{
	if (!PE_parse_boot_argn("usb", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!hakios_board_is_alioth()) {
		say("usb: these are the Redmi K40's addresses, and this is not a Redmi K40: nothing done\n");
		return;
	}
	gcc = ml_io_map(GCC_BASE, 0x10000 + 0xf000);
	dwc = ml_io_map(DWC3_BASE, 0x100000);
	smmu = ml_io_map(SMMU_BASE, 0x1000);
	if (gcc == 0 || dwc == 0 || smmu == 0) {
		say("usb: cannot map the registers\n");
		return;
	}
	say("usb: level %u: %s\n", level, level == 1 ? "a look, reading only" : "a look, then the controller for the kernel");
	look_power();
	look_controller();
	look_smmu();
	if (level >= 2) {
		take_controller();
	}
}
