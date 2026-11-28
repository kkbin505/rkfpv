/* Host test for the frame tap: a writer thread fills frames as fast as it
 * can while the reader copies them; every copy must be one whole frame
 * (no mix of two), frame numbers must rise, the scaling must pick the
 * right source pixels. Then ftdump reads from a live writer.
 *
 *   test_frametap <ftdump binary>
 */
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* a reader that copies slowly: half, pause, half. The writer finishes
 * frames meanwhile, so reads and writes of the same slot overlap. */
#include <stddef.h>
#include <string.h>
#include <unistd.h>
static void *slow_copy(void *d, const void *s, size_t n)
{
	memcpy(d, s, n / 2);
	usleep(400);
	memcpy((char *)d + n / 2, (const char *)s + n / 2, n - n / 2);
	return d;
}
#define FRAMETAP_MEMCPY slow_copy
#include "../src/frametap.h"

#define SW 1280
#define SH 720

static int fails;
static void check(const char *name, int ok)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", name);
	fails += !ok;
}

/* source pixel of frame n: differs between neighbouring frames, rows and
 * columns (4 patterns, so the writer can publish them back to back) */
static uint8_t src_px(uint64_t n, int x, int y) { return (uint8_t)((n % 4) * 61 + x + 3 * y); }

static void fill(uint8_t *y, int stride, uint64_t n)
{
	for (int r = 0; r < SH; r++)
		for (int c = 0; c < SW; c++)
			y[r * stride + c] = src_px(n, c, r);
}

struct writer { frametap_t *t; volatile int stop; int sleep_us; uint64_t n; };

static void *writer(void *arg)
{
	struct writer *w = arg;
	int stride = SW + 64;   /* camera rows are padded */
	uint8_t *y[4];
	for (int k = 0; k < 4; k++) {
		y[k] = malloc((size_t)stride * SH);
		fill(y[k], stride, k);
	}
	while (!w->stop) {
		w->n++;
		frametap_put(w->t, y[w->n % 4], stride, w->n * 1000, w->n * 1000 + 5);
		if (w->sleep_us)
			usleep(w->sleep_us);
	}
	for (int k = 0; k < 4; k++)
		free(y[k]);
	return NULL;
}

static const frametap_hdr_t *map(const char *p, size_t *sz)
{
	int fd = open(p, O_RDONLY);
	struct stat st;
	fstat(fd, &st);
	*sz = st.st_size;
	void *m = mmap(NULL, *sz, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	return m;
}

static void run(const char *path, int scale)
{
	char name[96];
	frametap_t *t = frametap_create(path, SW, SH, scale);
	snprintf(name, sizeof name, "scale %d: created", scale);
	check(name, t != NULL);
	if (!t)
		return;
	size_t sz;
	const frametap_hdr_t *h = map(path, &sz);
	snprintf(name, sizeof name, "scale %d: header %ux%u stride %u", scale, h->width, h->height, h->stride);
	check(name, frametap_valid(h) && h->width == (uint32_t)SW / scale &&
	      h->height == (uint32_t)SH / scale && h->stride % 16 == 0);
	uint64_t last = 0;
	frametap_slot_t info;
	uint8_t *pic = malloc((size_t)h->stride * h->height);
	check("empty tap: nothing to read", frametap_read(h, pic, &last, &info) == 0);

	/* a frame every ~0.3 ms against a 0.4 ms read: some reads overlap */
	struct writer w = { t, 0, 300, 0 };
	pthread_t th;
	pthread_create(&th, NULL, writer, &w);
	int reads = 0, torn = 0, bad = 0, order = 0, retry = 0;
	uint64_t prev = 0;
	time_t give_up = time(NULL) + 20;
	while (reads < 300 && time(NULL) < give_up) {
		int r = frametap_read(h, pic, &last, &info);
		if (r < 0) { retry++; continue; }
		if (r == 0) continue;
		reads++;
		if (info.frame <= prev) order++;
		prev = info.frame;
		if (info.pts_us != info.frame * 1000) bad++;
		/* whole picture from one frame, scaled from the right pixels */
		for (uint32_t j = 0; j < h->height && !torn; j += 7)
			for (uint32_t i = 0; i < h->width; i += 3)
				if (pic[j * h->stride + i] != src_px(info.frame, i * scale, j * scale)) {
					torn++;
					break;
				}
	}
	/* control: the same reads without the sequence check do get torn
	 * pictures, so the test above really raced the writer */
	int unsafe = 0;
	for (int k = 0; k < 1000; k++) {
		uint32_t i = __atomic_load_n(&h->latest, __ATOMIC_ACQUIRE);
		uint64_t f = h->slot[i].frame;
		slow_copy(pic, frametap_pixels(h, i), (size_t)h->stride * h->height);
		for (uint32_t j = 0; j < h->height; j += h->height - 1)
			for (uint32_t c = 0; c < h->width; c += 3)
				if (pic[j * h->stride + c] != src_px(f, c * scale, j * scale)) {
					unsafe++;
					break;
				}
	}
	/* asserted at full size only, where a read is long enough to be hit
	 * reliably; the seqlock code is the same for every scale */
	snprintf(name, sizeof name, "scale %d: control, %d of 1000 unchecked reads torn", scale, unsafe);
	if (scale == 1)
		check(name, unsafe > 0);
	else
		printf("info: %s\n", name);
	w.stop = 1;
	pthread_join(th, NULL);
	snprintf(name, sizeof name, "scale %d: %d reads racing the writer, none torn (%d gave up)", scale, reads, retry);
	check(name, reads == 300 && torn == 0);
	check("frame numbers rise, timestamps match", order == 0 && bad == 0);
	check("frame count", frametap_frames(t) == w.n);
	free(pic);
	munmap((void *)h, sz);
	frametap_destroy(t);
	check("file removed", access(path, F_OK) != 0);
}

int main(int argc, char **argv)
{
	char path[64];
	snprintf(path, sizeof path, "/tmp/test-frametap-%d", (int)getpid());
	run(path, 1);
	run(path, 2);
	run(path, 4);
	check("bad scale refused", frametap_create(path, SW, SH, 0) == NULL);

	if (argc > 1) {         /* ftdump against a writer at ~100 fps */
		frametap_t *t = frametap_create(path, SW, SH, 2);
		struct writer w = { t, 0, 10000, 0 };
		pthread_t th;
		pthread_create(&th, NULL, writer, &w);
		char cmd[256], pgm[80];
		snprintf(pgm, sizeof pgm, "%s.pgm", path);
		snprintf(cmd, sizeof cmd, "%s -p %s -n 20 -o %s > /dev/null", argv[1], path, pgm);
		int rc = system(cmd);
		w.stop = 1;
		pthread_join(th, NULL);
		frametap_destroy(t);
		struct stat st;
		check("ftdump reads 20 frames and saves a PGM",
		      rc == 0 && stat(pgm, &st) == 0 && st.st_size == (off_t)(640 * 360 + 15));
		unlink(pgm);
		snprintf(cmd, sizeof cmd, "%s -p %s -t 1 2> /dev/null", argv[1], path);
		check("ftdump fails cleanly without a tap", system(cmd) != 0);
	}
	printf(fails ? "%d frametap test(s) FAILED\n" : "all frametap tests passed\n", fails);
	return fails != 0;
}
