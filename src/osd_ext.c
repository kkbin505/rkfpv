#include "osd_ext.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void osd_ext_init(osd_ext_t *e)
{
	memset(e, 0, sizeof *e);
}

/* next integer in [*p, end), within the grid (a little outside is allowed so
 * boxes can be clipped at the edges) */
static int num(const char **p, const char *end, int *v)
{
	const char *s = *p;
	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	int neg = 0;
	if (s < end && (*s == '-' || *s == '+'))
		neg = *s++ == '-';
	if (s >= end || *s < '0' || *s > '9')
		return -1;
	long n = 0;
	while (s < end && *s >= '0' && *s <= '9') {
		n = n * 10 + (*s++ - '0');
		if (n > 10 * OSD_EXT_GRID)
			return -1;
	}
	if (s < end && *s != ' ' && *s != '\t')
		return -1;
	*v = (int)(neg ? -n : n);
	*p = s;
	return 0;
}

static int word(const char *s, const char *end, const char *w, const char **rest)
{
	size_t n = strlen(w);
	if ((size_t)(end - s) < n || memcmp(s, w, n))
		return 0;
	if (s + n < end && s[n] != ' ' && s[n] != '\t')
		return 0;
	*rest = s + n;
	return 1;
}

static int only_blanks(const char *s, const char *end)
{
	for (; s < end; s++)
		if (*s != ' ' && *s != '\t' && *s != '\r')
			return 0;
	return 1;
}

/* one command line; 1 = shape added, 0 = nothing to add, -1 = bad */
static int parse_line(osd_ext_t *e, const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (s == end || *s == '#')
		return 0;
	const char *p;
	osd_ext_shape_t sh;
	memset(&sh, 0, sizeof sh);
	int a, b, c, d;
	if (word(s, end, "clear", &p)) {
		if (!only_blanks(p, end))
			return -1;
		e->n = 0;
		return 0;
	}
	if (e->n >= OSD_EXT_MAX)
		return -1;
	if (word(s, end, "box", &p)) {
		if (num(&p, end, &a) || num(&p, end, &b) || num(&p, end, &c) || num(&p, end, &d) ||
		    !only_blanks(p, end) || c <= 0 || d <= 0)
			return -1;
		sh.kind = OSD_EXT_BOX;
		sh.x = a; sh.y = b; sh.w = c; sh.h = d;
	} else if (word(s, end, "cross", &p)) {
		if (num(&p, end, &a) || num(&p, end, &b) || num(&p, end, &c) ||
		    !only_blanks(p, end) || c <= 0)
			return -1;
		sh.kind = OSD_EXT_CROSS;
		sh.x = a; sh.y = b; sh.w = c;
	} else if (word(s, end, "text", &p)) {
		if (num(&p, end, &a) || num(&p, end, &b))
			return -1;
		if (p < end)
			p++;            /* the one blank after Y; the rest is text */
		size_t n = (size_t)(end - p);
		if (n == 0)
			return -1;
		if (n > OSD_EXT_TEXT - 1)
			n = OSD_EXT_TEXT - 1;
		sh.kind = OSD_EXT_TEXT_SHAPE;
		sh.x = a; sh.y = b;
		for (size_t i = 0; i < n; i++)  /* no control bytes on screen */
			sh.text[i] = (p[i] < ' ' || p[i] == 0x7f) ? '?' : p[i];
	} else
		return -1;
	e->s[e->n++] = sh;
	return 1;
}

int osd_ext_parse(osd_ext_t *e, const char *buf, size_t len, uint64_t now_ms)
{
	const char *s = buf, *end = buf + len;
	e->n = 0;
	e->last_ms = now_ms ? now_ms : 1;
	e->datagrams++;
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		const char *le = nl ? nl : end;
		if (parse_line(e, s, le) < 0)
			e->bad_lines++;
		s = nl ? nl + 1 : end;
	}
	return e->n;
}

int osd_ext_open(int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     /* board only */
	if (bind(fd, (struct sockaddr *)&a, sizeof a) ||
	    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

int osd_ext_poll(osd_ext_t *e, int fd, uint64_t now_ms)
{
	static char buf[4096];
	int changed = 0;
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n < 0)
			break;
		osd_ext_parse(e, buf, (size_t)n, now_ms);
		changed = 1;
	}
	return changed;
}

int osd_ext_live(const osd_ext_t *e, uint64_t now_ms, int ttl_ms)
{
	return e->n > 0 && e->last_ms && now_ms - e->last_ms <= (uint64_t)ttl_ms;
}

void osd_ext_draw(osd_t *o, const osd_ext_t *e, uint64_t now_ms, int ttl_ms)
{
	if (!o || !e || !osd_ext_live(e, now_ms, ttl_ms))
		return;
	int W = osd_width(o), H = osd_height(o);
	for (int i = 0; i < e->n; i++) {
		const osd_ext_shape_t *s = &e->s[i];
		int x = s->x * W / OSD_EXT_GRID, y = s->y * H / OSD_EXT_GRID;
		switch (s->kind) {
		case OSD_EXT_BOX:
			osd_box(o, x, y, s->w * W / OSD_EXT_GRID, s->h * H / OSD_EXT_GRID);
			break;
		case OSD_EXT_CROSS:
			osd_cross(o, x, y, s->w * H / OSD_EXT_GRID);
			break;
		case OSD_EXT_TEXT_SHAPE:
			osd_text(o, x, y, s->text);
			break;
		}
	}
}
