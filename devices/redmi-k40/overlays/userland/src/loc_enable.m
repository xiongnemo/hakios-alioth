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
 * own (Camera asked for location in a round and its alert stayed up) and every one installed later, looked for every
 * 10 seconds for as long as the system runs -- gets location "always" and every privacy service below answered yes.
 * usage: loc_enable [1|0] [--tcc] [--all] [BUNDLE-ID ...]   (no argument: 1)
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
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

/* --all: every application there is, then every new one, for as long as the system runs */
static int
allow_all(void)
{
	Class workspace = NSClassFromString(@"LSApplicationWorkspace");
	NSMutableSet<NSString *> *done = [NSMutableSet set];

	if (workspace == Nil || ![CLLocationManager respondsToSelector:@selector(setAuthorizationStatusByType:forBundleIdentifier:)]) {
		printf("loc_enable: no LSApplicationWorkspace or no setAuthorizationStatusByType:forBundleIdentifier: here\n");
		return 1;
	}
	tcc_everything = YES;
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
			}
		}
		sleep(10);
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
