/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * Redwood selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
hakios_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");


/*
 * What the audio DSP's battery manager says (bsd/dev/hakios.adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that iOS reads (iokit/Kernel/hakios.battery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void HakiosBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/hakios.battery.cpp */

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = 1;
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}

/*
 * Redmi K40's battery gauge. PM8150B (SPMI slave 2) carries Qualcomm's "fg-gen4" set -- the same one
 * the raphael port reads (Gsjsjzhznsz/ios-raphael, docs/FLASH-T13.md): fg-batt-soc@4000 with the
 * monotonic state of charge at 0x09 and a shadow copy at 0x0A, fg-batt-info@4100 with the voltage at
 * 0xA0, the current at 0xA2 and the "battery missing" flags at 0x07, and SMB5's chgr@1000 (state at
 * 0x06) and usb@1300 (input present at 0x10 bit 4). The phone's own device tree has all of them:
 * qcom,spmi@c440000/qcom,pm8150b@2/qpnp,fg. Everything here is from that document, including the
 * conversions (they are the vendor kernel's).
 *
 * The transfers go through the PMIC arbiter, as the buttons' reads do; a channel of it names the
 * slave and the peripheral, so the register in a command is the peripheral's own offset. One
 * transfer at a time (hakios_spmi_lock), as Linux has it.
 */
#define BATT_ARB_CORE_PHYS      0x0c440000UL
#define BATT_ARB_CORE_SIZE      0x1100UL
#define BATT_ARB_OBSERVER_PHYS  0x0e600000UL
#define BATT_ARB_OBSERVER_SIZE  0x10000UL
#define BATT_ARB_CHANNELS       0x004           /* [10:0] how many */
#define BATT_ARB_CHANNEL(n)     (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
#define BATT_CHANNEL_COMMAND    0x00
#define BATT_CHANNEL_STATUS     0x08
#define BATT_CHANNEL_DATA0      0x18
#define BATT_CHANNEL_DATA1      0x1c
#define BATT_STATUS_DONE        0x1
#define BATT_STATUS_BAD         (0x2 | 0x4 | 0x8)       /* failure, denied, dropped */
#define BATT_COMMAND_READ       (1u << 27)      /* [11:4] the register, [2:0] the bytes - 1 */
#define BATT_TIMEOUT_US         2000

#define FG_SOC_ID               0x240           /* slave 2, peripheral 0x40: fg-batt-soc@4000 */
#define FG_INFO_ID              0x241           /* ... 0x41: fg-batt-info@4100 */
#define SMB_CHGR_ID             0x210           /* ... 0x10: SMB5's qcom,chgr@1000 */
#define SMB_USB_ID              0x213           /* ... 0x13: SMB5's qcom,usb@1300 */

static vm_offset_t batt_core, batt_observer;
static uint16_t fg_soc_channel, fg_info_channel, smb_chgr_channel = 0xffff, smb_usb_channel = 0xffff;

static inline uint32_t
batt_rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
batt_wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* One transfer. Returns false if the arbiter did not finish or did not carry all the bytes. */
static bool
batt_spmi_once(uint16_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t at = batt_observer + 0x80UL * channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2];

	nanoseconds_to_absolutetime(BATT_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	batt_wr(at + BATT_CHANNEL_COMMAND, BATT_COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = batt_rd(at + BATT_CHANNEL_STATUS);
		now = mach_absolute_time();
	} while (!(status & BATT_STATUS_DONE) && now - start < interval);
	data[0] = batt_rd(at + BATT_CHANNEL_DATA0);
	data[1] = count > 4 ? batt_rd(at + BATT_CHANNEL_DATA1) : 0;
	hakios_spmi_unlock();
	if (!(status & BATT_STATUS_DONE) || (status & BATT_STATUS_BAD)) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return true;
}

/* A transfer is sometimes dropped while the phone's own firmware polls the same PMIC: give it one more. */
static bool
batt_spmi(uint16_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	return batt_spmi_once(channel, reg, bytes, count) || batt_spmi_once(channel, reg, bytes, count);
}

static bool
batt_channel(uint16_t id, uint16_t *channel)
{
	uint32_t channels = batt_rd(batt_core + BATT_ARB_CHANNELS) & 0x7ff;

	if (channels == 0 || channels > 512) {
		channels = 512;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = batt_rd(batt_core + BATT_ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == id) {
			*channel = (uint16_t)n;
			return true;
		}
	}
	return false;
}

/* The vendor kernel's conversion: 255 is 100 per cent, 0 is 0, the rest is (raw - 1) * 98 / 253 + 1. */
static int
batt_percent(uint8_t raw)
{
	if (raw == 255) {
		return 100;
	}
	if (raw == 0) {
		return 0;
	}
	return ((int)raw - 1) * 98 / 253 + 1;
}

static void
battery_gauge_poll(bool say)
{
	uint8_t soc[2], voltage[2], current[2], info[1], chgr[1], usb[1];
	struct hakios_battery_state b;
	int percent, uv = 0, ua = 0, present = 1, external = 0, status = 2;

	if (!batt_spmi(fg_soc_channel, 0x09, soc, 2)) {
		battery_failures++;
		return;
	}
	if (soc[0] != soc[1]) {         /* the shadow copy is the gauge's own check that the read was whole */
		battery_failures++;
		return;
	}
	percent = batt_percent(soc[0]);

	if (batt_spmi(fg_info_channel, 0x07, info, 1)) {
		present = (info[0] & 0x07) == 0;        /* the HW, algorithm and comparator "missing" flags */
	}
	if (present && batt_spmi(fg_info_channel, 0xa0, voltage, 2)) {
		uv = (int)(((int64_t)(voltage[0] | voltage[1] << 8) * 122070) / 1000);
	}
	if (present && batt_spmi(fg_info_channel, 0xa2, current, 2)) {
		int32_t raw = (int32_t)(current[0] | current[1] << 8);
		/*
		 * 16 bits, two's complement. (Read as 15 -- bit 14 the sign -- a charge came out as a discharge of 16 A: with
		 * the panel asleep on USB the battery took 11.7 mA, register 0xffe8, and the sysctl said -15988 mA,
		 * 2026-11-02 06:52.)
		 */
		int32_t signed_raw = (int16_t)raw;
		ua = (int)(((int64_t)signed_raw * 488281) / 1000);
	}
	if (smb_usb_channel != 0xffff && batt_spmi(smb_usb_channel, 0x10, usb, 1)) {
		external = (usb[0] & 0x10) != 0;        /* USBIN_PLUGIN, bit 4 */
	}
	if (external && smb_chgr_channel != 0xffff && batt_spmi(smb_chgr_channel, 0x06, chgr, 1)) {
		int state = chgr[0] & 0x07;             /* 1 to 4 trickle, pre, fast, taper; 5 terminated */
		status = state >= 1 && state <= 4 ? 1 : state == 5 ? 4 : 3;
	} else if (external) {
		status = 3;
	}

	memset(&b, 0, sizeof(b));
	b.size = sizeof(b);
	b.percent_hundredths = percent * 100;
	b.voltage_uv = uv;
	b.current_ua = -ua;             /* the gauge counts a discharge as positive, this structure a charge */
	b.present = present;
	b.external = external;
	b.status = status;
	hakios_battery_report(&b);
	if (say) {
		printf("battery: PM8150B fg-gen4: %d%% (raw %#x, shadow %#x), %d mV, %d mA, battery %d, charger %d, status %d\n",
		    percent, soc[0], soc[1], uv / 1000, ua / 1000, present, external, status);
	}
}

static void
battery_gauge_thread(__unused void *parameter, __unused wait_result_t result)
{
	batt_core = ml_io_map(BATT_ARB_CORE_PHYS, BATT_ARB_CORE_SIZE);
	batt_observer = ml_io_map(BATT_ARB_OBSERVER_PHYS, BATT_ARB_OBSERVER_SIZE);
	if (batt_core == 0 || batt_observer == 0) {
		printf("battery: the PMIC arbiter could not be mapped (%#lx, %#lx): no battery\n", (unsigned long)batt_core,
		    (unsigned long)batt_observer);
		thread_terminate(current_thread());
		return;
	}
	printf("battery: PMIC arbiter version %#x\n", batt_rd(batt_core));
	if (!batt_channel(FG_SOC_ID, &fg_soc_channel) || !batt_channel(FG_INFO_ID, &fg_info_channel)) {
		printf("battery: no channel of the arbiter is the PM8150B's fuel gauge (%#x, %#x): no battery\n", FG_SOC_ID,
		    FG_INFO_ID);
		thread_terminate(current_thread());
		return;
	}
	if (!batt_channel(SMB_CHGR_ID, &smb_chgr_channel)) {
		smb_chgr_channel = 0xffff;
	}
	if (!batt_channel(SMB_USB_ID, &smb_usb_channel)) {
		smb_usb_channel = 0xffff;
	}
	printf("battery: PM8150B fuel gauge on channels %u (soc) and %u (info), charger on %d and %d; polling every 5 seconds\n",
	    fg_soc_channel, fg_info_channel, smb_chgr_channel == 0xffff ? -1 : smb_chgr_channel,
	    smb_usb_channel == 0xffff ? -1 : smb_usb_channel);
	for (unsigned tick = 0;; tick++) {
		battery_gauge_poll(tick % 12 == 0);     /* a line a minute */
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}


void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	/* This phone's gauge is read here, over SPMI: on unless battery=0 says otherwise. */
	if (PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) && !enabled) return;
	if (kernel_thread_start(battery_gauge_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
	return;
}
