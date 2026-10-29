/*
 * usbsh: commands from the computer over the phone's USB serial port (the kernel's /dev/hakios-usb,
 * boot-arg usb=2: bsd/dev/hakios.usb.c), run on iOS, their output sent back.
 *
 * The port carries the kernel's log until the computer types something; then the log pauses and
 * what is typed comes here, a line at a time (echoed, with backspace). A line is a built-in or a
 * program with its arguments ("quotes" keep spaces): a name without a slash is looked for in
 * /redwood/bin, /usr/bin, /bin, /usr/sbin, /sbin, /usr/libexec. Its standard output and error come
 * back as it writes them; Ctrl-C interrupts it (twice: kills it). "log" gives the port back to the
 * log, until the next thing typed. Started by launchd at boot (com.hakios.usbsh); without usb=2
 * there is no port and it ends at once.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HAKIOS_USB_LOG  _IO('u', 1)     /* (bsd/dev/hakios.usb.c) */

extern char **environ;

static int port = -1;
static int talking;                     /* the computer has typed since the log last had the port */
static char cwd[1024] = "/";

/* To the port, '\n' as "\r\n" (a terminal on the other side). */
static void
out_bytes(const char *p, size_t n)
{
	char buf[1024];
	size_t m = 0;

	for (size_t i = 0; i < n; i++) {
		if (p[i] == '\n') {
			buf[m++] = '\r';
		}
		buf[m++] = p[i];
		if (m >= sizeof(buf) - 2) {
			(void)write(port, buf, m);
			m = 0;
		}
	}
	if (m > 0) {
		(void)write(port, buf, m);
	}
}

static void out(const char *format, ...) __printflike(1, 2);

static void
out(const char *format, ...)
{
	char line[2048];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	out_bytes(line, n < 0 ? 0 : (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

static void
prompt(void)
{
	out("ios:%s# ", cwd);
}

/* Words of a line: spaces separate, "double quotes" keep them. */
static int
split(char *line, char **argv, int most)
{
	int argc = 0;
	char *p = line;

	while (*p && argc < most - 1) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (!*p) {
			break;
		}
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"') {
				p++;
			}
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t') {
				p++;
			}
		}
		if (*p) {
			*p++ = 0;
		}
	}
	argv[argc] = NULL;
	return argc;
}

static void
cmd_ls(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	unsigned n = 0;

	if (d == NULL) {
		out("ls: %s: %s\n", path, strerror(errno));
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char full[2048];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (lstat(full, &st) != 0) {
			out("  ?          %s\n", e->d_name);
		} else {
			out("  %c%03o %10lld  %s%s\n", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISCHR(st.st_mode) ? 'c' : '-',
			    st.st_mode & 0777, (long long)st.st_size, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
		}
		n++;
	}
	closedir(d);
	out("(%u entries)\n", n);
}

static void
cmd_cat(const char *path)
{
	char buf[4096];
	ssize_t n;
	size_t total = 0;
	int f = open(path, O_RDONLY);

	if (f < 0) {
		out("cat: %s: %s\n", path, strerror(errno));
		return;
	}
	while ((n = read(f, buf, sizeof(buf))) > 0 && total < (1u << 20)) {
		out_bytes(buf, (size_t)n);
		total += (size_t)n;
	}
	close(f);
	if (total >= (1u << 20)) {
		out("\n(cat: stopped at 1 MB)\n");
	}
}

static void
cmd_uname(void)
{
	struct utsname u;
	char version[64] = "", model[64] = "";
	size_t len;
	struct timeval boot;
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };

	uname(&u);
	len = sizeof(version);
	sysctlbyname("kern.osversion", version, &len, NULL, 0);
	len = sizeof(model);
	sysctlbyname("hw.machine", model, &len, NULL, 0);
	len = sizeof(boot);
	sysctl(mib, 2, &boot, &len, NULL, 0);
	out("%s %s %s %s (%s, iOS build %s), up %ld s\n%s\n", u.sysname, u.nodename, u.release, u.machine, model, version,
	    (long)(time(NULL) - boot.tv_sec), u.version);
}

static void
cmd_ps(void)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	struct kinfo_proc *procs;

	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0 || (procs = malloc(len + 16 * sizeof(*procs))) == NULL) {
		out("ps: %s\n", strerror(errno));
		return;
	}
	len += 16 * sizeof(*procs);
	if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) {
		out("ps: %s\n", strerror(errno));
		free(procs);
		return;
	}
	out("  PID  PPID   UID  COMMAND\n");
	for (size_t i = 0; i < len / sizeof(*procs); i++) {
		out("%5d %5d %5u  %s\n", procs[i].kp_proc.p_pid, procs[i].kp_eproc.e_ppid, procs[i].kp_eproc.e_ucred.cr_uid,
		    procs[i].kp_proc.p_comm);
	}
	out("(%zu processes)\n", len / sizeof(*procs));
	free(procs);
}

static const char *
find_program(const char *name, char *buf, size_t size)
{
	static const char *const dirs[] = { "/redwood/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin", "/usr/libexec" };

	if (strchr(name, '/')) {
		return name;
	}
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(buf, size, "%s/%s", dirs[i], name);
		if (access(buf, X_OK) == 0) {
			return buf;
		}
	}
	return NULL;
}

/* A program, its output to the port as it comes; Ctrl-C from the computer interrupts it. */
static void
run(char **argv)
{
	char where[1024];
	const char *path = find_program(argv[0], where, sizeof(where));
	posix_spawn_file_actions_t actions;
	int pipefd[2], status = 0, interrupts = 0;
	pid_t pid;

	if (path == NULL) {
		out("%s: no such program (help: what there is)\n", argv[0]);
		return;
	}
	if (pipe(pipefd) != 0) {
		out("usbsh: pipe: %s\n", strerror(errno));
		return;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, port);
	errno = posix_spawn(&pid, path, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	if (errno != 0) {
		out("%s: %s\n", path, strerror(errno));
		close(pipefd[0]);
		return;
	}
	for (;;) {
		fd_set r;
		char buf[4096];
		ssize_t n;

		FD_ZERO(&r);
		FD_SET(pipefd[0], &r);
		FD_SET(port, &r);
		if (select((pipefd[0] > port ? pipefd[0] : port) + 1, &r, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (FD_ISSET(port, &r) && (n = read(port, buf, sizeof(buf))) > 0) {
			for (ssize_t i = 0; i < n; i++) {
				if (buf[i] == 3) {
					kill(pid, ++interrupts > 1 ? SIGKILL : SIGINT);
					out("^C\n");
				}
			}
		}
		if (FD_ISSET(pipefd[0], &r)) {
			n = read(pipefd[0], buf, sizeof(buf));
			if (n <= 0) {
				break;
			}
			out_bytes(buf, (size_t)n);
		}
	}
	close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (WIFSIGNALED(status)) {
		out("[killed by signal %d]\n", WTERMSIG(status));
	} else if (WEXITSTATUS(status) != 0) {
		out("[exit status %d]\n", WEXITSTATUS(status));
	}
}

static void
help(void)
{
	out("usbsh: commands over USB, run on iOS\n"
	    "  ls [DIR]  cat FILE  cd DIR  pwd  uname  ps  kill [-SIG] PID  log  help\n"
	    "  anything else: a program and its arguments; names are looked for in\n"
	    "  /redwood/bin /usr/bin /bin /usr/sbin /sbin /usr/libexec -- e.g. battery_test, vol_ls /, net_test\n"
	    "  Ctrl-C interrupts a program; 'log' gives the port back to the kernel's log\n");
}

static void
execute(char *line)
{
	char *argv[64];
	int argc = split(line, argv, 64);

	if (argc == 0) {
		return;
	}
	if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
		help();
	} else if (!strcmp(argv[0], "log") || !strcmp(argv[0], "exit")) {
		out("usbsh: the log again (type anything to come back)\n");
		ioctl(port, HAKIOS_USB_LOG);
		talking = 0;
	} else if (!strcmp(argv[0], "ls")) {
		cmd_ls(argc > 1 ? argv[1] : ".");
	} else if (!strcmp(argv[0], "cat") && argc > 1) {
		for (int i = 1; i < argc; i++) {
			cmd_cat(argv[i]);
		}
	} else if (!strcmp(argv[0], "cd")) {
		if (chdir(argc > 1 ? argv[1] : "/") != 0) {
			out("cd: %s\n", strerror(errno));
		}
		if (getcwd(cwd, sizeof(cwd)) == NULL) {
			strlcpy(cwd, "?", sizeof(cwd));
		}
	} else if (!strcmp(argv[0], "pwd")) {
		out("%s\n", cwd);
	} else if (!strcmp(argv[0], "uname")) {
		cmd_uname();
	} else if (!strcmp(argv[0], "ps")) {
		cmd_ps();
	} else if (!strcmp(argv[0], "kill") && argc > 1) {
		int sig = SIGTERM, at = 1;

		if (argv[1][0] == '-' && argc > 2) {
			sig = atoi(argv[1] + 1);
			at = 2;
		}
		if (kill(atoi(argv[at]), sig) != 0) {
			out("kill: %s\n", strerror(errno));
		}
	} else {
		run(argv);
	}
}

int
main(void)
{
	char line[1024];
	size_t len = 0;

	signal(SIGPIPE, SIG_IGN);
	port = open("/dev/hakios-usb", O_RDWR);
	if (port < 0) {
		printf("usbsh: no USB serial port (%s): the kernel has not got the controller (usb=2)\n", strerror(errno));
		return 0;
	}
	printf("usbsh: the USB serial port is ours to share with the kernel's log\n");
	chdir("/");
	for (;;) {
		char buf[256];
		ssize_t n = read(port, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			printf("usbsh: the port: %s\n", strerror(errno));
			return 1;
		}
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];

			if (!talking) {
				/* the first thing typed since the log had the port */
				talking = 1;
				len = 0;
				out("\n--- usbsh: the log is paused; 'help' for commands, 'log' to resume it ---\n");
				prompt();
				if (c == '\r' || c == '\n') {
					continue;
				}
			}
			if (c == '\r' || c == '\n') {
				if (c == '\n' && i > 0 && buf[i - 1] == '\r') {
					continue;
				}
				out("\n");
				line[len] = 0;
				execute(line);
				len = 0;
				if (talking) {
					prompt();
				}
			} else if (c == 0x7f || c == 8) {
				if (len > 0) {
					len--;
					out("\b \b");
				}
			} else if (c == 3) {
				len = 0;
				out("^C\n");
				prompt();
			} else if (c >= 0x20 && len < sizeof(line) - 1) {
				line[len++] = c;
				out_bytes(&c, 1);
			}
		}
	}
}
