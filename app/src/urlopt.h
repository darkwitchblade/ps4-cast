// urlopt.h — split the IPTV-style "url|Key=Value&Key2=Value2" form into a clean
// URL plus an HTTP header block, and hold those headers for the current stream.
//
// Sites that feed a <video> from a blob: URL are almost always using MSE: JS
// fetches real segments and appends them to a SourceBuffer. The segment/manifest
// URLs are ordinary HTTP, but their CDNs usually gate on Referer/Origin (and
// sometimes Cookie or a specific User-Agent), so a bare GET gets 403. This is the
// same convention Kodi/VLC use, which means existing IPTV lists work unchanged.
#ifndef PS4CAST_URLOPT_H
#define PS4CAST_URLOPT_H

// Split `in` at the first '|'. Writes the clean URL to urlOut. Any options are
// converted to CRLF-terminated header lines and REMEMBERED for this stream.
// Safe to pass a URL with no '|' (headers are then cleared).
void urlopt_apply(const char *in, char *urlOut, int urlCap);

// Extra header lines for the current stream ("" when none). Already CRLF-terminated.
const char *urlopt_headers(void);

// Retry compatibility: temporarily omit Referer/Origin while retaining UA and
// Cookie. A few CDNs reject an incorrect outer-page context but accept a clean
// request; the setting resets on the next urlopt_apply().
int urlopt_has_page_headers(void);
void urlopt_set_page_headers_enabled(int enabled);

// Optional non-header media hint supplied by structured cast clients (for
// example HLS endpoints whose URL has no .m3u8 suffix).
const char *urlopt_kind(void);

#endif
