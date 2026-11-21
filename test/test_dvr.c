/* Host tests for src/dvr.c with a fake writer (no rkmuxer on the PC). */
#include "../src/dvr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[256];

/* ---- fake writer: each file gets the raw frames; a log of what arrived */
#define MAXLOG 4096
static struct { char file[64]; int key; int idx; size_t len; } wlog[MAXLOG];
static int nlog, opens, closes, flushes;
static long long cap_kb = 1 << 30;      /* fake card size; "MB" = KB here */

struct fh { FILE *f; char name[64]; };

static void *f_open(const char *path, const dvr_stream_info *info)
{
	(void)info;
	struct fh *h = calloc(1, sizeof *h);
	h->f = fopen(path, "wb");
	const char *b = strrchr(path, '/');
	snprintf(h->name, sizeof h->name, "%s", b ? b + 1 : path);
	opens++;
	return h;
}

static int frame_idx(const uint8_t *p, size_t n) { int v; memcpy(&v, p + n - 4, 4); return v; }

static int f_write(void *hp, const uint8_t *frame, size_t len, int64_t pts, int key)
{
	(void)pts;
	struct fh *h = hp;
	fwrite(frame, 1, len, h->f);
	fflush(h->f);
	if (nlog < MAXLOG) {
		snprintf(wlog[nlog].file, sizeof wlog[nlog].file, "%s", h->name);
		wlog[nlog].key = key;
		wlog[nlog].idx = frame_idx(frame, len);
		wlog[nlog].len = len;
		nlog++;
	}
	return 0;
}
static void f_flush(void *h) { (void)h; flushes++; }
static void f_close(void *hp) { struct fh *h = hp; fclose(h->f); free(h); closes++; }

static long long dir_kb(void)
{
	long long t = 0;
	DIR *dp = opendir(dir);
	struct dirent *e;
	char p[512];
	struct stat st;
	while ((e = readdir(dp))) {
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (!stat(p, &st) && S_ISREG(st.st_mode))
			t += st.st_size;
	}
	closedir(dp);
	return t / 1024;
}
static long long f_free(const char *d) { (void)d; long long f = cap_kb - dir_kb(); return f < 0 ? 0 : f; }

static const dvr_writer W = { f_open, f_write, f_flush, f_close, f_free };

/* ---- synthetic H.265 frames: [start code + NAL header]... + 4-byte index */
static size_t make_frame(uint8_t *b, int key, int idx, size_t payload)
{
	size_t n = 0;
	static const uint8_t vps[] = { 0, 0, 0, 1, 32 << 1, 1, 0xAA };
	static const uint8_t idr[] = { 0, 0, 0, 1, 19 << 1, 1 };
	static const uint8_t p[] = { 0, 0, 0, 1, 1 << 1, 1 };
	if (key) { memcpy(b, vps, sizeof vps); n += sizeof vps; memcpy(b + n, idr, sizeof idr); n += sizeof idr; }
	else { memcpy(b, p, sizeof p); n += sizeof p; }
	memset(b + n, 0x55, payload);
	n += payload;
	memcpy(b + n, &idx, 4);
	return n + 4;
}

static uint8_t buf[1 << 20];
static int frame_no;
static int64_t now_us;

/* push `secs` seconds of 30 fps video, keyframe every `gop` frames */
static void feed(dvr_t *d, double secs, int gop, size_t payload, int process)
{
	int n = (int)(secs * 30 + 0.5);
	for (int i = 0; i < n; i++) {
		size_t len = make_frame(buf, gop && frame_no % gop == 0, frame_no, payload);
		dvr_push(d, buf, len, (uint64_t)now_us, 1);
		frame_no++;
		now_us += 33334;
		if (process)
			dvr_process(d);
	}
}

static int count_files(void)
{
	int n = 0;
	DIR *dp = opendir(dir);
	struct dirent *e;
	while ((e = readdir(dp)))
		if (!strncmp(e->d_name, "rec_", 4))
			n++;
	closedir(dp);
	return n;
}

static int exists(const char *name)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return access(p, F_OK) == 0;
}

static void reset(void)
{
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -f %s/rec_*", dir);
	if (system(cmd)) {}
	nlog = opens = closes = flushes = 0;
	frame_no = 0;
	now_us = 1000000;
	cap_kb = 1 << 30;
}

static dvr_config cfg(int seg)
{
	dvr_config c;
	memset(&c, 0, sizeof c);
	snprintf(c.dir, sizeof c.dir, "%s", dir);
	c.segment_s = seg;
	c.min_free_mb = 10;
	c.overwrite = 1;
	c.ring_bytes = 256 * 1024;
	c.stream = (dvr_stream_info){ 1280, 720, 30, 3500, 0 };
	return c;
}

int main(void)
{
	snprintf(dir, sizeof dir, "/tmp/test_dvr_%d", (int)getpid());
	mkdir(dir, 0755);

	/* keyframe detection */
	size_t l = make_frame(buf, 1, 0, 10);
	CHECK(dvr_is_keyframe(buf, l), "VPS+IDR frame is key");
	l = make_frame(buf, 0, 0, 10);
	CHECK(!dvr_is_keyframe(buf, l), "P frame is not key");
	uint8_t cra[] = { 0, 0, 1, 21 << 1, 1, 9 };
	CHECK(dvr_is_keyframe(cra, sizeof cra), "CRA (3-byte start code) is key");

	/* 1. nothing recorded before start */
	reset();
	dvr_config c = cfg(2);
	dvr_t *d = dvr_create(&c, &W, 0);
	feed(d, 2, 30, 100, 1);
	CHECK(count_files() == 0 && nlog == 0, "no recording before start");

	/* 2./3. start mid-GOP: file begins at a keyframe; 2 s segments roll on keys */
	frame_no = 5;                           /* not at a keyframe */
	dvr_start(d);
	feed(d, 7, 30, 100, 1);
	dvr_stop(d);
	dvr_process(d);
	CHECK(exists("rec_0001_00.mp4") && exists("rec_0001_01.mp4") && exists("rec_0001_02.mp4"),
	      "segments 00..02 exist");
	CHECK(count_files() == 4, "4 files: keys at 1, 3, 5, 7 s start segments, got %d", count_files());
	int bad = 0, prev_idx = -1;
	char last[64] = "";
	for (int i = 0; i < nlog; i++) {
		if (strcmp(wlog[i].file, last)) {
			if (!wlog[i].key) bad++;        /* every file starts with a keyframe */
			snprintf(last, sizeof last, "%s", wlog[i].file);
		}
		if (prev_idx >= 0 && wlog[i].idx != prev_idx + 1) bad++;   /* in order, no gaps */
		prev_idx = wlog[i].idx;
	}
	CHECK(bad == 0, "files start on keyframes, frames in order (%d problems)", bad);
	CHECK(nlog > 0 && wlog[0].idx == 30, "first written frame is the first keyframe (got %d)", nlog ? wlog[0].idx : -1);
	CHECK(opens == 4 && closes == 4, "open/close balanced (%d/%d)", opens, closes);
	CHECK(flushes >= 3, "flushed inside every 2 s file (%d)", flushes);

	/* 5. stop -> start = new session */
	dvr_start(d);
	feed(d, 1.5, 30, 100, 1);
	dvr_stop(d);
	dvr_process(d);
	CHECK(exists("rec_0002_00.mp4"), "second start opens session 2");
	CHECK(dvr_next_session(dir) == 3, "next session = 3");

	/* 4. multi-pack frames arrive whole */
	reset();
	dvr_start(d);
	for (int i = 0; i < 40; i++) {
		size_t n = make_frame(buf, i % 30 == 0, i, 3000);
		dvr_push(d, buf, 1000, (uint64_t)now_us, 0);
		dvr_push(d, buf + 1000, 1000, (uint64_t)now_us, 0);
		dvr_push(d, buf + 2000, n - 2000, (uint64_t)now_us, 1);
		now_us += 33334;
		dvr_process(d);
	}
	dvr_stop(d);
	dvr_process(d);
	CHECK(nlog == 40 && wlog[0].len == make_frame(buf, 1, 0, 3000) && wlog[1].idx == 1,
	      "3-pack frames reassembled (n=%d)", nlog);

	/* ring overflow (writer stalled): drop up to the next keyframe */
	reset();
	dvr_start(d);
	feed(d, 3, 30, 20000, 0);               /* ~1.8 MB into a 256 KB ring, nothing processed */
	char st[256];
	dvr_status(d, st, sizeof st);
	CHECK(strstr(st, "drops=0") == NULL, "drops counted: %s", st);
	dvr_process(d);
	feed(d, 2, 30, 20000, 1);
	dvr_stop(d);
	dvr_process(d);
	bad = 0;
	for (int i = 1; i < nlog; i++)
		if (wlog[i].idx != wlog[i - 1].idx + 1 && !wlog[i].key)
			bad++;                          /* after a gap, writing resumes on a key */
	CHECK(bad == 0 && nlog > 0, "after drops the stream resumes on a keyframe (%d bad)", bad);

	/* space: overwrite deletes the oldest file, never the current one */
	reset();
	dvr_destroy(d);
	c = cfg(1);
	c.min_free_mb = 100;
	d = dvr_create(&c, &W, 0);
	cap_kb = 100 + 3 * 300;                 /* room for ~3 one-second files of ~300 KB */
	dvr_start(d);
	feed(d, 12, 30, 10000, 1);
	dvr_stop(d);
	dvr_process(d);
	CHECK(!exists("rec_0001_00.mp4") && exists("rec_0001_10.mp4"), "oldest deleted, newest kept");
	CHECK(count_files() <= 4, "card kept within capacity (%d files)", count_files());

	/* space: overwrite off -> full, recording stops */
	reset();
	dvr_destroy(d);
	c.overwrite = 0;
	d = dvr_create(&c, &W, 0);
	cap_kb = 100 + 3 * 300;
	dvr_start(d);
	feed(d, 12, 30, 10000, 1);
	dvr_status(d, st, sizeof st);
	CHECK(strstr(st, "state=full") != NULL, "full without overwrite: %s", st);
	CHECK(count_files() <= 4 && exists("rec_0001_00.mp4"), "nothing deleted without overwrite");
	dvr_stop(d);
	dvr_process(d);

	/* no keyframe for segment + 3 s (intra refresh): ask for an IDR */
	reset();
	dvr_destroy(d);
	c = cfg(2);
	d = dvr_create(&c, &W, 0);
	dvr_start(d);
	feed(d, 0.1, 30, 100, 1);               /* first key at frame 0 */
	feed(d, 6, 0, 100, 1);                  /* then P frames only */
	CHECK(dvr_want_idr(d) == 1, "IDR requested when no keyframe arrives");
	CHECK(dvr_want_idr(d) == 0, "request flag is cleared once read");
	dvr_stop(d);
	dvr_process(d);
	dvr_destroy(d);

	/* threaded mode: everything written by destroy */
	reset();
	c = cfg(600);
	d = dvr_create(&c, &W, 1);
	dvr_start(d);
	feed(d, 3, 30, 1000, 0);
	usleep(300000);
	dvr_destroy(d);
	CHECK(nlog == 90 && closes == 1, "threaded: all 90 frames written and file closed (%d, %d)", nlog, closes);

	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd)) {}
	printf("%d/%d passed\n", passes, passes + fails);
	return fails ? 1 : 0;
}
