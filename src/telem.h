/*
 * Flight-controller telemetry for the OSD, decoded from the MAVLink stream
 * mavbridge copies to a local UDP port (mavbridge -c 127.0.0.1:14560).
 * Only the few messages the OSD shows are decoded, each CRC-checked.
 */
#ifndef RKFPV_TELEM_H
#define RKFPV_TELEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t last_ms;               /* last valid message from the FC, 0 = never */

	int have_hb;                    /* HEARTBEAT */
	uint32_t custom_mode;
	uint8_t base_mode, type, autopilot;
	int armed;

	int have_sys;                   /* SYS_STATUS */
	float volts, amps;              /* amps < 0: unknown */
	int remaining;                  /* %, -1 unknown */

	int mah;                        /* BATTERY_STATUS current_consumed, -1 unknown */

	int rc_rssi;                    /* RC_CHANNELS(_RAW).rssi 0..254, -1 unknown */
	uint64_t rc_ms;

	/* flight time: counts while armed */
	uint64_t armed_since_ms;
	uint64_t last_flight_ms;

	unsigned long frames_ok, frames_bad;
} telem_t;

void telem_init(telem_t *t);

/* parse whole MAVLink frames in a datagram (v1/v2, mixed) */
void telem_feed(telem_t *t, const uint8_t *buf, size_t len, uint64_t now_ms);

/* UDP listener on 127.0.0.1:port, non-blocking; -1 on error */
int telem_open(int port);
/* read everything that is queued */
void telem_poll(telem_t *t, int fd, uint64_t now_ms);

/* "LOITER", "FBWA", "MODE 42" ... */
void telem_mode_name(const telem_t *t, char *out, size_t n);
/* flight time in ms: current flight while armed, else the last one */
uint64_t telem_flight_ms(const telem_t *t, uint64_t now_ms);

#endif
