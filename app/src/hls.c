#include "hls.h"
#include "httpsrc.h"
#include "aseg.h"
#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/libkernel.h>

// HLS reader: parse the .m3u8, then stream the segments back-to-back through
// httpsrc so ffmpeg's mpegts/mov demuxer sees one continuous byte stream. All
// networking (http + https/BearSSL + redirects) is reused from httpsrc.

#define HLS_MAX_SEGMENTS 8192
#define PLAYLIST_CAP     (4 * 1024 * 1024)
#define HLS_MEM_SEG_CAP  (16 * 1024 * 1024)
#define HLS_PREFETCH_MAX 5
#define HLS_AUDIO_PREFETCH_SLOTS 3

static char  **g_segs;            // resolved absolute segment URLs
static int     g_segCount;
static int     g_segIdx;          // current segment being read
static char   *g_initSeg;         // fMP4 init segment URL (EXT-X-MAP), or NULL
static int     g_initPending;     // 1 = init segment still to be streamed first
static char    g_mediaUrl[2048];  // current media playlist URL (for live refresh)
static int     g_isLive;          // no EXT-X-ENDLIST: refresh playlist at the end
static int     g_targetDurMs;     // EXT-X-TARGETDURATION for live refresh pacing
static int     g_mediaSeq;        // EXT-X-MEDIA-SEQUENCE for sliding live windows
static uint64_t g_lastRefreshUs;
static uint64_t g_segPos;         // byte cursor within current open segment
static volatile int g_segGen;     // increments when advancing to the next media segment
static volatile int g_resetGen;   // increments when live HLS skips/jumps and player must re-anchor
static int     g_open;            // a segment is open in httpsrc
static int     g_active;
static char    g_dbg[360] = "idle";   // holds open errors; live status built in hls_debug

typedef struct {
    int seg;
    uint8_t *buf;
    int len;
    int ready;
    int fetching;
} HlsPrefetch;
static HlsPrefetch       g_pref[HLS_PREFETCH_MAX];
static OrbisPthread      g_prefThread;
static OrbisPthreadMutex g_prefMtx;
static OrbisPthreadCond  g_prefCond;
static int               g_prefUp;
static volatile int      g_prefStop;
static int               g_prefDepth = 3;
static int               g_memSeg = -1;
static uint8_t          *g_memBuf;
static int               g_memLen, g_memPos;
static int               g_liveFetchFailStreak;
static char              g_vLastUrl[96] = "";
static int               g_vLastRc, g_vLastBytes, g_vLastMs, g_vFailCount;
static uint64_t          g_vOpenUs;

// ABR: master-playlist variants. We rank by a software-decode-friendly score
// (codec, resolution, bitrate, fps) — see variant_score — and downshift when the
// buffer keeps draining.
#define HLS_MAX_VARIANTS 24
enum { VC_H264 = 0, VC_HEVC, VC_VP9, VC_AV1, VC_OTHER };
typedef struct { int bw, height, fps, codec; char url[2048]; char agroup[64]; } HlsVariant;
static HlsVariant g_variants[HLS_MAX_VARIANTS];
static int        g_variantCount;
static int        g_curVariant = -1;   // index into g_variants
static int        g_sepAudio = 0;      // master has a separate audio rendition (EXT-X-MEDIA)

// ---- separate audio rendition (EXT-X-MEDIA:TYPE=AUDIO) ---------------------
// When present, the audio is its own playlist of audio-only segments. We parse
// it into a second segment list and stream it through aseg (its own connection),
// so the player can demux/decode it in parallel with the video segments.
static char  **g_asegs;          // resolved audio segment URLs
static int     g_asegCount;
static int     g_asegIdx;        // current audio segment
static char   *g_aInit;          // audio fMP4 init segment (EXT-X-MAP), or NULL
static int     g_aInitPending;   // 1 = audio init still to be streamed first
static int     g_audioReady;     // a separate audio rendition is parsed + usable
static uint8_t *g_aBuf;          // current audio segment fetched into RAM
static int      g_aBufLen, g_aBufPos;
static volatile int g_aAbort;    // sticky: Stop/cast ends the audio path
static HlsPrefetch       g_apref[HLS_AUDIO_PREFETCH_SLOTS];
static OrbisPthread      g_aprefThread;
static OrbisPthreadMutex g_aprefMtx;
static OrbisPthreadCond  g_aprefCond;
static int               g_aprefUp;
static volatile int      g_aprefStop;
static char              g_aLastUrl[96] = "";
static int               g_aLastRc, g_aLastBytes, g_aLastMs, g_aFailCount;

static const char *codec_name(int c) {
    return c == VC_H264 ? "avc" : c == VC_HEVC ? "hevc" : c == VC_VP9 ? "vp9" : c == VC_AV1 ? "av1" : "?";
}
static int codec_from_str(const char *codecs) {  // CODECS="avc1.x,mp4a.y"
    if (strstr(codecs, "avc1") || strstr(codecs, "avc3") || strstr(codecs, "h264")) return VC_H264;
    if (strstr(codecs, "hvc1") || strstr(codecs, "hev1") || strstr(codecs, "dvh"))  return VC_HEVC;
    if (strstr(codecs, "vp09") || strstr(codecs, "vp9"))  return VC_VP9;
    if (strstr(codecs, "av01"))                            return VC_AV1;
    return VC_OTHER;
}

extern uint64_t sceKernelGetProcessTime(void); // microseconds, monotonic

static void short_url(const char *url, char *out, int cap) {
    if (!url || cap <= 0) return;
    int n = (int)strlen(url);
    const char *p = url;
    if (n >= cap) p = url + n - (cap - 1);
    snprintf(out, cap, "%s", p);
}

static void prefer_plain_s3(char *url, int cap) {
    (void)cap;
    if (!url) return;
    if (strncmp(url, "https://", 8) != 0) return;
    const char *slash = strchr(url + 8, '/');
    int hostLen = slash ? (int)(slash - (url + 8)) : (int)strlen(url + 8);
    if (hostLen <= 0) return;
    if (strstr(url + 8, ".amazonaws.com") && strstr(url + 8, ".amazonaws.com") < url + 8 + hostLen) {
        memmove(url + 7, url + 8, strlen(url + 8) + 1);
        memcpy(url, "http://", 7);
    }
}

static void configure_prefetch_depth(void) {
    int bw = (g_curVariant >= 0 && g_curVariant < g_variantCount) ? g_variants[g_curVariant].bw : 0;
    int d = 3;
    if (g_isLive) d = 2;
    else if (bw > 0 && bw <= 4000000) d = 5;
    else if (bw > 0 && bw <= 8000000) d = 4;
    else if (bw > 14000000) d = 2;
    if (d < 1) d = 1;
    if (d > HLS_PREFETCH_MAX) d = HLS_PREFETCH_MAX;
    g_prefDepth = d;
}

const char *hls_debug(void) {
    static char b[480];
    if (g_active && g_curVariant >= 0 && g_curVariant < g_variantCount) {
        HlsVariant *v = &g_variants[g_curVariant];
        int cached = 0;
        if (g_prefUp) {
            scePthreadMutexLock(&g_prefMtx);
            for (int i = 0; i < HLS_PREFETCH_MAX; i++) if (g_pref[i].ready) cached++;
            scePthreadMutexUnlock(&g_prefMtx);
        }
        int acached = 0;
        if (g_aprefUp) {
            scePthreadMutexLock(&g_aprefMtx);
            for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++) if (g_apref[i].ready) acached++;
            scePthreadMutexUnlock(&g_aprefMtx);
        }
        snprintf(b, sizeof(b),
                 "hls v%d/%d %s %dp %dk%s%s seg=%d/%d pf=%d/%d vf=%dms/%dKB/rc%d/f%d af=%d/%d %dms/%dKB/rc%d/f%d",
                 g_curVariant + 1, g_variantCount, codec_name(v->codec), v->height,
                 v->bw / 1000, v->fps ? (v->fps > 30 ? "60" : "") : "",
                 g_sepAudio ? " SEPAUDIO" : "", g_segIdx, g_segCount,
                 cached, g_prefUp ? g_prefDepth : 0,
                 g_vLastMs, g_vLastBytes / 1024, g_vLastRc, g_vFailCount,
                 acached, g_aprefUp ? HLS_AUDIO_PREFETCH_SLOTS : 0,
                 g_aLastMs, g_aLastBytes / 1024, g_aLastRc, g_aFailCount);
        return b;
    }
    if (g_active) {
        int cached = 0;
        if (g_prefUp) {
            scePthreadMutexLock(&g_prefMtx);
            for (int i = 0; i < HLS_PREFETCH_MAX; i++) if (g_pref[i].ready) cached++;
            scePthreadMutexUnlock(&g_prefMtx);
        }
        snprintf(b, sizeof(b),
                 "hls media%s seg=%d/%d pf=%d/%d vf=%dms/%dKB/rc%d/f%d url=%s",
                 g_isLive ? " live" : "", g_segIdx, g_segCount,
                 cached, g_prefUp ? g_prefDepth : 0,
                 g_vLastMs, g_vLastBytes / 1024, g_vLastRc, g_vFailCount,
                 g_vLastUrl);
        return b;
    }
    return g_dbg;   // pre-roll / errors
}

int hls_is_url(const char *url) {
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    if (n >= 5 && strncmp(url + n - 5, ".m3u8", 5) == 0) return 1;
    if (n >= 4 && strncmp(url + n - 4, ".m3u", 4) == 0)  return 1;
    return 0;
}

int hls_generation(void) { return g_segGen; }
int hls_reset_generation(void) { return g_resetGen; }
int hls_is_live(void) { return g_isLive; }
int hls_can_segment_demux(void) {
    return g_active && g_isLive && !g_initSeg && !g_sepAudio && g_variantCount == 0;
}

static void free_segs(void) {
    if (g_segs) {
        for (int i = 0; i < g_segCount; i++) free(g_segs[i]);
        free(g_segs);
        g_segs = NULL;
    }
    free(g_initSeg); g_initSeg = NULL; g_mediaUrl[0] = '\0';
    g_segCount = 0; g_segIdx = 0;
}

// free_asegs is defined alongside the audio path below.
static void free_asegs(void);

static void prefetch_clear_locked(void) {
    for (int i = 0; i < HLS_PREFETCH_MAX; i++) {
        if (g_pref[i].buf) free(g_pref[i].buf);
        memset(&g_pref[i], 0, sizeof(g_pref[i]));
        g_pref[i].seg = -1;
    }
    if (g_memBuf) free(g_memBuf);
    g_memSeg = -1; g_memBuf = NULL; g_memLen = g_memPos = 0;
}

static void prefetch_stop(void) {
    if (g_prefUp) {
        scePthreadMutexLock(&g_prefMtx);
        g_prefStop = 1;
        scePthreadCondSignal(&g_prefCond);
        scePthreadMutexUnlock(&g_prefMtx);
        aseg_abort();
        scePthreadJoin(g_prefThread, NULL);
        scePthreadCondDestroy(&g_prefCond);
        scePthreadMutexDestroy(&g_prefMtx);
        g_prefUp = 0;
    }
    prefetch_clear_locked();
}

static int prefetch_is_fetching_locked(int seg) {
    for (int i = 0; i < HLS_PREFETCH_MAX; i++)
        if (g_pref[i].fetching && g_pref[i].seg == seg) return 1;
    return 0;
}

static int prefetch_has_locked(int seg) {
    for (int i = 0; i < HLS_PREFETCH_MAX; i++)
        if ((g_pref[i].ready || g_pref[i].fetching) && g_pref[i].seg == seg) return 1;
    return 0;
}

static void *prefetch_main(void *arg) {
    (void)arg;
    for (;;) {
        int slot = -1, seg = -1;
        char url[2048];
        url[0] = '\0';

        scePthreadMutexLock(&g_prefMtx);
        if (g_prefStop) { scePthreadMutexUnlock(&g_prefMtx); break; }

        // For live HLS, let the worker own the current segment too. This avoids
        // the demux thread doing a duplicate blocking fetch while the worker is
        // already downloading the same sliding-window segment.
        int base = g_segIdx + (g_isLive ? 0 : 1);
        int depth = g_prefDepth;
        if (depth < 1) depth = 1;
        if (depth > HLS_PREFETCH_MAX) depth = HLS_PREFETCH_MAX;
        for (int want = base; want < base + depth && want < g_segCount; want++) {
            if (prefetch_has_locked(want)) continue;
            for (int i = 0; i < HLS_PREFETCH_MAX; i++) {
                if (!g_pref[i].ready && !g_pref[i].fetching) { slot = i; seg = want; break; }
            }
            if (slot >= 0) break;
        }

        if (slot < 0 || seg < 0 || seg >= g_segCount || !g_segs || !g_segs[seg]) {
            scePthreadCondTimedwait(&g_prefCond, &g_prefMtx, 300 * 1000);
            scePthreadMutexUnlock(&g_prefMtx);
            continue;
        }

        strncpy(url, g_segs[seg], sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        g_pref[slot].seg = seg;
        g_pref[slot].fetching = 1;
        scePthreadMutexUnlock(&g_prefMtx);

        uint8_t *buf = NULL; int len = 0;
        uint64_t t0 = sceKernelGetProcessTime();
        int rc = aseg_fetch(url, &buf, &len);
        int ms = (int)((sceKernelGetProcessTime() - t0) / 1000);
        short_url(url, g_vLastUrl, sizeof(g_vLastUrl));
        g_vLastRc = rc; g_vLastMs = ms; g_vLastBytes = len;
        if (rc != 0) g_vFailCount++;

        scePthreadMutexLock(&g_prefMtx);
        if (g_prefStop) {
            if (buf) free(buf);
            scePthreadMutexUnlock(&g_prefMtx);
            break;
        }
        if (g_pref[slot].seg == seg && g_pref[slot].fetching) {
            g_pref[slot].fetching = 0;
            if (rc == 0 && buf && len > 0) {
                g_pref[slot].buf = buf;
                g_pref[slot].len = len;
                g_pref[slot].ready = 1;
            } else {
                if (buf) free(buf);
                memset(&g_pref[slot], 0, sizeof(g_pref[slot]));
                g_pref[slot].seg = -1;
            }
        } else if (buf) {
            free(buf);
        }
        scePthreadCondSignal(&g_prefCond);
        scePthreadMutexUnlock(&g_prefMtx);
    }
    return NULL;
}

static void prefetch_start(void) {
    if (g_prefUp) return;
    // Live prefetch needs its own sequence-numbered queue. Reusing the VOD
    // index cache can race tiny sliding windows and was observed to crash at
    // the live edge. Keep v02.80 live-safe; VOD still prefetches normally.
    if (g_isLive) return;
    configure_prefetch_depth();
    for (int i = 0; i < HLS_PREFETCH_MAX; i++) g_pref[i].seg = -1;
    g_prefStop = 0;
    scePthreadMutexInit(&g_prefMtx, NULL, "ps4cast_hlsp_m");
    scePthreadCondInit(&g_prefCond, NULL, "ps4cast_hlsp_c");
    if (scePthreadCreate(&g_prefThread, NULL, prefetch_main, NULL, "ps4cast_hlsp") == 0) {
        g_prefUp = 1;
    } else {
        scePthreadCondDestroy(&g_prefCond);
        scePthreadMutexDestroy(&g_prefMtx);
    }
}

static int prefetch_take(int seg) {
    if (!g_prefUp) return 0;
    scePthreadMutexLock(&g_prefMtx);
    for (int i = 0; i < HLS_PREFETCH_MAX; i++) {
        if (g_pref[i].ready && g_pref[i].seg == seg && g_pref[i].buf && g_pref[i].len > 0) {
            g_memSeg = seg;
            g_memBuf = g_pref[i].buf;
            g_memLen = g_pref[i].len;
            g_memPos = 0;
            memset(&g_pref[i], 0, sizeof(g_pref[i]));
            g_pref[i].seg = -1;
            scePthreadCondSignal(&g_prefCond);
            scePthreadMutexUnlock(&g_prefMtx);
            return 1;
        }
    }
    scePthreadMutexUnlock(&g_prefMtx);
    return 0;
}

static int prefetch_wait_take(int seg) {
    if (!g_prefUp) return 0;
    int got = 0;
    scePthreadMutexLock(&g_prefMtx);
    scePthreadCondSignal(&g_prefCond);
    for (int tries = 0; tries < 30 && !got && !g_prefStop; tries++) {
        for (int i = 0; i < HLS_PREFETCH_MAX; i++) {
            if (g_pref[i].ready && g_pref[i].seg == seg && g_pref[i].buf && g_pref[i].len > 0) {
                g_memSeg = seg;
                g_memBuf = g_pref[i].buf;
                g_memLen = g_pref[i].len;
                g_memPos = 0;
                memset(&g_pref[i], 0, sizeof(g_pref[i]));
                g_pref[i].seg = -1;
                got = 1;
                break;
            }
        }
        if (got || !prefetch_is_fetching_locked(seg)) break;
        scePthreadCondTimedwait(&g_prefCond, &g_prefMtx, 150 * 1000);
    }
    scePthreadCondSignal(&g_prefCond);
    scePthreadMutexUnlock(&g_prefMtx);
    return got;
}

static void apref_clear_locked(void) {
    for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++) {
        if (g_apref[i].buf) free(g_apref[i].buf);
        memset(&g_apref[i], 0, sizeof(g_apref[i]));
        g_apref[i].seg = -1;
    }
}

static void apref_stop(void) {
    if (g_aprefUp) {
        scePthreadMutexLock(&g_aprefMtx);
        g_aprefStop = 1;
        scePthreadCondSignal(&g_aprefCond);
        scePthreadMutexUnlock(&g_aprefMtx);
        aseg_abort();
        scePthreadJoin(g_aprefThread, NULL);
        scePthreadCondDestroy(&g_aprefCond);
        scePthreadMutexDestroy(&g_aprefMtx);
        g_aprefUp = 0;
    }
    apref_clear_locked();
}

static int apref_has_locked(int seg) {
    for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++)
        if ((g_apref[i].ready || g_apref[i].fetching) && g_apref[i].seg == seg) return 1;
    return 0;
}

static void *apref_main(void *arg) {
    (void)arg;
    for (;;) {
        int slot = -1, seg = -1;
        char url[2048];
        url[0] = '\0';

        scePthreadMutexLock(&g_aprefMtx);
        if (g_aprefStop || g_aAbort) { scePthreadMutexUnlock(&g_aprefMtx); break; }

        int base = g_asegIdx;
        for (int want = base; want < base + HLS_AUDIO_PREFETCH_SLOTS && want < g_asegCount; want++) {
            if (apref_has_locked(want)) continue;
            for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++) {
                if (!g_apref[i].ready && !g_apref[i].fetching) { slot = i; seg = want; break; }
            }
            if (slot >= 0) break;
        }

        if (slot < 0 || seg < 0 || seg >= g_asegCount || !g_asegs || !g_asegs[seg]) {
            scePthreadCondTimedwait(&g_aprefCond, &g_aprefMtx, 300 * 1000);
            scePthreadMutexUnlock(&g_aprefMtx);
            continue;
        }

        strncpy(url, g_asegs[seg], sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        g_apref[slot].seg = seg;
        g_apref[slot].fetching = 1;
        scePthreadMutexUnlock(&g_aprefMtx);

        uint8_t *buf = NULL; int len = 0;
        uint64_t t0 = sceKernelGetProcessTime();
        int rc = aseg_fetch(url, &buf, &len);
        int ms = (int)((sceKernelGetProcessTime() - t0) / 1000);
        short_url(url, g_aLastUrl, sizeof(g_aLastUrl));
        g_aLastRc = rc; g_aLastMs = ms; g_aLastBytes = len;
        if (rc != 0) g_aFailCount++;

        scePthreadMutexLock(&g_aprefMtx);
        if (g_aprefStop || g_aAbort) {
            if (buf) free(buf);
            scePthreadMutexUnlock(&g_aprefMtx);
            break;
        }
        if (g_apref[slot].seg == seg && g_apref[slot].fetching) {
            g_apref[slot].fetching = 0;
            if (rc == 0 && buf && len > 0) {
                g_apref[slot].buf = buf;
                g_apref[slot].len = len;
                g_apref[slot].ready = 1;
            } else {
                if (buf) free(buf);
                memset(&g_apref[slot], 0, sizeof(g_apref[slot]));
                g_apref[slot].seg = -1;
            }
        } else if (buf) {
            free(buf);
        }
        scePthreadCondSignal(&g_aprefCond);
        scePthreadMutexUnlock(&g_aprefMtx);
    }
    return NULL;
}

static void apref_start(void) {
    if (g_aprefUp || !g_audioReady || g_aAbort || g_aInitPending) return;
    for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++) g_apref[i].seg = -1;
    g_aprefStop = 0;
    scePthreadMutexInit(&g_aprefMtx, NULL, "ps4cast_hlsap_m");
    scePthreadCondInit(&g_aprefCond, NULL, "ps4cast_hlsap_c");
    if (scePthreadCreate(&g_aprefThread, NULL, apref_main, NULL, "ps4cast_hlsap") == 0) {
        g_aprefUp = 1;
    } else {
        scePthreadCondDestroy(&g_aprefCond);
        scePthreadMutexDestroy(&g_aprefMtx);
    }
}

static int open_mem_segment(const char *u, int seg) {
    uint8_t *buf = NULL;
    int len = 0;
    short_url(u, g_vLastUrl, sizeof(g_vLastUrl));
    trace_mark("hls mem_fetch2 begin seg=%d idx=%d/%d url=%s", seg, g_segIdx, g_segCount, g_vLastUrl);
    uint64_t t0 = sceKernelGetProcessTime();

    int rc = aseg_fetch(u, &buf, &len);
    trace_mark("hls mem_fetch2 fetched seg=%d rc=%d len=%d", seg, rc, len);

    int ms = (int)((sceKernelGetProcessTime() - t0) / 1000);
    g_vLastRc = rc;
    g_vLastMs = ms;
    g_vLastBytes = len;
    if (rc == 0 && len > HLS_MEM_SEG_CAP) rc = -26;
    if (rc != 0 || !buf || len <= 0) {
        if (buf) free(buf);
        g_vFailCount++;
        if (g_isLive) g_liveFetchFailStreak++;
        trace_mark("hls mem_fetch2 fail seg=%d rc=%d len=%d fail=%d", seg, rc, len, g_vFailCount);
        return -1;
    }
    if (g_memBuf) free(g_memBuf);
    g_memSeg = seg;
    g_memBuf = buf;
    g_memLen = len;
    g_memPos = 0;
    g_liveFetchFailStreak = 0;
    trace_mark("hls mem_fetch2 ok seg=%d len=%d ms=%d", seg, len, ms);
    return 0;
}

static int apref_take_locked(int seg) {
    for (int i = 0; i < HLS_AUDIO_PREFETCH_SLOTS; i++) {
        if (g_apref[i].ready && g_apref[i].seg == seg && g_apref[i].buf && g_apref[i].len > 0) {
            g_aBuf = g_apref[i].buf;
            g_aBufLen = g_apref[i].len;
            g_aBufPos = 0;
            memset(&g_apref[i], 0, sizeof(g_apref[i]));
            g_apref[i].seg = -1;
            scePthreadCondSignal(&g_aprefCond);
            return 1;
        }
    }
    return 0;
}

static int apref_wait_take(int seg) {
    if (!g_aprefUp) return 0;
    int got = 0;
    scePthreadMutexLock(&g_aprefMtx);
    scePthreadCondSignal(&g_aprefCond);
    for (int tries = 0; tries < 40 && !got && !g_aprefStop && !g_aAbort; tries++) {
        got = apref_take_locked(seg);
        if (got) break;
        scePthreadCondTimedwait(&g_aprefCond, &g_aprefMtx, 200 * 1000);
    }
    scePthreadMutexUnlock(&g_aprefMtx);
    return got;
}

void hls_close(void) {
    apref_stop();
    prefetch_stop();
    if (g_open) { httpsrc_close(); g_open = 0; }
    free_segs();
    free_asegs();
    g_active = 0; g_segPos = 0; g_initPending = 0;
}

// Resolve a possibly-relative URL `ref` against `base` into out[cap].
static void resolve_url(const char *base, const char *ref, char *out, int cap) {
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, cap, "%s", ref);
        prefer_plain_s3(out, cap);
        return;
    }
    // scheme://host[:port]
    const char *p = strstr(base, "://");
    if (!p) { snprintf(out, cap, "%s", ref); return; }
    p += 3;
    const char *host_end = strchr(p, '/');
    if (ref[0] == '/') {
        // absolute path: scheme://host + ref
        int hostlen = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, cap, "%.*s%s", hostlen, base, ref);
        prefer_plain_s3(out, cap);
        return;
    }
    // relative path: base up to last '/'
    const char *last = host_end;
    for (const char *s = host_end; s && *s; s++) {
        if (*s == '/') last = s;
        if (*s == '?') break;
    }
    int dirlen = last ? (int)(last - base + 1) : (int)strlen(base);
    snprintf(out, cap, "%.*s%s", dirlen, base, ref);
    prefer_plain_s3(out, cap);
}

// Fetch an entire (small) resource into a malloc'd NUL-terminated buffer.
static char *fetch_all(const char *url, int *outlen) {
    uint8_t *raw = NULL;
    int len = 0;
    char fetchUrl[2048];
    snprintf(fetchUrl, sizeof(fetchUrl), "%s", url);
    prefer_plain_s3(fetchUrl, sizeof(fetchUrl));
    if (aseg_fetch(fetchUrl, &raw, &len) != 0 || !raw || len <= 0 || len >= PLAYLIST_CAP) {
        if (raw) free(raw);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { free(raw); return NULL; }
    memcpy(buf, raw, (size_t)len);
    buf[len] = '\0';
    free(raw);
    if (outlen) *outlen = len;
    return buf;
}

// Trim trailing CR/LF/space.
static void rstrip(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

static int has_unsupported_hls_tags(const char *body) {
    if (strstr(body, "#EXT-X-KEY"))       { snprintf(g_dbg, sizeof(g_dbg), "hls encrypted unsupported"); return 1; }
    if (strstr(body, "#EXT-X-BYTERANGE")) { snprintf(g_dbg, sizeof(g_dbg), "hls byterange unsupported"); return 1; }
    return 0;
}

// Parse a media playlist body into the segment list. base = playlist URL.
static int parse_media(char *body, const char *base) {
    if (has_unsupported_hls_tags(body)) return -2;
    g_segs = malloc(sizeof(char *) * HLS_MAX_SEGMENTS);
    if (!g_segs) return -1;
    g_segCount = 0;
    g_isLive = strstr(body, "#EXT-X-ENDLIST") ? 0 : 1;
    g_targetDurMs = 3000;
    g_mediaSeq = 0;

    char resolved[2048];
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        rstrip(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') {
            const char *td = strstr(line, "#EXT-X-TARGETDURATION:");
            if (td) {
                int s = atoi(td + 22);
                if (s > 0 && s < 120) g_targetDurMs = s * 1000;
            }
            const char *ms = strstr(line, "#EXT-X-MEDIA-SEQUENCE:");
            if (ms) g_mediaSeq = atoi(ms + 22);
            // fMP4 init segment.
            const char *map = strstr(line, "#EXT-X-MAP:");
            if (map) {
                const char *uri = strstr(map, "URI=\"");
                if (uri) {
                    uri += 5;
                    const char *end = strchr(uri, '"');
                    if (end) {
                        char raw[2048];
                        int l = (int)(end - uri);
                        if (l >= (int)sizeof(raw)) l = sizeof(raw) - 1;
                        memcpy(raw, uri, l); raw[l] = '\0';
                        resolve_url(base, raw, resolved, sizeof(resolved));
                        free(g_initSeg);
                        g_initSeg = strdup(resolved);
                    }
                }
            }
            continue;
        }
        if (g_segCount >= HLS_MAX_SEGMENTS) break;
        resolve_url(base, line, resolved, sizeof(resolved));
        g_segs[g_segCount] = strdup(resolved);
        if (g_segs[g_segCount]) g_segCount++;
    }
    return g_segCount > 0 ? 0 : -1;
}

// From a master playlist, pick a smooth PS4 software-decode variant. Prefer
// <=720p and <=5Mbps when available; fall back to the lowest variant above that
// rather than blindly taking a 1080p/60 high-bitrate stream.
static int pick_variant(const char *body, const char *base, char *out, int cap) {
    int best_score = 0x7fffffff, found = 0;
    char bestref[2048] = {0};
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF")) != NULL) {
        int bw = 0, height = 0;
        const char *bwp = strstr(p, "BANDWIDTH=");
        if (bwp) bw = atoi(bwp + 10);
        const char *rp = strstr(p, "RESOLUTION=");
        if (rp) {
            const char *x = strchr(rp, 'x');
            if (x) height = atoi(x + 1);
        }
        // the URI is on the next non-empty, non-# line
        const char *nl = strchr(p, '\n');
        while (nl) {
            const char *ls = nl + 1;
            const char *le = strchr(ls, '\n');
            int llen = le ? (int)(le - ls) : (int)strlen(ls);
            while (llen > 0 && (ls[llen-1] == '\r' || ls[llen-1] == ' ')) llen--;
            if (llen > 0 && ls[0] != '#') {
                int over = 0;
                if (height > 720) over += (height - 720) * 10000;
                if (bw > 5000000) over += (bw - 5000000) / 100;
                int under = (720 - height) > 0 ? (720 - height) * 50 : 0;
                int score = over ? (100000000 + over)
                                 : (under + (bw > 0 ? (5000000 - bw) / 10000 : 500));
                if (score < best_score) {
                    best_score = score;
                    int l = llen < (int)sizeof(bestref) ? llen : (int)sizeof(bestref) - 1;
                    memcpy(bestref, ls, l); bestref[l] = '\0';
                    found = 1;
                }
                break;
            }
            nl = le;
        }
        p += 17;
    }
    if (!found) return -1;
    resolve_url(base, bestref, out, cap);
    return 0;
}

static int g_downshiftReq = 0;   // set by the player when the buffer keeps draining

// Parse all master-playlist variants (codec/res/bitrate/fps), sorted by bw.
static int collect_variants(const char *body, const char *base) {
    g_variantCount = 0;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF")) != NULL && g_variantCount < HLS_MAX_VARIANTS) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        int bw = 0, height = 0, fps = 0, codec = VC_OTHER;
        const char *bwp = strstr(p, "BANDWIDTH="); if (bwp && bwp < eol) bw = atoi(bwp + 10);
        const char *rp = strstr(p, "RESOLUTION="); if (rp && rp < eol) { const char *x = strchr(rp, 'x'); if (x) height = atoi(x + 1); }
        const char *fp = strstr(p, "FRAME-RATE="); if (fp && fp < eol) fps = atoi(fp + 11);
        const char *cp = strstr(p, "CODECS=\"");
        if (cp && cp < eol) { char cbuf[128]; const char *cs = cp + 8; const char *ce = strchr(cs, '"');
            int cl = ce ? (int)(ce - cs) : 0; if (cl > 0 && cl < (int)sizeof(cbuf)) { memcpy(cbuf, cs, cl); cbuf[cl] = '\0'; codec = codec_from_str(cbuf); } }
        char agroup[64] = "";
        const char *ap = strstr(p, "AUDIO=\"");
        if (ap && ap < eol) { const char *as = ap + 7; const char *ae = strchr(as, '"');
            int al = ae ? (int)(ae - as) : 0; if (al > 0 && al < (int)sizeof(agroup)) { memcpy(agroup, as, al); agroup[al] = '\0'; } }
        const char *nl = eol;
        while (nl) {
            const char *ls = nl + 1; const char *le = strchr(ls, '\n');
            int llen = le ? (int)(le - ls) : (int)strlen(ls);
            while (llen > 0 && (ls[llen-1] == '\r' || ls[llen-1] == ' ')) llen--;
            if (llen > 0 && ls[0] != '#') {
                char ref[2048]; int l = llen < (int)sizeof(ref) ? llen : (int)sizeof(ref) - 1;
                memcpy(ref, ls, l); ref[l] = '\0';
                resolve_url(base, ref, g_variants[g_variantCount].url, sizeof(g_variants[0].url));
                g_variants[g_variantCount].bw = bw; g_variants[g_variantCount].height = height;
                g_variants[g_variantCount].fps = fps; g_variants[g_variantCount].codec = codec;
                strncpy(g_variants[g_variantCount].agroup, agroup, sizeof(g_variants[0].agroup) - 1);
                g_variants[g_variantCount].agroup[sizeof(g_variants[0].agroup) - 1] = '\0';
                g_variantCount++;
                break;
            }
            nl = le;
        }
        p += 17;
    }
    for (int i = 1; i < g_variantCount; i++) {     // insertion sort by bandwidth
        HlsVariant v = g_variants[i]; int j = i - 1;
        while (j >= 0 && g_variants[j].bw > v.bw) { g_variants[j+1] = g_variants[j]; j--; }
        g_variants[j+1] = v;
    }
    return g_variantCount;
}

// Variant score (lower = better) now that H.264 hardware decode is solid.
// Prefer H.264 up to 1080p, avoid 4K/HEVC/VP9/AV1, and keep 60fps as a
// cautious opt-in unless it is the only good option.
static int variant_score(const HlsVariant *v) {
    int s = 0;
    switch (v->codec) {                         // codec is the dominant factor
        case VC_H264:  s += 0;        break;
        case VC_HEVC:  s += 700000;   break;
        case VC_VP9:   s += 800000;   break;
        case VC_AV1:   s += 1000000;  break;
        default:       s += 250000;   break;    // unknown: cautious penalty
    }
    if (v->height > 1080)      s += 3000000;     // 4K+: reject unless no alternative
    else if (v->height <= 0)   s += 20000;
    else                       s += (1080 - v->height) / 4; // prefer 1080 over 720/360
    if (v->bw > 12000000)      s += (v->bw - 12000000) / 100;
    if (v->fps > 30)           s += 25000;       // 50/60fps is harder to present
    // tie-break: prefer quality up to ~10Mbps for H.264 1080p
    int q = v->bw < 10000000 ? v->bw : 10000000;
    s += (10000000 - q) / 100000;
    return s;
}

// Best-scoring variant; if maxBw>0, only consider variants strictly below it
// (used for stepping down). Returns index, or -1 if none.
static int pick_best(int maxBw) {
    int best = -1, bestScore = 0x7fffffff;
    for (int i = 0; i < g_variantCount; i++) {
        if (maxBw > 0 && g_variants[i].bw >= maxBw) continue;
        int sc = variant_score(&g_variants[i]);
        if (sc < bestScore) { bestScore = sc; best = i; }
    }
    return best;
}
static int pick_start_variant(void) { int b = pick_best(0); return b < 0 ? 0 : b; }

// Extract a quoted attribute value (KEY="value") from a single playlist line.
// Returns 1 and fills out[] if found, else 0.
static int attr_quoted(const char *line, const char *key, char *out, int cap) {
    const char *k = strstr(line, key);
    if (!k) return 0;
    k += strlen(key);
    if (*k != '"') return 0;
    k++;
    const char *e = strchr(k, '"');
    if (!e) return 0;
    int l = (int)(e - k);
    if (l >= cap) l = cap - 1;
    memcpy(out, k, l); out[l] = '\0';
    return 1;
}

// Scan a master playlist for the audio rendition (EXT-X-MEDIA:TYPE=AUDIO) whose
// GROUP-ID matches `group`, preferring DEFAULT=YES. Only renditions that carry
// their own URI (separately-delivered audio) are considered. Resolves the chosen
// URI against `base` into out[]. Returns 0 on success, -1 if none found.
static int find_audio_uri(const char *body, const char *base, const char *group,
                          char *out, int cap) {
    char bestUri[2048] = ""; int bestRank = -1;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-MEDIA")) != NULL) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        int linelen = (int)(eol - p);
        char line[1024];
        int cl = linelen < (int)sizeof(line) ? linelen : (int)sizeof(line) - 1;
        memcpy(line, p, cl); line[cl] = '\0';
        p = eol;
        if (!strstr(line, "TYPE=AUDIO")) continue;
        char uri[2048];
        if (!attr_quoted(line, "URI=", uri, sizeof(uri))) continue;  // muxed default: skip
        char gid[64] = ""; attr_quoted(line, "GROUP-ID=", gid, sizeof(gid));
        int isDefault = strstr(line, "DEFAULT=YES") != NULL;
        int matches = (group && group[0] && strcmp(gid, group) == 0);
        int rank = (matches ? 2 : 0) + (isDefault ? 1 : 0);  // matching group + default = best
        if (rank > bestRank) { bestRank = rank; strncpy(bestUri, uri, sizeof(bestUri) - 1); bestUri[sizeof(bestUri)-1] = '\0'; }
    }
    if (bestRank < 0) return -1;
    resolve_url(base, bestUri, out, cap);
    return 0;
}

// Parse an audio media playlist body into the audio segment list (mirrors
// parse_media but for the separate audio path). base = the audio playlist URL.
static int parse_audio_segs(char *body, const char *base) {
    g_asegs = malloc(sizeof(char *) * HLS_MAX_SEGMENTS);
    if (!g_asegs) return -1;
    g_asegCount = 0;
    free(g_aInit); g_aInit = NULL;

    char resolved[2048];
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        rstrip(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') {
            const char *map = strstr(line, "#EXT-X-MAP:");
            if (map) {
                char raw[2048];
                if (attr_quoted(map, "URI=", raw, sizeof(raw))) {
                    resolve_url(base, raw, resolved, sizeof(resolved));
                    free(g_aInit); g_aInit = strdup(resolved);
                }
            }
            continue;
        }
        if (g_asegCount >= HLS_MAX_SEGMENTS) break;
        resolve_url(base, line, resolved, sizeof(resolved));
        g_asegs[g_asegCount] = strdup(resolved);
        if (g_asegs[g_asegCount]) g_asegCount++;
    }
    return g_asegCount > 0 ? 0 : -1;
}

static void free_asegs(void) {
    if (g_asegs) {
        for (int i = 0; i < g_asegCount; i++) free(g_asegs[i]);
        free(g_asegs); g_asegs = NULL;
    }
    free(g_aInit); g_aInit = NULL;
    if (g_aBuf) { free(g_aBuf); g_aBuf = NULL; }
    g_asegCount = 0; g_asegIdx = 0; g_aInitPending = 0;
    g_aBufLen = g_aBufPos = 0; g_audioReady = 0;
}

// Set up the separate audio path for the chosen variant: locate its rendition,
// fetch + parse the audio media playlist. Returns 0 if audio is ready.
static int setup_audio_rendition(const char *masterBody, const char *masterUrl) {
    const char *group = (g_curVariant >= 0) ? g_variants[g_curVariant].agroup : "";
    char auri[2048];
    if (find_audio_uri(masterBody, masterUrl, group, auri, sizeof(auri)) != 0) return -1;
    int len = 0;
    char *abody = fetch_all(auri, &len);          // httpsrc is free at open time
    if (!abody) return -1;
    int rc = parse_audio_segs(abody, auri);
    free(abody);
    if (rc != 0) { free_asegs(); return -1; }
    g_aInitPending = (g_aInit != NULL);
    g_asegIdx = 0; g_aBufLen = g_aBufPos = 0;
    g_aAbort = 0;
    g_audioReady = 1;
    return 0;
}

// Fetch + parse a variant's media playlist, preserving the current segment index
// (HLS variants are time-aligned, so the same index ~= same moment).
static int load_variant(int idx) {
    if (idx < 0 || idx >= g_variantCount) return -1;
    int len = 0;
    char *body = fetch_all(g_variants[idx].url, &len);
    if (!body) return -1;
    int nextSeq = g_mediaSeq + g_segIdx;
    char mediaUrl[2048];
    strncpy(mediaUrl, g_variants[idx].url, sizeof(mediaUrl) - 1);
    mediaUrl[sizeof(mediaUrl) - 1] = '\0';
    free_segs();
    if (parse_media(body, mediaUrl) != 0) { free(body); return -1; }
    strncpy(g_mediaUrl, mediaUrl, sizeof(g_mediaUrl) - 1);
    g_mediaUrl[sizeof(g_mediaUrl) - 1] = '\0';
    free(body);
    g_segIdx = nextSeq - g_mediaSeq;
    if (g_segIdx >= g_segCount) g_segIdx = g_segCount > 0 ? g_segCount - 1 : 0;
    if (g_segIdx < 0) g_segIdx = 0;
    g_curVariant = idx;
    g_open = 0;
    return 0;
}

// Request a one-step bitrate downshift (applied at the next segment boundary).
void hls_request_downshift(void) { g_downshiftReq = 1; }

int hls_open(const char *url) {
    hls_close();
    trace_mark("hls open %s", url);
    g_variantCount = 0; g_curVariant = -1; g_downshiftReq = 0; g_sepAudio = 0;
    g_vLastRc = g_vLastBytes = g_vLastMs = g_vFailCount = 0; g_vLastUrl[0] = '\0';
    g_aLastRc = g_aLastBytes = g_aLastMs = g_aFailCount = 0; g_aLastUrl[0] = '\0';

    int len = 0;
    char *body = fetch_all(url, &len);
    if (!body) { snprintf(g_dbg, sizeof(g_dbg), "hls fetch failed"); return -1; }
    if (strstr(body, "#EXTM3U") == NULL) {
        snprintf(g_dbg, sizeof(g_dbg), "not a playlist");
        free(body); return -2;
    }

    if (strstr(body, "#EXT-X-STREAM-INF")) {
        // Separate audio rendition? (audio in its own playlist, not muxed in the
        // video segments) — our segment streamer only pulls the video variant,
        // so such streams would play silently. Flag it for telemetry.
        g_sepAudio = (strstr(body, "TYPE=AUDIO") != NULL && strstr(body, "URI=") != NULL);
        collect_variants(body, url);
        if (g_variantCount == 0) { free(body); snprintf(g_dbg, sizeof(g_dbg), "no variant"); return -3; }
        g_segIdx = 0;
        if (load_variant(pick_start_variant()) != 0) { free(body); snprintf(g_dbg, sizeof(g_dbg), "variant fetch failed"); return -4; }
        // Separate audio rendition: set up the parallel audio segment list now,
        // while we still hold the master body (needs the chosen variant's group).
        if (g_sepAudio) setup_audio_rendition(body, url);
        free(body);
    } else {
        if (parse_media(body, url) != 0) { snprintf(g_dbg, sizeof(g_dbg), "no segments"); free(body); hls_close(); return -5; }
        strncpy(g_mediaUrl, url, sizeof(g_mediaUrl) - 1);
        g_mediaUrl[sizeof(g_mediaUrl) - 1] = '\0';
        free(body);
    }

    g_initPending = (g_initSeg != NULL);
    g_segIdx = (g_variantCount > 0) ? g_segIdx : 0;
    g_segPos = 0;
    g_segGen = 0;
    g_resetGen = 0;
    g_liveFetchFailStreak = 0;
    g_open = 0;
    g_active = 1;
    g_lastRefreshUs = sceKernelGetProcessTime();
    prefetch_start();
    apref_start();
    snprintf(g_dbg, sizeof(g_dbg), "hls %d segs%s v=%d/%d", g_segCount,
             g_initSeg ? " +init" : "", g_curVariant + 1, g_variantCount);
    return 0;
}

static int refresh_live_playlist(void) {
    if (!g_isLive || !g_mediaUrl[0]) return -1;
    trace_mark("hls refresh begin idx=%d/%d seq=%d", g_segIdx, g_segCount, g_mediaSeq);
    uint64_t now = sceKernelGetProcessTime();
    int waitMs = g_targetDurMs / 2;
    if (waitMs < 500) waitMs = 500;
    if (now - g_lastRefreshUs < (uint64_t)waitMs * 1000ULL) sceKernelUsleep((unsigned)waitMs * 1000);
    g_lastRefreshUs = sceKernelGetProcessTime();

    int len = 0;
    char mediaUrl[2048];
    strncpy(mediaUrl, g_mediaUrl, sizeof(mediaUrl) - 1);
    mediaUrl[sizeof(mediaUrl) - 1] = '\0';
    char *body = fetch_all(mediaUrl, &len);
    if (!body) { trace_mark("hls refresh fetch failed"); return -1; }
    int nextSeq = g_mediaSeq + g_segIdx;
    prefetch_stop();
    free_segs();
    int rc = parse_media(body, mediaUrl);
    strncpy(g_mediaUrl, mediaUrl, sizeof(g_mediaUrl) - 1);
    g_mediaUrl[sizeof(g_mediaUrl) - 1] = '\0';
    free(body);
    if (rc != 0) { trace_mark("hls refresh parse failed rc=%d", rc); return -1; }
    // Avoid replaying/skipping after a sliding-window refresh.
    g_segIdx = nextSeq - g_mediaSeq;
    if (g_segIdx < 0) {
        g_segIdx = 0;             // old target fell off the live window
        g_resetGen++;
        trace_mark("hls live jump reset=%d idx=%d/%d seq=%d", g_resetGen, g_segIdx, g_segCount, g_mediaSeq);
    }
    if (g_segIdx < g_segCount) { trace_mark("hls refresh ok idx=%d/%d seq=%d", g_segIdx, g_segCount, g_mediaSeq); prefetch_start(); return 0; }
    if (g_segIdx > g_segCount) g_segIdx = g_segCount;  // no new segment yet
    prefetch_start();
    trace_mark("hls refresh no_new idx=%d/%d seq=%d", g_segIdx, g_segCount, g_mediaSeq);
    return -1;
}

// Ensure a segment is open in httpsrc; advance through init + segment list.
static int ensure_segment(void) {
    if (g_memBuf) return 0;
    if (g_open) return 0;
    const char *u = NULL;
    if (g_initPending) {
        u = g_initSeg;
    } else if (g_segIdx < g_segCount) {
        if (prefetch_take(g_segIdx)) return 0;
        if (g_isLive && prefetch_wait_take(g_segIdx)) return 0;
        u = g_segs[g_segIdx];
    } else {
        if (refresh_live_playlist() == 0 && g_segIdx < g_segCount) u = g_segs[g_segIdx];
        else return g_isLive ? -2 : -1; // live: wait for next segment; VOD: EOF
    }
    if (g_isLive && !g_initPending) {
        if (open_mem_segment(u, g_segIdx) == 0) return 0;
        // A CDN-backed live playlist can expose a segment before every edge has
        // it. Do not advance into segCount on a fetch miss; retry briefly and
        // refresh the playlist so only a real sliding-window jump creates a gap.
        if (g_liveFetchFailStreak >= 2) refresh_live_playlist();
        trace_mark("hls fetch retry idx=%d/%d streak=%d", g_segIdx, g_segCount, g_liveFetchFailStreak);
        return -2;
    }
    short_url(u, g_vLastUrl, sizeof(g_vLastUrl));
    int orc = httpsrc_open(u);
    g_vLastRc = orc;
    if (orc != 0) { g_vFailCount++; return -1; }
    g_vLastBytes = 0;
    g_vLastMs = 0;
    g_vOpenUs = sceKernelGetProcessTime();
    g_open = 1;
    g_segPos = 0;
    return 0;
}

int hls_read(uint8_t *buf, uint32_t len) {
    if (!g_active) return -1;
    for (;;) {
        int es = ensure_segment();
        if (es == -2) { sceKernelUsleep(300 * 1000); continue; }
        if (es != 0) return 0; // EOF

        if (g_memBuf) {
            int avail = g_memLen - g_memPos;
            if (avail > 0) {
                int take = (int)len < avail ? (int)len : avail;
                memcpy(buf, g_memBuf + g_memPos, take);
                g_memPos += take;
                return take;
            }
            free(g_memBuf);
            g_memBuf = NULL; g_memLen = g_memPos = 0; g_memSeg = -1;
            g_segIdx++;
            g_segGen++;
            trace_mark("hls mem drained next_idx=%d/%d gen=%d", g_segIdx, g_segCount, g_segGen);
            if (g_prefUp) {
                scePthreadMutexLock(&g_prefMtx);
                scePthreadCondSignal(&g_prefCond);
                scePthreadMutexUnlock(&g_prefMtx);
            }
            continue;
        }

        int n = httpsrc_read(buf, g_segPos, len);
        if (n > 0) { g_segPos += (uint32_t)n; return n; }

        // Current segment finished — advance to the next one.
        trace_mark("hls httpsrc drained idx=%d/%d pos=%llu", g_segIdx, g_segCount, (unsigned long long)g_segPos);
        httpsrc_close();
        g_vLastBytes = (int)g_segPos;
        g_vLastMs = (int)((sceKernelGetProcessTime() - g_vOpenUs) / 1000);
        g_open = 0;
        if (g_initPending) {
            g_initPending = 0; // init streamed; now media segments
        } else {
            g_segIdx++;
            g_segGen++;
            if (g_prefUp) {
                scePthreadMutexLock(&g_prefMtx);
                scePthreadCondSignal(&g_prefCond);
                scePthreadMutexUnlock(&g_prefMtx);
            }
            // Apply a pending ABR downshift here (safe: between segments). Step to
            // the best-scoring variant with lower bitrate (keeps codec preference).
            if (g_downshiftReq && g_curVariant >= 0) {
                g_downshiftReq = 0;
                int lower = pick_best(g_variants[g_curVariant].bw);
                if (lower >= 0 && lower != g_curVariant) {
                    prefetch_stop();
                    load_variant(lower);
                    prefetch_start();
                }
            }
        }
        // loop to open the next segment
    }
}

int hls_next_segment(uint8_t **outBuf, int *outLen, int *outResetGen) {
    if (outBuf) *outBuf = NULL;
    if (outLen) *outLen = 0;
    if (outResetGen) *outResetGen = g_resetGen;
    if (!g_active || !hls_can_segment_demux()) return -1;

    for (;;) {
        if (g_segIdx >= g_segCount) {
            if (refresh_live_playlist() != 0) {
                sceKernelUsleep(300 * 1000);
                continue;
            }
        }
        if (g_segIdx < 0 || g_segIdx >= g_segCount || !g_segs || !g_segs[g_segIdx]) {
            sceKernelUsleep(200 * 1000);
            continue;
        }

        int seg = g_segIdx;
        if (open_mem_segment(g_segs[seg], seg) == 0 && g_memBuf && g_memLen > 0) {
            if (outBuf) *outBuf = g_memBuf;
            if (outLen) *outLen = g_memLen;
            if (outResetGen) *outResetGen = g_resetGen;
            g_memBuf = NULL;
            g_memLen = g_memPos = 0;
            g_memSeg = -1;
            g_segIdx++;
            g_segGen++;
            trace_mark("hls segdemux take next_idx=%d/%d gen=%d reset=%d", g_segIdx, g_segCount, g_segGen, g_resetGen);
            return 0;
        }

        if (g_memBuf) {
            free(g_memBuf);
            g_memBuf = NULL;
            g_memLen = g_memPos = 0;
            g_memSeg = -1;
        }
        if (g_liveFetchFailStreak >= 2) refresh_live_playlist();
        trace_mark("hls segdemux retry idx=%d/%d streak=%d", g_segIdx, g_segCount, g_liveFetchFailStreak);
        sceKernelUsleep(300 * 1000);
    }
}

// ---- separate audio rendition read path -----------------------------------

int hls_has_audio(void) { return g_audioReady; }

void hls_audio_abort(void) { g_aAbort = 1; apref_stop(); aseg_abort(); }

// Make sure the current in-RAM audio segment has unread bytes; fetch the next
// audio segment (init first) when the current one is drained. Returns 0 ready,
// -1 EOF, -2 transient (fetch failed; skipped — caller should retry).
static int ensure_audio_buf(void) {
    if (g_aBuf && g_aBufPos < g_aBufLen) return 0;
    if (g_aBuf) { free(g_aBuf); g_aBuf = NULL; g_aBufLen = g_aBufPos = 0; }

    const char *u = NULL;
    if (g_aInitPending)               u = g_aInit;
    else if (g_asegIdx < g_asegCount) {
        if (apref_wait_take(g_asegIdx)) { g_asegIdx++; return 0; }
        if (g_aprefUp) { g_asegIdx++; return -2; }
        u = g_asegs[g_asegIdx];
    }
    else                              return -1;          // EOF

    uint8_t *buf = NULL; int len = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    int rc = aseg_fetch(u, &buf, &len);
    int ms = (int)((sceKernelGetProcessTime() - t0) / 1000);
    short_url(u, g_aLastUrl, sizeof(g_aLastUrl));
    g_aLastRc = rc; g_aLastMs = ms; g_aLastBytes = len;
    if (rc != 0) g_aFailCount++;
    if (g_aInitPending) { g_aInitPending = 0; apref_start(); }
    else g_asegIdx++;   // advance either way
    if (rc != 0 || !buf) return -2;                       // skip a bad segment
    g_aBuf = buf; g_aBufLen = len; g_aBufPos = 0;
    if (g_aprefUp) {
        scePthreadMutexLock(&g_aprefMtx);
        scePthreadCondSignal(&g_aprefCond);
        scePthreadMutexUnlock(&g_aprefMtx);
    }
    return 0;
}

int hls_audio_read(uint8_t *buf, uint32_t len) {
    if (!g_audioReady || g_aAbort || len == 0) return 0;
    for (int tries = 0; tries < 6; tries++) {
        if (g_aAbort) return 0;
        int rc = ensure_audio_buf();
        if (rc == -1) return 0;       // EOF
        if (rc == -2) continue;       // transient fetch failure — try next segment
        int avail = g_aBufLen - g_aBufPos;
        int take = (int)len < avail ? (int)len : avail;
        memcpy(buf, g_aBuf + g_aBufPos, take);
        g_aBufPos += take;
        if (g_aBufPos >= g_aBufLen && g_aprefUp) {
            scePthreadMutexLock(&g_aprefMtx);
            scePthreadCondSignal(&g_aprefCond);
            scePthreadMutexUnlock(&g_aprefMtx);
        }
        return take;
    }
    return 0;
}
