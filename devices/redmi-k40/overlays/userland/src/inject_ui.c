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
 * locationd is not asked for it. Not yet: the true north (no declination without a location: trueHeading is
 * the magnetic one) and the raw field (x, y, z are 0).
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <stdarg.h>
#include <sys/sysctl.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static id
context_with_eagl(id self, SEL selector, id eagl, id options)
{
	if (eagl != nil ? !has_table(eagl) : !gl_works()) {
		return nil;
	}
	return orig_context_with_eagl(self, selector, eagl, options);
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

static void
tick(struct watcher *w)
{
	static int shown;
	struct hakios_rotv r;
	size_t length = sizeof(r);
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);         /* (dispatch_time() is in ticks, not ns) */
	double accuracy = -1, heading, filter;
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
	values_of(h)[H_TRUE] = heading;         /* (no declination without a location) */
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

/* CLLocationManager's heading and CMMotionManager's device motion answered here (once each, in the first image
 * that has the class: CoreLocation's, CoreMotion's) */
static void
compass_hooks(const struct mach_header *header, intptr_t image_slide)
{
	static bool done, motion_done;
	Class manager, motion;
	Method m;

	(void)header;
	(void)image_slide;
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

__attribute__((constructor))
static void
hakios_ui_init(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	Class cls = objc_getClass("CIContext");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("internalContextWithEAGLContext:options:")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

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
