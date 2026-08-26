#include "hls.h"
extern void watchdog_kick(void);
extern const char *watchdog_note(const char *w);
#include "httpsrc.h"
#include "aseg.h"
#include "hls_parse.h"
#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/libkernel.h>

// HLS reader: parse the .m3u8, then stream the segments back-to-back through
// httpsrc so ffmpeg's mpegts/mov demuxer sees one continuous byte stream. All
// networking (http + https/BearSSL + redirects) is reused from httpsrc.

#define PLAYLIST_CAP     (4 * 1024 * 1024)
#define HLS_MEM_SEG_CAP  (16 * 1024 * 1024)
#define HLS_PREFETCH_MAX 10          // deeper buffer: thin-margin live CDN streams need more segments ahead
#define HLS_AUDIO_PREFETCH_SLOTS 3

// Playlist state (segments, variants, live flags) lives in hls_parse.c's
// HlsPlaylist so the pure parsing/selection code is host-testable. The macros
// below keep every existing reference in this file unchanged.
static HlsPlaylist g_pl;
#define g_segs         g_pl.segs
#define g_segCount     g_pl.segCount
#define g_segDisc      g_pl.segDisc
#define g_segDurMs     g_pl.segDurMs
#define g_totalDurMs   g_pl.totalDurMs
#define g_pendDisc     g_pl.pendDisc
#define g_initSeg      g_pl.initSeg
#define g_isLive       g_pl.isLive
#define g_targetDurMs  g_pl.targetDurMs
#define g_mediaSeq     g_pl.mediaSeq
#define g_variants     g_pl.variants
static int     g_segIdx;          // current segment being read
static int     g_initPending;     // 1 = init segment still to be streamed first
static char    g_mediaUrl[2048];  // current media playlist URL (for live refresh)
#define g_variantCount g_pl.variantCount
// Does the body we parsed actually belong to the URL we asked for?
static volatile unsigned g_openGen = 0;   // bumped on every close/open
// Retry traces fire ~3x/second and were evicting everything else from the ring
// (that is why 'hls open' lines were never visible when diagnosing the wedge).
static unsigned g_retryTick = 0;
#define RETRY_TRACE (((g_retryTick++) & 15) == 0)
// Hard ceiling on how long a read may sit retrying before it gives up.
// A live stream legitimately waits a few seconds for the next segment, but an
// UNBOUNDED wait let one bad channel freeze the app: the MAIN thread parks in
// this loop inside avformat_find_stream_info (measured: probe info=97526ms,
// 43s of retries), so it never services /chan again and every later channel
// change is silently ignored while the player still reports "stopped".
// It is invisible to the freeze watchdog because aseg pets it from this very
// thread -- alive, but useless.
#define HLS_READ_STALL_US (20ULL * 1000 * 1000)
static char    g_dbgVarUrl[56] = "";
static char    g_dbgSeg0[56] = "";
static uint64_t g_lastRefreshUs;
static uint64_t g_segPos;         // byte cursor within current open segment
static volatile int g_segGen;     // increments when advancing to the next media segment
static volatile int g_resetGen;   // increments when live HLS skips/jumps and player must re-anchor
static int     g_open;            // a segment is open in httpsrc
static int     g_active;
static int     g_externalSegFetch; // player segment-demux owns video fetching
static volatile int *g_segStopFlag = NULL;   // points at the fetch thread's stop flag; checked in hls_next_segment's retry loop
void hls_set_seg_stop_flag(volatile int *p) { g_segStopFlag = p; }
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
static volatile uint64_t g_hlsRxBytes;   // total bytes fetched (video+audio), for the stats overlay
static char              g_vLastUrl[96] = "";
static char              g_segFail[96] = "";   // last VOD-segment open failure detail
static int               g_forceAsegSeg = 0;   // httpsrc dead for this origin: serve via aseg
static int               g_vLastRc, g_vLastBytes, g_vLastMs, g_vFailCount;
static uint64_t          g_vOpenUs;

static int        g_curVariant = -1;   // index into g_variants
static int        g_sepAudio = 0;      // master has a separate audio rendition (EXT-X-MEDIA)
static volatile int g_upshiftReq;
// fMP4 cannot switch variant in place: each variant has its OWN init segment and
// the continuous AVIO/MOV demuxer cannot take a codec/resolution change mid-stream.
// So a switch is recorded here and applied by reopening at the current position
// (the same machinery an fMP4 seek uses). Sticky across that reopen by design.
static int        g_preferVariant = -1;
static volatile int g_variantSwitchPending = 0;
static char       g_lastMasterUrl[2048];
static int        g_fastFetchStreak;
static int        g_estBandwidth;
static int        g_autoMaxHeight = 720;
static int        g_requestedMaxHeight = 720;

static int next_higher_variant(int current);

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

extern uint64_t sceKernelGetProcessTime(void); // microseconds, monotonic

static void short_url(const char *url, char *out, int cap) {
    if (!url || cap <= 0) return;
    int n = (int)strlen(url);
    const char *p = url;
    if (n >= cap) p = url + n - (cap - 1);
    snprintf(out, cap, "%s", p);
}

static void configure_prefetch_depth(void) {
    int bw = (g_curVariant >= 0 && g_curVariant < g_variantCount) ? g_variants[g_curVariant].bw : 0;
    int d = 6;
    // Live CDN streams (sliding window) buffered too shallow (d=2) -> constant
    // rebuffer on thin-bandwidth-margin links. Buffer much deeper; prefetch_main
    // only fetches segments that actually exist in the window, so this self-caps.
    if (g_isLive) d = 8;
    else if (bw > 0 && bw <= 4000000) d = 8;
    else if (bw > 0 && bw <= 8000000) d = 6;
    else if (bw > 14000000) d = 3;
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
                 "hls v%d/%d %s %dp %dk%s%s%s seg=%d/%d pf=%d/%d vf=%dms/%dKB/rc%d/f%d af=%d/%d %dms/%dKB/rc%d/f%d st=%d[%s] reuse=%d hop=%d plen=%d p=%s VAR=%s SEG0=%s",
                 g_curVariant + 1, g_variantCount, codec_name(v->codec), v->height,
                 v->bw / 1000, v->fps ? (v->fps > 30 ? "60" : "") : "",
                 g_sepAudio ? " SEPAUDIO" : "", g_initSeg ? " FMP4LOCK" : "",
                 g_segIdx, g_segCount,
                 cached, g_prefUp ? g_prefDepth : 0,
                 g_vLastMs, g_vLastBytes / 1024, g_vLastRc, g_vFailCount,
                 acached, g_aprefUp ? HLS_AUDIO_PREFETCH_SLOTS : 0,
                 g_aLastMs, g_aLastBytes / 1024, g_aLastRc, g_aFailCount,
                 aseg_last_status(), aseg_last_line(), aseg_bad_reuse(), aseg_bad_hop(),
                 aseg_bad_pathlen(), aseg_bad_path(), g_dbgVarUrl, g_dbgSeg0);
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
                 "hls media%s seg=%d/%d pf=%d/%d vf=%dms/%dKB/rc%d/f%d url=%s%s%s",
                 g_isLive ? " live" : "", g_segIdx, g_segCount,
                 cached, g_prefUp ? g_prefDepth : 0,
                 g_vLastMs, g_vLastBytes / 1024, g_vLastRc, g_vFailCount,
                 g_vLastUrl,
                 g_segFail[0] ? " segfail=" : "", g_segFail);
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
int hls_is_fmp4(void) { return g_active && g_initSeg != NULL; }
// Seek is refused for these: there is no audio segment index to move in step,
// so repositioning video alone would desync A/V.
int hls_has_separate_audio(void) { return g_active && g_sepAudio; }
int hls_can_segment_demux(void) {
    // Segment-demux feeds TS (Annex-B) media playlists straight to the hardware
    // H.264 decoder. Enabled for BOTH live and VOD: the "VOD GPU-fault after ~30s"
    // that this used to be disabled for was actually the SceShellUI crash on an
    // ANONYMOUS user (CE-36329-3) — NOT a decode/GPU fault — and is fixed
    // separately (user-guard + PowerTick). fMP4 (EXT-X-MAP) stays on the generic
    // continuous AVIO/MOV demuxer; its H.264 packets may still use hardware decode.
    // Runtime escape hatch: POST /hwdecode disables HW globally (-> SW) without a
    // rebuild if a specific VOD stream ever misbehaves on HW.
    return g_active && !g_initSeg;
}
void hls_set_decode_cap(int max_height) {
    if (max_height < 240) max_height = 240;
    if (max_height > 1080) max_height = 1080;
    g_requestedMaxHeight = max_height;
    g_autoMaxHeight = max_height;
}
int hls_can_seek(void) {
    // fMP4 (EXT-X-MAP) IS seekable: the init segment just has to be re-sent
    // before the target media segment, which hls_seek_clamped now does. Separate
    // audio still blocks it -- there is no audio segment index to move in step,
    // so seeking video alone would desync A/V.
    return g_active && !g_isLive && !g_sepAudio &&
           g_segCount > 0 && g_totalDurMs > 0;
}

// Live-FLAGGED playlists may still hold seekable VOD content ("fake-live":
// movie CDNs omit EXT-X-ENDLIST so players cannot download the file). Allow a
// segment-index seek clamped to the currently known window; on a genuine live
// sliding window this simply clamps to the oldest retained segment.
int hls_can_seek_clamped(void) {
    return g_active && !g_sepAudio && g_segCount > 0;
}
double hls_duration(void) {
    return g_totalDurMs > 0 ? (double)g_totalDurMs / 1000.0 : 0.0;
}
// VOD playback has consumed every segment -> a clean end-of-stream.
int hls_at_eof(void) { return g_active && !g_isLive && g_segIdx >= g_segCount; }

// Total bytes fetched from the network (for the on-screen network-speed stat).
uint64_t hls_rx_total(void) { return g_hlsRxBytes; }

// Read-ahead fill as a percent of the target prefetch depth (the HLS "buffer").
int hls_buffer_pct(void) {
    if (!g_active || !g_prefUp) return 0;
    int cached = 0;
    scePthreadMutexLock(&g_prefMtx);
    for (int i = 0; i < HLS_PREFETCH_MAX; i++) if (g_pref[i].ready) cached++;
    scePthreadMutexUnlock(&g_prefMtx);
    int depth = g_prefDepth > 0 ? g_prefDepth : 1;
    int pct = cached * 100 / depth;
    return pct > 100 ? 100 : pct;
}

static void rstrip(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

static void free_segs(void) {
    hlspl_free(&g_pl);
    g_mediaUrl[0] = '\0';
    g_segIdx = 0;
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
        watchdog_kick();
        watchdog_note("join/pref");
        scePthreadJoin(g_prefThread, NULL);
        watchdog_kick();
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

        // Conventional ABR: start conservatively, then promote only after
        // several complete segments prove enough sustained headroom for the
        // next rendition. A single cache hit must never trigger an upshift.
        if (rc == 0 && len > 0 && ms > 0 && seg >= 0 && seg < g_segCount &&
            g_curVariant >= 0 && g_curVariant < g_variantCount) {
            int sample = (int)(((uint64_t)len * 8ULL * 1000ULL) / (uint64_t)ms);
            g_estBandwidth = g_estBandwidth > 0 ? (g_estBandwidth * 3 + sample) / 4 : sample;
            int higher = next_higher_variant(g_curVariant);
            int dur = g_segDurMs[seg] > 0 ? g_segDurMs[seg] : g_targetDurMs;
            if (higher >= 0 && dur > 0 && ms * 4 < dur * 3 &&
                (int64_t)g_estBandwidth * 10 > (int64_t)g_variants[higher].bw * 14) {
                if (++g_fastFetchStreak >= 3) { g_upshiftReq = 1; g_fastFetchStreak = 0; }
            } else {
                g_fastFetchStreak = 0;
            }
        } else if (rc != 0) {
            g_fastFetchStreak = 0;
        }

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
                g_hlsRxBytes += (uint64_t)len;
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
    if (g_prefUp || g_externalSegFetch) return;
    aseg_resume();   // a mid-playback variant switch stops then restarts us; without this the sticky abort would wedge the new worker
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

void hls_set_external_segment_fetch(int on) {
    g_externalSegFetch = on ? 1 : 0;
    if (g_externalSegFetch) {
        prefetch_stop();
        aseg_resume(); // prefetch_stop raises the sticky abort; external owner starts next
    }
    else if (g_active) prefetch_start();
}

// Move a finite muxed TS playlist to the segment containing `seconds`. The
// player stops its segment-fetch thread before calling us, so this function is
// the sole owner of g_segIdx while the prefetch cache is rebuilt.
int hls_seek_time(double seconds, double *actual) {
    if (!hls_can_seek()) return -1;
    return hls_seek_clamped(seconds, actual);
}

int hls_seek_clamped(double seconds, double *actual) {
    if (!hls_can_seek_clamped()) return -1;
    if (seconds < 0) seconds = 0;
    if (seconds > hls_duration()) seconds = hls_duration();

    prefetch_stop();
    aseg_resume(); // external segment fetch resumes after the old cache is joined
    if (g_open) { httpsrc_close(); g_open = 0; }
    if (g_memBuf) { free(g_memBuf); g_memBuf = NULL; }
    g_memSeg = -1; g_memLen = g_memPos = 0;

    int target = g_segCount;
    int64_t startMs = 0;
    int64_t wantMs = (int64_t)(seconds * 1000.0);
    for (int i = 0; i < g_segCount; i++) {
        int dur = g_segDurMs[i] > 0 ? g_segDurMs[i] : g_targetDurMs;
        if (wantMs < startMs + dur) { target = i; break; }
        startMs += dur;
    }
    if (target >= g_segCount) startMs = g_totalDurMs;
    g_segIdx = target;
    g_segPos = 0;
    // Re-arm the fMP4 init segment: after a reposition the demuxer is rebuilt
    // from scratch, so it needs ftyp/moov again before the media segment or it
    // has no codec configuration. Leaving this at 0 is why fMP4 seek was gated
    // off entirely (scrubbing was a silent no-op on every EXT-X-MAP playlist).
    g_initPending = (g_initSeg != NULL);
    g_segGen++;
    g_resetGen++;
    if (actual) *actual = (double)startMs / 1000.0;
    trace_mark("hls seek req=%.3f seg=%d/%d actual=%.3f reset=%d",
               seconds, target, g_segCount, (double)startMs / 1000.0, g_resetGen);
    prefetch_start();
    return 0;
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
        watchdog_kick();
        watchdog_note("join/apref");
        scePthreadJoin(g_aprefThread, NULL);
        watchdog_kick();
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
    aseg_resume();
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
    g_hlsRxBytes += (uint64_t)len;
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
    trace_mark("hls close gen=%u->%u self=%p", g_openGen, g_openGen + 1, (void *)scePthreadSelf());
    g_openGen++;      // anything still looping from the previous stream is now stale
    g_active = 0;     // ...and hls_next_segment's retry loop re-checks this every pass
    watchdog_note("close/apref-stop");
    apref_stop();
    watchdog_note("close/pref-stop");
    prefetch_stop();
    watchdog_note("close/httpsrc");
    if (g_open) { httpsrc_close(); g_open = 0; }
    watchdog_note("close/free");
    free_segs();
    free_asegs();
    g_active = 0; g_segPos = 0; g_initPending = 0; g_externalSegFetch = 0;
}

// Fetch an entire (small) resource into a malloc'd NUL-terminated buffer.
// Last playlist-fetch failure detail. "hls fetch failed" alone was useless: the
// same channel fetches fine one minute and fails the next, and the aseg return
// code distinguishes DNS (-2/-3) from connect (-1), HTTP status (-4), redirect
// exhaustion (-5) and the time budget (-12).
static int g_lastFetchRc, g_lastFetchLen;
static char *fetch_all(const char *url, int *outlen) {
    uint8_t *raw = NULL;
    int len = 0;
    char fetchUrl[2048];
    snprintf(fetchUrl, sizeof(fetchUrl), "%s", url);
    hlspl_prefer_plain_s3(fetchUrl, sizeof(fetchUrl));
    int frc = aseg_fetch(fetchUrl, &raw, &len);
    g_lastFetchRc = frc; g_lastFetchLen = len;
    if (frc != 0 || !raw || len <= 0 || len >= PLAYLIST_CAP) {
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
static int g_downshiftReq = 0;   // set by the player when the buffer keeps draining

static int next_higher_variant(int current) {
    if (current < 0 || current >= g_variantCount) return -1;
    int next = -1;
    for (int i = 0; i < g_variantCount; i++) {
        HlsVariant *v = &g_variants[i];
        if (v->codec != g_variants[current].codec || v->height > g_autoMaxHeight ||
            v->bw <= g_variants[current].bw) continue;
        if (next < 0 || v->bw < g_variants[next].bw) next = i;
    }
    return next;
}

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
    hlspl_resolve_url(base, bestUri, out, cap);
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
                    hlspl_resolve_url(base, raw, resolved, sizeof(resolved));
                    free(g_aInit); g_aInit = strdup(resolved);
                }
            }
            continue;
        }
        if (g_asegCount >= HLS_MAX_SEGMENTS) break;
        hlspl_resolve_url(base, line, resolved, sizeof(resolved));
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
    watchdog_note("lv/fetch");
    { const char *u = g_variants[idx].url; int L = (int)strlen(u);
      snprintf(g_dbgVarUrl, sizeof(g_dbgVarUrl), "%s", L > 50 ? u + L - 50 : u); }
    char *body = fetch_all(g_variants[idx].url, &len);
    watchdog_note("lv/post-fetch");
    if (!body) return -1;
    int nextSeq = g_mediaSeq + g_segIdx;
    char mediaUrl[2048];
    strncpy(mediaUrl, g_variants[idx].url, sizeof(mediaUrl) - 1);
    mediaUrl[sizeof(mediaUrl) - 1] = '\0';
    watchdog_note("lv/free-segs");
    free_segs();
    watchdog_note("lv/parse");
    if (hlspl_parse_media(&g_pl, body, mediaUrl) != 0) { free(body); watchdog_note("lv/done"); return -1; }
    watchdog_note("lv/done");
    strncpy(g_mediaUrl, mediaUrl, sizeof(g_mediaUrl) - 1);
    g_mediaUrl[sizeof(g_mediaUrl) - 1] = '\0';
    free(body);
    g_segIdx = nextSeq - g_mediaSeq;
    { const char *s0 = (g_segCount > 0 && g_segs[0]) ? g_segs[0] : "(none)";
      int L = (int)strlen(s0);
      snprintf(g_dbgSeg0, sizeof(g_dbgSeg0), "%s", L > 50 ? s0 + L - 50 : s0); }
    if (g_segIdx >= g_segCount) g_segIdx = g_segCount > 0 ? g_segCount - 1 : 0;
    if (g_segIdx < 0) g_segIdx = 0;
    g_curVariant = idx;
    g_open = 0;
    return 0;
}

// Request a one-step bitrate downshift (applied at the next segment boundary).
void hls_request_downshift(void) {
    g_downshiftReq = 1;
}

// 1 exactly once when a quality switch is waiting; the player then reopens at the
// current position with g_preferVariant applied.
int hls_take_variant_switch(void) {
    if (!g_variantSwitchPending) return 0;
    g_variantSwitchPending = 0;
    return 1;
}

int hls_open(const char *url) {
    if (strncmp(url ? url : "", g_lastMasterUrl, sizeof(g_lastMasterUrl)) != 0) {
        g_preferVariant = -1;               // different title: start fresh
        snprintf(g_lastMasterUrl, sizeof(g_lastMasterUrl), "%s", url ? url : "");
    }
    g_variantSwitchPending = 0;
    aseg_set_playlist_budget(1);   // small fetches: fail fast so a dead channel can't block the switch
    watchdog_note("open/close");
    hls_close();                   // raises abort + joins the prefetch threads
    g_externalSegFetch = 0;
    aseg_resume();                 // ...so resume only AFTER they are gone, never before
    aseg_clear_error();            // do not report a previous channel's HTTP failure
    watchdog_note("open/master");
    trace_mark("hls open gen=%u self=%p %s", g_openGen, (void *)scePthreadSelf(), url);
    g_variantCount = 0; g_curVariant = -1; g_downshiftReq = 0; g_upshiftReq = 0;
    g_fastFetchStreak = 0; g_estBandwidth = 0; g_autoMaxHeight = g_requestedMaxHeight; g_sepAudio = 0;
    g_vLastRc = g_vLastBytes = g_vLastMs = g_vFailCount = 0; g_vLastUrl[0] = '\0';
    g_segFail[0] = '\0'; g_forceAsegSeg = 0;
    g_aLastRc = g_aLastBytes = g_aLastMs = g_aFailCount = 0; g_aLastUrl[0] = '\0';

    int len = 0;
    char *body = fetch_all(url, &len);
    if (!body) { snprintf(g_dbg, sizeof(g_dbg),
                          "hls fetch failed rc=%d len=%d st=%d[%s] at=%s %s",
                          g_lastFetchRc, g_lastFetchLen, aseg_last_status(),
                          aseg_last_line(), aseg_bad_stage(), aseg_native_debug());
                 { aseg_set_playlist_budget(0); return -1; } }
    if (strstr(body, "#EXTM3U") == NULL) {
        snprintf(g_dbg, sizeof(g_dbg), "not a playlist");
        free(body); { aseg_set_playlist_budget(0); return -2; }
    }

    if (strstr(body, "#EXT-X-STREAM-INF")) {
        // Separate audio rendition? (audio in its own playlist, not muxed in the
        // video segments) — our segment streamer only pulls the video variant,
        // so such streams would play silently. Flag it for telemetry.
        g_sepAudio = (strstr(body, "TYPE=AUDIO") != NULL && strstr(body, "URI=") != NULL);
        watchdog_note("open/variants");
        hlspl_collect_variants(&g_pl, body, url);
        if (g_variantCount == 0) { free(body); snprintf(g_dbg, sizeof(g_dbg), "no variant"); { aseg_set_playlist_budget(0); return -3; } }
        g_segIdx = 0;
        watchdog_note("open/load-variant");
        int startVar = hlspl_pick_start_variant(&g_pl);
        if (g_preferVariant >= 0 && g_preferVariant < g_pl.variantCount)
            startVar = g_preferVariant;       // carried across a quality-switch reopen
        if (load_variant(startVar) != 0) {
            free(body);
            snprintf(g_dbg, sizeof(g_dbg), "variant fetch failed st=%d[%s] at=%s %s",
                     aseg_last_status(), aseg_last_line(), aseg_bad_stage(),
                     aseg_native_debug());
            { aseg_set_playlist_budget(0); return -4; }
        }
        if (g_initSeg) {
            int fixedVariant = hlspl_pick_fmp4_start_variant(&g_pl, g_autoMaxHeight);
            if (fixedVariant >= 0 && fixedVariant != g_curVariant &&
                load_variant(fixedVariant) != 0) {
                free(body);
                snprintf(g_dbg, sizeof(g_dbg), "fmp4 fixed variant fetch failed st=%d[%s] at=%s",
                         aseg_last_status(), aseg_last_line(), aseg_bad_stage());
                { aseg_set_playlist_budget(0); return -4; }
            }
        }
        // Separate audio rendition: set up the parallel audio segment list now,
        // while we still hold the master body (needs the chosen variant's group).
        watchdog_note("open/audio-rend");
        if (g_sepAudio) setup_audio_rendition(body, url);
        free(body);
    } else {
        if (hlspl_parse_media(&g_pl, body, url) != 0) { snprintf(g_dbg, sizeof(g_dbg), "no segments"); free(body); hls_close(); { aseg_set_playlist_budget(0); return -5; } }
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
    g_hlsRxBytes = 0;
    g_open = 0;
    g_active = 1;
    g_lastRefreshUs = sceKernelGetProcessTime();
    watchdog_note("open/pref-start");
    prefetch_start();
    watchdog_note("open/apref-start");
    apref_start();
    watchdog_note("open/done");
    snprintf(g_dbg, sizeof(g_dbg), "hls %d segs%s v=%d/%d", g_segCount,
             g_initSeg ? " +init" : "", g_curVariant + 1, g_variantCount);
    { aseg_set_playlist_budget(0); return 0; }
}

static int refresh_live_playlist(void) {
    if (!g_isLive || !g_mediaUrl[0]) return -1;
    if (RETRY_TRACE) trace_mark("hls refresh begin idx=%d/%d seq=%d self=%p", g_segIdx, g_segCount, g_mediaSeq, (void *)scePthreadSelf());
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
    int rc = hlspl_parse_media(&g_pl, body, mediaUrl);
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
    if (!g_active) return -1;      // closed: never start new work for a dead stream
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
        if (RETRY_TRACE) trace_mark("hls fetch retry idx=%d/%d streak=%d self=%p gen=%u act=%d",
                                    g_segIdx, g_segCount, g_liveFetchFailStreak,
                                    (void *)scePthreadSelf(), g_openGen, g_active);
        return -2;
    }
    short_url(u, g_vLastUrl, sizeof(g_vLastUrl));
    if (!g_forceAsegSeg) {
        int orc = httpsrc_open(u);
        g_vLastRc = orc;
        if (orc == 0) {
            g_vLastBytes = 0;
            g_vLastMs = 0;
            g_open = 1;
            g_segPos = 0;
            return 0;
        }
        // Surface WHY the ranged reader refused this origin — previously this
        // failed silently and surfaced only as a generic FFmpeg "format" error.
        snprintf(g_segFail, sizeof(g_segFail), "%s", httpsrc_debug());
        trace_mark("hls vseg open fail idx=%d rc=%d [%s]", g_segIdx, orc, g_segFail);
    }
    // Fallback: fetch the whole segment through aseg, whose native SceHttp
    // fallback already proved it can reach origins that stall BearSSL. Served
    // from memory exactly like a live segment. Once one segment succeeds this
    // way, skip the (dead) httpsrc attempt for the rest of the stream.
    if (open_mem_segment(u, g_segIdx) == 0) {
        g_forceAsegSeg = 1;
        return 0;
    }
    snprintf(g_segFail, sizeof(g_segFail), "aseg fallback also failed");
    g_vFailCount++;
    return -1;
}

int hls_read(uint8_t *buf, uint32_t len) {
    if (!g_active) return -1;
    const unsigned myGen = g_openGen;
    uint64_t stall0 = 0;                 // set on the first retry, cleared on progress
    for (;;) {
        // Re-check EVERY pass, not just on entry. This retry loop is driven by
        // FFmpeg on the decode thread; with only the entry check, a thread already
        // in here when the stream closed retried forever at 300ms -- the zombie
        // that kept fetching the OLD channel and overwriting the HLS globals of
        // every channel tuned afterwards (a post-burst /chan looked ignored).
        if (!g_active || g_openGen != myGen) return -1;
        int es = ensure_segment();
        if (es == -2) {
            uint64_t now = sceKernelGetProcessTime();
            if (!stall0) stall0 = now;
            else if (now - stall0 > HLS_READ_STALL_US) {
                trace_mark("hls read give up after %llums idx=%d/%d",
                           (unsigned long long)((now - stall0) / 1000), g_segIdx, g_segCount);
                return 0;                // EOF -> the open fails cleanly and /chan works again
            }
            sceKernelUsleep(300 * 1000);
            continue;
        }
        stall0 = 0;                      // made progress
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
                g_upshiftReq = 0; g_fastFetchStreak = 0;
                int lower = hlspl_pick_best(&g_pl, g_variants[g_curVariant].bw);
                if (lower >= 0 && lower != g_curVariant) {
                    if (g_initSeg) { g_preferVariant = lower; g_variantSwitchPending = 1; }
                    else {
                        prefetch_stop();
                        aseg_resume();
                        load_variant(lower);
                        prefetch_start();
                    }
                }
            } else if (g_upshiftReq && g_curVariant >= 0) {
                g_upshiftReq = 0;
                int higher = next_higher_variant(g_curVariant);
                if (higher >= 0) {
                    if (g_initSeg) { g_preferVariant = higher; g_variantSwitchPending = 1; }
                    else {
                        prefetch_stop();
                        aseg_resume();
                        load_variant(higher);
                        prefetch_start();
                    }
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
    const unsigned myGen = g_openGen;
    uint64_t stall0 = 0;

    for (;;) {
        // Bail immediately on teardown. Without this, this retry loop kept
        // re-fetching during a channel switch (the abort flag is per-fetch and got
        // re-cleared), so player_stop blocked ~30s waiting for it to give up.
        if (g_segStopFlag && *g_segStopFlag) return -1;
        // g_active / generation were checked only ON ENTRY, so a thread already
        // inside this loop when the stream closed never noticed and kept fetching
        // and refreshing FOREVER -- against the OLD channel, while mutating the
        // shared HLS globals underneath every channel tuned afterwards. That
        // orphan is why a post-burst /chan appeared to do nothing at all.
        if (!g_active || g_openGen != myGen) return -1;
        if (g_segIdx >= g_segCount) {
            if (!g_isLive) return -1;            // VOD: all segments played -> EOF
            {   uint64_t now = sceKernelGetProcessTime();
                if (!stall0) stall0 = now;
                else if (now - stall0 > HLS_READ_STALL_US) {
                    trace_mark("hls seg give up after %llums idx=%d/%d",
                               (unsigned long long)((now - stall0) / 1000), g_segIdx, g_segCount);
                    return -1;
                } }
            if (refresh_live_playlist() != 0) {
                if (!g_active || g_openGen != myGen) return -1;
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
            // A segment flagged EXT-X-DISCONTINUITY needs fresh decoder/clock
            // state; the reset generation is what the player re-anchors on.
            if (g_segDisc[seg]) {
                g_resetGen++;
                if (outResetGen) *outResetGen = g_resetGen;
                trace_mark("hls discontinuity at seg=%d -> reset=%d", seg, g_resetGen);
            }
            trace_mark("hls segdemux take next_idx=%d/%d gen=%d reset=%d", g_segIdx, g_segCount, g_segGen, g_resetGen);
            stall0 = 0;                 // progress
            return 0;
        }

        if (g_memBuf) {
            free(g_memBuf);
            g_memBuf = NULL;
            g_memLen = g_memPos = 0;
            g_memSeg = -1;
        }
        if (g_liveFetchFailStreak >= 2) refresh_live_playlist();
        if (RETRY_TRACE)
            trace_mark("hls segdemux retry idx=%d/%d streak=%d", g_segIdx, g_segCount, g_liveFetchFailStreak);
        // Give up on a channel whose SEGMENTS keep failing. The deadline used to
        // sit only in the "ran out of segments" branch above, but a dead channel
        // has plenty of segments -- they just 400 -- so it fell through to here and
        // retried forever, parking the main thread inside avformat_find_stream_info
        // and freezing every later channel change (measured: still stuck at 60s).
        {   uint64_t now = sceKernelGetProcessTime();
            if (!stall0) stall0 = now;
            else if (now - stall0 > HLS_READ_STALL_US) {
                trace_mark("hls segdemux give up after %llums idx=%d/%d streak=%d",
                           (unsigned long long)((now - stall0) / 1000),
                           g_segIdx, g_segCount, g_liveFetchFailStreak);
                return -1;
            } }
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
