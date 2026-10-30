/*
 * usb=3: what an iPhone's kernel has for its USB mux, AppleUSBDeviceMux, as lockdownd sees it -- a service of
 * that class, whose clients are told when a computer's usbmux is there (message 0xe3ff8201, argument 1) and when
 * it has gone (argument 0). lockdownd matches the class (IOServiceMatching("AppleUSBDeviceMux")), asks for its
 * general interest, and in mux_notification_handler takes the message for "a USB host is connected"
 * (usb_host_connected: the flag without which it shows no "Trust This Computer" and refuses to pair,
 * "USB or proxy host no longer connected"). The mux itself is /redwood/bin/usbmux and bsd/dev/hakios.usb.c;
 * this is only its face in the registry.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <sys/errno.h>

#define kMuxHostMessage 0xe3ff8201u     /* (lockdownd 20F66, mux_notification_handler) */

class AppleUSBDeviceMux : public IOService
{
	OSDeclareDefaultStructors(AppleUSBDeviceMux);
};

OSDefineMetaClassAndStructors(AppleUSBDeviceMux, IOService);

static AppleUSBDeviceMux *gMux;
static bool gMuxHost;

extern "C" int hakios_usbmux_publish(void);
extern "C" void hakios_usbmux_host(bool connected);

/* The service, once (when the port becomes an iPhone's). */
extern "C" int
hakios_usbmux_publish(void)
{
	IOService *provider = IOService::getPlatform();
	AppleUSBDeviceMux *mux;

	if (gMux != NULL) {
		return 0;
	}
	mux = new AppleUSBDeviceMux;
	if (provider == NULL || mux == NULL || !mux->init()) {
		if (mux != NULL) {
			mux->release();
		}
		return ENOMEM;
	}
	mux->setName("AppleUSBDeviceMux");
	mux->setProperty("HakiosUSBMux", true);
	if (!mux->attach(provider)) {
		mux->release();
		return EIO;
	}
	if (!mux->start(provider)) {
		mux->detach(provider);
		mux->release();
		return EIO;
	}
	mux->registerService();
	gMux = mux;
	IOLog("usb: AppleUSBDeviceMux published, for lockdownd\n");
	return 0;
}

/* A computer's usbmux has come (or gone): its clients told; said again for each, the same state included. */
extern "C" void
hakios_usbmux_host(bool connected)
{
	if (gMux == NULL) {
		return;
	}
	if (connected != gMuxHost) {
		IOLog("usb: AppleUSBDeviceMux: the computer's usbmux has %s\n", connected ? "come" : "gone");
	}
	gMuxHost = connected;
	gMux->setProperty("HakiosHostConnected", connected);
	gMux->messageClients(kMuxHostMessage, (void *)(uintptr_t)(connected ? 1 : 0));
}
