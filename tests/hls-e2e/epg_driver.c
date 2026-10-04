// End-to-end programme guide: epg.c's thread fetches the gzip XMLTV guide a
// playlist links to from server.py (streamed through aseg, inflate, xmltv,
// guide), keeps it in /data, and then: a broken link override, going back to
// the playlist's link (served from the cache, no new download), a refresh
// (downloads again), and a link that is not a guide.
// Usage: epg_driver BASE_URL SERVER_LOG   ($PS4CAST_DATA = scratch dir)
#include "epg.h"
#include "httpd.h"
#include "httpd_channels.h"
#include "aseg.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define T0 1790596800LL      // 2026-09-28 12:00:00 UTC, as in server.py's guide

int64_t wallclock_utc(void) { return T0 + 600; }
void netmon_ip(char *out, int cap) { snprintf(out, (size_t)cap, "127.0.0.1"); }

static const char *g_log;
static int count_gets(const char *path) {
    FILE *f = fopen(g_log, "r");
    if (!f) return 0;
    char line[512], want[256];
    snprintf(want, sizeof want, "GET %s", path);
    int n = 0;
    while (fgets(line, sizeof line, f)) if (strncmp(line, want, strlen(want)) == 0) n++;
    fclose(f);
    return n;
}

static int status_has(const char *s) {
    char st[256];
    epg_status(st, sizeof st);
    return strstr(st, s) != NULL;
}

#define WAIT(cond, secs) do { int _i; for (_i = 0; _i < (secs) * 10 && !(cond); _i++) usleep(100000); \
    if (!(cond)) { char _st[256]; epg_status(_st, sizeof _st); printf("timeout: %s (status: %s)\n", #cond, _st); return 1; } } while (0)
#define CHECK(c) do { if (!(c)) { char _st[256]; epg_status(_st, sizeof _st); printf("FAIL %s:%d %s (status: %s)\n", __FILE__, __LINE__, #c, _st); return 1; } } while (0)

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    alarm(120);
    const char *base = argv[1];
    g_log = argv[2];
    aseg_init();
    httpd_channels_init();
    epg_init();

    char list[2048];
    snprintf(list, sizeof list,
             "#EXTM3U x-tvg-url=\"%s/epg/guide.xml.gz\"\n"
             "#EXTINF:-1 tvg-id=\"bbc1.uk\" group-title=\"UK\",BBC One\nhttp://tv.example/1.m3u8\n"
             "#EXTINF:-1 group-title=\"UK\",UK: ITV 1 HD\nhttp://tv.example/2.m3u8\n"
             "#EXTINF:-1,Nothing Listed\nhttp://tv.example/3.m3u8\n", base);
    int jl = 0;
    free(httpd_channels_load_playlist(list, "http://p.example/list.m3u", &jl));
    CHECK(httpd_chan_count() == 3);

    epg_start();
    WAIT(epg_loaded(), 30);
    CHECK(status_has("2 channels"));
    EpgProgramme on, nx;
    int64_t now = wallclock_utc();
    CHECK(epg_now_next(0, now, &on, &nx) == 3);
    CHECK(strcmp(on.title, "Noon News") == 0 && on.start == T0 && on.stop == T0 + 1800);
    CHECK(strcmp(on.desc, "Headlines & weather") == 0);
    CHECK(strcmp(nx.title, "Half past") == 0);
    CHECK((epg_now_next(1, now, &on, NULL) & 1) && strcmp(on.title, "Film") == 0);   // by name
    CHECK(epg_now_next(2, now, &on, &nx) == 0);
    EpgProgramme sch[8];
    CHECK(epg_schedule(0, now, sch, 8) == 3 && strcmp(sch[2].title, "Evening") == 0);
    CHECK(count_gets("/epg/guide.xml.gz") == 1);
    char path[1024];
    snprintf(path, sizeof path, "%s/ps4cast_epg.cache", getenv("PS4CAST_DATA"));
    CHECK(access(path, R_OK) == 0);
    int ver = epg_version();

    // A broken override: the old guide goes, the failure is reported.
    char bad[512];
    snprintf(bad, sizeof bad, "%s/epg/missing.xml.gz", base);
    epg_set_url(bad);
    WAIT(!epg_loaded() && status_has("download failed"), 30);
    CHECK(epg_version() != ver);

    // Back to the playlist's link: the copy in /data serves it, no download.
    epg_set_url("");
    WAIT(epg_loaded(), 30);
    CHECK(count_gets("/epg/guide.xml.gz") == 1);
    CHECK(epg_now_next(0, now, &on, NULL) & 1);

    // Refresh downloads again.
    epg_refresh();
    WAIT(count_gets("/epg/guide.xml.gz") == 2 && epg_loaded() && status_has("2 channels"), 30);

    // A page that is not a guide.
    snprintf(bad, sizeof bad, "%s/epg/page.html", base);
    epg_set_url(bad);
    WAIT(status_has("not an XMLTV guide"), 30);
    CHECK(!epg_loaded());

    printf("guide ok\n");
    return 0;
}
