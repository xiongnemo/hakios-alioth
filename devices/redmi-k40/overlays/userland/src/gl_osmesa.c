/*
 * The OpenGL ES library behind src/eagl.c when that library is Mesa as tools/mk-mesa.sh builds
 * it: any of its drivers that draws into memory, reached through its off-screen interface
 * (OSMesa, given an OpenGL ES profile by tools/mesa/osmesa-es-profile.patch).
 *
 * This file is all that knows which library draws. eagl.c wants four things besides the gl*
 * functions themselves: a context of a version of OpenGL ES, that context made the calling
 * thread's, a context taken away again, and a name for the log. A library with another way to
 * its contexts gets another file like this one.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hakios.gl.h"

/* GL/osmesa.h of Mesa */
#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38    /* OpenGL ES 1, 2 or 3 (tools/mesa/osmesa-es-profile.patch) */
#define GL_UNSIGNED_BYTE                0x1401
#define GL_TEXTURE_2D                   0x0de1
#define GL_RENDERER                     0x1f01

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern void (*OSMesaGetProcAddress(const char *name))(void);
/* Mesa's own per-thread current context, read and cleared without being looked into (forget_dead_current()) */
extern void *_mesa_glapi_get_context(void);
extern void _mesa_glapi_set_context(void *context);
extern void _mesa_glapi_set_dispatch(void *dispatch);
extern unsigned char OSMesaHakiosCopy(unsigned renderbuffer, unsigned texture, unsigned char flip)
    __attribute__((weak_import));      /* tools/mesa/osmesa-hakios-copy.patch */
extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);

/*
 * OSMesa draws its "window" into memory handed to it. Nothing here draws into a window (the
 * window server draws into textures of its own, see eagl.c): a few pixels will do.
 */
#define WINDOW  16

struct hakios_gl {
	OSMesaContext context;
	void *current;                  /* Mesa's context of it, as the thread's current one once made so */
	unsigned char window[WINDOW * WINDOW * 4];
};

/*
 * Every context alive, by Mesa's own pointer (what the thread's current one is). A thread whose current context is
 * not among them has a freed one current: Mesa's next make-current there reads it to flush it, and faults --
 * SceneKit's display link in -[SCNRenderer _installContext] -> -[EAGLContext dealloc] of the context it let go of
 * (GPU Check scenekit.default, again on 2026-11-08 16:15, after app_settle() had closed the ways we knew of).
 * Such a thread's current context is forgotten first, without Mesa looking into it.
 */
#define LIVE    64
static struct hakios_gl *live[LIVE];
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned forgotten;

/* a line on the console, where eagl.c's go too (an application's standard error goes nowhere) */
static void
gl_say(const char *format, ...)
{
	static int console = -2;
	char line[256];
	va_list ap;
	int n;

	if (console == -2) {
		console = open("/dev/console", O_WRONLY);
	}
	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static void
forget_dead_current(void)
{
	void *current = _mesa_glapi_get_context();
	bool alive = current == NULL;

	if (alive) {
		return;
	}
	pthread_mutex_lock(&live_lock);
	for (unsigned i = 0; i < LIVE && !alive; i++) {
		alive = live[i] != NULL && live[i]->current == current;
	}
	pthread_mutex_unlock(&live_lock);
	if (!alive) {
		_mesa_glapi_set_context(NULL);
		_mesa_glapi_set_dispatch(NULL);
		if (forgotten++ < 8) {
			gl_say("hakios-gl: %s: a freed context was this thread's current one (%p, thread %p): forgotten\n",
			    getprogname(), current, (void *)pthread_self());
		}
	}
}

struct hakios_gl *
hakios_gl_create_shared(int version, struct hakios_gl *share)
{
	const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, version, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	struct hakios_gl *gl = calloc(1, sizeof(*gl));

	if (gl != NULL &&
	    (gl->context = OSMesaCreateContextAttribs(attributes, share != NULL ? share->context : NULL)) == NULL) {
		free(gl);
		gl = NULL;
	}
	if (gl != NULL) {
		pthread_mutex_lock(&live_lock);
		for (unsigned i = 0; i < LIVE; i++) {
			if (live[i] == NULL) {
				live[i] = gl;
				break;
			}
		}
		pthread_mutex_unlock(&live_lock);
	}
	return gl;
}

struct hakios_gl *
hakios_gl_create(int version)
{
	return hakios_gl_create_shared(version, NULL);
}

int
hakios_gl_make_current(struct hakios_gl *gl)
{
	int done;

	forget_dead_current();
	if (gl == NULL) {
		return OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
	}
	done = OSMesaMakeCurrent(gl->context, gl->window, GL_UNSIGNED_BYTE, WINDOW, WINDOW);
	if (done) {
		gl->current = _mesa_glapi_get_context();
	}
	return done;
}

int
hakios_gl_has_current(void)
{
	return _mesa_glapi_get_context() != NULL;
}

void
hakios_gl_destroy(struct hakios_gl *gl)
{
	pthread_mutex_lock(&live_lock);
	for (unsigned i = 0; i < LIVE; i++) {
		if (live[i] == gl) {
			live[i] = NULL;
		}
	}
	pthread_mutex_unlock(&live_lock);
	forget_dead_current();          /* (Mesa makes the dying one current, then the thread's one before it again) */
	OSMesaDestroyContext(gl->context);
	free(gl);
}

const char *
hakios_gl_library(void)
{
	return "Mesa, off-screen (OSMesa)";
}

/*
 * OES_EGL_image, with the IOSurface's ID as the image (tools/mesa/freedreno-darwin.patch: the
 * kernel's GPU driver makes a buffer of the surface's memory). The drivers that draw in memory
 * of their own with this processor are not asked: they refuse every time.
 */
int
hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface)
{
	static void (*image_target)(unsigned target, void *image);
	static int asked, software;

	(void)gl;
	if (!asked) {
		const char *renderer = (const char *)glGetString(GL_RENDERER);

		asked = 1;
		image_target = (void (*)(unsigned, void *))OSMesaGetProcAddress("glEGLImageTargetTexture2DOES");
		software = renderer == NULL || strstr(renderer, "softpipe") != NULL || strstr(renderer, "llvmpipe") != NULL;
	}
	if (image_target == NULL || software) {
		return 0;
	}
	while (glGetError() != 0) {
	}
	image_target(GL_TEXTURE_2D, (void *)(uintptr_t)surface);
	return glGetError() == 0;
}

int
hakios_gl_copy(struct hakios_gl *gl, unsigned renderbuffer, unsigned texture, int flip)
{
	(void)gl;
	return OSMesaHakiosCopy != NULL && OSMesaHakiosCopy(renderbuffer, texture, flip != 0);
}
