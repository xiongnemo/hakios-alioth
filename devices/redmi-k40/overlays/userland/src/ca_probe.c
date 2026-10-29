/*
 * Inside the window server (backboardd; libhakios.gl.dylib, src/eagl.c starts this): what
 * CoreAnimation's render server has made of the display's refresh, read out of it.
 *
 * Boot-arg ca_probe=N: N looks, 5 s apart from 10 s after the window server started, each a few
 * lines "ca-probe:": QuartzCore's debug switches and values that are set (and the CA_ variables of the
 * environment, which set them), its device answers that bear on the frame rate, and of every display
 * its heartbeat, frame durations, packed current mode, the frame-interval ranges registered with it
 * and the server's frame interval.
 *
 * Boot-arg ca_poke=ADDR=VALUE[,ADDR=VALUE...]: writes VALUE -- a byte, or with a '.' in it a float --
 * at ADDR of QuartzCore's data, as the cache has it unslid, again and again for the first 30 s (so
 * that QuartzCore's own initialisation does not undo it): a debug switch of init_debug() is the byte
 * at 0x1e1f6b790 + the index of its name in the table at 0x1d5b57720, a value the float at
 * 0x1e1f6b670 + 4 * the index of its name in the table at 0x1d5b57be8.
 *
 * The addresses and offsets are those of iOS 16.5 (20F66) QuartzCore for arm64, found by disassembly
 * (work/wsl/qc-*.sh); with any other QuartzCore this says so and looks at nothing.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QC_PREFERS_60HZ_APT     0x1884c31a0     /* CADevicePrefers60HzAPT(): the build check, and the slide */
#define QC_REDUCED_VSYNC        0x18832a22c     /* CADisplayEnableReducedServerVsyncRender() */
#define QC_SWITCH_NAMES         0x1d5b57720     /* init_debug(): a byte per name ... */
#define QC_SWITCHES             0x1e1f6b790
#define QC_SWITCH_COUNT         0x99
#define QC_VALUE_NAMES          0x1d5b57be8     /* ... and a float per name */
#define QC_VALUES               0x1e1f6b670
#define QC_VALUE_COUNT          0x36
#define QC_SUPPORTS_APT         0x1e1f6a1a8     /* CADeviceSupportsAPT()'s answer (MobileGestalt, CA_DISABLE_APT) */
#define QC_PHONE_CLASS          0x1dcf3c558     /* DeviceClassNumber == 1 */
#define QC_CLCD_VTABLE          (0x1d5b44520 + 16)      /* CA::WindowServer::AppleCLCDDisplay, as objects point at it */

/* CA::WindowServer::Display, the virtual functions (AppleCLCDDisplay's table) */
#define VT_HARDWARE_REFRESH     0x2c0
#define VT_REFRESH              0x2c8
#define VT_HEARTBEAT            0x2d8           /* (seconds a beat, for all its name) */
#define VT_MIN_FRAME            0x2e0           /* frame durations: in beats */
#define VT_HW_MIN_FRAME         0x2e8
#define VT_PREFERRED_FRAME      0x2f0

extern char **environ;
static void (*say)(const char *format, ...);
static intptr_t slide;
static const char *pokes;
static unsigned looks;

static void
poke_all(void)
{
	const char *at = pokes;

	while (at != NULL && *at != 0 && *at != ' ') {
		char *end;
		uintptr_t address = (uintptr_t)strtoull(at, &end, 16);
		const char *value;
		size_t length;

		if (*end != '=') {
			return;
		}
		value = end + 1;
		length = strcspn(value, ", ");
		if (address >= 0x1dcf00000 && address < 0x1e2000000) {        /* QuartzCore's __DATA, __DATA_DIRTY */
			if (memchr(value, '.', length) != NULL) {
				*(volatile float *)(address + slide) = strtof(value, NULL);
			} else {
				*(volatile uint8_t *)(address + slide) = (uint8_t)strtoul(value, NULL, 0);
			}
		}
		at = value + length;
		at += *at == ',';
	}
}

static void
look_at_quartzcore(void)
{
	const char **switch_names = (const char **)(QC_SWITCH_NAMES + slide);
	const char **value_names = (const char **)(QC_VALUE_NAMES + slide);
	const uint8_t *switches = (const uint8_t *)(QC_SWITCHES + slide);
	const float *values = (const float *)(QC_VALUES + slide);
	bool (*prefers60)(void) = (bool (*)(void))(QC_PREFERS_60HZ_APT + slide);
	bool (*reduced)(void) = (bool (*)(void))(QC_REDUCED_VSYNC + slide);
	bool (*no_minimum)(void) = (bool (*)(void))dlsym(RTLD_DEFAULT, "CADeviceDisableMinimumFrameDuration");
	char line[900];
	int n;

	for (char **e = environ; *e != NULL; e++) {
		if (strncmp(*e, "CA_", 3) == 0) {
			say("ca-probe: environment %s\n", *e);
		}
	}
	n = snprintf(line, sizeof(line), "ca-probe: switches on:");
	for (unsigned i = 0; i < QC_SWITCH_COUNT && n < (int)sizeof(line) - 60; i++) {
		if (switches[i] != 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s(%u)", switch_names[i], i);
		}
	}
	say("%s\n", line);
	n = snprintf(line, sizeof(line), "ca-probe: values set:");
	for (unsigned i = 0; i < QC_VALUE_COUNT && n < (int)sizeof(line) - 60; i++) {
		if (values[i] != 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s(%u)=%g", value_names[i], i, (double)values[i]);
		}
	}
	say("%s\n", line);
	say("ca-probe: CADevicePrefers60HzAPT %d (supports APT %u, phone %u), CADisplayEnableReducedServerVsyncRender %d, "
	    "CADeviceDisableMinimumFrameDuration %d\n", prefers60(), *(const uint8_t *)(QC_SUPPORTS_APT + slide),
	    *(const uint8_t *)(QC_PHONE_CLASS + slide), reduced(), no_minimum != NULL ? no_minimum() : -1);
}

static void
look_at_display(unsigned index, id display)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(display), "_impl");
	uint8_t *impl, *server, *d, *timings, *range, *ranges_end;
	void **vt;
	uint64_t mode, flags9f2;
	Dl_info info;

	if (ivar == NULL || (impl = *(uint8_t **)((uint8_t *)display + ivar_getOffset(ivar))) == NULL ||
	    (server = *(uint8_t **)(impl + 0x40)) == NULL || (d = *(uint8_t **)(server + 0x60)) == NULL) {
		say("ca-probe: display %u: no server or display behind it\n", index);
		return;
	}
	vt = *(void ***)d;
	if ((uintptr_t)vt != QC_CLCD_VTABLE + (uintptr_t)slide) {
		say("ca-probe: display %u: not an AppleCLCDDisplay (vtable %p, %s); fields not read\n", index, (void *)vt,
		    dladdr(vt, &info) && info.dli_sname != NULL ? info.dli_sname : "?");
		return;
	}
	mode = *(uint64_t *)(d + 0x230);
	memcpy(&flags9f2, d + 0x9f2, sizeof(flags9f2));
	say("ca-probe: display %u: heartbeat %.6f s, refresh %.6f, hardware refresh %.6f; frame duration minimum %u, "
	    "hardware minimum %u, preferred %u (beats)\n", index, ((double (*)(void *))vt[VT_HEARTBEAT / 8])(d),
	    ((double (*)(void *))vt[VT_REFRESH / 8])(d), ((double (*)(void *))vt[VT_HARDWARE_REFRESH / 8])(d),
	    ((uint32_t (*)(void *))vt[VT_MIN_FRAME / 8])(d), ((uint32_t (*)(void *))vt[VT_HW_MIN_FRAME / 8])(d),
	    ((uint32_t (*)(void *))vt[VT_PREFERRED_FRAME / 8])(d));
	say("ca-probe: display %u: flags@0x220 %#x, mode %#llx (%.3f Hz%s), @0x878 %u, @0x9f2 %#llx, timings ready %u\n",
	    index, *(uint32_t *)(d + 0x220), mode, (double)((mode >> 29) & 0x1ffffff) / 65536,
	    (int64_t)mode < 0 && mode != ~0ULL ? ", VRR" : "", *(uint32_t *)(d + 0x878), flags9f2, *(uint8_t *)(d + 0x4b4));
	if ((timings = *(uint8_t **)(d + 0x4c0)) != NULL) {
		say("ca-probe: display %u: timings: flags %#x, heartbeat %u ticks, switch at %llu, server frame interval %u then %u\n",
		    index, *(volatile uint32_t *)timings, *(volatile uint32_t *)(timings + 4),
		    *(volatile unsigned long long *)(timings + 0x10), *(volatile uint32_t *)(timings + 0x1c),
		    *(volatile uint32_t *)(timings + 0x20));
	}
	range = *(uint8_t **)(d + 0x938);
	ranges_end = *(uint8_t **)(d + 0x940);
	say("ca-probe: display %u: %ld frame-interval ranges registered\n", index, range != NULL ? (long)(ranges_end - range) / 0x18 : 0L);
	for (unsigned i = 0; range != NULL && range < ranges_end && i < 12; range += 0x18, i++) {
		say("ca-probe:   range %u: id %#llx, %u %u %u, flags %#x\n", i, *(unsigned long long *)range,
		    *(uint32_t *)(range + 8), *(uint32_t *)(range + 0xc), *(uint32_t *)(range + 0x10), range[0x14]);
	}
	say("ca-probe: display %u: server: next render %g, vsyncs: last %llu, @0x2d8 %llu, @0x308 %u, @0x30d %#x\n", index,
	    *(double *)(server + 0x2b0), *(unsigned long long *)(server + 0x2e0), *(unsigned long long *)(server + 0x2d8),
	    *(uint32_t *)(server + 0x308), server[0x30d]);
	{
		/* the framebuffer as CoreAnimation has it (display + 0x318) and its frame info: what the server set up */
		void *fb = *(void **)(d + 0x318);
		const char **(*supported)(void *) = (const char **(*)(void *))dlsym(RTLD_DEFAULT, "IOMobileFramebufferSupportedFrameInfo");
		const char **tags = fb != NULL && supported != NULL ? supported(fb) : NULL;
		char line[400];
		int n = snprintf(line, sizeof(line), "ca-probe: display %u: framebuffer %p, vsync source %p, frame info source %p; tags:",
		    index, fb, *(void **)(server + 0x270), *(void **)(server + 0x2a8));

		for (unsigned i = 0; tags != NULL && tags[i] != NULL && i < 16 && n < (int)sizeof(line) - 40; i++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", tags[i]);
		}
		say("%s%s\n", line, tags == NULL ? " (none)" : "");
	}
}

static void *
probe(void *unused)
{
	(void)unused;
	Class window_server = objc_getClass("CAWindowServer");
	SEL running = sel_registerName("serverIfRunning"), displays = sel_registerName("displays");
	id server = nil;

	for (unsigned tenths = 0; tenths < 300; tenths++) {
		if (server == nil && window_server != Nil) {
			server = ((id (*)(Class, SEL))objc_msgSend)(window_server, running);
		}
		if (server != nil && pokes != NULL) {
			poke_all();
		}
		if (tenths == 100 && looks != 0) {
			break;
		}
		usleep(100000);
	}
	if (pokes != NULL) {
		say("ca-probe: poked %s\n", pokes);
	}
	for (unsigned look = 0; look < looks; look++) {
		CFArrayRef list;

		say("ca-probe: look %u\n", look + 1);
		look_at_quartzcore();
		if (server == nil && window_server != Nil) {
			server = ((id (*)(Class, SEL))objc_msgSend)(window_server, running);
		}
		list = server != nil ? (CFArrayRef)((id (*)(id, SEL))objc_msgSend)(server, displays) : NULL;
		for (CFIndex i = 0; list != NULL && i < CFArrayGetCount(list); i++) {
			look_at_display((unsigned)i, (id)CFArrayGetValueAtIndex(list, i));
		}
		if (list == NULL) {
			say("ca-probe: no window server yet\n");
		}
		if (pokes != NULL && look < 6) {
			poke_all();
		}
		sleep(5);
	}
	return NULL;
}

void
ca_probe_start(const char *arguments, void (*printer)(const char *format, ...))
{
	const char *at;
	pthread_t thread;
	bool found = false;

	say = printer;
	if ((at = strstr(arguments, "ca_probe=")) != NULL) {
		looks = (unsigned)strtoul(at + 9, NULL, 10);
	}
	if ((at = strstr(arguments, "ca_poke=")) != NULL) {
		pokes = at + 8;
	}
	if (looks == 0 && pokes == NULL) {
		return;
	}
	/* (CADevicePrefers60HzAPT is not exported: the slide is the shared cache's, QuartzCore's image's) */
	for (uint32_t i = 0; i < _dyld_image_count() && !found; i++) {
		const char *name = _dyld_get_image_name(i);
		size_t length = name != NULL ? strlen(name) : 0;

		if (length >= 11 && strcmp(name + length - 11, "/QuartzCore") == 0) {
			slide = _dyld_get_image_vmaddr_slide(i);
			found = true;
		}
	}
	if (!found) {
		say("ca-probe: QuartzCore is not loaded\n");
		return;
	}
	if (strcmp(*(const char **)(QC_SWITCH_NAMES + slide), "CA_COLOR_FLUSH") != 0 ||
	    strcmp(((const char **)(QC_SWITCH_NAMES + slide))[119], "CA_FORCE_120HZ") != 0) {
		say("ca-probe: QuartzCore's tables are not where 20F66 has them: nothing done\n");
		return;
	}
	say("ca-probe: QuartzCore slid by %#lx; %u looks%s%s\n", (long)slide, looks, pokes != NULL ? ", poking " : "",
	    pokes != NULL ? pokes : "");
	pthread_create(&thread, NULL, probe, NULL);
	pthread_detach(thread);
}
