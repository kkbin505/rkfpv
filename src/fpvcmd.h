/*
 * fpvcmd: run one `fpv` subcommand for a network client, shared by fpvd
 * (UDP) and dvrhttpd (HTTP, hotspot mode).
 *
 * Only whitelisted subcommands, only arguments made of [A-Za-z0-9_.,:-],
 * executed with execv (no shell). The reply is fpv's --json output.
 */
#ifndef FPVCMD_H
#define FPVCMD_H

#include <stddef.h>

#define FPVCMD_MAX_ARGS 32

/* 1 if `cmd` (the first word after "fpv") may be run over the network */
int fpvcmd_allowed(const char *cmd);

/* 1 if one argument contains only safe characters (and is 1..64 long) */
int fpvcmd_valid_arg(const char *s);

/* {"ok":false,"error":"<msg>"}, malloc'd; msg must not need escaping */
char *fpvcmd_error(const char *msg, size_t *len);

/* Checks args (args[0] = subcommand) and runs `<fpv> args... --json` with a
 * timeout. Returns malloc'd JSON (fpv's output, or an error object), NULL
 * only when out of memory. */
char *fpvcmd_run(const char *fpv, char **args, int nargs, int timeout_ms, size_t *len);

#endif
