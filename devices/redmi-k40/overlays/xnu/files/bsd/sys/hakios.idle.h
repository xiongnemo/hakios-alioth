/*
 * (2026-11-24, WEEK-1121) boot-arg idle_set=1: every idle boot-arg of November 22-24 at the value measured for it
 * (WEEK-1121's boot-args table) in ten characters of the kernel's line. Spelled out they take ~200, and the line has
 * no room for them beside the rest of the week's (1104 of 1022 characters with a release's apps_token).
 * One given by its own name wins, 0 included (idle_set=1 bt_idle=0: Bluetooth as before). Absent / 0, the default:
 * each only by its own name, as before -- all off.
 */
#ifndef _SYS_HAKIOS_IDLE_H_
#define _SYS_HAKIOS_IDLE_H_

#include <pexpert/pexpert.h>

#define HAKIOS_IDLE_GLINK               10      /* glink_idle: the sensor / audio DSP links (hakios.slpi.c, hakios.adsp.c) */
#define HAKIOS_IDLE_FOLLOW              2       /* glink_follow: quick looks after the audio DSP's traffic (hakios.adsp.c; 11-24 evening) */
#define HAKIOS_IDLE_WIFI                4       /* wifi_idle: the IPC router, the ticker, the rings (umi_wifi.c, lx) */
#define HAKIOS_IDLE_SOUND               100     /* sound_idle: the speaker watcher (hakios.sound.c) */
#define HAKIOS_IDLE_FEED                20      /* sound_feed_wait: the sound feeder (hakios.sound.c) */
#define HAKIOS_IDLE_BT                  10      /* bt_idle: the Bluetooth UART worker (umi_bluetooth.c) */
#define HAKIOS_IDLE_MDM                 10      /* mdm_idle: the modem's QRTR listen (alioth_mdm.c) */
#define HAKIOS_IDLE_WIFI_WQ             1000    /* wifi_wq_idle: lx's work queues (lx_base.c) */
#define HAKIOS_IDLE_HAPTIC              100     /* haptic_idle: the haptic service (hakios.haptic.c) */
#define HAKIOS_IDLE_NET                 1000    /* net_idle: a never-polled link's thread (hakios.net.c) */
#define HAKIOS_IDLE_MIC                 100     /* mic_idle: the microphone service (hakios.mic.c) */
#define HAKIOS_IDLE_TORCH               100     /* torch_idle: the torch service (hakios.torch.c) */

/*
 * (2026-11-26, WEEK-1121 N31) boot-arg glink_irq=1, with glink_idle (idle_set's): IPCC's interrupt wakes the audio and
 * sensor DSPs' link threads (hakios.adsp.c, hakios.slpi.c; the shared part in hakios.slpi.c, where it is told).
 */
#define HAKIOS_IPCC_ADSP                0u
#define HAKIOS_IPCC_SLPI                1u
#define HAKIOS_IPCC_ON                  2u      /* hakios_ipcc_irq.state: taken (0 not tried, 1 being taken, 3 not / given up) */
#define GLINK_IRQ_WAIT_MS               1000u   /* a link's longest wait while the interrupt is unmasked */
struct hakios_ipcc_irq {
	volatile uint32_t state;
	volatile uint32_t armed;                /* unmasked: the next ring interrupts */
	volatile uint32_t count;                /* interrupts so far */
	volatile uint32_t rings;                /* the rings both links heard and cleared so far */
	void *volatile event[2];                /* what each link waits on (HAKIOS_IPCC_*); NULL: none */
	uintptr_t ipcc;                         /* IPCC's registers, as the first link mapped them (never unmapped) */
};
struct hakios_ipcc_link {                /* a link thread's own view of it */
	const char *name;
	unsigned who;
	int on;                                 /* joined: it waits for the interrupt */
	int long_wait, quiet_wait;              /* this wait GLINK_IRQ_WAIT_MS; and no interrupt ended it */
	uint32_t irqs, missed, looks, waits, rang, minutes, second_from, second_rings;
	uint64_t minute_at, second_at;
};
extern struct hakios_ipcc_irq hakios_ipcc_irq;
int hakios_ipcc_join(struct hakios_ipcc_link *l, void *event, uintptr_t ipcc, uint32_t received);
int hakios_ipcc_kick_wanted(unsigned who);
uint32_t hakios_ipcc_wait(struct hakios_ipcc_link *l);
int hakios_ipcc_rang(const struct hakios_ipcc_link *l);
void hakios_ipcc_begin(struct hakios_ipcc_link *l);
void hakios_ipcc_looked(struct hakios_ipcc_link *l, uint32_t received, uint32_t rang, int kicked);
void hakios_ipcc_leave(unsigned who);

/* the boot-arg `name' if given; else `set_value' with idle_set=1; else false and *value untouched */
static inline int
hakios_idle_arg(const char *name, uint32_t *value, uint32_t set_value)
{
	uint32_t set = 0;

	if (PE_parse_boot_argn(name, value, sizeof(*value))) {
		return 1;
	}
	if (PE_parse_boot_argn("idle_set", &set, sizeof(set)) && set != 0) {
		*value = set_value;
		return 1;
	}
	return 0;
}

#endif
