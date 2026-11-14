// ipainstall: put an application on the phone the way the original release had its extra one (RedwoodTriangle.app,
// unsigned, in /Applications of its system image, which LaunchServices registers at every boot): unpack the IPA
// into a bundle directory of its own, make its data container, and register it with LaunchServices as a system
// application. installd is not asked: it refuses whatever Apple has not signed ("No code signature found",
// MICodeSigningVerifier), and the kernel runs such programs as they are (miniamfi: no signature policy).
// But an application's entitlements live in its signature, and an unsigned or decrypted IPA has none the kernel
// reads: so every Mach-O of the bundle is signed here, ad hoc (src/adhoc_sign.c) -- the application's and its
// extensions' executables with their own entitlements, or with what an app expects of its signature when they have
// none (its app group, keychain group, iCloud), everything else without (boot-arg ipa_sign=0: left as it comes).
//   ipainstall PATH.ipa        install (again: the old bundle goes, the data container stays)
//   ipainstall URL             the same with an IPA downloaded first (https://, or http:// from the computer)
//   ipainstall -r PATH.app     register an application directory where it is (read-only images: /redwood/apps)
//   ipainstall -a DIR          every DIR/*.ipa and DIR/*.app, as above
//   ipainstall -u BUNDLE-ID    uninstall
//   ipainstall -l              the applications that are not the image's own
//   ipainstall -d APPS SPOOL   the service (job com.hakios.apps, root): APPS as -a at boot; with boot-arg
//                              apps_token= the IPAs the computer pushes (start.ps1, the release's apps\ folder:
//                              listen_for_computer() below); and the requests the application IPA Installer
//                              (src/installer) leaves in SPOOL/queue, forever
//   ipainstall -s BUNDLE-ID STATE.zip   put an application's saved state back (below)
//   ipainstall -S BUNDLE-ID OUT.zip     save it
// /private/var is a RAM file system here: what is installed lasts until the phone restarts -- and what an application
// keeps there too (a messenger's login, say). Its STATE -- its data container and its app group containers, as a zip
// (data/..., group/<GROUP-ID>/...) -- can go to the computer (SAVE over the computer's connection, or -S) and come
// back with the IPA at the next boot (STATE, or -s): the computer keeps it (start.ps1's apps\state\BUNDLE-ID.zip).
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zlib.h>
#include "adhoc_sign.h"

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary;
- (BOOL)unregisterApplication:(NSURL *)url;
- (NSArray *)allInstalledApplications;
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@property (readonly) NSString *applicationIdentifier;
@property (readonly) NSString *applicationType;
@property (readonly) NSURL *bundleURL;
@property (readonly) NSString *localizedName;
@property (readonly) NSString *shortVersionString;
@end

@interface MCMContainer : NSObject
+ (instancetype)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create existed:(BOOL *)existed
    error:(NSError **)error;
@property (readonly) NSURL *url;
@end

// (libproc: not in this SDK's headers)
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

#define BUNDLES "/private/var/containers/Bundle/Application"
#define DATA "/private/var/mobile/Containers/Data/Application"
#define GROUPS "/private/var/mobile/Containers/Shared/AppGroup"
#define CONTAINER_META ".com.apple.mobile_container_manager.metadata.plist"
#define MOBILE 501

// what is said goes to standard output, and into the log of the request being served, if any
static NSMutableString *request_log;
static void (^request_progress)(void);

static void say(const char *fmt, ...) __printflike(1, 2);

static void
say(const char *fmt, ...)
{
	va_list ap;
	char *line = NULL;

	va_start(ap, fmt);
	vasprintf(&line, fmt, ap);
	va_end(ap);
	if (line == NULL) {
		return;
	}
	fputs(line, stdout);
	fflush(stdout);
	if (request_log != nil) {
		NSString *text = @(line);

		[request_log appendString:[text hasPrefix:@"ipainstall: "] ? [text substringFromIndex:12] : text];
		if (request_progress != nil) {
			request_progress();
		}
	}
	free(line);
}

// ---------------------------------------------------------------- the IPA (a zip file)

static uint16_t
le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t
le32(const uint8_t *p)
{
	return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint64_t
le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

// the directories above PATH, made as they are needed
static int
make_parents(const char *path)
{
	char buf[PATH_MAX];

	strlcpy(buf, path, sizeof(buf));
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}
	return 0;
}

static int
write_entry(const char *path, const uint8_t *data, uint64_t csize, uint64_t size, int method, mode_t mode)
{
	if (method != 0 && method != 8) {
		say("ipainstall: %s: compression method %d, which this does not unpack\n", path, method);
		return -1;
	}
	if (S_ISLNK(mode)) {
		char target[PATH_MAX];

		if (method != 0 || size >= sizeof(target)) {
			return -1;
		}
		memcpy(target, data, size);
		target[size] = 0;
		unlink(path);
		return symlink(target, path);
	}
	/*
	 * (the K40's) Every file readable by the application, as installd leaves a bundle: the zip's mode says who could
	 * read a file on the computer that made it, and here a bundle belongs to root while the application runs as
	 * mobile. Flutter's engine ships its ICU data at 0600 (Flutter.framework/icudtl.dat) and stopped at launch for
	 * want of reading it (Flutter Check, 2026-11-10). Executable stays executable.
	 */
	mode_t perm = (mode & 0111) ? 0755 : 0644;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, perm);

	if (fd < 0) {
		return -1;
	}
	(void)fchmod(fd, perm);         /* (a file there before keeps its old mode through O_TRUNC) */
	int ret = 0;

	if (method == 0) {
		ret = write(fd, data, size) == (ssize_t)size ? 0 : -1;
	} else {
		static uint8_t out[1 << 20];
		z_stream z = { .next_in = (Bytef *)data, .avail_in = (uInt)csize };
		uint64_t total = 0;
		int zr = Z_OK;

		inflateInit2(&z, -MAX_WBITS);
		while (zr == Z_OK) {
			z.next_out = out;
			z.avail_out = sizeof(out);
			zr = inflate(&z, Z_NO_FLUSH);
			if (zr != Z_OK && zr != Z_STREAM_END) {
				break;
			}
			size_t n = sizeof(out) - z.avail_out;

			if (write(fd, out, n) != (ssize_t)n) {
				zr = Z_ERRNO;
				break;
			}
			total += n;
		}
		inflateEnd(&z);
		if (zr != Z_STREAM_END || total != size) {
			say("ipainstall: %s: inflate %d, %llu of %llu bytes\n", path, zr, total, size);
			ret = -1;
		}
	}
	close(fd);
	return ret;
}

// Every entry of the zip file IPA (an IPA, or an application's state) that MAP gives a path for, written there (nil:
// left out; names with ".." never get there); NO when one could not be
static BOOL
unzip_each(NSString *ipa, NSString *(^map)(NSArray<NSString *> *parts), unsigned *files_out, uint64_t *bytes_out)
{
	int fd = open(ipa.fileSystemRepresentation, O_RDONLY);
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 22) {
		say("ipainstall: %s: %s\n", ipa.UTF8String, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return NO;
	}
	const uint8_t *zip = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	size_t len = st.st_size;

	close(fd);
	if (zip == MAP_FAILED) {
		return NO;
	}
	// the end of the central directory, and its 64-bit form when the counts do not fit
	const uint8_t *eocd = NULL;

	for (size_t i = len - 22; i + 65557 >= len && i > 0; i--) {
		if (le32(zip + i) == 0x06054b50) {
			eocd = zip + i;
			break;
		}
	}
	if (eocd == NULL) {
		say("ipainstall: %s: not a zip file\n", ipa.UTF8String);
		munmap((void *)zip, len);
		return NO;
	}
	uint64_t entries = le16(eocd + 10), cd = le32(eocd + 16);

	if ((entries == 0xffff || cd == 0xffffffff) && eocd - zip >= 20 && le32(eocd - 20) == 0x07064b50) {
		uint64_t at = le64(eocd - 20 + 8);

		if (at + 56 <= len && le32(zip + at) == 0x06064b50) {
			entries = le64(zip + at + 32);
			cd = le64(zip + at + 48);
		}
	}
	const uint8_t *p = zip + cd;
	unsigned files = 0;
	uint64_t bytes = 0;
	BOOL failed = NO;

	for (uint64_t i = 0; i < entries; i++) {
		if (p + 46 > zip + len || le32(p) != 0x02014b50) {
			say("ipainstall: %s: central directory entry %llu is damaged\n", ipa.UTF8String, i);
			failed = YES;
			break;
		}
		int made_by = le16(p + 4) >> 8, method = le16(p + 10);
		uint64_t csize = le32(p + 20), size = le32(p + 24), local = le32(p + 42);
		unsigned name_len = le16(p + 28), extra_len = le16(p + 30), comment_len = le16(p + 32);
		mode_t mode = made_by == 3 ? le32(p + 38) >> 16 : 0;
		NSString *name = [[NSString alloc] initWithBytes:p + 46 length:name_len encoding:NSUTF8StringEncoding];

		// (the zip64 extra field: the sizes and the offset that did not fit, in this order)
		for (const uint8_t *x = p + 46 + name_len; x + 4 <= p + 46 + name_len + extra_len; x += 4 + le16(x + 2)) {
			if (le16(x) == 1) {
				const uint8_t *v = x + 4;

				if (size == 0xffffffff) {
					size = le64(v), v += 8;
				}
				if (csize == 0xffffffff) {
					csize = le64(v), v += 8;
				}
				if (local == 0xffffffff) {
					local = le64(v);
				}
			}
		}
		p += 46 + name_len + extra_len + comment_len;
		NSArray *parts = name.pathComponents;

		if (name == nil || [parts containsObject:@".."]) {
			continue;
		}
		NSString *out = map(parts);

		if (out == nil) {
			continue;
		}
		if ([name hasSuffix:@"/"]) {
			make_parents([out stringByAppendingString:@"/"].fileSystemRepresentation);
			continue;
		}
		if (local + 30 > len || le32(zip + local) != 0x04034b50) {
			say("ipainstall: %s: no local header\n", name.UTF8String);
			failed = YES;
			break;
		}
		const uint8_t *data = zip + local + 30 + le16(zip + local + 26) + le16(zip + local + 28);

		if (data + csize > zip + len || make_parents(out.fileSystemRepresentation) != 0 ||
		    write_entry(out.fileSystemRepresentation, data, csize, size, method, mode) != 0) {
			say("ipainstall: %s: not written (%s)\n", out.UTF8String, strerror(errno));
			failed = YES;
			break;
		}
		files++;
		bytes += size;
	}
	munmap((void *)zip, len);
	*files_out = files;
	*bytes_out = bytes;
	return !failed;
}

// Payload/NAME.app of the IPA into DEST/NAME.app; the application's directory, or nil
static NSString *
unpack(NSString *ipa, NSString *dest)
{
	__block NSString *app = nil;
	unsigned files = 0;
	uint64_t bytes = 0;
	BOOL ok = unzip_each(ipa, ^NSString *(NSArray<NSString *> *parts) {
		if (parts.count < 2 || ![parts[0] isEqualToString:@"Payload"] || ![parts[1] hasSuffix:@".app"]) {
			return nil;
		}
		if (app == nil) {
			app = [dest stringByAppendingPathComponent:parts[1]];
		}
		return [dest stringByAppendingPathComponent:
		    [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(1, parts.count - 1)]]];
	}, &files, &bytes);

	if (!ok) {
		return nil;
	}
	if (app == nil) {
		say("ipainstall: %s: no Payload/NAME.app in it\n", ipa.UTF8String);
		return nil;
	}
	say("ipainstall: unpacked %u files, %llu bytes\n", files, bytes);
	return app;
}

// ---------------------------------------------------------------- the data container

// The application's home: containermanagerd's own when it makes one, else a directory laid out as it would
static NSString *
data_container(NSString *bundle_id)
{
	Class mcm = nil;

	if (dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW)) {
		mcm = NSClassFromString(@"MCMAppDataContainer");
	}
	if (mcm != nil) {
		NSError *error = nil;
		BOOL existed = NO;
		MCMContainer *c = [mcm containerWithIdentifier:bundle_id createIfNecessary:YES existed:&existed error:&error];

		if (c.url != nil) {
			say("ipainstall: data container %s (containermanagerd, %s)\n", c.url.path.UTF8String,
			    existed ? "kept" : "new");
			return c.url.path;
		}
		say("ipainstall: containermanagerd made no container: %s\n", error.description.UTF8String);
	}
	NSFileManager *fm = NSFileManager.defaultManager;

	// (one made before for this application: its metadata names it)
	for (NSString *uuid in [fm contentsOfDirectoryAtPath:@DATA error:NULL]) {
		NSString *dir = [@DATA stringByAppendingPathComponent:uuid];
		NSDictionary *meta = [NSDictionary dictionaryWithContentsOfFile:
		    [dir stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"]];

		if ([meta[@"MCMMetadataIdentifier"] isEqualToString:bundle_id]) {
			say("ipainstall: data container %s (kept)\n", dir.UTF8String);
			return dir;
		}
	}
	NSString *uuid = NSUUID.UUID.UUIDString;
	NSString *dir = [@DATA stringByAppendingPathComponent:uuid];

	for (NSString *sub in @[ @"", @"Documents", @"Library", @"Library/Caches", @"Library/Preferences", @"tmp" ]) {
		NSString *d = [dir stringByAppendingPathComponent:sub];

		[fm createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:NULL];
		chown(d.fileSystemRepresentation, MOBILE, MOBILE);
	}
	[@{ @"MCMMetadataIdentifier": bundle_id, @"MCMMetadataContentClass": @2, @"MCMMetadataUUID": uuid }
	    writeToFile:[dir stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"]
	     atomically:YES];
	say("ipainstall: data container %s (new)\n", dir.UTF8String);
	return dir;
}

// ---------------------------------------------------------------- LaunchServices

static BOOL
register_app(NSString *app)
{
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
	NSString *bundle_id = info[@"CFBundleIdentifier"], *exe = info[@"CFBundleExecutable"];

	if (bundle_id == nil || exe == nil) {
		say("ipainstall: %s: no Info.plist with CFBundleIdentifier and CFBundleExecutable\n", app.UTF8String);
		return NO;
	}
	// (a zip made without Unix modes leaves the program not executable)
	chmod([app stringByAppendingPathComponent:exe].fileSystemRepresentation, 0755);

	NSString *home = data_container(bundle_id);
	NSDictionary *app_dict = @{
		@"ApplicationType": @"System",
		@"CFBundleIdentifier": bundle_id,
		@"CodeInfoIdentifier": bundle_id,
		@"CompatibilityState": @0,
		@"Path": app,
		@"Container": home,
		@"IsContainerized": @YES,
		@"EnvironmentVariables": @{
			@"CFFIXED_USER_HOME": home,
			@"HOME": home,
			@"TMPDIR": [home stringByAppendingPathComponent:@"tmp"],
		},
		@"IsDeletable": @NO,
		@"LSInstallType": @1,
	};
	BOOL ok = [[LSApplicationWorkspace defaultWorkspace] registerApplicationDictionary:app_dict];
	LSApplicationProxy *proxy = [LSApplicationProxy applicationProxyForIdentifier:bundle_id];

	say("ipainstall: %s %s (\"%s\"): %s\n", bundle_id.UTF8String,
	    [info[@"CFBundleShortVersionString"] ?: @"?" UTF8String],
	    [info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"] ?: @"?" UTF8String],
	    ok && proxy.bundleURL != nil ? "INSTALLED" : "NOT REGISTERED");
	return ok && proxy.bundleURL != nil;
}

// the bundle directory of ours that BUNDLE-ID was installed into, if any
static NSString *
our_bundle_dir(NSString *bundle_id)
{
	NSString *path = [LSApplicationProxy applicationProxyForIdentifier:bundle_id].bundleURL.path;

	path = path.stringByResolvingSymlinksInPath;
	if ([path hasPrefix:@BUNDLES "/"] || [path hasPrefix:@"/var/containers/Bundle/Application/"]) {
		return path.stringByDeletingLastPathComponent;
	}
	return nil;
}

// an IPA from the network (https://, or http:// from the computer: this program's Info.plist lets it) into
// /private/var/tmp, its path or nil
static NSString *
download(NSString *url_string)
{
	NSURL *url = [NSURL URLWithString:url_string];
	NSString *path = [@"/private/var/tmp" stringByAppendingPathComponent:
	    [NSString stringWithFormat:@"ipainstall-%@.ipa", NSUUID.UUID.UUIDString]];
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block NSError *failure = nil;
	__block long status = 0;
	NSDate *start = NSDate.date;

	if (url == nil) {
		say("ipainstall: %s: not a URL\n", url_string.UTF8String);
		return nil;
	}
	say("ipainstall: downloading %s\n", url_string.UTF8String);
	[[NSURLSession.sharedSession downloadTaskWithURL:url
	    completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
		status = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)response).statusCode : 0;
		failure = error;
		if (location != nil && status == 200) {
			[NSFileManager.defaultManager moveItemAtPath:location.path toPath:path error:&failure];
		}
		dispatch_semaphore_signal(done);
	}] resume];
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	if (failure != nil || status != 200) {
		say("ipainstall: download failed: HTTP %ld %s\n", status, failure.localizedDescription.UTF8String ?: "");
		return nil;
	}
	unsigned long long size = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL].fileSize;

	say("ipainstall: %.1f MB in %.1f s\n", size / 1e6, -start.timeIntervalSinceNow);
	return path;
}

// ---------------------------------------------------------------- signing (src/adhoc_sign.c)

static NSString *boot_arg(const char *name);

/*
 * What an application expects to find in its signature when it brings no entitlements of its own (an unsigned
 * build): its identifier, its app group group.<main bundle id> (Telegram clients keep everything there and show a
 * white window without it), a keychain group, iCloud's (CloudKit traps -- brk #1 -- in an app that makes a
 * CKContainer without them) and push's. The team is made up; nothing checks it here. (work/wsl/resign-ipa.sh made
 * the same set by hand, 2026-11-15.)
 */
static NSData *
made_entitlements(NSString *bundle_id, NSString *main_id)
{
	NSString *team = @"HAKIOSSIDE";
	NSDictionary *ents = @{
		@"application-identifier": [NSString stringWithFormat:@"%@.%@", team, bundle_id],
		@"com.apple.developer.team-identifier": team,
		@"com.apple.security.application-groups": @[ [@"group." stringByAppendingString:main_id] ],
		@"keychain-access-groups": @[ [NSString stringWithFormat:@"%@.%@", team, main_id] ],
		@"com.apple.developer.icloud-container-identifiers": @[ [@"iCloud." stringByAppendingString:main_id] ],
		@"com.apple.developer.icloud-services": @[ @"CloudKit", @"CloudDocuments" ],
		@"com.apple.developer.icloud-container-environment": @"Production",
		@"com.apple.developer.ubiquity-kvstore-identifier": [NSString stringWithFormat:@"%@.%@", team, main_id],
		@"aps-environment": @"production",
	};

	return [NSPropertyListSerialization dataWithPropertyList:ents format:NSPropertyListXMLFormat_v1_0 options:0
	    error:NULL];
}

/*
 * Every Mach-O of the bundle APP signed ad hoc: first the executables of the application and of its PlugIns/NAME.appex
 * with entitlements (their own signature's when they have one, else made_entitlements()), then every other Mach-O
 * file (frameworks, dylibs, helpers) without. A file that cannot be signed (still encrypted, not arm64) is said and
 * left as it is: the install goes on.
 */
static void
sign_bundle(NSString *app)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *sign = boot_arg("ipa_sign");

	if ([sign isEqualToString:@"0"]) {
		say("ipainstall: not signed (ipa_sign=0)\n");
		return;
	}
	NSString *main_id = [NSDictionary dictionaryWithContentsOfFile:
	    [app stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
	NSMutableArray<NSString *> *bundles = [NSMutableArray arrayWithObject:app];
	NSMutableSet<NSString *> *done = [NSMutableSet set];
	unsigned signed_files = 0, failed = 0;
	const char *how = "none";
	char why[256];
	NSDate *start = NSDate.date;

	if (main_id == nil) {
		return;
	}
	for (NSString *name in [fm contentsOfDirectoryAtPath:[app stringByAppendingPathComponent:@"PlugIns"] error:NULL]) {
		if ([name.pathExtension isEqualToString:@"appex"]) {
			[bundles addObject:[[app stringByAppendingPathComponent:@"PlugIns"] stringByAppendingPathComponent:name]];
		}
	}
	for (NSString *bundle in bundles) {
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
		    [bundle stringByAppendingPathComponent:@"Info.plist"]];
		NSString *bid = info[@"CFBundleIdentifier"], *exe = info[@"CFBundleExecutable"];
		NSString *path = exe ? [bundle stringByAppendingPathComponent:exe] : nil;
		size_t len = 0;
		char *own;
		NSData *ents;

		if (bid == nil || path == nil || !adhoc_is_macho(path.fileSystemRepresentation)) {
			continue;
		}
		own = adhoc_entitlements(path.fileSystemRepresentation, &len);
		ents = own ? [NSData dataWithBytesNoCopy:own length:len freeWhenDone:YES] : made_entitlements(bid, main_id);
		if (bundle == app) {
			how = own ? "its own" : "made";
		}
		if (adhoc_sign(path.fileSystemRepresentation, bid.UTF8String, ents.bytes, ents.length, why, sizeof(why)) == 0) {
			signed_files++;
		} else if (failed++ < 5) {
			say("ipainstall: %s not signed: %s\n", path.lastPathComponent.UTF8String, why);
		}
		[done addObject:path];
	}
	NSDirectoryEnumerator *e = [fm enumeratorAtPath:app];

	for (NSString *rel in e) {
		NSString *path = [app stringByAppendingPathComponent:rel];

		if ([done containsObject:path] || ![e.fileAttributes[NSFileType] isEqualToString:NSFileTypeRegular] ||
		    [e.fileAttributes[NSFileSize] unsignedLongLongValue] < 4096 || !adhoc_is_macho(path.fileSystemRepresentation)) {
			continue;
		}
		@autoreleasepool {
			NSString *ident = rel.lastPathComponent.stringByDeletingPathExtension;

			if (adhoc_sign(path.fileSystemRepresentation, ident.UTF8String, NULL, 0, why, sizeof(why)) == 0) {
				signed_files++;
			} else if (failed++ < 5) {
				say("ipainstall: %s not signed: %s\n", rel.UTF8String, why);
			}
		}
	}
	say("ipainstall: signed %u files in %.1f s (the application's entitlements: %s)%s\n", signed_files,
	    -start.timeIntervalSinceNow, how, failed ? [NSString stringWithFormat:@", %u not", failed].UTF8String : "");
}

static BOOL
install_ipa_once(NSString *ipa)
{
	NSFileManager *fm = NSFileManager.defaultManager;

	if ([ipa hasPrefix:@"https://"] || [ipa hasPrefix:@"http://"]) {
		NSString *file = download(ipa);
		BOOL ok = file != nil && install_ipa_once(file);

		if (file != nil) {
			[fm removeItemAtPath:file error:NULL];
		}
		return ok;
	}
	NSString *dir = [@BUNDLES stringByAppendingPathComponent:NSUUID.UUID.UUIDString];

	[fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
	NSString *app = unpack(ipa, dir);

	if (app == nil) {
		[fm removeItemAtPath:dir error:NULL];
		return NO;
	}
	NSString *bundle_id = [NSDictionary dictionaryWithContentsOfFile:
	    [app stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
	NSString *old = bundle_id ? our_bundle_dir(bundle_id) : nil;

	sign_bundle(app);
	if (!register_app(app)) {
		[fm removeItemAtPath:dir error:NULL];
		return NO;
	}
	if (old != nil && ![old isEqualToString:dir]) {
		[fm removeItemAtPath:old error:NULL];
		say("ipainstall: the earlier copy removed\n");
	}
	return YES;
}

/*
 * An install that fails is tried once more, 5 s later (a download with it): The Sims failed twice at boot on a short
 * write ("inflate -1", 2026-11-01 20:29 and 20:39 -- the data volume briefly out of pages while everything else
 * started), then installed at the next three boots. A file that is not an IPA at all fails twice, said twice.
 */
static BOOL
install_ipa(NSString *ipa)
{
	if (install_ipa_once(ipa)) {
		return YES;
	}
	say("ipainstall: %s did not install: once more in 5 s\n", ipa.lastPathComponent.UTF8String);
	sleep(5);
	if (install_ipa_once(ipa)) {
		say("ipainstall: %s installed at the second try\n", ipa.lastPathComponent.UTF8String);
		return YES;
	}
	return NO;
}

static BOOL
uninstall(NSString *bundle_id)
{
	LSApplicationProxy *proxy = [LSApplicationProxy applicationProxyForIdentifier:bundle_id];
	NSString *dir = our_bundle_dir(bundle_id);
	BOOL ok = proxy.bundleURL != nil && ![proxy.bundleURL.path hasPrefix:@"/Applications/"] &&
	    [[LSApplicationWorkspace defaultWorkspace] unregisterApplication:proxy.bundleURL];

	if (ok && dir != nil) {
		[NSFileManager.defaultManager removeItemAtPath:dir error:NULL];
	}
	say("ipainstall: uninstall %s: %s\n", bundle_id.UTF8String, ok ? "done" : "FAILED");
	return ok;
}

// the applications that are not the image's own: {BundleID, Name, Version, Path}
static NSArray *
installed(void)
{
	NSMutableArray *list = [NSMutableArray array];

	for (LSApplicationProxy *app in [[LSApplicationWorkspace defaultWorkspace] allInstalledApplications]) {
		NSString *path = app.bundleURL.path;

		if (path != nil && ![path hasPrefix:@"/Applications/"] && ![path hasPrefix:@"/System/"]) {
			[list addObject:@{
				@"BundleID": app.applicationIdentifier ?: @"",
				@"Name": app.localizedName ?: app.applicationIdentifier ?: @"",
				@"Version": app.shortVersionString ?: @"",
				@"Path": path,
			}];
		}
	}
	[list sortUsingDescriptors:@[ [NSSortDescriptor sortDescriptorWithKey:@"Name" ascending:YES] ]];
	return list;
}

static int
install_dir(NSString *dir)
{
	int failed = 0, done = 0;

	for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL]
	    sortedArrayUsingSelector:@selector(compare:)]) {
		NSString *path = [dir stringByAppendingPathComponent:name];

		if ([name.pathExtension isEqualToString:@"ipa"]) {
			install_ipa(path) ? done++ : failed++;
		} else if ([name.pathExtension isEqualToString:@"app"]) {
			register_app(path) ? done++ : failed++;
		}
	}
	say("ipainstall: %s: %d installed, %d failed\n", dir.UTF8String, done, failed);
	return failed;
}

// ---------------------------------------------------------------- an application's state

// the application's processes (its extensions too: everything run from its bundle) stopped; how many
static int
kill_app(NSString *bundle_id)
{
	NSString *bundle = [LSApplicationProxy applicationProxyForIdentifier:bundle_id].bundleURL.path;
	int n = proc_listallpids(NULL, 0), killed = 0;

	if (bundle == nil || n <= 0) {
		return 0;
	}
	bundle = [bundle.stringByResolvingSymlinksInPath stringByAppendingString:@"/"];
	pid_t *pids = calloc((size_t)n + 64, sizeof(pid_t));

	n = proc_listallpids(pids, (int)(((size_t)n + 64) * sizeof(pid_t)));
	for (int i = 0; i < n; i++) {
		char path[4 * PATH_MAX];

		if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof(path)) > 0 &&
		    [@(path).stringByResolvingSymlinksInPath hasPrefix:bundle] && kill(pids[i], SIGKILL) == 0) {
			killed++;
		}
	}
	free(pids);
	if (killed > 0) {
		usleep(500 * 1000);     /* (gone, its files closed) */
		say("ipainstall: %s: %d process%s stopped\n", bundle_id.UTF8String, killed, killed == 1 ? "" : "es");
	}
	return killed;
}

// containermanagerd's app group container GROUP-ID (made if need be: the application gets this one when it asks)
static NSString *
group_container(NSString *group_id)
{
	Class mcm = nil;

	if (dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW)) {
		mcm = NSClassFromString(@"MCMSharedDataContainer");
	}
	if (mcm != nil) {
		NSError *error = nil;
		BOOL existed = NO;
		MCMContainer *c = [mcm containerWithIdentifier:group_id createIfNecessary:YES existed:&existed error:&error];

		if (c.url != nil) {
			return c.url.path;
		}
		say("ipainstall: containermanagerd made no container %s: %s\n", group_id.UTF8String,
		    error.description.UTF8String);
	}
	for (NSString *uuid in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@GROUPS error:NULL]) {
		NSString *dir = [@GROUPS stringByAppendingPathComponent:uuid];
		NSDictionary *meta = [NSDictionary dictionaryWithContentsOfFile:
		    [dir stringByAppendingPathComponent:@CONTAINER_META]];

		if ([meta[@"MCMMetadataIdentifier"] isEqualToString:group_id]) {
			return dir;
		}
	}
	return nil;
}

// The application's app groups: its signature's entitlements (com.apple.security.application-groups, the plist in
// the executable's code signature), else every group container named after it
static NSArray<NSString *> *
app_groups(NSString *bundle_id)
{
	NSString *bundle = [LSApplicationProxy applicationProxyForIdentifier:bundle_id].bundleURL.path;
	NSString *exe = [NSDictionary dictionaryWithContentsOfFile:
	    [bundle stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleExecutable"];
	NSData *bin = exe ? [NSData dataWithContentsOfFile:[bundle stringByAppendingPathComponent:exe]
	    options:NSDataReadingMappedIfSafe error:NULL] : nil;
	NSData *key = [@"<key>com.apple.security.application-groups</key>" dataUsingEncoding:NSUTF8StringEncoding];
	NSRange at = bin ? [bin rangeOfData:key options:0 range:NSMakeRange(0, bin.length)] : NSMakeRange(NSNotFound, 0);

	if (at.location != NSNotFound) {
		NSData *open = [@"<?xml" dataUsingEncoding:NSUTF8StringEncoding];
		NSData *close = [@"</plist>" dataUsingEncoding:NSUTF8StringEncoding];
		NSRange start = [bin rangeOfData:open options:NSDataSearchBackwards range:NSMakeRange(0, at.location)];
		NSRange end = [bin rangeOfData:close options:0 range:NSMakeRange(at.location, bin.length - at.location)];

		if (start.location != NSNotFound && end.location != NSNotFound) {
			NSData *xml = [bin subdataWithRange:NSMakeRange(start.location, NSMaxRange(end) - start.location)];
			NSDictionary *ents = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:NULL];
			NSArray *groups = [ents isKindOfClass:NSDictionary.class] ? ents[@"com.apple.security.application-groups"] : nil;

			if ([groups isKindOfClass:NSArray.class]) {
				return groups;
			}
		}
	}
	NSMutableArray *groups = [NSMutableArray array];

	for (NSString *uuid in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@GROUPS error:NULL]) {
		NSString *gid = [NSDictionary dictionaryWithContentsOfFile:[[@GROUPS stringByAppendingPathComponent:uuid]
		    stringByAppendingPathComponent:@CONTAINER_META]][@"MCMMetadataIdentifier"];

		if ([gid hasPrefix:@"group."] && [gid containsString:bundle_id]) {
			[groups addObject:gid];
		}
	}
	return groups;
}

// everything in a container but containermanagerd's metadata file, removed
static void
empty_container(NSString *dir)
{
	NSFileManager *fm = NSFileManager.defaultManager;

	for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:NULL]) {
		if (![name isEqualToString:@CONTAINER_META]) {
			[fm removeItemAtPath:[dir stringByAppendingPathComponent:name] error:NULL];
		}
	}
}

// a container's files the application's own again (unpacked by root)
static void
give_to_mobile(NSString *dir)
{
	lchown(dir.fileSystemRepresentation, MOBILE, MOBILE);
	for (NSString *rel in [NSFileManager.defaultManager enumeratorAtPath:dir]) {
		lchown([dir stringByAppendingPathComponent:rel].fileSystemRepresentation, MOBILE, MOBILE);
	}
}

// The state saved before (ipainstall -S, SAVE) back in place of what the application has: its data container's
// files, and those of each app group container in the zip. The application is stopped first.
static BOOL
restore_state(NSString *bundle_id, NSString *zip)
{
	if ([LSApplicationProxy applicationProxyForIdentifier:bundle_id].bundleURL == nil) {
		say("ipainstall: %s is not installed: its state stays where it is\n", bundle_id.UTF8String);
		return NO;
	}
	kill_app(bundle_id);
	NSMutableDictionary<NSString *, NSString *> *roots = [NSMutableDictionary dictionary];
	unsigned files = 0;
	uint64_t bytes = 0;
	BOOL ok = unzip_each(zip, ^NSString *(NSArray<NSString *> *parts) {
		NSString *key = nil;
		NSUInteger skip = 0;

		if (parts.count >= 2 && [parts[0] isEqualToString:@"data"]) {
			key = @"data", skip = 1;
		} else if (parts.count >= 3 && [parts[0] isEqualToString:@"group"] && [parts[1] hasPrefix:@"group."]) {
			key = parts[1], skip = 2;
		} else {
			return nil;
		}
		NSString *root = roots[key];

		if (root == nil) {          /* (the first of its files: the container, emptied) */
			root = ([key isEqualToString:@"data"] ? data_container(bundle_id) : group_container(key)) ?: @"";
			if (root.length > 0) {
				empty_container(root);
			}
			roots[key] = root;
		}
		if (root.length == 0 || [parts.lastObject isEqualToString:@CONTAINER_META]) {
			return nil;
		}
		return [root stringByAppendingPathComponent:
		    [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(skip, parts.count - skip)]]];
	}, &files, &bytes);

	// (what a data container always has, the saved state or not: tmp/ is the application's TMPDIR)
	for (NSString *sub in @[ @"Documents", @"Library", @"Library/Caches", @"Library/Preferences", @"tmp" ]) {
		if (roots[@"data"].length > 0) {
			[NSFileManager.defaultManager createDirectoryAtPath:[roots[@"data"] stringByAppendingPathComponent:sub]
			    withIntermediateDirectories:YES attributes:nil error:NULL];
		}
	}
	for (NSString *key in roots) {
		if (roots[key].length > 0) {
			give_to_mobile(roots[key]);
		}
	}
	say("ipainstall: %s: state put back (%s): %u files, %.1f MB -- %s\n", bundle_id.UTF8String,
	    [[roots.allKeys sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@", "].UTF8String,
	    files, bytes / 1e6, ok ? "done" : "FAILED");
	return ok;
}

// ---- a zip file, written as it goes (each file deflated when that makes it smaller -- a messenger's database to a
// quarter, owner 11-15: "之后也压缩吧" -- else stored; sizes and offsets below 4 GB)

static void
put16(NSMutableData *d, uint16_t v)
{
	uint8_t b[2] = { v & 0xff, v >> 8 };

	[d appendBytes:b length:2];
}

static void
put32(NSMutableData *d, uint32_t v)
{
	put16(d, v & 0xffff);
	put16(d, v >> 16);
}

static BOOL
zip_add(int fd, uint64_t *at, NSMutableData *cd, unsigned *count, NSString *name, NSData *data)
{
	NSData *n = [name dataUsingEncoding:NSUTF8StringEncoding];
	uint32_t crc = (uint32_t)crc32(0, data.bytes, (uInt)data.length);
	NSMutableData *h = [NSMutableData data];
	NSData *body = data;
	uint16_t method = 0;

	if (data.length > 64 && data.length < 0xffffffffULL) {
		z_stream z = { 0 };

		if (deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
			NSMutableData *packed = [NSMutableData dataWithLength:deflateBound(&z, (uLong)data.length)];

			z.next_in = (Bytef *)data.bytes, z.avail_in = (uInt)data.length;
			z.next_out = packed.mutableBytes, z.avail_out = (uInt)packed.length;
			if (deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out < data.length) {
				packed.length = z.total_out;
				body = packed, method = 8;
			}
			deflateEnd(&z);
		}
	}
	if (*at + 30 + n.length + body.length >= 0xffffffffULL) {
		say("ipainstall: the state is 4 GB or more: not saved\n");
		return NO;
	}
	put32(h, 0x04034b50), put16(h, 20), put16(h, 0x0800), put16(h, method), put16(h, 0), put16(h, 0x21);
	put32(h, crc), put32(h, (uint32_t)body.length), put32(h, (uint32_t)data.length);
	put16(h, (uint16_t)n.length), put16(h, 0);
	[h appendData:n];
	if (write(fd, h.bytes, h.length) != (ssize_t)h.length ||
	    (body.length > 0 && write(fd, body.bytes, body.length) != (ssize_t)body.length)) {
		say("ipainstall: writing the state: %s\n", strerror(errno));
		return NO;
	}
	put32(cd, 0x02014b50), put16(cd, 3 << 8 | 20), put16(cd, 20), put16(cd, 0x0800), put16(cd, method);
	put16(cd, 0), put16(cd, 0x21), put32(cd, crc), put32(cd, (uint32_t)body.length);
	put32(cd, (uint32_t)data.length), put16(cd, (uint16_t)n.length), put16(cd, 0), put16(cd, 0), put16(cd, 0);
	put16(cd, 0), put32(cd, (uint32_t)(S_IFREG | 0644) << 16), put32(cd, (uint32_t)*at);
	[cd appendData:n];
	*at += h.length + body.length;
	(*count)++;
	return YES;
}

/*
 * The application's state into the zip file OUT: data/... (its data container) and group/<GROUP-ID>/... (each of its
 * app group containers), regular files only, none whose name in the zip has one of SKIP in it (caches: they come
 * back by themselves) -- and never data/Library/Caches/, data/Library/SplashBoard/ (the screenshots of its last
 * screen), data/tmp/. The application is stopped first: a database copied while it is written is torn.
 */
static BOOL
save_state(NSString *bundle_id, NSString *out, NSArray<NSString *> *skip)
{
	NSFileManager *fm = NSFileManager.defaultManager;

	if ([LSApplicationProxy applicationProxyForIdentifier:bundle_id].bundleURL == nil) {
		say("ipainstall: %s is not installed\n", bundle_id.UTF8String);
		return NO;
	}
	kill_app(bundle_id);
	NSMutableArray *never = [@[ @"data/Library/Caches/", @"data/Library/SplashBoard/", @"data/tmp/" ] mutableCopy];

	[never addObjectsFromArray:skip ?: @[]];
	NSMutableArray *roots = [NSMutableArray arrayWithObject:@[ @"data", data_container(bundle_id) ]];

	for (NSString *gid in app_groups(bundle_id)) {
		NSString *dir = group_container(gid);

		if (dir != nil) {
			[roots addObject:@[ [@"group/" stringByAppendingString:gid], dir ]];
		}
	}
	int fd = open(out.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	NSMutableData *cd = [NSMutableData data];
	uint64_t at = 0, bytes = 0;
	unsigned count = 0, left_out = 0;
	BOOL ok = fd >= 0;

	for (NSArray *r in roots) {
		NSString *label = r[0], *dir = r[1];
		NSDirectoryEnumerator *e = [fm enumeratorAtPath:dir];

		for (NSString *rel in e) {
			NSString *name = [NSString stringWithFormat:@"%@/%@", label, rel];
			BOOL skipped = [rel isEqualToString:@CONTAINER_META];

			if (!ok) {
				break;
			}
			if (![e.fileAttributes[NSFileType] isEqualToString:NSFileTypeRegular]) {
				continue;
			}
			for (NSString *s in never) {
				skipped = skipped || [name containsString:s];
			}
			if (skipped) {
				left_out++;
				continue;
			}
			@autoreleasepool {
				NSData *data = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:rel]
				    options:NSDataReadingMappedIfSafe error:NULL];

				ok = data != nil && zip_add(fd, &at, cd, &count, name, data);
				bytes += data.length;
			}
		}
	}
	if (ok) {
		NSMutableData *end = [NSMutableData data];

		put32(end, 0x06054b50), put16(end, 0), put16(end, 0), put16(end, (uint16_t)MIN(count, 0xffffu));
		put16(end, (uint16_t)MIN(count, 0xffffu)), put32(end, (uint32_t)cd.length), put32(end, (uint32_t)at);
		put16(end, 0);
		ok = count < 0xffff && write(fd, cd.bytes, cd.length) == (ssize_t)cd.length &&
		    write(fd, end.bytes, end.length) == (ssize_t)end.length;
	}
	if (fd >= 0) {
		close(fd);
	}
	NSMutableArray *labels = [NSMutableArray array];

	for (NSArray *r in roots) {
		[labels addObject:r[0]];
	}
	say("ipainstall: %s: state saved (%s): %u files, %.1f MB, %u left out -- %s\n", bundle_id.UTF8String,
	    [labels componentsJoinedByString:@", "].UTF8String, count, bytes / 1e6, left_out, ok ? "done" : "FAILED");
	if (!ok) {
		unlink(out.fileSystemRepresentation);
	}
	return ok;
}

// ---------------------------------------------------------------- the service

static void
wait_for_launchservices(void)
{
	int waited = 0;

	// (at boot: not before LaunchServices has the image's own applications, its first scan of /Applications,
	// which would otherwise come after us)
	while ([LSApplicationProxy applicationProxyForIdentifier:@"com.apple.Preferences"].bundleURL == nil &&
	    waited < 300) {
		sleep(1);
		waited++;
	}
	say("ipainstall: LaunchServices ready after %d s\n", waited);
}

// the value of NAME= among the kernel's boot arguments
static NSString *
boot_arg(const char *name)
{
	char args[2048] = "";
	size_t len = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return nil;
	}
	for (NSString *arg in [@(args) componentsSeparatedByString:@" "]) {
		NSString *prefix = [NSString stringWithFormat:@"%s=", name];

		if ([arg hasPrefix:prefix]) {
			return [arg substringFromIndex:prefix.length];
		}
	}
	return nil;
}

static void
write_plist(id object, NSString *path)
{
	NSData *data = [NSPropertyListSerialization dataWithPropertyList:object format:NSPropertyListXMLFormat_v1_0
	    options:0 error:NULL];

	[data writeToFile:path atomically:YES];
	chmod(path.fileSystemRepresentation, 0644);
}

// one installation at a time, whoever asks (the application's spool, the computer); and where the list goes
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static NSString *installed_list;

// ---------------------------------------------------------------- IPAs from the computer
//
// boot-arg apps_token=HEX (start.ps1 makes one for every boot and puts it on the kernel's command line): the
// release's apps\ folder, pushed by the computer, which finds the phone on its network by this port. The
// computer only connects out, so its firewall has nothing to let in (serving the folder from the computer
// instead waited on a Windows Firewall question nobody was there to answer). One connection at a time:
//   phone: "hakios-apps 1"
//   computer: "TOKEN HEX"                       phone: "OK", or "NO" and it closes
//   computer: "FILE SIZE NAME", SIZE bytes      phone: what ipainstall says, a line "| ..." each, then "DONE 1|0"
//   computer: "STATE SIZE BUNDLE-ID", SIZE bytes (a zip: data/..., group/<GROUP-ID>/...)    phone: the same -- the
//             application's saved state put back (after its FILE: the application is there already)
//   computer: "SAVE BUNDLE-ID [SKIP ...]"       phone: "| ..." lines, then "ZIP SIZE" and SIZE bytes, or "DONE 0"
//             (SKIP: pieces of names in the zip to leave out -- a messenger's media cache, say: /postbox/media/)
//   computer: "END"
#define APPS_PORT 48620
static NSString *apps_token;

static BOOL
read_line(int fd, char *buf, size_t max)
{
	size_t n = 0;

	while (n + 1 < max) {
		char c;

		if (read(fd, &c, 1) != 1) {
			return NO;
		}
		if (c == '\n') {
			break;
		}
		buf[n++] = c;
	}
	buf[n] = 0;
	if (n > 0 && buf[n - 1] == '\r') {
		buf[n - 1] = 0;
	}
	return YES;
}

static void
send_text(int fd, const char *text)
{
	size_t len = strlen(text);

	while (len > 0) {
		ssize_t w = write(fd, text, len);

		if (w <= 0) {
			return;
		}
		text += w;
		len -= (size_t)w;
	}
}

// SIZE bytes of the connection into PATH; NO when it ended first
static BOOL
receive_file(int fd, unsigned long long size, NSString *path)
{
	static char buf[1 << 16];
	unsigned long long got = 0;
	int out = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	while (out >= 0 && got < size) {
		ssize_t r = read(fd, buf, (size_t)MIN((unsigned long long)sizeof(buf), size - got));

		if (r <= 0 || write(out, buf, (size_t)r) != r) {
			break;
		}
		got += (unsigned long long)r;
	}
	if (out >= 0) {
		close(out);
	}
	if (got != size) {
		unlink(path.fileSystemRepresentation);
		printf("ipainstall: from the computer: %llu of %llu bytes, the connection ended\n", got, size);
		return NO;
	}
	return YES;
}

static BOOL
send_file(int fd, NSString *path)
{
	static char buf[1 << 16];
	int in = open(path.fileSystemRepresentation, O_RDONLY);
	ssize_t r = -1;
	BOOL ok = in >= 0;

	while (ok && (r = read(in, buf, sizeof(buf))) > 0) {
		for (ssize_t done = 0; ok && done < r;) {
			ssize_t w = write(fd, buf + done, (size_t)(r - done));

			ok = w > 0;
			done += w;
		}
	}
	if (in >= 0) {
		close(in);
	}
	return ok && r == 0;
}

// WORK done (one piece of work at a time, whoever asks) with what ipainstall says to the computer, a "| ..." line each
static BOOL
said_to(int fd, BOOL (^work)(void))
{
	pthread_mutex_lock(&work_lock);
	request_log = [NSMutableString string];
	__block NSUInteger sent = 0;
	request_progress = ^{
		NSString *more = [request_log substringFromIndex:sent];

		sent = request_log.length;
		for (NSString *l in [more componentsSeparatedByString:@"\n"]) {
			if (l.length > 0) {
				send_text(fd, [NSString stringWithFormat:@"| %@\n", l].UTF8String);
			}
		}
	};
	BOOL ok = work();

	request_progress = nil;
	request_log = nil;
	if (installed_list != nil) {
		write_plist(installed(), installed_list);
	}
	pthread_mutex_unlock(&work_lock);
	return ok;
}

static void
push_session(int fd)
{
	char line[1024];

	send_text(fd, "hakios-apps 1\n");
	if (!read_line(fd, line, sizeof(line)) || strncmp(line, "TOKEN ", 6) != 0 || apps_token == nil ||
	    ![@(line + 6) isEqualToString:apps_token]) {
		send_text(fd, "NO\n");
		printf("ipainstall: a connection on port %d without the token: refused\n", APPS_PORT);
		return;
	}
	send_text(fd, "OK\n");
	while (read_line(fd, line, sizeof(line))) {
		NSString *tmp = [@"/private/var/tmp" stringByAppendingPathComponent:
		    [NSString stringWithFormat:@"push-%@.zip", NSUUID.UUID.UUIDString]];

		if (strncmp(line, "FILE ", 5) == 0 || strncmp(line, "STATE ", 6) == 0) {
			BOOL state = line[0] == 'S';
			char *end = NULL;
			unsigned long long size = strtoull(line + (state ? 6 : 5), &end, 10);
			NSString *name = end != NULL && *end == ' ' ? @(end + 1) : @"?";

			if (!receive_file(fd, size, tmp)) {
				return;
			}
			BOOL ok = said_to(fd, ^BOOL {
				if (state) {
					say("ipainstall: %s's state from the computer, %.1f MB\n", name.UTF8String, size / 1e6);
					return restore_state(name, tmp);
				}
				say("ipainstall: %s from the computer, %.1f MB\n", name.UTF8String, size / 1e6);
				return install_ipa(tmp);
			});

			unlink(tmp.fileSystemRepresentation);
			send_text(fd, ok ? "DONE 1\n" : "DONE 0\n");
		} else if (strncmp(line, "SAVE ", 5) == 0) {
			NSArray<NSString *> *words = [[@(line + 5) componentsSeparatedByString:@" "]
			    filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
			NSString *bundle_id = words.firstObject ?: @"";
			NSArray *skip = words.count > 1 ? [words subarrayWithRange:NSMakeRange(1, words.count - 1)] : @[];
			BOOL ok = said_to(fd, ^BOOL {
				return save_state(bundle_id, tmp, skip);
			});

			if (ok) {
				unsigned long long size = [NSFileManager.defaultManager attributesOfItemAtPath:tmp error:NULL].fileSize;

				send_text(fd, [NSString stringWithFormat:@"ZIP %llu\n", size].UTF8String);
				ok = send_file(fd, tmp);
				unlink(tmp.fileSystemRepresentation);
				if (!ok) {
					return;
				}
			} else {
				send_text(fd, "DONE 0\n");
			}
		} else {
			break;      /* (END) */
		}
	}
}

static void *
listen_for_computer(__unused void *arg)
{
	int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
	struct sockaddr_in a = {
		.sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(APPS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY),
	};

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
		printf("ipainstall: port %d: %s\n", APPS_PORT, strerror(errno));
		return NULL;
	}
	printf("ipainstall: waiting for the computer on port %d\n", APPS_PORT);
	fflush(stdout);
	for (;;) {
		int c = accept(s, NULL, NULL);
		struct timeval patience = { .tv_sec = 60 };

		if (c < 0) {
			continue;
		}
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
		@autoreleasepool {
			push_session(c);
		}
		close(c);
	}
	return NULL;
}

// SPOOL/queue/ID.plist {Action: install|uninstall, Source: path or URL, BundleID} from the application, served
// one at a time; the answer in SPOOL/results/ID.plist {Done, OK, Log} as it goes, and the list of what is
// installed in SPOOL/installed.plist after each
static void
serve(NSString *spool)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *queue = [spool stringByAppendingPathComponent:@"queue"];
	NSString *results = [spool stringByAppendingPathComponent:@"results"];
	NSString *list = [spool stringByAppendingPathComponent:@"installed.plist"];

	[fm createDirectoryAtPath:queue withIntermediateDirectories:YES attributes:nil error:NULL];
	[fm createDirectoryAtPath:results withIntermediateDirectories:YES attributes:nil error:NULL];
	chown(spool.fileSystemRepresentation, MOBILE, MOBILE);
	chown(queue.fileSystemRepresentation, MOBILE, MOBILE);
	chmod(spool.fileSystemRepresentation, 0755);
	chmod(queue.fileSystemRepresentation, 0755);
	chmod(results.fileSystemRepresentation, 0755);
	installed_list = list;
	write_plist(installed(), list);

	int kq = kqueue(), fd = open(queue.fileSystemRepresentation, O_EVTONLY);
	struct kevent change;

	EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, NULL);
	kevent(kq, &change, 1, NULL, 0, NULL);
	say("ipainstall: serving %s (uid %d)\n", queue.UTF8String, getuid());
	for (;;) {
		@autoreleasepool {
			NSArray *names = [[fm contentsOfDirectoryAtPath:queue error:NULL]
			    sortedArrayUsingSelector:@selector(compare:)];

			for (NSString *name in names) {
				if (![name.pathExtension isEqualToString:@"plist"] || [name hasPrefix:@"."]) {
					continue;
				}
				NSString *path = [queue stringByAppendingPathComponent:name];
				NSDictionary *req = [NSDictionary dictionaryWithContentsOfFile:path];
				NSString *result = [results stringByAppendingPathComponent:name];
				NSString *action = req[@"Action"];
				BOOL ok = NO;

				[fm removeItemAtPath:path error:NULL];
				pthread_mutex_lock(&work_lock);
				request_log = [NSMutableString string];
				__block NSDate *last = NSDate.distantPast;
				request_progress = ^{
					if (-last.timeIntervalSinceNow > 0.3) {
						write_plist(@{ @"Done": @NO, @"Log": request_log }, result);
						last = NSDate.date;
					}
				};
				if ([action isEqualToString:@"install"] && [req[@"Source"] length] > 0) {
					ok = install_ipa(req[@"Source"]);
				} else if ([action isEqualToString:@"uninstall"] && [req[@"BundleID"] length] > 0) {
					ok = uninstall(req[@"BundleID"]);
				} else if (![action isEqualToString:@"list"]) {
					say("ipainstall: %s: not a request\n", name.UTF8String);
				} else {
					ok = YES;
				}
				request_progress = nil;
				write_plist(installed(), list);
				write_plist(@{ @"Done": @YES, @"OK": @(ok), @"Log": request_log }, result);
				request_log = nil;
				pthread_mutex_unlock(&work_lock);
			}
		}
		struct kevent event;

		kevent(kq, NULL, 0, &event, 1, NULL);
	}
}

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		if ([NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] == nil) {
			say("ipainstall: no LSApplicationWorkspace\n");
			return 1;
		}
		if (argc >= 2 && strcmp(argv[1], "-l") == 0) {
			for (NSDictionary *app in installed()) {
				say("%s  %s  %s  %s\n", [app[@"BundleID"] UTF8String], [app[@"Version"] UTF8String],
				    [app[@"Name"] UTF8String], [app[@"Path"] UTF8String]);
			}
			return 0;
		}
		if (argc >= 3 && strcmp(argv[1], "-u") == 0) {
			return uninstall(@(argv[2])) ? 0 : 1;
		}
		if (argc >= 3 && strcmp(argv[1], "-r") == 0) {
			return register_app(@(argv[2])) ? 0 : 1;
		}
		if (argc == 4 && strcmp(argv[1], "-s") == 0) {
			return restore_state(@(argv[2]), @(argv[3])) ? 0 : 1;
		}
		if (argc >= 4 && strcmp(argv[1], "-S") == 0) {
			NSMutableArray *skip = [NSMutableArray array];

			for (int i = 4; i < argc; i++) {
				[skip addObject:@(argv[i])];
			}
			return save_state(@(argv[2]), @(argv[3]), skip) ? 0 : 1;
		}
		if (argc >= 3 && strcmp(argv[1], "-a") == 0) {
			wait_for_launchservices();
			return install_dir(@(argv[2])) ? 1 : 0;
		}
		if (argc >= 4 && strcmp(argv[1], "-d") == 0) {
			pthread_t listener;

			signal(SIGPIPE, SIG_IGN);
			wait_for_launchservices();
			install_dir(@(argv[2]));
			apps_token = boot_arg("apps_token");
			if (apps_token.length >= 16) {
				pthread_create(&listener, NULL, listen_for_computer, NULL);
			}
			serve(@(argv[3]));
			return 0;
		}
		if (argc != 2 || argv[1][0] == '-') {
			say("usage: ipainstall PATH.ipa | URL | -r PATH.app | -a DIR | -u BUNDLE-ID | -l | -d APPS SPOOL |\n"
			    "       -s BUNDLE-ID STATE.zip | -S BUNDLE-ID OUT.zip [SKIP ...]\n");
			return 2;
		}
		return install_ipa(@(argv[1])) ? 0 : 1;
	}
}
