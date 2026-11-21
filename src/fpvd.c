/*
 * fpvd: `fpv` command service over UDP.
 *
 * Lets a ground station / the PC configurator run `fpv` subcommands on the
 * air unit over any IP path: USB RNDIS today, the wfb-ng tunnel later
 * (issue #1). It runs `fpv` only, never a shell, with arguments checked
 * against a strict character set.
 *
 * Protocol (one request per datagram, ASCII):
 *
 *   request:  FPV1 <id> <cmd> [arg ...]
 *   reply:    FPV1 <id> <seq>/<total>\n<chunk>      (one or more datagrams)
 *
 *   <id>     1-16 chars [A-Za-z0-9_-], chosen by the client, new per request
 *   <cmd>    get set schema status log restart mode reset dvr, or ping
 *   payload  the JSON that `fpv <cmd> [arg ...] --json` prints, split into
 *            chunks of at most CHUNK bytes; seq counts from 1
 *
 * The link may drop datagrams, so a client resends a request with the SAME
 * id until it has every chunk. fpvd keeps the last CACHE replies and answers
 * a repeated id from the cache instead of running the command again: a
 * retried `restart` or `set` is not executed twice.
 *
 *   fpvd [-p 7000] [-b 0.0.0.0] [-f /usr/bin/fpv] [-v]
 *        [-a <adc file>] [-H 2000] [-m /tmp/mode.status] [-x /etc/init.d/S99fpv]
 *
 * Hotspot button: the board's BOOT button, which pulls SARADC channel 0 to
 * GND (1023 released, ~10 pressed). fpvd reads it every BUTTON_POLL_MS;
 * held for 2 s it runs "<script> hotspot-off" if the mode file says
 * "hotspot", else "<script> hotspot-on", in the background. One action per
 * press: the button must be released before it counts again. Ignored while
 * the mode is not set yet or "switching" (boot still running, or a change in
 * progress). BOOT held at power-up enters maskrom, so there is no boot-time
 * decision. State: /tmp/switch.status.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAGIC       "FPV1"
#define MAX_REQ     1024
#define MAX_ARGS    32
#define MAX_ID      16
#define CHUNK       1200          /* fits a wfb-ng packet with room for headers */
#define MAX_OUT     (256 * 1024)
#define CACHE       8
#define EXEC_TIMEOUT_MS 20000

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static int g_verbose;
static const char *g_fpv = "/usr/bin/fpv";

static const char *const ALLOWED[] = {
	"get", "set", "schema", "status", "log", "restart", "mode", "reset", "dvr", NULL
};

struct entry {
	char id[MAX_ID + 1];
	struct sockaddr_in peer;
	char *out;
	size_t len;
	unsigned long long age;
};
static struct entry g_cache[CACHE];
static unsigned long long g_clock;

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int valid_id(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > MAX_ID)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return 0;
	return 1;
}

/* values are numbers, key names, 1280x720, true/false, auto: nothing a
 * shell or fpv's own parsing could misread */
static int valid_arg(const char *s)
{
	if (!*s || strlen(s) > 64)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || strchr("_.,:-", *s)))
			return 0;
	return 1;
}

static int allowed(const char *cmd)
{
	for (int i = 0; ALLOWED[i]; i++)
		if (!strcmp(cmd, ALLOWED[i]))
			return 1;
	return 0;
}

static char *dup_error(const char *msg, size_t *len)
{
	char *b = malloc(64 + strlen(msg));
	if (!b)
		return NULL;
	*len = (size_t)sprintf(b, "{\"ok\":false,\"error\":\"%s\"}", msg);
	return b;
}

/* run `fpv argv... --json`, return its stdout (malloc'd) */
static char *run_fpv(char **args, int nargs, size_t *len)
{
	char *argv[MAX_ARGS + 3];
	argv[0] = "fpv";
	for (int i = 0; i < nargs; i++)
		argv[i + 1] = args[i];
	argv[nargs + 1] = "--json";
	argv[nargs + 2] = NULL;

	int p[2];
	if (pipe(p) < 0)
		return dup_error("pipe failed", len);
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return dup_error("fork failed", len);
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(p[1], 1);
		dup2(null, 2);
		close(p[0]);
		close(p[1]);
		execv(g_fpv, argv);
		_exit(127);
	}
	close(p[1]);

	size_t cap = 8192, n = 0;
	char *out = malloc(cap);
	uint64_t deadline = now_ms() + EXEC_TIMEOUT_MS;
	int timed_out = 0;
	while (out) {
		int left = (int)(deadline - now_ms());
		if (left <= 0) {
			timed_out = 1;
			break;
		}
		struct pollfd pf = { .fd = p[0], .events = POLLIN };
		if (poll(&pf, 1, left) <= 0) {
			if (errno == EINTR && !g_quit)
				continue;
			timed_out = 1;
			break;
		}
		if (n == cap) {
			if (cap >= MAX_OUT)
				break;          /* keep what we have, drop the rest */
			char *b = realloc(out, cap * 2);
			if (!b)
				break;
			out = b;
			cap *= 2;
		}
		ssize_t r = read(p[0], out + n, cap - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		n += (size_t)r;
	}
	close(p[0]);
	if (timed_out)
		kill(pid, SIGKILL);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;

	if (!out)
		return dup_error("out of memory", len);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		n--;
	if (timed_out) {
		free(out);
		return dup_error("fpv timed out", len);
	}
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127 && n == 0) {
		free(out);
		return dup_error("cannot run fpv", len);
	}
	if (n == 0) {
		free(out);
		return dup_error("fpv produced no output", len);
	}
	*len = n;
	return out;
}

static void send_reply(int fd, const struct sockaddr_in *peer, const char *id,
		       const char *out, size_t len)
{
	unsigned total = len ? (unsigned)((len + CHUNK - 1) / CHUNK) : 1;
	char pkt[CHUNK + 64];
	for (unsigned seq = 1; seq <= total; seq++) {
		size_t off = (size_t)(seq - 1) * CHUNK;
		size_t n = len - off < CHUNK ? len - off : CHUNK;
		int h = snprintf(pkt, sizeof pkt, MAGIC " %s %u/%u\n", id, seq, total);
		memcpy(pkt + h, out + off, n);
		sendto(fd, pkt, (size_t)h + n, 0, (const struct sockaddr *)peer, sizeof *peer);
	}
}

static struct entry *cache_find(const char *id, const struct sockaddr_in *peer)
{
	for (int i = 0; i < CACHE; i++)
		if (g_cache[i].out && !strcmp(g_cache[i].id, id) &&
		    g_cache[i].peer.sin_addr.s_addr == peer->sin_addr.s_addr &&
		    g_cache[i].peer.sin_port == peer->sin_port)
			return &g_cache[i];
	return NULL;
}

static void cache_put(const char *id, const struct sockaddr_in *peer, char *out, size_t len)
{
	struct entry *e = &g_cache[0];
	for (int i = 0; i < CACHE; i++) {
		if (!g_cache[i].out) {
			e = &g_cache[i];
			break;
		}
		if (g_cache[i].age < e->age)
			e = &g_cache[i];
	}
	free(e->out);
	snprintf(e->id, sizeof e->id, "%s", id);
	e->peer = *peer;
	e->out = out;
	e->len = len;
	e->age = ++g_clock;
}

static void handle(int fd, char *req, size_t n, const struct sockaddr_in *peer)
{
	req[n] = '\0';
	char *tok[MAX_ARGS + 3];
	int nt = 0;
	for (char *s = strtok(req, " \t\r\n"); s; s = strtok(NULL, " \t\r\n")) {
		if (nt == MAX_ARGS + 2) {
			nt = -1;
			break;
		}
		tok[nt++] = s;
	}
	if (nt < 2 || strcmp(tok[0], MAGIC) || !valid_id(tok[1]))
		return;                 /* not for us: no reply */
	const char *id = tok[1];

	struct entry *hit = cache_find(id, peer);
	if (hit) {
		if (g_verbose)
			fprintf(stderr, "fpvd: %s repeated, cached reply\n", id);
		hit->age = ++g_clock;
		send_reply(fd, peer, id, hit->out, hit->len);
		return;
	}

	size_t len = 0;
	char *out;
	if (nt < 0)
		out = dup_error("too many arguments", &len);
	else if (nt < 3)
		out = dup_error("no command", &len);
	else if (!strcmp(tok[2], "ping")) {
		out = strdup("{\"ok\":true,\"pong\":true}");
		len = out ? strlen(out) : 0;
	} else if (!allowed(tok[2]))
		out = dup_error("command not allowed", &len);
	else {
		int bad = 0;
		for (int i = 2; i < nt; i++)
			if (!valid_arg(tok[i]))
				bad = 1;
		if (bad)
			out = dup_error("invalid characters in arguments", &len);
		else {
			if (g_verbose) {
				fprintf(stderr, "fpvd: %s from %s:", id, inet_ntoa(peer->sin_addr));
				for (int i = 2; i < nt; i++)
					fprintf(stderr, " %s", tok[i]);
				fputc('\n', stderr);
			}
			out = run_fpv(tok + 2, nt - 2, &len);
		}
	}
	if (!out)
		return;
	send_reply(fd, peer, id, out, len);
	cache_put(id, peer, out, len);
}

/* ---- hotspot button (BOOT, on the ADC) --------------------------------- */

#define BUTTON_ADC "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define BUTTON_PRESSED_BELOW 300      /* 10-bit ADC: ~1023 released, ~10 pressed */
#define BUTTON_POLL_MS 200
#define SWITCH_STATUS "/tmp/switch.status"

/* 1 pressed, 0 released, -1 unreadable */
static int button_read(int fd)
{
	char b[16];
	ssize_t n = pread(fd, b, sizeof b - 1, 0);
	if (n <= 0)
		return -1;
	b[n] = 0;
	return atoi(b) < BUTTON_PRESSED_BELOW;
}

/* first word of the mode file, "" if missing */
static void read_mode(const char *path, char out[16])
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fscanf(f, "%15s", out) != 1)
		out[0] = 0;
	fclose(f);
}

static void switch_status(int pressed, int busy)
{
	FILE *f = fopen(SWITCH_STATUS ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "switch=%s%s\n", pressed ? "pressed" : "released", busy ? " switching=1" : "");
	fclose(f);
	rename(SWITCH_STATUS ".tmp", SWITCH_STATUS);
}

static uint64_t mono_ms(void) { return now_ms(); }

int main(int argc, char **argv)
{
	int port = 7000;
	const char *bind_ip = "0.0.0.0";
	const char *sw_adc = BUTTON_ADC, *sw_script = "/etc/init.d/S99fpv";
	const char *sw_mode = "/tmp/mode.status";
	int hold_ms = 2000;
	int c;
	while ((c = getopt(argc, argv, "p:b:f:va:H:m:x:h")) != -1) {
		switch (c) {
		case 'p': port = atoi(optarg); break;
		case 'b': bind_ip = optarg; break;
		case 'f': g_fpv = optarg; break;
		case 'v': g_verbose = 1; break;
		case 'a': sw_adc = optarg; break;
		case 'H': hold_ms = atoi(optarg); break;
		case 'm': sw_mode = optarg; break;
		case 'x': sw_script = optarg; break;
		default:
			fprintf(stderr, "usage: fpvd [-p 7000] [-b 0.0.0.0] [-f /usr/bin/fpv] [-v]\n"
				"            [-a adc_file] [-H hold_ms] [-m mode_file] [-x /etc/init.d/S99fpv]\n");
			return c == 'h' ? 0 : 1;
		}
	}

	/* hotspot button */
	int swfd = open(sw_adc, O_RDONLY | O_CLOEXEC);
	int sw_down = swfd >= 0 ? button_read(swfd) : -1;
	if (sw_down < 0 && swfd >= 0) {
		close(swfd);
		swfd = -1;
	}
	int sw_used = sw_down > 0;      /* held since start: wait for a release first */
	uint64_t sw_since = mono_ms();
	pid_t sw_child = 0;
	if (swfd >= 0) {
		fprintf(stderr, "fpvd: hotspot button on %s\n", sw_adc);
		switch_status(sw_down, 0);
	} else {
		fprintf(stderr, "fpvd: no hotspot button (%s)\n", sw_adc);
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("fpvd: socket");
		return 1;
	}
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
		fprintf(stderr, "fpvd: bad bind address %s\n", bind_ip);
		return 1;
	}
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		perror("fpvd: bind");
		return 1;
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "fpvd: listening on %s:%d, running %s\n", bind_ip, port, g_fpv);

	char buf[MAX_REQ + 1];
	while (!g_quit) {
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		poll(&pf, 1, swfd >= 0 ? BUTTON_POLL_MS : 1000);

		if (pf.revents & POLLIN) {
			struct sockaddr_in peer;
			socklen_t pl = sizeof peer;
			ssize_t n = recvfrom(fd, buf, MAX_REQ, 0, (struct sockaddr *)&peer, &pl);
			if (n > 0)
				handle(fd, buf, (size_t)n, &peer);
		}
		if (swfd < 0)
			continue;

		if (sw_child && waitpid(sw_child, NULL, WNOHANG) == sw_child) {
			sw_child = 0;
			switch_status(sw_down, 0);
		}
		int v = button_read(swfd);
		if (v < 0)
			continue;
		if (v != sw_down) {
			sw_down = v;
			sw_since = mono_ms();
			if (!v)
				sw_used = 0;
			switch_status(sw_down, sw_child != 0);
		}
		if (!sw_down || sw_used || sw_child || mono_ms() - sw_since < (uint64_t)hold_ms)
			continue;
		sw_used = 1;
		char mode[16];
		read_mode(sw_mode, mode);
		if (!mode[0] || !strcmp(mode, "switching")) {
			fprintf(stderr, "fpvd: hotspot button ignored (mode '%s')\n", mode);
			continue;
		}
		const char *arg = strcmp(mode, "hotspot") ? "hotspot-on" : "hotspot-off";
		fprintf(stderr, "fpvd: hotspot button held: %s %s\n", sw_script, arg);
		pid_t pid = fork();
		if (pid == 0) {
			close(fd);
			execl(sw_script, sw_script, arg, (char *)NULL);
			_exit(127);
		}
		sw_child = pid > 0 ? pid : 0;
		switch_status(sw_down, sw_child != 0);
	}
	for (int i = 0; i < CACHE; i++)
		free(g_cache[i].out);
	close(fd);
	if (swfd >= 0)
		close(swfd);
	return 0;
}
