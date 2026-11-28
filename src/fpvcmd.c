#include "fpvcmd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUT (256 * 1024)

/* preview: only allowed by fpv itself in debug and hotspot mode;
 * hotspot / iq: status and switching the IQ file (no file upload here) */
static const char *const ALLOWED[] = {
	"get", "set", "schema", "status", "log", "restart", "mode", "reset", "dvr",
	"preview", "hotspot", "iq", "blackbox", NULL
};

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

int fpvcmd_allowed(const char *cmd)
{
	for (int i = 0; ALLOWED[i]; i++)
		if (!strcmp(cmd, ALLOWED[i]))
			return 1;
	return 0;
}

/* values are numbers, key names, 1280x720, true/false, auto: nothing a
 * shell or fpv's own parsing could misread */
int fpvcmd_valid_arg(const char *s)
{
	if (!*s || strlen(s) > 64)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || strchr("_.,:-", *s)))
			return 0;
	return 1;
}

char *fpvcmd_error(const char *msg, size_t *len)
{
	char *b = malloc(64 + strlen(msg));
	if (!b)
		return NULL;
	*len = (size_t)sprintf(b, "{\"ok\":false,\"error\":\"%s\"}", msg);
	return b;
}

char *fpvcmd_run(const char *fpv, char **args, int nargs, int timeout_ms, size_t *len)
{
	if (nargs < 1)
		return fpvcmd_error("no command", len);
	if (nargs > FPVCMD_MAX_ARGS)
		return fpvcmd_error("too many arguments", len);
	if (!fpvcmd_allowed(args[0]))
		return fpvcmd_error("command not allowed", len);
	for (int i = 0; i < nargs; i++)
		if (!fpvcmd_valid_arg(args[i]))
			return fpvcmd_error("invalid characters in arguments", len);

	char *argv[FPVCMD_MAX_ARGS + 3];
	argv[0] = "fpv";
	for (int i = 0; i < nargs; i++)
		argv[i + 1] = args[i];
	argv[nargs + 1] = "--json";
	argv[nargs + 2] = NULL;

	int p[2];
	if (pipe(p) < 0)
		return fpvcmd_error("pipe failed", len);
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return fpvcmd_error("fork failed", len);
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(p[1], 1);
		dup2(null, 2);
		close(p[0]);
		close(p[1]);
		execv(fpv, argv);
		_exit(127);
	}
	close(p[1]);

	size_t cap = 8192, n = 0;
	char *out = malloc(cap);
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
	int timed_out = 0;
	while (out) {
		int left = (int)(deadline - now_ms());
		if (left <= 0) {
			timed_out = 1;
			break;
		}
		struct pollfd pf = { .fd = p[0], .events = POLLIN };
		int pr = poll(&pf, 1, left);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr <= 0) {
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
		return fpvcmd_error("out of memory", len);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		n--;
	if (timed_out) {
		free(out);
		return fpvcmd_error("fpv timed out", len);
	}
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127 && n == 0) {
		free(out);
		return fpvcmd_error("cannot run fpv", len);
	}
	if (n == 0) {
		free(out);
		return fpvcmd_error("fpv produced no output", len);
	}
	*len = n;
	return out;
}
