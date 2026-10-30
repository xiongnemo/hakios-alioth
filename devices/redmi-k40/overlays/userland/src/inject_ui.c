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
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdarg.h>
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
