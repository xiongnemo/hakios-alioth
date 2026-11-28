/*
 * libhakios.at.dylib: inside assistivetouchd (etc/inject.conf), AssistiveTouch's Home.
 *
 * (2026-11-23, WEEK-1121 N15) The menu's Home did nothing: AccessibilityPhysicalInteraction's -[AXPISystemActionHelper
 * pressHomeButtonDown] / -liftHomeButtonUp (and -activateHomeButton, which is the one, then the other) all come to
 * -_handleHomeButtonDispatch:(BOOL down), which presses Home through an IOHIDUserDevice of its own made by -warm
 * (IOHIDUserDeviceCreate). This kernel has no IOHIDResource, so there is no such device: assistivetouchd says
 * "_homeButtonUserDevice expectedly nil. Need to call warm" and nothing happens (runs 141613, 142232).
 *
 * With boot-arg at_home=1, while that device is nil, the press goes where `rwtouch button home` sends it and where it
 * works: /dev/hakios-touch, HAKIOS_BUTTON_HOME down and up (the node is 0666: bsd/dev/hakios.input.c). Where the device
 * exists, and without the boot-arg, the method is left as it is.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "hakios.input.h"

typedef void (*dispatch_imp)(id, SEL, bool);
static dispatch_imp original_dispatch;
static Ivar device_ivar;

static bool
boot_arg_on(const char *name, bool otherwise)
{
	char args[1100] = "";
	size_t n = sizeof(args) - 1, len = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0) {
		return otherwise;
	}
	for (char *p = strstr(args, name); p != NULL; p = strstr(p + 1, name)) {
		if ((p == args || p[-1] == ' ') && p[len] == '=') {
			return p[len + 1] != '0';
		}
	}
	return otherwise;                       /* (2026-11-26, 精简) absent: the caller's default */
}

static void
home_dispatch(id self, SEL _cmd, bool down)
{
	static unsigned said;
	struct hakios_touch t;
	int fd;

	if (device_ivar != NULL && object_getIvar(self, device_ivar) != nil) {
		original_dispatch(self, _cmd, down);            /* (a real device of its own: as iOS does it) */
		return;
	}
	memset(&t, 0, sizeof(t));
	t.type = down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP;
	t.finger = HAKIOS_BUTTON_HOME;
	fd = open("/dev/hakios-touch", O_WRONLY);
	if (fd < 0 || write(fd, &t, sizeof(t)) != (ssize_t)sizeof(t)) {
		fprintf(stderr, "libhakios.at: Home %s not sent: /dev/hakios-touch: %s\n", down ? "down" : "up", strerror(errno));
	} else if (said++ < 8) {
		fprintf(stderr, "libhakios.at: AssistiveTouch's Home %s sent as the home button (at_home)\n", down ? "down" : "up");
	}
	if (fd >= 0) {
		close(fd);
	}
}

/* (once the class is there: at once if assistivetouchd links its framework, else when a later image brings it) */
static bool
install(void)
{
	static bool done;
	Class cls;
	Method m;

	if (done || (cls = objc_getClass("AXPISystemActionHelper")) == Nil) {
		return done;
	}
	done = true;
	m = class_getInstanceMethod(cls, sel_registerName("_handleHomeButtonDispatch:"));
	if (m == NULL) {
		fprintf(stderr, "libhakios.at: no -_handleHomeButtonDispatch: here: AssistiveTouch's Home left as it is\n");
		return true;
	}
	device_ivar = class_getInstanceVariable(cls, "_homeButtonUserDevice");
	original_dispatch = (dispatch_imp)method_setImplementation(m, (IMP)home_dispatch);
	fprintf(stderr, "libhakios.at: AssistiveTouch's Home goes to the home button when it has no device of its own "
	    "(at_home)%s\n", device_ivar == NULL ? " -- no _homeButtonUserDevice to look at: always" : "");
	return true;
}

static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh;
	(void)slide;
	(void)install();
}

__attribute__((constructor))
static void
at_init(void)
{
	if (!boot_arg_on("at_home", true)) {
		return;
	}
	if (!install()) {
		/* (called at once for every image already there, then for each one loaded later) */
		_dyld_register_func_for_add_image(image_added);
	}
}
