/*
 * bblog: Betaflight serial blackbox logger.
 *
 * The flight controller streams its blackbox log over a UART
 * (blackbox_device = SERIAL, port function "Blackbox logging"). bblog saves
 * the bytes as they come, unchanged, as a .bbl file next to the DVR
 * recordings, for Gyroflow / Blackbox Explorer:
 *
 *   - a log starts at Betaflight's first header line
 *     ("H Product:Blackbox flight data recorder"), bytes before it (noise
 *     on the line, a half log from before we started) are dropped;
 *   - it ends at the log-end event ("End of log" + NUL), at the next
 *     header, or after -t ms without a byte (power lost, cable pulled);
 *   - its file is named after the recording that was being written when it
 *     started: rec_0012_03.mp4 -> rec_0012_03.bbl (read from rkfpv's
 *     /tmp/dvr.status). A second arm in the same segment is appended to the
 *     same file: a .bbl holds any number of logs back to back.
 *   - no recording running (no card, DVR stopped): the log is not saved.
 *   - -s -: no DVR pairing, logs go to <dir>/log_001.bbl, log_002.bbl, ...
 *     (the loopback self test).
 *
 * Data is flushed and fsync'ed every 2 s and at the end of every log, so a
 * power cut loses at most ~2 s.
 *
 *   bblog [-d /dev/ttyS4] [-b 1500000] [-o /mnt/dvr] [-s /tmp/dvr.status]
 *         [-S /tmp/bblog.status] [-t 2000] [-n 0]
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "serial.h"

#define HDR "H Product:Blackbox flight data recorder"
#define END "End of log"
#define HDR_LEN (sizeof(HDR) - 1)
#define END_LEN (sizeof(END) - 1)
#define TAIL (HDR_LEN - 1)               /* bytes kept to find a marker split across reads */
#define CHUNK 4096

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

typedef struct {
	const char *dir, *dvr_status, *status;
	int logging;            /* inside a log (saved or not) */
	FILE *f;                /* NULL while logging: not saved (no DVR / error) */
	char name[64];          /* file of the current / last log */
	const char *why;        /* not saved: why */
	int want_nul;           /* "End of log" seen at the end of a read: take its NUL */
	/* the last bytes of a read are written only with the next one: a
	 * marker split across two reads must not leave half of it in the
	 * previous log. They stay in the reader's tail, copied here for an
	 * end by timeout. */
	uint8_t hold[TAIL];
	size_t hold_len;
	uint64_t log_bytes, total_bytes, dropped, logs, errors;
	uint64_t last_rx, last_sync;
	int seq;                /* -s -: next log_NNN */
} bb_t;

/* rec_SSSS_GG.<ext> being recorded -> rec_SSSS_GG.bbl; 0 if none */
static int pair_name(const char *status, char *out, size_t n)
{
	char line[512];
	FILE *f = fopen(status, "r");
	if (!f)
		return 0;
	size_t len = fread(line, 1, sizeof line - 1, f);
	fclose(f);
	line[len] = '\0';
	if (strncmp(line, "state=recording", 15) || (line[15] != ' ' && line[15] != '\n'))
		return 0;
	const char *p = strstr(line, " file=rec_");
	if (!p)
		return 0;
	p += 6;
	int s, g, k = 0;
	if (sscanf(p, "rec_%d_%d.%n", &s, &g, &k) != 2 || !k)
		return 0;
	snprintf(out, n, "rec_%04d_%02d.bbl", s, g);
	return 1;
}

static void write_status(bb_t *b)
{
	if (!b->status)
		return;
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s.tmp", b->status);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	const char *st = !b->logging ? "idle" : b->f ? "logging" : "not-saved";
	fprintf(f, "state=%s file=%s log_bytes=%llu bytes=%llu logs=%llu dropped=%llu errors=%llu%s%s\n",
		st, b->name[0] ? b->name : "-", (unsigned long long)b->log_bytes,
		(unsigned long long)b->total_bytes, (unsigned long long)b->logs,
		(unsigned long long)b->dropped, (unsigned long long)b->errors,
		b->logging && !b->f ? " why=" : "", b->logging && !b->f ? b->why : "");
	fclose(f);
	rename(tmp, b->status);
}

static void sync_file(bb_t *b)
{
	if (b->f && (fflush(b->f) || fsync(fileno(b->f)))) {
		fprintf(stderr, "bblog: %s: %s, rest of this log not saved\n", b->name, strerror(errno));
		fclose(b->f);
		b->f = NULL;
		b->why = "write-error";
		b->errors++;
	}
	b->last_sync = now_ms();
}

static void put(bb_t *b, const uint8_t *p, size_t n);

static void log_end(bb_t *b, const char *how)
{
	if (!b->logging)
		return;
	put(b, b->hold, b->hold_len);
	b->hold_len = 0;
	if (b->f) {
		sync_file(b);
		if (b->f) {
			fclose(b->f);
			b->f = NULL;
		}
		fprintf(stderr, "bblog: log %llu ended (%s): %s, %llu bytes\n",
			(unsigned long long)b->logs, how, b->name, (unsigned long long)b->log_bytes);
	} else
		fprintf(stderr, "bblog: log %llu ended (%s), not saved (%s), %llu bytes\n",
			(unsigned long long)b->logs, how, b->why, (unsigned long long)b->log_bytes);
	b->logging = 0;
	b->want_nul = 0;
	write_status(b);
}

static void log_start(bb_t *b)
{
	char path[512];
	b->logging = 1;
	b->log_bytes = 0;
	b->logs++;
	b->f = NULL;
	if (!b->dvr_status) {
		snprintf(b->name, sizeof b->name, "log_%03d.bbl", b->seq++);
	} else if (!pair_name(b->dvr_status, b->name, sizeof b->name)) {
		b->name[0] = '\0';
		b->why = "dvr-not-recording";
		fprintf(stderr, "bblog: log %llu started, DVR not recording: not saved\n",
			(unsigned long long)b->logs);
		write_status(b);
		return;
	}
	snprintf(path, sizeof path, "%s/%s", b->dir, b->name);
	b->f = fopen(path, "ab");
	if (!b->f) {
		fprintf(stderr, "bblog: %s: %s\n", path, strerror(errno));
		b->why = "open-error";
		b->errors++;
	} else {
		setvbuf(b->f, NULL, _IOFBF, 32768);
		fprintf(stderr, "bblog: log %llu started -> %s\n", (unsigned long long)b->logs, b->name);
	}
	b->last_sync = now_ms();
	write_status(b);
}

static void put(bb_t *b, const uint8_t *p, size_t n)
{
	if (!n)
		return;
	b->log_bytes += n;
	b->total_bytes += n;
	if (b->f && fwrite(p, 1, n, b->f) != n) {
		fprintf(stderr, "bblog: %s: %s, rest of this log not saved\n", b->name, strerror(errno));
		fclose(b->f);
		b->f = NULL;
		b->why = "write-error";
		b->errors++;
	}
}

/* first match of m (len ml) in w[from..n) that ends after byte `after` */
static long find(const uint8_t *w, size_t n, size_t from, const char *m, size_t ml, size_t after)
{
	for (size_t i = from; i + ml <= n; i++)
		if (i + ml > after && w[i] == (uint8_t)m[0] && !memcmp(w + i, m, ml))
			return (long)i;
	return -1;
}

/* w = TAIL bytes already handled + the new bytes from index t */
static void process(bb_t *b, uint8_t *w, size_t n, size_t t)
{
	size_t pos = t - b->hold_len;   /* first byte not yet written / dropped */
	b->hold_len = 0;
	size_t from = 0;        /* markers are searched from here ... */
	size_t after = t;       /* ... and must end in the new bytes */
	if (b->want_nul && pos < n) {
		b->want_nul = 0;
		if (w[pos] == 0)
			put(b, w + pos++, 1);
		log_end(b, "end marker");
		from = after = pos;
	}
	while (pos < n) {
		if (!b->logging) {
			long h = find(w, n, from, HDR, HDR_LEN, after);
			if (h < 0) {
				b->dropped += n - pos;
				pos = n;
				break;
			}
			/* a header that began in the tail: those bytes were counted
			 * as dropped, but are still in w, so they are saved */
			if ((size_t)h >= pos)
				b->dropped += (size_t)h - pos;
			else
				b->dropped -= b->dropped < pos - (size_t)h ? b->dropped : pos - (size_t)h;
			log_start(b);
			pos = (size_t)h;
			from = (size_t)h + 1;
			after = from;
			continue;
		}
		long e = find(w, n, from, END, END_LEN, after);
		long h = find(w, n, from, HDR, HDR_LEN, after);
		if (e >= 0 && (h < 0 || e < h)) {
			size_t stop = (size_t)e + END_LEN;
			if (stop < pos)
				stop = pos;
			put(b, w + pos, stop - pos);
			pos = stop;
			if (pos == n) {         /* NUL comes with the next read */
				b->want_nul = 1;
				break;
			}
			if (w[pos] == 0)
				put(b, w + pos++, 1);
			log_end(b, "end marker");
			from = after = pos;
		} else if (h >= 0) {
			/* a new header without an end: FC rebooted or the end was lost */
			put(b, w + pos, (size_t)h - pos);
			log_end(b, "new header");
			pos = (size_t)h;
			from = after = pos;
		} else {
			size_t safe = n > TAIL ? n - TAIL : 0;
			if (safe < pos)
				safe = pos;
			put(b, w + pos, safe - pos);
			memcpy(b->hold, w + safe, n - safe);
			b->hold_len = n - safe;
			pos = n;
		}
	}
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-d dev] [-b baud] [-o dir] [-s dvr_status|-] [-S status] [-t ms] [-n logs]\n"
		"  -d dev       flight-controller blackbox UART     (/dev/ttyS4)\n"
		"  -b baud      115200 .. 2000000                    (1500000)\n"
		"  -o dir       where the .bbl files go              (/mnt/dvr)\n"
		"  -s file      rkfpv's DVR status, names the files  (/tmp/dvr.status)\n"
		"               '-': no DVR, log_001.bbl, log_002.bbl, ...\n"
		"  -S file      own status, key=value                (/tmp/bblog.status)\n"
		"  -t ms        silence that ends a log              (2000)\n"
		"  -n logs      exit after this many logs, 0 = never (0)\n", p);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS4";
	int baud = 1500000, idle_ms = 2000, max_logs = 0, opt;
	bb_t b;
	memset(&b, 0, sizeof b);
	b.dir = "/mnt/dvr";
	b.dvr_status = "/tmp/dvr.status";
	b.status = "/tmp/bblog.status";
	b.seq = 1;
	while ((opt = getopt(argc, argv, "d:b:o:s:S:t:n:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'b': baud = atoi(optarg); break;
		case 'o': b.dir = optarg; break;
		case 's': b.dvr_status = strcmp(optarg, "-") ? optarg : NULL; break;
		case 'S': b.status = *optarg ? optarg : NULL; break;
		case 't': idle_ms = atoi(optarg); break;
		case 'n': max_logs = atoi(optarg); break;
		default: usage(argv[0]); return 2;
		}
	}
	if (idle_ms < 100)
		idle_ms = 100;

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	int fd = serial_open(dev, baud, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return 1;
	fprintf(stderr, "bblog: %s @ %d -> %s (%s)\n", dev, baud, b.dir,
		b.dvr_status ? "named after the recording" : "log_NNN.bbl");
	write_status(&b);

	static uint8_t w[TAIL + CHUNK];
	size_t tail = 0;
	uint64_t last_status = now_ms();
	b.last_rx = now_ms();
	while (!g_quit) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r = poll(&p, 1, 200);
		uint64_t now = now_ms();
		if (r > 0 && (p.revents & POLLIN)) {
			ssize_t n = read(fd, w + tail, CHUNK);
			if (n > 0) {
				b.last_rx = now;
				process(&b, w, tail + (size_t)n, tail);
				size_t all = tail + (size_t)n;
				tail = all < TAIL ? all : TAIL;
				memmove(w, w + all - tail, tail);
			} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
				perror("read");
				break;
			} else if (n == 0) {
				usleep(50000);  /* pty with no writer: hangup, not data */
			}
		} else if (r > 0 && (p.revents & (POLLHUP | POLLERR))) {
			usleep(50000);
		}
		if (b.logging && now - b.last_rx >= (uint64_t)idle_ms)
			log_end(&b, "timeout");
		if (b.logging && b.f && now - b.last_sync >= 2000)
			sync_file(&b);
		if (now - last_status >= 1000) {
			write_status(&b);
			last_status = now;
		}
		if (max_logs && !b.logging && b.logs >= (uint64_t)max_logs)
			break;
	}
	log_end(&b, "exit");
	write_status(&b);
	close(fd);
	return 0;
}
