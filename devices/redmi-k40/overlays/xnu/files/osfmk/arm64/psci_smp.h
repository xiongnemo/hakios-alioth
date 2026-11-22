/*
 * More than one processor on generic (non-Apple) ARMv8 boards: see psci_smp.c.
 */
#ifndef _ARM64_PSCI_SMP_H_
#define _ARM64_PSCI_SMP_H_

#include <mach/mach_types.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* How far a processor other than the boot processor has come (psci_smp_cpu_reached()). */
#define PSCI_SMP_CPU_ASKED      1       /* the firmware has agreed to start it */
#define PSCI_SMP_CPU_IN_KERNEL  2       /* it runs C code on the kernel's page tables (arm_init_cpu()) */
#define PSCI_SMP_CPU_RUNNING    3       /* it has joined the scheduler (psci_smp_cpu_running()) */
#define PSCI_SMP_CPU_OFF        4       /* it has switched itself off, or is about to */

extern void             psci_smp_init(void);
extern kern_return_t    psci_smp_start_cpu(int cpu);
extern void             psci_smp_cpu_reached(uint32_t stage);
extern void             psci_smp_cpu_running(void);
extern void             psci_smp_cpu_off(void) __dead2;

extern void             psci_smp_stop_others(void);
extern void             psci_smp_stop_here(void);

/*
 * CPU idle states (11-22). With boot-arg cpu_suspend=MASK (not 0) every core is registered with
 * psci_processor_idle() as its idle notifier: cpu_idle() calls it with interrupts off right before its WFI (enter
 * TRUE) and in cpu_idle_exit() (enter FALSE) -- also when the core came back through reset after a power-down,
 * which arm_init_idle_cpu() reaches without any saved registers. It calls psci_idle_hook, which a platform driver
 * sets (the K40's: hakios.cpufreq.c). Without the boot-arg nothing is registered: cpu_idle() as before.
 */
typedef void (*psci_idle_hook_t)(boolean_t enter);
extern psci_idle_hook_t psci_idle_hook;
extern bool             psci_idle_wanted(void);
extern void             psci_processor_idle(void *cpu_id, boolean_t enter, uint64_t *new_timeout_ticks);
/* PSCI CPU_SUSPEND (64-bit) with POWER_STATE, entering the kernel again at phys(LowResetVectorBase) through the
   core's cpu_reset_handler (resume_idle_cpu) when it was powered down; the status when it was not */
extern int64_t          psci_cpu_suspend(uint32_t power_state);
/* the K40's rail power collapse in idle (psci_smp.c): set-up started by the platform (boot-arg cpu_suspend=MASK),
   and its counters as text (kern.hakios.cpuidle) */
extern void             psci_idle_start(void);
extern size_t           psci_idle_text(char *buf, size_t size);

/* osfmk/arm64/gic_v3_irq.c */
extern void             gic_v3_irq_signal(uint32_t phys_id);
extern void             gic_v3_irq_signal_stop(uint32_t phys_id);
extern void             gic_v3_irq_quiesce(void);
extern void             gic_v3_irq_cpu_shutdown(void);
extern void             gic_v3_irq_cpu_resume(void);

__END_DECLS

#endif /* _ARM64_PSCI_SMP_H_ */
