// Host tests for resolve.c's page-scraping half. The interesting functions are
// static, so the test includes the translation unit directly and stubs its
// network dependency (aseg_fetch) — resolve_page itself is on-device territory.
#define ASEG_H_INCLUDED
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- minimal stubs for resolve.c's dependencies -------------------------
static int g_stubRc = 0;
static const char *g_stubBody = "";
int aseg_fetch(const char *url, uint8_t **buf, int *len) {
    (void)url;
    int n = (int)strlen(g_stubBody);
    if (g_stubRc != 0 || n == 0) { *buf = NULL; *len = 0; return g_stubRc ? g_stubRc : -1; }
    uint8_t *b = malloc((size_t)n + 1);
    memcpy(b, g_stubBody, (size_t)n + 1);
    *buf = b; *len = n;
    return 0;
}
void aseg_set_playlist_budget(int on) { (void)on; }
#include "../../app/src/urlopt.h"
// urlopt_apply etc. come from the real module:
#include "../../app/src/urlopt.c"

#include "../../app/src/resolve.c"

#include <stdio.h>
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void) {
    char out[1600];

    // master playlist embedded in an HTML/JS page wins over renditions
    g_stubBody = "<html><script>var s=[\"https://cdn.example/hls/seg-2.m3u8\","
                 "\"https://cdn.example/hls/master.m3u8?token=abc\"];</script>";
    CHECK(resolve_page("https://site.example/watch/1", out, sizeof out) == 1);
    CHECK(strncmp(out, "https://cdn.example/hls/master.m3u8?token=abc", 30) == 0);
    CHECK(strstr(out, "Referer=https://site.example/watch/1") != NULL);

    // junk (ads/analytics) is skipped
    g_stubBody = "<html>https://google-analytics.com/a.m3u8 https://cdn.example/v.m3u8";
    CHECK(resolve_page("https://site.example/watch/2", out, sizeof out) == 1);
    CHECK(strncmp(out, "https://cdn.example/v.m3u8", 20) == 0);

    // no manifest -> clean failure with debug text
    g_stubBody = "<html>nothing here</html>";
    CHECK(resolve_page("https://site.example/watch/3", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "no manifest") != NULL);

    // fetch failure propagates
    g_stubRc = -3; g_stubBody = "";
    CHECK(resolve_page("https://site.example/watch/4", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "fetch failed") != NULL);

    printf(failures ? "test_resolve: %d FAILURES\n" : "test_resolve: all ok\n", failures);
    return failures ? 1 : 0;
}
