/*
 * QRTR packets, the IPC router's, as the K40's SDX55 sends them over its MHI channels 20/21 ("IPCR"): the
 * header (version 1: eight little-endian words; version 2, which Linux also takes: 16 bytes), and the control
 * messages that tell of services (HELLO, BYE, NEW_SERVER, DEL_SERVER, NEW_LOOKUP ...). The layout is Linux's
 * (net/qrtr/qrtr.h, af_qrtr.c), written here from it. Header-only, for the kernel (bsd/dev/alioth_mdm.c, step 6)
 * and the pretend modem on the build machine (devices/redmi-k40/tools/test-mhi.c) alike.
 */
#ifndef _SYS_ALIOTH_QRTR_H_
#define _SYS_ALIOTH_QRTR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QRTR_V1_HEADER          32
#define QRTR_V2_HEADER          16
#define QRTR_TYPE_DATA          1
#define QRTR_TYPE_HELLO         2
#define QRTR_TYPE_BYE           3
#define QRTR_TYPE_NEW_SERVER    4
#define QRTR_TYPE_DEL_SERVER    5
#define QRTR_TYPE_DEL_CLIENT    6
#define QRTR_TYPE_RESUME_TX     7
#define QRTR_TYPE_EXIT          8
#define QRTR_TYPE_PING          9
#define QRTR_TYPE_NEW_LOOKUP    10
#define QRTR_TYPE_DEL_LOOKUP    11
#define QRTR_PORT_CTRL          0xfffffffeu
#define QRTR_NODE_BCAST         0xffffffffu
#define QRTR_CONTROL_SIZE       20              /* cmd, then four words */
#define QRTR_SERVICE_LOC        0x10            /* QMI location (GNSS) */

struct qrtr_packet {
	uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
	const uint8_t *payload;
};

struct qrtr_server {
	uint32_t service, version, instance, node, port;
};

static inline uint32_t
qrtr_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t
qrtr_le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline void
qrtr_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* A packet of either version; false if it is not one, or says more than there is. */
static inline bool
qrtr_parse(const uint8_t *p, size_t length, struct qrtr_packet *out)
{
	size_t header;

	if (length < 4) {
		return false;
	}
	if (qrtr_le32(p) == 1) {
		if (length < QRTR_V1_HEADER) {
			return false;
		}
		out->version = 1;
		out->type = qrtr_le32(p + 4);
		out->src_node = qrtr_le32(p + 8);
		out->src_port = qrtr_le32(p + 12);
		out->confirm_rx = qrtr_le32(p + 16);
		out->size = qrtr_le32(p + 20);
		out->dst_node = qrtr_le32(p + 24);
		out->dst_port = qrtr_le32(p + 28);
		header = QRTR_V1_HEADER;
	} else if (p[0] == 3) {
		if (length < QRTR_V2_HEADER) {
			return false;
		}
		header = QRTR_V2_HEADER + 4 * (size_t)p[3];         /* optlen: words of options */
		out->version = 2;
		out->type = p[1];
		out->confirm_rx = p[2] & 1;
		out->size = qrtr_le16(p + 4);
		out->src_node = qrtr_le16(p + 6);
		out->src_port = qrtr_le16(p + 8);
		out->dst_node = qrtr_le16(p + 10);
		out->dst_port = qrtr_le16(p + 12);
		/* (16-bit ports: the control port is 0xfffe there) */
		if (out->src_port == 0xfffe) {
			out->src_port = QRTR_PORT_CTRL;
		}
		if (out->dst_port == 0xfffe) {
			out->dst_port = QRTR_PORT_CTRL;
		}
		if (out->dst_node == 0xffff) {
			out->dst_node = QRTR_NODE_BCAST;
		}
	} else {
		return false;
	}
	if (header > length || out->size > length - header) {
		return false;
	}
	out->payload = p + header;
	return true;
}

/* A version 1 packet into `out` (room bytes): its length, padded to words, or 0 if it does not fit. */
static inline size_t
qrtr_make(uint8_t *out, size_t room, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
    uint32_t dst_port, const void *payload, uint32_t size, bool confirm_rx)
{
	size_t length = QRTR_V1_HEADER + (((size_t)size + 3) & ~(size_t)3);

	if (length > room) {
		return 0;
	}
	for (size_t i = 0; i < length; i++) {
		out[i] = 0;
	}
	qrtr_put32(out, 1);
	qrtr_put32(out + 4, type);
	qrtr_put32(out + 8, src_node);
	qrtr_put32(out + 12, src_port);
	qrtr_put32(out + 16, confirm_rx);
	qrtr_put32(out + 20, size);
	qrtr_put32(out + 24, dst_node);
	qrtr_put32(out + 28, dst_port);
	for (uint32_t i = 0; i < size; i++) {
		out[QRTR_V1_HEADER + i] = ((const uint8_t *)payload)[i];
	}
	return length;
}

/* A control message (cmd = its type, then four words) from our control port. */
static inline size_t
qrtr_make_control(uint8_t *out, size_t room, uint32_t type, uint32_t our_node, uint32_t dst_node, const uint32_t words[4])
{
	uint8_t payload[QRTR_CONTROL_SIZE];

	qrtr_put32(payload, type);
	for (unsigned i = 0; i < 4; i++) {
		qrtr_put32(payload + 4 + 4 * i, words != NULL ? words[i] : 0);
	}
	return qrtr_make(out, room, type, our_node, QRTR_PORT_CTRL, dst_node, QRTR_PORT_CTRL, payload, QRTR_CONTROL_SIZE, false);
}

/* NEW_SERVER / DEL_SERVER: service, instance (<< 8 | version), node, port. */
static inline bool
qrtr_server_of(const struct qrtr_packet *packet, struct qrtr_server *server)
{
	uint32_t instance;

	if ((packet->type != QRTR_TYPE_NEW_SERVER && packet->type != QRTR_TYPE_DEL_SERVER) || packet->size < QRTR_CONTROL_SIZE ||
	    qrtr_le32(packet->payload) != packet->type) {
		return false;
	}
	instance = qrtr_le32(packet->payload + 8);
	server->service = qrtr_le32(packet->payload + 4);
	server->version = instance & 0xff;
	server->instance = instance >> 8;
	server->node = qrtr_le32(packet->payload + 12);
	server->port = qrtr_le32(packet->payload + 16);
	return true;
}

#endif
