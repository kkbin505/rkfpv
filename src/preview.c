#define _GNU_SOURCE             /* accept4 */
#include "preview.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SNDBUF (512 * 1024)

int preview_open(preview_t *p, const char *ip, int port)
{
	memset(p, 0, sizeof(*p));
	p->lfd = p->cfd = -1;
	if (port <= 0)
		return 0;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("preview: socket");
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 ||
	    bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) {
		fprintf(stderr, "preview: cannot listen on %s:%d: %s\n", ip, port, strerror(errno));
		close(fd);
		return -1;
	}
	p->lfd = fd;
	fprintf(stderr, "preview: listening on tcp %s:%d\n", ip, port);
	return 0;
}

static void drop_client(preview_t *p, const char *why)
{
	if (p->cfd < 0)
		return;
	close(p->cfd);
	p->cfd = -1;
	free(p->au);
	free(p->out);
	p->au = p->out = NULL;
	p->au_len = p->au_cap = p->out_cap = p->out_len = p->out_off = 0;
	fprintf(stderr, "preview: client gone (%s), %llu frames sent, %llu dropped\n",
		why, (unsigned long long)p->frames, (unsigned long long)p->dropped);
}

void preview_close(preview_t *p)
{
	drop_client(p, "exit");
	if (p->lfd >= 0)
		close(p->lfd);
	p->lfd = -1;
}

/* send what is pending; 0 = all sent, 1 = some left, -1 = client dropped */
static int flush(preview_t *p)
{
	while (p->out_off < p->out_len) {
		ssize_t n = send(p->cfd, p->out + p->out_off, p->out_len - p->out_off,
				 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			p->out_off += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 1;
		if (n < 0 && errno == EINTR)
			continue;
		drop_client(p, n == 0 ? "closed" : strerror(errno));
		return -1;
	}
	p->out_len = p->out_off = 0;
	return 0;
}

void preview_poll(preview_t *p)
{
	if (p->lfd < 0)
		return;
	int fd = accept4(p->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd >= 0) {
		if (p->cfd >= 0)
			drop_client(p, "replaced by a new client");
		int v = SNDBUF;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
		v = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
		p->cfd = fd;
		p->need_idr = p->want_idr = 1;
		p->frames = p->dropped = 0;
		p->clients++;
		fprintf(stderr, "preview: client connected\n");
	}
	if (p->cfd < 0)
		return;
	/* the client never sends anything: readable means closed */
	char c;
	ssize_t n = recv(p->cfd, &c, 1, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		drop_client(p, n == 0 ? "closed" : strerror(errno));
		return;
	}
	if (p->out_len)
		flush(p);
}

int preview_is_idr(const uint8_t *b, size_t len)
{
	/* walk the NAL units up to the first slice: VPS/SPS/PPS or an IRAP
	 * slice (types 16..23) mean the decoder can start here */
	for (size_t i = 0; i + 4 < len; i++) {
		if (b[i] || b[i + 1] || b[i + 2] != 1)
			continue;
		int t = (b[i + 3] >> 1) & 0x3f;
		if (t == 32 || (t >= 16 && t <= 23))
			return 1;
		if (t < 32)
			return 0;       /* a non-IRAP slice came first */
		i += 2;
	}
	return 0;
}

static void end_frame(preview_t *p)
{
	int bad = p->au_bad;
	size_t len = p->au_len;
	p->au_len = 0;
	p->au_bad = 0;
	if (bad || !len)
		goto drop;
	if (p->need_idr && !preview_is_idr(p->au + 4, len))
		return;             /* waiting for a picture the decoder can start at */
	if (p->out_len) {
		/* the previous frame is still on its way: no room, this one goes */
		if (flush(p) != 0)
			goto drop;
	}
	p->au[0] = len >> 24;
	p->au[1] = len >> 16;
	p->au[2] = len >> 8;
	p->au[3] = len;
	uint8_t *b = p->out;
	size_t cap = p->out_cap;
	p->out = p->au;
	p->out_cap = p->au_cap;
	p->au = b;
	p->au_cap = cap;
	p->out_len = len + 4;
	p->out_off = 0;
	p->need_idr = 0;
	p->frames++;
	flush(p);
	return;
drop:
	if (p->cfd < 0)
		return;
	p->dropped++;
	if (!p->need_idr)
		p->want_idr = 1;
	p->need_idr = 1;
}

void preview_push(preview_t *p, const uint8_t *data, size_t len, int end_of_au)
{
	if (p->cfd < 0)
		return;
	if (!p->au_bad) {
		size_t need = 4 + p->au_len + len;
		if (p->au_len + len > PREVIEW_MAX_AU) {
			p->au_bad = 1;
		} else {
			if (need > p->au_cap) {
				size_t cap = p->au_cap ? p->au_cap : 64 * 1024;
				while (cap < need)
					cap *= 2;
				uint8_t *n = realloc(p->au, cap);
				if (!n) {
					p->au_bad = 1;
					goto end;
				}
				p->au = n;
				p->au_cap = cap;
			}
			memcpy(p->au + 4 + p->au_len, data, len);
			p->au_len += len;
		}
	}
end:
	if (end_of_au)
		end_frame(p);
}

int preview_take_idr_request(preview_t *p)
{
	int r = p->want_idr;
	p->want_idr = 0;
	return r;
}
