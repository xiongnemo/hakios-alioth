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
 * usage: gnss_feed [-d DELIVERY] [-r REPEAT] [-x XTRA_FILE]   (CLSimulationManager's behaviours, not set unless\n *        given; XTRA's file on Qualcomm's servers, xtra3grcej.bin: GPS, GLONASS, BeiDou, Galileo, QZSS)
 * The job com.hakios.gnss runs it at boot; with no kern.hakios.gnss, or no session asked for, it ends at once.
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
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

/*
 * XTRA (boot-arg mdm_xtra=1: kern.hakios.gnss_xtra reads 1): Qualcomm's predicted orbits, the file Android's
 * xtra-daemon fetches, from its servers over Wi-Fi, written to the kernel, which gives it to the modem's engine (QMI LOC
 * 0x35): a first fix in minutes instead of 6-13 (rounds 35-37). Tried every 15 s while the network comes up.
 */
static void
xtra_fetch(const char *name)
{
	uint32_t state = 0;
	size_t n = sizeof(state);
	NSString *file = [NSString stringWithUTF8String:name];

	if (sysctlbyname("kern.hakios.gnss_xtra", &state, &n, NULL, 0) != 0 || state != 1) {
		say("gnss_feed: XTRA not asked for by the kernel (state %u; boot-arg mdm_xtra=1 asks)\n", state);
		return;
	}
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		NSArray<NSString *> *hosts = @[ @"xtrapath1", @"xtrapath2", @"xtrapath3" ];

		for (int attempt = 0; attempt < 60; attempt++) {
			NSString *host = hosts[(NSUInteger)attempt % hosts.count];
			NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"https://%@.izatcloud.net/%@", host, file]];
			NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
			dispatch_semaphore_t done = dispatch_semaphore_create(0);
			__block NSData *body = nil;
			__block NSInteger code = 0;
			__block NSString *failure = nil;
			NSURLSession *s;

			c.timeoutIntervalForRequest = 20;
			s = [NSURLSession sessionWithConfiguration:c];
			[[s dataTaskWithURL:url completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
				body = d;
				code = [r isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)r).statusCode : 0;
				failure = e.localizedDescription;
				dispatch_semaphore_signal(done);
			}] resume];
			dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * (int64_t)NSEC_PER_SEC));
			[s invalidateAndCancel];
			if (code == 200 && body.length >= 1024 && body.length <= 256 * 1024) {
				if (sysctlbyname("kern.hakios.gnss_xtra", NULL, NULL, (void *)body.bytes, body.length) == 0) {
					say("gnss_feed: XTRA: %lu bytes of %s from %s given to the kernel (try %d)\n",
					    (unsigned long)body.length, name, host.UTF8String, attempt + 1);
				} else {
					say("gnss_feed: XTRA: the kernel did not take it (%s)\n", strerror(errno));
				}
				return;
			}
			if (attempt < 3 || attempt % 10 == 0) {
				say("gnss_feed: XTRA: %s: %s (try %d; again in 15 s)\n", host.UTF8String,
				    failure ? failure.UTF8String : [NSString stringWithFormat:@"HTTP %ld, %lu bytes", (long)code,
				    (unsigned long)body.length].UTF8String, attempt + 1);
			}
			sleep(15);
		}
		say("gnss_feed: XTRA: not fetched; the engine goes on without it\n");
	});
}

/*
 * (2026-11-14, the owner: every assistance there is) THE SEED POSITION: iOS's own Wi-Fi location (Apple's service) --
 * asked as a location bundle of ours, /redwood/LocationBundles/HakiosGNSS.bundle, which this program authorizes itself
 * (entitlements com.apple.locationd.authorizeapplications and .effective_bundle) -- handed to the kernel
 * (kern.hakios.gnss_seed), which gives it to the modem's engine with QMI LOC INJECT_POSITION while it has no fix.
 * Our own (simulated) locations are not taken. Stops once the modem has a fix. The position is never written anywhere.
 */
#define SEED_BUNDLE "/redwood/LocationBundles/HakiosGNSS.bundle"

struct hk_gnss_seed {
	uint64_t latitude_bits, longitude_bits;
	uint32_t accuracy_bits, source;
	uint64_t utc_ms;
};
struct hk_gnss_time {
	uint64_t utc_ms;
	uint32_t uncertainty_ms, reserved;
};

@interface HKSeed : NSObject <CLLocationManagerDelegate>
@property (nonatomic) CLLocationManager *manager;
@property (nonatomic) double given;          /* the accuracy of the last seed given (0: none) */
@end

@implementation HKSeed

- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	(void)manager;
	for (CLLocation *l in locations) {
		BOOL simulated = NO;
		struct hk_gnss_seed seed;
		double acc = l.horizontalAccuracy;
		float accf = (float)acc;
		double lat = l.coordinate.latitude, lon = l.coordinate.longitude;

		if (@available(iOS 15.0, *)) {
			simulated = l.sourceInformation.isSimulatedBySoftware;
		}
		/* (a first one, or one at least twice as good; never our own) */
		if (simulated || acc <= 0 || acc > 5000 || (self.given > 0 && acc > self.given / 2)) {
			continue;
		}
		memset(&seed, 0, sizeof(seed));
		memcpy(&seed.latitude_bits, &lat, 8);
		memcpy(&seed.longitude_bits, &lon, 8);
		memcpy(&seed.accuracy_bits, &accf, 4);
		seed.source = 3;                        /* (QMI LOC: Wi-Fi) */
		seed.utc_ms = (uint64_t)(l.timestamp.timeIntervalSince1970 * 1000);
		if (sysctlbyname("kern.hakios.gnss_seed", NULL, NULL, &seed, sizeof(seed)) == 0) {
			self.given = acc;
			say("gnss_feed: seed: iOS's Wi-Fi location, +-%.0f m, given to the modem (the position is not written)\n", acc);
		} else {
			say("gnss_feed: seed: the kernel did not take it (%s)\n", strerror(errno));
		}
	}
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	(void)manager;
	say("gnss_feed: seed: no Wi-Fi location yet (%s)\n", error.localizedDescription.UTF8String ?: "?");
}

@end

static HKSeed *seed_source;

static void
seed_start(void)
{
	NSString *path = @SEED_BUNDLE;
	Class cl = [CLLocationManager class];
	SEL authorize = sel_registerName("setAuthorizationStatus:forBundlePath:");
	SEL init = sel_registerName("initWithEffectiveBundlePath:delegate:onQueue:");
	dispatch_queue_t queue = dispatch_queue_create("gnss_feed.seed", DISPATCH_QUEUE_SERIAL);

	if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
		say("gnss_feed: seed: no %s: no seed position\n", SEED_BUNDLE);
		return;
	}
	if ([cl respondsToSelector:authorize]) {
		((void (*)(id, SEL, BOOL, id))objc_msgSend)(cl, authorize, YES, path);
	}
	if (![cl instancesRespondToSelector:init]) {
		say("gnss_feed: seed: CoreLocation has no initWithEffectiveBundlePath:delegate:onQueue: here\n");
		return;
	}
	seed_source = [HKSeed new];
	dispatch_async(queue, ^{
		CLLocationManager *m = ((id (*)(id, SEL, id, id, id))objc_msgSend)([CLLocationManager alloc], init, path,
		    seed_source, queue);

		seed_source.manager = m;
		m.desiredAccuracy = kCLLocationAccuracyHundredMeters;
		[m startUpdatingLocation];
		say("gnss_feed: seed: asking iOS for its Wi-Fi location (as %s)\n", SEED_BUNDLE);
	});
}

static void
seed_stop(void)
{
	if (seed_source.manager != nil) {
		CLLocationManager *m = seed_source.manager;

		seed_source.manager = nil;
		[m stopUpdatingLocation];
		say("gnss_feed: seed: the modem has its own fix: no more Wi-Fi locations asked\n");
	}
}

/*
 * THE PRECISE TIME: SNTP to time.apple.com once the network is up -- the server's transmit time plus half the round
 * trip, the round trip's half (and 20 ms) as the uncertainty -- handed to the kernel (kern.hakios.gnss_time), which
 * gives it to the engine with INJECT_UTC_TIME. (The calendar the kernel injects first came from the loader's epoch=,
 * +-3 s.)
 */
static bool
sntp_once(const char *host, struct hk_gnss_time *out)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
	uint8_t packet[48] = { 0x23 };          /* (LI 0, version 4, client) */
	struct timeval tv = { 2, 0 }, t1, t4;
	int s;
	bool ok = false;

	if (getaddrinfo(host, "123", &hints, &ai) != 0 || ai == NULL) {
		return false;
	}
	s = socket(ai->ai_family, ai->ai_socktype, 0);
	if (s >= 0) {
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		gettimeofday(&t1, NULL);
		if (sendto(s, packet, sizeof(packet), 0, ai->ai_addr, ai->ai_addrlen) == sizeof(packet) &&
		    recv(s, packet, sizeof(packet), 0) == sizeof(packet)) {
			uint32_t secs = (uint32_t)packet[40] << 24 | (uint32_t)packet[41] << 16 | (uint32_t)packet[42] << 8 | packet[43];
			uint32_t frac = (uint32_t)packet[44] << 24 | (uint32_t)packet[45] << 16 | (uint32_t)packet[46] << 8 | packet[47];
			uint64_t rtt_ms;

			gettimeofday(&t4, NULL);
			rtt_ms = (uint64_t)((t4.tv_sec - t1.tv_sec) * 1000 + (t4.tv_usec - t1.tv_usec) / 1000);
			if (secs > 2208988800u && rtt_ms < 3000) {
				out->utc_ms = (uint64_t)(secs - 2208988800u) * 1000 + ((uint64_t)frac * 1000 >> 32) + rtt_ms / 2;
				out->uncertainty_ms = (uint32_t)(rtt_ms / 2 + 20);
				ok = true;
			}
		}
		close(s);
	}
	freeaddrinfo(ai);
	return ok;
}

static void
time_fetch(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		for (int attempt = 0; attempt < 40; attempt++) {
			struct hk_gnss_time t = { 0 };

			if (sntp_once("time.apple.com", &t)) {
				if (sysctlbyname("kern.hakios.gnss_time", NULL, NULL, &t, sizeof(t)) == 0) {
					say("gnss_feed: time: from time.apple.com, +-%u ms, given to the modem (try %d)\n", t.uncertainty_ms,
					    attempt + 1);
				}
				return;
			}
			sleep(15);
		}
		say("gnss_feed: time: no answer from time.apple.com\n");
	});
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
		const char *xtra_name = "xtra3grcej.bin";
		bool active = false;
		uint32_t last_reports;
		unsigned fed = 0, sessions = 0, waited = 0;
		time_t last_fix = 0, said = 0;
		float last_acc = 0, best_acc = 1e9f;
		Class cls;
		id<HKLocationSimulation> sim;

		setvbuf(stdout, NULL, _IOLBF, 0);
		while ((opt = getopt(argc, argv, "d:r:x:")) != -1) {
			if (opt == 'd') {
				delivery = atoi(optarg);
			} else if (opt == 'r') {
				repeat = atoi(optarg);
			} else if (opt == 'x') {
				xtra_name = optarg;
			} else {
				fprintf(stderr, "usage: gnss_feed [-d DELIVERY] [-r REPEAT] [-x XTRA_FILE]\n");
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
		xtra_fetch(xtra_name);
		time_fetch();
		seed_start();
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
						seed_stop();
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
