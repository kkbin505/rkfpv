/*
 * dvrhttpd: the DVR recordings over HTTP, for hotspot mode.
 *
 *   GET  /                  page: list, download, delete (works on a phone)
 *   GET  /api/list          {"files":[{"name","size"}],"free_mb","total_mb"}
 *   GET  /dvr/<name>        the file; Range requests (resume, seeking)
 *   HEAD /dvr/<name>
 *   DELETE /dvr/<name>      needs header "X-FPV-Confirm: 1"
 *   POST /api/delete-all    needs header "X-FPV-Confirm: 1"
 *   POST /api/fpv           body "status" / "set bitrate 4000" ...: runs one
 *                           `fpv ... --json` (fpvcmd.c: whitelist, safe
 *                           characters, no shell); needs "X-FPV-Confirm: 1"
 *   GET  /live              rkfpv's preview stream (127.0.0.1:<preview port>)
 *                           relayed as is: u32 big-endian length + Annex-B
 *                           access unit, starting at an IDR. The newest
 *                           viewer wins (rkfpv keeps one client).
 *   OPTIONS *               CORS preflight, answered for the -o origin only
 *
 * Only names like rec_0003_01.mp4 / .ts / .bbl (bblog's flight-controller
 * blackbox log) in the DVR directory are served or deleted; no other path
 * exists. The file being recorded (rkfpv's /tmp/dvr.status) and the
 * blackbox log being written (/tmp/bblog.status) are never deleted.
 *
 * Who may change things: every request that changes something needs the
 * custom header X-FPV-Confirm. A page from another site can only send it
 * after a CORS preflight, and the preflight is answered for exactly one
 * origin: the hosted web configurator (-o, e.g. https://kkbin505.github.io;
 * none without -o). The browser itself blocks every other site. Only phones
 * that know the WPA2 hotspot password reach this server at all.
 *
 * Captive portal: in hotspot mode dnsmasq answers every DNS name with this
 * board, so a phone's connectivity check (connectivitycheck.gstatic.com/
 * generate_204, captive.apple.com/hotspot-detect.html, msftconnecttest.com/
 * connecttest.txt, ...) lands here. Any request whose Host is not this
 * board's address gets a 302 to the recordings page, and the phone pops up
 * its sign-in window showing it. Requests to the board's own address are
 * served as listed above.
 *
 *   dvrhttpd [-b 10.0.0.1] [-p 80] [-d /mnt/dvr] [-s /tmp/dvr.status]
 *            [-B /tmp/bblog.status] [-o https://origin] [-f /usr/bin/fpv] [-P 5602]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fpvcmd.h"

#define MAX_REQ 8192
#define MAX_BODY 4096
#define MAX_CHILDREN 6
#define FPV_TIMEOUT_MS 30000

static const char *g_dir = "/mnt/dvr";
static const char *g_status = "/tmp/dvr.status";
static const char *g_bbstatus = "/tmp/bblog.status";
static const char *g_host = "10.0.0.1";   /* our address: other Hosts are redirected */
static int g_port = 80;
static const char *g_origin;              /* the one origin CORS is answered for */
static const char *g_fpv = "/usr/bin/fpv";
static int g_preview_port = 5602;
/* CORS response headers for this request ("" unless the Origin is g_origin);
 * one process per request, so a global is per request */
static char g_cors[512];
static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static int valid_name(const char *n)
{
	/* rec_<digits>_<digits>.mp4|ts|bbl */
	size_t len = strlen(n);
	if (len > 64 || strncmp(n, "rec_", 4))
		return 0;
	const char *p = n + 4;
	if (!isdigit((unsigned char)*p))
		return 0;
	while (isdigit((unsigned char)*p))
		p++;
	if (*p++ != '_' || !isdigit((unsigned char)*p))
		return 0;
	while (isdigit((unsigned char)*p))
		p++;
	return !strcmp(p, ".mp4") || !strcmp(p, ".ts") || !strcmp(p, ".bbl");
}

/* file= of a status file whose state= is `state`, "" if not */
static void busy_name(const char *path, const char *state, char *out, size_t n)
{
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[512], want[32];
	snprintf(want, sizeof want, "state=%s ", state);
	if (fgets(line, sizeof line, f)) {
		char *p = strstr(line, " file=");
		if (p && !strncmp(line, want, strlen(want))) {
			p += 6;
			size_t i = 0;
			while (p[i] && p[i] != ' ' && p[i] != '\n' && i + 1 < n) {
				out[i] = p[i];
				i++;
			}
			out[i] = '\0';
		}
	}
	fclose(f);
}

/* the file rkfpv is writing, "" if none */
static void recording_name(char *out, size_t n)
{
	busy_name(g_status, "recording", out, n);
}

/* the file being recorded or the blackbox log being written: not deletable */
static int in_use(const char *name)
{
	char b[80];
	recording_name(b, sizeof b);
	if (!strcmp(name, b))
		return 1;
	busy_name(g_bbstatus, "logging", b, sizeof b);
	return !strcmp(name, b);
}

static void send_all(int fd, const char *b, size_t n)
{
	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
		if (w <= 0)
			return;
		b += w;
		n -= (size_t)w;
	}
}

static void reply_n(int fd, int code, const char *status, const char *ctype,
		    const char *body, size_t n)
{
	char h[1024];
	int hl = snprintf(h, sizeof h,
			  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			  "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n",
			  code, status, ctype, n, g_cors);
	send_all(fd, h, (size_t)hl);
	if (n)
		send_all(fd, body, n);
}

static void reply(int fd, int code, const char *status, const char *ctype, const char *body)
{
	reply_n(fd, code, status, ctype, body, body ? strlen(body) : 0);
}

/* captive portal: send a phone's connectivity check to the recordings page */
static void redirect_home(int fd)
{
	char h[256], port[16] = "";
	if (g_port != 80)
		snprintf(port, sizeof port, ":%d", g_port);
	int hl = snprintf(h, sizeof h,
			  "HTTP/1.1 302 Found\r\nLocation: http://%s%s/\r\nContent-Length: 0\r\n"
			  "Cache-Control: no-store\r\nConnection: close\r\n\r\n", g_host, port);
	send_all(fd, h, (size_t)hl);
}

/* 1 if the Host header names some other site (port ignored) */
static int foreign_host(const char *host)
{
	size_t n = strlen(g_host);
	if (!host)
		return 0;               /* HTTP/1.0 without Host: serve normally */
	return strncasecmp(host, g_host, n) || (host[n] && host[n] != ':');
}

static void json_error(int fd, int code, const char *status, const char *msg)
{
	char b[256];
	snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", msg);
	reply(fd, code, status, "application/json", b);
}

static void space(long long *total_mb, long long *free_mb)
{
	struct statvfs s;
	*total_mb = *free_mb = 0;
	if (!statvfs(g_dir, &s)) {
		*total_mb = (long long)s.f_blocks * s.f_frsize / 1048576;
		*free_mb = (long long)s.f_bavail * s.f_frsize / 1048576;
	}
}

static void api_list(int fd)
{
	size_t cap = 4096, n = 0;
	char *b = malloc(cap);
	if (!b)
		return json_error(fd, 500, "Internal Server Error", "out of memory");
	n += (size_t)snprintf(b, cap, "{\"ok\":true,\"files\":[");
	DIR *d = opendir(g_dir);
	int first = 1;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!valid_name(e->d_name))
			continue;
		char p[512];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
		if (lstat(p, &st) || !S_ISREG(st.st_mode))
			continue;
		if (n + 200 > cap) {
			char *nb = realloc(b, cap *= 2);
			if (!nb)
				break;
			b = nb;
		}
		n += (size_t)snprintf(b + n, cap - n, "%s{\"name\":\"%s\",\"size\":%lld}",
				      first ? "" : ",", e->d_name, (long long)st.st_size);
		first = 0;
	}
	if (d)
		closedir(d);
	long long total, freemb;
	space(&total, &freemb);
	char rec[80];
	recording_name(rec, sizeof rec);
	if (n + 200 > cap) {
		char *nb = realloc(b, cap + 200);
		if (nb) { b = nb; cap += 200; }
	}
	snprintf(b + n, cap - n, "],\"total_mb\":%lld,\"free_mb\":%lld,\"recording\":\"%s\"}",
		 total, freemb, rec);
	reply(fd, 200, "OK", "application/json", b);
	free(b);
}

/* "bytes=a-b" / "bytes=a-" / "bytes=-n" -> [*from, *to]; 0 ok, -1 bad */
static int parse_range(const char *r, long long size, long long *from, long long *to)
{
	if (strncmp(r, "bytes=", 6))
		return -1;
	r += 6;
	char *end;
	if (*r == '-') {
		long long n = strtoll(r + 1, &end, 10);
		if (end == r + 1 || n <= 0)
			return -1;
		*from = n >= size ? 0 : size - n;
		*to = size - 1;
		return 0;
	}
	long long a = strtoll(r, &end, 10);
	if (end == r || *end != '-' || a < 0 || a >= size)
		return -1;
	r = end + 1;
	long long b = size - 1;
	if (*r && *r != ',') {
		b = strtoll(r, &end, 10);
		if (end == r || b < a)
			return -1;
		if (b > size - 1)
			b = size - 1;
	}
	*from = a;
	*to = b;
	return 0;
}

static void serve_file(int fd, const char *name, const char *range, int head, int attach)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", g_dir, name);
	int f = open(p, O_RDONLY | O_NOFOLLOW);
	struct stat st;
	if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) {
		if (f >= 0)
			close(f);
		return json_error(fd, 404, "Not Found", "no such recording");
	}
	long long size = st.st_size, from = 0, to = size - 1;
	int partial = 0;
	if (range && size > 0) {
		if (parse_range(range, size, &from, &to)) {
			char h[1024];
			int hl = snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\n"
					  "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n"
					  "%sConnection: close\r\n\r\n", size, g_cors);
			send_all(fd, h, (size_t)hl);
			close(f);
			return;
		}
		partial = 1;
	}
	long long len = size ? to - from + 1 : 0;
	const char *ct = strstr(name, ".ts") ? "video/mp2t" :
			 strstr(name, ".bbl") ? "application/octet-stream" : "video/mp4";
	char h[1280];
	int hl;
	if (partial)
		hl = snprintf(h, sizeof h, "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
			      "Content-Length: %lld\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
			      "Accept-Ranges: bytes\r\n%sConnection: close\r\n\r\n",
			      ct, len, from, to, size, g_cors);
	else
		hl = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
			      "Content-Length: %lld\r\nAccept-Ranges: bytes\r\n"
			      "Content-Disposition: %s; filename=\"%s\"\r\n%sConnection: close\r\n\r\n",
			      ct, len, attach ? "attachment" : "inline", name, g_cors);
	send_all(fd, h, (size_t)hl);
	if (!head) {
		off_t off = (off_t)from;
		long long left = len;
		while (left > 0) {
			ssize_t s = sendfile(fd, f, &off, left > (1 << 20) ? (1 << 20) : (size_t)left);
			if (s <= 0)
				break;
			left -= s;
		}
	}
	close(f);
}

static void do_delete(int fd, const char *name)
{
	char p[512];
	if (in_use(name))
		return json_error(fd, 409, "Conflict", "this file is being recorded");
	snprintf(p, sizeof p, "%s/%s", g_dir, name);
	struct stat st;
	if (lstat(p, &st) || !S_ISREG(st.st_mode))
		return json_error(fd, 404, "Not Found", "no such recording");
	if (unlink(p))
		return json_error(fd, 500, "Internal Server Error", "delete failed");
	sync();
	reply(fd, 200, "OK", "application/json", "{\"ok\":true,\"deleted\":1}");
}

static void do_delete_all(int fd)
{
	int n = 0;
	DIR *d = opendir(g_dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!valid_name(e->d_name) || in_use(e->d_name))
			continue;
		char p[512];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
		if (!lstat(p, &st) && S_ISREG(st.st_mode) && !unlink(p))
			n++;
	}
	if (d)
		closedir(d);
	sync();
	char b[64];
	snprintf(b, sizeof b, "{\"ok\":true,\"deleted\":%d}", n);
	reply(fd, 200, "OK", "application/json", b);
}

/* 1 if the request's Origin is the one we serve CORS for */
static int origin_ok(const char *origin)
{
	return g_origin && origin && !strcmp(origin, g_origin);
}

/* CORS preflight: only for the configured origin; anyone else gets no
 * Access-Control headers, so the browser refuses the real request */
static void preflight(int fd, const char *origin, int private_net)
{
	if (!origin_ok(origin))
		return json_error(fd, 403, "Forbidden", "origin not allowed");
	char h[1024];
	int hl = snprintf(h, sizeof h,
			  "HTTP/1.1 204 No Content\r\n"
			  "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n"
			  "Access-Control-Allow-Methods: GET, HEAD, POST, DELETE\r\n"
			  "Access-Control-Allow-Headers: X-FPV-Confirm, Content-Type, Range\r\n"
			  "%sAccess-Control-Max-Age: 600\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
			  g_origin, private_net ? "Access-Control-Allow-Private-Network: true\r\n" : "");
	send_all(fd, h, (size_t)hl);
}

/* POST /api/fpv: body = fpv arguments separated by blanks */
static void api_fpv(int fd, char *body)
{
	char *args[FPVCMD_MAX_ARGS + 1];
	int n = 0;
	for (char *s = strtok(body, " \t\r\n"); s; s = strtok(NULL, " \t\r\n")) {
		if (n == FPVCMD_MAX_ARGS)
			return json_error(fd, 400, "Bad Request", "too many arguments");
		args[n++] = s;
	}
	size_t len = 0;
	char *out = fpvcmd_run(g_fpv, args, n, FPV_TIMEOUT_MS, &len);
	if (!out)
		return json_error(fd, 500, "Internal Server Error", "out of memory");
	reply_n(fd, 200, "OK", "application/json", out, len);
	free(out);
}

/* GET /live: relay rkfpv's preview stream until either side goes away */
static void live(int fd)
{
	int up = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)g_preview_port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (up < 0 || connect(up, (struct sockaddr *)&a, sizeof a)) {
		if (up >= 0)
			close(up);
		return json_error(fd, 503, "Service Unavailable", "preview not running (fpv preview on)");
	}
	/* no data for 5 s: rkfpv stopped or restarts; the page reconnects */
	struct timeval tv = { 5, 0 };
	setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	char h[1024];
	int hl = snprintf(h, sizeof h,
			  "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
			  "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n", g_cors);
	send_all(fd, h, (size_t)hl);
	char buf[64 * 1024];
	for (;;) {
		ssize_t r = recv(up, buf, sizeof buf, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		/* a slow phone blocks us here; rkfpv then drops frames on its
		 * side instead of queueing them: the picture stays current */
		const char *p = buf;
		size_t left = (size_t)r;
		while (left) {
			ssize_t w = send(fd, p, left, MSG_NOSIGNAL);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				goto out;
			p += w;
			left -= (size_t)w;
		}
	}
out:
	close(up);
}

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>rkfpv DVR</title>"
"<style>body{font:15px system-ui,sans-serif;margin:16px;background:#f5f3ee;color:#26231f}"
"h1{font-size:20px}h1 b{color:#f54700}table{border-collapse:collapse;width:100%;max-width:720px}"
"td{padding:8px 6px;border-bottom:1px solid #e2ddd3}td.s{color:#77716a;white-space:nowrap}"
"a,button{font:inherit}button{border:1px solid #e2ddd3;background:#fff;border-radius:4px;padding:5px 10px;cursor:pointer}"
"button.d{color:#d23c2f}a.b{background:#f54700;color:#fff;border-radius:4px;padding:5px 10px;text-decoration:none}"
"#sp{color:#77716a;margin:8px 0}#tip{font-size:13px;color:#77716a;max-width:720px}</style></head><body>"
"<h1>rk<b>fpv</b> recordings</h1><div id=sp></div>"
"<p id=tip>下载没反应？关掉这个弹窗，选“保持连接 / 不使用互联网”，关闭手机流量，再用浏览器打开 "
"<b>http://<span id=ip></span>/</b></p>"
"<p><button class=d onclick=delAll()>Delete all</button></p><table id=t></table>"
"<script>"
"const H={'X-FPV-Confirm':'1'};document.getElementById('ip').textContent=location.host;"
"function mb(b){return (b/1048576).toFixed(1)+' MB'}"
"async function load(){const r=await (await fetch('/api/list')).json();"
"document.getElementById('sp').textContent=(r.free_mb/1024).toFixed(1)+' GB free of '+(r.total_mb/1024).toFixed(1)+' GB';"
"r.files.sort((a,b)=>a.name<b.name?1:-1);const t=document.getElementById('t');t.innerHTML='';"
"if(!r.files.length)t.innerHTML='<tr><td>No recordings.</td></tr>';"
"for(const f of r.files){const tr=document.createElement('tr');"
"tr.innerHTML=`<td>${f.name}</td><td class=s>${mb(f.size)}</td><td><a class=b href='/dvr/${f.name}' download>Download</a></td>"
"<td><button class=d>Delete</button></td>`;"
"tr.querySelector('button').onclick=()=>del(f.name);t.appendChild(tr)}}"
"async function del(n){if(!confirm('Delete '+n+'?'))return;const r=await fetch('/dvr/'+n,{method:'DELETE',headers:H});"
"if(!r.ok)alert((await r.json()).error);load()}"
"async function delAll(){if(!confirm('Delete ALL recordings?'))return;"
"const r=await (await fetch('/api/delete-all',{method:'POST',headers:H})).json();alert('Deleted '+r.deleted);load()}"
"load()</script></body></html>";

/* copy a header value (after "Name:") into out; stop at a blank if asked */
static const char *header_value(const char *h, char *out, size_t n, int stop_space)
{
	while (*h == ' ')
		h++;
	size_t i = 0;
	while (h[i] && h[i] != '\r' && !(stop_space && h[i] == ' ') && i + 1 < n) {
		out[i] = h[i];
		i++;
	}
	out[i] = '\0';
	return out;
}

static void handle(int fd)
{
	struct timeval tv = { 10, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	char req[MAX_REQ + 1];
	size_t n = 0;
	while (n < MAX_REQ) {
		ssize_t r = recv(fd, req + n, MAX_REQ - n, 0);
		if (r <= 0)
			return;
		n += (size_t)r;
		req[n] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	if (!strstr(req, "\r\n\r\n"))
		return json_error(fd, 431, "Request Header Fields Too Large", "bad request");

	char *hdr_end = strstr(req, "\r\n\r\n");
	char method[8], path[256];
	if (sscanf(req, "%7s %255s", method, path) != 2)
		return json_error(fd, 400, "Bad Request", "bad request");
	int attach = 0;                 /* ?dl=1: save instead of play */
	char *q = strchr(path, '?');
	if (q) {
		attach = strstr(q, "dl=1") != NULL;
		*q = '\0';
	}

	/* headers we care about */
	const char *range = NULL, *host = NULL, *origin = NULL;
	int confirm = 0, private_net = 0;
	long clen = 0;
	char rangebuf[128], hostbuf[128], originbuf[128];
	for (char *line = strstr(req, "\r\n"); line && line < hdr_end; line = strstr(line + 2, "\r\n")) {
		char *h = line + 2;
		if (!strncasecmp(h, "Range:", 6)) {
			range = header_value(h + 6, rangebuf, sizeof rangebuf, 0);
		} else if (!strncasecmp(h, "Host:", 5)) {
			host = header_value(h + 5, hostbuf, sizeof hostbuf, 1);
		} else if (!strncasecmp(h, "Origin:", 7)) {
			origin = header_value(h + 7, originbuf, sizeof originbuf, 1);
		} else if (!strncasecmp(h, "X-FPV-Confirm:", 14)) {
			h += 14;
			while (*h == ' ')
				h++;
			confirm = *h == '1';
		} else if (!strncasecmp(h, "Content-Length:", 15)) {
			clen = strtol(h + 15, NULL, 10);
		} else if (!strncasecmp(h, "Access-Control-Request-Private-Network:", 39)) {
			h += 39;
			while (*h == ' ')
				h++;
			private_net = !strncasecmp(h, "true", 4);
		}
	}
	if (origin_ok(origin))
		snprintf(g_cors, sizeof g_cors,
			 "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n"
			 "Access-Control-Expose-Headers: Content-Length, Content-Range\r\n", g_origin);

	if (foreign_host(host))
		return redirect_home(fd);
	if (!strcmp(method, "OPTIONS"))
		return preflight(fd, origin, private_net);
	int get = !strcmp(method, "GET"), head = !strcmp(method, "HEAD");
	if ((get || head) && !strcmp(path, "/"))
		return reply(fd, 200, "OK", "text/html; charset=utf-8", head ? NULL : PAGE);
	if (get && !strcmp(path, "/api/list"))
		return api_list(fd);
	if (get && !strcmp(path, "/live"))
		return live(fd);
	if (!strcmp(path, "/api/fpv")) {
		if (strcmp(method, "POST"))
			return json_error(fd, 405, "Method Not Allowed", "use POST");
		if (!confirm)
			return json_error(fd, 403, "Forbidden", "missing X-FPV-Confirm header");
		size_t have = n - (size_t)(hdr_end + 4 - req);
		if (clen < 0 || clen > MAX_BODY) {
			/* read (a bounded amount of) the rest first: closing with
			 * unread data resets the connection and loses the reply */
			char junk[4096];
			long left = (clen > 65536 ? 65536 : clen) - (long)have;
			while (left > 0) {
				ssize_t r = recv(fd, junk, left > (long)sizeof junk ? sizeof junk : (size_t)left, 0);
				if (r <= 0)
					break;
				left -= r;
			}
			return json_error(fd, 413, "Payload Too Large", "body too large");
		}
		char body[MAX_BODY + 1];
		if (have > (size_t)clen)
			have = (size_t)clen;
		memcpy(body, hdr_end + 4, have);
		while (have < (size_t)clen) {
			ssize_t r = recv(fd, body + have, (size_t)clen - have, 0);
			if (r <= 0)
				return json_error(fd, 400, "Bad Request", "body cut short");
			have += (size_t)r;
		}
		body[have] = '\0';
		return api_fpv(fd, body);
	}
	if (!strncmp(path, "/dvr/", 5)) {
		const char *name = path + 5;
		if (!valid_name(name))
			return json_error(fd, 404, "Not Found", "not a recording name");
		if (get || head)
			return serve_file(fd, name, range, head, attach);
		if (!strcmp(method, "DELETE")) {
			if (!confirm)
				return json_error(fd, 403, "Forbidden", "missing X-FPV-Confirm header");
			return do_delete(fd, name);
		}
		return json_error(fd, 405, "Method Not Allowed", "method not allowed");
	}
	if (!strcmp(path, "/api/delete-all")) {
		if (strcmp(method, "POST"))
			return json_error(fd, 405, "Method Not Allowed", "use POST");
		if (!confirm)
			return json_error(fd, 403, "Forbidden", "missing X-FPV-Confirm header");
		return do_delete_all(fd);
	}
	json_error(fd, 404, "Not Found", "not found");
}

int main(int argc, char **argv)
{
	const char *bind_ip = "10.0.0.1";
	int port = 80, c;
	while ((c = getopt(argc, argv, "b:p:d:s:B:o:f:P:h")) != -1) {
		switch (c) {
		case 'b': bind_ip = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'd': g_dir = optarg; break;
		case 's': g_status = optarg; break;
		case 'B': g_bbstatus = optarg; break;
		case 'o': g_origin = *optarg ? optarg : NULL; break;
		case 'f': g_fpv = optarg; break;
		case 'P': g_preview_port = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: dvrhttpd [-b ip] [-p port] [-d dir] [-s dvr.status]\n"
				"                [-B bblog.status] [-o https://origin] [-f fpv] [-P preview port]\n");
			return c == 'h' ? 0 : 1;
		}
	}
	g_host = bind_ip;
	g_port = port;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1 || bind(s, (struct sockaddr *)&a, sizeof a) ||
	    listen(s, 8)) {
		perror("dvrhttpd");
		return 1;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "dvrhttpd: http://%s:%d/ serving %s, web configurator origin %s\n",
		bind_ip, port, g_dir, g_origin ? g_origin : "(none)");

	int children = 0;
	while (!g_quit) {
		while (waitpid(-1, NULL, WNOHANG) > 0)
			children--;
		int fd = accept(s, NULL, NULL);
		if (fd < 0)
			continue;
		while (waitpid(-1, NULL, WNOHANG) > 0)     /* downloads that ended meanwhile */
			children--;
		if (children >= MAX_CHILDREN) {
			json_error(fd, 503, "Service Unavailable", "busy");
			close(fd);
			continue;
		}
		pid_t pid = fork();
		if (pid == 0) {                 /* one process per download: a long one blocks nothing */
			close(s);
			handle(fd);
			shutdown(fd, SHUT_WR);
			close(fd);
			_exit(0);
		}
		if (pid > 0)
			children++;
		close(fd);
	}
	close(s);
	return 0;
}
