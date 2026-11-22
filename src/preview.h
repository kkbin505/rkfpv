/*
 * Wired preview: the link stream (the same H.265 access units, OSD burned
 * in, that go to wfb_tx) served to one TCP client on 127.0.0.1, reached
 * from the PC with `adb forward tcp:5602 tcp:5602`.
 *
 * Wire format, one record per access unit:
 *   u32 big-endian length, then the access unit in Annex-B
 *
 * Never blocks the stream loop: the socket is non-blocking, a frame that
 * does not fit is dropped and the stream resumes at the next IDR (the
 * caller is asked for one). No client = nothing allocated, nothing sent.
 */
#ifndef PREVIEW_H
#define PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#define PREVIEW_MAX_AU (1024 * 1024)   /* larger access units are dropped */

typedef struct {
	int lfd;                /* listening socket, -1 = off */
	int cfd;                /* client, -1 = none */
	/* two buffers that swap: one collects the next access unit from the
	 * packs (4 bytes kept free in front for the length), the other holds
	 * the record still being sent. No copy, no allocation per frame. */
	uint8_t *au;
	size_t au_len, au_cap;  /* au_len counts the AU bytes after the header */
	int au_bad;             /* this AU overflowed: drop it */
	uint8_t *out;
	size_t out_cap, out_len, out_off;
	int need_idr;           /* skip until an IDR access unit */
	int want_idr;           /* caller should request an IDR */
	uint64_t clients, frames, dropped;
} preview_t;

/* Listens on ip:port (port 0 = off: every call becomes a no-op).
 * Returns 0, or -1 with a message on stderr (preview stays off). */
int preview_open(preview_t *p, const char *ip, int port);
void preview_close(preview_t *p);

/* Accepts a new client, notices a gone one, sends what is pending.
 * Call once per stream loop iteration. */
void preview_poll(preview_t *p);

/* Feed every pack of the link stream; end_of_au on the frame's last pack. */
void preview_push(preview_t *p, const uint8_t *data, size_t len, int end_of_au);

/* 1 once after a client connected or a frame was dropped: request an IDR. */
int preview_take_idr_request(preview_t *p);

/* 1 if the Annex-B access unit holds an IRAP picture or a VPS. */
int preview_is_idr(const uint8_t *au, size_t len);

#endif
