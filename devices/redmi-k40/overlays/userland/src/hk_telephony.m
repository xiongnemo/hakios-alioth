/*
 * hk_telephony: CoreTelephony's XPC service (com.apple.commcenter.coretelephony.xpc) answered in CommCenter's place.
 *
 * Apple's CommCenter cannot run on this phone (userland/mkjobs.py: it dies on AppleBasebandPCI and AppleIPAppender,
 * an Apple modem's drivers), and the Redmi K40's modem is a Qualcomm SDX55 driven from the kernel
 * (bsd/dev/alioth_mdm.c). With the device tree's "baseband" node (loader dt_baseband=1) iOS believes in a modem --
 * Settings has Cellular, About has an IMEI row -- and every CoreTelephony client then talks to this service. Nothing
 * answering it, clients waited (Flutter Check went black, 2026-11-14).
 *
 * Here every request of CoreTelephony's own protocol (CTXPCServiceInterface, the client's) is answered at once from
 * its reply block's signature: an NSError where one is expected (ENXIO: no such service), nil and zeros otherwise --
 * "no SIM, no network", which is the truth. One thing more: the mobile equipment information (CTMobileEquipmentInfo:
 * the IMEI), from the kernel (root-only sysctl kern.hakios.mdm_imei: what the modem's DMS gave), for a client that
 * holds CommCenter's "identity" entitlement (Settings does) and no other -- as CommCenter itself does.
 *
 * NOT DONE, ON PURPOSE: nothing about activation, the SIM, a network or a carrier is made up. The IMEI is never
 * written anywhere: /var/tmp/hk_telephony.log has which requests came and from whom, and whether the IMEI was given.
 * The job: com.hakios.telephony (MachServices: the service name). Root (the sysctl).
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

extern const char *_Block_signature(void *block);
extern int proc_name(int pid, void *buffer, uint32_t size);

/* (in Foundation on iOS too, only not in the SDK's headers: what system daemons serve their services with) */
@interface NSXPCConnection (HKPrivate)
- (id)valueForEntitlement:(NSString *)entitlement;
@end

static NSXPCListener *
mach_listener(NSString *name)
{
	SEL init = sel_registerName("initWithMachServiceName:");
	NSXPCListener *l = [NSXPCListener alloc];

	return [l respondsToSelector:init] ? ((NSXPCListener * (*)(id, SEL, NSString *))objc_msgSend)(l, init, name) : nil;
}

static Protocol *service;
static NSMutableDictionary<NSString *, NSNumber *> *asked;
static unsigned identities_given, refused_identity, slot_given;

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[512];
	va_list arguments;
	FILE *f;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if ((f = fopen("/var/tmp/hk_telephony.log", "a")) != NULL) {
		time_t now = time(NULL);
		struct tm tm;
		char when[16];

		strftime(when, sizeof(when), "%H:%M:%S", localtime_r(&now, &tm));
		fprintf(f, "%s %s", when, line);
		fclose(f);
	}
}

/*
 * The phone's one SIM slot, EMPTY -- what CommCenter says of an iPhone with no SIM in it (round 42: with no
 * subscription at all, About has no SIM section and so no IMEI row, though the IMEI was given). Slot 1, SIM not
 * present, not good; the preferred one for data and voice as the only one. The UUID is a constant of ours, nothing of
 * the phone's. Boot-arg tel_noslot=1: none (round 42's behaviour).
 */
static BOOL no_slot;

static id
slot_context(void)
{
	static id context;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class c = NSClassFromString(@"CTXPCServiceSubscriptionContext");
		NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:@"6B40A11D-0000-4000-8000-000000000001"];
		SEL init = sel_registerName("initWithUUID:andSlot:");

		if (c == Nil || ![c instancesRespondToSelector:init]) {
			return;
		}
		context = ((id (*)(id, SEL, id, long long))objc_msgSend)([c alloc], init, uuid, 1);
		((void (*)(id, SEL, BOOL))objc_msgSend)(context, sel_registerName("setIsSimPresent:"), NO);
		((void (*)(id, SEL, BOOL))objc_msgSend)(context, sel_registerName("setIsSimGood:"), NO);
		((void (*)(id, SEL, id))objc_msgSend)(context, sel_registerName("setUserDataPreferred:"), @YES);
		((void (*)(id, SEL, id))objc_msgSend)(context, sel_registerName("setUserDefaultVoice:"), @YES);
	});
	return context;
}

static id
slot_info(void)
{
	static id info;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		id context = slot_context();

		info = context ? [NSClassFromString(@"CTXPCServiceSubscriptionInfo") new] : nil;
		((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setSubscriptions:"), @[ context ]);
		((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setSubscriptionsInUse:"), @[ context ]);
		((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setSubscriptionsValid:"), @[ context ]);
	});
	return info;
}

/*
 * (round 43: About showed the IMEI; the Cellular pane stayed empty -- Settings asked getActiveContexts:, answered
 * nil.) The active contexts: the one slot as a context too (CTXPCContextInfo: our UUID, no account, slot 1), the
 * preferred one for data and voice. And getSIMStatus:'s answer is iOS's own kCTSIMSupportSIMStatusNotInserted.
 */
static id
slot_contexts(void)
{
	static id contexts;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class ci = NSClassFromString(@"CTXPCContextInfo"), cs = NSClassFromString(@"CTXPCContexts");
		SEL init = sel_registerName("initWithUUID:andAccountID:andSlot:");
		id context = slot_context();
		NSUUID *uuid = context ? ((NSUUID * (*)(id, SEL))objc_msgSend)(context, sel_registerName("uuid")) : nil;
		id info;

		if (ci == Nil || cs == Nil || uuid == nil || ![ci instancesRespondToSelector:init]) {
			return;
		}
		info = ((id (*)(id, SEL, id, id, long long))objc_msgSend)([ci alloc], init, uuid, @"", 1);
		contexts = [cs new];
		((void (*)(id, SEL, id))objc_msgSend)(contexts, sel_registerName("setSubscriptions:"), info ? @[ info ] : @[]);
		((void (*)(id, SEL, id))objc_msgSend)(contexts, sel_registerName("setDataPreferred:"), uuid);
		((void (*)(id, SEL, id))objc_msgSend)(contexts, sel_registerName("setVoicePreferred:"), uuid);
	});
	return contexts;
}

/* the IMEI the modem gave the kernel, or nil (no modem session, not read yet) */
static NSString *
modem_imei(void)
{
	char imei[32] = "";
	size_t n = sizeof(imei) - 1;

	if (sysctlbyname("kern.hakios.mdm_imei", imei, &n, NULL, 0) != 0 || n != 15) {
		return nil;
	}
	imei[n] = 0;
	return [NSString stringWithUTF8String:imei];
}

static NSString *
class_named(const char *type)
{
	/* an extended type: @"ClassName" (or @"ClassName<Protocol>") */
	const char *a = strchr(type, '"'), *b = a ? strpbrk(a + 1, "\"<") : NULL;

	return a && b ? [[NSString alloc] initWithBytes:a + 1 length:(NSUInteger)(b - a - 1) encoding:NSUTF8StringEncoding] : nil;
}

@interface HKAnswer : NSObject
@property (nonatomic) BOOL identity;    /* the client may have the IMEI */
@property (nonatomic, copy) NSString *client;
@end

@implementation HKAnswer

static const char *
types_for(SEL selector)
{
	struct objc_method_description d = protocol_getMethodDescription(service, selector, YES, YES);

	if (d.types == NULL) {
		d = protocol_getMethodDescription(service, selector, NO, YES);
	}
	return d.types;
}

- (BOOL)respondsToSelector:(SEL)selector
{
	return [super respondsToSelector:selector] || types_for(selector) != NULL;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	NSMethodSignature *s = [super methodSignatureForSelector:selector];
	const char *types;

	if (s != nil) {
		return s;
	}
	types = types_for(selector);
	return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

/* the equipment information: one entry (slot 1) with the IMEI */
- (id)equipmentInfo:(NSString *)imei
{
	id info = [NSClassFromString(@"CTMobileEquipmentInfo") new];

	if (info == nil) {
		return nil;
	}
	((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setIMEI:"), imei);
	if ([info respondsToSelector:sel_registerName("setSlotId:")]) {
		((void (*)(id, SEL, long long))objc_msgSend)(info, sel_registerName("setSlotId:"), 1);
	}
	return info;
}

/*
 * The class names of the arguments of the `which`-th block of a method, from the protocol's EXTENDED type encoding
 * (round 40: a reply block's own signature has a bare "@" for each object, so the equipment list was never
 * recognised): ... @?<v@?@"CTMobileEquipmentInfoList"@"NSError"> ... -> [CTMobileEquipmentInfoList, NSError];
 * "" for an argument that is not a named object.
 */
extern const char *_protocol_getMethodTypeEncoding(Protocol *p, SEL selector, BOOL required, BOOL instance);

static NSArray<NSString *> *
block_classes(SEL selector, unsigned which, const char **whole)
{
	const char *ext = _protocol_getMethodTypeEncoding(service, selector, YES, YES), *p;
	NSMutableArray<NSString *> *out = [NSMutableArray array];
	int arg = -1;

	if (ext == NULL) {
		ext = _protocol_getMethodTypeEncoding(service, selector, NO, YES);
	}
	*whole = ext;
	if (ext == NULL) {
		return nil;
	}
	p = ext;
	for (unsigned k = 0;; k++) {
		p = strstr(p, "@?<");
		if (p == NULL) {
			return nil;
		}
		if (k == which) {
			break;
		}
		p += 3;
	}
	p += 3;
	while (*p != 0 && *p != '>') {
		const char *q = p;
		NSString *cls = @"";

		while (*q != 0 && strchr("rnNoORVA", *q) != NULL) {
			q++;
		}
		if (q[0] == '@' && q[1] == '"') {
			const char *e = strchr(q + 2, '"'), *lt;

			if (e == NULL) {
				break;
			}
			lt = memchr(q + 2, '<', (size_t)(e - q - 2));
			cls = [[NSString alloc] initWithBytes:q + 2 length:(NSUInteger)((lt ? lt : e) - q - 2)
			    encoding:NSUTF8StringEncoding] ?: @"";
			p = e + 1;
		} else if (q[0] == '@' && q[1] == '?') {
			p = q + 2;
			if (*p == '<') {
				int depth = 1;

				for (p++; *p != 0 && depth > 0; p++) {
					depth += *p == '<' ? 1 : *p == '>' ? -1 : 0;
				}
			}
		} else if (*q == '{' || *q == '(' || *q == '[') {
			char open = *q, close = open == '{' ? '}' : open == '(' ? ')' : ']';
			int depth = 0;

			p = q;
			do {
				depth += *p == open ? 1 : *p == close ? -1 : 0;
				p++;
			} while (*p != 0 && depth > 0);
		} else if (*q == '^') {
			p = q + 1;
			if (p[0] == '@' && p[1] == '"') {
				const char *e = strchr(p + 2, '"');

				p = e ? e + 1 : p + 1;
			} else if (*p != 0) {
				p++;
			}
		} else {
			p = *q ? q + 1 : q;
		}
		while (*p >= '0' && *p <= '9') {
			p++;
		}
		if (arg >= 1) {
			[out addObject:cls];
		}
		arg++;
	}
	return out;
}

/* a reply block called with what its signature asks for */
- (void)answer:(id)block for:(NSString *)name which:(unsigned)which
{
	const char *whole = NULL;
	NSArray<NSString *> *classes = block_classes(NSSelectorFromString(name), which, &whole);
	const char *signature = _Block_signature((__bridge void *)block);
	NSMethodSignature *s;
	NSInvocation *call;
	BOOL gave = NO, gave_slot = NO;
	NSString *imei = nil;
	NSError *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENXIO userInfo:nil];

	if (signature == NULL || (s = [NSMethodSignature signatureWithObjCTypes:signature]) == nil) {
		return;
	}
	call = [NSInvocation invocationWithMethodSignature:s];
	call.target = block;
	/* (the arguments start zeroed; objects are set where a class is named) */
	for (NSUInteger i = 1; i < s.numberOfArguments; i++) {
		const char *type = [s getArgumentTypeAtIndex:i];
		NSString *cls = type[0] != '@' ? nil : i - 1 < classes.count && classes[i - 1].length ? classes[i - 1] :
		    class_named(type);
		__unsafe_unretained id value = nil;
		id keep = nil;          /* (strong, to the end of this argument: round 41 crashed on a list freed too soon) */

		if ([cls isEqualToString:@"CTMobileEquipmentInfoList"] || [cls isEqualToString:@"CTMobileEquipmentInfo"]) {
			if (!self.identity) {
				refused_identity++;
				continue;
			}
			imei = imei ?: modem_imei();
			if (imei == nil) {
				say("hk_telephony: %s: %s asks the IMEI, and the kernel has none (yet)\n", name.UTF8String,
				    self.client.UTF8String);
				continue;
			}
			id info = [self equipmentInfo:imei];

			if ([cls isEqualToString:@"CTMobileEquipmentInfoList"]) {
				id list = [NSClassFromString(@"CTMobileEquipmentInfoList") new];

				((void (*)(id, SEL, id))objc_msgSend)(list, sel_registerName("setMeInfoList:"), info ? @[ info ] : @[]);
				keep = list;
			} else {
				keep = info;
			}
			value = keep;
			[call retainArguments];
			gave = value != nil;
			[call setArgument:&value atIndex:(NSInteger)i];
		} else if (!no_slot && ([cls isEqualToString:@"CTXPCServiceSubscriptionContext"] ||
		    [cls isEqualToString:@"CTXPCServiceSubscriptionInfo"] || [cls isEqualToString:@"CTXPCContexts"] ||
		    ([cls isEqualToString:@"NSString"] && [name hasPrefix:@"getSIMStatus:"]))) {
			keep = [cls isEqualToString:@"CTXPCServiceSubscriptionInfo"] ? slot_info() :
			    [cls isEqualToString:@"CTXPCContexts"] ? slot_contexts() :
			    [cls isEqualToString:@"NSString"] ? @"kCTSIMSupportSIMStatusNotInserted" : slot_context();
			value = keep;
			[call retainArguments];
			[call setArgument:&value atIndex:(NSInteger)i];
			slot_given += keep != nil;
			gave_slot = keep != nil;
		} else if ([cls isEqualToString:@"NSError"] && !gave && !gave_slot) {
			value = error;
			[call setArgument:&value atIndex:(NSInteger)i];
		}
	}
	[call retainArguments];
	[call invoke];
	if ([name containsString:@"MobileEquipment"]) {
		static unsigned told;

		if (told++ < 6) {
			say("hk_telephony: %s from %s: extended type %s; the reply's classes %s; IMEI %s\n", name.UTF8String,
			    self.client.UTF8String, whole ?: "(none)", [classes componentsJoinedByString:@","].UTF8String ?: "(none)",
			    gave ? "GIVEN" : self.identity ? "not given" : "not for this client");
		}
	}
	if (gave_slot && slot_given <= 15) {
		say("hk_telephony: %s: the empty SIM slot given to %s\n", name.UTF8String, self.client.UTF8String);
	}
	if (gave) {
		identities_given++;
		say("hk_telephony: %s: the IMEI given to %s (%u times)\n", name.UTF8String, self.client.UTF8String, identities_given);
	}
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	NSMethodSignature *s = invocation.methodSignature;
	NSString *name = NSStringFromSelector(invocation.selector);
	NSString *key = [NSString stringWithFormat:@"%@ %@", self.client, name];

	@synchronized(asked) {
		unsigned n = asked[key].unsignedIntValue + 1;

		asked[key] = @(n);
		if (n == 1 && asked.count <= 400) {
			say("hk_telephony: %s asks %s\n", self.client.UTF8String, name.UTF8String);
		}
	}
	for (NSUInteger i = 2, blocks = 0; i < s.numberOfArguments; i++) {
		if (strncmp([s getArgumentTypeAtIndex:i], "@?", 2) == 0) {
			__unsafe_unretained id block = nil;

			[invocation getArgument:&block atIndex:(NSInteger)i];
			if (block != nil) {
				[self answer:block for:name which:(unsigned)blocks];
			}
			blocks++;
		}
	}
}

@end

@interface HKListener : NSObject <NSXPCListenerDelegate>
@end

@implementation HKListener

static BOOL
has_identity(NSXPCConnection *c)
{
	id fine = [c valueForEntitlement:@"com.apple.CommCenter.fine-grained"];
	id get = [c valueForEntitlement:@"com.apple.coretelephony.Identity.get"];

	return ([fine isKindOfClass:NSArray.class] && [fine containsObject:@"identity"]) ||
	    ([get isKindOfClass:NSNumber.class] && [get boolValue]);
}

static NSString *
name_of(pid_t pid)
{
	char name[256] = "";

	if (proc_name(pid, name, sizeof(name)) > 0) {
		return [NSString stringWithUTF8String:name] ?: @"?";
	}
	return [NSString stringWithFormat:@"pid %d", pid];
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)c
{
	static unsigned connections;
	HKAnswer *answer = [HKAnswer new];

	(void)listener;
	answer.client = name_of(c.processIdentifier);
	answer.identity = has_identity(c);
	c.exportedInterface = [NSXPCInterface interfaceWithProtocol:service];
	c.exportedObject = answer;
	[c resume];
	if (++connections <= 60) {
		say("hk_telephony: connection %u from %s%s\n", connections, answer.client.UTF8String,
		    answer.identity ? " (may have the IMEI)" : "");
	}
	return YES;
}

@end

/*
 * (2026-11-14, round 58) The OLDER service, com.apple.commcenter.xpc -- CTServerConnection's C XPC: SpringBoard's main
 * thread asks it once a second for the whole session ("CTServerConnection returned error: (2, 5). Destroying
 * connection", _updateLocale) and Settings' Cellular pane asks it too; nothing answered (369 failed lookups a session).
 * Here it is answered: every request gets an EMPTY reply (no value: nothing made up), and the requests' key names,
 * value types and integer values are logged (never strings: the protocol, not anyone's data), to learn what is asked.
 * OPT-IN, boot-arg tel_old=1: round 59 with it served (empty replies) never got iOS's picture on the screen -- the\n * loader's stayed, while the kernel, the modem and the daemons ran: something on the display's way up waits for\n * a real answer. Off unless asked. (No xpc/xpc.h in this SDK: declared as umi_bluetool.c does.)
 */
typedef void *hk_xpc_t;
extern const char _xpc_type_dictionary, _xpc_type_connection, _xpc_type_int64, _xpc_type_uint64, _xpc_type_string,
    _xpc_type_bool, _xpc_type_data, _xpc_type_array;
extern const void *xpc_get_type(hk_xpc_t);
extern hk_xpc_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(hk_xpc_t, void (^)(hk_xpc_t));
extern void xpc_connection_resume(hk_xpc_t);
extern void xpc_connection_send_message(hk_xpc_t, hk_xpc_t);
extern pid_t xpc_connection_get_pid(hk_xpc_t);
extern hk_xpc_t xpc_dictionary_create_reply(hk_xpc_t);
extern bool xpc_dictionary_apply(hk_xpc_t, bool (^)(const char *, hk_xpc_t));
extern int64_t xpc_int64_get_value(hk_xpc_t);
extern uint64_t xpc_uint64_get_value(hk_xpc_t);
extern void xpc_release(hk_xpc_t);
extern void *xpc_retain(hk_xpc_t);
extern void xpc_connection_cancel(hk_xpc_t);
extern const char *xpc_dictionary_get_string(hk_xpc_t, const char *);
extern hk_xpc_t xpc_dictionary_get_value(hk_xpc_t, const char *);
extern void xpc_dictionary_set_bool(hk_xpc_t, const char *, bool);

static unsigned old_requests, old_said;

static void
old_request(hk_xpc_t peer, hk_xpc_t message)
{
	hk_xpc_t reply;

	if (xpc_get_type(message) != &_xpc_type_dictionary) {
		return;
	}
	old_requests++;
	if (old_said < 150) {
		char who[64] = "?", keys[400] = "";
		__block size_t used = 0;
		__block char *k = keys;

		proc_name(xpc_connection_get_pid(peer), who, sizeof(who));
		{
			/* (the request's NAME -- a protocol identifier, not anyone's data -- is the one string said) */
			const char *request = xpc_dictionary_get_string(message, "kRequest");

			if (request != NULL) {
				used += (size_t)snprintf(k, sizeof(keys), "kRequest=%s ", request);
			}
		}
		xpc_dictionary_apply(message, ^bool(const char *key, hk_xpc_t value) {
			const void *t = xpc_get_type(value);
			char one[96];

			if (t == &_xpc_type_int64) {
				snprintf(one, sizeof(one), "%s=%lld ", key, (long long)xpc_int64_get_value(value));
			} else if (t == &_xpc_type_uint64) {
				snprintf(one, sizeof(one), "%s=%llu ", key, (unsigned long long)xpc_uint64_get_value(value));
			} else {
				snprintf(one, sizeof(one), "%s:%s ", key, t == &_xpc_type_string ? "string" : t == &_xpc_type_bool ? "bool" :
				    t == &_xpc_type_data ? "data" : t == &_xpc_type_dictionary ? "dict" : t == &_xpc_type_array ? "array" : "other");
			}
			used += strlcpy(k + used, one, sizeof(keys) - used);
			return used < sizeof(keys) - 1;
		});
		old_said++;
		say("hk_telephony: commcenter.xpc request %u from %s: %s\n", old_requests, who, keys);
	}
	reply = xpc_dictionary_create_reply(message);
	if (reply != NULL) {
		const char *request = xpc_dictionary_get_string(message, "kRequest");

		/*
		 * (round 68: opening the Cellular pane, Settings asked kRadioModuleGetProperty for kRadioModuleIsDead and the pane
		 * stayed empty on an empty reply) The radio module -- the SDX55 -- is alive: kRadioModuleIsDead = false, the
		 * property under its own key. The one real answer so far; everything else stays empty.
		 */
		if (request != NULL && strcmp(request, "kRadioModuleGetProperty") == 0 &&
		    xpc_dictionary_get_value(message, "kRadioModuleIsDead") != NULL) {
			xpc_dictionary_set_bool(reply, "kRadioModuleIsDead", false);
		}
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	}
}

static void
old_service(void)
{
	hk_xpc_t listener = xpc_connection_create_mach_service("com.apple.commcenter.xpc", dispatch_get_main_queue(), 1);

	if (listener == NULL) {
		say("hk_telephony: com.apple.commcenter.xpc could not be served\n");
		return;
	}
	xpc_connection_set_event_handler(listener, ^(hk_xpc_t peer) {
		char who[64] = "";

		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		/*
		 * (round 59: empty replies to EVERY client kept iOS's picture off the screen) Settings alone is answered, to learn
		 * what its Cellular pane asks; anyone else's connection is cancelled at once -- an immediate error, as close to
		 * "no such service" as a registered name allows.
		 */
		proc_name(xpc_connection_get_pid(peer), who, sizeof(who));
		if (strcmp(who, "Preferences") != 0) {
			xpc_connection_set_event_handler(peer, ^(hk_xpc_t event) {
				(void)event;
			});
			xpc_connection_resume(peer);
			xpc_connection_cancel(peer);
			return;
		}
		xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(hk_xpc_t message) {
			old_request(peer, message);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	say("hk_telephony: com.apple.commcenter.xpc answered too (empty replies)\n");
}

/* the instance methods of a protocol and of those it takes in */
static unsigned
methods_in(Protocol *p)
{
	unsigned count = 0, n = 0, total = 0;
	struct objc_method_description *m;
	Protocol *__unsafe_unretained *inner = protocol_copyProtocolList(p, &n);

	for (int required = 0; required < 2; required++) {
		m = protocol_copyMethodDescriptionList(p, required, YES, &count);
		free(m);
		total += count;
	}
	for (unsigned i = 0; i < n; i++) {
		total += methods_in(inner[i]);
	}
	free(inner);
	return total;
}

int
main(void)
{
	@autoreleasepool {
		NSXPCListener *listener;
		HKListener *delegate = [HKListener new];

		(void)[NSClassFromString(@"CoreTelephonyClient") class];      /* (CoreTelephony loaded: its protocols known) */
		service = NSProtocolFromString(@"CTXPCServiceInterface");
		if (service == nil) {
			say("hk_telephony: no CTXPCServiceInterface in CoreTelephony: nothing to serve\n");
			return 1;
		}
		asked = [NSMutableDictionary new];
		{
			char args[1024] = "";
			size_t n = sizeof(args) - 1;

			no_slot = sysctlbyname("kern.bootargs", args, &n, NULL, 0) == 0 && strstr(args, "tel_noslot=1") != NULL;
			if (strstr(args, "tel_old=1") != NULL) {
				old_service();
			}
		}
		say("hk_telephony: the SIM slot: %s\n", no_slot ? "none said (tel_noslot=1)" : "one, slot 1, EMPTY (no SIM)");
		listener = mach_listener(@"com.apple.commcenter.coretelephony.xpc");
		if (listener == nil) {
			say("hk_telephony: NSXPCListener has no initWithMachServiceName: here\n");
			return 1;
		}
		listener.delegate = delegate;
		[listener resume];
		say("hk_telephony: answering com.apple.commcenter.coretelephony.xpc (%u methods in CTXPCServiceInterface and the "
		    "protocols it takes in); the modem's IMEI %s\n", methods_in(service), modem_imei() ? "is there" : "is not there yet");
		[NSRunLoop.mainRunLoop run];
	}
	return 0;
}
