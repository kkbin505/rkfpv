/* Host-side test: packetize synthetic Annex-B H.265, depacketize, compare. */
#include "../src/rtp_h265.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBUF (1 << 20)

static uint8_t out[MAXBUF];   /* reconstructed Annex-B */
static size_t out_len;
static int markers, pkts, bad_size;
static int last_marker_pkt;
static uint16_t last_seq;
static int have_seq;
static size_t pkt_size;

static void depack_sink(const uint8_t *hdr, size_t hdr_len,
			const uint8_t *pl, size_t pl_len, void *ctx)
{
	(void)ctx;
	uint8_t pkt[4096];
	memcpy(pkt, hdr, hdr_len);
	memcpy(pkt + hdr_len, pl, pl_len);
	size_t n = hdr_len + pl_len;

	if (n > pkt_size)
		bad_size++;
	assert(pkt[0] == 0x80);
	uint16_t seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
	if (have_seq)
		assert(seq == (uint16_t)(last_seq + 1));
	last_seq = seq;
	have_seq = 1;
	if (pkt[1] & 0x80) {
		markers++;
		last_marker_pkt = pkts;
	}
	pkts++;

	const uint8_t *p = pkt + RTP_HDR_LEN;
	size_t len = n - RTP_HDR_LEN;
	uint8_t type = (p[0] >> 1) & 0x3f;

	if (type == 49) {
		uint8_t fu = p[2];
		if (fu & 0x80) {        /* start: rebuild NAL header */
			static const uint8_t sc[4] = { 0, 0, 0, 1 };
			memcpy(out + out_len, sc, 4);
			out_len += 4;
			out[out_len++] = (uint8_t)((p[0] & 0x81) | ((fu & 0x3f) << 1));
			out[out_len++] = p[1];
		}
		memcpy(out + out_len, p + 3, len - 3);
		out_len += len - 3;
	} else {
		static const uint8_t sc[4] = { 0, 0, 0, 1 };
		memcpy(out + out_len, sc, 4);
		out_len += 4;
		memcpy(out + out_len, p, len);
		out_len += len;
	}
}

struct ctx { rtp_h265_t *r; int end; };

static void on_nal(const uint8_t *nal, size_t len, int last, void *c)
{
	struct ctx *x = c;
	rtp_h265_packetize_nal(x->r, nal, len, 1234, last && x->end, depack_sink, NULL);
}

/* Appends a NAL with 4-byte start code; payload avoids 00 00 0x sequences. */
static size_t add_nal(uint8_t *b, size_t pos, uint8_t type, size_t body)
{
	b[pos++] = 0; b[pos++] = 0; b[pos++] = 0; b[pos++] = 1;
	b[pos++] = (uint8_t)(type << 1);
	b[pos++] = 1;
	for (size_t i = 0; i < body; i++)
		b[pos++] = (uint8_t)(1 + rand() % 255);
	return pos;
}

static void reset(void)
{
	out_len = 0; markers = 0; pkts = 0; bad_size = 0; have_seq = 0;
	last_marker_pkt = -1;
}

static void run_case(size_t psize, size_t idr_body, int split_chunks)
{
	static uint8_t in[MAXBUF];
	size_t len = 0;
	len = add_nal(in, len, 32, 20);      /* VPS */
	len = add_nal(in, len, 33, 40);      /* SPS */
	len = add_nal(in, len, 34, 8);       /* PPS */
	size_t idr_at = len;
	len = add_nal(in, len, 19, idr_body);/* IDR_W_RADL */

	rtp_h265_t r = { .fd = -1, .pt = 96, .seq = 65530, .ssrc = 1 };
	r.pkt_size = psize;
	pkt_size = psize;
	reset();

	if (split_chunks) {
		/* parameter sets in one chunk, IDR in another (like RK packs) */
		struct ctx a = { &r, 0 }, b = { &r, 1 };
		annexb_split(in, idr_at, on_nal, &a);
		annexb_split(in + idr_at, len - idr_at, on_nal, &b);
	} else {
		struct ctx a = { &r, 1 };
		annexb_split(in, len, on_nal, &a);
	}

	assert(out_len == len);
	assert(memcmp(in, out, len) == 0);
	assert(markers == 1);
	assert(last_marker_pkt == pkts - 1);
	assert(bad_size == 0);
	printf("ok: pkt_size=%zu idr=%zu chunks=%d -> %d packets\n",
	       psize, idr_body, split_chunks ? 2 : 1, pkts);
}

int main(void)
{
	srand(1);
	run_case(1400, 50000, 0);
	run_case(1400, 50000, 1);
	run_case(1400, 100, 0);       /* everything fits single-NAL packets */
	run_case(1400, 1386, 0);      /* exactly max payload (1400-12-2 body) */
	run_case(1400, 1387, 0);      /* one byte over: FU */
	run_case(200, 7777, 1);
	puts("all tests passed");
	return 0;
}
