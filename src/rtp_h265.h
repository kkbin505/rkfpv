/*
 * Minimal H.265 RTP packetizer (RFC 7798): single NAL unit packets and
 * fragmentation units (FU). No aggregation packets, no DONL.
 */
#ifndef RTP_H265_H
#define RTP_H265_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#define RTP_HDR_LEN 12

typedef struct {
	int fd;
	struct sockaddr_in dst;
	uint16_t seq;
	uint32_t ssrc;
	uint8_t pt;
	size_t pkt_size;      /* total RTP packet size, header included */
	uint64_t pkts_sent;
	uint64_t bytes_sent;
	uint64_t send_errors;
} rtp_h265_t;

/* Opens a UDP socket aimed at ip:port. pkt_size is the max RTP packet size. */
int rtp_h265_open(rtp_h265_t *r, const char *ip, int port, size_t pkt_size);
void rtp_h265_close(rtp_h265_t *r);

/*
 * Sends an Annex-B byte stream chunk (start codes 00 00 01 or 00 00 00 01).
 * ts90k is the RTP timestamp (90 kHz clock). An access unit may be passed
 * in several chunks (the RK encoder returns several packs per frame); pass
 * end_of_au = 1 only with the last chunk, so the marker bit lands on the
 * last packet of the access unit.
 * Returns the number of NAL units sent, or -1 on a bad argument.
 */
int rtp_h265_send(rtp_h265_t *r, const uint8_t *buf, size_t len,
		  uint32_t ts90k, int end_of_au);

/*
 * Splits an Annex-B buffer into NAL units. Calls cb for each NAL (start code
 * stripped). Exposed for testing.
 */
typedef void (*nal_cb)(const uint8_t *nal, size_t len, int last, void *ctx);
int annexb_split(const uint8_t *buf, size_t len, nal_cb cb, void *ctx);

/* Low-level: builds packets for one NAL into a caller sink. Exposed for tests. */
typedef void (*pkt_sink)(const uint8_t *hdr, size_t hdr_len,
			 const uint8_t *payload, size_t payload_len, void *ctx);
void rtp_h265_packetize_nal(rtp_h265_t *r, const uint8_t *nal, size_t len,
			    uint32_t ts90k, int last_nal_of_au,
			    pkt_sink sink, void *ctx);

#endif
