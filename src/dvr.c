#include "dvr.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { ST_STOPPED, ST_WAIT_KEY, ST_REC, ST_FULL, ST_ERROR };
static const char *const ST_NAME[] = { "stopped", "waiting", "recording", "full", "error" };

#define REC_HDR   16                    /* u32 len, u32 key, i64 pts */
#define WRAP_MARK 0xFFFFFFFFu
#define US        1000000LL

struct dvr {
	dvr_config cfg;                 /* segment/min_free/overwrite/ts change live (lock) */
	dvr_writer w;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thr;
	int threaded, quit;

	/* ring of whole frames: producer = streaming loop, consumer = writer */
	uint8_t *ring;
	size_t cap, head, tail, used;

	/* producer: packs of one frame are gathered here (multi-pack frames) */
	uint8_t *stage;
	size_t stage_len, stage_cap;
	int stage_bad, drop_until_key;

	/* control (any thread) */
	volatile int want_rec;
	volatile int gen;               /* bumped by every dvr_start */
	volatile int want_idr;

	/* writer state */
	void *h;
	char cur[256];
	int seen_gen, need_session, session, seg;
	int64_t seg_start, last_flush, last_space, wait_since, err_at;
	volatile int state;
	volatile unsigned long long drops, bytes;
	volatile long long free_mb;
	char err[64];
};

static size_t align8(size_t n) { return (n + 7) & ~(size_t)7; }

/* ------------------------------------------------------------- helpers */

int dvr_is_keyframe(const uint8_t *p, size_t n)
{
	/* IRAP pictures (BLA/IDR/CRA, NAL types 16..21) start a decodable GOP */
	for (size_t i = 0; i + 3 < n; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			int t = (p[i + 3] >> 1) & 0x3f;
			if (t >= 16 && t <= 21)
				return 1;
			i += 2;
		}
	}
	return 0;
}

static int parse_name(const char *name, int *session, int *seg)
{
	char ext[8];
	int s, g, n = 0;
	if (sscanf(name, "rec_%d_%d.%7s%n", &s, &g, ext, &n) != 3 || name[n] != '\0')
		return 0;
	if (strcmp(ext, "mp4") && strcmp(ext, "ts"))
		return 0;
	*session = s;
	*seg = g;
	return 1;
}

int dvr_next_session(const char *dir)
{
	int max = 0, s, g;
	DIR *dp = opendir(dir);
	if (!dp)
		return 1;
	struct dirent *e;
	while ((e = readdir(dp)))
		if (parse_name(e->d_name, &s, &g) && s > max)
			max = s;
	closedir(dp);
	return max + 1;
}

/* oldest recording that is not the file being written */
static int find_oldest(const dvr_t *d, char *out, size_t n)
{
	int bs = 0, bg = 0, found = 0, s, g;
	const char *cur = strrchr(d->cur, '/');
	cur = cur ? cur + 1 : d->cur;
	DIR *dp = opendir(d->cfg.dir);
	if (!dp)
		return 0;
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (!parse_name(e->d_name, &s, &g))
			continue;
		if (d->h && !strcmp(e->d_name, cur))
			continue;
		if (!found || s < bs || (s == bs && g < bg)) {
			bs = s;
			bg = g;
			snprintf(out, n, "%s/%s", d->cfg.dir, e->d_name);
			found = 1;
		}
	}
	closedir(dp);
	return found;
}

/* make room: 0 = enough space, -1 = full */
static int ensure_space(dvr_t *d)
{
	if (!d->w.free_mb)
		return 0;
	long long f = d->w.free_mb(d->cfg.dir);
	d->free_mb = f;
	while (f >= 0 && f < d->cfg.min_free_mb) {
		char old[512];
		if (!d->cfg.overwrite || !find_oldest(d, old, sizeof old))
			return -1;
		if (unlink(old) < 0)
			return -1;
		fprintf(stderr, "dvr: low space (%lld MB), deleted %s\n", f, old);
		f = d->w.free_mb(d->cfg.dir);
		d->free_mb = f;
	}
	return 0;
}

static void close_file(dvr_t *d)
{
	if (!d->h)
		return;
	d->w.close(d->h);
	d->h = NULL;
	fprintf(stderr, "dvr: closed %s\n", d->cur);
}

static void set_error(dvr_t *d, const char *msg, int64_t pts)
{
	close_file(d);
	snprintf(d->err, sizeof d->err, "%s", msg);
	d->state = ST_ERROR;
	d->err_at = pts;
	fprintf(stderr, "dvr: %s\n", msg);
}

static int open_segment(dvr_t *d, int64_t pts)
{
	if (d->need_session) {
		d->session = dvr_next_session(d->cfg.dir);
		d->seg = 0;
		d->need_session = 0;
	}
	if (ensure_space(d) < 0) {
		d->state = ST_FULL;
		fprintf(stderr, "dvr: card full (%lld MB free), recording stopped\n", (long long)d->free_mb);
		return -1;
	}
	dvr_stream_info info = d->cfg.stream;
	info.ts = d->cfg.ts;
	snprintf(d->cur, sizeof d->cur, "%s/rec_%04d_%02d.%s", d->cfg.dir, d->session, d->seg,
		 info.ts ? "ts" : "mp4");
	d->h = d->w.open(d->cur, &info);
	if (!d->h) {
		set_error(d, "cannot create file", pts);
		return -1;
	}
	fprintf(stderr, "dvr: recording %s\n", d->cur);
	d->seg_start = d->last_flush = d->last_space = pts;
	d->wait_since = -1;
	d->state = ST_REC;
	d->err[0] = '\0';
	return 0;
}

/* ------------------------------------------------------------- writer */

static void handle_control(dvr_t *d)
{
	if (!d->want_rec) {
		if (d->h)
			close_file(d);
		d->state = ST_STOPPED;
		return;
	}
	if (d->gen != d->seen_gen) {            /* a new start: new session */
		d->seen_gen = d->gen;
		close_file(d);
		d->need_session = 1;
		d->wait_since = -1;
		d->state = ST_WAIT_KEY;
	}
}

static void handle_frame(dvr_t *d, const uint8_t *data, size_t len, int64_t pts, int key)
{
	if (!d->want_rec || d->state == ST_STOPPED)
		return;
	if (d->state == ST_FULL)
		return;                         /* until the next dvr_start */
	if (d->state == ST_ERROR && pts - d->err_at < 5 * US)
		return;                         /* retry a failed card every 5 s */

	if (!d->h) {
		if (!key) {
			if (d->wait_since < 0)
				d->wait_since = pts;
			else if (pts - d->wait_since > 3 * US) {
				d->want_idr = 1;
				d->wait_since = pts;
			}
			if (d->state != ST_ERROR)
				d->state = ST_WAIT_KEY;
			return;
		}
		if (open_segment(d, pts) < 0)
			return;
	} else if (pts - d->seg_start >= (int64_t)d->cfg.segment_s * US) {
		if (key) {
			close_file(d);
			d->seg++;
			if (open_segment(d, pts) < 0)
				return;
		} else if (pts - d->seg_start >= ((int64_t)d->cfg.segment_s + 3) * US && !d->want_idr) {
			d->want_idr = 1;            /* no keyframe for a while (intra refresh) */
			d->seg_start += 3 * US;     /* ask again in 3 s if needed */
		}
	}

	if (d->w.write(d->h, data, len, pts, key) < 0) {
		set_error(d, "write failed (card full or removed?)", pts);
		return;
	}
	d->bytes += len;

	if (pts - d->last_flush >= US) {
		d->w.flush(d->h);
		d->last_flush = pts;
	}
	if (pts - d->last_space >= 10 * US) {
		d->last_space = pts;
		if (ensure_space(d) < 0) {
			close_file(d);
			d->state = ST_FULL;
			fprintf(stderr, "dvr: card full, recording stopped\n");
		}
	}
}

void dvr_process(dvr_t *d)
{
	handle_control(d);
	for (;;) {
		pthread_mutex_lock(&d->lock);
		if (d->used == 0) {
			pthread_mutex_unlock(&d->lock);
			break;
		}
		uint32_t len, key;
		int64_t pts;
		if (d->cap - d->tail < REC_HDR ||
		    (memcpy(&len, d->ring + d->tail, 4), len == WRAP_MARK)) {
			d->used -= d->cap - d->tail;
			d->tail = 0;
			pthread_mutex_unlock(&d->lock);
			continue;
		}
		memcpy(&key, d->ring + d->tail + 4, 4);
		memcpy(&pts, d->ring + d->tail + 8, 8);
		const uint8_t *p = d->ring + d->tail + REC_HDR;
		size_t need = REC_HDR + align8(len);
		pthread_mutex_unlock(&d->lock);

		/* the producer does not touch this record until `used` drops */
		handle_control(d);
		handle_frame(d, p, len, pts, (int)key);

		pthread_mutex_lock(&d->lock);
		d->tail += need;
		if (d->tail >= d->cap)
			d->tail = 0;
		d->used -= need;
		pthread_mutex_unlock(&d->lock);
	}
	handle_control(d);
}

static void *writer_thread(void *arg)
{
	dvr_t *d = arg;
	for (;;) {
		pthread_mutex_lock(&d->lock);
		if (d->used == 0 && !d->quit) {
			struct timespec t;
			clock_gettime(CLOCK_REALTIME, &t);
			t.tv_nsec += 200 * 1000000L;
			if (t.tv_nsec >= 1000000000L) {
				t.tv_sec++;
				t.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&d->cond, &d->lock, &t);
		}
		int quit = d->quit;
		pthread_mutex_unlock(&d->lock);
		dvr_process(d);
		if (quit)
			break;
	}
	return NULL;
}

/* ------------------------------------------------------------- producer */

/* copy one whole frame into the ring; 0 = queued, -1 = no room */
static int ring_put(dvr_t *d, const uint8_t *data, size_t len, int64_t pts, int key)
{
	size_t need = REC_HDR + align8(len);
	int ret = -1;
	pthread_mutex_lock(&d->lock);
	if (need <= d->cap) {
		size_t skip = 0;
		if (d->cap - d->head < need)
			skip = d->cap - d->head;        /* record must be contiguous */
		if (d->used + skip + need <= d->cap) {
			if (skip) {
				if (skip >= REC_HDR) {
					uint32_t m = WRAP_MARK;
					memcpy(d->ring + d->head, &m, 4);
				}
				d->used += skip;
				d->head = 0;
			}
			uint32_t l = (uint32_t)len, k = (uint32_t)key;
			memcpy(d->ring + d->head, &l, 4);
			memcpy(d->ring + d->head + 4, &k, 4);
			memcpy(d->ring + d->head + 8, &pts, 8);
			memcpy(d->ring + d->head + REC_HDR, data, len);
			d->head += need;
			if (d->head >= d->cap)
				d->head = 0;
			d->used += need;
			ret = 0;
			pthread_cond_signal(&d->cond);
		}
	}
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static void push_frame(dvr_t *d, const uint8_t *data, size_t len, int64_t pts)
{
	int key = dvr_is_keyframe(data, len);
	if (!d->want_rec)
		return;
	if (d->drop_until_key && !key) {
		d->drops++;
		return;
	}
	if (ring_put(d, data, len, pts, key) < 0) {
		/* card too slow: drop up to the next keyframe so files stay decodable */
		d->drops++;
		d->drop_until_key = 1;
		return;
	}
	d->drop_until_key = 0;
}

void dvr_push(dvr_t *d, const uint8_t *data, size_t len, uint64_t pts_us, int frame_end)
{
	if (!d)
		return;
	if (d->stage_len == 0 && frame_end) {           /* one pack = one frame */
		push_frame(d, data, len, (int64_t)pts_us);
		return;
	}
	if (!d->stage_bad) {
		if (d->stage_len + len > d->stage_cap) {
			size_t nc = d->stage_cap ? d->stage_cap : 65536;
			while (nc < d->stage_len + len)
				nc *= 2;
			uint8_t *b = nc <= d->cap ? realloc(d->stage, nc) : NULL;
			if (!b)
				d->stage_bad = 1;
			else {
				d->stage = b;
				d->stage_cap = nc;
			}
		}
		if (!d->stage_bad) {
			memcpy(d->stage + d->stage_len, data, len);
			d->stage_len += len;
		}
	}
	if (frame_end) {
		if (d->stage_bad) {
			d->drops++;
			d->drop_until_key = 1;
		} else
			push_frame(d, d->stage, d->stage_len, (int64_t)pts_us);
		d->stage_len = 0;
		d->stage_bad = 0;
	}
}

/* ------------------------------------------------------------- control */

dvr_t *dvr_create(const dvr_config *cfg, const dvr_writer *w, int threaded)
{
	dvr_t *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	d->cfg = *cfg;
	d->w = *w;
	d->cap = cfg->ring_bytes & ~(size_t)7;
	d->ring = malloc(d->cap);
	if (!d->ring) {
		free(d);
		return NULL;
	}
	d->state = ST_STOPPED;
	d->wait_since = -1;
	d->free_mb = -1;
	pthread_mutex_init(&d->lock, NULL);
	pthread_cond_init(&d->cond, NULL);
	d->threaded = threaded;
	if (threaded && pthread_create(&d->thr, NULL, writer_thread, d) != 0) {
		free(d->ring);
		free(d);
		return NULL;
	}
	return d;
}

void dvr_destroy(dvr_t *d)
{
	if (!d)
		return;
	if (d->threaded) {
		pthread_mutex_lock(&d->lock);
		d->quit = 1;
		pthread_cond_signal(&d->cond);
		pthread_mutex_unlock(&d->lock);
		pthread_join(d->thr, NULL);     /* drains the ring first */
	} else
		dvr_process(d);
	close_file(d);
	pthread_mutex_destroy(&d->lock);
	pthread_cond_destroy(&d->cond);
	free(d->stage);
	free(d->ring);
	free(d);
}

void dvr_start(dvr_t *d)
{
	if (!d)
		return;
	pthread_mutex_lock(&d->lock);
	d->gen++;
	d->want_rec = 1;
	d->drop_until_key = 1;          /* a file must begin with a keyframe */
	pthread_cond_signal(&d->cond);
	pthread_mutex_unlock(&d->lock);
}

void dvr_stop(dvr_t *d)
{
	if (!d)
		return;
	pthread_mutex_lock(&d->lock);
	d->want_rec = 0;
	pthread_cond_signal(&d->cond);
	pthread_mutex_unlock(&d->lock);
}

void dvr_reconfig(dvr_t *d, int segment_s, int min_free_mb, int overwrite, int ts)
{
	if (!d)
		return;
	pthread_mutex_lock(&d->lock);
	d->cfg.segment_s = segment_s;
	d->cfg.min_free_mb = min_free_mb;
	d->cfg.overwrite = overwrite;
	d->cfg.ts = ts;                 /* used from the next file on */
	pthread_mutex_unlock(&d->lock);
}

int dvr_want_idr(dvr_t *d)
{
	if (!d || !d->want_idr)
		return 0;
	d->want_idr = 0;
	return 1;
}

void dvr_status(dvr_t *d, char *buf, size_t n)
{
	if (!d) {
		snprintf(buf, n, "state=off");
		return;
	}
	const char *cur = strrchr(d->cur, '/');
	cur = cur ? cur + 1 : d->cur;
	snprintf(buf, n, "state=%s file=%s bytes=%llu drops=%llu free_mb=%lld%s%s",
		 ST_NAME[d->state], d->h ? cur : "-", d->bytes, d->drops, d->free_mb,
		 d->err[0] ? " error=" : "", d->err[0] ? d->err : "");
	/* spaces in the error text would break key=value parsing */
	for (char *p = strstr(buf, "error="); p && *p; p++)
		if (*p == ' ')
			*p = '_';
}
