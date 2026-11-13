/*
 * gnss_feed: the SDX55 modem's own GNSS fixes into iOS's location services.
 *
 * The kernel (bsd/dev/alioth_mdm.c, boot-arg mdm_loc=1) runs a QMI LOC session on the Redmi K40's external SDX55 and
 * keeps its last position report and satellites in the sysctl kern.hakios.gnss (struct alioth_gnss, version 1;
 * numbers as their IEEE bits). First fix on the phone 2026-11-14, indoors, unassisted: 716 s after the modem's reset,
 * best 12 m. locationd has no GNSS of its own here (an iPhone's comes from its baseband, which this phone does not
 * have), so applications got Apple's Wi-Fi position only. This daemon hands each new fix (a report of status 0 with a
 * position and its accuracy) to locationd through CoreLocation's location simulation -- CLSimulationManager, what
 * Xcode's "simulate location" uses, the entitlement com.apple.locationd.simulation -- and stops it when no fix has
 * come for FIX_TIMEOUT seconds, so that locationd's own (Wi-Fi) location is used again. The locations are the
 * modem's real ones; applications that look see them marked as simulated (CLLocationSourceInformation).
 *
 * PRIVACY: the position is the owner's. Nothing here prints it: only counts and accuracy.
 *
 * usage: gnss_feed [-d DELIVERY] [-r REPEAT]   (CLSimulationManager's behaviours; not set unless given)
 * The job com.hakios.gnss runs it at boot; with no kern.hakios.gnss, or no session asked for, it ends at once.
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define FIX_TIMEOUT     15      /* seconds without a fix before locationd is let go */
#define WAIT_SESSION    180     /* seconds to wait at boot for the kernel to begin the session */

/* (bsd/dev/alioth_mdm.c's, as it is) */
#define GNSS_SATS 64
struct alioth_gnss_sat {
	uint32_t system;
	uint16_t prn;
	uint8_t status;
	uint8_t used;
	uint32_t elevation_bits, azimuth_bits, snr_bits;
};
struct alioth_gnss {
	uint32_t version, size;
	uint64_t updated_ms;
	uint32_t state, engine_state, session_status, opmode;
	uint32_t reports, fixes, sv_reports;
	uint32_t valid;
	uint64_t latitude_bits, longitude_bits;
	uint32_t accuracy_bits, altitude_ellipsoid_bits, altitude_msl_bits, speed_bits, heading_bits, vertical_bits;
	uint64_t utc_ms;
	uint32_t sat_count;
	struct alioth_gnss_sat sat[GNSS_SATS];
};
#define GNSS_V_POSITION         (1u << 0)
#define GNSS_V_ACCURACY         (1u << 1)
#define GNSS_V_ALTITUDE_ELL     (1u << 2)
#define GNSS_V_ALTITUDE_MSL     (1u << 3)
#define GNSS_V_SPEED            (1u << 4)
#define GNSS_V_HEADING          (1u << 5)
#define GNSS_V_VERTICAL         (1u << 6)

/* CoreLocation's private class (iOS 16.5: all of these are there) */
@protocol HKLocationSimulation
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)location;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
- (void)flush;
- (void)setLocationDeliveryBehavior:(uint8_t)behavior;
- (void)setLocationRepeatBehavior:(uint8_t)behavior;
@end

static volatile sig_atomic_t stopping;

/*
 * a line to the console and to /var/tmp/gnss_feed.log (a daemon's console is not in the kernel's log the tests read;
 * the computer reads this file over the shell). Counts and accuracy only.
 */
static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[256];
	va_list arguments;
	FILE *f;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	fputs(line, stdout);
	if ((f = fopen("/var/tmp/gnss_feed.log", "a")) != NULL) {
		time_t now = time(NULL);
		struct tm tm;
		char when[16];

		strftime(when, sizeof(when), "%H:%M:%S", localtime_r(&now, &tm));
		fprintf(f, "%s %s", when, line);
		fclose(f);
	}
}

static void
on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static bool
read_gnss(struct alioth_gnss *g)
{
	size_t n = sizeof(*g);

	return sysctlbyname("kern.hakios.gnss", g, &n, NULL, 0) == 0 && n == sizeof(*g) && g->version == 1 &&
	    g->size == sizeof(*g);
}

static float
f32(uint32_t bits)
{
	float f;

	memcpy(&f, &bits, sizeof(f));
	return f;
}

static double
f64(uint64_t bits)
{
	double d;

	memcpy(&d, &bits, sizeof(d));
	return d;
}

static void
let_go(id<HKLocationSimulation> sim)
{
	[sim stopLocationSimulation];
	[sim clearSimulatedLocations];
	if ([(id)sim respondsToSelector:@selector(flush)]) {
		[sim flush];
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		struct alioth_gnss g;
		int delivery = -1, repeat = -1, opt;
		bool active = false;
		uint32_t last_reports;
		unsigned fed = 0, sessions = 0, waited = 0;
		time_t last_fix = 0, said = 0;
		float last_acc = 0, best_acc = 1e9f;
		Class cls;
		id<HKLocationSimulation> sim;

		setvbuf(stdout, NULL, _IOLBF, 0);
		while ((opt = getopt(argc, argv, "d:r:")) != -1) {
			if (opt == 'd') {
				delivery = atoi(optarg);
			} else if (opt == 'r') {
				repeat = atoi(optarg);
			} else {
				fprintf(stderr, "usage: gnss_feed [-d DELIVERY] [-r REPEAT]\n");
				return 64;
			}
		}
		if (!read_gnss(&g)) {
			say("gnss_feed: no kern.hakios.gnss here (a kernel without the modem's GNSS): nothing to do\n");
			return 0;
		}
		/* (the boot job starts before the kernel's modem thread has asked for a session: wait for it a while) */
		while (g.state == 0 && waited < WAIT_SESSION) {
			sleep(5);
			waited += 5;
			if (!read_gnss(&g)) {
				return 0;
			}
		}
		if (g.state == 0) {
			say("gnss_feed: no GNSS session asked of the modem (boot-arg mdm_loc=1): nothing to do\n");
			return 0;
		}
		cls = NSClassFromString(@"CLSimulationManager");
		if (cls == Nil) {
			say("gnss_feed: CoreLocation has no CLSimulationManager here\n");
			return 1;
		}
		sim = (id<HKLocationSimulation>)[[cls alloc] init];
		if (delivery >= 0) {
			[sim setLocationDeliveryBehavior:(uint8_t)delivery];
		}
		if (repeat >= 0) {
			[sim setLocationRepeatBehavior:(uint8_t)repeat];
		}
		signal(SIGTERM, on_signal);
		signal(SIGINT, on_signal);
		last_reports = g.reports;
		say("gnss_feed: watching kern.hakios.gnss (session state %u): the modem's fixes go to locationd\n", g.state);
		while (!stopping) {
			time_t now;

			usleep(500000);
			if (!read_gnss(&g)) {
				continue;
			}
			now = time(NULL);
			if (g.reports != last_reports) {
				last_reports = g.reports;
				if (g.session_status == 0 && (g.valid & (GNSS_V_POSITION | GNSS_V_ACCURACY)) ==
				    (GNSS_V_POSITION | GNSS_V_ACCURACY)) {
					CLLocationCoordinate2D c = { f64(g.latitude_bits), f64(g.longitude_bits) };
					double acc = f32(g.accuracy_bits), altitude = 0, vertical = -1, speed = -1, course = -1;

					if (CLLocationCoordinate2DIsValid(c) && acc > 0 && acc < 100000) {
						CLLocation *location;

						if (g.valid & (GNSS_V_ALTITUDE_MSL | GNSS_V_ALTITUDE_ELL)) {
							altitude = f32((g.valid & GNSS_V_ALTITUDE_MSL) ? g.altitude_msl_bits : g.altitude_ellipsoid_bits);
							vertical = (g.valid & GNSS_V_VERTICAL) ? f32(g.vertical_bits) : -1;
						}
						if (g.valid & GNSS_V_SPEED) {
							speed = f32(g.speed_bits);
						}
						if ((g.valid & GNSS_V_HEADING) && speed > 0.5) {
							course = f32(g.heading_bits);
						}
						location = [[CLLocation alloc] initWithCoordinate:c altitude:altitude horizontalAccuracy:acc
						    verticalAccuracy:vertical course:course speed:speed timestamp:[NSDate date]];
						[sim clearSimulatedLocations];
						[sim appendSimulatedLocation:location];
						if ([(id)sim respondsToSelector:@selector(flush)]) {
							[sim flush];
						}
						if (!active) {
							[sim startLocationSimulation];
							active = true;
							sessions++;
							say("gnss_feed: A FIX: locationd gets the modem's position from now on (+-%.0f m)\n", acc);
						}
						fed++;
						last_fix = now;
						last_acc = (float)acc;
						best_acc = (float)acc < best_acc ? (float)acc : best_acc;
					}
				}
			}
			if (active && now - last_fix > FIX_TIMEOUT) {
				let_go(sim);
				active = false;
				say("gnss_feed: no fix for %d s: locationd is back on its own location (%u fixes given so far)\n",
				    FIX_TIMEOUT, fed);
			}
			if (active && now - said >= 60) {
				said = now;
				say("gnss_feed: %u fixes given to locationd, the last +-%.0f m, the best +-%.0f m\n", fed, last_acc,
				    best_acc);
			}
		}
		if (active) {
			let_go(sim);
		}
		say("gnss_feed: stopped: %u fixes given in %u stretches\n", fed, sessions);
		return 0;
	}
}
