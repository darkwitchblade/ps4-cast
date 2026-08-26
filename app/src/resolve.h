// resolve.h — turn a PAGE url into a playable manifest url, on-device.
//
// A blob: URL cannot be fetched: it is a browser-local handle to a MediaSource
// buffer. The real media arrives as an HLS/DASH manifest plus segments, and on
// most sites that manifest URL is present verbatim in the page HTML or in a JSON
// island inside it. We fetch the page and dig it out.
//
// LIMIT: sites that COMPUTE the url in JavaScript at runtime (or fetch it from a
// signed XHR) cannot be resolved here -- that needs a JS engine, which this app
// does not and will not have. Those still need the DevTools/yt-dlp route.
#ifndef PS4CAST_RESOLVE_H
#define PS4CAST_RESOLVE_H

// 1 if `url` looks like a web PAGE rather than a media file we can open directly.
int resolve_is_page(const char *url);

// Fetch `pageUrl` and extract the best manifest URL into out (with |Referer=...
// options appended, since CDNs almost always gate on it). Returns 1 on success.
int resolve_page(const char *pageUrl, char *out, int cap);

// Short human-readable note about the last attempt, for /status.
const char *resolve_debug(void);

#endif
