/*
 * ftdump: reads rkfpv's frame tap (frametap.h) and shows what arrives.
 * Also the smallest example of a reader.
 *
 *   ftdump [-p /dev/shm/rkfpv-frame] [-n frames] [-o picture.pgm] [-t seconds]
 *
 * Prints size, rate and delay (camera timestamp -> copy done -> read) once
 * a second; -o saves the last picture read as PGM (any image viewer).
 *
 * rkfpv creates a new tap file each time the video starts. After 2 s
 * without a new frame ftdump maps the file again (once a second) and goes
 * on with the new one; it gives up after 10 s without frames. A reader
 * that runs for the whole flight should do the same.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "frametap.h"

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* maps the tap; NULL while rkfpv has not created it (yet). *ino tells one
 * tap file from the next: rkfpv creates a new file at every video start. */
static const frametap_hdr_t *map_tap(const char *path, size_t *size, ino_t *ino)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	void *m = MAP_FAILED;
	if (fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(frametap_hdr_t))
		m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return NULL;
	const frametap_hdr_t *h = m;
	if (!frametap_valid(h) ||
	    (size_t)st.st_size < h->data_offset + (size_t)h->slot_bytes * h->slots) {
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	*size = (size_t)st.st_size;
	*ino = st.st_ino;
	return h;
}

int main(int argc, char **argv)
{
	const char *path = FRAMETAP_PATH, *out = NULL;
	long want = 0;
	int secs = 0, opt;
	while ((opt = getopt(argc, argv, "p:n:o:t:h")) != -1) {
		switch (opt) {
		case 'p': path = optarg; break;
		case 'n': want = atol(optarg); break;
		case 'o': out = optarg; break;
		case 't': secs = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-p %s] [-n frames] [-o out.pgm] [-t seconds]\n",
				argv[0], FRAMETAP_PATH);
			return opt == 'h' ? 0 : 1;
		}
	}
	uint64_t start = now_us(), deadline = secs ? start + (uint64_t)secs * 1000000u : 0;
	size_t size = 0;
	ino_t ino = 0;
	const frametap_hdr_t *h = NULL;
	while (!(h = map_tap(path, &size, &ino))) {
		if (deadline ? now_us() > deadline : now_us() - start > 3000000u) {
			fprintf(stderr, "ftdump: no frame tap at %s (frame_tap = 1? rkfpv running?)\n", path);
			return 1;
		}
		usleep(100000);
	}
	printf("ftdump: %ux%u (camera %ux%u / %u), writer pid %u\n", h->width, h->height,
	       h->src_width, h->src_height, h->scale, h->writer_pid);
	size_t pic_bytes = (size_t)h->stride * h->height;
	uint8_t *pic = malloc(pic_bytes);
	if (!pic)
		return 1;
	/* the picture -o saves: its own size, a reopened tap may differ */
	uint32_t pic_w = h->width, pic_h = h->height, pic_stride = h->stride;

	uint64_t last = 0, got = 0, torn = 0, t_rep = now_us(), n_rep = 0, t_new = now_us();
	uint64_t t_try = 0, reopened = 0;
	uint64_t lat_cap = 0, lat_read = 0, first_frame = 0;
	frametap_slot_t info;
	for (;;) {
		int r = frametap_read(h, pic, &last, &info);
		uint64_t now = now_us();
		if (r > 0) {
			pic_w = h->width; pic_h = h->height; pic_stride = h->stride;
			if (!first_frame)
				first_frame = info.frame;
			got++;
			n_rep++;
			t_new = now;
			lat_read += now - info.write_us;
			if (info.pts_us && info.pts_us <= now)
				lat_cap += now - info.pts_us;
		} else if (r < 0)
			torn++;
		if (now - t_rep >= 1000000u && n_rep) {
			printf("frame %llu: %.1f fps, camera->read %.1f ms, copy->read %.2f ms%s\n",
			       (unsigned long long)last, n_rep * 1e6 / (now - t_rep),
			       lat_cap / 1000.0 / n_rep, lat_read / 1000.0 / n_rep,
			       torn ? " (writer overran a read)" : "");
			fflush(stdout);
			t_rep = now; n_rep = 0; lat_cap = 0; lat_read = 0;
		}
		if ((want && (long)got >= want) || (deadline && now > deadline))
			break;
		if (now - t_new > 10000000u) {
			fprintf(stderr, "ftdump: no new frame for 10 s (rkfpv stopped?)\n");
			break;
		}
		/* rkfpv restarted (new file) or stopped: map the tap again */
		if (now - t_new > 2000000u && now - t_try > 1000000u) {
			t_try = now;
			size_t nsize = 0;
			ino_t nino = 0;
			const frametap_hdr_t *n = map_tap(path, &nsize, &nino);
			if (n && nino == ino) {
				munmap((void *)n, nsize);          /* still the same file */
			} else if (n) {
				/* the buffer only grows: it still holds the last picture
				 * (pic_w x pic_h, pic_stride) for -o until a new frame comes */
				size_t need = (size_t)n->stride * n->height;
				uint8_t *np = need > pic_bytes ? realloc(pic, need) : pic;
				if (!np) {
					munmap((void *)n, nsize);
				} else {
					pic = np;
					if (need > pic_bytes)
						pic_bytes = need;
					munmap((void *)h, size);
					h = n;
					size = nsize;
					ino = nino;
					last = 0;          /* frame numbers start again */
					reopened++;
					printf("ftdump: tap reopened: %ux%u, writer pid %u\n", h->width, h->height,
					       h->writer_pid);
					fflush(stdout);
				}
			}
		}
		if (r <= 0)
			usleep(2000);
	}
	if (out && got) {
		FILE *f = fopen(out, "wb");
		if (!f) {
			perror(out);
			return 1;
		}
		fprintf(f, "P5\n%u %u\n255\n", pic_w, pic_h);
		for (uint32_t y = 0; y < pic_h; y++)
			fwrite(pic + (size_t)y * pic_stride, 1, pic_w, f);
		fclose(f);
		printf("ftdump: frame %llu saved to %s\n", (unsigned long long)last, out);
	}
	printf("ftdump: %llu frames read (%llu..%llu)%s\n", (unsigned long long)got,
	       (unsigned long long)first_frame, (unsigned long long)last,
	       reopened ? ", tap reopened" : "");
	munmap((void *)h, size);
	free(pic);
	return got ? 0 : 1;
}
