/*
 * Host tool: streams an Annex-B H.265 file through rtp_h265_send() to
 * ip:port at a fixed frame rate. Used to check the packetizer end to end
 * against GStreamer's rtph265depay (the same depayloader the ground
 * stations use).
 *
 *   send_file <file.h265> <ip> <port> [fps] [pkt_size]
 */
#include "../src/rtp_h265.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int is_vcl(uint8_t t) { return t < 32; }

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s file.h265 ip port [fps] [pkt_size]\n", argv[0]);
		return 1;
	}
	int fps = argc > 4 ? atoi(argv[4]) : 30;
	size_t psize = argc > 5 ? (size_t)atoi(argv[5]) : 1400;

	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("open"); return 1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 1; }
	fclose(f);

	rtp_h265_t r;
	if (rtp_h265_open(&r, argv[2], atoi(argv[3]), psize)) { perror("socket"); return 1; }

	/* Find NAL start offsets (offset of the start code). */
	size_t cap = 1024, n = 0;
	size_t *starts = malloc(cap * sizeof(*starts));
	for (size_t i = 0; i + 3 < (size_t)sz; i++) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
			size_t s = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;
			if (n == cap) starts = realloc(starts, (cap *= 2) * sizeof(*starts));
			starts[n++] = s;
			i += 2;
		}
	}

	/* Group NALs into access units: a new AU begins at a non-VCL NAL that
	 * follows a VCL NAL, or at a VCL NAL with first_slice_segment_in_pic_flag
	 * that follows a VCL NAL. */
	uint32_t ts = 0;
	int frames = 0;
	size_t au_start = 0;
	int prev_vcl = 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	for (size_t k = 0; k <= n; k++) {
		int boundary = 0;
		if (k == n) {
			boundary = 1;
		} else {
			size_t p = starts[k] + (buf[starts[k] + 2] == 1 ? 3 : 4);
			uint8_t t = (buf[p] >> 1) & 0x3f;
			int first_slice = is_vcl(t) && (buf[p + 2] & 0x80);
			if (prev_vcl && (!is_vcl(t) || first_slice))
				boundary = 1;
			prev_vcl = is_vcl(t);
		}
		if (boundary && k > 0) {
			size_t end = (k == n) ? (size_t)sz : starts[k];
			rtp_h265_send(&r, buf + starts[au_start], end - starts[au_start], ts, 1);
			frames++;
			ts += 90000 / fps;
			au_start = k;

			next.tv_nsec += 1000000000L / fps;
			if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		}
	}

	fprintf(stderr, "sent %d frames, %llu packets, %llu bytes, %llu errors\n",
		frames, (unsigned long long)r.pkts_sent,
		(unsigned long long)r.bytes_sent, (unsigned long long)r.send_errors);
	rtp_h265_close(&r);
	return 0;
}
