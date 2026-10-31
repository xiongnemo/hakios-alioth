/*
 * loc_enable: Location Services' own switch on, as the Setup Assistant turns it on on an iPhone that is set up
 * (+[CLLocationManager setLocationServicesEnabled:], CoreLocation's, what Settings' switch calls too). Nothing set
 * this phone up, so the switch was never on: locationd ran, but Settings had no Location Services and applications
 * got no location (2026-10-31). The image's /private/var is made afresh at every boot: the boot job runs this.
 * usage: loc_enable [1|0]   (no argument: 1)
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

@interface CLLocationManager (HakiosPrivate)
+ (void)setLocationServicesEnabled:(BOOL)enabled;
@end

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
		return after == want ? 0 : 2;
	}
}
