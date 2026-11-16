#include "rtp_h265.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define H265_NAL_FU 49
#define FU_HDR_LEN 3   /* 2-byte PayloadHdr + 1-byte FU header */

int rtp_h265_open(rtp_h265_t *r, const char *ip, int port, size_t pkt_size)
{
	memset(r, 0, sizeof(*r));
	if (pkt_size < RTP_HDR_LEN + FU_HDR_LEN + 16)
		return -1;

	r->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (r->fd < 0)
		return -1;

	int sndbuf = 1 << 20;
	setsockopt(r->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	r->dst.sin_family = AF_INET;
	r->dst.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &r->dst.sin_addr) != 1) {
		close(r->fd);
		r->fd = -1;
		return -1;
	}

	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	r->ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
	r->seq = (uint16_t)rand();
	r->pt = 96;
	r->pkt_size = pkt_size;
	return 0;
}

void rtp_h265_close(rtp_h265_t *r)
{
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
}

static void write_rtp_hdr(rtp_h265_t *r, uint8_t *h, uint32_t ts, int marker)
{
	h[0] = 0x80;                               /* V=2 */
	h[1] = (uint8_t)((marker ? 0x80 : 0) | (r->pt & 0x7f));
	h[2] = (uint8_t)(r->seq >> 8);
	h[3] = (uint8_t)(r->seq);
	h[4] = (uint8_t)(ts >> 24);
	h[5] = (uint8_t)(ts >> 16);
	h[6] = (uint8_t)(ts >> 8);
	h[7] = (uint8_t)(ts);
	h[8] = (uint8_t)(r->ssrc >> 24);
	h[9] = (uint8_t)(r->ssrc >> 16);
	h[10] = (uint8_t)(r->ssrc >> 8);
	h[11] = (uint8_t)(r->ssrc);
	r->seq++;
}

void rtp_h265_packetize_nal(rtp_h265_t *r, const uint8_t *nal, size_t len,
			    uint32_t ts90k, int last_nal_of_au,
			    pkt_sink sink, void *ctx)
{
	uint8_t hdr[RTP_HDR_LEN + FU_HDR_LEN];
	size_t max_payload = r->pkt_size - RTP_HDR_LEN;

	if (len < 3)   /* 2-byte NAL header + at least 1 byte */
		return;

	if (len <= max_payload) {
		/* Single NAL unit packet. */
		write_rtp_hdr(r, hdr, ts90k, last_nal_of_au);
		sink(hdr, RTP_HDR_LEN, nal, len, ctx);
		return;
	}

	/* Fragmentation units. */
	uint8_t type = (nal[0] >> 1) & 0x3f;
	uint8_t ph0 = (uint8_t)((nal[0] & 0x81) | (H265_NAL_FU << 1)); /* keep F + LayerId MSB */
	uint8_t ph1 = nal[1];                                          /* LayerId low + TID */
	const uint8_t *p = nal + 2;
	size_t left = len - 2;
	size_t chunk_max = max_payload - FU_HDR_LEN;
	int first = 1;

	while (left > 0) {
		size_t n = left > chunk_max ? chunk_max : left;
		int end = (n == left);

		write_rtp_hdr(r, hdr, ts90k, end && last_nal_of_au);
		hdr[RTP_HDR_LEN + 0] = ph0;
		hdr[RTP_HDR_LEN + 1] = ph1;
		hdr[RTP_HDR_LEN + 2] = (uint8_t)((first ? 0x80 : 0) | (end ? 0x40 : 0) | type);
		sink(hdr, RTP_HDR_LEN + FU_HDR_LEN, p, n, ctx);

		p += n;
		left -= n;
		first = 0;
	}
}

/* Returns the offset of the next start code at or after pos, or len. */
static size_t find_start(const uint8_t *b, size_t len, size_t pos, size_t *sc_len)
{
	for (size_t i = pos; i + 3 <= len; i++) {
		if (b[i] == 0 && b[i + 1] == 0) {
			if (b[i + 2] == 1) {
				*sc_len = 3;
				return i;
			}
			if (i + 4 <= len && b[i + 2] == 0 && b[i + 3] == 1) {
				*sc_len = 4;
				return i;
			}
		}
	}
	*sc_len = 0;
	return len;
}

int annexb_split(const uint8_t *buf, size_t len, nal_cb cb, void *ctx)
{
	size_t sc;
	size_t s = find_start(buf, len, 0, &sc);
	int count = 0;

	if (s == len) {         /* no start code: treat the whole buffer as one NAL */
		if (len) {
			cb(buf, len, 1, ctx);
			count = 1;
		}
		return count;
	}

	while (s < len) {
		size_t nal_start = s + sc;
		size_t next_sc;
		size_t next = find_start(buf, len, nal_start, &next_sc);
		size_t nal_end = next;

		/* trailing_zero_8bits before the next start code belong to nobody */
		while (nal_end > nal_start && buf[nal_end - 1] == 0 && next < len)
			nal_end--;

		if (nal_end > nal_start) {
			cb(buf + nal_start, nal_end - nal_start, next >= len, ctx);
			count++;
		}
		s = next;
		sc = next_sc;
	}
	return count;
}

struct send_ctx {
	rtp_h265_t *r;
	uint32_t ts;
	int end_of_au;
};

static void udp_sink(const uint8_t *hdr, size_t hdr_len,
		     const uint8_t *payload, size_t payload_len, void *ctx)
{
	rtp_h265_t *r = ctx;
	struct iovec iov[2] = {
		{ .iov_base = (void *)hdr, .iov_len = hdr_len },
		{ .iov_base = (void *)payload, .iov_len = payload_len },
	};
	struct msghdr msg = {
		.msg_name = &r->dst,
		.msg_namelen = sizeof(r->dst),
		.msg_iov = iov,
		.msg_iovlen = 2,
	};
	ssize_t n;

	do {
		n = sendmsg(r->fd, &msg, 0);
	} while (n < 0 && errno == EINTR);

	if (n < 0) {
		r->send_errors++;
		return;
	}
	r->pkts_sent++;
	r->bytes_sent += (uint64_t)n;
}

static void send_nal_cb(const uint8_t *nal, size_t len, int last, void *ctx)
{
	struct send_ctx *s = ctx;
	rtp_h265_packetize_nal(s->r, nal, len, s->ts, last && s->end_of_au,
			       udp_sink, s->r);
}

int rtp_h265_send(rtp_h265_t *r, const uint8_t *buf, size_t len,
		  uint32_t ts90k, int end_of_au)
{
	if (!r || r->fd < 0 || !buf)
		return -1;
	struct send_ctx s = { .r = r, .ts = ts90k, .end_of_au = end_of_au };
	return annexb_split(buf, len, send_nal_cb, &s);
}
