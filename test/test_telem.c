/* Feeds recorded datagrams into src/telem.c and prints the decoded state.
 * Input records: u32 now_ms, u16 len, len bytes (little endian).
 * Driven by test/test_telem.py, which builds the MAVLink frames itself. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/telem.h"

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	telem_t t;
	telem_init(&t);
	uint8_t hdr[6], buf[4096];
	uint64_t now = 0;
	while (fread(hdr, 1, 6, f) == 6) {
		now = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
		size_t n = hdr[4] | hdr[5] << 8;
		if (n > sizeof buf || fread(buf, 1, n, f) != n)
			return 3;
		telem_feed(&t, buf, n, now);
	}
	fclose(f);
	char mode[32];
	telem_mode_name(&t, mode, sizeof mode);
	printf("mode=%s armed=%d volts=%.2f amps=%.2f remaining=%d mah=%d rc_rssi=%d "
	       "flight_ms=%llu ok=%lu bad=%lu autopilot=%u type=%u\n",
	       mode, t.armed, t.volts, t.amps, t.remaining, t.mah, t.rc_rssi,
	       (unsigned long long)telem_flight_ms(&t, now), t.frames_ok, t.frames_bad,
	       t.autopilot, t.type);
	return 0;
}
