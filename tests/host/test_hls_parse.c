// Host tests for the pure HLS playlist parser (app/src/hls_parse.c).
#include "../../app/src/hls_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_resolve_url(void) {
    char out[2048];
    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "/seg/0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/seg/0.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/live/0.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "//other.example/x.ts", out, sizeof out);
    CHECK(strcmp(out, "https://other.example/x.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "https://abs.example/a.ts?sig=1", out, sizeof out);
    CHECK(strcmp(out, "https://abs.example/a.ts?sig=1") == 0);

    // query on the base must not leak into a relative resolution
    hlspl_resolve_url("https://cdn.example.com/live/index.m3u8?token=abc", "0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/live/0.ts") == 0);

    // amazonaws.com https downgraded to http (TLS-fingerprint workaround)
    hlspl_resolve_url("https://bucket.s3.amazonaws.com/pl.m3u8", "a.ts", out, sizeof out);
    CHECK(strncmp(out, "http://bucket.s3.amazonaws.com", 30) == 0);
}

static void test_parse_media_vod(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-MEDIA-SEQUENCE:1\n"
        "#EXTINF:5.005,\n"
        "seg0.ts\n"
        "#EXTINF:4.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example.com/vod/index.m3u8") == 0);
    CHECK(pl.segCount == 2);
    CHECK(pl.isLive == 0);
    CHECK(pl.targetDurMs == 6000);
    CHECK(pl.totalDurMs == 9005);
    CHECK(strcmp(pl.segs[0], "https://cdn.example.com/vod/seg0.ts") == 0);
    CHECK(pl.segDurMs[1] == 4000);
    hlspl_free(&pl);
}

static void test_parse_media_live_disc_map(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:77\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4.0,\n"
        "a.m4s\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4.0,\n"
        "b.m4s\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example.com/hls/index.m3u8") == 0);
    CHECK(pl.isLive == 1);
    CHECK(pl.mediaSeq == 77);
    CHECK(pl.initSeg && strcmp(pl.initSeg, "https://cdn.example.com/hls/init.mp4") == 0);
    CHECK(pl.segDisc[1] == 1 && pl.segDisc[0] == 0);
    hlspl_free(&pl);
}

static void test_parse_media_rejects_encryption(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char enc[] = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, enc, "https://x/y.m3u8") == -2);
    char plain[] = "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, plain, "https://x/y.m3u8") == 0);
    hlspl_free(&pl);
}

static void test_collect_variants(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x536,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
        "mid.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=670000,RESOLUTION=640x268,CODECS=\"avc1.42c015\"\n"
        "low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4400000,RESOLUTION=1920x804,CODECS=\"hvc1.1.6.L123.00\"\n"
        "hi.m3u8\n";
    CHECK(hlspl_collect_variants(&pl, body, "https://cdn.example.com/master.m3u8") == 3);
    // sorted by bandwidth
    CHECK(pl.variants[0].bw == 670000 && strstr(pl.variants[0].url, "low.m3u8"));
    CHECK(pl.variants[2].codec == VC_HEVC);
    CHECK(hlspl_codec_from_str("avc1.64001f") == VC_H264);

    // start variant: best H.264 at or under 2.5Mbps -> the 2.4M rendition
    CHECK(hlspl_pick_start_variant(&pl) == 1);
    // fMP4 lock: highest H.264 height within the bw cap -> 1280x536
    CHECK(hlspl_pick_fmp4_start_variant(&pl, 1080) == 1);
    CHECK(hlspl_pick_best(&pl, 700000) == 0);    // strictly-below filter
    CHECK(hlspl_pick_best(&pl, 0) == 1);         // best score: H264 mid tier
    CHECK(hlspl_pick_best(&pl, 2400000) == 0);   // excludes mid+hi
    hlspl_free(&pl);
}

int main(void) {
    test_resolve_url();
    test_parse_media_vod();
    test_parse_media_live_disc_map();
    test_parse_media_rejects_encryption();
    test_collect_variants();
    printf(failures ? "test_hls_parse: %d FAILURES\n" : "test_hls_parse: all ok\n", failures);
    return failures ? 1 : 0;
}
