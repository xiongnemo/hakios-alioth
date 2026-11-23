/*
 * loc_enable: Location Services' own switch on, as the Setup Assistant turns it on on an iPhone that is set up
 * (+[CLLocationManager setLocationServicesEnabled:], CoreLocation's, what Settings' switch calls too). Nothing set
 * this phone up, so the switch was never on: locationd ran, but Settings had no Location Services and applications
 * got no location (2026-10-31). The image's /private/var is made afresh at every boot: the boot job runs this.
 *
 * With bundle IDs after it (test roots' boot job, 2026-11-15): each of those applications is also allowed location
 * "while using" as soon as it is installed, as Settings' own switch does it (+setAuthorizationStatusByType:
 * forBundleIdentifier:, the entitlement com.apple.locationd.authorizeapplications) -- so that an automated round
 * never meets the location alert, a SYSTEM alert that stays on top of every application until it is answered (it
 * covered the night round's screenshots and measurements). Waits up to 15 minutes for them to be installed.
 * --tcc (test roots too): the same applications also get the privacy prompts a round meets answered yes -- camera,
 * microphone, photos (read, add) -- through tccd as Settings' privacy switches do (TCCAccessSetForBundleId, the
 * entitlement com.apple.private.tcc.manager): Nagram's photo picker, the round that measures it, asks for both.
 * --all (test roots, the owner 11-15: "in development every permission allowed"): EVERY application -- the system's
 * own (Camera asked for location in a round and its alert stayed up) and every one installed later, as LaunchServices
 * says it is installed (and looked for every 10 seconds besides) for as long as the system runs -- gets location
 * "always" and every privacy service below answered yes.
 * usage: loc_enable [1|0] [--tcc] [--all] [BUNDLE-ID ...]   (no argument: 1)
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

@interface CLLocationManager (HakiosPrivate)
+ (void)setLocationServicesEnabled:(BOOL)enabled;
+ (void)setAuthorizationStatusByType:(int)status forBundleIdentifier:(NSString *)bundleIdentifier;
+ (int)authorizationStatusForBundleIdentifier:(NSString *)bundleIdentifier;
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@property (readonly) NSURL *bundleURL;
@property (readonly) NSString *bundleIdentifier;
@end

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (NSArray<LSApplicationProxy *> *)allApplications;
- (void)addObserver:(id)observer;
@end

/*
 * --all: an application is granted the moment LaunchServices says it is installed, not up to 10 s later: iOS ends a
 * running application whose privacy answers change (exit reason namespace 11, TCC), and 11-16 System Check -- opened
 * 10 s after its install -- was killed in its Metal tab by this program's grants. LSApplicationWorkspace tells its
 * observers -applicationsDidInstall: (the protocol is LSApplicationWorkspaceObserverProtocol); the look every 10 s
 * stays, for whatever comes without it.
 */
@interface HKInstallWatch : NSObject
@property (atomic) BOOL installed;
@end

@implementation HKInstallWatch
- (void)applicationsDidInstall:(NSArray *)applications
{
	(void)applications;
	self.installed = YES;
	CFRunLoopStop(CFRunLoopGetMain());
}
- (void)applicationsDidUninstall:(NSArray *)applications
{
	(void)applications;
}
@end

#define ALLOWED_WHEN_IN_USE 4           /* (kCLAuthorizationStatusAuthorizedWhenInUse) */
#define ALLOWED_ALWAYS      3           /* (kCLAuthorizationStatusAuthorizedAlways) */

static BOOL tcc_everything;             /* --all: every service of the list, not only the four a round meets */

/* TCC's answers for BID set to yes (TCC.framework, private; looked up at run time): camera, microphone, photos, and
   with --all the rest of what an application may ask for */
static void
tcc_allow(NSString *bid)
{
	typedef Boolean (*set_fn)(CFStringRef service, CFStringRef bundle_id, Boolean granted);
	static set_fn set;
	static const char *services[] = { "kTCCServiceCamera", "kTCCServiceMicrophone", "kTCCServicePhotos",
		"kTCCServicePhotosAdd",
		/* (--all from here) */
		"kTCCServiceAddressBook", "kTCCServiceCalendar", "kTCCServiceReminders", "kTCCServiceMediaLibrary",
		"kTCCServiceSpeechRecognition", "kTCCServiceMotion", "kTCCServiceBluetoothAlways",
		"kTCCServiceBluetoothPeripheral", "kTCCServiceUserTracking", "kTCCServiceWillow", "kTCCServiceFocusStatus",
		"kTCCServiceNearbyInteraction", "kTCCServiceFaceID", "kTCCServiceSiri" };
	size_t count = tcc_everything ? sizeof(services) / sizeof(services[0]) : 4;
	char said[1024] = "";

	if (set == NULL) {
		void *tcc = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_NOW);

		set = tcc ? (set_fn)dlsym(tcc, "TCCAccessSetForBundleId") : NULL;
		if (set == NULL) {
			printf("loc_enable: no TCCAccessSetForBundleId here\n");
			return;
		}
	}
	for (size_t i = 0; i < count; i++) {
		CFStringRef service = CFStringCreateWithCString(NULL, services[i], kCFStringEncodingUTF8);
		Boolean ok = set(service, (__bridge CFStringRef)bid, true);

		CFRelease(service);
		snprintf(said + strlen(said), sizeof(said) - strlen(said), " %s %s", services[i] + 11, ok ? "yes" : "REFUSED");
	}
	printf("loc_enable: %s privacy:%s\n", bid.UTF8String, said);
	fflush(stdout);
}

/* each of IDS allowed location "while using" once it is installed (and, with TCC, its privacy prompts answered);
   0 when all are */
static int
allow(NSMutableArray<NSString *> *ids, BOOL tcc)
{
	Class proxy;

	dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
	proxy = NSClassFromString(@"LSApplicationProxy");
	if (proxy == Nil || ![CLLocationManager respondsToSelector:@selector(setAuthorizationStatusByType:forBundleIdentifier:)]) {
		printf("loc_enable: no LSApplicationProxy or no setAuthorizationStatusByType:forBundleIdentifier: here\n");
		return 1;
	}
	for (int waited = 0; ids.count > 0 && waited < 15 * 60; waited += 3) {
		for (NSString *bid in [ids copy]) {
			if ([(LSApplicationProxy *)[proxy applicationProxyForIdentifier:bid] bundleURL] == nil) {
				continue;
			}
			[CLLocationManager setAuthorizationStatusByType:ALLOWED_WHEN_IN_USE forBundleIdentifier:bid];
			usleep(300000);
			int now = [CLLocationManager respondsToSelector:@selector(authorizationStatusForBundleIdentifier:)] ?
			    [CLLocationManager authorizationStatusForBundleIdentifier:bid] : -1;

			printf("loc_enable: %s allowed location while in use (status now %d) after %d s\n", bid.UTF8String, now,
			    waited);
			fflush(stdout);
			if (tcc) {
				tcc_allow(bid);
			}
			[ids removeObject:bid];
		}
		if (ids.count > 0) {
			sleep(3);
		}
	}
	for (NSString *bid in ids) {
		printf("loc_enable: %s never installed (15 min): not allowed\n", bid.UTF8String);
	}
	return ids.count > 0 ? 3 : 0;
}

/*
 * Photos: what TCCAccessSetForBundleId writes is a version 1 record (auth_value 2, reason 4 "system set", version 1);
 * since iOS 14's limited library PhotoKit reads only version 2 ones and asked anyway (11-15: System Check's Photos alert
 * with --all on). The row a user's "Allow Access to All Photos" writes is auth_value 2, auth_reason 2, auth_version 2
 * (the phone's TCC.db, 11-15). So those rows are made so in the database itself and tccd started afresh (launchd
 * starts it again at the next request; it keeps answers cached, so an edit alone would not be read).
 */
#include <signal.h>
#include <sqlite3.h>

extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

static void
photos_version_two(void)
{
	sqlite3 *db = NULL;
	int changed = 0;

	if (sqlite3_open("/private/var/mobile/Library/TCC/TCC.db", &db) != SQLITE_OK) {
		printf("loc_enable: TCC.db would not open: %s\n", db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return;
	}
	sqlite3_busy_timeout(db, 5000);
	if (sqlite3_exec(db, "UPDATE access SET auth_version = 2, auth_reason = 2 WHERE service IN ('kTCCServicePhotos', "
	    "'kTCCServicePhotosAdd') AND auth_value = 2 AND auth_version < 2", NULL, NULL, NULL) == SQLITE_OK) {
		changed = sqlite3_changes(db);
	} else {
		printf("loc_enable: TCC.db: %s\n", sqlite3_errmsg(db));
	}
	sqlite3_close(db);
	if (changed == 0) {
		return;
	}
	/* (tccd started afresh, to read the rows again) */
	pid_t pids[2048];
	int n = proc_listallpids(pids, sizeof(pids)), killed = 0;

	for (int i = 0; i < n && i < 2048; i++) {
		char name[64] = "";

		if (pids[i] > 0 && proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, "tccd") == 0) {
			killed += kill(pids[i], SIGTERM) == 0;
		}
	}
	printf("loc_enable: --all: %d Photos rows made version 2 (as \"Allow Access to All Photos\" writes them); tccd "
	    "restarted (%d)\n", changed, killed);
	fflush(stdout);
}

/*
 * Boot-arg loc_poll=N: --all looks for new applications every N seconds (10, the old way, is the default);
 * LaunchServices' install notes still cut a wait short. Each look lists every application through lsd, so a census
 * of an idle phone wants it rare (11-21, WEEK-1121 P1: loc_poll=600).
 */
/* the boot-arg NAME=N's N (NAME without the "="), or `otherwise' when it is not there */
static unsigned
boot_arg_uint(const char *name, unsigned otherwise)
{
	char args[1100] = "", key[64];
	size_t n = sizeof(args) - 1, k;
	unsigned v = otherwise;

	snprintf(key, sizeof(key), "%s=", name);
	k = strlen(key);
	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) == 0) {
		for (char *p = strstr(args, key); p != NULL; p = strstr(p + 1, key)) {
			if (p == args || p[-1] == ' ') {
				v = (unsigned)strtoul(p + k, NULL, 10);
				break;
			}
		}
	}
	return v;
}

static unsigned
loc_poll_seconds(void)
{
	unsigned v = boot_arg_uint("loc_poll", 10);

	return v ? v : 10;
}

/* --all: every application there is, then every new one, for as long as the system runs */
static int
allow_all(void)
{
	Class workspace = NSClassFromString(@"LSApplicationWorkspace");
	NSMutableSet<NSString *> *done = [NSMutableSet set];
	unsigned poll = loc_poll_seconds();

	if (workspace == Nil || ![CLLocationManager respondsToSelector:@selector(setAuthorizationStatusByType:forBundleIdentifier:)]) {
		printf("loc_enable: no LSApplicationWorkspace or no setAuthorizationStatusByType:forBundleIdentifier: here\n");
		return 1;
	}
	tcc_everything = YES;
	HKInstallWatch *watch = [HKInstallWatch new];
	/*
	 * (11-24, WEEK-1121 N22) boot-arg loc_photos_v2=0: the Photos rows left as TCCAccessSetForBundleId wrote them (no
	 * edit, no tccd restart). The restart after the first look is followed by ~8 min of tccd at ~45 % of a core
	 * (+ locationd ~14 %, launchd ~4 %) on every test boot (logs 051726 / 061348 / 063423: the look itself took ~2 s) --
	 * is the restart the cost? 1 / absent, the default: edited and restarted, the old way (PhotoKit then sees v2).
	 */
	unsigned photos_v2 = boot_arg_uint("loc_photos_v2", 1);

	if (poll != 10) {
		printf("loc_enable: --all: a look every %u s (loc_poll)\n", poll);
		fflush(stdout);
	}
	if (!photos_v2) {
		printf("loc_enable: --all: the Photos rows left at version 1, tccd not restarted (loc_photos_v2=0)\n");
		fflush(stdout);
	}

	if ([[workspace defaultWorkspace] respondsToSelector:@selector(addObserver:)]) {
		[[workspace defaultWorkspace] addObserver:watch];
	}
	for (unsigned round = 0;; round++) {
		@autoreleasepool {
			unsigned added = 0;

			for (LSApplicationProxy *app in [[workspace defaultWorkspace] allApplications]) {
				NSString *bid = app.bundleIdentifier;

				if (bid.length == 0 || [done containsObject:bid]) {
					continue;
				}
				[done addObject:bid];
				[CLLocationManager setAuthorizationStatusByType:ALLOWED_ALWAYS forBundleIdentifier:bid];
				tcc_allow(bid);
				added++;
			}
			if (added > 0) {
				printf("loc_enable: --all: %u more applications allowed everything (%lu in all, look %u)\n", added,
				    (unsigned long)done.count, round);
				fflush(stdout);
				if (photos_v2) {
					sleep(1);       /* (tccd's writes in first) */
					photos_version_two();
				}
			}
		}
		/* (loc_poll s, or until LaunchServices says something was installed: HKInstallWatch stops the run loop) */
		watch.installed = NO;
		uint64_t before = clock_gettime_nsec_np(CLOCK_MONOTONIC);

		if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, (double)poll, false) == kCFRunLoopRunFinished ||
		    (!watch.installed && clock_gettime_nsec_np(CLOCK_MONOTONIC) - before < 1000000000ull)) {
			sleep(poll);    /* (a run loop with nothing in it returns at once: no busy looking) */
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		BOOL want = argc < 2 || strcmp(argv[1], "0") != 0, before, after = NO;

		before = [CLLocationManager locationServicesEnabled];
		if (![CLLocationManager respondsToSelector:@selector(setLocationServicesEnabled:)]) {
			printf("loc_enable: CoreLocation has no setLocationServicesEnabled: here\n");
			return 1;
		}
		[CLLocationManager setLocationServicesEnabled:want];
		/* (locationd takes it over XPC: look again for a few seconds) */
		for (int i = 0; i < 20; i++) {
			after = [CLLocationManager locationServicesEnabled];
			if (after == want) {
				break;
			}
			usleep(250000);
		}
		printf("loc_enable: Location Services were %s, asked %s, now %s\n", before ? "on" : "off", want ? "on" : "off",
		    after ? "on" : "off");
		fflush(stdout);
		if (after != want) {
			return 2;
		}
		if (want && argc > 2) {
			NSMutableArray<NSString *> *ids = [NSMutableArray array];
			BOOL tcc = NO, all = NO;

			for (int i = 2; i < argc; i++) {
				if (strcmp(argv[i], "--tcc") == 0) {
					tcc = YES;
				} else if (strcmp(argv[i], "--all") == 0) {
					all = YES;
				} else {
					[ids addObject:@(argv[i])];
				}
			}
			if (all) {
				dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
				return allow_all();
			}
			return allow(ids, tcc);
		}
		return 0;
	}
}
