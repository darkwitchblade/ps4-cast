#include "resolve.h"
#include "aseg.h"
#include "urlopt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char g_dbg[128] = "";
const char *resolve_debug(void) { return g_dbg; }

// Extensions we can hand straight to the player -- no point fetching the page.
static const char *MEDIA_EXT[] = {
    ".m3u8", ".m3u", ".mpd", ".mp4", ".m4v", ".mkv", ".webm", ".avi", ".mov",
    ".ts", ".mp3", ".aac", ".flac", ".wav", ".ogg", ".m4a", 0
};

static const char *path_end(const char *url) {
    const char *q = strpbrk(url, "?#");
    return q ? q : url + strlen(url);
}

int resolve_is_page(const char *url) {
    if (!url || strncmp(url, "http", 4) != 0) return 0;
    const char *end = path_end(url);
    for (int i = 0; MEDIA_EXT[i]; i++) {
        size_t l = strlen(MEDIA_EXT[i]);
        if ((size_t)(end - url) >= l && strncasecmp(end - l, MEDIA_EXT[i], l) == 0) return 0;
    }
    return 1;   // no media extension -> treat as a page worth scraping
}

// Copy url origin ("https://host") into out.
static int origin_of(const char *url, char *out, int cap) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    const char *slash = strchr(p + 3, '/');
    int n = slash ? (int)(slash - url) : (int)strlen(url);
    if (n >= cap) return 0;
    memcpy(out, url, (size_t)n); out[n] = '\0';
    return 1;
}

// JSON and HTML both escape slashes; undo the common forms in place.
static void unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == '/') { *w++ = '/'; r++; }
        else if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 4; }
        else if (strncmp(r, "\\u0026", 6) == 0) { *w++ = '&'; r += 5; }
        else *w++ = *r;
    }
    *w = '\0';
}

// Obvious non-content hits: ad tech, analytics, thumbnails, tracking pixels.
static int is_junk(const char *u) {
    static const char *bad[] = { "doubleclick", "googlesyndication", "google-analytics",
                                 "/ads/", "adserver", "imasdk", "moatads", "scorecardresearch", 0 };
    for (int i = 0; bad[i]; i++) if (strstr(u, bad[i])) return 1;
    return 0;
}

// Higher is better. Master/index playlists beat a single rendition, and a URL
// that carries a signed token is usually THE one the player was given.
static int score_of(const char *u) {
    int s = 0;
    if (strstr(u, ".m3u8")) s += 10;
    if (strstr(u, ".mpd"))  s += 6;       // parsed but the app cannot play DASH yet
    if (strstr(u, "master") || strstr(u, "index") || strstr(u, "playlist")) s += 5;
    if (strstr(u, "token") || strstr(u, "expires") || strstr(u, "hdnts")) s += 4;
    if (strncmp(u, "https://", 8) == 0) s += 1;
    if (strstr(u, "chunklist") || strstr(u, "media_")) s -= 2;   // a rendition, not the master
    return s;
}

// Walk back from a hit to the start of the URL/quoted string containing it.
static const char *token_start(const char *body, const char *hit) {
    const char *p = hit;
    while (p > body) {
        char c = p[-1];
        if (c == '"' || c == '\'' || c == '(' || c == ' ' || c == '\t' ||
            c == '\n' || c == '\r' || c == '=' || c == ',' || c == '>') break;
        p--;
    }
    return p;
}

static int extract_best(const char *body, const char *pageUrl, char *out, int cap) {
    char origin[256];
    if (!origin_of(pageUrl, origin, sizeof(origin))) return 0;

    char best[1400] = ""; int bestScore = -1;
    const char *needles[] = { ".m3u8", ".mpd", 0 };

    for (int n = 0; needles[n]; n++) {
        const char *p = body;
        while ((p = strstr(p, needles[n])) != NULL) {
            const char *s = token_start(body, p);
            const char *e = p + strlen(needles[n]);
            while (*e && !strchr("\"'()<> \t\r\n,\\", *e)) e++;   // include ?query
            int len = (int)(e - s);
            p = e;
            if (len <= 0 || len >= (int)sizeof(best)) continue;

            char cand[1400];
            memcpy(cand, s, (size_t)len); cand[len] = '\0';
            unescape(cand);

            char abs[1400];
            if (strncmp(cand, "http", 4) == 0)      snprintf(abs, sizeof(abs), "%s", cand);
            else if (cand[0] == '/' && cand[1] == '/') snprintf(abs, sizeof(abs), "https:%s", cand);
            else if (cand[0] == '/')                snprintf(abs, sizeof(abs), "%s%s", origin, cand);
            else continue;    // relative-to-directory: too ambiguous to guess safely

            if (is_junk(abs)) continue;
            int sc = score_of(abs);
            if (sc > bestScore) { bestScore = sc; snprintf(best, sizeof(best), "%s", abs); }
        }
    }

    if (bestScore < 0) return 0;
    // Attach the page as Referer/Origin: this is what the CDN checks, and it is
    // the single most common reason a correct manifest URL still returns 403.
    snprintf(out, (size_t)cap, "%s|Referer=%s&User-Agent=%s", best, pageUrl,
             "Mozilla/5.0 (SMART-TV; Linux) AppleWebKit/537.36");
    return 1;
}

int resolve_page(const char *pageUrl, char *out, int cap) {
    g_dbg[0] = '\0';
    if (!pageUrl || !out || cap <= 0) return 0;

    // Ask as a browser would; many sites serve a stub (or 403) to unknown agents.
    char withHdrs[1600];
    snprintf(withHdrs, sizeof(withHdrs),
             "%s|User-Agent=%s&Referer=%s",
             pageUrl, "Mozilla/5.0 (SMART-TV; Linux) AppleWebKit/537.36", pageUrl);
    char clean[1400];
    urlopt_apply(withHdrs, clean, sizeof(clean));

    uint8_t *body = NULL; int len = 0;
    aseg_set_playlist_budget(0);            // a page is bigger than a playlist
    int rc = aseg_fetch(clean, &body, &len);
    if (rc != 0 || !body || len <= 0) {
        snprintf(g_dbg, sizeof(g_dbg), "resolve: fetch failed rc=%d", rc);
        if (body) free(body);
        return 0;
    }
    char *text = malloc((size_t)len + 1);
    if (!text) { free(body); snprintf(g_dbg, sizeof(g_dbg), "resolve: oom"); return 0; }
    memcpy(text, body, (size_t)len); text[len] = '\0';
    free(body);

    int got = extract_best(text, pageUrl, out, cap);
    free(text);
    if (!got) {
        // Almost always means the URL is built in JS at runtime.
        snprintf(g_dbg, sizeof(g_dbg), "resolve: no manifest in %dKB page", len / 1024);
        return 0;
    }
    snprintf(g_dbg, sizeof(g_dbg), "resolve: ok from %dKB page", len / 1024);
    return 1;
}
