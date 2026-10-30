// ipainstall: put an application on the phone the way the original release had its extra one (RedwoodTriangle.app,
// unsigned, in /Applications of its system image, which LaunchServices registers at every boot): unpack the IPA
// into a bundle directory of its own, make its data container, and register it with LaunchServices as a system
// application. installd is not asked: it refuses whatever Apple has not signed ("No code signature found",
// MICodeSigningVerifier), and the kernel runs such programs as they are (miniamfi: no signature policy).
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
// /private/var is a RAM file system here: what is installed lasts until the phone restarts.
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

#define BUNDLES "/private/var/containers/Bundle/Application"
#define DATA "/private/var/mobile/Containers/Data/Application"
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
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode & 0777) ? (mode & 0777) : 0644);

	if (fd < 0) {
		return -1;
	}
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

// Payload/NAME.app of the IPA into DEST/NAME.app; the application's directory, or nil
static NSString *
unpack(NSString *ipa, NSString *dest)
{
	int fd = open(ipa.fileSystemRepresentation, O_RDONLY);
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 22) {
		say("ipainstall: %s: %s\n", ipa.UTF8String, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return nil;
	}
	const uint8_t *zip = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	size_t len = st.st_size;

	close(fd);
	if (zip == MAP_FAILED) {
		return nil;
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
		return nil;
	}
	uint64_t entries = le16(eocd + 10), cd = le32(eocd + 16);

	if ((entries == 0xffff || cd == 0xffffffff) && eocd - zip >= 20 && le32(eocd - 20) == 0x07064b50) {
		uint64_t at = le64(eocd - 20 + 8);

		if (at + 56 <= len && le32(zip + at) == 0x06064b50) {
			entries = le64(zip + at + 32);
			cd = le64(zip + at + 48);
		}
	}
	NSString *app = nil;
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

		if (name == nil || parts.count < 2 || ![parts[0] isEqualToString:@"Payload"] || [parts containsObject:@".."] ||
		    ![parts[1] hasSuffix:@".app"]) {
			continue;
		}
		if (app == nil) {
			app = [dest stringByAppendingPathComponent:parts[1]];
		}
		NSString *out = [dest stringByAppendingPathComponent:
		    [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(1, parts.count - 1)]]];

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
	if (failed) {
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

static BOOL
install_ipa(NSString *ipa)
{
	NSFileManager *fm = NSFileManager.defaultManager;

	if ([ipa hasPrefix:@"https://"] || [ipa hasPrefix:@"http://"]) {
		NSString *file = download(ipa);
		BOOL ok = file != nil && install_ipa(file);

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

static void
push_session(int fd)
{
	static char buf[1 << 16];
	char line[1024];

	send_text(fd, "hakios-apps 1\n");
	if (!read_line(fd, line, sizeof(line)) || strncmp(line, "TOKEN ", 6) != 0 || apps_token == nil ||
	    ![@(line + 6) isEqualToString:apps_token]) {
		send_text(fd, "NO\n");
		printf("ipainstall: a connection on port %d without the token: refused\n", APPS_PORT);
		return;
	}
	send_text(fd, "OK\n");
	while (read_line(fd, line, sizeof(line)) && strncmp(line, "FILE ", 5) == 0) {
		char *end = NULL;
		unsigned long long size = strtoull(line + 5, &end, 10), got = 0;
		NSString *name = end != NULL && *end == ' ' ? @(end + 1) : @"?.ipa";
		NSString *path = [@"/private/var/tmp" stringByAppendingPathComponent:
		    [NSString stringWithFormat:@"push-%@.ipa", NSUUID.UUID.UUIDString]];
		int out = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644);

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
			printf("ipainstall: %s from the computer: %llu of %llu bytes, the connection ended\n", name.UTF8String, got, size);
			return;
		}
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
		say("ipainstall: %s from the computer, %.1f MB\n", name.UTF8String, size / 1e6);
		BOOL ok = install_ipa(path);

		request_progress = nil;
		request_log = nil;
		if (installed_list != nil) {
			write_plist(installed(), installed_list);
		}
		pthread_mutex_unlock(&work_lock);
		unlink(path.fileSystemRepresentation);
		send_text(fd, ok ? "DONE 1\n" : "DONE 0\n");
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
			say("usage: ipainstall PATH.ipa | URL | -r PATH.app | -a DIR | -u BUNDLE-ID | -l | -d APPS SPOOL\n");
			return 2;
		}
		return install_ipa(@(argv[1])) ? 0 : 1;
	}
}
