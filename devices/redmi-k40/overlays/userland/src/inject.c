/*
 * libhakios.dylib: code of ours inside stock iOS programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of iOS 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "hakios.input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[2048];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("hakios: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

static void
window_server_fixes(void)
{
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

/*
 * A light sensor that arrives after CoreBrightness has started (ours: the sensor DSP is up some 20 s after the
 * boot) went -[CBDisplayContaineriOS addHIDServiceClient:] -> setColorMitigations -> -[CBABModuleiOS
 * shouldMitigateHarmony:] -> AABC::getALSSensorTypeFromService() on the module's AABC, which is still NULL then:
 * backboardd died at 0x1c8, every 10 s, and the home screen never came (2026-10-30, slpi=1). On an iPhone the sensor
 * is there before the module. With no AABC there is nothing to mitigate True Tone ("Harmony") for: NO.
 */
#define CB_SHOULD_MITIGATE_HARMONY      0x1af03a028UL   /* -[CBABModuleiOS shouldMitigateHarmony:] (20F66) */
#define CB_ABMODULE_AABC_OFFSET         0x1ddda5140UL   /* the ivar offset it reads the AABC with */

static BOOL (*orig_should_mitigate_harmony)(id, SEL, id);

static BOOL
should_mitigate_harmony(id self, SEL selector, id service)
{
	int32_t offset = *(const int32_t *)(CB_ABMODULE_AABC_OFFSET + slide);

	if (self == NULL || *(void *const *)((const uint8_t *)(const void *)self + offset) == NULL) {
		return NO;
	}
	return orig_should_mitigate_harmony(self, selector, service);
}

/*
 * -[CBABModuleiOS addHIDServiceClient:] makes the module's AABC -- what turns the light into a brightness -- only
 * for a light sensor it finds in the IORegistry by the service's RegistryID: IOServiceGetMatchingService(
 * IORegistryEntryIDMatching(id)) != 0 (0x1af039708 .. 0x1af039754), and AABC::open() then reads the sensor's tuning
 * from that entry (ALSBrightenPdeltaSlow, alsCalibration, crgb, ...; every one of them may be missing). Ours is a
 * virtual HID service: its RegistryID is one the event server made up, no entry has it, and no AABC was made --
 * Auto-Brightness was on and the screen stayed as bright under a torch as in the dark (2026-10-30). For that one
 * call the service gives the kernel's nub HakiosALS instead (iokit/Kernel/hakios.sensors.cpp), an entry with no
 * tuning: CoreBrightness's defaults. The ID goes back right after; the event server knows the service by its own.
 */
#define CB_ADD_HID_SERVICE_CLIENT       0x1af0395d8UL   /* -[CBABModuleiOS addHIDServiceClient:] (20F66) */
#define CB_AB_HANDLE_EVENT              0x1aefa3fb4UL   /* -[CBABModuleiOS handleHIDEvent:from:] */
#define SERVICE_CLIENT_REGISTRY_ID      0x10            /* IOHIDServiceClientGetRegistryID(): ldr x0, [x0, #0x10]; ret */

typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
extern CFTypeRef IOHIDServiceClientGetRegistryID(IOHIDServiceClientRef service);
extern Boolean IOHIDServiceClientConformsTo(IOHIDServiceClientRef service, uint32_t page, uint32_t usage);

static void (*orig_add_hid_service_client)(id, SEL, IOHIDServiceClientRef);

static bool
in_registry(CFTypeRef number)
{
	uint64_t entry_id;
	io_service_t entry;

	if (number == NULL || CFGetTypeID(number) != CFNumberGetTypeID() ||
	    !CFNumberGetValue((CFNumberRef)number, kCFNumberSInt64Type, &entry_id)) {
		return false;
	}
	entry = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(entry_id));
	if (entry == IO_OBJECT_NULL) {
		return false;
	}
	IOObjectRelease(entry);
	return true;
}

/* (what CoreBrightness did with the light: light_thread() says it every 30 s, where the log's tail keeps it) */
static volatile unsigned cb_offered, cb_lights;
static id cb_module;                    /* the CBABModuleiOS the light went to */

/* "KEY=value" of the AB module's own properties (-copyPropertyForKey:, AABC::getPropertyForClient()), into `text` */
static void
cb_properties(char *text, size_t size)
{
	static const char *const keys[] = { "TrustedLux", "nits", "BrightnessCurveLevel", "CBAutoBrightnessAvailable",
		"ALSSAvailable", "ALSUserPreference", "SemanticAmbientLightLevel", "ALSCurveInfo", "ALSInternalSettings" };
	size_t at = 0;

	text[0] = 0;
	for (unsigned i = 0; cb_module != NULL && i < sizeof(keys) / sizeof(keys[0]) && at < size - 1; i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
		CFTypeRef value = ((CFTypeRef (*)(id, SEL, CFStringRef))objc_msgSend)(cb_module,
		    sel_registerName("copyPropertyForKey:"), key);
		char shown[700] = "-";

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			double number = 0;

			CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &number);
			snprintf(shown, sizeof(shown), "%g", number);
		} else if (value != NULL && CFGetTypeID(value) == CFBooleanGetTypeID()) {
			strlcpy(shown, CFBooleanGetValue((CFBooleanRef)value) ? "yes" : "no", sizeof(shown));
		} else if (value != NULL) {
			CFStringRef description = CFCopyDescription(value);

			if (description != NULL) {
				CFStringGetCString(description, shown, sizeof(shown), kCFStringEncodingUTF8);
				CFRelease(description);
			}
		}
		if (value != NULL) {
			CFRelease(value);
		}
		/* (squeezed: the log's lines end at some 500 characters; a long one goes out in pieces of its own) */
		size_t out = 0;

		for (size_t in = 0; shown[in] != 0; in++) {
			char c = shown[in] == '\n' || shown[in] == '\t' ? ' ' : shown[in];

			if (c != ' ' || (out > 0 && shown[out - 1] != ' ')) {
				shown[out++] = c;
			}
		}
		shown[out] = 0;
		if (out > 300) {
			for (size_t piece = 0; piece < out; piece += 300) {
				say("hakios: light: %s [%zu]: %.300s\n", keys[i], piece / 300, shown + piece);
			}
		} else {
			at += (size_t)snprintf(text + at, size - at, " %s=%s", keys[i], shown);
		}
		CFRelease(key);
	}
}

static void *
cb_aabc(void)
{
	int32_t offset = *(const int32_t *)(CB_ABMODULE_AABC_OFFSET + slide);

	return cb_module == NULL ? NULL : *(void *const *)((const uint8_t *)(const void *)cb_module + offset);
}

static void
add_hid_service_client(id self, SEL selector, IOHIDServiceClientRef service)
{
	CFTypeRef *slot = service ? (CFTypeRef *)(void *)((uint8_t *)service + SERVICE_CLIENT_REGISTRY_ID) : NULL;
	CFTypeRef own_id;
	CFNumberRef nub_id = NULL;
	io_service_t nub;
	uint64_t entry_id = 0;
	bool light = service != NULL && IOHIDServiceClientConformsTo(service, 0xff00, 4);

	cb_offered++;
	if (!light || in_registry(*slot)) {
		orig_add_hid_service_client(self, selector, service);
		if (light) {
			cb_lights++;
			cb_module = self;
			say("hakios: CoreBrightness: a light sensor of the registry's taken; its AABC %p\n", cb_aabc());
		}
		return;
	}
	cb_lights++;
	cb_module = self;
	nub = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("HakiosALS"));
	if (nub != IO_OBJECT_NULL) {
		if (IORegistryEntryGetRegistryEntryID(nub, &entry_id) == KERN_SUCCESS) {
			nub_id = CFNumberCreate(NULL, kCFNumberSInt64Type, &entry_id);
		}
		IOObjectRelease(nub);
	}
	if (nub_id == NULL) {
		say("hakios: CoreBrightness: no HakiosALS in the registry: the light sensor gets no AABC\n");
		orig_add_hid_service_client(self, selector, service);
		return;
	}
	own_id = *slot;
	*slot = nub_id;
	orig_add_hid_service_client(self, selector, service);
	*slot = own_id;
	CFRelease(nub_id);
	say("hakios: CoreBrightness: the light sensor taken with the registry's HakiosALS (entry %#llx); its AABC %p\n",
	    (unsigned long long)entry_id, cb_aabc());
}

/*
 * (and what reaches the module: -[CBABModuleiOS handleHIDEvent:from:], counted, the light's events apart -- the light
 * went in and the screen's brightness did not move, 2026-10-30, with nothing in the log to say where it stopped)
 */
extern uint32_t IOHIDEventGetType(void *event);
static uintptr_t (*orig_ab_handle_event)(id, SEL, void *, IOHIDServiceClientRef);
static volatile unsigned cb_events, cb_light_events;
static volatile uintptr_t cb_light_answer = 0xdead;

static uintptr_t
ab_handle_event(id self, SEL selector, void *event, IOHIDServiceClientRef service)
{
	uintptr_t answer = orig_ab_handle_event(self, selector, event, service);

	cb_events++;
	if (event != NULL && IOHIDEventGetType(event) == 12 /* ambient light */) {
		cb_light_answer = answer & 0xff;
		if (cb_light_events++ < 3) {
			say("hakios: CoreBrightness: the AB module was given a light event (from a light: %d); it answered %lu, its AABC %p\n",
			    service != NULL && IOHIDServiceClientConformsTo(service, 0xff00, 4), (unsigned long)(answer & 0xff),
			    *(void *const *)((const uint8_t *)(const void *)self + *(const int32_t *)(CB_ABMODULE_AABC_OFFSET + slide)));
		}
	}
	return answer;
}

/*
 * Boot-arg cblog=1: CoreBrightness's own words on the console. They are os_log messages of the debug level, which
 * neither the subsystem's level in /private/var/preferences/Logging nor OS_ACTIVITY_MODE=debug brought out
 * (2026-10-30); libsystem_trace's in-process hook does (os_log_set_hook, which also turns those levels on in the
 * process: os_log_type_enabled() consults it). Only the messages whose format string is CoreBrightness's own --
 * its __TEXT, where its __oslogstring is -- are written out, formatted by os_log_copy_message_string().
 */
typedef void (^os_log_hook_block_t)(uint8_t type, void *message);
extern os_log_hook_block_t os_log_set_hook(uint8_t level, os_log_hook_block_t hook);
extern char *os_log_copy_message_string(void *message);
#define CB_TEXT_START                   0x1aef00000UL
#define CB_TEXT_END                     0x1af120000UL
#define OS_LOG_MESSAGE_FORMAT           0x58            /* (os_log_copy_message_string() reads it there) */

/*
 * Auto-brightness's last step. CoreBrightness works out the light the screen should give (AABC: the lux, its
 * curve, what the user's slider taught it) and ramps to it in nits -- "[BRT update: ALS]: Begin ramp: L_target=N
 * ... timeConstant=T". On this panel's legacy backlight path the ramp ends in CoreBrightness's own model: what it
 * writes to the backlight is the slider's level alone, so the screen never moved (2026-10-30); the path in nits
 * ("new-backlight-architecture") waits for display hardware this phone has not, and the screen stopped drawing.
 * So that message, which CoreBrightness says at the default level whenever an ALS ramp begins, is taken at its
 * word: the target goes to the backlight in nits ("brightness-nits", 16.16, which the kernel's AppleARMBacklight
 * maps on its calibration), faded over the ramp's time (the kernel caps a fade at 5 s).
 */
static io_service_t cb_backlight;
/* (the SDK marks it unavailable on iOS; IOKit has it, and CoreBrightness sets the backlight with it) */
extern kern_return_t registry_set_properties(io_registry_entry_t entry, CFTypeRef properties)
    __asm__("_IORegistryEntrySetCFProperties");

static void
ramp_to(double nits, double seconds)
{
	static unsigned said;
	int32_t value = (int32_t)(nits * 65536.0), fade = (int32_t)((seconds > 5.0 ? 5.0 : seconds < 0.0 ? 0.0 : seconds) * 65536.0);
	CFNumberRef value_number, fade_number;
	CFMutableDictionaryRef properties;
	kern_return_t kr;

	if (nits <= 0.0 || nits > 2000.0) {
		return;
	}
	if (cb_backlight == IO_OBJECT_NULL) {
		cb_backlight = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleARMBacklight"));
		if (cb_backlight == IO_OBJECT_NULL) {
			say("hakios: auto-brightness: no AppleARMBacklight\n");
			return;
		}
	}
	properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	value_number = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
	fade_number = CFNumberCreate(NULL, kCFNumberSInt32Type, &fade);
	CFDictionarySetValue(properties, CFSTR("brightness-nits"), value_number);
	CFDictionarySetValue(properties, CFSTR("FadePeriod"), fade_number);
	kr = registry_set_properties(cb_backlight, properties);
	CFRelease(value_number);
	CFRelease(fade_number);
	CFRelease(properties);
	if (said++ < 40) {
		say("hakios: auto-brightness: %.1f nits over %.1f s (%#x)\n", nits, seconds, kr);
	}
}

static void
ramp_message(const char *text)
{
	const char *at = strstr(text, "[BRT update: ALS]: Begin ramp: L_target=");
	const char *tc;

	if (at == NULL || (tc = strstr(at, "timeConstant=")) == NULL) {
		return;
	}
	ramp_to(strtod(at + strlen("[BRT update: ALS]: Begin ramp: L_target="), NULL), strtod(tc + strlen("timeConstant="), NULL));
}

/* slpi=1: the ramps followed; cblog=1: and every message of CoreBrightness's written out, at all levels */
static void
cb_log_on(bool everything)
{
	os_log_set_hook(everything ? 2 /* OS_LOG_TYPE_DEBUG */ : 0 /* OS_LOG_TYPE_DEFAULT */, ^(uint8_t type, void *message) {
		uintptr_t format = message ? *(const uintptr_t *)((const uint8_t *)message + OS_LOG_MESSAGE_FORMAT) : 0;
		char *text;

		if (format < CB_TEXT_START + slide || format >= CB_TEXT_END + slide) {
			return;
		}
		if ((text = os_log_copy_message_string(message)) != NULL) {
			if (everything) {
				say("cb[%u]: %.400s\n", type, text);
			}
			ramp_message(text);
			free(text);
		}
	});
	say("hakios: CoreBrightness's ramps followed%s\n", everything ? ", and its own messages to the console (cblog=1)" : "");
}

static void
light_sensor_fixes(void)
{
	Class cls = objc_getClass("CBABModuleiOS");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("shouldMitigateHarmony:")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;
	const uint32_t *get_id = (const uint32_t *)(const void *)IOHIDServiceClientGetRegistryID;

	if (original == NULL || (uintptr_t)original != CB_SHOULD_MITIGATE_HARMONY + slide) {
		say("hakios: -[CBABModuleiOS shouldMitigateHarmony:]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	orig_should_mitigate_harmony = (BOOL (*)(id, SEL, id))original;
	method_setImplementation(method, (IMP)should_mitigate_harmony);
	say("hakios: CoreBrightness: a light sensor that comes late no longer finds a NULL AABC\n");

	method = class_getInstanceMethod(cls, sel_registerName("addHIDServiceClient:"));
	original = method ? method_getImplementation(method) : NULL;
	if (original == NULL || (uintptr_t)original != CB_ADD_HID_SERVICE_CLIENT + slide ||
	    get_id[0] != 0xf9400800 /* ldr x0, [x0, #0x10] */ || get_id[1] != 0xd65f03c0 /* ret */) {
		say("hakios: -[CBABModuleiOS addHIDServiceClient:]: found %#lx (RegistryID %08x %08x), not what was "
		    "expected; left alone\n", original ? (uintptr_t)original - slide : 0, get_id[0], get_id[1]);
		return;
	}
	orig_add_hid_service_client = (void (*)(id, SEL, IOHIDServiceClientRef))original;
	method_setImplementation(method, (IMP)add_hid_service_client);
	say("hakios: CoreBrightness: the light sensor will be found in the registry\n");

	method = class_getInstanceMethod(cls, sel_registerName("handleHIDEvent:from:"));
	original = method ? method_getImplementation(method) : NULL;
	if (original == NULL || (uintptr_t)original != CB_AB_HANDLE_EVENT + slide) {
		say("hakios: -[CBABModuleiOS handleHIDEvent:from:]: found %#lx, not what was expected; not watched\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	orig_ab_handle_event = (uintptr_t (*)(id, SEL, void *, IOHIDServiceClientRef))original;
	method_setImplementation(method, (IMP)ab_handle_event);
	say("hakios: CoreBrightness: -[CBABModuleiOS handleHIDEvent:from:] (%s) watched\n", method_getTypeEncoding(method));
}

/* ---- backboardd: the device's orientation ------------------------------------------------------------- */

/*
 * backboardd's BKOrientationManager has the device's orientation from CoreMotion's CMDeviceOrientationManager --
 * -startDeviceOrientationUpdatesToQueue:withHandler: while SpringBoard wants orientation events, and
 * -deviceOrientationBlocking when it asks -- which reads the AOP's orientation service, or an accelerometer and a
 * gyro of the iPhone's own. None of them is here ("[Orientation] fHidDeviceInterface NULL", "missing accel HID
 * service plugin"): the screen never turned. The sensor DSP knows the orientation (sysctl kern.hakios.orientation,
 * bsd/dev/hakios.slpi.c, Qualcomm's device_orient: portrait, upside down and the two landscapes, never face up or
 * down); with slpi=1 those methods give that instead, looked at ten times a second (work/research/sensors-ios.md).
 */
#define CM_START_ORIENTATION    0x19199d3f0UL   /* -[CMDeviceOrientationManager startDeviceOrientationUpdatesToQueue:withHandler:] */
#define CM_STOP_ORIENTATION     0x191c5288cUL   /* -[CMDeviceOrientationManager stopDeviceOrientationUpdates] */
#define CM_ORIENTATION_BLOCKING 0x1919a32ccUL   /* -[CMDeviceOrientationManager deviceOrientationBlocking] (20F66) */

static pthread_mutex_t orientation_lock = PTHREAD_MUTEX_INITIALIZER;
static id orientation_queue;                    /* (an NSOperationQueue, retained) */
static void (^orientation_handler)(id, id);     /* (a copy: void (^)(CMDeviceOrientation *, NSError *)) */
static int orientation_now;                     /* a UIDeviceOrientation, 0 while not known */

static int
orientation_read(void)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname("kern.hakios.orientation", &value, &size, NULL, 0) == 0 ? value : -1;
}

/* a CMDeviceOrientation, +1 (-initWithOrientation:andTimestamp: 0x191c51d80: an int, and seconds of mach time) */
static id
orientation_object(int orientation)
{
	Class cls = objc_getClass("CMDeviceOrientation");
	id object = cls ? ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc")) : nil;
	mach_timebase_info_data_t base;

	mach_timebase_info(&base);
	return object == nil ? nil : ((id (*)(id, SEL, int, double))objc_msgSend)(object,
	           sel_registerName("initWithOrientation:andTimestamp:"), orientation,
	           (double)mach_absolute_time() * base.numer / base.denom / 1e9);
}

/* The handler, on its queue, told the orientation (with orientation_lock held). */
static void
orientation_deliver(int orientation)
{
	void (^handler)(id, id) = orientation_handler;
	id object;

	if (orientation_queue == nil || handler == NULL || (object = orientation_object(orientation)) == nil) {
		return;
	}
	/* (the block keeps the handler; the object it releases itself: plain C does not retain an id it takes along) */
	((void (*)(id, SEL, void (^)(void)))objc_msgSend)(orientation_queue, sel_registerName("addOperationWithBlock:"), ^{
		handler(object, nil);
		((void (*)(id, SEL))objc_msgSend)(object, sel_registerName("release"));
	});
}

static void
orientation_start(__unused id self, __unused SEL selector, id queue, void (^handler)(id, id))
{
	static unsigned said;

	pthread_mutex_lock(&orientation_lock);
	if (orientation_queue != nil) {
		((void (*)(id, SEL))objc_msgSend)(orientation_queue, sel_registerName("release"));
	}
	if (orientation_handler != NULL) {
		Block_release(orientation_handler);
	}
	orientation_queue = queue != nil ? ((id (*)(id, SEL))objc_msgSend)(queue, sel_registerName("retain")) : nil;
	orientation_handler = handler != NULL ? Block_copy(handler) : NULL;
	if (orientation_now > 0) {
		orientation_deliver(orientation_now);
	}
	pthread_mutex_unlock(&orientation_lock);
	if (said++ < 8) {
		say("hakios: orientation: updates wanted (now %d)\n", orientation_now);
	}
}

static void
orientation_stop(__unused id self, __unused SEL selector)
{
	pthread_mutex_lock(&orientation_lock);
	if (orientation_queue != nil) {
		((void (*)(id, SEL))objc_msgSend)(orientation_queue, sel_registerName("release"));
		orientation_queue = nil;
	}
	if (orientation_handler != NULL) {
		Block_release(orientation_handler);
		orientation_handler = NULL;
	}
	pthread_mutex_unlock(&orientation_lock);
}

static id
orientation_blocking(__unused id self, __unused SEL selector)
{
	int now = orientation_read();
	id object = orientation_object(now > 0 ? now : 0);

	return object == nil ? nil : ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("autorelease"));
}

static void *
orientation_thread(__unused void *arg)
{
	unsigned changes = 0;

	pthread_setname_np("redwood orientation");
	for (;;) {
		int value = orientation_read();

		if (value < 0) {
			say("hakios: orientation: no kern.hakios.orientation: not looked at\n");
			return NULL;
		}
		if (value > 0 && value != orientation_now) {
			pthread_mutex_lock(&orientation_lock);
			orientation_now = value;
			orientation_deliver(value);
			pthread_mutex_unlock(&orientation_lock);
			if (changes++ < 40) {
				say("hakios: orientation: %d (UIDeviceOrientation)\n", value);
			}
		}
		usleep(100000);
	}
}

static void
orientation_fixes(void)
{
	static const struct {
		const char *name;
		uintptr_t expected;
		IMP ours;
	} methods[] = {
		{ "startDeviceOrientationUpdatesToQueue:withHandler:", CM_START_ORIENTATION, (IMP)orientation_start },
		{ "stopDeviceOrientationUpdates", CM_STOP_ORIENTATION, (IMP)orientation_stop },
		{ "deviceOrientationBlocking", CM_ORIENTATION_BLOCKING, (IMP)orientation_blocking },
	};
	Class cls = objc_getClass("CMDeviceOrientationManager");
	Method found[3];
	pthread_t thread;

	for (unsigned i = 0; i < 3; i++) {
		IMP original;

		found[i] = cls ? class_getInstanceMethod(cls, sel_registerName(methods[i].name)) : NULL;
		original = found[i] ? method_getImplementation(found[i]) : NULL;
		if (original == NULL || (uintptr_t)original != methods[i].expected + slide) {
			say("hakios: -[CMDeviceOrientationManager %s]: found %#lx, not what was expected; the orientation left alone\n",
			    methods[i].name, original ? (uintptr_t)original - slide : 0);
			return;
		}
	}
	for (unsigned i = 0; i < 3; i++) {
		method_setImplementation(found[i], methods[i].ours);
	}
	if (pthread_create(&thread, NULL, orientation_thread, NULL) == 0) {
		pthread_detach(thread);
	}
	say("hakios: orientation: CoreMotion's in backboardd is the sensor DSP's (kern.hakios.orientation)\n");
}

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * The ambient light sensor (boot-arg slpi=1: the sensor DSP's, which the kernel reads -- sysctl
 * kern.hakios.als_millilux, bsd/dev/hakios.slpi.c step 13): a service of Apple's vendor page, usage 4,
 * that sends an IOHIDEventTypeAmbientLightSensor event (12) whenever the light changes and answers
 * CoreBrightness's asking for one (copyEvent) with the last. Its level is in lux; iOS 16.5's
 * IOHIDEventCreateAmbientLightSensorEvent(allocator, time, level, options) takes only that, the
 * illuminance as a float is a field of its own (kIOHIDEventFieldAmbientLightSensorIlluminance).
 */
extern IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef allocator, uint64_t time, int32_t level,
    uint32_t options);
extern void IOHIDEventSetFloatValue(IOHIDEventRef event, uint32_t field, double value);
#define HID_PAGE_APPLE_VENDOR           0xff00
#define HID_USAGE_AMBIENT_LIGHT         0x04
#define HID_EVENT_AMBIENT_LIGHT         12
#define FIELD_ALS_ILLUMINANCE           0xc000b /* (0xc000a is the colour temperature: work/research/sensors-ios.md) */

static volatile int als_millilux = -1;
static volatile unsigned als_asked;             /* (copyEvent calls) */

static IOHIDEventRef
als_event(void)
{
	int milli = als_millilux;
	IOHIDEventRef event = IOHIDEventCreateAmbientLightSensorEvent(NULL, mach_absolute_time(), milli < 0 ? 0 : milli / 1000, 0);

	if (event != NULL) {
		IOHIDEventSetFloatValue(event, FIELD_ALS_ILLUMINANCE, milli < 0 ? 0.0 : milli / 1000.0);
		IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	}
	return event;
}

static IOHIDEventRef
als_copy_event(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, uint32_t type,
    __unused IOHIDEventRef matching, __unused uint32_t options)
{
	if (als_asked++ < 4) {
		say("hakios: input: the light asked for (event type %u): %d mlx\n", type, als_millilux);
	}
	return type == HID_EVENT_AMBIENT_LIGHT ? als_event() : NULL;
}

static const struct virtual_service_callbacks als_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property, .copyEvent = als_copy_event,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service_with(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display,
    const struct virtual_service_callbacks *callbacks);

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	return make_service_with(client, product, page, usage, display, &service_callbacks);
}

static IOHIDServiceClientRef
make_service_with(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display,
    const struct virtual_service_callbacks *callbacks)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

/* The light as the kernel last read it, looked at 4 times a second; an event to the event server when it changes. */
static void *
light_thread(void *arg)
{
	IOHIDServiceClientRef light = arg;
	int last = -2, events = -1, sent = 0;
	unsigned quiet = 0;

	pthread_setname_np("redwood light");
	for (unsigned turn = 1;; turn++) {
		int milli = -1, count = 0;
		size_t size = sizeof(milli);

		if (turn % 120 == 0) {
			char properties[1800];

			say("hakios: light: %d mlx, %d events sent, asked for %u times; CoreBrightness: offered %u services, %u a "
			    "light, AABC %p; its AB module given %u events, %u of the light (the last answered %lu)\n", als_millilux,
			    sent, als_asked, cb_offered, cb_lights, cb_aabc(), cb_events, cb_light_events,
			    (unsigned long)cb_light_answer);
			cb_properties(properties, sizeof(properties));
			say("hakios: light: the AB module says%s\n", properties);
		}

		if (sysctlbyname("kern.hakios.als_millilux", &milli, &size, NULL, 0) != 0) {
			say("hakios: input: no kern.hakios.als_millilux: the light is not looked at\n");
			return NULL;
		}
		size = sizeof(count);
		(void)sysctlbyname("kern.hakios.als_events", &count, &size, NULL, 0);
		als_millilux = milli;
		/* a reading the sensor sent, or the same light again after 2 s (CoreBrightness averages over time) */
		if (milli >= 0 && (count != events || milli != last || ++quiet >= 8)) {
			IOHIDEventRef event = als_event();

			if (event != NULL) {
				IOHIDVirtualServiceClientDispatchEvent(light, event);
				CFRelease(event);
			}
			if (sent++ < 4) {
				say("hakios: input: the light: %d.%03d lux\n", milli / 1000, milli % 1000);
			}
			last = milli;
			events = count;
			quiet = 0;
		}
		usleep(250000);
	}
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL, light = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	bool light_wanted = false;
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
			light_wanted = strstr(arguments, "slpi=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("hakios: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	if (screen != NULL && light_wanted) {
		pthread_t thread;

		light = make_service_with(client, "HakiOS ambient light", HID_PAGE_APPLE_VENDOR, HID_USAGE_AMBIENT_LIGHT, false,
		    &als_callbacks);
		if (light != NULL && pthread_create(&thread, NULL, light_thread, (void *)light) == 0) {
			pthread_detach(thread);
		}
	}
	say("hakios: input: touch screen service %p, buttons %p, ambient light %p\n", (void *)screen, (void *)buttons, (void *)light);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
static void
opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return;                                 /* (an image built without it) */
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		functions_said_yes(arguments);
		compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				wanted = argument[3] != '0';
				told = 1;
			}
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("hakios: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(HAKIOS_GL, RTLD_NOW)) == NULL) {
		say("hakios: %s: %s\n", HAKIOS_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "hakios_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "hakios_gl_function_refused");
}

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "backboardd") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/*
		 * Boot-arg ca_vbl=1: CoreAnimation's frame clock from the display driver's refresh
		 * notifications. QuartzCore takes them (IOMobileFramebufferEnableVSyncNotifications) only on
		 * the phones with an A7 to A10, or when CA_FORCE_TIMER is in the environment
		 * (CADeviceUseVBL()); on the iPhone X it runs a timer of its own instead (CADisplayTimer),
		 * whose phase it would set from the frame timings of a display driver this one is not.
		 */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				setenv(setting, equals + 1, 1);
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		opengl_es();
		window_server_fixes();
		if (strstr(arguments, "slpi=1") != NULL || strstr(arguments, "cblog=1") != NULL) {
			cb_log_on(strstr(arguments, "cblog=1") != NULL);
		}
		light_sensor_fixes();
		if (strstr(arguments, "slpi=1") != NULL) {
			orientation_fixes();
		}
		/*
		 * brightness_fixes() is not used for now: with it backboardd no longer took our touch
		 * screen service for its own ("still waiting for a main screen digitizer"), whether the
		 * service was made before or after the touch processing starts. (Most likely it only
		 * moved the start of the event server onto the moment make_service() now knows how to
		 * survive; not tried again since, and until it is, the five seconds stay.)
		 */
		input_start();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	}
}
