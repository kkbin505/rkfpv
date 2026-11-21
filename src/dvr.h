/*
 * DVR: record the encoded H.265 stream to files on the TF card.
 *
 * The streaming loop hands every encoder pack to dvr_push(); whole frames go
 * through a ring buffer to a writer thread, so the radio stream never waits
 * on the card. The container is done by a dvr_writer (librkmuxer on the
 * board: fragmented MP4 or TS; a fake in the host tests).
 *
 * Files: <dir>/rec_SSSS_NN.<ext>, SSSS = session (one per start), NN =
 * segment. A new segment starts at the first keyframe after `segment_s`.
 * Segment timing uses the frames' own timestamps, not the wall clock.
 */
#ifndef DVR_H
#define DVR_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	int width, height, fps, bitrate_kbps;
	int ts;                         /* 1: MPEG-TS, 0: fragmented MP4 */
} dvr_stream_info;

typedef struct {
	/* open a new file, NULL on failure */
	void *(*open)(const char *path, const dvr_stream_info *info);
	/* one whole frame; < 0 on error */
	int (*write)(void *h, const uint8_t *frame, size_t len, int64_t pts_us, int key);
	/* push written data to the card (called about once per second) */
	void (*flush)(void *h);
	void (*close)(void *h);
	/* free space of the DVR directory in MB, < 0 if unknown */
	long long (*free_mb)(const char *dir);
} dvr_writer;

typedef struct {
	char dir[128];
	int segment_s;                  /* new file after this many seconds */
	int min_free_mb;                /* keep at least this much free */
	int overwrite;                  /* 1: delete oldest files to make room */
	int ts;                         /* 1: .ts, 0: .mp4 */
	size_t ring_bytes;
	dvr_stream_info stream;
} dvr_config;

typedef struct dvr dvr_t;

/* threaded = 0: no writer thread, the caller runs dvr_process() (tests) */
dvr_t *dvr_create(const dvr_config *cfg, const dvr_writer *w, int threaded);
void dvr_destroy(dvr_t *d);             /* closes the current file cleanly */

/* producer side (streaming loop); never blocks on the card */
void dvr_push(dvr_t *d, const uint8_t *data, size_t len, uint64_t pts_us, int frame_end);

void dvr_start(dvr_t *d);               /* begin a new session at the next keyframe */
void dvr_stop(dvr_t *d);
void dvr_reconfig(dvr_t *d, int segment_s, int min_free_mb, int overwrite, int ts);

/* 1 if the writer is waiting for a keyframe for too long: request an IDR */
int dvr_want_idr(dvr_t *d);

/* writer side: handle queued frames (the thread calls this itself) */
void dvr_process(dvr_t *d);

/* one line of key=value pairs for /tmp/dvr.status and the stats line */
void dvr_status(dvr_t *d, char *buf, size_t n);

/* helpers, exposed for tests */
int dvr_is_keyframe(const uint8_t *data, size_t len);
int dvr_next_session(const char *dir);

#endif
