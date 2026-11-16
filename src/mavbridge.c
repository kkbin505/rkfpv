/*
 * mavbridge: flight-controller UART <-> UDP, for wfb-ng telemetry.
 *
 *   downlink: UART -> split into MAVLink v1/v2 frames -> pack several whole
 *             frames per UDP datagram -> wfb_tx (-p 16 -u 14551)
 *   uplink:   wfb_rx (-p 144 -u 14550) -> UDP -> UART, bytes as received
 *
 * Packing whole frames means a lost radio packet drops whole messages, never
 * half a frame that would corrupt the next one. No CRC check here; the
 * ground station's MAVLink parser does that.
 *
 *   mavbridge [-d /dev/ttyS3] [-b 115200] [-o 127.0.0.1:14551] [-i 14550]
 *             [-a 15] [-m 1000] [-t 30] [-r] [-s 0]
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAV1_STX 0xFE
#define MAV2_STX 0xFD
#define MAX_FRAME 280            /* v2: 10 hdr + 255 payload + 2 crc + 13 sig */
#define RXBUF (MAX_FRAME * 8)

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static speed_t baud_const(int b)
{
	switch (b) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1500000: return B1500000;
	default: return 0;
	}
}

static int open_serial(const char *dev, int baud)
{
	speed_t sp = baud_const(baud);
	if (!sp) {
		fprintf(stderr, "unsupported baud rate %d\n", baud);
		return -1;
	}
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return -1;
	}
	struct termios tio;
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tio.c_cflag &= ~(CSTOPB | CRTSCTS);
		tio.c_cc[VMIN] = 0;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, sp);
		cfsetospeed(&tio, sp);
		tcsetattr(fd, TCSANOW, &tio);
		tcflush(fd, TCIOFLUSH);
	}
	/* isatty() false (e.g. a FIFO in tests): just use it as a byte stream */
	return fd;
}

static int parse_hostport(const char *s, struct sockaddr_in *a)
{
	char host[64];
	const char *colon = strrchr(s, ':');
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	if (!colon || (size_t)(colon - s) >= sizeof(host))
		return -1;
	memcpy(host, s, colon - s);
	host[colon - s] = '\0';
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &a->sin_addr) != 1)
		return -1;
	a->sin_port = htons(port);
	return 0;
}

/* Length of the complete frame at b[0] (b[0] is an STX), 0 if more bytes are
 * needed, -1 if b[0] cannot start a frame. */
static int frame_len(const uint8_t *b, size_t n)
{
	if (b[0] == MAV1_STX) {
		if (n < 2) return 0;
		return 8 + b[1];
	}
	if (b[0] == MAV2_STX) {
		if (n < 3) return 0;
		if (b[2] & 0xFE)        /* only "signed" is a defined incompat flag */
			return -1;
		return 12 + b[1] + ((b[2] & 0x01) ? 13 : 0);
	}
	return -1;
}

typedef struct {
	int udp;
	struct sockaddr_in dst;
	int max_msgs;           /* flush after this many frames */
	int max_bytes;          /* datagram size cap */
	int flush_ms;           /* flush this long after the first queued frame */

	uint8_t out[2048];
	int out_len;
	int out_msgs;
	uint64_t out_since;

	uint64_t frames, datagrams, bytes_down, bytes_up, junk;
} bridge_t;

static void flush_out(bridge_t *b)
{
	if (!b->out_len)
		return;
	if (sendto(b->udp, b->out, b->out_len, 0,
		   (struct sockaddr *)&b->dst, sizeof(b->dst)) > 0) {
		b->datagrams++;
		b->bytes_down += b->out_len;
	}
	b->out_len = 0;
	b->out_msgs = 0;
}

static void queue_frame(bridge_t *b, const uint8_t *f, int len)
{
	if (b->out_len + len > b->max_bytes)
		flush_out(b);
	if (!b->out_len)
		b->out_since = now_ms();
	memcpy(b->out + b->out_len, f, len);
	b->out_len += len;
	b->out_msgs++;
	b->frames++;
	if (b->out_msgs >= b->max_msgs || b->out_len >= b->max_bytes)
		flush_out(b);
}

/* Consumes complete frames from rx; returns bytes left (a partial frame). */
static size_t parse_rx(bridge_t *b, uint8_t *rx, size_t n)
{
	size_t i = 0;
	while (i < n) {
		if (rx[i] != MAV1_STX && rx[i] != MAV2_STX) {
			i++;
			b->junk++;
			continue;
		}
		int fl = frame_len(rx + i, n - i);
		if (fl < 0) {                        /* false STX: resync */
			i++;
			b->junk++;
			continue;
		}
		if (fl == 0 || (size_t)fl > n - i)
			break;                       /* need more bytes */
		queue_frame(b, rx + i, fl);
		i += fl;
	}
	memmove(rx, rx + i, n - i);
	return n - i;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -d dev       serial device             (/dev/ttyS3)\n"
		"  -b baud      baud rate                 (115200)\n"
		"  -o ip:port   downlink UDP destination  (127.0.0.1:14551, wfb_tx -u)\n"
		"  -i port      uplink UDP listen port    (14550, wfb_rx -u)\n"
		"  -a n         frames per datagram       (15; 1 = no packing)\n"
		"  -m bytes     max datagram size         (1000)\n"
		"  -t ms        flush timeout             (30)\n"
		"  -r           raw: forward UART bytes without MAVLink framing\n"
		"  -s sec       print stats every sec     (0 = off)\n", p);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3", *out = "127.0.0.1:14551";
	int baud = 115200, in_port = 14550, raw = 0, stats = 0, opt;
	bridge_t b;
	memset(&b, 0, sizeof(b));
	b.max_msgs = 15;
	b.max_bytes = 1000;
	b.flush_ms = 30;

	while ((opt = getopt(argc, argv, "d:b:o:i:a:m:t:rs:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'b': baud = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'i': in_port = atoi(optarg); break;
		case 'a': b.max_msgs = atoi(optarg); break;
		case 'm': b.max_bytes = atoi(optarg); break;
		case 't': b.flush_ms = atoi(optarg); break;
		case 'r': raw = 1; break;
		case 's': stats = atoi(optarg); break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (b.max_msgs < 1) b.max_msgs = 1;
	if (b.max_bytes < MAX_FRAME) b.max_bytes = MAX_FRAME;
	if (b.max_bytes > (int)sizeof(b.out)) b.max_bytes = sizeof(b.out);

	if (parse_hostport(out, &b.dst)) {
		fprintf(stderr, "bad -o %s\n", out);
		return 1;
	}
	int ser = open_serial(dev, baud);
	if (ser < 0)
		return 1;

	b.udp = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in in;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_port = htons(in_port);
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int one = 1;
	setsockopt(b.udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (b.udp < 0 || bind(b.udp, (struct sockaddr *)&in, sizeof(in))) {
		perror("udp bind");
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr, "mavbridge: %s@%d -> %s, udp:%d -> uart, %s, %d frames/%d B/%d ms\n",
		dev, baud, out, in_port, raw ? "raw" : "mavlink",
		b.max_msgs, b.max_bytes, b.flush_ms);

	uint8_t rx[RXBUF];
	size_t rx_len = 0;
	uint64_t t_stats = now_ms();

	while (!g_quit) {
		struct pollfd p[2] = {
			{ .fd = ser, .events = POLLIN },
			{ .fd = b.udp, .events = POLLIN },
		};
		int timeout = 100;
		if (b.out_len) {
			int64_t left = (int64_t)(b.out_since + b.flush_ms) - (int64_t)now_ms();
			timeout = left > 0 ? (int)left : 0;
		}
		if (poll(p, 2, timeout) < 0 && errno != EINTR)
			break;

		if (p[0].revents & POLLIN) {
			ssize_t n = read(ser, rx + rx_len, sizeof(rx) - rx_len);
			if (n > 0) {
				if (raw) {
					/* raw mode: forward bytes as they come, size-capped */
					for (ssize_t k = 0; k < n; ) {
						int room = b.max_bytes - b.out_len;
						int c = (n - k) < room ? (int)(n - k) : room;
						if (!b.out_len) b.out_since = now_ms();
						memcpy(b.out + b.out_len, rx + k, c);
						b.out_len += c;
						k += c;
						if (b.out_len >= b.max_bytes) flush_out(&b);
					}
				} else {
					rx_len = parse_rx(&b, rx, rx_len + n);
					if (rx_len == sizeof(rx))   /* cannot happen with valid lengths */
						rx_len = 0;
				}
			}
		}
		if (p[1].revents & POLLIN) {
			uint8_t up[2048];
			ssize_t n = recv(b.udp, up, sizeof(up), 0);
			if (n > 0) {
				ssize_t w = 0;
				while (w < n) {
					ssize_t k = write(ser, up + w, n - w);
					if (k > 0) { w += k; continue; }
					if (k < 0 && errno != EAGAIN && errno != EINTR) break;
					struct pollfd wp = { .fd = ser, .events = POLLOUT };
					poll(&wp, 1, 20);
				}
				b.bytes_up += w;
			}
		}

		if (b.out_len && now_ms() >= b.out_since + b.flush_ms)
			flush_out(&b);

		if (stats && now_ms() - t_stats >= (uint64_t)stats * 1000u) {
			fprintf(stderr, "stats: frames %llu, datagrams %llu, down %llu B, up %llu B, junk %llu B\n",
				(unsigned long long)b.frames, (unsigned long long)b.datagrams,
				(unsigned long long)b.bytes_down, (unsigned long long)b.bytes_up,
				(unsigned long long)b.junk);
			t_stats = now_ms();
		}
	}
	flush_out(&b);
	close(ser);
	close(b.udp);
	return 0;
}
