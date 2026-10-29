/*
 * A sampling profiler for one process (Redmi K40 overlay): boot-arg prof=1 (prof_name=NAME, default
 * backboardd, CoreAnimation's render server; prof_after=S seconds before it starts, default 40;
 * prof_secs=S how long, default 30; prof_us=US between samples, default 250).
 *
 * Every prof_us a cross-call interrupts all the processors; on each, if the thread it interrupted belongs
 * to the process, the place is kept: the user PC (and, interrupted in the kernel -- a system call --, the
 * user PC the call was made from, marked, and the kernel PC). At the end, into the log: how many samples,
 * by thread, and the places most often seen, as raw addresses for the host to resolve against the
 * dyld shared cache (eagl says the slide) and the GL library (eagl says where it is), and kernel PCs
 * unslid for the kernel's own symbols.
 */
#include <mach/mach_types.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <string.h>
#include <os/atomic_private.h>
#include <stdarg.h>

extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void *get_bsdtask_info(task_t);
extern char *proc_name_address(void *p);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern vm_offset_t vm_kernel_slide;

#define PROF_MAX        (1u << 18)
#define PROF_KERNEL     (1ull << 63)    /* the sample was taken in the kernel: the address is the user PC of the system call */

struct prof_sample {
	uint64_t pc;            /* user PC (| PROF_KERNEL: in the kernel, the system call's user PC) */
	uint64_t kpc;           /* kernel PC when in the kernel, unslid; else the user LR */
	uint64_t kcall[3];      /* in the kernel: the callers, by the frame pointers, unslid */
	uint32_t thread;        /* the thread's id, low bits */
};

static struct prof_sample *prof_samples;
static volatile uint32_t prof_count, prof_seen_other;
static char prof_name[32] = "backboardd";
static uint32_t prof_after = 40, prof_secs = 30, prof_us = 250;

static void
prof_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* On every processor, in interrupt context: is the interrupted thread one of the process's? */
static void
prof_xcall(__unused void *parameter)
{
	thread_t thread = current_thread();
	task_t task = get_threadtask(thread);
	void *proc = task != TASK_NULL ? get_bsdtask_info(task) : NULL;
	arm_saved_state_t *state = getCpuDatap()->cpu_int_state;
	const char *name;
	uint32_t at;
	uint64_t pc, kpc;

	if (proc == NULL || state == NULL || (name = proc_name_address(proc)) == NULL || strncmp(name, prof_name, sizeof(prof_name)) != 0) {
		return;
	}
	if (PSR64_IS_USER(get_saved_state_cpsr(state))) {
		pc = get_saved_state_pc(state);
		kpc = get_saved_state_lr(state);
	} else {
		arm_saved_state_t *user = thread->machine.upcb;

		pc = (user != NULL ? get_saved_state_pc(user) : 0) | PROF_KERNEL;
		kpc = get_saved_state_pc(state) - vm_kernel_slide;
	}
	at = os_atomic_inc_orig(&prof_count, relaxed);
	if (at < PROF_MAX) {
		prof_samples[at].pc = pc;
		prof_samples[at].kpc = kpc;
		prof_samples[at].thread = (uint32_t)thread_tid(thread);
		prof_samples[at].kcall[0] = prof_samples[at].kcall[1] = prof_samples[at].kcall[2] = 0;
		if (pc & PROF_KERNEL) {
			/* the callers: frame records { previous fp, lr } on this thread's kernel stack, and nowhere else */
			vm_offset_t low = thread->kernel_stack, high = low + kernel_stack_size;
			uint64_t fp = get_saved_state_fp(state);

			for (unsigned k = 0; k < 3 && fp >= low && fp + 16 <= high && (fp & 7) == 0; k++) {
				uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];

				prof_samples[at].kcall[k] = (uint64_t)ptrauth_strip((void *)lr, ptrauth_key_return_address) - vm_kernel_slide;
				if (next <= fp) {
					break;
				}
				fp = next;
			}
		}
	}
}

/* The kernel's places with their callers: (pc, caller, caller's caller) counted together. */
static void
prof_stacks(uint32_t n, uint32_t shown)
{
	const uint32_t slots = 1u << 15;
	struct { uint64_t key[3]; uint32_t count; } *table = kalloc_data(slots * sizeof(*table), Z_WAITOK | Z_ZERO);

	if (table == NULL) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint64_t key[3] = { prof_samples[i].kpc, prof_samples[i].kcall[0], prof_samples[i].kcall[1] };
		uint32_t h;

		if (!(prof_samples[i].pc & PROF_KERNEL)) {
			continue;
		}
		h = (uint32_t)(((key[0] >> 2) ^ (key[1] >> 1) ^ key[2]) * 2654435761u) & (slots - 1);
		while (table[h].count != 0 && memcmp(table[h].key, key, sizeof(key)) != 0) {
			h = (h + 1) & (slots - 1);
		}
		memcpy(table[h].key, key, sizeof(key));
		table[h].count++;
	}
	prof_say("prof: kernel places with their callers, the %u most seen:\n", shown);
	for (uint32_t k = 0; k < shown; k++) {
		uint32_t best = 0, at = 0;

		for (uint32_t h = 0; h < slots; h++) {
			if (table[h].count > best) {
				best = table[h].count;
				at = h;
			}
		}
		if (best == 0) {
			break;
		}
		prof_say("prof: s %#llx %#llx %#llx %u\n", table[at].key[0], table[at].key[1], table[at].key[2], best);
		table[at].count = 0;
	}
	kfree_data(table, slots * sizeof(*table));
}

/* The places most often seen: an open hash of address -> count, then the top ones, largest first. */
static void
prof_top(const char *what, bool kernel_side, uint32_t n, uint32_t shown)
{
	const uint32_t slots = 1u << 16;
	struct { uint64_t pc; uint32_t count; } *table = kalloc_data(slots * sizeof(*table), Z_WAITOK | Z_ZERO);

	if (table == NULL) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint64_t pc = kernel_side ? prof_samples[i].kpc : prof_samples[i].pc;
		uint32_t h;

		if (kernel_side && !(prof_samples[i].pc & PROF_KERNEL)) {
			continue;
		}
		h = (uint32_t)((pc >> 2) * 2654435761u) & (slots - 1);
		while (table[h].count != 0 && table[h].pc != pc) {
			h = (h + 1) & (slots - 1);
		}
		table[h].pc = pc;
		table[h].count++;
	}
	prof_say("prof: %s, the %u most seen:\n", what, shown);
	for (uint32_t k = 0; k < shown; k++) {
		uint32_t best = 0, at = 0;

		for (uint32_t h = 0; h < slots; h++) {
			if (table[h].count > best) {
				best = table[h].count;
				at = h;
			}
		}
		if (best == 0) {
			break;
		}
		prof_say("prof: %s %#llx %u\n", kernel_side ? "k" : "u", table[at].pc, best);
		table[at].count = 0;
	}
	kfree_data(table, slots * sizeof(*table));
}

static void
prof_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t span, start;
	uint32_t n, rounds = 0, kernel = 0;
	uint32_t threads[8] = { 0 }, thread_counts[8] = { 0 };

	delay((uint64_t)prof_after * USEC_PER_SEC);
	prof_say("prof: sampling \"%s\" every %u us for %u s on all processors\n", prof_name, prof_us, prof_secs);
	nanoseconds_to_absolutetime((uint64_t)prof_secs * NSEC_PER_SEC, &span);
	start = mach_absolute_time();
	while (mach_absolute_time() - start < span && prof_count < PROF_MAX) {
		cpu_broadcast_xcall_simple(TRUE, prof_xcall, NULL);
		rounds++;
		delay(prof_us);
	}
	n = prof_count < PROF_MAX ? prof_count : PROF_MAX;
	for (uint32_t i = 0; i < n; i++) {
		kernel += (prof_samples[i].pc & PROF_KERNEL) != 0;
		for (unsigned t = 0; t < 8; t++) {
			if (threads[t] == prof_samples[i].thread || thread_counts[t] == 0) {
				threads[t] = prof_samples[i].thread;
				thread_counts[t]++;
				break;
			}
		}
	}
	prof_say("prof: %u rounds, %u samples of \"%s\" (%u in the kernel); by thread:", rounds, n, prof_name, kernel);
	for (unsigned t = 0; t < 8 && thread_counts[t] != 0; t++) {
		prof_say(" %#x %u", threads[t], thread_counts[t]);
	}
	prof_say("\n");
	prof_top("user addresses (| 1<<63: in a system call made from there)", false, n, 120);
	prof_top("kernel addresses, unslid", true, n, 40);
	prof_stacks(n, 40);
	prof_say("prof: done\n");
	thread_terminate(current_thread());
}

void hakios_prof_init(void);

void
hakios_prof_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("prof", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_arg_str("prof_name", prof_name, sizeof(prof_name));
	PE_parse_boot_argn("prof_after", &prof_after, sizeof(prof_after));
	PE_parse_boot_argn("prof_secs", &prof_secs, sizeof(prof_secs));
	PE_parse_boot_argn("prof_us", &prof_us, sizeof(prof_us));
	prof_us = prof_us < 50 ? 50 : prof_us;
	prof_samples = kalloc_data(PROF_MAX * sizeof(*prof_samples), Z_WAITOK | Z_ZERO);
	if (prof_samples == NULL || kernel_thread_start(prof_thread, NULL, &thread) != KERN_SUCCESS) {
		prof_say("prof: cannot start\n");
		return;
	}
	thread_deallocate(thread);
}
