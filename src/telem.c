#include "telem.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* MAVLink message ids, payload lengths and CRC_EXTRA (common.xml) */
#define MSG_HEARTBEAT       0
#define MSG_SYS_STATUS      1
#define MSG_RC_CHANNELS_RAW 35
#define MSG_RC_CHANNELS     65
#define MSG_BATTERY_STATUS  147

static const struct { uint32_t id; uint8_t len, crc_extra; } MSGS[] = {
	{ MSG_HEARTBEAT, 9, 50 },
	{ MSG_SYS_STATUS, 31, 124 },
	{ MSG_RC_CHANNELS_RAW, 22, 244 },
	{ MSG_RC_CHANNELS, 42, 118 },
	{ MSG_BATTERY_STATUS, 36, 154 },
};

#define MAV_AUTOPILOT_INVALID 8
#define MAV_MODE_FLAG_SAFETY_ARMED 0x80

void telem_init(telem_t *t)
{
	memset(t, 0, sizeof *t);
	t->amps = -1;
	t->remaining = -1;
	t->mah = -1;
	t->rc_rssi = -1;
}

static uint16_t crc_acc(uint8_t b, uint16_t crc)
{
	uint8_t tmp = b ^ (uint8_t)(crc & 0xff);
	tmp ^= (uint8_t)(tmp << 4);
	return (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void handle(telem_t *t, uint32_t id, const uint8_t *p, uint8_t compid, uint64_t now)
{
	switch (id) {
	case MSG_HEARTBEAT: {
		uint8_t autopilot = p[5];
		/* only the flight controller: GCS / gimbal / radio heartbeats say
		 * "no autopilot" (8) */
		if (autopilot == MAV_AUTOPILOT_INVALID || compid != 1)
			return;
		t->custom_mode = u32(p);
		t->type = p[4];
		t->autopilot = autopilot;
		t->base_mode = p[6];
		int armed = (t->base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
		if (armed && !t->armed)
			t->armed_since_ms = now;
		if (!armed && t->armed && t->armed_since_ms)
			t->last_flight_ms = now - t->armed_since_ms;
		t->armed = armed;
		t->have_hb = 1;
		break;
	}
	case MSG_SYS_STATUS: {
		uint16_t mv = u16(p + 14);
		int16_t ca = (int16_t)u16(p + 16);
		t->volts = mv == 0xFFFF ? 0 : mv / 1000.0f;
		t->amps = ca < 0 ? -1 : ca / 100.0f;
		t->remaining = (int8_t)p[30];
		t->have_sys = 1;
		break;
	}
	case MSG_BATTERY_STATUS: {
		int32_t mah = (int32_t)u32(p);
		t->mah = mah < 0 ? -1 : mah;
		break;
	}
	case MSG_RC_CHANNELS:
		t->rc_rssi = p[41] == 255 ? -1 : p[41];
		t->rc_ms = now;
		break;
	case MSG_RC_CHANNELS_RAW:
		if (now - t->rc_ms > 1500 || t->rc_ms == 0)      /* RC_CHANNELS wins if both come */
			t->rc_rssi = p[21] == 255 ? -1 : p[21];
		break;
	}
	t->last_ms = now;
}

void telem_feed(telem_t *t, const uint8_t *b, size_t n, uint64_t now)
{
	size_t i = 0;
	while (i < n) {
		size_t hdr, flen;
		uint32_t id;
		uint8_t plen, sysid, compid;
		if (b[i] == 0xFE && i + 8 <= n) {               /* v1 */
			plen = b[i + 1];
			hdr = 6;
			sysid = b[i + 3];
			compid = b[i + 4];
			id = b[i + 5];
			flen = hdr + plen + 2;
		} else if (b[i] == 0xFD && i + 12 <= n) {       /* v2 */
			plen = b[i + 1];
			hdr = 10;
			sysid = b[i + 5];
			compid = b[i + 6];
			id = b[i + 7] | (uint32_t)b[i + 8] << 8 | (uint32_t)b[i + 9] << 16;
			flen = hdr + plen + 2 + ((b[i + 2] & 1) ? 13 : 0);
		} else {
			i++;
			continue;
		}
		(void)sysid;
		if (i + flen > n) {
			t->frames_bad++;
			break;
		}
		int k = -1;
		for (size_t m = 0; m < sizeof MSGS / sizeof MSGS[0]; m++)
			if (MSGS[m].id == id)
				k = (int)m;
		if (k >= 0) {
			uint16_t crc = 0xFFFF;
			for (size_t j = 1; j < hdr + plen; j++)
				crc = crc_acc(b[i + j], crc);
			crc = crc_acc(MSGS[k].crc_extra, crc);
			if (crc == u16(b + i + hdr + plen)) {
				/* v2 drops trailing zero bytes: pad back to full length */
				uint8_t p[64];
				memset(p, 0, sizeof p);
				memcpy(p, b + i + hdr, plen < MSGS[k].len ? plen : MSGS[k].len);
				handle(t, id, p, compid, now);
				t->frames_ok++;
			} else
				t->frames_bad++;
		}
		i += flen;
	}
}

int telem_open(int port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

void telem_poll(telem_t *t, int fd, uint64_t now)
{
	uint8_t buf[2048];
	for (;;) {
		ssize_t r = recv(fd, buf, sizeof buf, 0);
		if (r <= 0)
			break;
		telem_feed(t, buf, (size_t)r, now);
	}
}

uint64_t telem_flight_ms(const telem_t *t, uint64_t now)
{
	if (t->armed && t->armed_since_ms)
		return now - t->armed_since_ms;
	return t->last_flight_ms;
}

/* ---- flight mode names ----------------------------------------------- */

static const char *const COPTER[] = {
	"STABILIZE", "ACRO", "ALT HOLD", "AUTO", "GUIDED", "LOITER", "RTL", "CIRCLE", NULL, "LAND",
	NULL, "DRIFT", NULL, "SPORT", "FLIP", "AUTOTUNE", "POSHOLD", "BRAKE", "THROW", "AVOID ADSB",
	"GUIDED NOGPS", "SMART RTL", "FLOWHOLD", "FOLLOW", "ZIGZAG", "SYSTEMID", "AUTOROTATE",
	"AUTO RTL", "TURTLE",
};
static const char *const PLANE[] = {
	"MANUAL", "CIRCLE", "STABILIZE", "TRAINING", "ACRO", "FBWA", "FBWB", "CRUISE", "AUTOTUNE",
	NULL, "AUTO", "RTL", "LOITER", "TAKEOFF", "AVOID ADSB", "GUIDED", NULL, "QSTABILIZE",
	"QHOVER", "QLOITER", "QLAND", "QRTL", "QAUTOTUNE", "QACRO", "THERMAL", "LOITER QLAND",
};
static const char *const PX4_MAIN[] = {
	NULL, "MANUAL", "ALTCTL", "POSCTL", "AUTO", "ACRO", "OFFBOARD", "STABILIZED", "RATTITUDE",
};
static const char *const PX4_AUTO[] = {
	NULL, "READY", "TAKEOFF", "LOITER", "MISSION", "RTL", "LAND",
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

void telem_mode_name(const telem_t *t, char *out, size_t n)
{
	const char *s = NULL;
	uint32_t m = t->custom_mode;
	if (!t->have_hb) {
		snprintf(out, n, "-");
		return;
	}
	if (t->autopilot == 12) {                       /* PX4 */
		uint32_t mm = (m >> 16) & 0xff, sub = (m >> 24) & 0xff;
		if (mm == 4 && sub < N(PX4_AUTO))
			s = PX4_AUTO[sub];
		else if (mm < N(PX4_MAIN))
			s = PX4_MAIN[mm];
	} else {
		/* ArduPilot (3), and INAV (reports ArduPilot mode numbers) */
		int plane = t->type == 1 || (t->type >= 19 && t->type <= 22);   /* fixed wing, VTOL */
		if (plane && m < N(PLANE))
			s = PLANE[m];
		else if (!plane && m < N(COPTER))
			s = COPTER[m];
	}
	if (s)
		snprintf(out, n, "%s", s);
	else
		snprintf(out, n, "MODE %u", (unsigned)m);
}
