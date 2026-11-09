/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
 */
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/device_tree.h>

/*
 * (redmi-k40) boot_args holds 607 characters of the kernel's arguments (BOOT_LINE_LENGTH). With the loader's own in
 * front, the daily ones and a test's went past that and the last were lost: on 2026-11-09 wifi_mac came without its
 * last digit, iOS took another address and nothing on the network found it. A longer line comes whole in
 * /chosen "alioth-boot-args" (devices/redmi-k40/loader/loader.c, kernel_line), and that is the line parsed here and
 * said by kern.bootargs (bsd/kern/kern_sysctl.c, xnu-device.patch) from the first look after the device tree is set
 * up -- the first parses in PE_init_platform come before that and see boot_args' line, which holds the first
 * arguments, the loader's own among them. Without the property (a shorter line, an older loader) nothing changes.
 */
static char alioth_boot_line[1024];
static int alioth_boot_line_looked;

char *
PE_boot_args(
	void)
{
	if (!alioth_boot_line_looked) {
		DTEntry chosen;
		void const *line;
		unsigned int size;

		/* (SecureDTLookupEntry fails until SecureDTInit: looked at again next time) */
		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess) {
			alioth_boot_line_looked = 1;
			if (SecureDTGetProperty(chosen, "alioth-boot-args", &line, &size) == kSuccess && size > 1) {
				if (size > sizeof(alioth_boot_line)) {
					size = sizeof(alioth_boot_line);
				}
				memcpy(alioth_boot_line, line, size);
				alioth_boot_line[size - 1] = '\0';
			}
		}
	}
	if (alioth_boot_line[0] != '\0') {
		return alioth_boot_line;
	}
	return (char *)((boot_args *)PE_state.bootArgs)->CommandLine;
}
