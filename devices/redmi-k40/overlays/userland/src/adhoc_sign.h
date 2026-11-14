/*
 * adhoc_sign: an ad hoc code signature for an arm64 Mach-O, made here (ipainstall signs what it installs, 2026-11-15).
 *
 * Why: this port's kernel runs code without checking signatures (miniamfi), but an application's ENTITLEMENTS live in
 * its signature -- and an unsigned IPA (a GitHub build) or one decrypted from the App Store (its signature no longer
 * matches the pages) has none the kernel will read. Without them containermanagerd gives no app group container
 * (Telegram clients: a white window), CloudKit traps, and dyld's map_with_linking_np refuses unsigned frameworks.
 *
 * Plain C, no other library (its own SHA-256): the same file builds for the phone and, for tests, on the computer.
 */
#ifndef ADHOC_SIGN_H
#define ADHOC_SIGN_H

#include <stddef.h>

/* 1 if PATH is a Mach-O (thin or fat) file, 0 if not */
int adhoc_is_macho(const char *path);

/* 1 if PATH's arm64 code is still encrypted (an App Store binary not decrypted: it cannot run anyway) */
int adhoc_is_encrypted(const char *path);

/* the XML entitlements of PATH's existing signature (its arm64 slice), malloc'd with *LEN, or NULL when none */
char *adhoc_entitlements(const char *path, size_t *len);

/*
 * Sign PATH in place: its arm64 slice only (a fat file becomes thin), a CodeDirectory of SHA-256 page hashes, ad hoc,
 * with IDENTIFIER; with ENTITLEMENTS (an XML plist of LEN bytes, written as XML and as DER) or none (NULL). An existing
 * signature is replaced. 0 when done; else -1 and a reason in ERR.
 */
int adhoc_sign(const char *path, const char *identifier, const char *entitlements, size_t len, char *err, size_t errlen);

/* the DER form of an XML entitlements plist (as CoreEntitlements and ldid write it), malloc'd; 0 or -1 and ERR */
int adhoc_der(const char *xml, size_t len, unsigned char **der, size_t *der_len, char *err, size_t errlen);

#endif
