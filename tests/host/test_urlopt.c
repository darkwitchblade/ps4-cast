// Host tests for the URL option/header machinery (app/src/urlopt.c).
// urlopt.c is self-contained, so it links directly.
#include "../../app/src/urlopt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void) {
    char clean[512];

    // headers appended via | survive into urlopt_headers()
    urlopt_apply("https://cdn.example/v.m3u8|User-Agent=TestUA&Referer=https://page.example/", clean, sizeof clean);
    CHECK(strcmp(clean, "https://cdn.example/v.m3u8") == 0);
    const char *h = urlopt_headers();
    CHECK(strstr(h, "User-Agent: TestUA") != NULL);
    CHECK(strstr(h, "Referer: https://page.example/") != NULL);

    // percent-encoded option values decode, and control chars are dropped
    urlopt_apply("https://cdn.example/v.m3u8|User-Agent=A%0AB", clean, sizeof clean);
    CHECK(strstr(urlopt_headers(), "\nB:") == NULL);   // no header injection

    // plain URL resets headers
    urlopt_apply("https://cdn.example/other.m3u8", clean, sizeof clean);
    CHECK(strcmp(clean, "https://cdn.example/other.m3u8") == 0);

    // page-header policy toggle (403 clean-retry path): disabling drops
    // Referer/Origin from the sent headers but keeps the UA
    urlopt_apply("https://cdn.example/v.m3u8|User-Agent=TestUA&Referer=https://page.example/", clean, sizeof clean);
    urlopt_set_page_headers_enabled(0);
    CHECK(strstr(urlopt_headers(), "Referer:") == NULL);
    CHECK(strstr(urlopt_headers(), "User-Agent: TestUA") != NULL);
    urlopt_set_page_headers_enabled(1);
    CHECK(strstr(urlopt_headers(), "Referer: https://page.example/") != NULL);

    printf(failures ? "test_urlopt: %d FAILURES\n" : "test_urlopt: all ok\n", failures);
    return failures ? 1 : 0;
}
