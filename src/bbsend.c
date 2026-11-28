/*
 * bbsend: plays the flight controller's side of a serial blackbox, for the
 * UART loopback test (TX wired to RX on the same board) and the host tests.
 *
 * Sends -n logs, each preceded by a few junk bytes (bblog must drop them)
 * and followed by -g ms of silence, paced at -r KB/s like a flight
 * controller logging at 1-2 kHz. Each log is either -f <file> (a real
 * .bbl from a flight controller) or a synthetic one in Betaflight's
 * format: the usual "H ..." header lines, then random frame bytes, then
 * the log-end event ('E', 0xFF, "End of log", NUL). -w <dir> keeps a copy
 * of every log as sent (ref_001.bbl, ...) to compare with what bblog
 * saved.
 *
 *   bbsend [-d /dev/ttyS4] [-b 1500000] [-r 60] [-n 2] [-g 3000]
 *          [-s 600000] [-S 1] [-w dir] [-f file.bbl]
 */
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "serial.h"

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static uint32_t g_rng;
static uint8_t rnd(void)
{
	g_rng = g_rng * 1103515245u + 12345u;
	return (uint8_t)(g_rng >> 16);
}

static int contains(const uint8_t *b, size_t n, const char *m)
{
	size_t ml = strlen(m);
	for (size_t i = 0; i + ml <= n; i++)
		if (!memcmp(b + i, m, ml))
			return 1;
	return 0;
}

static const char HEADER[] =
	"H Product:Blackbox flight data recorder by Nicholas Sherlock\n"
	"H Data version:2\n"
	"H I interval:32\n"
	"H P interval:1\n"
	"H P ratio:32\n"
	"H Firmware type:Cleanflight\n"
	"H Firmware revision:Betaflight 4.5.1 (rkfpv bbsend synthetic log)\n"
	"H Log start datetime:0000-01-01T00:00:00.000+00:00\n"
	"H Field I name:loopIteration,time,axisP[0],axisP[1],axisP[2],gyroADC[0],gyroADC[1],gyroADC[2],gyroUnfilt[0],gyroUnfilt[1],gyroUnfilt[2]\n"
	"H Field I signed:0,0,1,1,1,1,1,1,1,1,1\n"
	"H Field I predictor:0,0,0,0,0,0,0,0,0,0,0\n"
	"H Field I encoding:1,1,0,0,0,0,0,0,0,0,0\n"
	"H Field P predictor:6,2,1,1,1,1,1,1,1,1,1\n"
	"H Field P encoding:9,0,0,0,0,0,0,0,0,0,0\n"
	"H looptime:125\n"
	"H gyro_sync_denom:1\n"
	"H pid_process_denom:2\n"
	"H blackbox_sample_rate:1/4\n";

/* one synthetic log of about `size` bytes; never contains a header or end
 * marker except its own */
static uint8_t *make_log(size_t size, size_t *len)
{
	size_t hl = sizeof HEADER - 1;
	static const uint8_t end[] = { 'E', 0xFF, 'E', 'n', 'd', ' ', 'o', 'f', ' ', 'l', 'o', 'g', 0 };
	if (size < hl + sizeof end + 64)
		size = hl + sizeof end + 64;
	uint8_t *b = malloc(size);
	if (!b)
		return NULL;
	memcpy(b, HEADER, hl);
	size_t body = size - hl - sizeof end;
	/* 'I' / 'P' frames of random bytes, like the compressed real ones */
	for (size_t i = 0; i < body; ) {
		size_t fl = 8 + rnd() % 24;
		if (fl > body - i)
			fl = body - i;
		b[hl + i] = (i / 32) % 32 == 0 ? 'I' : 'P';
		for (size_t k = 1; k < fl; k++)
			b[hl + i + k] = rnd();
		i += fl;
	}
	memcpy(b + size - sizeof end, end, sizeof end);
	/* the random body must not hold a marker by chance */
	if (contains(b + hl, body, "End of log") || contains(b + hl, body, "H Product:")) {
		free(b);
		return make_log(size, len);
	}
	*len = size;
	return b;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "%s: cannot read\n", path);
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				usleep(1000);
				continue;
			}
			perror("write");
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* paced: `rate` KB/s in 10 ms slices, 0 = as fast as the UART takes it */
static int send_paced(int fd, const uint8_t *p, size_t n, int rate)
{
	if (rate <= 0)
		return write_all(fd, p, n);
	size_t per_tick = (size_t)rate * 1024 / 100;
	if (!per_tick)
		per_tick = 1;
	uint64_t t0 = now_us();
	for (size_t off = 0, tick = 0; off < n; tick++) {
		size_t k = n - off < per_tick ? n - off : per_tick;
		if (write_all(fd, p + off, k))
			return -1;
		off += k;
		uint64_t due = t0 + (tick + 1) * 10000;
		uint64_t now = now_us();
		if (due > now)
			usleep((useconds_t)(due - now));
	}
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-d dev] [-b baud] [-r KB/s] [-n logs] [-g ms] [-s bytes] [-S seed] [-w dir] [-f file]\n"
		"  -d dev     UART to send on                     (/dev/ttyS4)\n"
		"  -b baud    115200 .. 2000000                   (1500000)\n"
		"  -r KB/s    pacing, 0 = full speed              (60)\n"
		"  -n logs    how many logs (arms)                (2)\n"
		"  -g ms      silence after each log              (3000)\n"
		"  -s bytes   synthetic log size                  (600000)\n"
		"  -S seed    synthetic log content               (1)\n"
		"  -w dir     keep each log as sent: ref_NNN.bbl\n"
		"  -f file    send this .bbl instead of a synthetic log\n", p);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS4", *refdir = NULL, *file = NULL;
	int baud = 1500000, rate = 60, logs = 2, gap = 3000, opt;
	size_t size = 600000;
	g_rng = 1;
	while ((opt = getopt(argc, argv, "d:b:r:n:g:s:S:w:f:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'b': baud = atoi(optarg); break;
		case 'r': rate = atoi(optarg); break;
		case 'n': logs = atoi(optarg); break;
		case 'g': gap = atoi(optarg); break;
		case 's': size = (size_t)atol(optarg); break;
		case 'S': g_rng = (uint32_t)atol(optarg); break;
		case 'w': refdir = optarg; break;
		case 'f': file = optarg; break;
		default: usage(argv[0]); return 2;
		}
	}
	int fd = serial_open(dev, baud, O_WRONLY);
	if (fd < 0)
		return 1;
	int rc = 0;
	for (int i = 1; i <= logs && !rc; i++) {
		size_t len;
		uint8_t *log = file ? read_file(file, &len) : make_log(size, &len);
		if (!log)
			return 1;
		if (refdir) {
			char path[512];
			snprintf(path, sizeof path, "%s/ref_%03d.bbl", refdir, i);
			FILE *f = fopen(path, "wb");
			if (!f || fwrite(log, 1, len, f) != len) {
				perror(path);
				rc = 1;
			}
			if (f)
				fclose(f);
		}
		/* line noise before the log: must not end up in the file */
		uint8_t junk[32];
		for (size_t k = 0; k < sizeof junk; k++)
			junk[k] = rnd();
		uint64_t t0 = now_us();
		if (!rc && (write_all(fd, junk, sizeof junk) || send_paced(fd, log, len, rate)))
			rc = 1;
		tcdrain(fd);
		double s = (now_us() - t0) / 1e6;
		fprintf(stderr, "bbsend: log %d: %zu bytes in %.2f s (%.1f KB/s)\n",
			i, len, s, s > 0 ? len / 1024.0 / s : 0);
		free(log);
		usleep((useconds_t)gap * 1000);
	}
	close(fd);
	return rc;
}
