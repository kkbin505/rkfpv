/* Host test of the wired preview server (src/preview.c). */
#include "../src/preview.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("ok   %s\n", name); } \
	else { failed++; printf("FAIL %s (line %d)\n", name, __LINE__); } } while (0)

/* Annex-B access units: IDR = VPS + IDR slice, P = TRAIL_R slice */
static size_t make_au(uint8_t *b, int idr, size_t size, uint8_t fill)
{
	size_t n = 0;
	if (idr) {
		memcpy(b, "\0\0\0\1\x40\x01\x0c", 7);   /* VPS */
		n = 7;
		memcpy(b + n, "\0\0\0\1\x26\x01", 6);   /* IDR_W_RADL */
		n += 6;
	} else {
		memcpy(b, "\0\0\0\1\x02\x01", 6);       /* TRAIL_R */
		n = 6;
	}
	while (n < size)
		b[n++] = fill;
	return n;
}

/* push an AU in up to three packs, like the RK encoder does */
static void push_au(preview_t *p, const uint8_t *b, size_t len)
{
	size_t a = len / 3, c = len / 3;
	preview_push(p, b, a, 0);
	preview_push(p, b + a, c, 0);
	preview_push(p, b + a + c, len - a - c, 1);
}

static int connect_to(int port, int rcvbuf)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (rcvbuf)
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		perror("connect");
		exit(2);
	}
	return fd;
}

/* read records until the socket is empty for 100 ms */
typedef struct { int n, idr[4096]; size_t len[4096]; uint8_t fill[4096]; int bad; } recs_t;

static void read_records(int fd, preview_t *p, recs_t *r)
{
	static uint8_t buf[4 << 20];
	size_t have = 0;
	int idle = 0;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	memset(r, 0, sizeof(*r));
	while (idle < 20) {
		if (p)
			preview_poll(p);      /* lets the server send what is pending */
		ssize_t n = recv(fd, buf + have, sizeof(buf) - have, 0);
		if (n > 0) {
			have += n;
			idle = 0;
		} else {
			usleep(5000);
			idle++;
		}
		size_t off = 0;
		while (have - off >= 4) {
			size_t l = (size_t)buf[off] << 24 | buf[off + 1] << 16 | buf[off + 2] << 8 | buf[off + 3];
			if (l == 0 || l > PREVIEW_MAX_AU) {
				r->bad = 1;
				return;
			}
			if (have - off - 4 < l)
				break;
			const uint8_t *au = buf + off + 4;
			if (r->n < 4096) {
				r->idr[r->n] = preview_is_idr(au, l);
				r->len[r->n] = l;
				r->fill[r->n] = au[l - 1];
				r->n++;
			}
			off += 4 + l;
		}
		memmove(buf, buf + off, have - off);
		have -= off;
	}
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	static uint8_t au[1200 * 1024];
	size_t n;
	int port = 40000 + getpid() % 20000;
	preview_t p;
	recs_t r;

	/* NAL type detection */
	n = make_au(au, 1, 100, 0);
	CHECK("IDR access unit detected", preview_is_idr(au, n));
	n = make_au(au, 0, 100, 0);
	CHECK("P access unit is not IDR", !preview_is_idr(au, n));
	CHECK("3-byte start code", preview_is_idr((const uint8_t *)"\0\0\1\x26\x01\x00", 6));
	CHECK("junk is not IDR", !preview_is_idr((const uint8_t *)"\1\2\3\4\5\6", 6));

	/* port 0: off */
	CHECK("port 0 is off", preview_open(&p, "127.0.0.1", 0) == 0 && p.lfd < 0);
	preview_poll(&p);
	push_au(&p, au, n);
	preview_close(&p);

	CHECK("listen", preview_open(&p, "127.0.0.1", port) == 0 && p.lfd >= 0);
	n = make_au(au, 0, 5000, 7);
	push_au(&p, au, n);
	CHECK("no client: nothing allocated", p.au == NULL && p.out == NULL);

	/* client: waits for an IDR, then gets every frame in order */
	int c = connect_to(port, 0);
	usleep(20000);
	preview_poll(&p);
	CHECK("client accepted, IDR requested", p.cfd >= 0 && preview_take_idr_request(&p) == 1);
	CHECK("IDR request taken once", preview_take_idr_request(&p) == 0);
	n = make_au(au, 0, 5000, 1);
	push_au(&p, au, n);                     /* skipped: no IDR yet */
	n = make_au(au, 1, 30000, 2);
	push_au(&p, au, n);
	for (int i = 0; i < 20; i++) {
		n = make_au(au, 0, 4000 + i * 100, 10 + i);
		push_au(&p, au, n);
		preview_poll(&p);
	}
	read_records(c, &p, &r);
	int order_ok = r.n == 21 && r.idr[0] && r.len[0] == 30000 && r.fill[0] == 2;
	for (int i = 1; order_ok && i < 21; i++)
		order_ok = !r.idr[i] && r.len[i] == (size_t)(4000 + (i - 1) * 100) && r.fill[i] == 10 + i - 1;
	CHECK("starts at the IDR, all frames intact and in order", !r.bad && order_ok);
	CHECK("no drops with a reading client", p.dropped == 0);

	/* a client that stops reading: the loop never blocks, frames drop,
	 * the stream stays well formed and resumes at an IDR */
	close(c);
	usleep(20000);
	preview_poll(&p);
	CHECK("closed client noticed", p.cfd < 0 && p.au == NULL && p.out == NULL);
	c = connect_to(port, 4096);
	usleep(20000);
	preview_poll(&p);
	preview_take_idr_request(&p);
	double t0 = now();
	n = make_au(au, 1, 100000, 3);
	push_au(&p, au, n);
	for (int i = 0; i < 200; i++) {
		n = make_au(au, 0, 100000, 4);
		push_au(&p, au, n);
		preview_poll(&p);
	}
	double dt = now() - t0;
	CHECK("stalled client does not block the loop", dt < 0.5);
	CHECK("frames dropped and an IDR requested", p.dropped > 0 && preview_take_idr_request(&p) == 1);
	n = make_au(au, 0, 1000, 5);
	push_au(&p, au, n);                     /* still waiting for an IDR: skipped */
	read_records(c, &p, &r);                /* drain what was queued */
	n = make_au(au, 1, 2000, 6);
	push_au(&p, au, n);
	n = make_au(au, 0, 1000, 7);
	push_au(&p, au, n);
	recs_t r2;
	read_records(c, &p, &r2);
	CHECK("stream well formed after drops", !r.bad && !r2.bad && r.n >= 1 && r.idr[0]);
	CHECK("resumes at the next IDR", r2.n == 2 && r2.idr[0] && r2.fill[0] == 6 && r2.fill[1] == 7);

	/* oversize access unit is dropped, the next IDR goes through */
	n = make_au(au, 0, PREVIEW_MAX_AU + 100, 8);
	push_au(&p, au, n);
	n = make_au(au, 1, 3000, 9);
	push_au(&p, au, n);
	read_records(c, &p, &r);
	CHECK("oversize AU dropped, next IDR sent", !r.bad && r.n == 1 && r.fill[0] == 9);

	/* a second client replaces the first */
	int c2 = connect_to(port, 0);
	usleep(20000);
	preview_poll(&p);
	CHECK("new client replaces the old one", p.cfd >= 0 && p.clients == 3);
	char x;
	fcntl(c, F_SETFL, O_NONBLOCK);
	usleep(20000);
	CHECK("old client was closed", recv(c, &x, 1, 0) == 0);
	close(c);
	close(c2);
	preview_close(&p);

	printf("%d/%d passed\n", passed, passed + failed);
	return failed ? 1 : 0;
}
