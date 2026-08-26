#include "urlopt.h"
#include <string.h>
#include <stdio.h>

static char g_hdrs[768];
static char g_hdrsNoPage[768];
static char g_kind[16];
static int g_pageHeadersEnabled = 1;

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Option values are percent-encoded by structured senders so URLs containing
// '&' cannot be mistaken for another option. Drop controls after decoding: the
// result is copied into an HTTP header and must never be able to inject a line.
static int decode_value(const char *src, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && o < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' && i + 2 < len) {
            int hi = hex_value(src[i + 1]), lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) { c = (unsigned char)((hi << 4) | lo); i += 2; }
        }
        if (c == '\r' || c == '\n' || c == 0 || c < 0x20 || c == 0x7f) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

// Only headers that make sense to forge per-source. Anything else is ignored so a
// malformed list can't inject arbitrary request headers.
static const char *canon_header(const char *k, int klen) {
    struct { const char *in; const char *out; } map[] = {
        { "referer",     "Referer"     },
        { "referrer",    "Referer"     },
        { "origin",      "Origin"      },
        { "user-agent",  "User-Agent"  },
        { "cookie",      "Cookie"      },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if ((int)strlen(map[i].in) == klen) {
            int same = 1;
            for (int j = 0; j < klen; j++) {
                char a = k[j], b = map[i].in[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (a != b) { same = 0; break; }
            }
            if (same) return map[i].out;
        }
    }
    return 0;
}

void urlopt_apply(const char *in, char *urlOut, int urlCap) {
    g_hdrs[0] = '\0';
    g_hdrsNoPage[0] = '\0';
    g_kind[0] = '\0';
    g_pageHeadersEnabled = 1;
    if (!in || !urlOut || urlCap <= 0) return;

    const char *bar = strchr(in, '|');
    int ulen = bar ? (int)(bar - in) : (int)strlen(in);
    if (ulen >= urlCap) ulen = urlCap - 1;
    memcpy(urlOut, in, (size_t)ulen);
    urlOut[ulen] = '\0';
    if (!bar) return;

    int used = 0, safeUsed = 0;
    const char *p = bar + 1;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq  = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            int klen = (int)(eq - p);
            if ((klen == 4 && strncasecmp(p, "type", 4) == 0) ||
                (klen == 4 && strncasecmp(p, "kind", 4) == 0)) {
                decode_value(eq + 1, (int)(end - (eq + 1)), g_kind, sizeof(g_kind));
                goto next_option;
            }
            const char *name = canon_header(p, (int)(eq - p));
            int vlen = (int)(end - (eq + 1));
            if (name && vlen > 0 && vlen < 700) {
                char value[700];
                int decoded = decode_value(eq + 1, vlen, value, sizeof(value));
                if (decoded <= 0) goto next_option;
                int n = snprintf(g_hdrs + used, sizeof(g_hdrs) - (size_t)used,
                                 "%s: %s\r\n", name, value);
                if (n > 0 && used + n < (int)sizeof(g_hdrs)) used += n;
                if (strcmp(name, "Referer") != 0 && strcmp(name, "Origin") != 0) {
                    n = snprintf(g_hdrsNoPage + safeUsed,
                                 sizeof(g_hdrsNoPage) - (size_t)safeUsed,
                                 "%s: %s\r\n", name, value);
                    if (n > 0 && safeUsed + n < (int)sizeof(g_hdrsNoPage)) safeUsed += n;
                }
            }
        }
next_option:
        if (!amp) break;
        p = amp + 1;
    }

    // A CDN that checks Referer usually checks Origin too; derive it when only one
    // was given, since IPTV lists in the wild almost always carry just the referer.
    if (strstr(g_hdrs, "Referer:") && !strstr(g_hdrs, "Origin:")) {
        const char *r = strstr(g_hdrs, "Referer: ") + 9;
        const char *scheme = strstr(r, "://");
        if (scheme) {
            const char *host_end = strchr(scheme + 3, '/');
            int olen = host_end ? (int)(host_end - r) : (int)strcspn(r, "\r\n");
            int n = snprintf(g_hdrs + used, sizeof(g_hdrs) - (size_t)used,
                             "Origin: %.*s\r\n", olen, r);
            if (n > 0 && used + n < (int)sizeof(g_hdrs)) used += n;
        }
    }
}

const char *urlopt_headers(void) {
    return g_pageHeadersEnabled ? g_hdrs : g_hdrsNoPage;
}
int urlopt_has_page_headers(void) {
    return strstr(g_hdrs, "Referer:") != NULL || strstr(g_hdrs, "Origin:") != NULL;
}
void urlopt_set_page_headers_enabled(int enabled) { g_pageHeadersEnabled = enabled ? 1 : 0; }
const char *urlopt_kind(void) { return g_kind; }
