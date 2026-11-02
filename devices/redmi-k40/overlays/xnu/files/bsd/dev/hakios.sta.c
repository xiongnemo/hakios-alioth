/*
 * Darwin-on-hakios: what stands between the two sides of the station boundary
 * (sys/hakios.sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the last scan heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as iOS itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.sta.h>
#include <sys/systm.h>

extern void thread_set_thread_name(thread_t thread, const char *name);

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hakios-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hakios_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static struct hakios_sta_scan asked_scan_what;
static struct hakios_sta_join asked_join_what;
static struct hakios_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hakios_sta_join fallback;        /* ... as it was before a join that has not come through yet */

static struct hakios_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct hakios_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

/*
 * Redmi K40 overlay: the standing network tried again (boot-arg wifi_retry=0: not). It used to be joined once at the
 * start and once whenever the radio came back on, and a join that failed -- an access point that deauthenticated us
 * the whole run, 2026-11-02 -- left the phone off the network until somebody turned Wi-Fi off and on in Settings.
 * Now a failed join of the standing network, or its loss once joined (hakios_sta_lost()), brings it back after 5, 10,
 * 20, 40, 60 s, then every 60 s, while the radio is on and nobody has asked for anything else; a leave asked for
 * (Settings) stops it until a join or the radio's power is asked for again. Boot-arg wifi_test_fail=N: the first N
 * joins fail without being tried (to see the retries).
 */
static uint64_t retry_at;                       /* mach time of the next try (0: none) */
static unsigned retry_count;
static bool retry_blocked, retry_checked, retry_off;
static uint32_t test_fail_left;

static void
retry_args(void)                                /* (sta_lock held) */
{
	uint32_t on = 1;

	if (retry_checked) {
		return;
	}
	retry_checked = true;
	PE_parse_boot_argn("wifi_retry", &on, sizeof(on));
	retry_off = on == 0;
	PE_parse_boot_argn("wifi_test_fail", &test_fail_left, sizeof(test_fail_left));
}

static void
retry_later(uint64_t after_ms)                  /* (sta_lock held) */
{
	uint64_t interval;

	retry_args();
	if (retry_off || retry_blocked || standing.ssid_length == 0 || !sta_on) {
		return;
	}
	nanoseconds_to_absolutetime(after_ms * NSEC_PER_MSEC, &interval);
	retry_at = mach_absolute_time() + interval;
}

/* Whether the standing network is to be tried again by itself (for what hakios.wifi.c says after the first join). */
bool hakios_sta_retrying(void);
bool
hakios_sta_retrying(void)
{
	bool retrying;

	lck_mtx_lock(&sta_lock);
	retrying = retry_at != 0;
	lck_mtx_unlock(&sta_lock);
	return retrying;
}

static void
heard_one(const struct hakios_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan scan;
	static struct hakios_sta_join join;

	for (;;) {
		const struct hakios_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan)) {
			if (retry_at != 0 && sta_ops != NULL) {
				if (mach_absolute_time() >= retry_at) {
					/* (the standing network again: nobody has asked for anything meanwhile) */
					retry_at = 0;
					if (!retry_blocked && standing.ssid_length != 0 && sta_on) {
						asked_join_what = standing;
						asked_join = true;
					}
					continue;
				}
				lck_mtx_sleep_deadline(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT, retry_at);
			} else {
				lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
			}
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (power) {
			struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HAKIOS_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 0);
			}
			hakios_apple80211_event(HAKIOS_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join) {
				asked_join_what = standing;
				asked_join = true;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hakios_apple80211_event(HAKIOS_STA_LINK, 0);
		} else if (do_join) {
			bool was_standing, test_fail = false;
			uint32_t test_left = 0;

			lck_mtx_lock(&sta_lock);
			retry_args();
			retry_at = 0;                   /* (this join is the try) */
			if (test_fail_left > 0) {
				test_left = --test_fail_left;
				test_fail = true;
			}
			lck_mtx_unlock(&sta_lock);
			if (test_fail) {
				error = ETIMEDOUT;
				hakios_wifi_tell("wifi: TEST: this join fails without being tried (wifi_test_fail; %u more)\n", test_left);
			} else {
				error = ops->join(&join);
			}
			hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, error);
			if (error == 0) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 1);
			}
			lck_mtx_lock(&sta_lock);
			/* (still the standing network after the join, unless somebody has asked for another meanwhile) */
			was_standing = standing.ssid_length != 0 && join.ssid_length == standing.ssid_length &&
			    memcmp(join.ssid, standing.ssid, join.ssid_length) == 0;
			bzero(&join, sizeof(join));
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			bzero(&fallback, sizeof(fallback));
			if (error == 0) {
				if (was_standing && retry_count != 0) {
					hakios_wifi_tell("wifi: the standing network is joined, at try %u\n", retry_count + 1);
				}
				retry_count = 0;
			} else if (was_standing && !asked_join && !asked_leave && !asked_power) {
				static const uint32_t backoff_s[] = { 5, 10, 20, 40, 60 };
				uint32_t s = backoff_s[retry_count < 5 ? retry_count : 4];

				retry_later((uint64_t)s * 1000);
				if (retry_at != 0) {
					retry_count++;
					hakios_wifi_tell("wifi: the standing network did not join (%d): again in %u s (try %u)\n", error, s,
					    retry_count + 1);
				}
			}
			lck_mtx_unlock(&sta_lock);
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hakios_sta_attach(const struct hakios_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		asked_power = asked_scan = asked_join = asked_leave = false;
		retry_at = 0;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		hakios_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hakios_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	lck_mtx_lock(&sta_lock);
	sta_ops = ops;
	sta_on = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
}

void
hakios_sta_lost(int error)
{
	hakios_apple80211_event(HAKIOS_STA_LINK, error != 0 ? error : ECONNRESET);
	/* the standing network, if that is what was lost, is joined again by itself (unless a try is already due) */
	lck_mtx_lock(&sta_lock);
	if (retry_at == 0 && !asked_join && !asked_leave && !asked_power) {
		retry_later(2000);
		if (retry_at != 0) {
			hakios_wifi_tell("wifi: the standing network is lost (%d): again in 2 s\n", error);
		}
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hakios_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hakios_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hakios_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hakios_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	retry_blocked = false;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
		retry_at = 0;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_join_standing(const struct hakios_sta_join *what)
{
	int error = hakios_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_leave = false;
	retry_blocked = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	retry_blocked = true;                   /* (left on purpose: not joined again until asked) */
	retry_at = 0;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
{
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
hakios_sta_state(struct hakios_sta_state *state)
{
	const struct hakios_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
hakios_sta_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hakios_sta_address(uint8_t *address)
{
	const struct hakios_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
