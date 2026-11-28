/*
 * bbsim: a Betaflight flight controller's serial blackbox, simulated, for
 * hardware-in-the-loop tests of bblog on the board.
 *
 * Unlike bbsend (random frame bytes, for byte-exact link tests) bbsim sends
 * logs a real decoder can read: Betaflight's header lines, then I and P
 * frames encoded the way the firmware does (predictors, variable-byte
 * encodings), then the log-end event. The flight data is a known function of
 * time (gyro and motor sines, a small loop-time jitter), so whatever decodes
 * the saved .bbl can be compared value by value with -w's expected CSV.
 *
 * Output goes to a pseudo-terminal (bblog opens its slave like a UART, no
 * wiring) or to a real serial device:
 *
 *   bbsim -p /tmp/bbuart [-n 2] [-s 20] [-r 1000] [-g 3000] [-j 64] [-c] [-w dir]
 *   bbsim -d /dev/ttyS4 -b 1500000 ...
 *
 *   -p link    create a pty, symlink its slave at link (removed at exit)
 *   -d dev     a real serial device instead (-b baud)
 *   -n logs    logs to send (arm/disarm cycles)              (2)
 *   -s secs    flight time per log                           (20)
 *   -r hz      logged frames per second (BF 1-2 kHz)         (1000)
 *   -g ms      silence after each log                        (3000)
 *   -j bytes   junk before each log (noise on the line)      (64)
 *   -c         cut the last log: no log-end event (cable pulled / power lost)
 *   -w dir     write ref_NNN.bbl (bytes as sent) and ref_NNN.csv (values)
 *   -W ms      wait before the first log (bblog starting)     (1000)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../src/serial.h"

#define I_INTERVAL 32           /* an I frame every 32 logged iterations, as BF */
#define NFIELDS 9

static const char *NAMES[NFIELDS] = {
	"loopIteration", "time", "gyroADC[0]", "gyroADC[1]", "gyroADC[2]",
	"motor[0]", "motor[1]", "motor[2]", "motor[3]",
};
static const int SIGNED[NFIELDS] = { 0, 0, 1, 1, 1, 0, 0, 0, 0 };
/* predictors: 0 none, 1 previous, 2 straight line, 6 increment */
static const int I_PRED[NFIELDS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int P_PRED[NFIELDS] = { 6, 2, 1, 1, 1, 1, 1, 1, 1 };
/* encodings: 0 signed VB, 1 unsigned VB, 9 null (nothing written) */
static const int I_ENC[NFIELDS] = { 1, 1, 0, 0, 0, 1, 1, 1, 1 };
static const int P_ENC[NFIELDS] = { 9, 0, 0, 0, 0, 0, 0, 0, 0 };

static int g_fd = -1;
static int g_hz = 1000;          /* logged frames per second */
static FILE *g_ref;
static uint64_t g_bytes;

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void out(const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = write(g_fd, b, n);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN) { usleep(200); continue; }
			perror("bbsim: write");
			exit(1);
		}
		if (g_ref)
			fwrite(b, 1, (size_t)w, g_ref);
		b += w; n -= (size_t)w; g_bytes += (uint64_t)w;
	}
}

static void outs(const char *s) { out(s, strlen(s)); }

/* unsigned variable byte: 7 bits per byte, low first, high bit = more */
static int uvb(uint8_t *b, uint32_t v)
{
	int n = 0;
	while (v > 127) { b[n++] = (uint8_t)(v | 0x80); v >>= 7; }
	b[n++] = (uint8_t)v;
	return n;
}

/* signed: zig-zag, then unsigned */
static int svb(uint8_t *b, int32_t v)
{
	return uvb(b, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31));
}

static void header(void)
{
	char line[512];
	outs("H Product:Blackbox flight data recorder by Nicholas Sherlock\n");
	outs("H Data version:2\n");
	outs("H I interval:32\n");
	outs("H P interval:1/1\n");
	outs("H Firmware type:Cleanflight\n");
	outs("H Firmware revision:Betaflight 4.4.2 (bbsim) STM32F7X2\n");
	outs("H Firmware date:Nov 28 2026 00:00:00\n");
	outs("H Board information:BBSM BBSIM\n");
	outs("H Log start datetime:0000-01-01T00:00:00.000+00:00\n");
	outs("H Craft name:rkfpv-hil\n");
#define LIST(label, arr, fmt)                                                   \
	do {                                                                    \
		int k = snprintf(line, sizeof line, "H Field %s:", label);      \
		for (int i = 0; i < NFIELDS; i++)                               \
			k += snprintf(line + k, sizeof line - k, fmt "%s", arr[i], \
				      i + 1 < NFIELDS ? "," : "\n");            \
		outs(line);                                                     \
	} while (0)
	LIST("I name", NAMES, "%s");
	LIST("I signed", SIGNED, "%d");
	LIST("I predictor", I_PRED, "%d");
	LIST("I encoding", I_ENC, "%d");
	LIST("P predictor", P_PRED, "%d");
	LIST("P encoding", P_ENC, "%d");
#undef LIST
	outs("H minthrottle:1070\n");
	outs("H maxthrottle:2000\n");
	outs("H gyro_scale:0x3f800000\n");
	outs("H motorOutput:1070,2000\n");
	outs("H vbatref:420\n");
	char lt[48];
	snprintf(lt, sizeof lt, "H looptime:%d\n", 1000000 / g_hz);
	outs(lt);
	outs("H gyro_sync_denom:1\n");
	outs("H pid_process_denom:1\n");
}

/* the flight: value of every field at logged iteration i (time in us) */
static void sample(uint32_t i, int64_t *v)
{
	double t = (double)i / g_hz;
	v[0] = i;
	/* the logging rate with a little jitter: the straight-line predictor's residual */
	v[1] = 2000000 + (int64_t)i * 1000000 / g_hz + (int64_t)((i * 7919u) % 7) - 3;
	v[2] = (int64_t)lround(400 * sin(2 * M_PI * 1.3 * t) + 25 * sin(2 * M_PI * 97 * t));
	v[3] = (int64_t)lround(-250 * sin(2 * M_PI * 0.7 * t + 1) + 18 * sin(2 * M_PI * 131 * t));
	v[4] = (int64_t)lround(120 * sin(2 * M_PI * 0.4 * t + 2));
	for (int m = 0; m < 4; m++)
		v[5 + m] = 1400 + (int64_t)lround(300 * sin(2 * M_PI * 0.5 * t + m) +
						  20 * sin(2 * M_PI * 60 * t + m));
}

static void frame(uint32_t i, const int64_t *cur, const int64_t *prev, const int64_t *prev2)
{
	uint8_t b[NFIELDS * 5 + 1];
	int n = 0;
	int is_i = i % I_INTERVAL == 0;
	b[n++] = is_i ? 'I' : 'P';
	for (int f = 0; f < NFIELDS; f++) {
		int pred = is_i ? I_PRED[f] : P_PRED[f], enc = is_i ? I_ENC[f] : P_ENC[f];
		int64_t p = 0;
		switch (pred) {
		case 1: p = prev[f]; break;
		case 2: p = 2 * prev[f] - prev2[f]; break;
		case 6: p = prev[f] + 1; break;
		}
		int64_t d = cur[f] - p;
		if (enc == 9) {
			if (d) { fprintf(stderr, "bbsim: field %s not predictable\n", NAMES[f]); exit(1); }
		} else if (enc == 1) {
			n += uvb(b + n, (uint32_t)d);
		} else {
			n += svb(b + n, (int32_t)d);
		}
	}
	out(b, (size_t)n);
}

int main(int argc, char **argv)
{
	const char *link = NULL, *dev = NULL, *wdir = NULL;
	int baud = 1500000, logs = 2, secs = 20, hz = 1000, gap = 3000, junk = 64, cut = 0, wait0 = 1000, opt;
	while ((opt = getopt(argc, argv, "p:d:b:n:s:r:g:j:cw:W:")) != -1) {
		switch (opt) {
		case 'p': link = optarg; break;
		case 'd': dev = optarg; break;
		case 'b': baud = atoi(optarg); break;
		case 'n': logs = atoi(optarg); break;
		case 's': secs = atoi(optarg); break;
		case 'r': hz = atoi(optarg); break;
		case 'g': gap = atoi(optarg); break;
		case 'j': junk = atoi(optarg); break;
		case 'c': cut = 1; break;
		case 'w': wdir = optarg; break;
		case 'W': wait0 = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s -p link | -d dev [-b baud] [-n logs] [-s secs] [-r hz] "
					"[-g ms] [-j bytes] [-c] [-w dir] [-W ms]\n", argv[0]);
			return 2;
		}
	}
	g_hz = hz;
	if (!link == !dev || hz < 1 || hz > 32000 || secs < 1) {
		fprintf(stderr, "bbsim: one of -p / -d, -r 1..32000, -s > 0\n");
		return 2;
	}
	if (link) {
		g_fd = posix_openpt(O_RDWR | O_NOCTTY);
		if (g_fd < 0 || grantpt(g_fd) || unlockpt(g_fd)) { perror("bbsim: pty"); return 1; }
		struct termios t;
		tcgetattr(g_fd, &t);
		cfmakeraw(&t);
		tcsetattr(g_fd, TCSANOW, &t);
		unlink(link);
		if (symlink(ptsname(g_fd), link)) { perror(link); return 1; }
	} else if ((g_fd = serial_open(dev, baud, O_WRONLY)) < 0) {
		return 1;
	}
	printf("bbsim: %d log(s) of %d s at %d Hz -> %s%s\n", logs, secs, hz, link ? link : dev,
	       cut ? ", last one cut" : "");
	fflush(stdout);
	usleep((useconds_t)wait0 * 1000);

	uint32_t rng = 12345;
	for (int l = 1; l <= logs; l++) {
		FILE *csv = NULL;
		if (wdir) {
			char p[512];
			snprintf(p, sizeof p, "%s/ref_%03d.bbl", wdir, l);
			g_ref = fopen(p, "wb");
			snprintf(p, sizeof p, "%s/ref_%03d.csv", wdir, l);
			csv = fopen(p, "w");
			if (!g_ref || !csv) { perror(p); return 1; }
			for (int f = 0; f < NFIELDS; f++)
				fprintf(csv, "%s%s", NAMES[f], f + 1 < NFIELDS ? "," : "\n");
		}
		FILE *ref = g_ref;
		g_ref = NULL;               /* the junk is not part of the log */
		for (int k = 0; k < junk; k++) {
			rng = rng * 1103515245u + 12345u;
			uint8_t c = (uint8_t)(rng >> 16);
			out(&c, 1);
		}
		g_ref = ref;
		uint64_t start = now_us(), b0 = g_bytes;
		header();
		int64_t prev[NFIELDS] = { 0 }, prev2[NFIELDS] = { 0 }, cur[NFIELDS];
		uint32_t frames = (uint32_t)secs * (uint32_t)hz;
		for (uint32_t i = 0; i < frames; i++) {
			sample(i, cur);
			frame(i, cur, prev, prev2);
			if (csv)
				for (int f = 0; f < NFIELDS; f++)
					fprintf(csv, "%lld%s", (long long)cur[f], f + 1 < NFIELDS ? "," : "\n");
			memcpy(prev2, i % I_INTERVAL == 0 ? cur : prev, sizeof prev2);
			memcpy(prev, cur, sizeof prev);
			/* pace like the firmware: frame i leaves at i / hz seconds */
			uint64_t due = start + (uint64_t)(i + 1) * 1000000u / (uint64_t)hz;
			uint64_t now = now_us();
			if (due > now)
				usleep((useconds_t)(due - now));
		}
		int last_cut = cut && l == logs;
		if (!last_cut) {
			static const uint8_t end[] = { 'E', 0xFF, 'E', 'n', 'd', ' ', 'o', 'f', ' ', 'l', 'o', 'g', 0 };
			out(end, sizeof end);
		}
		double s = (now_us() - start) / 1e6;
		printf("bbsim: log %d: %u frames, %llu bytes in %.1f s (%.0f KB/s)%s\n", l, frames,
		       (unsigned long long)(g_bytes - b0), s, (g_bytes - b0) / 1024.0 / s,
		       last_cut ? ", cut (no log end)" : "");
		fflush(stdout);
		if (g_ref) { fclose(g_ref); g_ref = NULL; }
		if (csv) fclose(csv);
		tcdrain(g_fd);
		usleep((useconds_t)gap * 1000);
	}
	if (link)
		unlink(link);
	close(g_fd);
	return 0;
}
