/*
 * libhakios.pb.dylib: inside PosterBoard (etc/inject.conf), where its wallpaper store lives.
 *
 * (2026-11-25, WEEK-1121 N6b) From upstream's umi_posterboard.m (HakiOS, JopangJovan/hackios-xnu-kernel-umi c7dcce0,
 * devices/xiaomi-mi-10/overlays/userland/src; MIT, as this file): in a RAM boot PosterBoard has no container, so
 * -[PBFPosterExtensionDataStoreXPCServiceGlue _baseDataStoreURL] answers the SHARED /var/mobile/Library/Application
 * Support, and PosterBoard's store reset then deletes what other services keep there -- ScreenTimeAgent's database
 * among them ("CoreData: XPC: Can't create coordinator ... Failed to create file; code = 2" every 3 s, 081637-play.log;
 * and N6's directory made in var.skel was gone again, 11-22).
 *
 * With boot-arg pb_store=1, when that answer is the shared directory, PosterBoard gets its own subdirectory of it
 * (com.apple.PosterBoard, 0700) instead; any other answer is left alone, and so is everything without the boot-arg.
 * If the subdirectory cannot be made, the shared one stays (as before) and the console says why.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static NSURL *(*original_base_store_url)(id, SEL);

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
	/*
	 * ... and into /var/tmp/libhakios.pb.txt, which netsh can read: PosterBoard's console lines never reach the
	 * kernel's log (runs 091645, 092454: none, while the library was there -- its mark in 094843). Only with
	 * pb_store=1: nothing calls this without it.
	 */
	int file = open("/var/tmp/libhakios.pb.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);

	if (file >= 0) {
		(void)write(file, line, (size_t)k);
		close(file);
	}
}

static bool
boot_arg_on(const char *name)
{
	char args[1100] = "";
	size_t n = sizeof(args) - 1, len = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0) {
		return false;
	}
	for (char *p = strstr(args, name); p != NULL; p = strstr(p + 1, name)) {
		if ((p == args || p[-1] == ' ') && p[len] == '=' && p[len + 1] != '0') {
			return true;
		}
	}
	return false;
}

static NSURL *
base_store_url(id service, SEL selector)
{
	static unsigned said;
	NSURL *base = original_base_store_url(service, selector);
	NSString *path = base.path;

	if (!base.isFileURL || (![path isEqualToString:@"/var/mobile/Library/Application Support"] &&
	    ![path isEqualToString:@"/private/var/mobile/Library/Application Support"])) {
		return base;
	}
	NSURL *store = [base URLByAppendingPathComponent:@"com.apple.PosterBoard" isDirectory:YES];
	NSError *error = nil;

	if (![NSFileManager.defaultManager createDirectoryAtURL:store withIntermediateDirectories:NO
	    attributes:@{NSFilePosixPermissions: @0700} error:&error] &&
	    !(error.code == NSFileWriteFileExistsError && [error.domain isEqualToString:NSCocoaErrorDomain])) {
		say("pb_store=1: its own store directory could not be made (%s %ld): the shared one stays\n",
		    error.domain.UTF8String, (long)error.code);
		return base;
	}
	if (said++ == 0) {
		say("pb_store=1: the store base was the shared %s -> its own %s\n", path.UTF8String, store.path.UTF8String);
	}
	return store;
}

/* true once the method is replaced, or found not to be what was expected (then left alone for good) */
static bool
install(void)
{
	static bool done;

	if (done) {
		return true;
	}
	Class glue = objc_getClass("PBFPosterExtensionDataStoreXPCServiceGlue");

	if (glue == Nil) {
		return false;                           /* (its framework not loaded yet: image_added tries again) */
	}
	done = true;
	Method method = class_getInstanceMethod(glue, sel_registerName("_baseDataStoreURL"));

	if (method == NULL || strcmp(method_getTypeEncoding(method), "@16@0:8") != 0) {
		say("pb_store=1: -[PBFPosterExtensionDataStoreXPCServiceGlue _baseDataStoreURL] is not as expected: "
		    "nothing changed\n");
		return true;
	}
	original_base_store_url = (NSURL *(*)(id, SEL))method_getImplementation(method);
	method_setImplementation(method, (IMP)base_store_url);
	say("pb_store=1: the store base is checked (its own directory when it is the shared Application Support)\n");
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
pb_init(void)
{
	if (strcmp(getprogname(), "PosterBoard") != 0 || !boot_arg_on("pb_store")) {
		return;
	}
	say("pb_store=1: loaded\n");
	if (!install()) {
		say("pb_store=1: PBFPosterExtensionDataStoreXPCServiceGlue not there yet: waiting for the images to come\n");
		/* (called at once for every image already there, then for each one loaded later) */
		_dyld_register_func_for_add_image(image_added);
	}
}
