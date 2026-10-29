/*
 * usbmux: the phone's side of Apple's usbmux, over the kernel's iPhone-shaped USB port (boot-arg usb=3,
 * /dev/hakios-usb: bsd/dev/hakios.usb.c, the descriptors in hakios.dwc3.c). The computer's usbmuxd --
 * Apple Mobile Device Service on Windows, usbmuxd on Linux and macOS -- finds an iPhone there and opens
 * TCP-like connections through it to the phone's ports: lockdownd's 62078 first, the services lockdownd
 * starts after. Each one becomes a connection to 127.0.0.1 here.
 *
 * The protocol, as usbmuxd's src/device.c speaks it from the other end:
 *   a packet = a header (protocol, length -- big-endian 32-bit -- and from version 2 on: magic 0xfeedface,
 *   tx_seq, rx_seq, 16-bit) and its payload; one packet is one USB transfer each way (the kernel's write()
 *   is one transfer; what the computer sends comes as a stream here and is cut by the lengths)
 *   - protocol 0, version: the computer's {2, 0} with an 8-byte header; answered {2, 0} the same way
 *   - protocol 2, setup: one byte (7), after which both sides use the 16-byte header
 *   - protocol 6, TCP: a struct tcphdr and data. SYN opens (answered SYN|ACK, or RST when nothing listens
 *     on that port), packets with exactly ACK carry data and acknowledgements (th_ack: the next byte
 *     expected; th_win: the room left, in 256-byte units), RST closes, from either side. There is no FIN,
 *     and anything but ACK on an open connection is a reset to the computer.
 *   - protocol 1, control: messages for the computer's log; none are sent from here
 *
 * The UDID (MobileGestalt's UniqueDeviceID) is what the port shows as its serial: given to the kernel
 * (ioctl HAKIOS_USB_IPHONE), which starts the device side only then.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

struct hakios_usb_iphone {
	char serial[48];
};
#define HAKIOS_USB_IPHONE       _IOW('u', 2, struct hakios_usb_iphone)

#define PACKET_MAX      4096                    /* the kernel's DWC3_PACKET_MAX: one USB transfer */
#define HEADER_V1       8
#define HEADER_V2       16
#define PAYLOAD_MAX     (PACKET_MAX - HEADER_V2 - (int)sizeof(struct tcphdr))
#define CONNS           64
#define TO_SOCKET       (256 * 1024)            /* what the computer sent, not yet taken by the socket */

enum { PROTO_VERSION = 0, PROTO_CONTROL = 1, PROTO_SETUP = 2, PROTO_TCP = IPPROTO_TCP };

struct conn {
	int fd;                         /* -1: free */
	uint16_t host_port, port;       /* the computer's port, ours */
	uint32_t seq;                   /* the next byte we send */
	uint32_t host_next;             /* the next byte we expect from the computer (what we acknowledge) */
	uint32_t host_acked;            /* how far the computer has acknowledged ours */
	uint32_t host_window;           /* the room the computer said it has */
	uint8_t *out;                   /* TO_SOCKET bytes: from the computer, for the socket */
	uint32_t out_used;
	uint32_t window_said;           /* the room we last told the computer of */
};

static int port = -1;
static int v2;                          /* the 16-byte header (from the setup packet on) */
static uint16_t tx_seq, rx_seq;
static struct conn conns[CONNS];
static unsigned verbose = 1;
static unsigned long packets_in, packets_out;

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	fputs("usbmux: ", stderr);
	vfprintf(stderr, format, list);
	fputc('\n', stderr);
	va_end(list);
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* One packet to the computer: our header, then the protocol's header and data, in one write (one transfer). */
static int
send_packet(uint32_t protocol, const void *header, size_t header_len, const void *data, size_t data_len, int force_v1)
{
	uint8_t packet[PACKET_MAX];
	size_t hdr = (v2 && !force_v1) ? HEADER_V2 : HEADER_V1, total = hdr + header_len + data_len;

	if (total > sizeof(packet)) {
		say("a packet of %zu bytes is more than a transfer: not sent", total);
		return -1;
	}
	put32(packet, protocol);
	put32(packet + 4, (uint32_t)total);
	if (hdr == HEADER_V2) {
		put32(packet + 8, 0xfeedface);
		packet[12] = (uint8_t)(tx_seq >> 8); packet[13] = (uint8_t)tx_seq;
		packet[14] = (uint8_t)(rx_seq >> 8); packet[15] = (uint8_t)rx_seq;
		tx_seq++;
	}
	if (header_len) {
		memcpy(packet + hdr, header, header_len);
	}
	if (data_len) {
		memcpy(packet + hdr + header_len, data, data_len);
	}
	for (;;) {
		ssize_t w = write(port, packet, total);

		if (w == (ssize_t)total) {
			packets_out++;
			return 0;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		say("write to the port: %s", w < 0 ? strerror(errno) : "short");
		return -1;
	}
}

static uint32_t
room(const struct conn *c)
{
	return TO_SOCKET - c->out_used;
}

static int
send_tcp(struct conn *c, uint16_t host_port, uint16_t our_port, uint32_t seq, uint32_t ack, uint8_t flags, const void *data,
    size_t len)
{
	struct tcphdr th;

	memset(&th, 0, sizeof(th));
	th.th_sport = htons(our_port);
	th.th_dport = htons(host_port);
	th.th_seq = htonl(seq);
	th.th_ack = htonl(ack);
	th.th_off = sizeof(th) / 4;
	th.th_flags = flags;
	th.th_win = htons((uint16_t)((c != NULL ? room(c) : 0) >> 8));
	if (c != NULL) {
		c->window_said = room(c);
	}
	return send_packet(PROTO_TCP, &th, sizeof(th), data, len, 0);
}

static void
conn_close(struct conn *c, int send_rst, const char *why)
{
	if (c->fd < 0) {
		return;
	}
	if (send_rst) {
		(void)send_tcp(NULL, c->host_port, c->port, c->seq, c->host_next, TH_RST, NULL, 0);
	}
	if (verbose) {
		say("connection %u -> %u closed (%s)", c->host_port, c->port, why);
	}
	close(c->fd);
	c->fd = -1;
	free(c->out);
	c->out = NULL;
	c->out_used = 0;
}

static void
close_all(const char *why)
{
	for (int i = 0; i < CONNS; i++) {
		conn_close(&conns[i], 0, why);
	}
}

static struct conn *
find(uint16_t host_port, uint16_t our_port)
{
	for (int i = 0; i < CONNS; i++) {
		if (conns[i].fd >= 0 && conns[i].host_port == host_port && conns[i].port == our_port) {
			return &conns[i];
		}
	}
	return NULL;
}

/* The computer opens a connection to one of our ports: a socket to 127.0.0.1 on it, SYN|ACK, or RST. */
static void
tcp_open(const struct tcphdr *th)
{
	uint16_t host_port = ntohs(th->th_sport), our_port = ntohs(th->th_dport);
	struct conn *c = find(host_port, our_port), *slot = NULL;
	struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(our_port),
		                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int fd, one = 1;

	if (c != NULL) {
		conn_close(c, 0, "opened again");
	}
	for (int i = 0; i < CONNS && slot == NULL; i++) {
		if (conns[i].fd < 0) {
			slot = &conns[i];
		}
	}
	fd = slot != NULL ? socket(AF_INET, SOCK_STREAM, 0) : -1;
	if (fd < 0 || connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
		say("connection %u -> %u refused (%s)", host_port, our_port, slot == NULL ? "too many" : strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		(void)send_tcp(NULL, host_port, our_port, 0, ntohl(th->th_seq) + 1, TH_RST, NULL, 0);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	memset(slot, 0, sizeof(*slot));
	slot->fd = fd;
	slot->host_port = host_port;
	slot->port = our_port;
	slot->seq = 0;
	slot->host_next = ntohl(th->th_seq) + 1;
	slot->host_acked = 0;
	slot->host_window = (uint32_t)ntohs(th->th_win) << 8;
	slot->out = malloc(TO_SOCKET);
	if (slot->out == NULL) {
		conn_close(slot, 1, "no memory");
		return;
	}
	(void)send_tcp(slot, host_port, our_port, slot->seq, slot->host_next, TH_SYN | TH_ACK, NULL, 0);
	slot->seq++;            /* (the SYN counts one) */
	slot->host_acked = slot->seq;
	if (verbose) {
		say("connection %u -> %u (127.0.0.1:%u) open", host_port, our_port, our_port);
	}
}

/* What the computer sent waits in out until the socket takes it. */
static int
flush_to_socket(struct conn *c)
{
	while (c->out_used > 0) {
		ssize_t w = write(c->fd, c->out, c->out_used);

		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				return 0;
			}
			return -1;
		}
		memmove(c->out, c->out + w, c->out_used - (uint32_t)w);
		c->out_used -= (uint32_t)w;
	}
	return 0;
}

static void
tcp_input(const uint8_t *p, size_t len)
{
	const struct tcphdr *th = (const struct tcphdr *)(const void *)p;
	size_t off;
	struct conn *c;
	uint16_t host_port, our_port;

	if (len < sizeof(struct tcphdr)) {
		return;
	}
	off = (size_t)th->th_off * 4;
	if (off < sizeof(struct tcphdr) || off > len) {
		off = sizeof(struct tcphdr);
	}
	host_port = ntohs(th->th_sport);
	our_port = ntohs(th->th_dport);
	if (th->th_flags & TH_SYN) {
		tcp_open(th);
		return;
	}
	c = find(host_port, our_port);
	if (th->th_flags & TH_RST) {
		if (c != NULL) {
			conn_close(c, 0, "reset by the computer");
		}
		return;
	}
	if (c == NULL) {
		(void)send_tcp(NULL, host_port, our_port, 0, ntohl(th->th_seq), TH_RST, NULL, 0);
		return;
	}
	if (th->th_flags != TH_ACK) {
		conn_close(c, 1, "flags not understood");
		return;
	}
	c->host_acked = ntohl(th->th_ack);
	c->host_window = (uint32_t)ntohs(th->th_win) << 8;
	if (len > off) {
		size_t n = len - off;

		if (n > room(c)) {
			conn_close(c, 1, "more than the window");
			return;
		}
		memcpy(c->out + c->out_used, p + off, n);
		c->out_used += (uint32_t)n;
		c->host_next += (uint32_t)n;
		if (flush_to_socket(c) != 0) {
			conn_close(c, 1, "the socket closed");
			return;
		}
		(void)send_tcp(c, c->host_port, c->port, c->seq, c->host_next, TH_ACK, NULL, 0);
	}
}

static void
packet_input(const uint8_t *p, size_t len, size_t hdr)
{
	uint32_t protocol = be32(p);

	packets_in++;
	if (hdr == HEADER_V2) {
		rx_seq = (uint16_t)(p[12] << 8 | p[13]);        /* the computer's tx_seq: what we have seen */
	}
	switch (protocol) {
	case PROTO_VERSION: {
		uint8_t answer[12];

		if (len < hdr + 8) {
			return;
		}
		close_all("a new session");
		say("the computer's usbmux, version %u.%u: answered 2.0", be32(p + hdr), be32(p + hdr + 4));
		memset(answer, 0, sizeof(answer));
		put32(answer, 2);
		put32(answer + 4, 0);
		v2 = 0;
		(void)send_packet(PROTO_VERSION, answer, sizeof(answer), NULL, 0, 1);
		break;
	}
	case PROTO_SETUP:
		/* from here on, 16-byte headers both ways; the computer counts from 0, and so do we */
		v2 = 1;
		tx_seq = 0;
		say("setup (%zu byte%s): the session is up", len - hdr, len - hdr == 1 ? "" : "s");
		break;
	case PROTO_CONTROL:
		break;
	case PROTO_TCP:
		tcp_input(p + hdr, len - hdr);
		break;
	default:
		say("a packet of protocol %u: not understood", protocol);
		break;
	}
}

/* What came from the port, cut into packets by their lengths. */
static void
port_input(uint8_t *buf, size_t *used)
{
	size_t at = 0;

	while (*used - at >= HEADER_V1) {
		uint32_t protocol = be32(buf + at), len = be32(buf + at + 4);
		size_t hdr = protocol == PROTO_VERSION || !v2 ? HEADER_V1 : HEADER_V2;

		if (len < hdr || len > 1u << 20) {
			say("a packet of %u bytes (protocol %u): out of step, %zu bytes dropped", len, protocol, *used - at);
			at = *used;
			break;
		}
		if (*used - at < len) {
			break;
		}
		packet_input(buf + at, len, hdr);
		at += len;
	}
	memmove(buf, buf + at, *used - at);
	*used -= at;
}

static int
udid(char *out, size_t size)
{
	CFTypeRef answer = MGCopyAnswer(CFSTR("UniqueDeviceID"));
	int ok = answer != NULL && CFGetTypeID(answer) == CFStringGetTypeID() &&
	    CFStringGetCString(answer, out, (CFIndex)size, kCFStringEncodingASCII);

	if (answer != NULL) {
		CFRelease(answer);
	}
	return ok ? 0 : -1;
}

int
main(int argc, char **argv)
{
	static uint8_t in[256 * 1024];
	size_t used = 0;
	struct hakios_usb_iphone ask;

	(void)argc;
	(void)argv;
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	for (int i = 0; i < CONNS; i++) {
		conns[i].fd = -1;
	}
	port = open("/dev/hakios-usb", O_RDWR);
	if (port < 0) {
		say("no /dev/hakios-usb (%s): the kernel has not got the controller (usb=3)", strerror(errno));
		return 0;
	}
	memset(&ask, 0, sizeof(ask));
	if (udid(ask.serial, sizeof(ask.serial)) != 0) {
		say("no UniqueDeviceID from MobileGestalt: the port shows the phone's serial instead");
		strlcpy(ask.serial, "hakios-alioth", sizeof(ask.serial));
	}
	if (ioctl(port, HAKIOS_USB_IPHONE, &ask) != 0 && errno != EALREADY) {
		say("the kernel will not show an iPhone (%s): not usb=3?", strerror(errno));
		return 0;
	}
	say("an iPhone to the computer, serial %s: waiting for its usbmux", ask.serial);
	for (;;) {
		struct pollfd fds[1 + CONNS];
		struct conn *which[1 + CONNS];
		nfds_t n = 0;

		fds[n].fd = port;
		fds[n].events = POLLIN;
		which[n++] = NULL;
		for (int i = 0; i < CONNS; i++) {
			struct conn *c = &conns[i];
			uint32_t in_flight;

			if (c->fd < 0) {
				continue;
			}
			in_flight = c->seq - c->host_acked;
			fds[n].fd = c->fd;
			fds[n].events = (c->host_window > in_flight ? POLLIN : 0) | (c->out_used ? POLLOUT : 0);
			which[n++] = c;
		}
		if (poll(fds, n, 1000) < 0) {
			if (errno == EINTR) {
				continue;
			}
			say("poll: %s", strerror(errno));
			return 1;
		}
		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			ssize_t r = read(port, in + used, sizeof(in) - used);

			if (r < 0 && errno != EINTR && errno != EAGAIN) {
				say("read from the port: %s", strerror(errno));
				return 1;
			}
			if (r > 0) {
				used += (size_t)r;
				port_input(in, &used);
			}
		}
		for (nfds_t k = 1; k < n; k++) {
			struct conn *c = which[k];

			if (c->fd < 0 || fds[k].fd != c->fd) {
				continue;       /* (closed, or its slot reused, while the packets were read) */
			}
			if (fds[k].revents & POLLOUT) {
				uint32_t before = room(c);

				if (flush_to_socket(c) != 0) {
					conn_close(c, 1, "the socket closed");
					continue;
				}
				/* the window opened by much since the computer last heard: say so */
				if (room(c) > before && room(c) - c->window_said >= TO_SOCKET / 4) {
					(void)send_tcp(c, c->host_port, c->port, c->seq, c->host_next, TH_ACK, NULL, 0);
				}
			}
			if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
				uint8_t data[PAYLOAD_MAX];
				uint32_t in_flight = c->seq - c->host_acked;
				uint32_t can = c->host_window > in_flight ? c->host_window - in_flight : 0;
				ssize_t r;

				if (can == 0 && !(fds[k].revents & (POLLHUP | POLLERR))) {
					continue;
				}
				r = read(c->fd, data, can < sizeof(data) ? can : sizeof(data));
				if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
					conn_close(c, 1, r == 0 ? "the service closed it" : strerror(errno));
					continue;
				}
				if (r > 0) {
					if (send_tcp(c, c->host_port, c->port, c->seq, c->host_next, TH_ACK, data, (size_t)r) != 0) {
						conn_close(c, 0, "the port failed");
						continue;
					}
					c->seq += (uint32_t)r;
				}
			}
		}
	}
}
