/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/thread.h>
#include <kern/timer_queue.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

/* ---- CPU idle states (11-22; psci_smp.h) ---------------------------------------------------------------- */

#define PSCI_CPU_SUSPEND_64     0xC4000001u

psci_idle_hook_t psci_idle_hook;                /* set by a platform driver; NULL: the notifier does nothing */

/* boot-arg cpu_suspend=MASK given and not 0: GenericARMPE registers psci_processor_idle() for every core */
bool
psci_idle_wanted(void)
{
	uint32_t mask = 0;

	return PE_parse_boot_argn("cpu_suspend", &mask, sizeof(mask)) && mask != 0;
}

/* The idle notifier (processor_idle_t): interrupts are off; the driver's hook decides everything. */
void
psci_processor_idle(void *cpu_id __unused, boolean_t enter, uint64_t *new_timeout_ticks __unused)
{
	psci_idle_hook_t hook = psci_idle_hook;

	if (hook != NULL) {
		hook(enter);
	}
}

/*
 * CPU_SUSPEND: a power-down state comes back at the reset vector -- LowResetVectorBase, which finds this core's
 * cpu_data and jumps to its cpu_reset_handler, resume_idle_cpu since cpu_machine_idle_init() -- and from there
 * through arm_init_idle_cpu() to cpu_idle_exit(TRUE): nothing of this call's frame is needed again. A return means
 * the core did not go down (an interrupt was pending, or the state was refused): its status.
 */
int64_t
psci_cpu_suspend(uint32_t power_state)
{
	return psci_call(PSCI_CPU_SUSPEND_64, power_state, ml_static_vtop((vm_offset_t)&LowResetVectorBase), 0);
}

/*
 * ---- The K40's cores in rail power collapse (SM8250, 11-22; work/research/psci-idle.md) ---------------------
 *
 * boot-arg cpu_suspend=MASK: the cores whose bit is set (physical index = MPIDR Aff1; never the boot core) power
 * down in idle -- PSCI power_state 0x40000004, "rail-pc" in the phone's qcom,lpm-levels (is-reset,
 * use-broadcast-timer) -- when their next timer is at least cpu_suspend_min microseconds away (10000; rail-pc's
 * own minimum residency is 3934 us silver, 4488 gold). The core's generic timer stops down there, so its deadline
 * goes to the QTimer's frame 0 (0x17c21000, "arm,armv7-timer-mem" in the phone's tree; Linux's broadcast timer
 * too), whose interrupt (SPI 8 physical / SPI 6 virtual) is routed to the boot core, which wakes the sleeper with
 * an IPI. The deadline is taken early by the state's exit latency (531 us silver, 1061 gold). A core that comes
 * back through reset gets its GIC CPU interface and its own timer again (psci_idle_hook(FALSE)).
 * Counters: kern.hakios.cpuidle (psci_idle_text()). Without the boot-arg none of this runs.
 */
#define IDLE_CORES              8
#define IDLE_STATE_RAIL_PC      0x40000004u
#define QTIMER_CTL_PHYS         0x17c20000ull           /* CNTCTLBase */
#define QTIMER_FRAME0_PHYS      0x17c21000ull           /* CNTBase of frame 0 */
#define QT_CNTNSAR              0x004
#define QT_CNTTIDR              0x008
#define QT_CNTACR0              0x040
#define QT_CNTPCT_LO            0x000
#define QT_CNTPCT_HI            0x004
#define QT_CNTVCT_LO            0x008
#define QT_CNTVCT_HI            0x00c
#define QT_CNTP_CVAL_LO         0x020
#define QT_CNTP_CVAL_HI         0x024
#define QT_CNTP_CTL             0x02c
#define QT_CNTV_CVAL_LO         0x030
#define QT_CNTV_CVAL_HI         0x034
#define QT_CNTV_CTL             0x03c
#define QT_CTL_ENABLE           1u
#define QT_CTL_IMASK            2u
#define QT_ACR_RPCT             (1u << 0)
#define QT_ACR_RVCT             (1u << 1)
#define QT_ACR_RWVT             (1u << 4)
#define QT_ACR_RWPT             (1u << 5)
#define QTIMER_INTID_PHYS       (32 + 8)
#define QTIMER_INTID_VIRT       (32 + 6)

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);

static struct {
	uint32_t        mask, boot_core, intid;
	bool            virt;                           /* frame 0's virtual timer (its physical one is not ours) */
	vm_offset_t     frame;
	uint64_t        offset;                         /* the frame's count - mach_absolute_time() */
	uint64_t        min_ticks, exit_ticks[IDLE_CORES];
	int             cpu_of[IDLE_CORES];             /* kernel cpu number of a physical core, -1 none */
	uint64_t        want[IDLE_CORES];               /* a sleeper's wake-up (absolute time), 0 none */
	uint64_t        pop[IDLE_CORES];                /* ... and the deadline it was for */
	volatile uint32_t down[IDLE_CORES];             /* set before CPU_SUSPEND; still set in hook(FALSE) = reset */
	uint64_t        entries[IDLE_CORES], not_entered[IDLE_CORES], resumed[IDLE_CORES], too_soon[IDLE_CORES];
	uint64_t        ipis[IDLE_CORES], late_sum_us[IDLE_CORES], late_max_us[IDLE_CORES];
	int64_t         last_status[IDLE_CORES];
	uint64_t        fired;
	const char      *why;                           /* why it is not running, if it is not */
} idle;
decl_simple_lock_data(static, idle_lock);

static inline uint32_t
qt_read(uint32_t reg)
{
	return *(volatile uint32_t *)(idle.frame + reg);
}

static inline void
qt_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(idle.frame + reg) = value;
}

static uint64_t
qt_count(void)
{
	uint32_t lo_reg = idle.virt ? QT_CNTVCT_LO : QT_CNTPCT_LO, hi, lo;

	do {
		hi = qt_read(lo_reg + 4);
		lo = qt_read(lo_reg);
	} while (hi != qt_read(lo_reg + 4));
	return (uint64_t)hi << 32 | lo;
}

/* the QTimer at the earliest sleeper's wake-up, or off (masked: a level interrupt quieted). idle_lock held. */
static void
qt_arm_locked(void)
{
	uint32_t ctl = idle.virt ? QT_CNTV_CTL : QT_CNTP_CTL, cval = idle.virt ? QT_CNTV_CVAL_LO : QT_CNTP_CVAL_LO;
	uint64_t next = UINT64_MAX;

	for (int i = 0; i < IDLE_CORES; i++) {
		if (idle.want[i] != 0 && idle.want[i] < next) {
			next = idle.want[i];
		}
	}
	qt_write(ctl, QT_CTL_IMASK);
	if (next != UINT64_MAX) {
		next += idle.offset;
		qt_write(cval, (uint32_t)next);
		qt_write(cval + 4, (uint32_t)(next >> 32));
		qt_write(ctl, QT_CTL_ENABLE);
	}
	__builtin_arm_dsb(DSB_SY);
}

/* the QTimer's interrupt, on the boot core: every sleeper whose wake-up has come gets an IPI */
static void
qt_fired(__unused void *context, __unused uint32_t intid)
{
	uint32_t wake = 0;
	uint64_t now;

	simple_lock(&idle_lock, LCK_GRP_NULL);
	now = mach_absolute_time();
	idle.fired++;
	for (int i = 0; i < IDLE_CORES; i++) {
		if (idle.want[i] != 0 && idle.want[i] <= now + idle.exit_ticks[i] / 8) {
			idle.want[i] = 0;
			idle.ipis[i]++;
			wake |= 1u << i;
		}
	}
	qt_arm_locked();
	simple_unlock(&idle_lock);
	for (int i = 0; i < IDLE_CORES; i++) {
		if ((wake & (1u << i)) && idle.cpu_of[i] >= 0) {
			(void) cpu_signal(cpu_datap(idle.cpu_of[i]), SIGPnop, NULL, NULL);
		}
	}
}

/* cpu_idle()'s notifier (psci_processor_idle()): interrupts are off */
static void
idle_hook(boolean_t enter)
{
	cpu_data_t *cdp = getCpuDatap();
	uint32_t core = (uint32_t)(cdp->cpu_phys_id >> 8) & 0xff;
	uint64_t now, pop;

	if (core >= IDLE_CORES) {
		return;
	}
	if (!enter) {
		if (idle.down[core] == 0) {
			return;
		}
		/* back through reset: the CPU interface and this core's own timer were lost */
		idle.down[core] = 0;
		gic_v3_irq_cpu_resume();
		now = mach_absolute_time();
		simple_lock(&idle_lock, LCK_GRP_NULL);
		idle.resumed[core]++;
		if (idle.pop[core] != 0 && now > idle.pop[core]) {
			uint64_t late_us;

			absolutetime_to_nanoseconds(now - idle.pop[core], &late_us);
			late_us /= NSEC_PER_USEC;
			idle.late_sum_us[core] += late_us;
			idle.late_max_us[core] = late_us > idle.late_max_us[core] ? late_us : idle.late_max_us[core];
		}
		idle.want[core] = idle.pop[core] = 0;
		qt_arm_locked();
		simple_unlock(&idle_lock);
		cdp->rtcPop = 0;                /* (timer_resync_deadlines sets the timer only if this differs) */
		timer_resync_deadlines();
		return;
	}
	if (!(idle.mask & (1u << core)) || core == idle.boot_core) {
		return;
	}
	now = mach_absolute_time();
	pop = cdp->rtcPop;
	if (pop != EndOfAllTime && pop < now + idle.min_ticks) {
		idle.too_soon[core]++;
		return;
	}
	simple_lock(&idle_lock, LCK_GRP_NULL);
	idle.pop[core] = pop == EndOfAllTime ? 0 : pop;
	idle.want[core] = pop == EndOfAllTime ? 0 : pop - idle.exit_ticks[core];
	idle.entries[core]++;
	qt_arm_locked();
	simple_unlock(&idle_lock);
	idle.down[core] = 1;
	__builtin_arm_dsb(DSB_SY);
	idle.last_status[core] = psci_cpu_suspend(IDLE_STATE_RAIL_PC);
	/* returned: it did not go down (an interrupt pending, or refused) */
	idle.down[core] = 0;
	simple_lock(&idle_lock, LCK_GRP_NULL);
	idle.not_entered[core]++;
	idle.want[core] = idle.pop[core] = 0;
	qt_arm_locked();
	simple_unlock(&idle_lock);
}

/* set-up, on a thread bound to the boot processor (the QTimer's SPI is routed to the core that attaches it) */
static void
idle_setup(__unused void *arg, __unused wait_result_t wr)
{
	vm_offset_t ctl;
	uint32_t acr, nsar, us = 10000;
	uint64_t a, b, c;

	thread_bind(master_processor);
	thread_block(THREAD_CONTINUE_NULL);
	ctl = ml_io_map(QTIMER_CTL_PHYS, PAGE_SIZE);
	idle.frame = ml_io_map(QTIMER_FRAME0_PHYS, PAGE_SIZE);
	if (ctl == 0 || idle.frame == 0) {
		idle.why = "the QTimer could not be mapped";
		goto out;
	}
	nsar = *(volatile uint32_t *)(ctl + QT_CNTNSAR);
	acr = *(volatile uint32_t *)(ctl + QT_CNTACR0);
	printf("psci idle: QTimer CNTNSAR 0x%x CNTTIDR 0x%x CNTACR0 0x%x\n", nsar,
	    *(volatile uint32_t *)(ctl + QT_CNTTIDR), acr);
	if ((acr & (QT_ACR_RPCT | QT_ACR_RWPT)) == (QT_ACR_RPCT | QT_ACR_RWPT)) {
		idle.virt = false;
		idle.intid = QTIMER_INTID_PHYS;
	} else if ((acr & (QT_ACR_RVCT | QT_ACR_RWVT)) == (QT_ACR_RVCT | QT_ACR_RWVT)) {
		idle.virt = true;
		idle.intid = QTIMER_INTID_VIRT;
	} else {
		idle.why = "frame 0's timers are not ours (CNTACR0)";
		goto out;
	}
	a = mach_absolute_time();
	c = qt_count();
	b = mach_absolute_time();
	idle.offset = c - (a + (b - a) / 2);
	PE_parse_boot_argn("cpu_suspend_min", &us, sizeof(us));
	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &idle.min_ticks);
	for (int i = 0; i < IDLE_CORES; i++) {
		idle.cpu_of[i] = -1;
		nanoseconds_to_absolutetime((i < 4 ? 531 : 1061) * NSEC_PER_USEC, &idle.exit_ticks[i]);
	}
	for (int n = 0; n <= ml_get_max_cpu_number(); n++) {
		cpu_data_t *cdp = cpu_datap(n);

		if (cdp != NULL && ((cdp->cpu_phys_id >> 8) & 0xff) < IDLE_CORES) {
			idle.cpu_of[(cdp->cpu_phys_id >> 8) & 0xff] = n;
		}
	}
	idle.boot_core = (uint32_t)(cpu_datap(master_cpu)->cpu_phys_id >> 8) & 0xff;
	simple_lock_init(&idle_lock, 0);
	simple_lock(&idle_lock, LCK_GRP_NULL);
	qt_arm_locked();                        /* (off) */
	simple_unlock(&idle_lock);
	if (gic_v3_spi_attach(idle.intid, false, qt_fired, NULL) != KERN_SUCCESS) {
		idle.why = "the QTimer's interrupt could not be attached";
		goto out;
	}
	os_atomic_thread_fence(release);
	psci_idle_hook = idle_hook;
	printf("psci idle: cores 0x%x power down in idle (rail-pc 0x%x) when their next timer is %u us away; boot core %u "
	    "never; QTimer frame 0 %s timer (interrupt %u), offset %lld\n", idle.mask & ~(1u << idle.boot_core),
	    IDLE_STATE_RAIL_PC, us, idle.boot_core, idle.virt ? "virtual" : "physical", idle.intid, (long long)idle.offset);
out:
	if (idle.why != NULL) {
		printf("psci idle: NOT started: %s\n", idle.why);
	}
	thread_bind(PROCESSOR_NULL);
	/* (returning ends the thread: Call_continuation goes on to thread_terminate) */
}

/* From the platform's start-up (hakios.cpufreq.c), in thread context: starts the set-up if cpu_suspend=MASK. */
void
psci_idle_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpu_suspend", &idle.mask, sizeof(idle.mask)) || idle.mask == 0) {
		return;
	}
	if (kernel_thread_start(idle_setup, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "psci:idle_setup");
		thread_deallocate(thread);
	}
}

/* kern.hakios.cpuidle */
size_t
psci_idle_text(char *buf, size_t size)
{
	size_t n = 0;

	n += snprintf(buf + n, size - n, "mask 0x%x boot core %u %s, QTimer fired %llu\n", idle.mask, idle.boot_core,
	    psci_idle_hook != NULL ? "running" : (idle.why ? idle.why : "off"), idle.fired);
	for (int i = 0; i < IDLE_CORES && n + 1 < size; i++) {
		if (idle.entries[i] == 0 && idle.too_soon[i] == 0) {
			continue;
		}
		n += snprintf(buf + n, size - n, "core %d: entries %llu not-entered %llu (last status %lld) resumed %llu "
		    "too-soon %llu ipis %llu late mean %llu max %llu us\n", i, idle.entries[i], idle.not_entered[i],
		    idle.last_status[i], idle.resumed[i], idle.too_soon[i], idle.ipis[i],
		    idle.resumed[i] ? idle.late_sum_us[i] / idle.resumed[i] : 0, idle.late_max_us[i]);
	}
	return n < size ? n : size - 1;
}

#endif /* defined(GENERIC_PSCI_SMP) */
