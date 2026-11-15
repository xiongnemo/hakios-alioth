/*
 * psample: where a process's time goes, without a debugger (2026-11-15). lldb-19 dies attaching to an application
 * with ~900 libraries (Nagram with its photo picker open) and leaves it stopped; this looks at a process from the
 * outside with the task port, a thread held for microseconds at a time.
 *
 *   psample top [-t SECONDS] [-r COUNT] [-n LINES]
 *       processor time of every process over SECONDS (default 5), the busiest first, COUNT times (default 1); each
 *       report starts with the kernel's uptime ("@ 1234.5", as dmesg's stamps) for lining up with other logs
 *   psample threads PID|NAME [-t SECONDS]
 *       the same for the threads of one process, with their names
 *   psample stacks PID|NAME [-t SECONDS] [-i MICROSECONDS] [-d DEPTH] [-a] [-T WORD] [-o FILE] [-x PROGRAM ARG...]
 *       (-x, last: a program started as the sampling starts and waited for -- rwtouch's taps driving the application)
 *       every -i (default 2000) for -t (default 10): the main thread (-a: every thread; -T: the threads whose name
 *       has WORD) held, its registers and its frame-pointer chain read, let go. Lines (to FILE, default stdout):
 *         psample 2 pid P name N interval I depth D uptime U timebase NUMER/DENOM
 *         thread TID NAME                       (once, when first seen)
 *         s MICROSECONDS TID R|W:CPU:PRI:SYS PC PC ...
 *             (R: was running or runnable, W: waiting; CPU: the thread's processor time so far in us -- of a "running"
 *             thread it says whether it had a core or waited for one; PRI: its priority then; SYS: how much of CPU
 *             was in the kernel (system calls, page faults): what of it is this port's; PC first, then callers)
 *         cache SLIDE BASE                      (the shared cache's slide and where it is)
 *         image LOAD SLIDE PATH                 (every loaded image, read at the end: what loaded meanwhile too)
 *         end STOPS HELD_US ELAPSED_US
 *       Names come on the computer: work/tools/psample-report.py (shared cache: work/wsl/sym.py; the app's own
 *       images: llvm-symbolizer on its files).
 * Needs the task port of the process: src/psample.entitlements (as debugserver's), run as root.
 */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_status.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* (the read-port trap: in libsystem_kernel since iOS 14.5, not in this SDK's headers) */
extern kern_return_t task_read_for_pid(mach_port_name_t target_tport, int pid, mach_port_name_t *t) __attribute__((weak_import));
/* (libproc: not in this SDK's headers) */
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
#define PROC_PIDTHREADID64INFO  15
struct proc_threadinfo {
	uint64_t pth_user_time, pth_system_time;
	int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority,
	    pth_maxpriority;
	char pth_name[64];
};

#define ADDR_MASK       0x0000007fffffffffULL   /* (user addresses: 39 bits; anything above is not an address) */
#define MAX_PIDS        4096
#define MAX_THREADS     512
#define MAX_DEPTH       256
#define STACK_GULP      (32 * 1024)

static mach_timebase_info_data_t tb;

static double
abs_to_s(uint64_t t)
{
	return (double)t * tb.numer / tb.denom / 1e9;
}

static double
uptime_s(void)
{
	return abs_to_s(mach_absolute_time());
}

static pid_t
find_pid(const char *what)
{
	char *end;
	long n = strtol(what, &end, 10);
	static pid_t pids[MAX_PIDS];
	int count;
	pid_t found = -1;

	if (*what != 0 && *end == 0) {
		return (pid_t)n;
	}
	count = proc_listallpids(pids, sizeof(pids)) ;
	for (int i = 0; i < count && i < MAX_PIDS; i++) {
		char name[256] = "";

		if (pids[i] > 0 && proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, what) == 0 &&
		    pids[i] > found) {
			found = pids[i];        /* (the newest of that name) */
		}
	}
	return found;
}

/* ---- psample top / threads ---- */

struct row { int id; uint64_t before, after; char name[64]; };

static int
by_time(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	uint64_t dx = x->after - x->before, dy = y->after - y->before;

	return dx < dy ? 1 : dx > dy ? -1 : 0;
}

static uint64_t
pid_time(pid_t pid)
{
	struct rusage_info_v2 ri;

	if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) {
		return 0;
	}
	return ri.ri_user_time + ri.ri_system_time;
}

static int
top(int seconds, int count, int lines)
{
	static pid_t pids[MAX_PIDS];
	static struct row rows[MAX_PIDS];

	for (int r = 0; r < count; r++) {
		int n = proc_listallpids(pids, sizeof(pids)), used = 0;
		double t0 = uptime_s(), wall;
		uint64_t total = 0;

		for (int i = 0; i < n && i < MAX_PIDS; i++) {
			if (pids[i] <= 0) {
				continue;
			}
			rows[used].id = pids[i];
			rows[used].before = pid_time(pids[i]);
			rows[used].name[0] = 0;
			proc_name(pids[i], rows[used].name, sizeof(rows[used].name));
			used++;
		}
		sleep((unsigned)seconds);
		wall = uptime_s() - t0;
		for (int i = 0; i < used; i++) {
			rows[i].after = pid_time(rows[i].id);
			if (rows[i].after < rows[i].before) {
				rows[i].after = rows[i].before;         /* (gone meanwhile, or its pid taken again) */
			}
			total += rows[i].after - rows[i].before;
		}
		qsort(rows, (size_t)used, sizeof(rows[0]), by_time);
		printf("@ %.1f: %d processes, %.1f s; all of them %.0f%% of one processor\n", t0, used, wall,
		    100.0 * abs_to_s(total) / wall);
		for (int i = 0; i < used && i < lines; i++) {
			double s = abs_to_s(rows[i].after - rows[i].before);

			if (s * 1000 < wall) {
				break;                                  /* (under 0.1%) */
			}
			printf("  %5.1f%%  %6d  %s\n", 100.0 * s / wall, rows[i].id, rows[i].name);
		}
		fflush(stdout);
	}
	return 0;
}

/*
 * The process's task port: the control port when the kernel gives one (iOS's developer mode on: hakios.board.c), so
 * that a thread can be held while its stack is read; else the read port, with which threads are looked at running
 * (thread_get_state stops one for the moment it takes; a stack read after that may be torn now and then). With the
 * developer mode off, task_for_pid says KERN_SUCCESS and hands back a DEAD name (osfmk/kern/ipc_tt.c).
 */
static bool task_is_control;

static task_t
task_of(pid_t pid)
{
	task_t task = MACH_PORT_NULL;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);

	if (kr == KERN_SUCCESS && MACH_PORT_VALID(task)) {
		task_is_control = true;
		return task;
	}
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "psample: task_for_pid(%d): %s (%d)\n", pid, mach_error_string(kr), kr);
	} else {
		fprintf(stderr, "psample: task_for_pid(%d) gave a %s port (iOS's developer mode off?): reading only\n", pid,
		    task == MACH_PORT_DEAD ? "dead" : "null");
	}
	task = MACH_PORT_NULL;
	if (task_read_for_pid == NULL) {
		fprintf(stderr, "psample: no task_read_for_pid in this system\n");
		return MACH_PORT_NULL;
	}
	kr = task_read_for_pid(mach_task_self(), pid, &task);
	if (kr != KERN_SUCCESS || !MACH_PORT_VALID(task)) {
		fprintf(stderr, "psample: task_read_for_pid(%d): %s (%d)\n", pid, mach_error_string(kr), kr);
		return MACH_PORT_NULL;
	}
	task_is_control = false;
	return task;
}

static uint64_t
thread_tid(thread_t th)
{
	thread_identifier_info_data_t ii;
	mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;

	return thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &n) == KERN_SUCCESS ? ii.thread_id : 0;
}

static void
thread_name(pid_t pid, uint64_t tid, char *name, size_t size)
{
	struct proc_threadinfo pi;

	name[0] = 0;
	if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, tid, &pi, sizeof(pi)) == (int)sizeof(pi)) {
		strlcpy(name, pi.pth_name, size);
	}
	for (char *c = name; *c; c++) {
		if (*c == ' ' || *c == '\n') *c = '_';         /* (one word in the lines) */
	}
	if (name[0] == 0) {
		strlcpy(name, "-", size);
	}
}

static uint64_t
thread_time(thread_t th)
{
	thread_basic_info_data_t bi;
	mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;

	if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS) {
		return 0;
	}
	return (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000 + bi.user_time.microseconds +
	    bi.system_time.microseconds;
}

static int
threads(pid_t pid, int seconds)
{
	task_t task = task_of(pid);
	thread_act_array_t list;
	mach_msg_type_number_t n;
	static struct row rows[MAX_THREADS];
	static thread_t ports[MAX_THREADS];
	char pname[256] = "";
	double t0, wall;
	unsigned used;

	if (task == MACH_PORT_NULL) {
		return 1;
	}
	kern_return_t kr = task_threads(task, &list, &n);

	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "psample: task_threads: %s (%d)\n", mach_error_string(kr), kr);
		return 1;
	}
	used = n < MAX_THREADS ? n : MAX_THREADS;
	t0 = uptime_s();
	for (unsigned i = 0; i < used; i++) {
		ports[i] = list[i];
		rows[i].id = (int)i;
		rows[i].before = thread_time(list[i]);
		thread_name(pid, thread_tid(list[i]), rows[i].name, sizeof(rows[i].name));
	}
	sleep((unsigned)seconds);
	wall = uptime_s() - t0;
	for (unsigned i = 0; i < used; i++) {
		rows[i].after = thread_time(ports[i]);
		if (rows[i].after < rows[i].before) rows[i].after = rows[i].before;
	}
	qsort(rows, used, sizeof(rows[0]), by_time);
	proc_name(pid, pname, sizeof(pname));
	printf("@ %.1f: %s (%d), %u threads, %.1f s\n", t0, pname, pid, used, wall);
	for (unsigned i = 0; i < used; i++) {
		double s = (rows[i].after - rows[i].before) / 1e6;

		printf("  %5.1f%%  #%-3d %s%s\n", 100.0 * s / wall, rows[i].id, rows[i].name, rows[i].id == 0 ? " (main)" : "");
	}
	for (unsigned i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), list[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(list[0]));
	return 0;
}

/* ---- psample stacks ---- */

static bool
peek(task_t task, uint64_t at, void *buffer, size_t size)
{
	vm_size_t got = 0;

	return vm_read_overwrite(task, (vm_address_t)at, size, (vm_address_t)buffer, &got) == KERN_SUCCESS && got == size;
}

/* one stack: PC, LR, then the frame records' return addresses (fp -> {next fp, return address}) */
static int
walk(task_t task, thread_t th, uint64_t *pcs, int depth)
{
	static uint8_t gulp[STACK_GULP];
	arm_thread_state64_t st;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	uint64_t fp, sp, lo = 0, hi = 0;
	int k = 0;

	if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) {
		return 0;
	}
	pcs[k++] = arm_thread_state64_get_pc(st) & ADDR_MASK;
	if (k < depth) {
		pcs[k++] = arm_thread_state64_get_lr(st) & ADDR_MASK;
	}
	fp = arm_thread_state64_get_fp(st) & ADDR_MASK;
	sp = arm_thread_state64_get_sp(st) & ADDR_MASK;
	/* (the stack from sp up in one read when it can: a frame then costs nothing) */
	for (size_t size = STACK_GULP; size >= 4096; size /= 2) {
		if (peek(task, sp, gulp, size)) {
			lo = sp, hi = sp + size;
			break;
		}
	}
	for (int frames = 0; fp != 0 && (fp & 7) == 0 && k < depth && frames < MAX_DEPTH; frames++) {
		uint64_t rec[2];

		if (fp >= lo && fp + 16 <= hi) {
			memcpy(rec, gulp + (fp - lo), 16);
		} else if (!peek(task, fp, rec, 16)) {
			break;
		}
		rec[1] &= ADDR_MASK;
		if (rec[1] == 0) {
			break;
		}
		if (!(frames == 0 && rec[1] == pcs[1])) {      /* (LR is often the first record's return address) */
			pcs[k++] = rec[1];
		}
		if ((rec[0] & ADDR_MASK) <= fp) {
			break;                                  /* (frames go up the stack) */
		}
		fp = rec[0] & ADDR_MASK;
	}
	return k;
}

static void
images(task_t task, FILE *out)
{
	struct task_dyld_info di;
	mach_msg_type_number_t n = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos all;
	struct dyld_image_info *infos = NULL;

	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &n) != KERN_SUCCESS ||
	    !peek(task, di.all_image_info_addr, &all, sizeof(all) < di.all_image_info_size ? sizeof(all) :
	    di.all_image_info_size)) {
		fprintf(out, "# no image list (TASK_DYLD_INFO)\n");
		return;
	}
	fprintf(out, "cache 0x%llx 0x%llx\n", (unsigned long long)all.sharedCacheSlide,
	    (unsigned long long)all.sharedCacheBaseAddress);
	/* (dyld empties infoArray while it changes it: try again a little) */
	for (int tries = 0; tries < 50 && (all.infoArray == NULL || all.infoArrayCount == 0); tries++) {
		usleep(2000);
		peek(task, di.all_image_info_addr, &all, sizeof(all) < di.all_image_info_size ? sizeof(all) :
		    di.all_image_info_size);
	}
	if (all.infoArray == NULL || all.infoArrayCount == 0 || all.infoArrayCount > 100000) {
		fprintf(out, "# no image list (infoArray %p, %u)\n", (void *)all.infoArray, all.infoArrayCount);
		return;
	}
	infos = calloc(all.infoArrayCount, sizeof(*infos));
	if (infos == NULL || !peek(task, (uint64_t)(uintptr_t)all.infoArray, infos, all.infoArrayCount * sizeof(*infos))) {
		fprintf(out, "# no image list (infoArray unreadable)\n");
		free(infos);
		return;
	}
	for (uint32_t i = 0; i < all.infoArrayCount; i++) {
		uint64_t load = (uint64_t)(uintptr_t)infos[i].imageLoadAddress, slide = 0;
		char path[1024] = "?";
		uint8_t head[0x8000];
		struct mach_header_64 *mh = (struct mach_header_64 *)head;

		/* (a path in one read when it can; near the end of its page, a shorter one) */
		for (size_t size = sizeof(path) - 1; size >= 64; size /= 2) {
			if (peek(task, (uint64_t)(uintptr_t)infos[i].imageFilePath, path, size)) {
				path[size] = 0;
				break;
			}
		}
		if (peek(task, load, head, sizeof(*mh)) && mh->magic == MH_MAGIC_64 &&
		    sizeof(*mh) + mh->sizeofcmds <= sizeof(head) && peek(task, load, head, sizeof(*mh) + mh->sizeofcmds)) {
			uint8_t *lc = head + sizeof(*mh);

			for (uint32_t c = 0; c < mh->ncmds && lc + sizeof(struct load_command) <= head + sizeof(head); c++) {
				struct segment_command_64 *seg = (struct segment_command_64 *)lc;

				if (seg->cmd == LC_SEGMENT_64 && strcmp(seg->segname, "__TEXT") == 0) {
					slide = load - seg->vmaddr;
					break;
				}
				lc += ((struct load_command *)lc)->cmdsize;
			}
		}
		fprintf(out, "image 0x%llx 0x%llx %s\n", (unsigned long long)load, (unsigned long long)slide, path);
	}
	free(infos);
}

/*
 * -x PROGRAM ARG...: started as the sampling starts (posix_spawn, the arguments as given), waited for at its end -- what
 * an unattended round needs to drive the application while it is sampled (rwtouch's taps), because usbsh runs one
 * command at a time and has no shell to put one in the background (11-16). Its pid and start are written into the
 * file; a program still running 30 s after the sampling is asked to stop (SIGTERM).
 */
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static pid_t
start_alongside(char **xargv, FILE *out, double at_s)
{
	pid_t child = -1;
	int e;

	if (xargv == NULL || xargv[0] == NULL) {
		return -1;
	}
	if ((e = posix_spawn(&child, xargv[0], NULL, NULL, xargv, environ)) != 0) {
		fprintf(out, "# -x %s: %s\n", xargv[0], strerror(e));
		fprintf(stderr, "psample: -x %s: %s\n", xargv[0], strerror(e));
		return -1;
	}
	fprintf(out, "# -x started pid %d at %.3f s:", child, at_s);
	for (char **a = xargv; *a; a++) {
		fprintf(out, " %s", *a);
	}
	fputc('\n', out);
	return child;
}

static void
end_alongside(pid_t child, FILE *out)
{
	int status = 0;

	if (child <= 0) {
		return;
	}
	for (int waited = 0; waited < 300; waited++) {
		if (waitpid(child, &status, WNOHANG) == child) {
			fprintf(out, "# -x pid %d ended: %s %d\n", child, WIFEXITED(status) ? "exit" : "signal",
			    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			return;
		}
		usleep(100000);
	}
	kill(child, SIGTERM);
	waitpid(child, &status, 0);
	fprintf(out, "# -x pid %d still running 30 s after the sampling: stopped\n", child);
}

struct seen { uint64_t tid; bool picked; };

static int
stacks(pid_t pid, int seconds, int interval_us, int depth, bool all, const char *word, char **xargv, FILE *out)
{
	pid_t alongside;
	task_t task = task_of(pid);
	static struct seen seen[MAX_THREADS];
	static uint64_t pcs[MAX_DEPTH + 2];
	unsigned nseen = 0, stops = 0;
	uint64_t held = 0, t0, end, next;
	char pname[256] = "";
	uint64_t main_tid = 0;          /* (the first thread task_threads gives: the main thread) */

	if (task == MACH_PORT_NULL) {
		return 1;
	}
	proc_name(pid, pname, sizeof(pname));
	fprintf(out, "psample 2 pid %d name %s interval %d depth %d uptime %.3f timebase %u/%u port %s\n", pid, pname,
	    interval_us, depth, uptime_s(), tb.numer, tb.denom, task_is_control ? "control" : "read");
	t0 = mach_absolute_time();
	end = t0 + (uint64_t)((double)seconds * 1e9 * tb.denom / tb.numer);
	next = t0;
	alongside = start_alongside(xargv, out, 0);
	while (mach_absolute_time() < end) {
		thread_act_array_t list;
		mach_msg_type_number_t n;
		uint64_t now_us;

		kern_return_t kr = task_threads(task, &list, &n);

		if (kr != KERN_SUCCESS) {
			fprintf(out, "# task_threads: %s (%d): the process is gone?\n", mach_error_string(kr), kr);
			fprintf(stderr, "psample: task_threads: %s (%d)\n", mach_error_string(kr), kr);
			break;
		}
		if (main_tid == 0 && n > 0) {
			main_tid = thread_tid(list[0]);
		}
		now_us = (uint64_t)(abs_to_s(mach_absolute_time() - t0) * 1e6);
		for (unsigned i = 0; i < n; i++) {
			thread_t th = list[i];
			uint64_t tid = thread_tid(th);
			unsigned s;
			thread_basic_info_data_t bi;
			mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT;
			uint64_t a, b;
			int k;

			for (s = 0; s < nseen && seen[s].tid != tid; s++) {
			}
			if (s == nseen && nseen < MAX_THREADS) {
				char name[64];

				thread_name(pid, tid, name, sizeof(name));
				seen[s].tid = tid;
				seen[s].picked = all || (word != NULL ? strstr(name, word) != NULL : tid == main_tid);
				fprintf(out, "thread %llu %s%s\n", (unsigned long long)tid, name, tid == main_tid ? " main" : "");
				nseen++;
			}
			if (s == nseen || !seen[s].picked) {
				continue;
			}
			if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &bn) != KERN_SUCCESS) {
				continue;
			}
			a = mach_absolute_time();
			if (task_is_control) {
				if (thread_suspend(th) != KERN_SUCCESS) {
					continue;
				}
				k = walk(task, th, pcs, depth);
				thread_resume(th);
			} else {
				k = walk(task, th, pcs, depth);         /* (a read port: looked at running) */
			}
			b = mach_absolute_time();
			held += b - a;
			/* (R|W:the thread's processor time so far, us:its priority now -- a "running" thread may be one
			   waiting for a core: the time it got says which) */
			struct policy_timeshare_info ts = { 0 };
			mach_msg_type_number_t tn = POLICY_TIMESHARE_INFO_COUNT;

			if (thread_info(th, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t)&ts, &tn) != KERN_SUCCESS) {
				ts.cur_priority = -1;   /* (a fixed-priority thread, say) */
			}
			fprintf(out, "s %llu %llu %c:%llu:%d:%llu", (unsigned long long)now_us, (unsigned long long)tid,
			    bi.run_state == TH_STATE_RUNNING ? 'R' : 'W',
			    (unsigned long long)(bi.user_time.seconds + bi.system_time.seconds) * 1000000ull +
			    (unsigned long long)(bi.user_time.microseconds + bi.system_time.microseconds), ts.cur_priority,
			    (unsigned long long)bi.system_time.seconds * 1000000ull + (unsigned long long)bi.system_time.microseconds);
			for (int j = 0; j < k; j++) {
				fprintf(out, " %llx", (unsigned long long)pcs[j]);
			}
			fputc('\n', out);
		}
		stops++;
		for (unsigned i = 0; i < n; i++) {
			mach_port_deallocate(mach_task_self(), list[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(list[0]));
		next += (uint64_t)((double)interval_us * 1e3 * tb.denom / tb.numer);
		if (next > mach_absolute_time()) {
			mach_wait_until(next);
		} else {
			next = mach_absolute_time();
		}
	}
	images(task, out);
	end_alongside(alongside, out);
	fprintf(out, "end %u %.0f %.0f\n", stops, abs_to_s(held) * 1e6, abs_to_s(mach_absolute_time() - t0) * 1e6);
	fprintf(stderr, "psample: %s (%d): %u stops in %.1f s, threads held %.1f ms in all\n", pname, pid, stops,
	    abs_to_s(mach_absolute_time() - t0), abs_to_s(held) * 1e3);
	return 0;
}

static int
usage(void)
{
	fprintf(stderr, "usage: psample top [-t SECONDS] [-r COUNT] [-n LINES]\n"
	    "       psample threads PID|NAME [-t SECONDS]\n"
	    "       psample stacks PID|NAME [-t SECONDS] [-i MICROSECONDS] [-d DEPTH] [-a] [-T WORD] [-o FILE]\n"
	    "                      [-x PROGRAM ARG...]   (-x last: the program started as the sampling starts)\n");
	return 2;
}

int
main(int argc, char **argv)
{
	int seconds = -1, count = 1, lines = 25, interval = 2000, depth = 64;
	bool all = false;
	const char *word = NULL, *file = NULL, *what = NULL, *cmd;
	char **xargv = NULL;
	pid_t pid = -1;
	FILE *out = stdout;
	int rc;

	mach_timebase_info(&tb);
	if (argc < 2) {
		return usage();
	}
	cmd = argv[1];
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(a, "-a") == 0) all = true;
		else if (strcmp(a, "-t") == 0 && v) seconds = atoi(v), i++;
		else if (strcmp(a, "-r") == 0 && v) count = atoi(v), i++;
		else if (strcmp(a, "-n") == 0 && v) lines = atoi(v), i++;
		else if (strcmp(a, "-i") == 0 && v) interval = atoi(v), i++;
		else if (strcmp(a, "-d") == 0 && v) depth = atoi(v), i++;
		else if (strcmp(a, "-T") == 0 && v) word = v, i++;
		else if (strcmp(a, "-o") == 0 && v) file = v, i++;
		else if (strcmp(a, "-x") == 0 && v) { xargv = &argv[i + 1]; break; }
		else if (a[0] != '-' && what == NULL) what = a;
		else return usage();
	}
	if (depth < 2) depth = 2;
	if (depth > MAX_DEPTH) depth = MAX_DEPTH;
	if (interval < 100) interval = 100;
	if (strcmp(cmd, "top") == 0) {
		return top(seconds > 0 ? seconds : 5, count > 0 ? count : 1, lines);
	}
	if (what == NULL || (pid = find_pid(what)) <= 0) {
		fprintf(stderr, "psample: no process %s\n", what ? what : "(none given)");
		return 1;
	}
	if (strcmp(cmd, "threads") == 0) {
		return threads(pid, seconds > 0 ? seconds : 5);
	}
	if (strcmp(cmd, "stacks") != 0) {
		return usage();
	}
	if (file != NULL && (out = fopen(file, "w")) == NULL) {
		fprintf(stderr, "psample: %s: %s\n", file, strerror(errno));
		return 1;
	}
	rc = stacks(pid, seconds > 0 ? seconds : 10, interval, depth, all, word, xargv, out);
	if (out != stdout) fclose(out);
	return rc;
}
