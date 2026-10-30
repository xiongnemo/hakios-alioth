/*
 * slpi=1: the light sensor in the registry, where MobileGestalt looks for one. Its answer "ambient-light-sensor"
 * (what CoreBrightness asks before it turns auto-brightness on, and what Settings' Auto-Brightness switch goes by)
 * is IOServiceGetMatchingService({ IOPropertyMatch = { PrimaryUsagePage = 0xff00, PrimaryUsage = 4 } }) != 0
 * (work/research/sensors-ios.md). The readings themselves reach backboardd through a HID service of its own
 * (inject.c, from sysctl kern.hakios.als_millilux, bsd/dev/hakios.slpi.c); this nub is only the registry's
 * word that the phone has the sensor. It is a plain IOService, not an IOHIDEventService, so the HID event system
 * does not take it for a second light sensor. Published early -- the question is asked while the system starts,
 * before the sensor DSP is up -- and only when slpi=1 is among the boot-args.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <sys/errno.h>

static IOService *gALS;

extern "C" int hakios_als_publish(void);

extern "C" int
hakios_als_publish(void)
{
	IOService *provider = IOService::getPlatform();
	IOService *nub;

	if (gALS != NULL) {
		return 0;
	}
	nub = new IOService;
	if (provider == NULL || nub == NULL || !nub->init()) {
		if (nub != NULL) {
			nub->release();
		}
		return ENOMEM;
	}
	nub->setName("HakiosALS");
	nub->setProperty("PrimaryUsagePage", 0xff00, 32);
	nub->setProperty("PrimaryUsage", 4, 32);
	nub->setProperty("Built-In", true);
	nub->setProperty("Product", "HakiOS ambient light (SLPI, TCS3701)");
	if (!nub->attach(provider)) {
		nub->release();
		return EIO;
	}
	if (!nub->start(provider)) {
		nub->detach(provider);
		nub->release();
		return EIO;
	}
	nub->registerService();
	gALS = nub;
	nub->release();         /* (the registry holds it) */
	IOLog("slpi: the light sensor is in the registry (HakiosALS, 0xff00/4) for MobileGestalt\n");
	return 0;
}
