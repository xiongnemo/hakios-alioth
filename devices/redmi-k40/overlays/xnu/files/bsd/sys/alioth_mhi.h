/*
 * MHI, the host's side, for the K40's SDX55 modem on PCIe2 (bsd/dev/alioth_mdm.c): polled, any channels, the
 * whole boot image through BHI. umi_mhi.c is the Wi-Fi's, shaped by the QCA6390 (its first 512 KB through BHI, the
 * rest through BHIe, IPCR only); this one follows the modem's configuration (Xiaomi's kona-mhi.dtsi, mhi_0) and
 * Linux's drivers/bus/mhi/host for the order of things. The register and ring formats are the MHI specification's,
 * as in umi_mhi.c. The PCIe owner supplies the BAR and memory the modem can reach; the host is compiled on the
 * build machine too, against a pretend SDX55 (devices/redmi-k40/tools/test-mhi.c).
 * The caller serializes calls. Everything returns 0 or an errno.
 */
#ifndef _SYS_ALIOTH_MHI_H_
#define _SYS_ALIOTH_MHI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALIOTH_MHI_EVENT_RINGS  3               /* 0 control, 1 and 2 data: kona-mhi.dtsi's first three */
#define ALIOTH_MHI_OPEN         6               /* channels open at once */
#define ALIOTH_MHI_CONTEXTS     256             /* channel contexts: the most MHICFG can count */

/* execution environments, BHI's EXECENV */
#define ALIOTH_MHI_EE_PBL       0
#define ALIOTH_MHI_EE_SBL       1
#define ALIOTH_MHI_EE_AMSS      2
#define ALIOTH_MHI_EE_RDDM      3
#define ALIOTH_MHI_EE_EDL       6

/* channel directions */
#define ALIOTH_MHI_TO_DEVICE    1
#define ALIOTH_MHI_FROM_DEVICE  2

struct alioth_mhi_dma {
	void *bytes;
	uint64_t address;                       /* as the device sees it */
	size_t size;
};

struct alioth_mhi_io {
	void *context;
	uint32_t bar_size;
	/* the device addresses allocations come from, the last one included (MHICTRLBASE/LIMIT); the SDX55 reaches
	 * 32 bits (Linux's pci_generic: dma_data_width 32), kona-mhi.dtsi gives it 0x20000000-0x3fffffff */
	uint64_t window_base, window_last;
	uint32_t (*read32)(void *, uint32_t);
	void (*write32)(void *, uint32_t, uint32_t);
	/* page aligned, zeroed, coherent (or uncached); owned until the endpoint is reset */
	int (*allocate)(void *, size_t, struct alioth_mhi_dma *);
	void (*delay_us)(void *, unsigned);
	void (*say)(void *, const char *, uint64_t, uint64_t, uint64_t);
};

/* bytes from the device on a channel; `end` when they end what it sent (MHI's EOT), not when the buffer filled */
typedef void (*alioth_mhi_receive)(void *, unsigned channel, const uint8_t *, uint32_t, bool end);

struct alioth_mhi_ring {
	struct alioth_mhi_dma dma;
	uint32_t elements;
	uint64_t rp, wp;                        /* the host's, as device addresses */
};

struct alioth_mhi_channel {
	bool open;
	uint8_t number, direction, event_ring;
	uint32_t slot;                          /* bytes of each element's buffer */
	struct alioth_mhi_ring ring;
	struct alioth_mhi_dma buffers;          /* one slot per ring element */
	alioth_mhi_receive receive;
	void *receive_context;
	uint32_t done, failed;
};

/* what the device is told about a channel before M0 (Linux's mhi_init_dev_ctxt does it for all it knows) */
struct alioth_mhi_known {
	uint8_t number, direction, event_ring;
};

struct alioth_mhi {
	const struct alioth_mhi_io *io;
	uint32_t bhi, channel_db, event_db, channels;
	uint32_t state, ee;                     /* as the device last said: MHI state, execution environment */
	bool poisoned, ready;
	uint64_t command_pending;
	int command_code;
	uint32_t events, bandwidth_requests, sequence;
	struct alioth_mhi_dma image, channel_contexts, event_contexts, command_context;
	struct alioth_mhi_ring commands, event[ALIOTH_MHI_EVENT_RINGS];
	struct alioth_mhi_channel open[ALIOTH_MHI_OPEN];
};

/* A zeroed host; reads registers only. */
int alioth_mhi_probe(struct alioth_mhi *, const struct alioth_mhi_io *);
int alioth_mhi_execution_environment(struct alioth_mhi *, uint32_t *);
/* In PBL: the whole image (the SDX55's SBL) through BHI. */
int alioth_mhi_bhi(struct alioth_mhi *, const void *, size_t, unsigned timeout_ms);
/* Once the device is READY: contexts, event rings, M0. */
int alioth_mhi_ready(struct alioth_mhi *, const struct alioth_mhi_known *, unsigned known, unsigned timeout_ms);
int alioth_mhi_open(struct alioth_mhi *, unsigned channel, unsigned direction, unsigned event_ring, unsigned elements,
    uint32_t slot, alioth_mhi_receive, void *);
int alioth_mhi_close(struct alioth_mhi *, unsigned channel);
/* Bytes to the device; `last` ends a transfer. Waits up to timeout_ms for room. */
int alioth_mhi_send(struct alioth_mhi *, unsigned channel, const void *, uint32_t, bool last, unsigned timeout_ms);
/* Everything the device has reported so far (receive callbacks run from here; they must not call into the host). */
int alioth_mhi_poll(struct alioth_mhi *);
int alioth_mhi_wait_ee(struct alioth_mhi *, uint32_t ee, unsigned timeout_ms);

/*
 * The SBL's images, over SAHARA (channels 2 to the device and 3 from it, event ring 1): asked for by id, served
 * from `image`, until the SBL says all are sent and the modem is in mission mode; channels 2 and 3 are then reset.
 * timeout_ms: the longest wait for the device to say anything.
 */
int alioth_mhi_sahara(struct alioth_mhi *, const uint8_t *(*image)(void *, uint64_t id, uint64_t *size), void *,
    unsigned timeout_ms);

#endif
