/*
 * Frame tap: rkfpv puts a small grey (luma) copy of the camera picture in
 * shared memory for other programs on the board (detection, AprilTag,
 * optical flow, ...). One writer (rkfpv), any number of readers; readers
 * never block the video.
 *
 * Layout of FRAMETAP_PATH (a file in /dev/shm, i.e. RAM):
 *   frametap_hdr_t, then FRAMETAP_SLOTS pictures of slot_bytes each at
 *   data_offset. A picture is width x height bytes, rows `stride` apart,
 *   one byte per pixel (Y of the camera's YUV, 0 black .. 255 white).
 *
 * Each slot has a sequence counter that is odd while rkfpv writes it
 * (seqlock). `latest` names the newest complete slot. A reader copies the
 * slot and checks the counter did not change; frametap_read() below does
 * that. This header is the whole reader API: include it, open the file,
 * mmap it read-only.
 *
 * Settings: frame_tap = 1, frame_tap_fps, frame_tap_scale (rkfpv.conf).
 */
#ifndef RKFPV_FRAMETAP_H
#define RKFPV_FRAMETAP_H

#include <stdint.h>
#include <string.h>

#define FRAMETAP_PATH "/dev/shm/rkfpv-frame"
#define FRAMETAP_MAGIC 0x54464b52u      /* "RKFT" */
#define FRAMETAP_VERSION 1
#define FRAMETAP_SLOTS 2

#ifndef FRAMETAP_MEMCPY
#define FRAMETAP_MEMCPY memcpy  /* tests slow it down to force overlaps */
#endif

typedef struct {
	uint32_t seq;           /* odd while being written */
	uint32_t pad;
	uint64_t frame;         /* rkfpv's tap frame number, from 1 */
	uint64_t pts_us;        /* capture time from the camera driver */
	uint64_t write_us;      /* CLOCK_MONOTONIC when the copy was complete */
} frametap_slot_t;

typedef struct {
	uint32_t magic, version;
	uint32_t width, height, stride;
	uint32_t slots, slot_bytes, data_offset;
	uint32_t latest;        /* newest complete slot */
	uint32_t writer_pid;
	uint32_t src_width, src_height, scale;  /* camera picture before scaling */
	uint32_t pad;
	frametap_slot_t slot[FRAMETAP_SLOTS];
} frametap_hdr_t;

static inline const uint8_t *frametap_pixels(const frametap_hdr_t *h, uint32_t i)
{
	return (const uint8_t *)h + h->data_offset + (size_t)i * h->slot_bytes;
}

static inline int frametap_valid(const frametap_hdr_t *h)
{
	return h->magic == FRAMETAP_MAGIC && h->version == FRAMETAP_VERSION &&
	       h->slots == FRAMETAP_SLOTS && h->stride >= h->width &&
	       h->slot_bytes >= h->stride * h->height;
}

/* Copies the newest picture (stride x height bytes) into dst if it is newer
 * than *last_frame. Returns 1 with *info filled, 0 if nothing new, -1 if the
 * writer kept overwriting it (try again later). */
static inline int frametap_read(const frametap_hdr_t *h, uint8_t *dst,
				uint64_t *last_frame, frametap_slot_t *info)
{
	for (int tries = 0; tries < 4; tries++) {
		uint32_t i = __atomic_load_n(&h->latest, __ATOMIC_ACQUIRE) % FRAMETAP_SLOTS;
		const frametap_slot_t *s = &h->slot[i];
		uint32_t q1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
		if (q1 & 1)
			continue;
		frametap_slot_t copy = *s;
		if (copy.frame == 0 || copy.frame <= *last_frame)
			return 0;
		FRAMETAP_MEMCPY(dst, frametap_pixels(h, i), (size_t)h->stride * h->height);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != q1)
			continue;       /* overwritten while we copied */
		*last_frame = copy.frame;
		if (info)
			*info = copy;
		return 1;
	}
	return -1;
}

/* ---- writer side (rkfpv; src/frametap.c) -------------------------------- */
typedef struct frametap frametap_t;

/* creates path with room for (src_w / scale) x (src_h / scale) pictures */
frametap_t *frametap_create(const char *path, int src_w, int src_h, int scale);
/* copies a luma plane, keeping every scale-th pixel of every scale-th row */
void frametap_put(frametap_t *t, const uint8_t *y, int src_stride,
		  uint64_t pts_us, uint64_t now_us);
uint64_t frametap_frames(const frametap_t *t);
void frametap_destroy(frametap_t *t);   /* also removes the file */

#endif
