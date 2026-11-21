/*
 * DVR writer on the board: the SDK's librkmuxer (what rkipc records with).
 * Parameters as rkipc's storage module sets them; frag_keyframe makes MP4
 * fragmented (a fragment per keyframe), so a file cut by a power loss still
 * plays up to its last keyframe. The container follows the file extension.
 */
#define _GNU_SOURCE
#include "dvr.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "rkmuxer.h"

#define MUXER_ID 0      /* rkfpv records one stream */

struct rkm {
	int dir_fd;         /* for syncfs */
	int file_fd;        /* read-only, to drop written pages from the cache */
};

static void *rkm_open(const char *path, const dvr_stream_info *info)
{
	VideoParam v;
	memset(&v, 0, sizeof v);
	snprintf(v.format, sizeof v.format, "NV12");
	snprintf(v.codec, sizeof v.codec, "H.265");
	v.width = info->width;
	v.height = info->height;
	v.vir_width = info->width;
	v.vir_height = info->height;
	v.bit_rate = info->bitrate_kbps * 1024;
	v.level = 52;
	v.frame_rate_num = info->fps;
	v.frame_rate_den = 1;
	v.frag_keyframe = info->ts ? 0 : 1;

	if (rkmuxer_init(MUXER_ID, NULL, path, &v, NULL) != 0)
		return NULL;

	struct rkm *h = calloc(1, sizeof *h);
	if (!h) {
		rkmuxer_deinit(MUXER_ID);
		return NULL;
	}
	char dir[256];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	h->dir_fd = open(slash ? dir : ".", O_RDONLY | O_DIRECTORY);
	h->file_fd = open(path, O_RDONLY);
	return h;
}

static int rkm_write(void *hp, const uint8_t *frame, size_t len, int64_t pts_us, int key)
{
	(void)hp;
	return rkmuxer_write_video_frame(MUXER_ID, (unsigned char *)frame, (unsigned int)len,
					 pts_us, key) < 0 ? -1 : 0;
}

/* rkmuxer does its own I/O: push the filesystem to the card, then drop the
 * written pages so recording does not eat the board's little free RAM */
static void rkm_flush(void *hp)
{
	struct rkm *h = hp;
	if (h->dir_fd >= 0)
		syncfs(h->dir_fd);
	if (h->file_fd >= 0)
		posix_fadvise(h->file_fd, 0, 0, POSIX_FADV_DONTNEED);
}

static void rkm_close(void *hp)
{
	struct rkm *h = hp;
	rkmuxer_deinit(MUXER_ID);
	rkm_flush(h);
	if (h->dir_fd >= 0)
		close(h->dir_fd);
	if (h->file_fd >= 0)
		close(h->file_fd);
	free(h);
}

static long long rkm_free_mb(const char *dir)
{
	struct statvfs s;
	if (statvfs(dir, &s) != 0)
		return -1;
	return (long long)s.f_bavail * (long long)s.f_frsize / (1024 * 1024);
}

const dvr_writer dvr_rkmuxer = { rkm_open, rkm_write, rkm_flush, rkm_close, rkm_free_mb };
