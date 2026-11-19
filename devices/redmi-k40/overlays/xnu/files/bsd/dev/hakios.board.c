/*
 * The board's addresses (sys/hakios.board.h): board_<block>=0x... on the command line, else the address on
 * the phone the port was brought up on. Read where a driver maps its block, so that one kernel serves every
 * phone whose SoC has the same blocks, wherever its device tree puts them.
 *
 * The names, and the drivers that ask for them (the default is the POCO X5 Pro's, SM7325):
 *   epss       cpufreq: the CPU frequency domains (qcom,cpufreq-epss "freq-domain0")      0x18591000
 *   cmd_db     cpufreq: RPMh's command database                                         0x80860000
 *   rsc        cpufreq: the apps RSC (RPMh votes)                                       0x18220000
 *   tsens0/1   thermal: the TM regions of the two TSENS blocks                         0x0c263000, 0x0c265000
 *   gcc        display: the global clock controller                                     0x00100000
 *   dispcc     display: the display clock controller                                    0x0af00000
 *   intf       display: the DSI panel's timing engine (mdp_intf_base)                   0x0ae36000
 *   ufshc      ufs: the UFS host controller ("ufs_mem")                                 0x01d84000
 *   spmi_core, spmi_cnfg, spmi_obsrvr   buttons: the PMIC arbiter                       0x0c440000 ...
 * (2026-11-20, issue #1) reserved-memory regions that move with XBL's removed_region from one firmware to the next,
 * each with a _size (board_pil_gpu_size=...); the kit takes them from the phone's own device tree (tools/dt-regions.py):
 *   pil_gpu    adreno: the zap shader (pil_gpu_region)                                  0x8681a000, 0x2000
 *   pil_slpi   slpi: the sensor DSP (pil_slpi_region)                                   0x88c00000, 0x2f00000
 *   pil_adsp   adsp: the audio DSP (pil_adsp_region)                                    0x8bb00000, 0x2500000
 */
#include <libkern/libkern.h>
#include <kern/startup.h>
#include <pexpert/pexpert.h>
#include <sys/code_signing.h>
#include <sys/hakios.board.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <pexpert/device_tree.h>

vm_offset_t
hakios_board_addr(const char *name, vm_offset_t dflt)
{
	char arg[40] = "board_";
	uint64_t value = 0;

	strlcat(arg, name, sizeof(arg));
	if (PE_parse_boot_argn(arg, &value, sizeof(value)) && value != 0) {
		return (vm_offset_t)value;
	}
	return dflt;
}

uint32_t
hakios_board_number(const char *name, uint32_t dflt)
{
	char arg[40] = "board_";
	uint32_t value = 0;

	strlcat(arg, name, sizeof(arg));
	if (PE_parse_boot_argn(arg, &value, sizeof(value))) {
		return value;
	}
	return dflt;
}

void
hakios_board_report(void)
{
	static const char *const names[] = { "epss", "cmd_db", "rsc", "tsens0", "tsens1", "gcc", "dispcc", "intf", "ufshc",
		                             "spmi_core", "spmi_cnfg", "spmi_obsrvr", "pil_gpu", "pil_slpi", "pil_adsp" };
	int given = 0;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		vm_offset_t a = hakios_board_addr(names[i], 0);

		if (a != 0) {
			printf("board: %s at %#lx (from the command line)\n", names[i], (unsigned long)a);
			given++;
		}
	}
	if (given == 0) {
		printf("board: no board_ addresses given: the POCO X5 Pro's (SM7325) for every block\n");
	}
}

/* The kona phones this kernel knows: the Mi 10 (umi) port's drivers run unchanged on both. */
bool
hakios_board_kona_name(const void *board, unsigned size)
{
	return (size == 4 && memcmp(board, "umi", 4) == 0) || (size == 7 && memcmp(board, "alioth", 7) == 0);
}

/* The loader's /chosen hakios-board is "alioth": Redmi K40 / POCO F3. */
bool
hakios_board_is_alioth(void)
{
	DTEntry chosen;
	const void *board;
	unsigned size;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	       SecureDTGetProperty(chosen, "hakios-board", &board, &size) == kSuccess &&
	       size == 7 && memcmp(board, "alioth", 7) == 0;
}

/* Panel lines: Mi 10 1080 x 2340, Redmi K40 (K11A 38 08 0a) 1080 x 2400. */
uint32_t
hakios_board_panel_height(void)
{
	return hakios_board_is_alioth() ? 2400 : 2340;
}

/*
 * iOS's Developer Mode, on (the owner, 2026-11-15). This kernel keeps the state where the PPL build of the code
 * signing glue does (bsd/kern/code_signing/ppl.c: ppl_developer_mode_storage, false until set), and on an iPhone
 * AMFI sets it at boot; our miniamfi never did, so it stayed off -- and with it off, a task port of ANOTHER process
 * asked for with task_for_pid comes back as a dead name (osfmk/kern/ipc_tt.c, convert_task_to_port_with_flavor:
 * "denied by security policy, make the port appear dead"), task_for_pid itself saying KERN_SUCCESS: psample's
 * threads/stacks got nothing (11-15). Set once here, before anything else could: the state may go from unset to
 * either, never from off to on (pmap_toggle_developer_mode_internal panics). Boot-arg dev_mode=0 keeps it off.
 * security.mac.amfi.developer_mode_status says it to user space (libamfi's amfi_developer_mode_status()).
 */
extern void loader_log_putc(char c);

__startup_func
static void
hakios_developer_mode_init(void)
{
	uint32_t on = 1;
	const char *said;

	PE_parse_boot_argn("dev_mode", &on, sizeof(on));
	if (on) {
		enable_developer_mode();
	} else {
		disable_developer_mode();
	}
	said = developer_mode_state() ? "board: iOS's developer mode on\n" : "board: iOS's developer mode off (dev_mode=0)\n";
	printf("%s", said);
	for (; *said != 0; said++) {
		loader_log_putc(*said);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_developer_mode_init);

static int
sysctl_developer_mode_status SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int on = developer_mode_state() ? 1 : 0;

	return SYSCTL_OUT(req, &on, sizeof(on));
}
SYSCTL_DECL(_security_mac);
SYSCTL_NODE(_security_mac, OID_AUTO, amfi, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "AMFI (the port's miniamfi)");
SYSCTL_PROC(_security_mac_amfi, OID_AUTO, developer_mode_status, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_developer_mode_status, "I", "iOS's developer mode, as the kernel has it");
