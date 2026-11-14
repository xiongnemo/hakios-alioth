/*
 * libhakios.ui.dylib: code of ours inside every application and app extension -- build.sh's INJECT names them
 * "*.app *.appex", the programs in a bundle of either kind (the kernel's exec_inject_env(), xnu-device.patch).
 *
 * CoreImage draws with Metal, and where there is no Metal device with OpenGL ES, without asking whether that
 * works. Outside the window server this phone has neither (the GPU is driven from inside backboardd only,
 * src/eagl.c): iOS's own EAGLContext gives CoreImage no table of functions (-getMacroContextPrivate is NULL),
 * and CI::GLContext::init() calls the first entry of that table all the same -- pc 0. So SpringBoard died, 31
 * times in a row, once Bold Text was turned on (2026-10-30): UIKit then has CoreUI draw symbol images with their
 * style presets, in a context of +[CUIShapeEffectStack sharedCIContext]. Here a context CoreImage would make on
 * an OpenGL ES that does not work is no context at all: CoreUI gives back no styled image for it, and UIKit keeps
 * the image it had (-[UIImage _imageWithStylePresets:tintColor:traitCollection:] goes on with a nil one).
 *
 * The compass. On an iPhone X locationd's heading (CLCompass7) is made from the motion coprocessor's "device
 * motion" HID service, vendor-defined events in Apple's own format; this phone has no such coprocessor, but its
 * sensor DSP fuses the accelerometer, gyroscope and magnetometer into a rotation vector itself (the kernel's
 * kern.hakios.rotv, bsd/dev/hakios.slpi.c). So CLLocationManager's heading is answered here, in the application:
 * +headingAvailable is YES, -startUpdatingHeading leases the rotation vector (kern.hakios.rotv_lease) and
 * gives the delegate -locationManager:didUpdateHeading: twenty times a second, the heading of the top of the
 * device -- of its back when it is held upright -- with the DSP's own accuracy; -heading is the last one.
 * locationd is not asked for it. trueHeading: the magnetic heading turned by the declination where the phone is
 * (NOAA's World Magnetic Model 2025, wmm2025.h), from the location manager's own last location; without one, the
 * magnetic heading, as before. Not yet: the raw field (x, y, z are 0).
 *
 * Metal, for the applications the owner allows and no others (metal_for_allowed(), boot-arg metal_apps=): the port's
 * Metal driver registered in the application before its own code runs.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <math.h>
#include <os/lock.h>
#include <stdarg.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wmm2025.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE              0x180000000UL
#define CI_CONTEXT_WITH_EAGL    0x1949bd640UL   /* +[CIContext(Internal) internalContextWithEAGLContext:options:] (20F66) */
#define EAGL_API_GLES2          2

static uintptr_t slide;
static id (*orig_context_with_eagl)(id, SEL, id, id);

static void
say(const char *format, ...)
{
	static int console = -1;
	char line[512];
	int k = snprintf(line, sizeof(line), "hakios: %s[%d]: ", getprogname(), getpid());
	va_list arguments;

	va_start(arguments, format);
	k += vsnprintf(line + k, sizeof(line) - (size_t)k, format, arguments);
	va_end(arguments);
	if (k > (int)sizeof(line) - 1) {
		k = (int)sizeof(line) - 1;
	}
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)k);
}

/* Whether an EAGLContext gives CoreImage the table of functions it calls OpenGL ES through. */
static bool
has_table(id context)
{
	return context != nil &&
	    ((void *(*)(id, SEL))objc_msgSend)(context, sel_registerName("getMacroContextPrivate")) != NULL;
}

/* ... and whether one that CoreImage makes itself would (it makes one when it is given none): asked once */
static bool
gl_works(void)
{
	static dispatch_once_t once;
	static bool works;

	dispatch_once(&once, ^{
		Class eagl = objc_getClass("EAGLContext");
		id context = eagl ? ((id (*)(id, SEL))objc_msgSend)((id)eagl, sel_registerName("alloc")) : nil;

		if (context != nil) {
			context = ((id (*)(id, SEL, unsigned long))objc_msgSend)(context, sel_registerName("initWithAPI:"),
			    EAGL_API_GLES2);
		}
		works = has_table(context);
		if (context != nil) {
			((void (*)(id, SEL))objc_msgSend)(context, sel_registerName("release"));
		}
		if (!works) {
			say("CoreImage: no Metal and no OpenGL ES that works here: its contexts are nil\n");
		}
	});
	return works;
}

static bool app_gl;                     /* this application draws OpenGL ES of ours (app_opengl_es()) */
static bool app_ci_off;                 /* boot-arg appci=0: CoreImage kept off an application's OpenGL ES of ours */

static id
context_with_eagl(id self, SEL selector, id eagl, id options)
{
	/*
	 * An application with OpenGL ES of ours: its contexts have a table of functions and a sharegroup, CoreImage
	 * may draw with them (GPU Check's coreimage.* tests; 2026-11-01, before that it was kept off them, untried).
	 */
	if ((app_gl && app_ci_off) || (eagl != nil ? !has_table(eagl) : !gl_works())) {
		return nil;
	}
	return orig_context_with_eagl(self, selector, eagl, options);
}

/* ---- OpenGL ES for an application of its own (a game) ------------------------------------------------------ */

#define HAKIOS_GL               "/redwood/lib/libhakios.gl.dylib"

/*
 * The main program's image: the one of type MH_EXECUTE. Not image 0 -- with libraries inserted that is the first of
 * them, this one ("/redwood/lib/libhakios.ui.dylib", seen 2026-10-31 in every application).
 */
static uint32_t
main_image(void)
{
	for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
		const struct mach_header *header = _dyld_get_image_header(i);

		if (header != NULL && header->filetype == MH_EXECUTE) {
			return i;
		}
	}
	return 0;
}

/*
 * Programs told another model in hw.machine (sysctlbyname, sysctl, uname) -- the program's own calls only, the
 * system's libraries in it still see the iPhone X. Minecraft 1.2 lays its controls out for the iPhone X's notch
 * when it sees iPhone10,3 or 10,6, and cuts its top row off doing so on a real iPhone X too (Mojang's MCPE-30223,
 * MCPE-31058): as an iPhone 8 Plus (the same A11) it lays them out on the whole screen. Boot-arg appmodel=0: none.
 */
static const struct {
	const char *program, *model;
} models[] = {
	{ "minecraftpe", "iPhone10,2" },
};

static const char *model_given;         /* this program's, or NULL */
static uintptr_t model_text, model_text_size;   /* where the program's own code is */

static void
model_decide(void)
{
	const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(main_image());
	const struct load_command *command;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *model = NULL;

	for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
		if (getprogname() != NULL && strcmp(getprogname(), models[i].program) == 0) {
			model = models[i].model;
		}
	}
	if (model == NULL || header == NULL || header->magic != MH_MAGIC_64) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "appmodel=0") != NULL) {
		say("hw.machine: as it is, with appmodel=0\n");
		return;
	}
	command = (const struct load_command *)(header + 1);
	for (uint32_t i = 0; i < header->ncmds; i++) {
		const struct segment_command_64 *segment = (const struct segment_command_64 *)command;

		if (command->cmd == LC_SEGMENT_64 && strcmp(segment->segname, "__TEXT") == 0) {
			model_text = (uintptr_t)header;
			model_text_size = segment->vmsize;
		}
		command = (const struct load_command *)((const char *)command + command->cmdsize);
	}
	if (model_text_size != 0) {
		model_given = model;
		say("hw.machine: %s for the program's own code (its iPhone X layout does not fit)\n", model);
	}
}

static bool
model_for(const void *caller)
{
	return model_given != NULL && (uintptr_t)caller - model_text < model_text_size;
}

static int
model_answer(void *old, size_t *oldlen)
{
	size_t need = strlen(model_given) + 1;

	if (oldlen == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (old != NULL && *oldlen < need) {
		*oldlen = need;
		errno = ENOMEM;
		return -1;
	}
	if (old != NULL) {
		memcpy(old, model_given, need);
	}
	*oldlen = need;
	return 0;
}

static int
model_sysctlbyname(const char *name, void *old, size_t *oldlen, void *new, size_t newlen)
{
	if (new == NULL && name != NULL && strcmp(name, "hw.machine") == 0 && model_for(__builtin_return_address(0))) {
		return model_answer(old, oldlen);
	}
	return sysctlbyname(name, old, oldlen, new, newlen);
}

static int
model_sysctl(int *mib, u_int count, void *old, size_t *oldlen, void *new, size_t newlen)
{
	if (new == NULL && count == 2 && mib[0] == CTL_HW && mib[1] == HW_MACHINE && model_for(__builtin_return_address(0))) {
		return model_answer(old, oldlen);
	}
	return sysctl(mib, count, old, oldlen, new, newlen);
}

static int
model_uname(struct utsname *name)
{
	int result = uname(name);

	if (result == 0 && model_for(__builtin_return_address(0))) {
		strlcpy(name->machine, model_given, sizeof(name->machine));
	}
	return result;
}

/*
 * CoreVideo's OpenGL ES texture cache, for a program with OpenGL ES of its own (app_gl): what it is asked and says,
 * the first times, and a cache asked for without a context made for the current one. Terraria's Everyplay asserted
 * on "CVOpenGLESTextureCacheCreate failed: -6661" (kCVReturnInvalidArgument: CoreVideo 20F66 0x1a1e83998 checks the
 * context, the place for the answer, and that both attribute arguments are dictionaries).
 */
extern int CVOpenGLESTextureCacheCreate(const void *allocator, const void *cache_attributes, void *context,
    const void *texture_attributes, void **cache) __attribute__((weak_import));
static bool app_gl;

static int
cv_cache_create(const void *allocator, const void *cache_attributes, void *context, const void *texture_attributes,
    void **cache)
{
	static unsigned told;
	void *given = context;
	int result;

	if (app_gl && context == NULL) {
		Class class = objc_getClass("EAGLContext");
		void *(*last)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "hakios_gl_app_context");

		context = class ? ((void *(*)(Class, SEL))objc_msgSend)(class, sel_registerName("currentContext")) : NULL;
		if (context == NULL && last != NULL) {
			context = last();       /* (the one made current last: libhakios.gl.dylib's) */
		}
	}
	result = CVOpenGLESTextureCacheCreate(allocator, cache_attributes, context, texture_attributes, cache);
	if (app_gl && told++ < 6) {
		say("CVOpenGLESTextureCacheCreate(attributes %p, context %p%s, texture attributes %p): %d\n", cache_attributes,
		    given, given != context ? context ? ", made the current one" : ", and none is current" : "", texture_attributes,
		    result);
	}
	return result;
}

/* (dyld's interposing: this library is inserted, DYLD_INSERT_LIBRARIES) */
__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposed[] = {
	{ (const void *)model_sysctlbyname, (const void *)sysctlbyname },
	{ (const void *)model_sysctl, (const void *)sysctl },
	{ (const void *)model_uname, (const void *)uname },
	{ (const void *)cv_cache_create, (const void *)CVOpenGLESTextureCacheCreate },
};

/* Whether the main program itself names OpenGLES among its libraries -- a game does; a framework might as well */
static bool
main_links_opengles(void)
{
	const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(main_image());
	const struct load_command *command;

	if (header == NULL || header->magic != MH_MAGIC_64) {
		return false;
	}
	command = (const struct load_command *)(header + 1);
	for (uint32_t i = 0; i < header->ncmds; i++) {
		if (command->cmd == LC_LOAD_DYLIB || command->cmd == LC_LOAD_WEAK_DYLIB) {
			const struct dylib_command *library = (const struct dylib_command *)command;

			if (strstr((const char *)library + library->dylib.name.offset, "/OpenGLES.framework/OpenGLES") != NULL) {
				return true;
			}
		}
		command = (const struct load_command *)((const char *)command + command->cmdsize);
	}
	return false;
}

/*
 * The window server's OpenGL ES library (libhakios.gl.dylib: eagl.c over Mesa's freedreno, the GPU) loaded into an
 * application of the user's that links OpenGLES, where it gives EAGLContexts of their own and hands a CAEAGLLayer's
 * frames over as IOSurfaces. Not into the system's own (/Applications, /System): there CoreImage and the like would
 * start drawing with it. Boot-arg appgl=0: none. (Until the GPU driver keeps processes apart, an application could
 * reach the others' GPU memory: the plan's B4.)
 */
static void gl_lazy_hooks(void);
static IMP fr_hook(Class cls, const char *name, IMP imp);
static bool gl_lazy_wanted;             /* an EAGLContext asked for loads the library (gl_lazy_hooks()) */

static void
app_gl_load(const char *why)
{
	static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;

	os_unfair_lock_lock(&lock);
	if (!app_gl) {
		if (dlopen(HAKIOS_GL, RTLD_NOW) == NULL) {
			say("%s: %s\n", HAKIOS_GL, dlerror());
		} else {
			app_gl = true;
			say("OpenGL ES of its own for %s (%s): %s (the GPU, as the window server's)\n",
			    _dyld_get_image_name(main_image()), why, HAKIOS_GL);
		}
	}
	os_unfair_lock_unlock(&lock);
}

/*
 * B1 (2026-11-08): an application whose main program does not link OpenGLES gets it too, at the first EAGLContext it
 * asks for -- an engine that is an embedded framework of its own (Unity's UnityFramework, loaded by the program at
 * run time), or SpriteKit, SceneKit, GLKit and CoreImage drawing with OpenGL ES where there is no Metal. Before, such a
 * context was Apple's, over a Metal there is not: nil, and nothing drawn. Boot-arg appgl_lazy=1 (off by default until
 * measured): without it, the main program's own OpenGLES only, as before.
 */
static void
app_opengl_es(void)
{
	const char *path = _dyld_get_image_name(main_image());
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (path == NULL || strncmp(path, "/Applications/", 14) == 0 || strncmp(path, "/System/", 8) == 0 ||
	    strncmp(path, "/private/var/staged_system_apps/", 32) == 0) {
		return;
	}
	(void)sysctlbyname("kern.bootargs", arguments, &size, NULL, 0);
	if (strstr(arguments, "appgl=0") != NULL) {
		if (main_links_opengles()) {
			say("OpenGL ES of its own: not with appgl=0\n");
		}
		return;
	}
	if (access(HAKIOS_GL, R_OK) != 0) {
		return;                 /* (an image without the library) */
	}
	app_ci_off = strstr(arguments, "appci=0") != NULL;
	if (main_links_opengles()) {
		app_gl_load("its program links OpenGLES");
	} else if (strstr(arguments, "appgl_lazy=1") != NULL) {
		gl_lazy_wanted = true;
		gl_lazy_hooks();        /* (OpenGLES may be in already; else the image callback, compass_hooks(), comes back) */
	}
}

/*
 * EAGLContext's initialisers while the library is not loaded: the first one called loads it -- its constructor
 * puts its own initialisers in their place -- and the call goes on to what EAGLContext has then (Apple's, if the
 * library could not be loaded).
 */
static IMP gl_lazy_api_original, gl_lazy_share_original, gl_lazy_props_original;

static IMP
gl_lazy_now(id self, SEL selector, IMP lazy, IMP original)
{
	Method m;
	IMP now;

	app_gl_load("an EAGLContext asked for");
	m = class_getInstanceMethod(object_getClass(self), selector);
	now = m != NULL ? method_getImplementation(m) : NULL;
	return now == NULL || now == lazy ? original : now;
}

static id
gl_lazy_api(id self, SEL selector, unsigned long api)
{
	IMP now = gl_lazy_now(self, selector, (IMP)gl_lazy_api, gl_lazy_api_original);

	return ((id (*)(id, SEL, unsigned long))now)(self, selector, api);
}

static id
gl_lazy_share(id self, SEL selector, unsigned long api, id sharegroup)
{
	IMP now = gl_lazy_now(self, selector, (IMP)gl_lazy_share, gl_lazy_share_original);

	return ((id (*)(id, SEL, unsigned long, id))now)(self, selector, api, sharegroup);
}

static id
gl_lazy_props(id self, SEL selector, unsigned long api, id properties)
{
	IMP now = gl_lazy_now(self, selector, (IMP)gl_lazy_props, gl_lazy_props_original);

	return ((id (*)(id, SEL, unsigned long, id))now)(self, selector, api, properties);
}

/* (once, when OpenGLES's EAGLContext is there and the library is wanted lazily) */
static void
gl_lazy_hooks(void)
{
	static int done;
	Class context;

	if (!gl_lazy_wanted || app_gl || __atomic_load_n(&done, __ATOMIC_ACQUIRE) ||
	    (context = objc_getClass("EAGLContext")) == Nil || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) {
		return;
	}
	gl_lazy_api_original = fr_hook(context, "initWithAPI:", (IMP)gl_lazy_api);
	gl_lazy_share_original = fr_hook(context, "initWithAPI:sharegroup:", (IMP)gl_lazy_share);
	gl_lazy_props_original = fr_hook(context, "initWithAPI:properties:", (IMP)gl_lazy_props);
	say("OpenGL ES of its own: at the first EAGLContext asked for (its program does not link OpenGLES)%s\n",
	    gl_lazy_api_original != NULL && gl_lazy_share_original != NULL ? "" : " -- AN INITIALISER IS MISSING");
}

/* ---- the compass ------------------------------------------------------------------------------------------- */

struct hakios_rotv {                    /* as bsd/dev/hakios.slpi.c makes it */
	uint32_t sequence, count, value[6];
	int32_t status;
	uint32_t on;
	uint64_t timestamp;
};

enum { H_MAGNETIC, H_TRUE, H_ACCURACY, H_X, H_Y, H_Z, H_TIME, H_COUNT };

static Class heading_class;             /* HakiosHeading: a CLHeading whose values are ours */
static ptrdiff_t heading_values;        /* where its double[H_COUNT] is */

static double *
values_of(id heading)
{
	return (double *)((char *)heading + heading_values);
}

static double get_magnetic(id self, SEL cmd) { (void)cmd; return values_of(self)[H_MAGNETIC]; }
static double get_true(id self, SEL cmd) { (void)cmd; return values_of(self)[H_TRUE]; }
static double get_accuracy(id self, SEL cmd) { (void)cmd; return values_of(self)[H_ACCURACY]; }
static double get_x(id self, SEL cmd) { (void)cmd; return values_of(self)[H_X]; }
static double get_y(id self, SEL cmd) { (void)cmd; return values_of(self)[H_Y]; }
static double get_z(id self, SEL cmd) { (void)cmd; return values_of(self)[H_Z]; }

static id
get_timestamp(id self, SEL cmd)
{
	(void)cmd;
	return ((id (*)(id, SEL, double))objc_msgSend)((id)objc_getClass("NSDate"),
	    sel_registerName("dateWithTimeIntervalSinceReferenceDate:"), values_of(self)[H_TIME]);
}

static id
copy_heading(id self, SEL cmd, void *zone)
{
	(void)cmd;
	(void)zone;
	return ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
}

static bool
make_heading_class(void)
{
	Class base = objc_getClass("CLHeading");
	Class cls;

	if (heading_class != Nil) {
		return true;
	}
	if (base == Nil || (cls = objc_allocateClassPair(base, "HakiosHeading", 0)) == Nil) {
		return false;
	}
	class_addIvar(cls, "hakios_values", sizeof(double) * H_COUNT, 3, "[7d]");
	class_addMethod(cls, sel_registerName("magneticHeading"), (IMP)get_magnetic, "d@:");
	class_addMethod(cls, sel_registerName("trueHeading"), (IMP)get_true, "d@:");
	class_addMethod(cls, sel_registerName("headingAccuracy"), (IMP)get_accuracy, "d@:");
	class_addMethod(cls, sel_registerName("x"), (IMP)get_x, "d@:");
	class_addMethod(cls, sel_registerName("y"), (IMP)get_y, "d@:");
	class_addMethod(cls, sel_registerName("z"), (IMP)get_z, "d@:");
	class_addMethod(cls, sel_registerName("timestamp"), (IMP)get_timestamp, "@@:");
	class_addMethod(cls, sel_registerName("copyWithZone:"), (IMP)copy_heading, "@@:^v");
	objc_registerClassPair(cls);
	heading_values = ivar_getOffset(class_getInstanceVariable(cls, "hakios_values"));
	heading_class = cls;
	return true;
}

/*
 * The heading from the rotation vector (x, y, z, w: device axes to east, north, up, as Android's): the device's
 * Y axis (its top) in the world is the rotation matrix's second column, and its heading atan2(east, north); held
 * upright, with that axis nearly straight up, the heading of the back of the device (-Z) instead. In degrees from
 * magnetic north, clockwise; NAN when the reading says nothing.
 */
static double
heading_from(const struct hakios_rotv *r, double *accuracy)
{
	float q[6] = { 0 };
	double x, y, z, w, e, n, up;

	if (r->count < 3) {
		return NAN;
	}
	memcpy(q, r->value, sizeof(q));
	x = q[0], y = q[1], z = q[2];
	w = r->count >= 4 ? q[3] : sqrt(fmax(0, 1 - x * x - y * y - z * z));
	e = 2 * (x * y - z * w);                /* R[0][1] */
	n = 1 - 2 * (x * x + z * z);            /* R[1][1] */
	up = 2 * (y * z + x * w);               /* R[2][1] */
	if (fabs(up) > 0.8) {
		e = -2 * (x * z + y * w);       /* -R[0][2] */
		n = -2 * (y * z - x * w);       /* -R[1][2] */
	}
	if (fabs(e) + fabs(n) < 1e-6) {
		return NAN;
	}
	if (r->count >= 5 && q[4] > 0 && q[4] < 3.2) {
		*accuracy = q[4] * 180 / M_PI;  /* (the DSP's estimate, in radians) */
	} else {
		*accuracy = r->status >= 3 ? 5 : r->status == 2 ? 15 : r->status == 1 ? 30 : -1;
	}
	return fmod(atan2(e, n) * 180 / M_PI + 360, 360);
}

#define WATCHERS 8
static struct watcher {
	id manager;                     /* retained while it is watched */
	dispatch_source_t timer;
	uint32_t sequence;
	double last;
	uint64_t leased;                /* when the lease was written last, in ns */
	id heading;                     /* the last one, retained */
} watchers[WATCHERS];

static struct watcher *
watcher_of(id manager)
{
	for (int i = 0; i < WATCHERS; i++) {
		if (watchers[i].manager == manager) {
			return &watchers[i];
		}
	}
	return NULL;
}

/*
 * The declination for trueHeading: from the manager's last location (-[CLLocationManager location]: what it was
 * given, or the system's last fix for an application that may have it), worked out again when the phone has moved
 * some 10 km or every 10 minutes, and looked for once a second at most. NAN: no location (trueHeading = magnetic).
 */
struct coordinate { double latitude, longitude; };      /* (CLLocationCoordinate2D) */

static double
declination_for(id manager)
{
	static double latitude = 1000, longitude, height, declination = NAN;
	static uint64_t asked, worked;
	static int shown;
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	id location;
	struct coordinate c;
	double accuracy, year;

	if (asked != 0 && now - asked < 1000000000ull) {
		return declination;
	}
	asked = now;
	location = ((id (*)(id, SEL))objc_msgSend)(manager, sel_registerName("location"));
	if (location == nil) {
		return declination;
	}
	accuracy = ((double (*)(id, SEL))objc_msgSend)(location, sel_registerName("horizontalAccuracy"));
	c = ((struct coordinate (*)(id, SEL))objc_msgSend)(location, sel_registerName("coordinate"));
	if (accuracy < 0 || !isfinite(c.latitude) || !isfinite(c.longitude)) {
		return declination;
	}
	if (!isnan(declination) && fabs(c.latitude - latitude) < 0.1 && fabs(c.longitude - longitude) < 0.1 &&
	    now - worked < 600000000000ull) {
		return declination;
	}
	latitude = c.latitude;
	longitude = c.longitude;
	height = ((double (*)(id, SEL))objc_msgSend)(location, sel_registerName("altitude")) / 1000;
	year = 2001 + ((double (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSDate"),
	    sel_registerName("timeIntervalSinceReferenceDate")) / (365.2425 * 86400);
	declination = wmm_declination(latitude, longitude, isfinite(height) ? height : 0, year);
	worked = now;
	if (shown++ < 4) {
		say("compass: declination %.2f degrees at %.2f, %.2f (WMM 2025, %.2f): true north is the magnetic heading + that\n",
		    declination, latitude, longitude, year);
	}
	return declination;
}

static void
tick(struct watcher *w)
{
	static int shown;
	struct hakios_rotv r;
	size_t length = sizeof(r);
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);         /* (dispatch_time() is in ticks, not ns) */
	double accuracy = -1, heading, filter, declination;
	id delegate, h;
	SEL did = sel_registerName("locationManager:didUpdateHeading:");

	if (w->leased == 0 || now - w->leased > 1000000000ull) {
		int one = 1;

		sysctlbyname("kern.hakios.rotv_lease", NULL, NULL, &one, sizeof(one));
		w->leased = now;
	}
	if (sysctlbyname("kern.hakios.rotv", &r, &length, NULL, 0) != 0 || r.sequence == w->sequence) {
		return;
	}
	w->sequence = r.sequence;
	heading = heading_from(&r, &accuracy);
	if (shown < 8 || r.sequence % 400 == 0) {
		float q[4];

		memcpy(q, r.value, sizeof(q));
		say("compass: reading %u: %u floats (%.3f %.3f %.3f %.3f), status %d: heading %.1f, accuracy %.1f\n",
		    r.sequence, r.count, q[0], q[1], q[2], q[3], r.status, heading, accuracy);
		shown++;
	}
	if (isnan(heading)) {
		return;
	}
	filter = ((double (*)(id, SEL))objc_msgSend)(w->manager, sel_registerName("headingFilter"));
	if (w->heading != nil && filter > 0 && fabs(remainder(heading - w->last, 360)) < filter) {
		return;
	}
	w->last = heading;
	h = class_createInstance(heading_class, 0);
	values_of(h)[H_MAGNETIC] = heading;
	declination = declination_for(w->manager);
	values_of(h)[H_TRUE] = isnan(declination) ? heading : fmod(heading + declination + 360, 360);
	values_of(h)[H_ACCURACY] = accuracy;
	values_of(h)[H_TIME] = ((double (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSDate"),
	    sel_registerName("timeIntervalSinceReferenceDate"));
	if (w->heading != nil) {
		((void (*)(id, SEL))objc_msgSend)(w->heading, sel_registerName("release"));
	}
	w->heading = h;
	delegate = ((id (*)(id, SEL))objc_msgSend)(w->manager, sel_registerName("delegate"));
	if (delegate != nil && ((bool (*)(id, SEL, SEL))objc_msgSend)(delegate, sel_registerName("respondsToSelector:"), did)) {
		((void (*)(id, SEL, id, id))objc_msgSend)(delegate, did, w->manager, h);
		if (shown < 12) {
			say("compass: heading %.1f given to %s\n", heading, object_getClassName(delegate));
			shown++;
		}
	} else if (shown < 12) {
		say("compass: heading %.1f for nobody: delegate %s\n", heading, delegate ? object_getClassName(delegate) : "nil");
		shown++;
	}
}

static void
start_heading(id self, SEL cmd)
{
	struct watcher *w = watcher_of(self);

	(void)cmd;
	if (w != NULL || !make_heading_class()) {
		return;
	}
	w = watcher_of(nil);
	if (w == NULL) {
		say("compass: more than %d location managers want the heading; this one is not answered\n", WATCHERS);
		return;
	}
	w->manager = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
	w->sequence = 0;
	w->leased = 0;
	w->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(w->timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 20, NSEC_PER_MSEC * 5);
	dispatch_source_set_event_handler(w->timer, ^{ tick(w); });
	dispatch_resume(w->timer);
	say("compass: the heading for %p, from the sensor DSP's rotation vector\n", (void *)self);
}

static void
stop_heading(id self, SEL cmd)
{
	struct watcher *w = watcher_of(self);

	(void)cmd;
	if (w == NULL || self == nil) {
		return;
	}
	dispatch_source_cancel(w->timer);
	dispatch_release(w->timer);
	w->timer = NULL;
	w->manager = nil;
	((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
}

static id
last_heading(id self, SEL cmd)
{
	struct watcher *w = watcher_of(self);

	(void)cmd;
	return w != NULL ? w->heading : nil;
}

static bool
heading_available(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return true;
}

/* ---- CoreMotion's device motion ------------------------------------------------------------------------------ */
/*
 * CMMotionManager's device motion from the same rotation vector (the Compass application asks for it beside the
 * heading, a level or a game for it alone): CMDeviceMotion and CMAttitude made here, in the reference frame
 * X magnetic north, Z up (Y west), whatever frame was asked for -- the arbitrary ones may be any, and there is no
 * true north without a location. Gravity follows from the attitude; the rotation rate and the user's acceleration
 * are 0 for now (the gyroscope and the accelerometer are not read on their own yet).
 */
typedef struct { double x, y, z, w; } hk_quaternion;
typedef struct { double m11, m12, m13, m21, m22, m23, m31, m32, m33; } hk_matrix;
typedef struct { double x, y, z; } hk_vector;
typedef struct { hk_vector field; int accuracy; } hk_calibrated_field;

enum { A_QX, A_QY, A_QZ, A_QW, A_M, A_ROLL = A_M + 9, A_PITCH, A_YAW, A_COUNT };
enum { D_GX, D_GY, D_GZ, D_FX, D_FY, D_FZ, D_ACCURACY, D_HEADING, D_TIME, D_RX, D_RY, D_RZ, D_UX, D_UY, D_UZ, D_COUNT };

static Class attitude_class, motion_class, accel_class, gyro_class;
static ptrdiff_t attitude_values, motion_values, motion_attitude, accel_values, gyro_values;
/* CMAccelerometerData's and CMGyroData's: x, y, z, the time */
static double *v_of(id o) { return (double *)((char *)o + (object_getClass(o) == accel_class ? accel_values : gyro_values)); }
static hk_vector get_vector(id self, SEL c) { (void)c; double *v = v_of(self); return (hk_vector){ v[0], v[1], v[2] }; }
static double get_vector_time(id self, SEL c) { (void)c; return v_of(self)[3]; }

static double *a_of(id a) { return (double *)((char *)a + attitude_values); }
static double *d_of(id d) { return (double *)((char *)d + motion_values); }
static id *d_attitude(id d) { return (id *)((char *)d + motion_attitude); }

static hk_quaternion get_quaternion(id self, SEL c) { (void)c; double *v = a_of(self); return (hk_quaternion){ v[A_QX], v[A_QY], v[A_QZ], v[A_QW] }; }
/* (the rotation matrix the other way: reference to device, as Apple's pARk sample uses it -- the transpose of
 * the quaternion's own matrix, which composable-core-motion found of Apple's too) */
static hk_matrix
get_matrix(id self, SEL c)
{
	const double *a = a_of(self) + A_M;

	(void)c;
	return (hk_matrix){ a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8] };
}
static double get_roll(id self, SEL c) { (void)c; return a_of(self)[A_ROLL]; }
static double get_pitch(id self, SEL c) { (void)c; return a_of(self)[A_PITCH]; }
static double get_yaw(id self, SEL c) { (void)c; return a_of(self)[A_YAW]; }
static id get_attitude(id self, SEL c) { (void)c; return *d_attitude(self); }
static hk_vector get_gravity(id self, SEL c) { (void)c; double *v = d_of(self); return (hk_vector){ v[D_GX], v[D_GY], v[D_GZ] }; }
static hk_vector get_rate(id self, SEL c) { (void)c; double *v = d_of(self); return (hk_vector){ v[D_RX], v[D_RY], v[D_RZ] }; }
static hk_vector get_user(id self, SEL c) { (void)c; double *v = d_of(self); return (hk_vector){ v[D_UX], v[D_UY], v[D_UZ] }; }
static hk_calibrated_field get_field(id self, SEL c) { (void)c; double *v = d_of(self); return (hk_calibrated_field){ { v[D_FX], v[D_FY], v[D_FZ] }, (int)v[D_ACCURACY] }; }
static double get_motion_heading(id self, SEL c) { (void)c; return d_of(self)[D_HEADING]; }
static double get_motion_time(id self, SEL c) { (void)c; return d_of(self)[D_TIME]; }
static long get_sensor_location(id self, SEL c) { (void)self; (void)c; return 0; }
/* (private: Compass reads it; 0 uncalibrated .. 3 high, the field's accuracy plus one) */
static int get_calibration_level(id self, SEL c) { (void)c; return (int)d_of(self)[D_ACCURACY] + 1; }

static uint64_t stub_zero(id self, SEL c) { (void)self; (void)c; return 0; }
static double stub_double(id self, SEL c) { (void)self; (void)c; return 0; }
static void stub_void(id self, SEL c) { (void)self; (void)c; }

/*
 * Every method CMDeviceMotion or CMAttitude declares itself that ours do not answer: 0, nil or nothing. Theirs
 * read the class's internal storage, of which ours have none -- Compass died reading -magneticFieldCalibrationLevel
 * through it (2026-10-31). What is answered so is said once, to find what applications want.
 */
static void
neutralize(Class ours, Class theirs)
{
	unsigned count = 0, stubbed = 0;
	Method *list = class_copyMethodList(theirs, &count);
	char names[700] = "";
	size_t at = 0;

	for (unsigned i = 0; i < count; i++) {
		SEL s = method_getName(list[i]);
		const char *n = sel_getName(s);
		char ret[16];
		IMP imp;

		if (n[0] == '.' || strcmp(n, "dealloc") == 0 || strncmp(n, "init", 4) == 0 || strncmp(n, "encode", 6) == 0 ||
		    strcmp(n, "copyWithZone:") == 0 || strcmp(n, "description") == 0 || strcmp(n, "debugDescription") == 0) {
			continue;
		}
		if (class_getMethodImplementation(ours, s) != method_getImplementation(list[i])) {
			continue;       /* (ours already) */
		}
		method_getReturnType(list[i], ret, sizeof(ret));
		if (ret[0] == '{' || ret[0] == '(' || ret[0] == '[') {
			continue;       /* (a struct of a size of its own: left as it is) */
		}
		imp = ret[0] == 'v' ? (IMP)stub_void : ret[0] == 'f' || ret[0] == 'd' ? (IMP)stub_double : (IMP)stub_zero;
		class_addMethod(ours, s, imp, method_getTypeEncoding(list[i]));
		stubbed++;
		if (at < sizeof(names) - 60) {
			at += (size_t)snprintf(names + at, sizeof(names) - at, " %s", n);
		}
	}
	free(list);
	say("motion: %u methods of %s answered with nothing:%s\n", stubbed, class_getName(theirs), names);
}

/* the Euler angles of a rotation matrix as CMAttitude has them (roll about Y, pitch about X, yaw about Z) */
static void
euler_from(double *a)
{
	const double *m = a + A_M;      /* m11 m12 m13 m21 m22 m23 m31 m32 m33 */

	a[A_PITCH] = asin(fmax(-1, fmin(1, m[7])));             /* m32 */
	a[A_ROLL] = atan2(-m[6], m[8]);                         /* -m31, m33 */
	a[A_YAW] = atan2(-m[1], m[4]);                          /* -m12, m22 */
}

static void
quaternion_from(double *a)
{
	const double *m = a + A_M;
	double t = m[0] + m[4] + m[8], s;

	if (t > 0) {
		s = sqrt(t + 1) * 2;
		a[A_QW] = s / 4, a[A_QX] = (m[7] - m[5]) / s, a[A_QY] = (m[2] - m[6]) / s, a[A_QZ] = (m[3] - m[1]) / s;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		s = sqrt(1 + m[0] - m[4] - m[8]) * 2;
		a[A_QW] = (m[7] - m[5]) / s, a[A_QX] = s / 4, a[A_QY] = (m[1] + m[3]) / s, a[A_QZ] = (m[2] + m[6]) / s;
	} else if (m[4] > m[8]) {
		s = sqrt(1 + m[4] - m[0] - m[8]) * 2;
		a[A_QW] = (m[2] - m[6]) / s, a[A_QX] = (m[1] + m[3]) / s, a[A_QY] = s / 4, a[A_QZ] = (m[5] + m[7]) / s;
	} else {
		s = sqrt(1 + m[8] - m[0] - m[4]) * 2;
		a[A_QW] = (m[3] - m[1]) / s, a[A_QX] = (m[2] + m[6]) / s, a[A_QY] = (m[5] + m[7]) / s, a[A_QZ] = s / 4;
	}
}

/*
 * The attitude from the rotation vector. Android's R (from x, y, z, w) turns device vectors into east, north, up;
 * the reference frame here is north, west, up, so the device-to-reference matrix is M R with M taking (e, n, u)
 * to (n, -e, u). CMAttitude's quaternion is that rotation, device to reference, and its rotationMatrix the
 * transpose, reference to device (get_matrix()): gravity in the device's axes is minus that matrix's third
 * column, pitch asin(m23), roll atan2(-m13, m33), yaw atan2(-m21, m22) -- worked out here on A itself. (Euler
 * angles taken from the transpose, tried first on 2026-10-31, gave a phone held upright a pitch of 45 degrees
 * instead of 70.)
 */
static bool
attitude_from(const struct hakios_rotv *r, double *a, double *gravity)
{
	float q[4] = { 0 };
	double x, y, z, w, R[9], A[9];

	if (r->count < 3) {
		return false;
	}
	memcpy(q, r->value, sizeof(q));
	x = q[0], y = q[1], z = q[2];
	w = r->count >= 4 ? q[3] : sqrt(fmax(0, 1 - x * x - y * y - z * z));
	R[0] = 1 - 2 * (y * y + z * z), R[1] = 2 * (x * y - z * w), R[2] = 2 * (x * z + y * w);
	R[3] = 2 * (x * y + z * w), R[4] = 1 - 2 * (x * x + z * z), R[5] = 2 * (y * z - x * w);
	R[6] = 2 * (x * z - y * w), R[7] = 2 * (y * z + x * w), R[8] = 1 - 2 * (x * x + y * y);
	/* M R: rows north (R's second row), west (minus the first), up (the third) */
	for (int c = 0; c < 3; c++) {
		A[0 + c] = R[3 + c];
		A[3 + c] = -R[0 + c];
		A[6 + c] = R[6 + c];
	}
	memcpy(a + A_M, A, sizeof(A));
	quaternion_from(a);
	euler_from(a);
	/* gravity, in the device's axes and in g: the reference frame's down (0, 0, -1) in the device's axes */
	gravity[0] = -A[6], gravity[1] = -A[7], gravity[2] = -A[8];
	return true;
}

static id
copy_retained(id self, SEL c, void *zone)
{
	(void)c;
	(void)zone;
	return ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
}

/* a device motion lets its attitude go with it (else one leaks with every reading) */
static void
motion_dealloc(id self, SEL c)
{
	struct objc_super up = { self, class_getSuperclass(object_getClass(self)) };
	id attitude = *(id *)((char *)self + motion_attitude);

	if (attitude != nil) {
		((void (*)(id, SEL))objc_msgSend)(attitude, sel_registerName("release"));
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&up, c);
}

static bool
make_motion_classes(void)
{
	Class att = objc_getClass("CMAttitude"), dm = objc_getClass("CMDeviceMotion"), cls;

	if (motion_class != Nil) {
		return true;
	}
	if (att == Nil || dm == Nil) {
		return false;
	}
	cls = objc_allocateClassPair(att, "HakiosAttitude", 0);
	class_addIvar(cls, "hakios_values", sizeof(double) * A_COUNT, 3, "[16d]");
	class_addMethod(cls, sel_registerName("quaternion"), (IMP)get_quaternion, "{?=dddd}@:");
	class_addMethod(cls, sel_registerName("rotationMatrix"), (IMP)get_matrix, "{?=ddddddddd}@:");
	class_addMethod(cls, sel_registerName("roll"), (IMP)get_roll, "d@:");
	class_addMethod(cls, sel_registerName("pitch"), (IMP)get_pitch, "d@:");
	class_addMethod(cls, sel_registerName("yaw"), (IMP)get_yaw, "d@:");
	class_addMethod(cls, sel_registerName("copyWithZone:"), (IMP)copy_retained, "@@:^v");
	objc_registerClassPair(cls);
	neutralize(cls, att);
	attitude_values = ivar_getOffset(class_getInstanceVariable(cls, "hakios_values"));
	attitude_class = cls;

	cls = objc_allocateClassPair(dm, "HakiosDeviceMotion", 0);
	class_addIvar(cls, "hakios_attitude", sizeof(id), 3, "@");
	class_addIvar(cls, "hakios_values", sizeof(double) * D_COUNT, 3, "[15d]");
	class_addMethod(cls, sel_registerName("attitude"), (IMP)get_attitude, "@@:");
	class_addMethod(cls, sel_registerName("gravity"), (IMP)get_gravity, "{?=ddd}@:");
	class_addMethod(cls, sel_registerName("userAcceleration"), (IMP)get_user, "{?=ddd}@:");
	class_addMethod(cls, sel_registerName("rotationRate"), (IMP)get_rate, "{?=ddd}@:");
	class_addMethod(cls, sel_registerName("magneticField"), (IMP)get_field, "{?={?=ddd}i}@:");
	class_addMethod(cls, sel_registerName("heading"), (IMP)get_motion_heading, "d@:");
	class_addMethod(cls, sel_registerName("timestamp"), (IMP)get_motion_time, "d@:");
	class_addMethod(cls, sel_registerName("sensorLocation"), (IMP)get_sensor_location, "q@:");
	class_addMethod(cls, sel_registerName("copyWithZone:"), (IMP)copy_retained, "@@:^v");
	class_addMethod(cls, sel_registerName("dealloc"), (IMP)motion_dealloc, "v@:");
	class_addMethod(cls, sel_registerName("magneticFieldCalibrationLevel"), (IMP)get_calibration_level, "i@:");
	objc_registerClassPair(cls);
	neutralize(cls, dm);
	motion_attitude = ivar_getOffset(class_getInstanceVariable(cls, "hakios_attitude"));
	motion_values = ivar_getOffset(class_getInstanceVariable(cls, "hakios_values"));
	motion_class = cls;

	for (int k = 0; k < 2; k++) {
		Class base = objc_getClass(k == 0 ? "CMAccelerometerData" : "CMGyroData");

		if (base == Nil) {
			continue;
		}
		cls = objc_allocateClassPair(base, k == 0 ? "HakiosAccelerometerData" : "HakiosGyroData", 0);
		class_addIvar(cls, "hakios_values", sizeof(double) * 4, 3, "[4d]");
		class_addMethod(cls, sel_registerName(k == 0 ? "acceleration" : "rotationRate"), (IMP)get_vector, "{?=ddd}@:");
		class_addMethod(cls, sel_registerName("timestamp"), (IMP)get_vector_time, "d@:");
		class_addMethod(cls, sel_registerName("copyWithZone:"), (IMP)copy_retained, "@@:^v");
		objc_registerClassPair(cls);
		neutralize(cls, base);
		if (k == 0) {
			accel_values = ivar_getOffset(class_getInstanceVariable(cls, "hakios_values"));
			accel_class = cls;
		} else {
			gyro_values = ivar_getOffset(class_getInstanceVariable(cls, "hakios_values"));
			gyro_class = cls;
		}
	}
	return true;
}

/*
 * What a CMMotionManager is given, by kind: device motion (the rotation vector, and the gyroscope and the
 * accelerometer for its rotation rate and the user's acceleration), the accelerometer, the gyroscope. One timer
 * each, on the main queue, at the interval the application set (1/100 s at most often, 1 s at least).
 */
enum { FEED_MOTION, FEED_ACCEL, FEED_GYRO, FEEDS };
static const char *const feed_name[FEEDS] = { "device motion", "accelerometer", "gyroscope" };
static const char *const feed_interval[FEEDS] = { "deviceMotionUpdateInterval", "accelerometerUpdateInterval",
	"gyroUpdateInterval" };

static struct mover {
	id manager;                     /* retained while it moves */
	int kind;
	dispatch_source_t timer;
	uint32_t sequence;
	id data;                        /* the last CMDeviceMotion, CMAccelerometerData or CMGyroData, retained */
	id handler, queue;              /* a copy of the block and the queue, retained, or nil */
} movers[WATCHERS * FEEDS];

static struct mover *
mover_of(id manager, int kind)
{
	for (int i = 0; i < WATCHERS * FEEDS; i++) {
		if (movers[i].manager == manager && (manager == nil || movers[i].kind == kind)) {
			return &movers[i];
		}
	}
	return NULL;
}

static void
msg_release(id o)
{
	if (o != nil) {
		((void (*)(id, SEL))objc_msgSend)(o, sel_registerName("release"));
	}
}

static id
msg_retain(id o)
{
	return o != nil ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("retain")) : nil;
}

/*
 * A stream of the kernel's (kern.hakios.NAME, bsd/dev/hakios.slpi.c), leased once a second while it is read: its
 * last reading, and whether it is a new one since `*sequence`.
 */
static struct stream_state {
	const char *name, *lease;
	uint64_t leased;
} stream_rotv = { "kern.hakios.rotv", "kern.hakios.rotv_lease", 0 },
  stream_accel = { "kern.hakios.accel", "kern.hakios.accel_lease", 0 },
  stream_gyro = { "kern.hakios.gyro", "kern.hakios.gyro_lease", 0 };

static bool
read_stream(struct stream_state *s, struct hakios_rotv *r, uint32_t *sequence)
{
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	size_t length = sizeof(*r);

	if (s->leased == 0 || now - s->leased > 1000000000ull) {
		int one = 1;

		sysctlbyname(s->lease, NULL, NULL, &one, sizeof(one));
		s->leased = now;
	}
	if (sysctlbyname(s->name, r, &length, NULL, 0) != 0 || r->count < 3) {
		return false;
	}
	if (sequence != NULL) {
		if (r->sequence == *sequence) {
			return false;
		}
		*sequence = r->sequence;
	}
	return true;
}

/* the first three floats of a reading, as doubles */
static void
vector_of(const struct hakios_rotv *r, double v[3])
{
	float f[3];

	memcpy(f, r->value, sizeof(f));
	v[0] = f[0], v[1] = f[1], v[2] = f[2];
}

#define STANDARD_GRAVITY 9.80665

static id
make_motion(const struct hakios_rotv *r)
{
	static int shown;
	struct hakios_rotv acc, gyr;
	double gravity[3], accuracy = -1, heading, *d, a3[3], g3[3];
	id m, a = class_createInstance(attitude_class, 0);

	if (!attitude_from(r, a_of(a), gravity)) {
		msg_release(a);
		return nil;
	}
	heading = heading_from(r, &accuracy);
	m = class_createInstance(motion_class, 0);
	*d_attitude(m) = a;             /* (the motion holds it; released with it) */
	d = d_of(m);
	d[D_GX] = gravity[0], d[D_GY] = gravity[1], d[D_GZ] = gravity[2];
	/* a field of 25 uT north and 50 uT up (the southern hemisphere's), in the device's axes: the transpose's */
	for (int i = 0; i < 3; i++) {
		d[D_FX + i] = 25 * a_of(a)[A_M + i] + 50 * a_of(a)[A_M + 6 + i];
	}
	d[D_ACCURACY] = r->status >= 3 ? 2 : r->status == 2 ? 1 : r->status == 1 ? 0 : -1;
	d[D_HEADING] = isnan(heading) ? -1 : heading;
	d[D_TIME] = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
	/* the rotation rate is the gyroscope's; the user's acceleration what the accelerometer has beyond gravity (in g:
	 * iOS's is minus Android's over g, touchHLE's reading of it) */
	if (read_stream(&stream_gyro, &gyr, NULL)) {
		vector_of(&gyr, g3);
		d[D_RX] = g3[0], d[D_RY] = g3[1], d[D_RZ] = g3[2];
	}
	if (read_stream(&stream_accel, &acc, NULL)) {
		vector_of(&acc, a3);
		for (int i = 0; i < 3; i++) {
			d[D_UX + i] = -a3[i] / STANDARD_GRAVITY - gravity[i];
		}
	}
	if (shown < 4) {
		double *q = a_of(a);

		say("motion: attitude q (%.3f %.3f %.3f %.3f) roll %.1f pitch %.1f yaw %.1f, gravity (%.2f %.2f %.2f), heading %.1f, "
		    "rate (%.2f %.2f %.2f), user (%.2f %.2f %.2f)\n", q[A_QX], q[A_QY], q[A_QZ], q[A_QW], q[A_ROLL] * 180 / M_PI,
		    q[A_PITCH] * 180 / M_PI, q[A_YAW] * 180 / M_PI, gravity[0], gravity[1], gravity[2], heading, d[D_RX], d[D_RY],
		    d[D_RZ], d[D_UX], d[D_UY], d[D_UZ]);
		shown++;
	}
	return m;
}

/* CMAccelerometerData (in g, iOS's way) or CMGyroData (rad/s) of a reading */
static id
make_vector_data(int kind, const struct hakios_rotv *r)
{
	static int shown[FEEDS];
	id o = class_createInstance(kind == FEED_ACCEL ? accel_class : gyro_class, 0);
	double *v = v_of(o);

	vector_of(r, v);
	if (kind == FEED_ACCEL) {
		v[0] /= -STANDARD_GRAVITY, v[1] /= -STANDARD_GRAVITY, v[2] /= -STANDARD_GRAVITY;
	}
	v[3] = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
	if (shown[kind]++ < 3) {
		say("motion: %s (%.3f %.3f %.3f)\n", feed_name[kind], v[0], v[1], v[2]);
	}
	return o;
}

static void
feed_tick(struct mover *v)
{
	struct hakios_rotv r;
	id data;

	if (v->kind == FEED_MOTION) {
		if (!read_stream(&stream_rotv, &r, &v->sequence) || (data = make_motion(&r)) == nil) {
			return;
		}
	} else if (!read_stream(v->kind == FEED_ACCEL ? &stream_accel : &stream_gyro, &r, &v->sequence)) {
		return;
	} else {
		data = make_vector_data(v->kind, &r);
	}
	msg_release(v->data);
	v->data = data;
	if (v->handler != nil && v->queue != nil) {
		id handler = v->handler, given = msg_retain(data);

		((void (*)(id, SEL, void (^)(void)))objc_msgSend)(v->queue, sel_registerName("addOperationWithBlock:"), ^{
			((void (^)(id, id))(void *)handler)(given, nil);
			msg_release(given);
		});
	}
}

static void
start_feed(id self, int kind, id queue, id handler)
{
	struct mover *v = mover_of(self, kind);
	double interval;

	/* (not make_heading_class(): that needs CoreLocation, which an application with CoreMotion alone never loads --
	 * Motion Test got no feed at all because of it, 2026-10-31) */
	if (!make_motion_classes()) {
		say("motion: %s for %p NOT ANSWERED: CoreMotion's data classes could not be made\n", feed_name[kind], (void *)self);
		return;
	}
	if (v == NULL && (v = mover_of(nil, kind)) == NULL) {
		say("motion: too many motion managers; this one is not answered\n");
		return;
	}
	if (v->manager == nil) {
		v->manager = msg_retain(self);
		v->kind = kind;
		v->sequence = 0;
		interval = ((double (*)(id, SEL))objc_msgSend)(self, sel_registerName(feed_interval[kind]));
		interval = interval < 1.0 / 100 ? 1.0 / 100 : interval > 1 ? 1 : interval;
		v->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		dispatch_source_set_timer(v->timer, DISPATCH_TIME_NOW, (uint64_t)(interval * NSEC_PER_SEC), NSEC_PER_MSEC * 2);
		dispatch_source_set_event_handler(v->timer, ^{ feed_tick(v); });
		dispatch_resume(v->timer);
		say("motion: %s for %p, every %.0f ms, from the sensor DSP\n", feed_name[kind], (void *)self, interval * 1000);
	}
	if (v->handler != nil) {
		_Block_release((const void *)v->handler);
	}
	msg_release(v->queue);
	v->handler = handler != nil ? (id)_Block_copy((const void *)handler) : nil;
	v->queue = msg_retain(queue);
}

static void
stop_feed(id self, int kind)
{
	struct mover *v = mover_of(self, kind);

	if (v == NULL || self == nil) {
		return;
	}
	dispatch_source_cancel(v->timer);
	dispatch_release(v->timer);
	v->timer = NULL;
	if (v->handler != nil) {
		_Block_release((const void *)v->handler);
	}
	msg_release(v->queue);
	v->handler = v->queue = nil;
	v->manager = nil;
	msg_release(self);
}

static void dm_start(id self, SEL c) { (void)c; start_feed(self, FEED_MOTION, nil, nil); }
static void dm_start_frame(id self, SEL c, unsigned long frame) { (void)c; (void)frame; start_feed(self, FEED_MOTION, nil, nil); }
static void dm_start_queue(id self, SEL c, id queue, id handler) { (void)c; start_feed(self, FEED_MOTION, queue, handler); }
static void dm_start_frame_queue(id self, SEL c, unsigned long frame, id queue, id handler) { (void)c; (void)frame; start_feed(self, FEED_MOTION, queue, handler); }
static void dm_stop(id self, SEL c) { (void)c; stop_feed(self, FEED_MOTION); }
static id dm_motion(id self, SEL c) { struct mover *v = mover_of(self, FEED_MOTION); (void)c; return v != NULL ? v->data : nil; }
static bool dm_active(id self, SEL c) { (void)c; return mover_of(self, FEED_MOTION) != NULL; }

static void ac_start(id self, SEL c) { (void)c; start_feed(self, FEED_ACCEL, nil, nil); }
static void ac_start_queue(id self, SEL c, id queue, id handler) { (void)c; start_feed(self, FEED_ACCEL, queue, handler); }
static void ac_stop(id self, SEL c) { (void)c; stop_feed(self, FEED_ACCEL); }
static id ac_data(id self, SEL c) { struct mover *v = mover_of(self, FEED_ACCEL); (void)c; return v != NULL ? v->data : nil; }
static bool ac_active(id self, SEL c) { (void)c; return mover_of(self, FEED_ACCEL) != NULL; }

static void gy_start(id self, SEL c) { (void)c; start_feed(self, FEED_GYRO, nil, nil); }
static void gy_start_queue(id self, SEL c, id queue, id handler) { (void)c; start_feed(self, FEED_GYRO, queue, handler); }
static void gy_stop(id self, SEL c) { (void)c; stop_feed(self, FEED_GYRO); }
static id gy_data(id self, SEL c) { struct mover *v = mover_of(self, FEED_GYRO); (void)c; return v != NULL ? v->data : nil; }
static bool gy_active(id self, SEL c) { (void)c; return mover_of(self, FEED_GYRO) != NULL; }

static bool dm_available(id self, SEL c) { (void)self; (void)c; return true; }
static unsigned long dm_frames(id self, SEL c) { (void)self; (void)c; return 0xf; }

static void
hook(Class cls, bool class_method, const char *name, IMP imp)
{
	Method m = class_method ? class_getClassMethod(cls, sel_registerName(name)) : class_getInstanceMethod(cls, sel_registerName(name));

	if (m != NULL) {
		method_setImplementation(m, imp);
	}
}

/*
 * Settings' Privacy & Security has Location Services only while -[PUIPrivacyController
 * _cachedLocationServicesAvailableValue] is YES, which -refreshLocationServicesAvailable asks the carrier bundle
 * for (CoreTelephony: each subscription's "LocationServicesAvailable"). There is no CommCenter here, the question
 * fails and the row was gone though Location Services worked (2026-10-31): the answer a phone without a SIM gets.
 */
static id
location_services_available(id self, SEL c)
{
	(void)self;
	(void)c;
	return ((id (*)(Class, SEL, BOOL))objc_msgSend)(objc_getClass("NSNumber"), sel_registerName("numberWithBool:"), true);
}

/*
 * A third-party application's request to send notifications, answered "not allowed" here without the system's
 * question (boot-arg notify_ask=1: asked as before). The data volume is in memory, so the answer given was gone at
 * the next boot and the games asked again at every start (owner, 2026-11-02; work/TODO.md Q1). The application hears
 * what a "Don't Allow" gives: its completion called on a queue of the system's -- or, by UIKit's older
 * -registerUserNotificationSettings:, its delegate told that it was given no types. The image's own programs
 * (/Applications, /System, /usr) are left alone.
 */
static bool
third_party(void)
{
	static int known = -1;

	if (known < 0) {
		char path[1024];
		uint32_t size = sizeof(path);

		known = _NSGetExecutablePath(path, &size) == 0 && strstr(path, ".app/") != NULL &&
		    strncmp(path, "/Applications/", 14) != 0 && strncmp(path, "/System/", 8) != 0 && strncmp(path, "/usr/", 5) != 0;
	}
	return known == 1;
}

static void
notify_request(id self, SEL c, unsigned long options, id handler)
{
	void (^done)(bool, id) = (void (^)(bool, id))handler;

	(void)self;
	(void)c;
	(void)options;
	if (done != NULL) {
		dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			done(false, nil);
		});
	}
}

static void
notify_register(id self, SEL c, id settings)
{
	id delegate = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("delegate"));
	SEL told = sel_registerName("application:didRegisterUserNotificationSettings:");
	Class kind = objc_getClass("UIUserNotificationSettings");
	id none;

	(void)c;
	(void)settings;
	if (delegate == nil || kind == Nil ||
	    !((bool (*)(id, SEL, SEL))objc_msgSend)(delegate, sel_registerName("respondsToSelector:"), told)) {
		return;
	}
	none = ((id (*)(Class, SEL, unsigned long, id))objc_msgSend)(kind, sel_registerName("settingsForTypes:categories:"), 0, nil);
	((id (*)(id, SEL))objc_msgSend)(none, sel_registerName("retain"));     /* (no ARC here: kept for the block) */
	dispatch_async(dispatch_get_main_queue(), ^{
		((void (*)(id, SEL, id, id))objc_msgSend)(delegate, told, self, none);
		((void (*)(id, SEL))objc_msgSend)(none, sel_registerName("release"));
	});
}

/*
 * Frame-rate requests (boot-arg fr_log=1: said; fr_min=N: UIKit's raised to at least N, in every application;
 * fr_min_all=1: the applications' own too). After a home-screen swipe the page settles at 30 Hz for 0.25-1.5 s (logs/040002-catriple.log:
 * ~190 intervals of 32-35 ms) while finger-down scrolling is at 120 Hz -- and halving CoreAnimation's beat changed
 * nothing (logs/030653-vrrR1.log): the 30 is asked for in frames per second, not in beats. Who asks it, and with what
 * range, is said once per caller and range (again at the 10th, 100th, 1000th time). fr_min=N lifts a range whose
 * maximum is under N to a maximum of N (a set preferred rate under N to N), its minimum left: with fr_min=120 the
 * settling's 20-30 is 20-120 and 99% of the after-lift intervals are under 12 ms, none 30-36 ms
 * (logs/033942-homeFR120.log). A game that asks for 30 on purpose (its timing may count on it) is left alone unless
 * fr_min_all=1. Nothing is hooked without fr_log or fr_min.
 */
typedef struct { float minimum, maximum, preferred; } fr_range;
static IMP fr_link_range_original, fr_link_fps_original, fr_link_interval_original, fr_animation_range_original;
static unsigned fr_min;
static bool fr_log;
static os_unfair_lock fr_lock = OS_UNFAIR_LOCK_INIT;

static void
fr_say(const char *what, void *caller, double a, double b, double c)
{
	static struct { void *caller; float a, b, c; unsigned n; } seen[128];
	Dl_info info;
	const char *image = "?", *function = "?";
	unsigned i, n = 0;

	os_unfair_lock_lock(&fr_lock);
	for (i = 0; i < 128 && seen[i].n != 0; i++) {
		if (seen[i].caller == caller && seen[i].a == (float)a && seen[i].b == (float)b && seen[i].c == (float)c) {
			break;
		}
	}
	if (i < 128) {
		if (seen[i].n == 0) {
			seen[i].caller = caller;
			seen[i].a = (float)a, seen[i].b = (float)b, seen[i].c = (float)c;
		}
		n = ++seen[i].n;
	}
	os_unfair_lock_unlock(&fr_lock);
	if (n != 1 && n != 10 && n != 100 && n != 1000) {
		return;
	}
	if (dladdr(caller, &info) != 0) {
		image = info.dli_fname != NULL && strrchr(info.dli_fname, '/') != NULL ? strrchr(info.dli_fname, '/') + 1 : "?";
		function = info.dli_sname != NULL ? info.dli_sname : "?";
	}
	say("framerate: %s: %s %g %g %g from %s %s (time %u)\n", getprogname(), what, a, b, c, image, function, n);
}

/*
 * (What fr_log found, 2026-11-08 logs/031727-frlog.log: UIKit in SpringBoard sets its display link to 48-120 while the
 * finger is down and to 20-30 -- a maximum of 30 -- for the settling, over a hundred times in a run of swipes. Only a
 * range that stops below fr_min is changed: its maximum lifted to fr_min, a preferred rate under it too. Raising the
 * minimum of the others as well (48-120 -> 60-120) cost finger-down frames: CoreAnimation then took 60 at times,
 * logs/032137-frmin60.log. The default range, all zeros, is left as it is.)
 */
/*
 * Whose request it is: UIKit's own (its scroll views' settling -- in every application, the same 20-30) is raised;
 * an application's own display link or animation is not, unless fr_min_all=1: a game may run at 30 on purpose and
 * count its time by frames. (The caller's image, by dladdr: a few hundred calls in a run of swipes.)
 */
static bool fr_all;

static bool
fr_from_uikit(void *caller)
{
	Dl_info info;

	if (fr_all) {
		return true;
	}
	return dladdr(caller, &info) != 0 && info.dli_fname != NULL && strstr(info.dli_fname, "/UIKitCore.framework/") != NULL;
}

static fr_range
fr_raise(fr_range r, void *caller)
{
	if (fr_min != 0 && r.maximum > 0 && r.maximum < (float)fr_min && fr_from_uikit(caller)) {
		r.maximum = (float)fr_min;
		r.preferred = r.preferred != 0 && r.preferred < (float)fr_min ? (float)fr_min : r.preferred;
	}
	return r;
}

static void
fr_link_range(id self, SEL c, fr_range r)
{
	void *caller = __builtin_return_address(0);

	if (fr_log) {
		fr_say("-[CADisplayLink setPreferredFrameRateRange:]", caller, r.minimum, r.maximum, r.preferred);
	}
	((void (*)(id, SEL, fr_range))fr_link_range_original)(self, c, fr_raise(r, caller));
}

static void
fr_animation_range(id self, SEL c, fr_range r)
{
	void *caller = __builtin_return_address(0);

	if (fr_log) {
		fr_say("-[CAAnimation setPreferredFrameRateRange:]", caller, r.minimum, r.maximum, r.preferred);
	}
	((void (*)(id, SEL, fr_range))fr_animation_range_original)(self, c, fr_raise(r, caller));
}

static void
fr_link_fps(id self, SEL c, long fps)
{
	void *caller = __builtin_return_address(0);

	if (fr_log) {
		fr_say("-[CADisplayLink setPreferredFramesPerSecond:]", caller, (double)fps, 0, 0);
	}
	if (fr_min != 0 && fps > 0 && fps < (long)fr_min && fr_from_uikit(caller)) {
		fps = (long)fr_min;
	}
	((void (*)(id, SEL, long))fr_link_fps_original)(self, c, fps);
}

static void
fr_link_interval(id self, SEL c, long interval)
{
	if (fr_log) {
		fr_say("-[CADisplayLink setFrameInterval:]", __builtin_return_address(0), (double)interval, 0, 0);
	}
	((void (*)(id, SEL, long))fr_link_interval_original)(self, c, interval);
}

static IMP
fr_hook(Class cls, const char *name, IMP imp)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(name));

	return m != NULL ? method_setImplementation(m, imp) : NULL;
}

/*
 * (2026-11-14, boot-arg layercheck=NAME: programs whose name begins with NAME) Flutter Check's intermittent black
 * launch, everything above the pixels found as in a drawn one (rounds 53-66): what the application gives Core
 * Animation as a layer's contents -- an IOSurface -- read back on this processor a second later: of a sample of its
 * pixels, how many are not 0. A black launch whose surfaces hold pixels: the render server shows them wrong; one whose
 * surfaces are empty: the frame was never stored into them (the Metal driver's pass into the drawable).
 */
static IMP lc_set_contents_original;
static unsigned lc_seen;

static void
lc_check(const void *surface)
{
	static unsigned long (*type_id)(void);
	static int (*lock)(const void *, uint32_t, uint32_t *), (*unlock)(const void *, uint32_t, uint32_t *);
	static void *(*base)(const void *);
	static size_t (*alloc_size)(const void *), (*row)(const void *), (*height)(const void *), (*width)(const void *);
	static uint32_t (*sid)(const void *);
	static void (*release)(const void *);
	uint32_t nonzero = 0, samples = 0, first = 0, middle = 0;
	const uint8_t *p;
	size_t size;

	if (lock == NULL) {
		*(void **)&lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
		*(void **)&unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
		*(void **)&base = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
		*(void **)&alloc_size = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize");
		*(void **)&row = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
		*(void **)&height = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight");
		*(void **)&width = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth");
		*(void **)&sid = dlsym(RTLD_DEFAULT, "IOSurfaceGetID");
		*(void **)&release = dlsym(RTLD_DEFAULT, "CFRelease");
		*(void **)&type_id = dlsym(RTLD_DEFAULT, "IOSurfaceGetTypeID");
	}
	if (lock == NULL || unlock == NULL || base == NULL || alloc_size == NULL || sid == NULL || release == NULL) {
		return;
	}
	if (lock(surface, 1 /* read only */, NULL) == 0) {
		p = base(surface);
		size = alloc_size(surface);
		for (size_t at = 0; p != NULL && at + 4 <= size; at += 4096) {
			uint32_t v;

			memcpy(&v, p + at, 4);
			nonzero += v != 0;
			samples++;
		}
		if (p != NULL && size >= 8) {
			memcpy(&first, p, 4);
			memcpy(&middle, p + (size / 2 & ~(size_t)3), 4);
		}
		unlock(surface, 1, NULL);
		say("layercheck: surface %u (%zux%zu, %zu bytes a row): %u of %u samples not 0, the first %#010x, one in the "
		    "middle %#010x (contents %u)\n", sid(surface), width ? width(surface) : 0, height ? height(surface) : 0,
		    row ? row(surface) : 0, nonzero, samples, first, middle, lc_seen);
	}
	release(surface);
}

static void
lc_set_contents(id self, SEL c, id contents)
{
	static unsigned long (*type_id)(void), (*get_type)(const void *);
	static const void *(*retain)(const void *);

	((void (*)(id, SEL, id))lc_set_contents_original)(self, c, contents);
	if (type_id == NULL) {
		*(void **)&type_id = dlsym(RTLD_DEFAULT, "IOSurfaceGetTypeID");
		*(void **)&get_type = dlsym(RTLD_DEFAULT, "CFGetTypeID");
		*(void **)&retain = dlsym(RTLD_DEFAULT, "CFRetain");
	}
	if (contents == nil || type_id == NULL || get_type == NULL || retain == NULL || get_type(contents) != type_id()) {
		return;
	}
	/* (the first 8, then every 60th) */
	if (lc_seen++ < 8 || lc_seen % 60 == 0) {
		const void *surface = retain(contents);

		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			lc_check(surface);
		});
	}
}

static void
lc_hooks(void)
{
	static bool done, checked, wanted;
	Class layer = objc_getClass("CALayer");

	if (!checked) {
		char args[1024] = "";
		size_t len = sizeof(args) - 1;
		const char *at;

		checked = true;
		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0 && (at = strstr(args, "layercheck=")) != NULL) {
			size_t n = strcspn(at + 11, " ");

			wanted = getprogname() != NULL && n > 0 && strncmp(getprogname(), at + 11, n) == 0;
		}
	}
	if (done || !wanted || layer == Nil) {
		return;
	}
	done = true;
	lc_set_contents_original = fr_hook(layer, "setContents:", (IMP)lc_set_contents);
	say("layercheck: %s: -[CALayer setContents:] %s: IOSurface contents read back a second after\n", getprogname(),
	    lc_set_contents_original ? "hooked" : "MISSING");
}

/* (once, when QuartzCore's classes are there) */
static void
fr_hooks(void)
{
	static bool done, checked, wanted;
	Class link = objc_getClass("CADisplayLink"), animation = objc_getClass("CAAnimation");

	if (!checked) {
		char args[1024] = "";
		size_t len = sizeof(args) - 1;
		const char *at;

		checked = true;
		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0) {
			fr_log = strstr(args, "fr_log=1") != NULL;
			fr_all = strstr(args, "fr_min_all=1") != NULL;
			if ((at = strstr(args, "fr_min=")) != NULL) {
				fr_min = (unsigned)strtoul(at + 7, NULL, 10);
				fr_min = fr_min > 240 ? 240 : fr_min;
			}
		}
		wanted = fr_log || fr_min != 0;
	}
	if (done || !wanted || link == Nil || animation == Nil) {
		return;
	}
	done = true;
	fr_link_range_original = fr_hook(link, "setPreferredFrameRateRange:", (IMP)fr_link_range);
	fr_link_fps_original = fr_hook(link, "setPreferredFramesPerSecond:", (IMP)fr_link_fps);
	fr_link_interval_original = fr_hook(link, "setFrameInterval:", (IMP)fr_link_interval);
	fr_animation_range_original = fr_hook(animation, "setPreferredFrameRateRange:", (IMP)fr_animation_range);
	say("framerate: %s: requests %s%s%s (display link range %s, fps %s, interval %s; animation range %s)\n", getprogname(),
	    fr_log ? "said" : "not said", fr_min != 0 ? ", raised to at least fr_min" : "", fr_min != 0 ? (fr_all ?
	    " (everybody's)" : " (UIKit's)") : "",
	    fr_link_range_original ? "hooked" : "MISSING", fr_link_fps_original ? "hooked" : "MISSING",
	    fr_link_interval_original ? "hooked" : "MISSING", fr_animation_range_original ? "hooked" : "MISSING");
}

/* CLLocationManager's heading and CMMotionManager's device motion answered here (once each, in the first image
 * that has the class: CoreLocation's, CoreMotion's); Settings' privacy pane's question about location too */
static void
compass_hooks(const struct mach_header *header, intptr_t image_slide)
{
	static bool done, motion_done, privacy_done, notify_center_done, notify_app_done, notify_checked;
	Class manager, motion, privacy, center, application;
	Method m;

	(void)header;
	(void)image_slide;
	fr_hooks();
	lc_hooks();
	gl_lazy_hooks();
	if (!notify_checked) {
		char args[1024] = "";
		size_t len = sizeof(args) - 1;

		notify_checked = true;
		if (!third_party() || (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0 && strstr(args, "notify_ask=1") != NULL)) {
			notify_center_done = notify_app_done = true;
		}
	}
	if (!notify_center_done && (center = objc_getClass("UNUserNotificationCenter")) != Nil) {
		notify_center_done = true;
		hook(center, false, "requestAuthorizationWithOptions:completionHandler:", (IMP)notify_request);
		say("notifications: %s asks no question (its requests answered \"not allowed\"; notify_ask=1: asked)\n",
		    getprogname());
	}
	if (!notify_app_done && (application = objc_getClass("UIApplication")) != Nil) {
		notify_app_done = true;
		hook(application, false, "registerUserNotificationSettings:", (IMP)notify_register);
	}
	if (!privacy_done && (privacy = objc_getClass("PUIPrivacyController")) != Nil) {
		privacy_done = true;
		hook(privacy, false, "_cachedLocationServicesAvailableValue", (IMP)location_services_available);
		say("privacy: Location Services shown in Privacy & Security (no carrier bundle to ask)\n");
	}
	if (!motion_done) {
		/* boot-arg motion_hook=0: CoreMotion left as it is (to tell what is ours from what is not) */
		char args[1024] = "";
		size_t len = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0 && strstr(args, "motion_hook=0") != NULL) {
			motion_done = true;
		}
	}
	if (!motion_done && (motion = objc_getClass("CMMotionManager")) != Nil) {
		motion_done = true;
		hook(motion, false, "isDeviceMotionAvailable", (IMP)dm_available);
		hook(motion, true, "availableAttitudeReferenceFrames", (IMP)dm_frames);
		hook(motion, false, "isDeviceMotionActive", (IMP)dm_active);
		hook(motion, false, "deviceMotion", (IMP)dm_motion);
		hook(motion, false, "startDeviceMotionUpdates", (IMP)dm_start);
		hook(motion, false, "startDeviceMotionUpdatesUsingReferenceFrame:", (IMP)dm_start_frame);
		hook(motion, false, "startDeviceMotionUpdatesToQueue:withHandler:", (IMP)dm_start_queue);
		hook(motion, false, "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:", (IMP)dm_start_frame_queue);
		hook(motion, false, "stopDeviceMotionUpdates", (IMP)dm_stop);
		hook(motion, false, "isAccelerometerAvailable", (IMP)dm_available);
		hook(motion, false, "isAccelerometerActive", (IMP)ac_active);
		hook(motion, false, "accelerometerData", (IMP)ac_data);
		hook(motion, false, "startAccelerometerUpdates", (IMP)ac_start);
		hook(motion, false, "startAccelerometerUpdatesToQueue:withHandler:", (IMP)ac_start_queue);
		hook(motion, false, "stopAccelerometerUpdates", (IMP)ac_stop);
		hook(motion, false, "isGyroAvailable", (IMP)dm_available);
		hook(motion, false, "isGyroActive", (IMP)gy_active);
		hook(motion, false, "gyroData", (IMP)gy_data);
		hook(motion, false, "startGyroUpdates", (IMP)gy_start);
		hook(motion, false, "startGyroUpdatesToQueue:withHandler:", (IMP)gy_start_queue);
		hook(motion, false, "stopGyroUpdates", (IMP)gy_stop);
	}
	if (done || (manager = objc_getClass("CLLocationManager")) == Nil) {
		return;
	}
	done = true;
	if ((m = class_getClassMethod(manager, sel_registerName("headingAvailable"))) != NULL) {
		method_setImplementation(m, (IMP)heading_available);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("headingAvailable"))) != NULL) {
		method_setImplementation(m, (IMP)heading_available);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("startUpdatingHeading"))) != NULL) {
		method_setImplementation(m, (IMP)start_heading);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("stopUpdatingHeading"))) != NULL) {
		method_setImplementation(m, (IMP)stop_heading);
	}
	if ((m = class_getInstanceMethod(manager, sel_registerName("heading"))) != NULL) {
		method_setImplementation(m, (IMP)last_heading);
	}
}

__attribute__((constructor))
static void
hakios_compass_init(void)
{
	/* (called at once for every image already loaded, then for each one loaded later) */
	_dyld_register_func_for_add_image(compass_hooks);
}

/*
 * Metal for the applications the owner allows (the Metal plan's step 3, hk's allow-list). The port's Metal driver
 * (libhakios.metal.dylib) is registered with Metal in this process -- MTLAddDevice, as System Check's Metal tab does
 * it -- before the application's own code runs, so its MTLCreateSystemDefaultDevice() answers the Adreno. Nothing is
 * allowed unless named: the boot-arg metal_apps=ID,ID,... or /redwood/etc/metal-apps, one bundle ID a line -- or, with
 * the boot-arg metal_flutter=1, by embedding Flutter.framework (Flutter has no other way to draw). Never a
 * program of the system's: backboardd and SpringBoard never load this library (build.sh's INJECT is "*.app *.appex"),
 * an application of /Applications or /System is not third_party(), and an app extension or widget is refused here by
 * its path. The window server keeps OpenGL ES, which composites at 120 Hz where Metal composited at 14 to 27 fps.
 *
 * In the constructor, because a game makes its device in -application:didFinishLaunching... or earlier. CoreFoundation,
 * Metal and IOKit are taken by dlopen and dlsym only when a list exists, so every other application loads nothing more
 * than before; dlopen runs their initialisers, CoreFoundation's among them, before anything of theirs is used.
 */
#define METAL_DRIVER "/redwood/lib/libhakios.metal.dylib"
#define METAL_APPS_FILE "/redwood/etc/metal-apps"

/* whether `want` is one of the entries of `list`, separated by `separator` (blanks around an entry ignored) */
static bool
metal_listed(const char *want, const char *list, char separator)
{
	size_t n = strlen(want);

	for (const char *p = list; p != NULL && *p != '\0';) {
		const char *end = strchr(p, separator);
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);

		while (len != 0 && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		while (len != 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n')) {
			len--;
		}
		if (len == n && strncmp(p, want, n) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		p = end + 1;
	}
	return false;
}

static void
metal_for_allowed(void)
{
	char path[1024], args[1024] = "", apps[1024] = "", file[4096] = "", bundle_id[256] = "";
	uint32_t path_size = sizeof(path);
	size_t args_size = sizeof(args) - 1;
	const char *a;
	int fd;

	if (!third_party() || _NSGetExecutablePath(path, &path_size) != 0 || strstr(path, ".appex/") != NULL) {
		return;
	}
	(void)sysctlbyname("kern.bootargs", args, &args_size, NULL, 0);
	if ((a = strstr(args, "metal_apps=")) != NULL) {
		size_t k = 0;

		for (a += 11; *a != '\0' && *a != ' ' && k < sizeof(apps) - 1; a++) {
			apps[k++] = *a;
		}
		apps[k] = '\0';
	}
	if ((fd = open(METAL_APPS_FILE, O_RDONLY)) >= 0) {
		ssize_t got = read(fd, file, sizeof(file) - 1);

		file[got > 0 ? got : 0] = '\0';
		close(fd);
	}
	/*
	 * (the K40's) metal_flutter=1: an application that embeds Flutter.framework is allowed without being named. Flutter
	 * on an iPhone draws through Metal and nothing else -- given no device, its engine stops the application at launch
	 * (Flutter Check: SIGABRT 0.1 s in, 2026-11-10).
	 */
	bool flutter = false;

	if (strstr(args, "metal_flutter=1") != NULL) {
		char framework[sizeof(path) + 48];
		const char *slash = strrchr(path, '/');

		if (slash != NULL) {
			snprintf(framework, sizeof(framework), "%.*s/Frameworks/Flutter.framework/Flutter", (int)(slash - path),
			    path);
			flutter = access(framework, F_OK) == 0;
		}
	}
	if (apps[0] == '\0' && file[0] == '\0' && !flutter) {
		return;                 /* (no list: nothing allowed, nothing loaded, nothing said) */
	}

	/* the bundle ID, through CoreFoundation taken by name (this library links none of it) */
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	void *(*main_bundle)(void) = cf ? (void *(*)(void))dlsym(cf, "CFBundleGetMainBundle") : NULL;
	const void *(*identifier)(void *) = cf ? (const void *(*)(void *))dlsym(cf, "CFBundleGetIdentifier") : NULL;
	bool (*c_string)(const void *, char *, long, uint32_t) =
	    cf ? (bool (*)(const void *, char *, long, uint32_t))dlsym(cf, "CFStringGetCString") : NULL;
	void *bundle = main_bundle ? main_bundle() : NULL;
	const void *bid = bundle && identifier ? identifier(bundle) : NULL;

	if (bid == NULL || c_string == NULL ||
	    !c_string(bid, bundle_id, sizeof(bundle_id), 0x08000100 /* kCFStringEncodingUTF8 */)) {
		say("metal: no bundle ID to look up in the allow-list; not allowed\n");
		return;
	}
	/*
	 * (the K40's) "*" in the list: every application of the owner's, the owner's choice for the release (2026-11-11).
	 * Still never a program of the system's nor an app extension (third_party() and the .appex test above): only what
	 * IPAs installed. An application that would draw better through OpenGL ES can be left out of it by naming it
	 * in /redwood/etc/metal-apps as "-ID".
	 */
	bool everyone = metal_listed("*", apps, ',') || metal_listed("*", file, '\n');
	char refused[260];

	snprintf(refused, sizeof(refused), "-%s", bundle_id);
	if (everyone && (metal_listed(refused, apps, ',') || metal_listed(refused, file, '\n'))) {
		say("metal: %s is left out of metal_apps=* (\"-%s\"); not allowed\n", bundle_id, bundle_id);
		return;
	}
	if (everyone) {
		say("metal: %s, an application of the owner's (metal_apps=*): allowed\n", bundle_id);
	} else if (!metal_listed(bundle_id, apps, ',') && !metal_listed(bundle_id, file, '\n')) {
		if (!flutter) {
			say("metal: %s is not on the allow-list (metal_apps=, %s); not allowed\n", bundle_id, METAL_APPS_FILE);
			return;
		}
		say("metal: %s embeds Flutter.framework, which draws through Metal only (metal_flutter=1): allowed\n",
		    bundle_id);
	}

	void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
	void (*add)(id) = metal ? (void (*)(id))dlsym(metal, "MTLAddDevice") : NULL;
	id (*system_default)(void) = metal ? (id (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice") : NULL;
	void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	void *(*matching)(const char *) = iokit ? (void *(*)(const char *))dlsym(iokit, "IOServiceMatching") : NULL;
	unsigned (*service_of)(unsigned, void *) = iokit ? (unsigned (*)(unsigned, void *))dlsym(iokit,
	    "IOServiceGetMatchingService") : NULL;
	Class device_class;
	unsigned service;
	id device;

	if (add == NULL || system_default == NULL || matching == NULL || service_of == NULL) {
		say("metal: %s is allowed and %s; nothing registered\n", bundle_id, add == NULL || system_default == NULL ?
		    "Metal has no MTLAddDevice / MTLCreateSystemDefaultDevice" : "IOKit's matching calls are missing");
		return;
	}
	/*
	 * Metal's own discovery first, for two reasons. MTLAddDevice asserts "_deviceArray != NULL" (Metal,
	 * MTLAddDevice+0x44) and that array is made by the discovery's dispatch_once -- unasked, it aborted every launch
	 * of the first allowed application (2026-11-09; System Check's Metal tab always asked first). And a device Metal
	 * found on its own is left alone: a second one is not added beside it.
	 */
	id found = system_default();
	if (found != nil) {
		((void (*)(id, SEL))objc_msgSend)(found, sel_registerName("release"));
		say("metal: %s is allowed and Metal has a device of its own already; nothing added\n", bundle_id);
		return;
	}
	if (dlopen(METAL_DRIVER, RTLD_NOW) == NULL || (device_class = objc_getClass("AliothMetalDevice")) == Nil) {
		say("metal: %s is allowed and %s will not load: %s\n", bundle_id, METAL_DRIVER,
		    dlerror() ?: "no AliothMetalDevice");
		return;
	}
	if ((service = service_of(0, matching("IOAcceleratorES"))) == 0) {
		say("metal: %s is allowed and there is no IOAcceleratorES service to hand the device\n", bundle_id);
		return;
	}
	device = ((id (*)(id, SEL, unsigned))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)((id)device_class,
	    sel_registerName("alloc")), sel_registerName("initWithAcceleratorPort:"), service);
	if (device == nil) {
		say("metal: %s is allowed and the device would not start (-initWithAcceleratorPort: gave nil)\n",
		    bundle_id);
		return;
	}
	add(device);
	say("metal: %s is allowed: the port's Metal device is registered in it\n", bundle_id);
}

/*
 * (the K40's) boot-arg app_images=1: every image an application has or loads outside the shared cache, said with its
 * address as it comes. The kernel's "fault:" lines give bare addresses, the cache's named by its slide and these by
 * nothing -- Flutter Check's first frame died at 0x10707d87c in an image no log named (2026-11-10).
 */
static uintptr_t image_cache_start, image_cache_end;

static void
image_said(const struct mach_header *header, intptr_t image_slide)
{
	Dl_info info;

	(void)image_slide;
	if ((uintptr_t)header >= image_cache_start && (uintptr_t)header < image_cache_end) {
		return;
	}
	if (dladdr(header, &info) != 0 && info.dli_fname != NULL) {
		say("image %#lx %s\n", (unsigned long)(uintptr_t)header, info.dli_fname);
	}
}

static void
images_said(const void *cache, size_t length)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, "app_images=1") == NULL) {
		return;
	}
	image_cache_start = (uintptr_t)cache;
	image_cache_end = (uintptr_t)cache + length;
	_dyld_register_func_for_add_image(image_said);      /* (at once for those loaded, then for each new one) */
}

__attribute__((constructor))
static void
hakios_ui_init(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	Class cls;
	Method method;
	IMP original;

	images_said(cache, length);
	model_decide();
	metal_for_allowed();
	app_opengl_es();
	cls = objc_getClass("CIContext");
	method = cls ? class_getClassMethod(cls, sel_registerName("internalContextWithEAGLContext:options:")) : NULL;
	original = method ? method_getImplementation(method) : NULL;
	if (cache == NULL || original == NULL) {
		return;         /* (no CoreImage in this program: nothing of it to keep off OpenGL ES) */
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if ((uintptr_t)original != CI_CONTEXT_WITH_EAGL + slide) {
		say("+[CIContext internalContextWithEAGLContext:options:]: found %#lx, not what was expected; left alone\n",
		    (unsigned long)((uintptr_t)original - slide));
		return;
	}
	orig_context_with_eagl = (id (*)(id, SEL, id, id))original;
	method_setImplementation(method, (IMP)context_with_eagl);
}
