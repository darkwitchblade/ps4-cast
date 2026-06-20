// httpsrc.h — minimal app-managed HTTP/1.1 byte-range reader.
//
// ffmpeg is built without networking, so remote sources are fed through a
// custom AVIO backed by this reader. It streams one persistent ranged GET over
// plain HTTP or BearSSL-backed HTTPS, and reconnects when ffmpeg seeks.
#ifndef PS4CAST_HTTPSRC_H
#define PS4CAST_HTTPSRC_H

#include <stdint.h>

// Parse + probe the URL (learns total size via a ranged probe). Returns 0 on
// success, negative on failure. Safe to call again to re-open a new URL.
int      httpsrc_open(const char *url);

// Positional read: fetch `len` bytes starting at byte `pos`. Returns the number
// of bytes written into `buf` (>0), 0 at EOF, or negative on error.
int      httpsrc_read(uint8_t *buf, uint64_t pos, uint32_t len);

// Total resource size in bytes (0 if unknown).
uint64_t httpsrc_size(void);

void     httpsrc_close(void);

// Interrupt a blocked read immediately (Stop / new cast) so an underrun stall
// can never trap the app.
void     httpsrc_abort(void);

// Forward read-ahead as a percent of the ring (0 = about to underrun).
int      httpsrc_fill_pct(void);
uint64_t httpsrc_ahead_bytes(void);   // forward-buffered bytes
int      httpsrc_is_lan(void);        // 1 if host is a private/LAN address

// One-line human-readable state for /status telemetry.
const char *httpsrc_debug(void);

#endif
