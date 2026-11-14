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
 * usage: loc_enable [1|0] [BUNDLE-ID ...]   (no argument: 1)
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
@end

#define ALLOWED_WHEN_IN_USE 4           /* (kCLAuthorizationStatusAuthorizedWhenInUse) */

/* each of IDS allowed location "while using" once it is installed; 0 when all are */
static int
allow(NSMutableArray<NSString *> *ids)
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

			for (int i = 2; i < argc; i++) {
				[ids addObject:@(argv[i])];
			}
			return allow(ids);
		}
		return 0;
	}
}
