/* Host test for src/osd_ext.c: parsing, grid scaling, TTL, the UDP socket.
 * The osd_* drawing calls are mocked and recorded. */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/osd_ext.h"

struct osd { int w, h; };
static char g_log[4096];

static void logf_(const char *s)
{
	strncat(g_log, s, sizeof g_log - strlen(g_log) - 1);
}

int osd_width(const osd_t *o) { return o->w; }
int osd_height(const osd_t *o) { return o->h; }
void osd_box(osd_t *o, int x, int y, int w, int h)
{
	(void)o; char b[64]; snprintf(b, sizeof b, "box %d %d %d %d;", x, y, w, h); logf_(b);
}
void osd_cross(osd_t *o, int x, int y, int s)
{
	(void)o; char b[64]; snprintf(b, sizeof b, "cross %d %d %d;", x, y, s); logf_(b);
}
void osd_text(osd_t *o, int x, int y, const char *t)
{
	(void)o; char b[96]; snprintf(b, sizeof b, "text %d %d [%s];", x, y, t); logf_(b);
}

static int fails;
static void check(const char *name, int ok)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", name);
	fails += !ok;
}

static const char *draw(osd_t *o, const osd_ext_t *e, uint64_t now)
{
	g_log[0] = 0;
	osd_ext_draw(o, e, now, 500);
	return g_log;
}

#define P(e, s, t) osd_ext_parse(e, s, strlen(s), t)

int main(void)
{
	osd_ext_t e;
	osd_ext_init(&e);
	osd_t link = { 1280, 720 }, dvr = { 1920, 1072 };

	check("three shapes", P(&e, "box 100 200 300 400\ntext 10 20 TAG 7  d=1.2m\ncross 500 500 50\n", 1000) == 3);
	check("link canvas scaled",
	      !strcmp(draw(&link, &e, 1000), "box 128 144 384 288;text 12 14 [TAG 7  d=1.2m];cross 640 360 36;"));
	check("dvr canvas scaled",
	      !strcmp(draw(&dvr, &e, 1000), "box 192 214 576 428;text 19 21 [TAG 7  d=1.2m];cross 960 536 53;"));
	check("still drawn at the ttl", draw(&link, &e, 1500)[0] != 0);
	check("gone after the ttl", draw(&link, &e, 1501)[0] == 0);

	P(&e, "box 1 1 10 10", 2000);
	check("next datagram replaces the set", e.n == 1 && !strcmp(draw(&link, &e, 2000), "box 1 0 12 7;"));
	P(&e, "", 2100);
	check("empty datagram clears", e.n == 0 && draw(&link, &e, 2100)[0] == 0);
	P(&e, "box 1 1 10 10\nclear\ntext 0 0 after", 2200);
	check("clear drops what came before it", e.n == 1 && e.s[0].kind == OSD_EXT_TEXT_SHAPE);

	unsigned long long b0 = e.bad_lines;
	P(&e, "box 1 2 3\nbox 1 2 3 4 5\nbox 1 2 0 4\nbox a b c d\nline 1 2 3 4\n"
	      "cross 1 2\ntext 5\ntext 5 6\nboxx 1 2 3 4\nbox 99999 1 2 3\nclear now\n"
	      "  # comment\n\n\r\nbox -50 -50 100 100\r\n", 3000);
	check("bad lines skipped and counted", e.bad_lines - b0 == 11);
	check("good line among bad ones kept, negative allowed", e.n == 1 && e.s[0].x == -50);

	char big[64 * 20 + 100] = "";
	for (int i = 0; i < 70; i++)
		strcat(big, "cross 1 1 1\n");
	b0 = e.bad_lines;
	P(&e, big, 4000);
	check("at most OSD_EXT_MAX shapes", e.n == OSD_EXT_MAX && e.bad_lines - b0 == 70 - OSD_EXT_MAX);

	char lng[200] = "text 0 0 ";
	memset(lng + strlen(lng), 'x', 150);
	P(&e, lng, 5000);
	check("long text cut", e.n == 1 && strlen(e.s[0].text) == OSD_EXT_TEXT - 1);
	char ctl[] = "text 0 0 a\tb\x01" "c";
	P(&e, ctl, 5000);
	check("control bytes shown as ?", !strcmp(e.s[0].text, "a?b?c"));
	char nul[] = "text 0 0 a\0b\nbox 1 1 9 9";
	osd_ext_parse(&e, nul, sizeof nul - 1, 5000);
	check("NUL inside text does not end the datagram", e.n == 2 && e.s[1].kind == OSD_EXT_BOX);

	/* socket: 127.0.0.1 only, last datagram wins */
	int fd = osd_ext_open(24580);
	check("socket opened", fd >= 0);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(24580) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sendto(s, "box 1 1 10 10", 13, 0, (struct sockaddr *)&a, sizeof a);
	sendto(s, "text 1 1 newest", 15, 0, (struct sockaddr *)&a, sizeof a);
	usleep(20000);
	check("poll reports a change", osd_ext_poll(&e, fd, 6000) == 1);
	check("last datagram wins", e.n == 1 && !strcmp(e.s[0].text, "newest"));
	check("nothing new: no change", osd_ext_poll(&e, fd, 6001) == 0);
	FILE *f = fopen("/proc/net/udp", "r");
	char line[256];
	int lo = 0, any = 0;
	while (f && fgets(line, sizeof line, f)) {
		if (strstr(line, ":6004 ")) {     /* 24580 = 0x6004 */
			any++;
			lo += strstr(line, "0100007F:6004") != NULL;
		}
	}
	if (f)
		fclose(f);
	check("bound to 127.0.0.1 only", any && lo == any);
	close(s);
	close(fd);

	printf(fails ? "%d osd_ext test(s) FAILED\n" : "all osd_ext tests passed\n", fails);
	return fails != 0;
}
