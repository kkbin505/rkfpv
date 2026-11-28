/*
 * External OSD input: other programs on the board draw boxes, text and
 * crosses into rkfpv's OSD over UDP (127.0.0.1:osd_ext_port).
 *
 * One datagram = the complete set of shapes, it replaces the previous one.
 * Text, one command per line:
 *
 *   box X Y W H          outline rectangle
 *   text X Y STRING      text, top-left at X Y (rest of the line)
 *   cross X Y SIZE       cross centred at X Y, arms SIZE long
 *   clear                nothing (an empty datagram does the same)
 *
 * Coordinates are on a 0..1000 grid in both directions (SIZE in 1/1000 of
 * the height), scaled to each canvas, so the same datagram fits the link
 * and the DVR picture. Shapes disappear when no datagram came for the TTL:
 * a crashed sender leaves nothing stale on screen.
 *
 * Parsing and drawing need no SDK: tested on the PC (test/test_osd_ext.c).
 */
#ifndef RKFPV_OSD_EXT_H
#define RKFPV_OSD_EXT_H

#include <stddef.h>
#include <stdint.h>

#include "osd.h"

#define OSD_EXT_MAX 64          /* shapes per datagram */
#define OSD_EXT_TEXT 48         /* bytes of text per shape, incl. the 0 */
#define OSD_EXT_GRID 1000

enum { OSD_EXT_BOX = 1, OSD_EXT_TEXT_SHAPE, OSD_EXT_CROSS };

typedef struct {
	uint8_t kind;
	int16_t x, y, w, h;     /* grid units; cross: w = size */
	char text[OSD_EXT_TEXT];
} osd_ext_shape_t;

typedef struct {
	osd_ext_shape_t s[OSD_EXT_MAX];
	int n;
	uint64_t last_ms;       /* when the current set arrived, 0 = never */
	unsigned long long datagrams, bad_lines;
} osd_ext_t;

void osd_ext_init(osd_ext_t *e);

/* Replaces the shape set with the commands in buf. Bad lines are skipped
 * and counted; returns the number of shapes. */
int osd_ext_parse(osd_ext_t *e, const char *buf, size_t len, uint64_t now_ms);

/* UDP socket on 127.0.0.1:port, non-blocking; -1 on error */
int osd_ext_open(int port);

/* Reads every waiting datagram (the last one wins); 1 if the set changed */
int osd_ext_poll(osd_ext_t *e, int fd, uint64_t now_ms);

/* 1 while the set is newer than ttl_ms and has something to draw */
int osd_ext_live(const osd_ext_t *e, uint64_t now_ms, int ttl_ms);

/* Draws the set onto a canvas between osd_begin and osd_commit */
void osd_ext_draw(osd_t *o, const osd_ext_t *e, uint64_t now_ms, int ttl_ms);

#endif
