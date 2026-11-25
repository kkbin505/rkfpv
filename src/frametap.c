/* Frame tap writer (see frametap.h). No SDK calls: tested on the PC. */
#include "frametap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

struct frametap {
	char path[128];
	frametap_hdr_t *h;
	size_t size;
	int scale;
	uint64_t frames;
};

frametap_t *frametap_create(const char *path, int src_w, int src_h, int scale)
{
	if (scale < 1 || src_w < scale || src_h < scale)
		return NULL;
	frametap_t *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	snprintf(t->path, sizeof t->path, "%s", path);
	t->scale = scale;
	uint32_t w = src_w / scale, h = src_h / scale;
	uint32_t stride = (w + 15) & ~15u;
	uint32_t slot = (stride * h + 63) & ~63u;
	uint32_t off = (sizeof(frametap_hdr_t) + 63) & ~63u;
	t->size = off + (size_t)slot * FRAMETAP_SLOTS;

	/* a reader may still map the old file: replace it, do not truncate */
	unlink(path);
	int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, (off_t)t->size)) {
		perror(path);
		if (fd >= 0)
			close(fd);
		free(t);
		return NULL;
	}
	t->h = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (t->h == MAP_FAILED) {
		perror("frametap mmap");
		unlink(path);
		free(t);
		return NULL;
	}
	frametap_hdr_t *hd = t->h;
	hd->version = FRAMETAP_VERSION;
	hd->width = w;
	hd->height = h;
	hd->stride = stride;
	hd->slots = FRAMETAP_SLOTS;
	hd->slot_bytes = slot;
	hd->data_offset = off;
	hd->writer_pid = (uint32_t)getpid();
	hd->src_width = src_w;
	hd->src_height = src_h;
	hd->scale = scale;
	__atomic_store_n(&hd->magic, FRAMETAP_MAGIC, __ATOMIC_RELEASE);  /* valid now */
	return t;
}

void frametap_put(frametap_t *t, const uint8_t *y, int src_stride,
		  uint64_t pts_us, uint64_t now_us)
{
	frametap_hdr_t *h = t->h;
	uint32_t i = (__atomic_load_n(&h->latest, __ATOMIC_RELAXED) + 1) % FRAMETAP_SLOTS;
	frametap_slot_t *s = &h->slot[i];
	uint8_t *dst = (uint8_t *)h + h->data_offset + (size_t)i * h->slot_bytes;
	int sc = t->scale;

	__atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELAXED);          /* odd */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	for (uint32_t r = 0; r < h->height; r++) {
		const uint8_t *src = y + (size_t)r * sc * src_stride;
		uint8_t *d = dst + (size_t)r * h->stride;
		if (sc == 1) {
			memcpy(d, src, h->width);
		} else {
			for (uint32_t c = 0; c < h->width; c++)
				d[c] = src[c * sc];
		}
	}
	s->frame = ++t->frames;
	s->pts_us = pts_us;
	s->write_us = now_us;
	__atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);          /* even */
	__atomic_store_n(&h->latest, i, __ATOMIC_RELEASE);
}

uint64_t frametap_frames(const frametap_t *t)
{
	return t ? t->frames : 0;
}

void frametap_destroy(frametap_t *t)
{
	if (!t)
		return;
	unlink(t->path);
	munmap(t->h, t->size);
	free(t);
}
