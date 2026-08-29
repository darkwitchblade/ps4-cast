// player_ff.c — ffmpeg-based player backend.
//
// ffmpeg is built with networking disabled, so input is fed through a custom
// AVIO backed by our sceNet reader (httpsrc), which is also where TLS/https
// lives. H.264 can be routed through the proven sceVideodec2 hardware path;
// other codecs fall back to ffmpeg software decode.
#include "player.h"
#include "httpsrc.h"
#include "hls.h"
#include "urlopt.h"
#include "resolve.h"
#include "aseg.h"
#include "audio.h"
#include "notify.h"
#include "vdec_hw.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <emmintrin.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include <orbis/libkernel.h>

extern uint64_t sceKernelGetProcessTime(void); // microseconds, monotonic

// ---- state ----------------------------------------------------------------
#define AVIO_BUFSZ (2 * 1024 * 1024)   // big read buffer: fewer callbacks, smoother net

static AVFormatContext  *g_fmt;
static AVIOContext      *g_avio;
static AVCodecContext   *g_vdec;
static struct SwsContext*g_sws;
static AVFrame          *g_frame;
static AVPacket         *g_pkt;
static int               g_vstream = -1;

// Hardware H.264 decode (libSceVideodec2). When g_useHw, video packets are
// converted to Annex B by g_bsf and decoded by vdec_hw into NV12 frames that
// flow through the SAME frame queue / sync / scale path as software frames.
static int               g_useHw = 0;
static int               g_interlaced = 0;   // source is interlaced -> bob-deinterlace
// Which stage of player_play is executing, for the watchdog's crash log. Four
// speculative fixes failed to stop a "HANG watchdog stale=36s" while zapping past
// dead channels because the blocking call was never identified — this makes the
// crash name it instead of us inferring it from the symptom.
static const char *volatile g_playStage = "idle";
void player_stage(const char **out) { if (out) *out = (const char *)g_playStage; }
static char              g_swDiag[80] = "";   // last channel-switch stage timing (stop/open/probe ms)
// Manual A/V sync trim, milliseconds. POSITIVE delays VIDEO (use when video runs
// ahead of the sound you hear); negative advances it. Corrects the fixed offset
// our clock cannot see: the audio device's own buffering plus the TV's video
// processing/soundbar delay downstream of "handed to the device". Every serious
// player exposes this because it is not measurable from inside the app.
static volatile int      g_avSyncMs = 0;
static char              g_stopDiag[64] = ""; // last player_stop breakdown (decode-join / fetch-join / audio-join ms)
static AVBSFContext     *g_bsf = NULL;
static AVPacket         *g_hwPkt = NULL;
// Reorder buffer: the low-delay hardware decoder emits frames in DECODE order,
// so B-frames arrive after their display slot. We hold up to `video_delay`
// frames and release them in PTS (display) order. video_delay==0 (no B-frames)
// adds zero latency.
#define HW_REORDER 20
#define H264_REORDER_FLOOR 4
static AVFrame          *g_ro[HW_REORDER];
static int               g_roN = 0;
static int               g_hwReorder = 0;
static int64_t           g_lastEmitPts = AV_NOPTS_VALUE;  // adaptive reorder: grow window if a frame arrives late
static int               g_hwEnabled = 1;                 // runtime HW on/off (toggle via httpd for A/B testing)
static int               g_hwFailCount = 0;
static const AVCodec    *g_swCodec = NULL;
static int               g_hwMaxW = 0, g_hwMaxH = 0, g_hwProfile = 0, g_hwLevel = 0;

// audio
static int               g_astream = -1;
static AVCodecContext   *g_adec;
static struct SwrContext*g_swr;
static AVFrame          *g_aframe;
static int16_t          *g_abuf;      // resampled S16 stereo scratch
static int               g_abufCap;   // in stereo frames
static int               g_haveAudio = 0;
// Stream layout is constant within an HLS rendition, but av_find_best_stream
// can transiently fail on a fresh per-segment TS. Cache the last-known-good
// indices so audio/video never drop out between segments.
static int               g_segVideoIdx = -1, g_segAudioIdx = -1;

// Separate-audio path (HLS rendition where audio is its own playlist). A second
// demuxer reads the audio segment stream via hls_audio_read on its own AVIO, and
// a dedicated thread decodes it -> audio_write. Video stays on g_fmt; both sync
// to the shared audio clock. g_adec/g_swr/g_aframe/g_abuf are reused (the muxed
// and separate paths are mutually exclusive).
static AVFormatContext  *g_afmt;
static AVIOContext      *g_aavio;
static AVPacket         *g_apkt;
static int               g_aastream = -1;
static OrbisPthread      g_audioThread;
static int               g_audioThreadUp = 0;
static volatile int      g_audioStop = 0;
static volatile int      g_sepAudioEof = 0;
static int               g_sepAudioMode = 0;

static uint8_t *g_scaled;          // BGRA scaled output, display-fit (software path)
static int      g_scaledW, g_scaledH;
static int      g_srcW, g_srcH;
// Source geometry/format the cached g_sws was built for. If a frame arrives with
// different source dims/format (e.g. an HLS discontinuity changes resolution),
// the old sws context would read fr->data with wrong strides -> over-read/crash.
static int      g_swsSrcW, g_swsSrcH;
static int      g_swsSrcFmt = -1;  // AV_PIX_FMT_NONE
// HW path presents NV12 straight to the framebuffer (no g_scaled), so we keep a
// ref to the last shown frame and re-present it on holds/pause/EOF — otherwise a
// held frame flips to a stale back-buffer (judder). NULL in software mode.
static AVFrame *g_lastShown = NULL;
// Bumped whenever a newly decoded frame becomes the frame on screen.
static unsigned g_shownGen = 0;
unsigned player_present_generation(void) { return g_shownGen; }

// Set by the main loop (player_request_bar_clear) when an overlay is drawing over
// the letterbox bars, so the next few frames re-clear the bars and the overlay
// doesn't ghost once dismissed. >0 = clear the bars this frame. The countdown
// spans all rotating framebuffers.
static volatile int g_barClearLeft = 0;
void player_request_bar_clear(void) { g_barClearLeft = GFX_BUFFER_COUNT + 1; }

static int   g_started = 0;        // intent: from play() until stop/EOF
static int   g_active  = 0;        // currently decoding
static int   g_gotFrame = 0;
static char  g_status[160] = "idle";
static char  g_playUrl[2048];
static char  g_errorCode[32];
static char  g_errorMessage[192];
static char  g_sourceErrorDiag[480];

static int64_t  g_pos = 0;         // byte cursor for AVIO
static int      g_isLocal = 0;     // web-uploaded file in the app's /data area
static int      g_localFd = -1;
static uint64_t g_localSize = 0;
static uint64_t g_startProc = 0;   // wall clock at first presented frame (us)
static int64_t  g_startPts  = 0;   // pts of first frame (us)

// transport control (set from http thread, applied in player_render)
static volatile int    g_paused = 0;
static int             g_wasPaused = 0;
static uint64_t        g_pauseAt = 0;
static volatile int    g_seekPending = 0;
static volatile double g_seekTo = 0;
static volatile uint64_t g_seekRequestedAt = 0;
static double          g_curSec = 0;   // current playback position
static double          g_durSec = 0;   // total duration

// debug counters
static long g_pkts = 0, g_frames = 0;
static long g_drops = 0, g_queueDrops = 0, g_lateDrops = 0, g_reorderDrops = 0;
static long g_audioPkts = 0, g_videoPkts = 0;
static char g_codecLabel[24] = "";
static int  g_lastErr = 0;
static int64_t g_lastLagUs = 0;
static uint64_t g_presentUsTotal = 0, g_presentUsMax = 0, g_presentCalls = 0;
static uint64_t g_decodeUsTotal = 0, g_decodeUsMax = 0, g_decodeCalls = 0;
static uint64_t g_fqWaitUsTotal = 0, g_fqWaitUsMax = 0, g_fqWaitCalls = 0;
static int  g_isHls = 0;
static int  g_hlsSegGen = 0;
static int  g_hlsResetGen = 0;
static int  g_hlsSegDemux = 0;

// ---- decode/render decouple (frame queue) ---------------------------------
// 1 = a decode thread decodes+scales video into a ready-frame queue and the
// main thread only presents the due frame (synced to the audio clock), so a
// single heavy frame/GOP no longer hitches the screen. Set to 0 to fall back to
// the single-thread decode-and-present path (the known-good 02.19 behaviour).
#define PLAYER_DECODE_THREAD 1
#define FQ_SLOTS 24
#define HLS_START_FRAMES 5     // show the first frame after fewer buffered frames -> faster channel start
#define PREVIDEO_SLOTS 512
#define PREVIDEO_BYTES (32 * 1024 * 1024)

typedef struct { AVFrame *frame; } FrameSlot;   // ref-counted clone, presented then freed
static FrameSlot         g_fq[FQ_SLOTS];
static int               g_fqHead, g_fqCount;
static OrbisPthreadMutex g_fqMtx;
static OrbisPthreadCond  g_fqNotFull, g_fqNotEmpty;
static OrbisPthread      g_decThread;
static int               g_threaded = 0;      // decode-thread path active
static volatile int      g_decStop = 0;
static volatile int      g_decEof = 0;
static volatile int      g_liveRestartPending = 0;
// Seek target carried across the reopen an fMP4 scrub needs: hls_open() rewinds
// to segment 0, so it must be re-applied after it and BEFORE the demuxer opens.
static volatile double   g_hlsResumeSec = -1.0;
static volatile int      g_rebuffering = 0;    // cache-pause: holding for buffer
static volatile int      g_nextStartupHeadstart = 0;
static int               g_startupHeadstart = 0;
static uint64_t          g_startupGateAt = 0;
static int               g_emptyCnt = 0;       // debounce queue-empty
static int               g_rebufHits = 0;      // consecutive rebuffers (for HLS ABR)
static int               g_rebufTotal = 0;     // total rebuffers this playback (telemetry)
static double            g_bytesPerSec = 0;    // bitrate estimate for time-based buffering
static double            g_resumeSec = 2.0;    // buffered-seconds needed to resume (LAN vs remote)
// Fragmented MP4 commonly stores a run of video samples before its audio
// samples. Keep those samples compressed during startup so demux can reach and
// decode the audio cushion without filling the much larger decoded-frame queue.
static AVPacket          *g_preVideo[PREVIDEO_SLOTS];
static int                g_preVideoCount;
static int64_t            g_preVideoBytes;
static int                g_preVideoAudioAfter;

static void  fq_flush(void);
static void  prevideo_clear(void);
static void  ro_clear(void);
static void  apply_hls_reset(void);
static void  seg_readahead_start(void);
static void  seg_readahead_stop(void);
static int   open_sw_video(const AVCodec *dec);

// ---- live HLS segment read-ahead -----------------------------------------
// A dedicated fetch thread pulls TS segments ahead of decode into a small ring,
// so the decode thread never blocks on the ~1.4s per-segment network fetch.
// Raw segments are ~1MB each (far cheaper to buffer than decoded frames). The
// ring depth is the "headstart" that rides over fetch-latency dips. If the
// thread/ring fails to start, decode falls back to fetching inline.
// Bounded by BOTH a slot count AND a byte budget: small segments use all 3 slots
// (~9s cushion); large-segment streams cap at the byte budget so RAM can't blow
// up (always keep >=1 buffered so a single huge segment can't deadlock).
#define SEG_RING 3
#define SEG_BUDGET_BYTES (24 * 1024 * 1024)
typedef struct { uint8_t *buf; int len; int gen; } SegSlot;
static SegSlot           g_segRing[SEG_RING];
static int               g_srHead, g_srCount;
static long              g_srBytes;       // total bytes buffered in the ring
static OrbisPthreadMutex g_srMtx;
static OrbisPthreadCond  g_srNotFull, g_srNotEmpty;
static OrbisPthread      g_segFetchThread;
static volatile int      g_segFetchStop;
static int               g_segFetchUp;       // fetch thread joined-state tracking
static int               g_segReadAhead;     // 1 = decode pops ring; 0 = inline fetch
static void *decode_segment_thread_main(void *arg);
static void seg_readahead_stop(void);
static void  present_pool_start(void);
static void  present_pool_stop(void);
static void *decode_thread_main(void *arg);
static int   setup_separate_audio(void);
static void *audio_thread_main(void *arg);

// ---- custom AVIO: uploaded file, plain HTTP(S), or HLS ---------------------
extern void watchdog_kick(void);       // main.c: pet the freeze watchdog from the main-thread probe
extern void watchdog_set_busy(int on);
extern const char *watchdog_note(const char *w); // main.c: longer watchdog grace during a slow channel switch

static int avio_read_cb(void *o, uint8_t *buf, int size) {
    (void)o;
    // During player_play's synchronous demux probe (runs on the main thread,
    // g_started==0), a slow channel switch can read for many seconds. Pet the
    // freeze watchdog on each read so a progressing switch isn't killed as a
    // freeze. Not kicked during playback (g_started==1) so a real main-loop
    // freeze is still caught.
    if (!g_started) watchdog_kick();
    int n;
    if (g_isLocal) {
        // libkernel declares pread as size_t even though failures use negative
        // errno-style returns. Cast through ssize_t before checking the result.
        ssize_t got = (ssize_t)sceKernelPread(g_localFd, buf, (size_t)size, (off_t)g_pos);
        n = (got > 0 && got <= INT32_MAX) ? (int)got : 0;
        if (n > 0) g_pos += n;
    } else if (g_isHls) {
        n = hls_read(buf, (uint32_t)size);   // hls tracks its own position
    } else {
        n = httpsrc_read(buf, (uint64_t)g_pos, (uint32_t)size);
        if (n > 0) g_pos += n;
    }
    if (n <= 0) return AVERROR_EOF;
    return n;
}

typedef struct { const uint8_t *buf; int len; int pos; } MemAvio;
static int avio_mem_read_cb(void *o, uint8_t *buf, int size) {
    MemAvio *m = (MemAvio *)o;
    int left = m->len - m->pos;
    if (left <= 0) return AVERROR_EOF;
    int take = size < left ? size : left;
    memcpy(buf, m->buf + m->pos, take);
    m->pos += take;
    return take;
}
// Second AVIO: the separate HLS audio rendition (its own segment byte stream).
static int avio_aread_cb(void *o, uint8_t *buf, int size) {
    (void)o;
    int n = hls_audio_read(buf, (uint32_t)size);
    if (n <= 0) return AVERROR_EOF;
    return n;
}
static int64_t avio_seek_cb(void *o, int64_t off, int whence) {
    (void)o;
    if (g_isHls) return -1;   // HLS stream is not seekable here
    uint64_t sz = g_isLocal ? g_localSize : httpsrc_size();
    if (whence == AVSEEK_SIZE) return sz ? (int64_t)sz : -1;
    int64_t next;
    if (whence == SEEK_SET)      next = off;
    else if (whence == SEEK_CUR) next = g_pos + off;
    else if (whence == SEEK_END) next = (int64_t)sz + off;
    else return -1;
    if (next < 0) next = 0;
    if (sz && (uint64_t)next > sz) next = (int64_t)sz;
    g_pos = next;
    return g_pos;
}

const char *player_status(void) { return g_status; }
const char *player_error_code(void) { return g_errorCode; }
const char *player_error_message(void) { return g_errorMessage; }
void player_clear_error(void) {
    g_errorCode[0] = '\0'; g_errorMessage[0] = '\0'; g_sourceErrorDiag[0] = '\0';
}
static void player_set_error(const char *code, const char *message) {
    snprintf(g_errorCode, sizeof(g_errorCode), "%s", code ? code : "playback");
    snprintf(g_errorMessage, sizeof(g_errorMessage), "%s", message ? message : "Playback failed.");
    snprintf(g_status, sizeof(g_status), "%s", g_errorMessage);
}
int player_init(void) { return 0; } // nothing global to set up for ffmpeg

void player_stop(void) {
    uint64_t st0 = sceKernelGetProcessTime(), stDec = st0, stFetch = st0;
#if PLAYER_DECODE_THREAD
    if (g_threaded) {
        g_decStop = 1;
        g_segFetchStop = 1;
        httpsrc_abort();   // unblock the decode thread if stuck in a network read
        if (g_segReadAhead) {           // unblock decode pop / fetch push on the ring
            aseg_abort();
            scePthreadMutexLock(&g_srMtx);
            scePthreadCondSignal(&g_srNotFull);
            scePthreadCondSignal(&g_srNotEmpty);
            scePthreadMutexUnlock(&g_srMtx);
        }
        scePthreadMutexLock(&g_fqMtx);
        scePthreadCondSignal(&g_fqNotFull);
        scePthreadCondSignal(&g_fqNotEmpty);
        scePthreadMutexUnlock(&g_fqMtx);
        scePthreadJoin(g_decThread, NULL);
        stDec = sceKernelGetProcessTime();
        seg_readahead_stop();           // join fetch thread, free buffered segs, destroy ring
        stFetch = sceKernelGetProcessTime();
        snprintf(g_stopDiag, sizeof(g_stopDiag), "stop dec=%llu fetch=%llu ms",
                 (unsigned long long)((stDec - st0) / 1000),
                 (unsigned long long)((stFetch - stDec) / 1000));
        fq_flush();
        prevideo_clear();
        scePthreadCondDestroy(&g_fqNotFull);
        scePthreadCondDestroy(&g_fqNotEmpty);
        scePthreadMutexDestroy(&g_fqMtx);
        g_threaded = 0;
    }
    g_decStop = 0; g_decEof = 0; g_segFetchStop = 0;
#endif
    // Stop the separate-audio thread before tearing down its demuxer/decoder.
    if (g_audioThreadUp) {
        g_audioStop = 1;
        hls_audio_abort();        // unblock a blocked audio-segment fetch
        scePthreadJoin(g_audioThread, NULL);
        g_audioThreadUp = 0;
    }
    g_audioStop = 0; g_sepAudioEof = 0;
    audio_pause(0);
    audio_close();
    if (g_swr)   { swr_free(&g_swr); }
    if (g_adec)  { avcodec_free_context(&g_adec); }
    if (g_aframe){ av_frame_free(&g_aframe); }
    if (g_apkt)  { av_packet_free(&g_apkt); }
    if (g_abuf)  { free(g_abuf); g_abuf = NULL; g_abufCap = 0; }
    if (g_afmt)  { avformat_close_input(&g_afmt); }
    if (g_aavio) { av_freep(&g_aavio->buffer); avio_context_free(&g_aavio); }
    g_astream = -1; g_aastream = -1; g_haveAudio = 0; g_sepAudioMode = 0; g_sepAudioEof = 0;
    if (g_sws)   { sws_freeContext(g_sws); g_sws = NULL; }
    if (g_frame) { av_frame_free(&g_frame); }
    if (g_pkt)   { av_packet_free(&g_pkt); }
    if (g_vdec)  { avcodec_free_context(&g_vdec); }
    ro_clear();
    if (g_useHw) vdec_hw_close();
    if (g_bsf)   { av_bsf_free(&g_bsf); g_bsf = NULL; }
    if (g_hwPkt) { av_packet_free(&g_hwPkt); }
    g_useHw = 0; g_hwReorder = 0;
    if (g_fmt)   { avformat_close_input(&g_fmt); }
    if (g_avio)  { av_freep(&g_avio->buffer); avio_context_free(&g_avio); }
    present_pool_stop();                 // join present workers before freeing buffers
    if (g_lastShown) av_frame_free(&g_lastShown);
    if (g_scaled){ free(g_scaled); g_scaled = NULL; }
    if (g_isLocal) {
        if (g_localFd >= 0) sceKernelClose(g_localFd);
    } else if (g_isHls) {
        hls_close();
    } else {
        httpsrc_close();
    }
    g_localFd = -1; g_localSize = 0; g_isLocal = 0;
    g_isHls = 0;
    g_hlsSegDemux = 0;
    g_rebuffering = 0; g_startupHeadstart = 0; g_startupGateAt = 0;
    g_vstream = -1; g_active = 0; g_started = 0; g_gotFrame = 0; g_interlaced = 0;
    g_pos = 0; g_startProc = 0; g_scaledW = g_scaledH = 0;
    g_paused = 0; g_wasPaused = 0; g_seekPending = 0; g_seekRequestedAt = 0;
    g_curSec = 0; g_durSec = 0;
    snprintf(g_status, sizeof(g_status), "stopped");
}

// Toggle the hardware-decode fast path at runtime (for A/B testing vs software).
void player_set_hw(int on) { g_hwEnabled = on ? 1 : 0; }
int  player_hw_enabled(void) { return g_hwEnabled; }

// Open the multi-threaded software H.264/etc decoder into g_vdec. Returns 0/-1.
// Used for the normal software path and as the hardware-failure fallback.
static int open_sw_video(const AVCodec *dec) {
    g_vdec = avcodec_alloc_context3(dec);
    if (!g_vdec) return -1;
    avcodec_parameters_to_context(g_vdec, g_fmt->streams[g_vstream]->codecpar);
    // Multi-core software decode — the PS4 has ~6 usable Jaguar cores. Frame +
    // slice threading is the biggest win for smooth HD playback. Disabling the
    // in-loop deblocking filter is the single biggest software-decode speedup;
    // FLAG2_FAST allows non-compliant shortcuts (slight blockiness for fps).
    g_vdec->thread_count = 6;
    g_vdec->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
    g_vdec->skip_loop_filter = AVDISCARD_ALL;
    g_vdec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(g_vdec, dec, NULL) < 0) return -1;
    return 0;
}

int player_play(const char *url) {
    // A cast request can ask for a small startup cushion. Capture it before
    // player_stop() resets the active playback state.
    int requestedHeadstart = g_nextStartupHeadstart;
    g_nextStartupHeadstart = 0;
    g_playStage = "enter";
    watchdog_set_busy(1);   // teardown + open + probe can run many seconds (esp. off a 1080i SW channel); don't let the freeze watchdog kill the switch
    uint64_t swt0 = sceKernelGetProcessTime();   // channel-switch stage timing (-> g_swDiag, shown in /status)
    char requested[2048];
    snprintf(requested, sizeof(requested), "%s", url ? url : "");
    char startUrl[2048];
    urlopt_apply(requested, startUrl, sizeof(startUrl));   // "url|Referer=..&User-Agent=.." -> clean url + headers
    int forceHls = strcmp(urlopt_kind(), "hls") == 0;
    // A page URL (what you get from a site whose player shows a blob:) is not
    // playable. Fetch it and dig the manifest out, then adopt the Referer the CDN
    // will demand. Sites that build the URL in JS still need the DevTools route.
    if (!forceHls && resolve_is_page(startUrl)) {
        g_playStage = "resolve";
        char resolved[2048];
        if (resolve_page(startUrl, resolved, sizeof(resolved)))
            urlopt_apply(resolved, startUrl, sizeof(startUrl));
        else
            urlopt_apply(requested, startUrl, sizeof(startUrl));
    }
    g_playStage = "stop";
    player_stop();
    player_clear_error();
    hls_set_seg_stop_flag(&g_segFetchStop);   // so hls_next_segment's retry loop bails instantly on the next teardown (no more ~30s player_stop)
    uint64_t swtStop = sceKernelGetProcessTime();
    strncpy(g_playUrl, startUrl, sizeof(g_playUrl) - 1);
    g_playUrl[sizeof(g_playUrl) - 1] = '\0';
    g_pkts = g_frames = g_drops = g_queueDrops = g_lateDrops = g_reorderDrops = 0;
    g_presentUsTotal = g_presentUsMax = g_presentCalls = 0;
    g_decodeUsTotal = g_decodeUsMax = g_decodeCalls = 0;
    g_fqWaitUsTotal = g_fqWaitUsMax = g_fqWaitCalls = 0;
    gfx_present_stats_reset();
    g_audioPkts = g_videoPkts = 0; g_lastErr = 0; g_lastLagUs = 0;
    g_hwFailCount = 0;
    // The lobby or previous source may have filled every scanout buffer since
    // the last video. Geometry caches intentionally survive between frames, so
    // force the new source to repaint its bars across the whole buffer rotation
    // even when it happens to have the same dimensions as the previous source.
    g_barClearLeft = GFX_BUFFER_COUNT + 1;

    // Open the source. The web-upload URL is deliberately fixed (never a user
    // supplied path), HLS uses the segment layer, and remote files use httpsrc.
    g_isLocal = strcmp(startUrl, PLAYER_LOCAL_UPLOAD_URL) == 0;
    g_isHls = !g_isLocal && (forceHls || hls_is_url(startUrl));
    g_playStage = "source-open";
    watchdog_note(g_isLocal ? "local_open" : (g_isHls ? "hls_open" : "httpsrc_open"));
    int orc;
    if (g_isLocal) {
        g_localFd = sceKernelOpen(PLAYER_LOCAL_UPLOAD_PATH, 0 /*O_RDONLY*/, 0);
        off_t end = g_localFd >= 0 ? sceKernelLseek(g_localFd, 0, SEEK_END) : -1;
        if (g_localFd >= 0 && end > 0 && sceKernelLseek(g_localFd, 0, SEEK_SET) >= 0) {
            g_localSize = (uint64_t)end;
            orc = 0;
        } else {
            if (g_localFd >= 0) sceKernelClose(g_localFd);
            g_localFd = -1; g_localSize = 0;
            orc = -1;
        }
    } else {
        if (g_isHls) hls_set_decode_cap(g_hwEnabled ? 1080 : 720);
        orc = g_isHls ? hls_open(startUrl) : httpsrc_open(startUrl);
    }
    watchdog_note("-");
    uint64_t swtOpen = sceKernelGetProcessTime();
    if (orc != 0) {
        char detail[160];
        snprintf(g_sourceErrorDiag, sizeof(g_sourceErrorDiag), "%s",
                 g_isLocal ? "local open failed" : (g_isHls ? hls_debug() : httpsrc_debug()));
        snprintf(detail, sizeof(detail), "%s",
                 g_isLocal ? "The uploaded file is no longer available. Upload it again."
                           : (g_isHls ? "The HLS source could not be opened. Check the link or server."
                                      : "The video source could not be reached. Check the link and try again."));
        player_stop();
        player_set_error("source", detail);
        return -1;
    }
    g_pos = 0;
    // Re-apply a target carried across an fMP4 scrub's reopen. Must run BEFORE
    // the demuxer opens so it reads init + the target segment, not the file start.
    if (g_isHls && g_hlsResumeSec >= 0) {
        double actual = 0;
        int src = hls_seek_clamped(g_hlsResumeSec, &actual);
        trace_mark("seek hls resume target=%.3f rc=%d actual=%.3f", g_hlsResumeSec, src, actual);
        g_hlsResumeSec = -1.0;
    }

    uint8_t *aviobuf = av_malloc(AVIO_BUFSZ);
    if (!aviobuf) {
        player_stop();
        player_set_error("memory", "Not enough memory to open this video. Stop playback and try again.");
        return -1;
    }
    g_avio = avio_alloc_context(aviobuf, AVIO_BUFSZ, 0, NULL, avio_read_cb, NULL, avio_seek_cb);
    if (!g_avio) {
        av_free(aviobuf);
        player_stop();
        player_set_error("memory", "Not enough memory to prepare this video. Stop playback and try again.");
        return -1;
    }
    if (g_isHls) g_avio->seekable = 0;  // concatenated segment stream

    g_fmt = avformat_alloc_context();
    g_fmt->pb = g_avio;
    g_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Probe further so the audio stream in a concatenated MPEG-TS (HLS) is
    // reliably detected — the default probe can stop before the audio PID and
    // leave HLS playing silently.
    // Probe size is a trade between channel-switch speed and finding the audio
    // PID. 8MB/6s (original) made switches take 15-25s on slow IPTV servers, but
    // 1MB/1.5s was too small and reintroduced exactly the silent-HLS bug the note
    // above warns about: a 6Mbps 1080p stream fits only ~1.3s in 1MB, so the audio
    // PID fell outside the window and playback ran with NO AUDIO (as=-1, audio
    // device never opened). 4MB/4s finds the audio PID on these streams while
    // keeping switches quick. Probe reads pet the watchdog, so a longer probe
    // cannot trip the freeze detector.
    // 4MB/4s. Measured: shrinking this to 2MB/2.5s did NOT speed up launch
    // (probe 2165ms -> 2315ms) because the probe is bound by DOWNLOADING ~2s of
    // stream, not by these ceilings — so the smaller value bought nothing and only
    // raised the risk of missing the audio PID (1MB did exactly that: silent HLS).
    // Launch latency has to be attacked by prefetching data, not by trimming here.
    g_fmt->probesize = 4 * 1024 * 1024;
    g_fmt->max_analyze_duration = 4 * (int64_t)AV_TIME_BASE;

    g_playStage = "demux-open";
    int rc = avformat_open_input(&g_fmt, "stream", NULL, NULL);
    if (rc < 0) {
        player_stop();
        // For HLS the demuxer starves only when segment delivery fails; carry
        // the reader's own failure detail into /status instead of failing blind.
        snprintf(g_sourceErrorDiag, sizeof(g_sourceErrorDiag), "%s",
                 g_isHls ? hls_debug() : "");
        player_set_error("format", "This stream format could not be opened. Try software decode or another source.");
        return -2;
    }
    g_playStage = "probe";
    if (avformat_find_stream_info(g_fmt, NULL) < 0) {
        player_stop();
        player_set_error("stream-info", "The source opened, but its audio and video tracks could not be read.");
        return -3;
    }
    {
        uint64_t swtInfo = sceKernelGetProcessTime();
        snprintf(g_swDiag, sizeof(g_swDiag), "sw stop=%llu open=%llu info=%llu ms",
                 (unsigned long long)((swtStop - swt0) / 1000),
                 (unsigned long long)((swtOpen - swtStop) / 1000),
                 (unsigned long long)((swtInfo - swtOpen) / 1000));
    }

    const AVCodec *dec = NULL;
    g_vstream = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (g_vstream < 0 || !dec) {
        player_stop();
        player_set_error("no-video", "No playable video track was found in this source.");
        return -4;
    }
    g_swCodec = dec;

    // Pick the audio stream (for sound). Discard any OTHER streams (data/subs)
    // so the demuxer doesn't seek across the file for packets we never use.
    const AVCodec *adec = NULL;
    g_astream = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &adec, 0);
    if (g_astream < 0) g_astream = -1;
    for (unsigned i = 0; i < g_fmt->nb_streams; i++)
        if ((int)i != g_vstream && (int)i != g_astream)
            g_fmt->streams[i]->discard = AVDISCARD_ALL;

    // Set up audio decode -> resample to S16 stereo 48kHz -> sceAudioOut.
    // If this is an HLS stream whose audio is a separate rendition, ignore any
    // (usually absent) muxed audio and drive the dedicated audio path instead.
    int useSep = (g_isHls && hls_has_audio());
    g_haveAudio = 0;
    g_segVideoIdx = -1; g_segAudioIdx = -1;
    if (useSep && g_astream >= 0) {
        g_fmt->streams[g_astream]->discard = AVDISCARD_ALL;
        g_astream = -1;
    }
    if (g_astream >= 0 && adec && !useSep) {
        g_adec = avcodec_alloc_context3(adec);
        avcodec_parameters_to_context(g_adec, g_fmt->streams[g_astream]->codecpar);
        g_adec->thread_count = 1;
        if (avcodec_open2(g_adec, adec, NULL) == 0) {
            AVChannelLayout out_ch = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&g_swr, &out_ch, AV_SAMPLE_FMT_S16, 48000,
                                    &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate,
                                    0, NULL) == 0 && g_swr && swr_init(g_swr) == 0) {
                g_aframe = av_frame_alloc();
                if (audio_open() == 0) {
                    g_haveAudio = 1;
                    notify_dbg("PS4 Cast: audio %s %dHz", adec->name, g_adec->sample_rate);
                }
            }
        }
        if (!g_haveAudio) {  // audio setup failed -> video only, discard audio too
            if (g_swr) swr_free(&g_swr);
            if (g_adec) avcodec_free_context(&g_adec);
            g_fmt->streams[g_astream]->discard = AVDISCARD_ALL;
            g_astream = -1;
        }
    }

    // Separate HLS audio rendition: bring up the second demuxer. On any failure
    // fall back to video-only (the stream still plays, just silent).
    if (useSep) {
        if (setup_separate_audio() != 0) {
            g_sepAudioMode = 0; g_haveAudio = 0;
            if (g_swr)    swr_free(&g_swr);
            if (g_adec)   avcodec_free_context(&g_adec);
            if (g_aframe) av_frame_free(&g_aframe);
            if (g_apkt)   av_packet_free(&g_apkt);
            if (g_afmt)   avformat_close_input(&g_afmt);
            if (g_aavio)  { av_freep(&g_aavio->buffer); avio_context_free(&g_aavio); }
            g_aastream = -1;
        }
    }

    // Hardware H.264 fast path: decode on the GPU silicon (CPU stays free for
    // networking/scaling). Set up a mp4->annexb bitstream filter and bring up the
    // hardware decoder; any failure cleanly falls back to software below.
    // Hardware H.264 fast path: decode on the GPU silicon (CPU stays free). Set up
    // a mp4->annexb bitstream filter + the hardware decoder; any failure (or the
    // runtime toggle being off) cleanly falls back to software below.
    AVCodecParameters *vpar = g_fmt->streams[g_vstream]->codecpar;
    g_useHw = 0;
    g_hlsSegDemux = g_isHls && hls_can_segment_demux();
    if (g_isHls) hls_set_external_segment_fetch(g_hlsSegDemux);
    // Hardware H.264: direct MP4 and fMP4 HLS need the mp4->annexb bitstream
    // filter; TS HLS uses the segment-demux path and is already Annex B.
    int interlaced = (vpar->field_order == AV_FIELD_TT || vpar->field_order == AV_FIELD_BB ||
                      vpar->field_order == AV_FIELD_TB || vpar->field_order == AV_FIELD_BT);
    g_interlaced = interlaced;   // -> bob-deinterlace in build_scaled (software path)
    // Interlaced H.264 is now hardware-decoded too (sceVideodec2 is not locked to
    // progressive — optimizeProgressiveVideo=0), and bob-deinterlaced when the NV12
    // is presented. This keeps heavy 1080i (IPTV) off the CPU-bound software path,
    // which is what made switching off a 1080i channel slow and watchdog-killable.
    // Hardware path is an experimentally-derived sceVideodec2 config: feed it ONLY
    // 8-bit 4:2:0 H.264 within sane dimensions. Hi10P (110), 4:2:2 (122) and
    // 4:4:4 (244) or oversized streams previously went straight to the GPU decoder
    // and could fault the compositor; they now fall back to software.
    int hwProfileOk = (vpar->profile <= 100);        // baseline/main/high only
    int hwDepthOk   = (vpar->format == AV_PIX_FMT_NONE ||
                       vpar->format == AV_PIX_FMT_YUV420P ||
                       vpar->format == AV_PIX_FMT_YUVJ420P);
    int hwSizeOk    = (vpar->width > 0 && vpar->height > 0 &&
                       vpar->width <= 1920 && vpar->height <= 1088);
    // Level bounds the decoder's DPB/bitrate assumptions; the config is opened for
    // <= 5.1, so anything above that (or an absurd level) goes to software.
    int hwLevelOk   = (vpar->level <= 51);
    int hwEligible = g_hwEnabled && vpar->codec_id == AV_CODEC_ID_H264 &&
                     hwProfileOk && hwDepthOk && hwSizeOk && hwLevelOk &&
                     (!g_isHls || g_hlsSegDemux || hls_is_fmp4());
    if (g_hwEnabled && vpar->codec_id == AV_CODEC_ID_H264 && !hwEligible)
        notify_dbg("PS4 Cast: H.264 prof=%d lvl=%d fmt=%d %dx%d -> software (unsupported by HW)",
                   vpar->profile, vpar->level, vpar->format, vpar->width, vpar->height);
    if (interlaced) notify_dbg("PS4 Cast: interlaced -> %s + bob-deinterlace", hwEligible ? "hardware" : "software");
    if (hwEligible) {
        int bsfOk = 1;
        if (!g_hlsSegDemux) { // MP4/fMP4 AVCC source -> Annex B for the decoder
            const AVBitStreamFilter *bf = av_bsf_get_by_name("h264_mp4toannexb");
            bsfOk = (bf && av_bsf_alloc(bf, &g_bsf) == 0 &&
                     avcodec_parameters_copy(g_bsf->par_in, vpar) >= 0 &&
                     av_bsf_init(g_bsf) == 0);
        }
        if (bsfOk) {
            g_hwPkt = av_packet_alloc();                  // allocate BEFORE enabling HW
            if (g_hwPkt && vdec_hw_open(vpar->width, vpar->height,
                                        vpar->profile > 0 ? vpar->profile : 0,
                                        vpar->level   > 0 ? vpar->level   : 0) == 0) {
                g_useHw = 1;
                g_hwFailCount = 0;
                g_hwMaxW = vpar->width; g_hwMaxH = vpar->height;
                g_hwProfile = vpar->profile > 0 ? vpar->profile : 0;
                g_hwLevel   = vpar->level   > 0 ? vpar->level   : 0;
                g_roN = 0; g_lastEmitPts = AV_NOPTS_VALUE;
                g_hwReorder = vpar->video_delay;          // B-frame reorder depth (adapts at runtime)
                if (g_hwReorder < 0) g_hwReorder = 0;
                if (g_hwReorder >= HW_REORDER) g_hwReorder = HW_REORDER - 1;
                notify_dbg("PS4 Cast: HW H.264 %dx%d reorder=%d %s (%s)", vpar->width, vpar->height,
                       g_hwReorder, g_hlsSegDemux ? "hls-seg" : "direct", vdec_hw_debug());
            }
        }
        if (!g_useHw) {     // HW unavailable -> tear down the half-built HW state
            if (g_hwPkt) { av_packet_free(&g_hwPkt); }
            if (g_bsf)   { av_bsf_free(&g_bsf); g_bsf = NULL; }
        }
    }

    if (!g_useHw) {
        if (open_sw_video(dec) != 0) {
            player_stop();
            player_set_error("decoder", "The video codec is not supported by the available decoders.");
            return -5;
        }
    }

    g_frame = av_frame_alloc();
    g_pkt   = av_packet_alloc();
    g_srcW = g_useHw ? vpar->width  : g_vdec->width;
    g_srcH = g_useHw ? vpar->height : g_vdec->height;
    snprintf(g_codecLabel, sizeof(g_codecLabel), "%s%s", dec->name ? dec->name : "video", g_interlaced ? " deint" : "");
    g_durSec = (g_fmt->duration > 0) ? (double)g_fmt->duration / AV_TIME_BASE : 0;
    if (g_isHls && hls_duration() > 0) g_durSec = hls_duration();
    g_hlsSegGen = g_isHls ? hls_generation() : 0;
    g_hlsResetGen = g_isHls ? hls_reset_generation() : 0;
    g_hlsSegDemux = g_isHls && hls_can_segment_demux();
    if (g_isHls) hls_set_decode_cap(g_useHw ? 1080 : 720);
    uint64_t sourceSize = g_isLocal ? g_localSize : httpsrc_size();
    g_bytesPerSec = (g_fmt->bit_rate > 0) ? (double)g_fmt->bit_rate / 8.0
                  : (g_durSec > 0 && sourceSize > 0) ? (double)sourceSize / g_durSec
                  : 0;
    // Bigger cushion for public/CDN links (4s) than LAN (1.5s) — 2s was too
    // small for remote HTTPS streams.
    g_resumeSec = g_isLocal ? 0.0 : ((!g_isHls && httpsrc_is_lan()) ? 1.5 : 4.0);
    g_startupHeadstart = requestedHeadstart;
    g_startupGateAt = sceKernelGetProcessTime();
    g_rebuffering = (g_isHls || g_startupHeadstart) ? 1 : 0;
    g_emptyCnt = 0; g_rebufHits = 0; g_rebufTotal = 0;

    g_started = 1; g_active = 1; g_gotFrame = 0;
    if (g_rebuffering) audio_pause(1);  // fill video and decoded audio before first presentation
    g_playStage = "started";
    snprintf(g_status, sizeof(g_status), "buffering %s %dx%d", dec->name, g_srcW, g_srcH);
    notify_dbg("PS4 Cast: ffmpeg %s %dx%d", dec->name, g_srcW, g_srcH);

#if PLAYER_DECODE_THREAD
    // Start the decode thread; main thread will only present. On failure fall
    // back to the single-thread decode-and-present path (g_threaded stays 0).
    g_threaded = 0; g_decStop = 0; g_decEof = 0; g_fqHead = g_fqCount = 0;
    prevideo_clear();
    for (int i = 0; i < FQ_SLOTS; i++) g_fq[i].frame = NULL;
    scePthreadMutexInit(&g_fqMtx, NULL, "ps4cast_fq");
    scePthreadCondInit(&g_fqNotFull, NULL, "ps4cast_fqnf");
    scePthreadCondInit(&g_fqNotEmpty, NULL, "ps4cast_fqne");
    // FFmpeg's HLS/TS demux + decode path can also recurse/use stack heavily on
    // Orbis. Use the same roomy stack for every decode thread so buffering
    // cannot turn into a CE crash from a default pthread-stack overflow.
    OrbisPthreadAttr dattr; OrbisPthreadAttr *pdattr = NULL;
    if (scePthreadAttrInit(&dattr) == 0) {
        scePthreadAttrSetstacksize(&dattr, 8 * 1024 * 1024);
        pdattr = &dattr;
    }
    // Bring up segment read-ahead BEFORE the decode thread so exactly one path
    // (the fetch thread) advances the HLS segment index — no double-fetch race.
    // If it fails to start, g_segReadAhead stays 0 and decode fetches inline.
    if (g_hlsSegDemux) seg_readahead_start();
    int dcr = scePthreadCreate(&g_decThread, pdattr,
                               g_hlsSegDemux ? decode_segment_thread_main : decode_thread_main,
                               NULL, "ps4cast_dec");
    if (pdattr) scePthreadAttrDestroy(&dattr);
    if (dcr == 0) {
        g_threaded = 1;
    } else if (g_useHw) {
        // Hardware decode REQUIRES the big-stack thread (no inline HW path). The
        // thread failed -> tear down HW and fall back to software decode.
        vdec_hw_close();
        if (g_bsf)   { av_bsf_free(&g_bsf); g_bsf = NULL; }
        if (g_hwPkt) { av_packet_free(&g_hwPkt); }
        g_useHw = 0;
        if (open_sw_video(dec) == 0 &&
            scePthreadCreate(&g_decThread, NULL,
                             g_hlsSegDemux ? decode_segment_thread_main : decode_thread_main,
                             NULL, "ps4cast_dec") == 0) {
            g_threaded = 1;
        } else {
            scePthreadCondDestroy(&g_fqNotFull);
            scePthreadCondDestroy(&g_fqNotEmpty);
            scePthreadMutexDestroy(&g_fqMtx);
        }
    } else {
        scePthreadCondDestroy(&g_fqNotFull);
        scePthreadCondDestroy(&g_fqNotEmpty);
        scePthreadMutexDestroy(&g_fqMtx);
    }
#endif
    // The startup gate is queue-based. If thread creation failed and playback
    // fell back to the inline renderer, do not leave audio paused indefinitely.
    if (!g_threaded && g_rebuffering) {
        g_rebuffering = 0;
        audio_pause(0);
    }
    // Hardware NV12 frames use the parallel presenter, writing directly to the
    // active framebuffer so we avoid a second full-screen blit.
    if (g_useHw) present_pool_start();

    // Separate-audio rendition: start its decode thread (independent of the video
    // decode-thread toggle — it feeds audio_write directly either way).
    if (g_sepAudioMode) {
        g_audioStop = 0;
        if (scePthreadCreate(&g_audioThread, NULL, audio_thread_main, NULL, "ps4cast_aud") == 0)
            g_audioThreadUp = 1;
    }
    return 0;
}

int player_is_active(void) { return g_active; }
int player_started(void)   { return g_started; }
int player_is_live(void)   { return g_isHls && hls_is_live(); }   // true live (not VOD)
int player_is_local(void)  { return g_isLocal; }

void player_pause(int paused) {
    g_paused = paused ? 1 : 0;
    audio_pause(g_paused || g_rebuffering);
}
int  player_is_paused(void)   { return g_paused; }
int  player_can_seek(void)    {
    return g_started && g_durSec > 0 &&
           (!g_isHls || hls_can_seek() || hls_can_seek_clamped());
}
void player_set_startup_headstart(int on) { g_nextStartupHeadstart = on ? 1 : 0; }
// Seek by a delta from the PENDING target when one is queued, else from the
// current position. Rapid R1/L1 presses used to all read g_curSec, which only
// updates once the decode thread applies the seek — so a second press within
// that window recomputed the SAME target and was silently a no-op.
void player_seek_relative(double delta) {
    double base = g_seekPending ? g_seekTo : g_curSec;
    player_seek(base + delta);
}

void player_seek(double seconds) {
    if (!player_can_seek()) return;
    if (seconds < 0) seconds = 0;
    if (g_durSec > 0 && seconds > g_durSec) seconds = g_durSec;
    g_seekTo = seconds;
    g_seekRequestedAt = sceKernelGetProcessTime();
    g_seekPending = 1;
}

static int seek_debounce_elapsed(void) {
    if (!g_seekPending) return 0;
    return sceKernelGetProcessTime() - g_seekRequestedAt >= 200000ULL;
}
// Unblock a stuck network read (called from the http thread on Stop / new cast)
// so an underrun stall never traps the app.
void player_interrupt(void) {
    if (!g_started && !g_active) return;
    if (!g_isLocal) httpsrc_abort();
    hls_audio_abort();
}

// True when we've started playing but no fresh frame is available — i.e. the
// decoder/network can't keep up. Drives the on-screen "Buffering" indicator.
int player_buffering(void) {
    if (!g_started || g_paused) return 0;
    if (g_threaded) return g_rebuffering;
    return 0;   // single-thread path blocks instead of reporting
}
int player_buffer_pct(void) {
    // On the decode-thread path (HLS seg-demux / live) the real read-ahead is the
    // decoded-frame queue, not the prefetch ring (which is idle there) — so report
    // that, otherwise live channels always showed buffer 0%.
    if (g_threaded) { int p = g_fqCount * 100 / FQ_SLOTS; return p > 100 ? 100 : p; }
    return g_isLocal ? 100 : (g_isHls ? hls_buffer_pct() : httpsrc_fill_pct());
}
// Total bytes pulled from the network on the ACTIVE source (HLS or direct HTTP),
// so the on-screen network-speed stat works on every stream type.
void player_set_avsync(int ms) { if (ms < -2000) ms = -2000; if (ms > 2000) ms = 2000; g_avSyncMs = ms; }
int  player_get_avsync(void) { return g_avSyncMs; }

uint64_t player_rx_total(void) { return g_isLocal ? 0 : (g_isHls ? hls_rx_total() : httpsrc_rx_total()); }

void player_progress(double *cur, double *dur) {
    if (cur) *cur = g_curSec;
    if (dur) *dur = g_durSec;
}

// Drop all queued frames (after a seek, or on stop). Safe to call when the
// queue is unused (inline path) — it just no-ops.
static void fq_flush(void) {
    if (!g_threaded) return;
    scePthreadMutexLock(&g_fqMtx);
    while (g_fqCount > 0) {
        AVFrame *f = g_fq[g_fqHead].frame;
        if (f) av_frame_free(&f);
        g_fq[g_fqHead].frame = NULL;
        g_fqHead = (g_fqHead + 1) % FQ_SLOTS;
        g_fqCount--;
    }
    scePthreadCondSignal(&g_fqNotFull);
    scePthreadMutexUnlock(&g_fqMtx);
}

static void prevideo_clear(void) {
    for (int i = 0; i < g_preVideoCount; i++)
        if (g_preVideo[i]) av_packet_free(&g_preVideo[i]);
    g_preVideoCount = 0;
    g_preVideoBytes = 0;
    g_preVideoAudioAfter = 0;
}

// Apply a pending seek. In threaded mode this runs on the decode thread; in
// inline mode on the render thread. Either way the ffmpeg context is owned by
// the caller.
static void apply_seek(void) {
    prevideo_clear();
    double sec = g_seekTo;
    uint64_t requestAt = g_seekRequestedAt;
    g_seekPending = 0;
    double actual = sec;
    int seekOk = 0;
    if (g_isHls && (hls_is_fmp4() || hls_has_separate_audio()) &&
        hls_can_seek_clamped()) {
        // fMP4 runs on the continuous AVIO/MOV demuxer: moov is already consumed
        // and a concatenated byte stream has no rewind, so seeking in place is
        // impossible. Rebuild at the target instead -- the fresh demuxer then
        // reads the re-armed init segment followed by the TARGET media segment.
        g_hlsResumeSec = sec;
        g_liveRestartPending = 1;
        snprintf(g_status, sizeof(g_status), "seeking %ds", (int)(sec + 0.5));
        trace_mark("seek hls reopen target=%.3f fmp4=%d sepAudio=%d",
                   sec, hls_is_fmp4(), hls_has_separate_audio());
        return;
    }
    if (g_isHls && hls_can_seek()) {
        // Exactly one thread may advance g_segIdx. Stop the read-ahead owner,
        // reposition HLS, then recreate the ring around the new segment.
        seg_readahead_stop();
        seekOk = (hls_seek_time(sec, &actual) == 0);
        if (seekOk) {
            g_hlsResetGen = hls_reset_generation();
            g_hlsSegGen = hls_generation();
            seg_readahead_start();
        }
    } else if (g_isHls && hls_is_live() && hls_can_seek_clamped()) {
        // Fake-live VOD (no EXT-X-ENDLIST): av_seek_frame can never work on the
        // non-seekable concatenated stream, so reposition by segment index and
        // clamp to the known window instead of silently doing nothing.
        seg_readahead_stop();
        seekOk = (hls_seek_clamped(sec, &actual) == 0);
        if (seekOk) {
            g_hlsResetGen = hls_reset_generation();
            g_hlsSegGen = hls_generation();
            seg_readahead_start();
        }
    } else {
        int64_t ts = (int64_t)(sec * AV_TIME_BASE);
        seekOk = (av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0);
        // Never fail silently: av_seek_frame cannot work on a concatenated HLS
        // stream, and a scrub bar that moves while nothing happens is impossible
        // to diagnose from the couch.
        if (!seekOk && g_isHls)
            snprintf(g_status, sizeof(g_status), "seek unavailable (%s)",
                     hls_has_separate_audio() ? "separate audio track" : "stream not seekable");
    }
    if (seekOk) {
        fq_flush();
        if (g_vdec) avcodec_flush_buffers(g_vdec);
        if (g_useHw && g_bsf) av_bsf_flush(g_bsf);   // next AU after seek is a keyframe
        if (g_useHw) { ro_clear(); vdec_hw_reset(); g_lastEmitPts = AV_NOPTS_VALUE; }  // fresh refs
        if (g_adec) avcodec_flush_buffers(g_adec);
        if (g_swr) { swr_close(g_swr); swr_init(g_swr); }
        if (g_haveAudio) {
            // av_seek_frame(...BACKWARD) lands on a keyframe before the requested
            // target. Leave the clock unanchored after flushing; the first real
            // decoded audio PTS must establish it. Forcing it to `sec` made the
            // earlier audio content play against a later clock, so Castify seeks
            // produced sound that visibly trailed the picture.
            audio_flush();
        }
        trace_mark("seek applied target=%.3f actual=%.3f hls=%d audio=reanchor-next-pts",
                   sec, actual, g_isHls);
        g_sepAudioEof = 0;
        g_gotFrame = 0;          // re-anchor the pacing clock on the next frame
        if (g_threaded) {
            g_rebuffering = 1;
            g_startupGateAt = sceKernelGetProcessTime();
            audio_pause(1);      // refill both clocks before presenting after scrub
        }
        g_curSec = actual;
        g_active = 1;
        snprintf(g_status, sizeof(g_status), "seeking %.0fs", sec);
    }
    // Do not erase the timestamp of a newer request that arrived while this
    // seek was resetting demux/decoder state; it gets its own debounce window.
    if (!g_seekPending && g_seekRequestedAt == requestAt) g_seekRequestedAt = 0;
}

static void apply_hls_reset(void) {
    fq_flush();
    if (g_vdec) avcodec_flush_buffers(g_vdec);
    if (g_adec) avcodec_flush_buffers(g_adec);
    if (g_swr) { swr_close(g_swr); swr_init(g_swr); }
    if (g_haveAudio) {
        audio_flush();
        audio_pause(1);
    }
    g_gotFrame = 0;
    g_startProc = 0;
    g_startPts = 0;
    g_emptyCnt = 0;
    g_rebuffering = 1;
    g_startupGateAt = sceKernelGetProcessTime();
    g_decEof = 0;
    g_lastLagUs = 0;
    snprintf(g_status, sizeof(g_status), "buffering live");
}

void player_stats(PlayerStats *s) {
    memset(s, 0, sizeof(*s));
    s->hw = g_useHw;
    s->hls = g_isHls;
    s->segDemux = g_hlsSegDemux;
    s->w = g_srcW; s->h = g_srcH;
    s->frames = g_frames;
    s->drops = g_drops;
    s->bitrateMbps = g_bytesPerSec * 8.0 / 1e6;
    s->aheadSec = g_isLocal ? 0 : ((g_bytesPerSec > 0) ? (double)httpsrc_ahead_bytes() / g_bytesPerSec : 0);
    s->bufPct = player_buffer_pct();
    s->lan = g_isLocal || ((!g_isHls) ? httpsrc_is_lan() : 0);
    snprintf(s->codec, sizeof(s->codec), "%s", g_codecLabel[0] ? g_codecLabel : "video");
}

void player_debug(char *out, int len) {
    double ahead = (!g_isLocal && g_bytesPerSec > 0) ? (double)httpsrc_ahead_bytes() / g_bytesPerSec : 0;
    uint64_t flipAvg = 0, flipMax = 0, flipWaitAvg = 0, flipWaitMax = 0;
    gfx_present_stats(&flipAvg, &flipMax, &flipWaitAvg, &flipWaitMax);
    snprintf(out, len,
             "ff%s%s%s %dx%d | fr=%ld drop=%ld(q%ld/l%ld/r%ld) q=%d/%d ro=%d cv=%llu/%llu dc=%llu/%llu qw=%llu/%llu flip=%llu/%llu(w%llu/%llu)us ra=%d/%d rb=%d ahead=%.1fs lag=%lldms er=%d dmem=%ldKB | as=%d%s%s %s | %s | %s | %s",
             g_useHw ? "/HW" : "", g_isHls ? (g_hlsSegDemux ? "/hls-seg" : "/hls") : "", g_threaded ? "/T" : "", g_srcW, g_srcH,
             g_frames, g_drops, g_queueDrops, g_lateDrops, g_reorderDrops, g_fqCount, FQ_SLOTS,
             g_hwReorder,
             (unsigned long long)(g_presentCalls ? g_presentUsTotal / g_presentCalls : 0),
             (unsigned long long)g_presentUsMax,
             (unsigned long long)(g_decodeCalls ? g_decodeUsTotal / g_decodeCalls : 0),
             (unsigned long long)g_decodeUsMax,
             (unsigned long long)(g_fqWaitCalls ? g_fqWaitUsTotal / g_fqWaitCalls : 0),
             (unsigned long long)g_fqWaitUsMax,
             (unsigned long long)flipAvg, (unsigned long long)flipMax,
             (unsigned long long)flipWaitAvg, (unsigned long long)flipWaitMax,
             g_srCount, SEG_RING, g_rebufTotal, ahead,
             (long long)(g_lastLagUs / 1000), g_lastErr, vdec_hw_dmem_outstanding() / 1024,
             g_sepAudioMode ? g_aastream : g_astream, g_sepAudioMode ? "/sep" : "",
             (g_sepAudioMode && g_sepAudioEof) ? "/eof" : "",
             audio_debug(),
             (!g_active && g_errorCode[0] && g_sourceErrorDiag[0]) ? g_sourceErrorDiag :
                 (g_isLocal ? "local file" : (g_isHls ? hls_debug() : httpsrc_debug())),
             g_swDiag, g_stopDiag);
}

// Convert+scale a decoded frame into g_scaled (BGRA), fitting the display with
// letterboxing. g_scaled then holds the last presented frame. Returns 0 on ok.
static int build_scaled(AVFrame *fr, Gfx *g) {
    int dw = g->width, dh = g->height;
    int sw = fr->width, sh = fr->height;
    if (sw <= 0 || sh <= 0) return -1;

    int scaledW = dw, scaledH = (int)((int64_t)dw * sh / sw);
    if (scaledH > dh) { scaledH = dh; scaledW = (int)((int64_t)dh * sw / sh); }
    if (scaledW < 1) scaledW = 1; if (scaledH < 1) scaledH = 1;

    enum AVPixelFormat srcFmt = g_useHw ? AV_PIX_FMT_NV12 : g_vdec->pix_fmt;
    // Bob-deinterlace: for an interlaced source (1080i broadcast, forced to
    // software decode) feed sws ONE field — half the lines via doubled strides —
    // and let it scale that field up to full height. Removes combing on motion;
    // costs half the vertical resolution, which is invisible after upscaling.
    int di = g_interlaced ? 1 : 0;
    int srcH = di ? sh / 2 : sh;

    if (scaledW != g_scaledW || scaledH != g_scaledH || !g_scaled) {
        free(g_scaled);
        g_scaled = malloc((size_t)scaledW * scaledH * 4);
        if (!g_scaled) return -1;
        g_scaledW = scaledW; g_scaledH = scaledH;
        if (g_sws) { sws_freeContext(g_sws); g_sws = NULL; }
    }
    // Rebuild sws if the SOURCE geometry/format changed too (not just the output)
    // — otherwise a discontinuity-driven resolution change reads with wrong strides.
    if (g_sws && (sw != g_swsSrcW || srcH != g_swsSrcH || (int)srcFmt != g_swsSrcFmt)) {
        sws_freeContext(g_sws); g_sws = NULL;
    }
    if (!g_sws) {
        // FAST_BILINEAR: the software present (HLS path) was dropping frames because
        // the single-threaded 720p->1080p upscale couldn't sustain 30fps. Fast
        // bilinear is materially cheaper at near-identical quality for upscales.
        g_sws = sws_getContext(sw, srcH, srcFmt,
                               scaledW, scaledH, AV_PIX_FMT_BGRA,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g_sws) return -1;
        g_swsSrcW = sw; g_swsSrcH = srcH; g_swsSrcFmt = (int)srcFmt;
    }
    uint8_t *dst[4] = { g_scaled, NULL, NULL, NULL };
    int dstStride[4] = { g_scaledW * 4, 0, 0, 0 };
    if (di) {
        const uint8_t *src[4] = { fr->data[0], fr->data[1], fr->data[2], fr->data[3] };
        int srcStr[4] = { fr->linesize[0] * 2, fr->linesize[1] * 2, fr->linesize[2] * 2, fr->linesize[3] * 2 };
        sws_scale(g_sws, src, srcStr, 0, srcH, dst, dstStride);
    } else {
        sws_scale(g_sws, (const uint8_t * const *)fr->data, fr->linesize, 0, sh, dst, dstStride);
    }
    return 0;
}

static int64_t frame_pts_us(AVFrame *f) {
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = f->pts;
    if (pts == AV_NOPTS_VALUE) return 0;
    if (g_hlsSegDemux) return pts;   // segment demux stores queue PTS directly in us
    return (int64_t)(pts * av_q2d(g_fmt->streams[g_vstream]->time_base) * 1000000.0);
}

// Clear ONLY the letterbox/pillarbox border, and only when needed (geometry
// change, or an overlay drew over the bars). Full-screen clears every frame were
// the dominant render-rate sink; the video rect needs no clear since the
// convert/blit overwrites it.
static void clear_bars_gated(Gfx *g, int ox, int oy, int sW, int sH) {
    int dw = g->width, dh = g->height;
    static int s_lw = -1, s_lh = -1, s_lx = -1, s_ly = -1;
    if (sW != s_lw || sH != s_lh || ox != s_lx || oy != s_ly) {
        s_lw = sW; s_lh = sH; s_lx = ox; s_ly = oy;
        g_barClearLeft = (sW != dw || sH != dh) ? GFX_BUFFER_COUNT + 1 : 0;
    }
    if (g_barClearLeft <= 0) return;
    g_barClearLeft--;
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    const uint32_t BAR = 0x80000000u;
    int vy0 = oy < 0 ? 0 : oy, vy1 = oy + sH > dh ? dh : oy + sH;
    int vx0 = ox < 0 ? 0 : ox, vx1 = ox + sW > dw ? dw : ox + sW;
    for (int yy = 0; yy < vy0; yy++)
        { uint32_t *r = fb + (size_t)yy * dw; for (int xx = 0; xx < dw; xx++) r[xx] = BAR; }
    for (int yy = vy1; yy < dh; yy++)
        { uint32_t *r = fb + (size_t)yy * dw; for (int xx = 0; xx < dw; xx++) r[xx] = BAR; }
    for (int yy = vy0; yy < vy1; yy++) {
        uint32_t *r = fb + (size_t)yy * dw;
        for (int xx = 0; xx < vx0; xx++)  r[xx] = BAR;
        for (int xx = vx1; xx < dw; xx++) r[xx] = BAR;
    }
}

static void blit_scaled(Gfx *g) {
    int dw = g->width, dh = g->height;
    int ox = (dw - g_scaledW) / 2, oy = (dh - g_scaledH) / 2;
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    clear_bars_gated(g, ox, oy, g_scaledW, g_scaledH);
    // sws already produced BGRA (memory B,G,R,A == framebuffer byte order with A
    // in the top byte). The framebuffer wants the top byte = 0x80, so just force
    // it: one mask+or per pixel instead of byte-by-byte shuffling.
    for (int y = 0; y < g_scaledH; y++) {
        const uint32_t *row = (const uint32_t *)(g_scaled + (size_t)y * g_scaledW * 4);
        uint32_t *out = fb + (size_t)(oy + y) * dw + ox;
        for (int x = 0; x < g_scaledW; x++)
            out[x] = (row[x] & 0x00FFFFFFu) | 0x80000000u;
    }
}

// Decode one audio packet (from either demuxer), resample to S16 stereo 48k,
// queue to sceAudioOut. `atb` is the audio stream's time_base for the clock.
static void decode_audio_frame(AVPacket *pkt, AVRational atb) {
    if (avcodec_send_packet(g_adec, pkt) < 0) return;
    for (;;) {
        int r = avcodec_receive_frame(g_adec, g_aframe);
        if (r < 0) break;
        int64_t apts = g_aframe->best_effort_timestamp;
        if (apts == AV_NOPTS_VALUE) apts = g_aframe->pts;
        if (apts != AV_NOPTS_VALUE) {
            double ptsSec = (double)apts * av_q2d(atb);
            int firstAnchor = !audio_has_clock();
            audio_set_base(ptsSec);
            if (firstAnchor) trace_mark("audio anchor pts=%.3f", ptsSec);
        }
        int out_max = (int)swr_get_out_samples(g_swr, g_aframe->nb_samples);
        if (out_max <= 0) continue;
        if (out_max > g_abufCap) {
            free(g_abuf);
            g_abuf = malloc((size_t)sizeof(int16_t) * 2 * out_max);
            g_abufCap = g_abuf ? out_max : 0;
        }
        if (!g_abuf) break;
        uint8_t *outp = (uint8_t *)g_abuf;
        int n = swr_convert(g_swr, &outp, out_max,
                            (const uint8_t **)g_aframe->extended_data, g_aframe->nb_samples);
        if (n > 0) audio_write(g_abuf, n);
    }
}
// Muxed path: audio packets arrive on g_fmt alongside video.
static void decode_audio_pkt(void) {
    decode_audio_frame(g_pkt, g_fmt->streams[g_astream]->time_base);
}

// Separate-audio thread: pump the audio rendition demuxer independently of the
// video decode, so HLS streams with audio in their own playlist still have sound.
static void *audio_thread_main(void *arg) {
    trace_mark("thread + audio self=%p", (void *)scePthreadSelf());
    (void)arg;
    AVRational atb = g_afmt->streams[g_aastream]->time_base;
    int eofStreak = 0;
    while (!g_audioStop) {
        if (g_paused) { sceKernelUsleep(8000); continue; }
        int rc = av_read_frame(g_afmt, g_apkt);
        if (rc < 0) {
            // Separate HLS audio is commonly a finite rendition beside a longer
            // video playlist. Once it is truly exhausted, stop driving video sync
            // from a clock that can no longer advance.
            if (++eofStreak >= 3) g_sepAudioEof = 1;
            sceKernelUsleep(30000);
            continue;
        }
        eofStreak = 0;
        if (g_apkt->stream_index == g_aastream) decode_audio_frame(g_apkt, atb);
        av_packet_unref(g_apkt);
    }
    return NULL;
}

// Set up the separate HLS audio rendition: second demuxer over hls_audio_read,
// reusing g_adec/g_swr/g_aframe. Returns 0 and sets g_haveAudio/g_sepAudioMode
// on success; leaves audio disabled on any failure (video still plays).
static int setup_separate_audio(void) {
    uint8_t *abuf = av_malloc(AVIO_BUFSZ);
    if (!abuf) return -1;
    g_aavio = avio_alloc_context(abuf, AVIO_BUFSZ, 0, NULL, avio_aread_cb, NULL, NULL);
    if (!g_aavio) { av_free(abuf); return -1; }
    g_aavio->seekable = 0;
    g_afmt = avformat_alloc_context();
    g_afmt->pb = g_aavio;
    g_afmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    g_afmt->probesize = 2 * 1024 * 1024;
    g_afmt->max_analyze_duration = 4 * (int64_t)AV_TIME_BASE;
    if (avformat_open_input(&g_afmt, "audio", NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(g_afmt, NULL) < 0) return -1;
    const AVCodec *adec = NULL;
    g_aastream = av_find_best_stream(g_afmt, AVMEDIA_TYPE_AUDIO, -1, -1, &adec, 0);
    if (g_aastream < 0 || !adec) { g_aastream = -1; return -1; }
    g_adec = avcodec_alloc_context3(adec);
    avcodec_parameters_to_context(g_adec, g_afmt->streams[g_aastream]->codecpar);
    g_adec->thread_count = 1;
    if (avcodec_open2(g_adec, adec, NULL) != 0) return -1;
    AVChannelLayout out_ch = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&g_swr, &out_ch, AV_SAMPLE_FMT_S16, 48000,
                            &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate,
                            0, NULL) != 0 || !g_swr || swr_init(g_swr) != 0) return -1;
    g_aframe = av_frame_alloc();
    g_apkt   = av_packet_alloc();
    if (audio_open() != 0) return -1;
    g_haveAudio = 1; g_sepAudioMode = 1;
    g_sepAudioEof = 0;
    notify_dbg("PS4 Cast: audio %s %dHz (HLS rendition)", adec->name, g_adec->sample_rate);
    return 0;
}

// Push a ready frame onto the queue (blocks if full; frees it and returns 0 if
// stopping/seeking). Shared by the software and hardware decode paths.
static int fq_push(AVFrame *cl) {
    scePthreadMutexLock(&g_fqMtx);
    uint64_t waitT0 = 0;
    while (!g_decStop && !g_seekPending && g_fqCount >= FQ_SLOTS) {
        if (!waitT0) waitT0 = sceKernelGetProcessTime();
        // The decode thread is the ONLY producer of audio as well as video, so
        // blocking here on a full video queue also stops audio being decoded and
        // the ring drains to empty — measured as fill swinging 2048ms -> 0ms with
        // underruns climbing ~10/s (audible drop-outs) and >2s of A/V drift from
        // the resulting silence padding. When audio is about to run dry, drop THIS
        // frame instead of waiting: the queue is already full of video, so losing
        // one frame costs far less than a gap in the sound, and returning to the
        // loop lets the next audio packets be decoded.
        if (audio_fill_ms() < 400) {
            if (waitT0) {
                uint64_t waitUs = sceKernelGetProcessTime() - waitT0;
                g_fqWaitUsTotal += waitUs; g_fqWaitCalls++;
                if (waitUs > g_fqWaitUsMax) g_fqWaitUsMax = waitUs;
            }
            g_drops++; g_queueDrops++;
            scePthreadMutexUnlock(&g_fqMtx);
            av_frame_free(&cl);
            return 1;
        }
        scePthreadCondTimedwait(&g_fqNotFull, &g_fqMtx, 20 * 1000);
    }
    if (waitT0) {
        uint64_t waitUs = sceKernelGetProcessTime() - waitT0;
        g_fqWaitUsTotal += waitUs; g_fqWaitCalls++;
        if (waitUs > g_fqWaitUsMax) g_fqWaitUsMax = waitUs;
    }
    if (g_decStop || g_seekPending) { scePthreadMutexUnlock(&g_fqMtx); av_frame_free(&cl); return 0; }
    g_fq[(g_fqHead + g_fqCount) % FQ_SLOTS].frame = cl;
    g_fqCount++;
    scePthreadCondSignal(&g_fqNotEmpty);
    scePthreadMutexUnlock(&g_fqMtx);
    return 1;
}

// Release the lowest-PTS frame from the reorder buffer to the render queue.
static void ro_emit_one(void) {
    if (g_roN <= 0) return;
    int mi = 0;
    for (int i = 1; i < g_roN; i++) if (g_ro[i]->pts < g_ro[mi]->pts) mi = i;
    AVFrame *f = g_ro[mi];
    g_ro[mi] = g_ro[--g_roN];          // compact (order within buffer doesn't matter)
    // The render queue is consumed from its head and therefore must remain PTS
    // monotonic. If a demuxer under-reported B-frame delay, presenting this old
    // frame after a future one would visibly jump backward. Grow the window for
    // subsequent frames and discard this already-late picture instead.
    if (g_lastEmitPts != AV_NOPTS_VALUE && f->pts < g_lastEmitPts) {
        g_drops++; g_reorderDrops++;
        av_frame_free(&f);
        return;
    }
    g_lastEmitPts = f->pts;
    fq_push(f);                         // frees f if stopping
}
static void ro_drain(void) { while (g_roN > 0) ro_emit_one(); }     // EOF: flush in order
static void ro_clear(void) { for (int i = 0; i < g_roN; i++) av_frame_free(&g_ro[i]); g_roN = 0; }

static void hw_observe_packet_order(const AVPacket *pkt) {
    if (!pkt || pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE || pkt->pts == pkt->dts)
        return;
    // Observe reordering before submitting the first delayed picture. Waiting
    // for a PTS regression is one frame too late because a future picture has
    // already entered the presentation queue by then. Four covers the common
    // H.264 maximum of three consecutive B pictures with one slot of margin.
    if (g_hwReorder < H264_REORDER_FLOOR) {
        g_hwReorder = H264_REORDER_FLOOR;
        trace_mark("hw reorder floor=%d pts=%lld dts=%lld", g_hwReorder,
                   (long long)pkt->pts, (long long)pkt->dts);
    }
}

static int hw_failover_to_software(int err) {
    g_lastErr = err;
    if (++g_hwFailCount < 3) return 0;
    trace_mark("hw failover err=%d dbg=%s", err, vdec_hw_debug());
    snprintf(g_status, sizeof(g_status), "HW decode failed; switching to software");
    ro_clear();
    vdec_hw_close();
    if (g_bsf)   { av_bsf_free(&g_bsf); g_bsf = NULL; }
    if (g_hwPkt) { av_packet_free(&g_hwPkt); }
    g_useHw = 0;
    g_lastEmitPts = AV_NOPTS_VALUE;
    if (g_vdec) avcodec_free_context(&g_vdec);
    if (g_swCodec && open_sw_video(g_swCodec) == 0) {
        notify_dbg("PS4 Cast: HW decode failed; software fallback");
        return 1;
    }
    g_active = 0;
    g_decEof = 1;
    player_set_error("decoder", "Hardware decode failed, and software decode could not recover this video.");
    return 1;
}

static int hw_reopen_for_params(const AVCodecParameters *par) {
    if (!g_useHw || !par || par->codec_id != AV_CODEC_ID_H264) return 0;
    int w = par->width  > 0 ? par->width  : g_hwMaxW;
    int h = par->height > 0 ? par->height : g_hwMaxH;
    int p = par->profile > 0 ? par->profile : 0;
    int l = par->level   > 0 ? par->level   : 0;

    // SPS/PPS can legally change at HLS discontinuities or variant switches.
    // Reset is enough when the already-open decoder maxima cover the new
    // geometry/profile; reopen when the max config itself must change.
    int needReopen = (w > g_hwMaxW || h > g_hwMaxH ||
                      (p && g_hwProfile && p != g_hwProfile) ||
                      (l && g_hwLevel && l > g_hwLevel));
    int changed = (w != g_srcW || h != g_srcH ||
                   (p && p != g_hwProfile) || (l && l != g_hwLevel));
    if (!changed && !needReopen) return 0;

    trace_mark("hw params change %dx%d p%d/l%d -> %dx%d p%d/l%d reopen=%d",
               g_srcW, g_srcH, g_hwProfile, g_hwLevel, w, h, p, l, needReopen);
    ro_clear();
    g_lastEmitPts = AV_NOPTS_VALUE;
    g_hwFailCount = 0;
    g_srcW = w; g_srcH = h;

    if (!needReopen) {
        vdec_hw_reset();
        if (p) g_hwProfile = p;
        if (l) g_hwLevel = l;
        return 0;
    }

    vdec_hw_close();
    if (vdec_hw_open(w, h, p, l) == 0) {
        g_hwMaxW = w; g_hwMaxH = h; g_hwProfile = p; g_hwLevel = l;
        return 0;
    }
    return hw_failover_to_software(-9001);
}

// Hardware H.264 path: convert the packet to Annex B, decode each access unit on
// the GPU silicon into NV12, copy it into an AVFrame, and feed it through the
// reorder buffer -> queue -> sync/scale/blit path (sws converts NV12->BGRA).
static void decode_video_hw(AVPacket *pkt) {
    if (av_bsf_send_packet(g_bsf, pkt) < 0) return;
    while (av_bsf_receive_packet(g_bsf, g_hwPkt) == 0) {
        hw_observe_packet_order(g_hwPkt);
        VdecHwFrame hf;
        uint64_t decodeT0 = sceKernelGetProcessTime();
        int got = vdec_hw_decode(g_hwPkt->data, g_hwPkt->size, g_hwPkt->pts, g_hwPkt->dts, &hf);
        uint64_t decodeUs = sceKernelGetProcessTime() - decodeT0;
        g_decodeUsTotal += decodeUs; g_decodeCalls++;
        if (decodeUs > g_decodeUsMax) g_decodeUsMax = decodeUs;
        av_packet_unref(g_hwPkt);
        if (got < 0) { if (hw_failover_to_software(got)) break; continue; }
        if (got != 1) continue;
        g_hwFailCount = 0;
        AVFrame *cl = av_frame_alloc();
        if (!cl) continue;
        cl->format = AV_PIX_FMT_NV12; cl->width = hf.width; cl->height = hf.height;
        if (av_frame_get_buffer(cl, 32) < 0) { av_frame_free(&cl); continue; }
        for (int y = 0; y < hf.height; y++)
            memcpy(cl->data[0] + (size_t)y * cl->linesize[0], hf.y + (size_t)y * hf.pitch, hf.width);
        for (int y = 0; y < hf.height / 2; y++)
            memcpy(cl->data[1] + (size_t)y * cl->linesize[1], hf.uv + (size_t)y * hf.pitch, hf.width);
        // Use the OUTPUT frame's own PTS (from vdec_hw's FIFO), not the submitted
        // packet's — they differ for reordered/delayed pictures.
        cl->pts = (hf.pts == AV_NOPTS_VALUE) ? (int64_t)g_frames : hf.pts;
        g_frames++;
        // Adaptive reorder: if a frame arrives with a PTS behind one we already
        // emitted, the window was too small (demuxer under-reported video_delay) —
        // grow it so it doesn't happen again. Robust when video_delay is 0/wrong.
        if (g_lastEmitPts != AV_NOPTS_VALUE && cl->pts < g_lastEmitPts && g_hwReorder < HW_REORDER - 1)
            g_hwReorder++;
        if (g_roN < HW_REORDER) g_ro[g_roN++] = cl; else av_frame_free(&cl);
        while (g_roN > g_hwReorder) ro_emit_one();   // keep a video_delay window
    }
}

// Decode one packet from the continuous demuxer. Keeping this in one helper
// lets fMP4 startup replay its compressed staging queue through exactly the
// same HW/SW path as ordinary packets.
static int decode_video_packet(AVPacket *pkt) {
    if (g_useHw) {
        if (g_isHls && !hls_is_fmp4()) {
            int gen = hls_generation();
            if (gen != g_hlsSegGen) {
                ro_clear();
                if (g_bsf) av_bsf_flush(g_bsf);
                vdec_hw_reset();
                g_lastEmitPts = AV_NOPTS_VALUE;
                g_hlsSegGen = gen;
            }
        }
        decode_video_hw(pkt);
        return !g_decStop && !g_seekPending;
    }

    if (avcodec_send_packet(g_vdec, pkt) < 0) return 1;
    for (;;) {
        int got = avcodec_receive_frame(g_vdec, g_frame);
        if (got == AVERROR(EAGAIN)) break;
        if (got < 0) { g_lastErr = got; break; }
        g_frames++;
        AVFrame *cl = av_frame_clone(g_frame);
        av_frame_unref(g_frame);
        if (!cl) continue;
        if (!fq_push(cl)) return 0;
    }
    return 1;
}

static int prevideo_stage(AVPacket *pkt) {
    if (g_preVideoCount >= PREVIDEO_SLOTS ||
        g_preVideoBytes + pkt->size > PREVIDEO_BYTES)
        return 0;
    AVPacket *copy = av_packet_clone(pkt);
    if (!copy) return 0;
    g_preVideo[g_preVideoCount++] = copy;
    g_preVideoBytes += copy->size;
    return 1;
}

static int prevideo_drain(void) {
    for (int i = 0; i < g_preVideoCount; i++) {
        AVPacket *pkt = g_preVideo[i];
        g_preVideo[i] = NULL;
        if (pkt) {
            int ok = decode_video_packet(pkt);
            av_packet_free(&pkt);
            if (!ok) {
                for (int j = i + 1; j < g_preVideoCount; j++)
                    if (g_preVideo[j]) av_packet_free(&g_preVideo[j]);
                g_preVideoCount = 0;
                g_preVideoBytes = 0;
                g_preVideoAudioAfter = 0;
                return 0;
            }
        }
    }
    g_preVideoCount = 0;
    g_preVideoBytes = 0;
    g_preVideoAudioAfter = 0;
    return 1;
}

// HLS segment-demux hardware path: MPEG-TS packets are already annex-b (SPS/PPS
// in-band, one access unit per packet), so feed them straight to the GPU decoder
// — no bitstream filter. Unlike the direct path, the queue stores PTS in
// microseconds (frame_pts_us returns it verbatim for seg-demux), so convert from
// the segment time_base here before the reorder buffer.
static void decode_video_hw_seg(AVPacket *pkt, AVRational vtb) {
    hw_observe_packet_order(pkt);
    VdecHwFrame hf;
    uint64_t decodeT0 = sceKernelGetProcessTime();
    int got = vdec_hw_decode(pkt->data, pkt->size, pkt->pts, pkt->dts, &hf);
    uint64_t decodeUs = sceKernelGetProcessTime() - decodeT0;
    g_decodeUsTotal += decodeUs; g_decodeCalls++;
    if (decodeUs > g_decodeUsMax) g_decodeUsMax = decodeUs;
    if (got < 0) { hw_failover_to_software(got); return; }
    if (got != 1) return;
    g_hwFailCount = 0;
    AVFrame *cl = av_frame_alloc();
    if (!cl) return;
    cl->format = AV_PIX_FMT_NV12; cl->width = hf.width; cl->height = hf.height;
    if (av_frame_get_buffer(cl, 32) < 0) { av_frame_free(&cl); return; }
    for (int y = 0; y < hf.height; y++)
        memcpy(cl->data[0] + (size_t)y * cl->linesize[0], hf.y + (size_t)y * hf.pitch, hf.width);
    for (int y = 0; y < hf.height / 2; y++)
        memcpy(cl->data[1] + (size_t)y * cl->linesize[1], hf.uv + (size_t)y * hf.pitch, hf.width);
    int64_t ptsUs = (hf.pts == AV_NOPTS_VALUE) ? (int64_t)g_frames * 33333
                                               : (int64_t)(hf.pts * av_q2d(vtb) * 1000000.0);
    cl->pts = ptsUs; cl->best_effort_timestamp = ptsUs;
    g_frames++;
    if (g_lastEmitPts != AV_NOPTS_VALUE && cl->pts < g_lastEmitPts && g_hwReorder < HW_REORDER - 1)
        g_hwReorder++;
    if (g_roN < HW_REORDER) g_ro[g_roN++] = cl; else av_frame_free(&cl);
    while (g_roN > g_hwReorder) ro_emit_one();
}

// ---- parallel NV12 present (hardware path) --------------------------------
// Hardware decode freed all 6 CPU cores, so we use them to do the NV12->BGRA
// colour-convert + scale that single-threaded couldn't sustain at 1080p. Worker
// threads each convert a horizontal band either into g_scaled or directly into
// the active framebuffer. Software (YUV420P) keeps the sws path.
#define PRESENT_WORKERS 5
typedef struct {
    const uint8_t *y, *uv; int sw, sh, ypitch, uvpitch;
    uint32_t *dst; int dstPitch, ox, oy, scaledW, scaledH;
    const uint16_t *xmap, *uvmap;
    int directX;
} PresentJob;

static OrbisPthread       g_pwTh[PRESENT_WORKERS];
static int                g_pwStarted[PRESENT_WORKERS];   // which slots actually launched
static OrbisPthreadMutex  g_pwMtx;
static OrbisPthreadCond   g_pwGo, g_pwDone;
static volatile int       g_pwGen, g_pwDoneCount, g_pwStop, g_pwUp;
static PresentJob         g_pwJob;

// Hardware sources are <=1920 wide. Precomputing the horizontal nearest-neighbor
// map removes a 64-bit divide from EVERY output pixel (the old 1080p hot spot).
#define PRESENT_MAP_MAX 1920
static uint16_t g_presentX[PRESENT_MAP_MAX], g_presentUV[PRESENT_MAP_MAX];
static int g_presentMapSW = -1, g_presentMapDW = -1;

// BT.709 limited-range contribution tables replace five integer multiplies per
// output pixel. The direct-width path also shares chroma work per NV12 pair.
static int g_yTab[256], g_rVTab[256], g_gUTab[256], g_gVTab[256], g_bUTab[256];
static int g_colorTablesReady;

static void prepare_color_tables(void) {
    if (g_colorTablesReady) return;
    for (int i = 0; i < 256; i++) {
        g_yTab[i]  = (i - 16) * 298;
        g_rVTab[i] = (i - 128) * 459;
        g_gUTab[i] = (i - 128) * -55;
        g_gVTab[i] = (i - 128) * -136;
        g_bUTab[i] = (i - 128) * 541;
    }
    g_colorTablesReady = 1;
}

static inline uint32_t pack_yuv(uint8_t y, int rv, int guv, int bu) {
    int c = g_yTab[y];
    int r = (c + rv) >> 8;
    int gg = (c + guv) >> 8;
    int b = (c + bu) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0x80000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
}

// Eight-pixel BT.709 limited-range NV12 -> BGRA conversion. PS4's Jaguar CPU
// guarantees SSE2; processing four chroma pairs together removes most scalar
// table traffic and per-pixel packing from the native-width 1080p path.
static inline void convert_8_nv12(const uint8_t *y, const uint8_t *uv, uint32_t *out) {
    const __m128i z = _mm_setzero_si128();
    const __m128i bias16 = _mm_set1_epi16(16);
    const __m128i bias128 = _mm_set1_epi16(128);
    __m128i y8 = _mm_loadl_epi64((const __m128i *)y);
    __m128i uv8 = _mm_loadl_epi64((const __m128i *)uv);
    __m128i u16pairs = _mm_and_si128(uv8, _mm_set1_epi16(0x00ff));
    __m128i v16pairs = _mm_srli_epi16(uv8, 8);
    __m128i u4 = _mm_packus_epi16(u16pairs, z);
    __m128i v4 = _mm_packus_epi16(v16pairs, z);
    __m128i u8 = _mm_unpacklo_epi8(u4, u4);
    __m128i v8 = _mm_unpacklo_epi8(v4, v4);

    __m128i yw = _mm_sub_epi16(_mm_unpacklo_epi8(y8, z), bias16);
    __m128i uw = _mm_sub_epi16(_mm_unpacklo_epi8(u8, z), bias128);
    __m128i vw = _mm_sub_epi16(_mm_unpacklo_epi8(v8, z), bias128);
    __m128i yc = _mm_mullo_epi16(yw, _mm_set1_epi16(74));
    __m128i rw = _mm_adds_epi16(yc, _mm_mullo_epi16(vw, _mm_set1_epi16(115)));
    __m128i gw = _mm_adds_epi16(yc, _mm_adds_epi16(
        _mm_mullo_epi16(uw, _mm_set1_epi16(-14)),
        _mm_mullo_epi16(vw, _mm_set1_epi16(-34))));
    __m128i bw = _mm_adds_epi16(yc, _mm_mullo_epi16(uw, _mm_set1_epi16(135)));
    rw = _mm_srai_epi16(rw, 6);
    gw = _mm_srai_epi16(gw, 6);
    bw = _mm_srai_epi16(bw, 6);

    __m128i rb = _mm_packus_epi16(rw, z);
    __m128i gb = _mm_packus_epi16(gw, z);
    __m128i bb = _mm_packus_epi16(bw, z);
    __m128i ab = _mm_set1_epi8((char)0x80);
    __m128i bg = _mm_unpacklo_epi8(bb, gb);
    __m128i ra = _mm_unpacklo_epi8(rb, ab);
    _mm_storeu_si128((__m128i *)(out + 0), _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128((__m128i *)(out + 4), _mm_unpackhi_epi16(bg, ra));
}

static void prepare_present_map(int sw, int scaledW) {
    if (sw == g_presentMapSW && scaledW == g_presentMapDW) return;
    for (int x = 0; x < scaledW; x++) {
        int sx = (int)((int64_t)x * sw / scaledW);
        if (sx >= sw) sx = sw - 1;
        g_presentX[x] = (uint16_t)sx;
        g_presentUV[x] = (uint16_t)(sx & ~1);
    }
    g_presentMapSW = sw;
    g_presentMapDW = scaledW;
}

// BT.709 limited-range YUV->RGB, nearest-neighbour scale.
static void convert_band(const PresentJob *j, int b0, int b1) {
    for (int oy = b0; oy < b1; oy++) {
        int srcY = (int)((int64_t)oy * j->sh / j->scaledH);
        if (srcY >= j->sh) srcY = j->sh - 1;
        const uint8_t *yrow  = j->y  + (size_t)srcY * j->ypitch;
        const uint8_t *uvrow = j->uv + (size_t)(srcY >> 1) * j->uvpitch;
        uint32_t *out = j->dst + (size_t)(j->oy + oy) * j->dstPitch + j->ox;
        if (j->directX) {
            int x = 0;
            for (; x + 7 < j->scaledW; x += 8)
                convert_8_nv12(yrow + x, uvrow + x, out + x);
            for (; x < j->scaledW; x += 2) {
                int u = uvrow[x], v = uvrow[x + 1];
                int rv = g_rVTab[v], guv = g_gUTab[u] + g_gVTab[v], bu = g_bUTab[u];
                out[x] = pack_yuv(yrow[x], rv, guv, bu);
                if (x + 1 < j->scaledW) out[x + 1] = pack_yuv(yrow[x + 1], rv, guv, bu);
            }
        } else {
            int lastUv = -1, rv = 0, guv = 0, bu = 0;
            for (int x = 0; x < j->scaledW; x++) {
                int uvx = j->uvmap[x];
                if (uvx != lastUv) {
                    int u = uvrow[uvx], v = uvrow[uvx + 1];
                    rv = g_rVTab[v]; guv = g_gUTab[u] + g_gVTab[v]; bu = g_bUTab[u];
                    lastUv = uvx;
                }
                out[x] = pack_yuv(yrow[j->xmap[x]], rv, guv, bu);
            }
        }
    }
}

static void *present_worker(void *arg) {
    int id = (int)(intptr_t)arg, lastGen = 0;
    for (;;) {
        scePthreadMutexLock(&g_pwMtx);
        while (!g_pwStop && g_pwGen == lastGen) scePthreadCondWait(&g_pwGo, &g_pwMtx);
        if (g_pwStop) { scePthreadMutexUnlock(&g_pwMtx); break; }
        lastGen = g_pwGen;
        PresentJob job = g_pwJob;
        scePthreadMutexUnlock(&g_pwMtx);

        int b0 = (int)((int64_t)job.scaledH * id / PRESENT_WORKERS);
        int b1 = (int)((int64_t)job.scaledH * (id + 1) / PRESENT_WORKERS);
        convert_band(&job, b0, b1);

        scePthreadMutexLock(&g_pwMtx);
        g_pwDoneCount++;
        scePthreadCondSignal(&g_pwDone);
        scePthreadMutexUnlock(&g_pwMtx);
    }
    return NULL;
}

static void present_pool_start(void) {
    if (g_pwUp) return;
    g_pwGen = g_pwDoneCount = g_pwStop = 0;
    scePthreadMutexInit(&g_pwMtx, NULL, "ps4cast_pw");
    scePthreadCondInit(&g_pwGo, NULL, "ps4cast_pwgo");
    scePthreadCondInit(&g_pwDone, NULL, "ps4cast_pwdone");
    int n = 0;
    for (int i = 0; i < PRESENT_WORKERS; i++) {
        g_pwStarted[i] = (scePthreadCreate(&g_pwTh[i], NULL, present_worker, (void *)(intptr_t)i, "ps4cast_pw") == 0);
        if (g_pwStarted[i]) n++;
    }
    g_pwUp = (n == PRESENT_WORKERS);
    if (!g_pwUp) {   // partial failure -> join exactly the started slots, fall back to serial
        scePthreadMutexLock(&g_pwMtx); g_pwStop = 1; scePthreadCondBroadcast(&g_pwGo); scePthreadMutexUnlock(&g_pwMtx);
        for (int i = 0; i < PRESENT_WORKERS; i++) if (g_pwStarted[i]) { scePthreadJoin(g_pwTh[i], NULL); g_pwStarted[i] = 0; }
        scePthreadCondDestroy(&g_pwGo); scePthreadCondDestroy(&g_pwDone); scePthreadMutexDestroy(&g_pwMtx);
    }
}
static void present_pool_stop(void) {
    if (!g_pwUp) return;
    scePthreadMutexLock(&g_pwMtx); g_pwStop = 1; scePthreadCondBroadcast(&g_pwGo); scePthreadMutexUnlock(&g_pwMtx);
    for (int i = 0; i < PRESENT_WORKERS; i++) if (g_pwStarted[i]) { scePthreadJoin(g_pwTh[i], NULL); g_pwStarted[i] = 0; }
    scePthreadCondDestroy(&g_pwGo); scePthreadCondDestroy(&g_pwDone); scePthreadMutexDestroy(&g_pwMtx);
    g_pwUp = 0;
}

// NV12 -> active framebuffer directly. This fuses color conversion, scaling, and
// final blit for the hardware path.
static int build_scaled_nv12_direct(AVFrame *fr, Gfx *g) {
    uint64_t presentT0 = sceKernelGetProcessTime();
    int dw = g->width, dh = g->height, sw = fr->width, sh = fr->height;
    // Hardware decode returns coded dimensions (e.g. 1920x1088 for 1080p).
    // Present only the visible stream size when known; otherwise the scaler
    // shrinks 1920x1088 into 1905x1080 and wastes work on padding/pillarbox.
    if (g_srcW > 0 && g_srcW <= sw) sw = g_srcW;
    if (g_srcH > 0 && g_srcH <= sh) sh = g_srcH;
    if (sw <= 0 || sh <= 0) return -1;
    int scaledW = dw, scaledH = (int)((int64_t)dw * sh / sw);
    if (scaledH > dh) { scaledH = dh; scaledW = (int)((int64_t)dh * sw / sh); }
    if (scaledW < 1) scaledW = 1; if (scaledH < 1) scaledH = 1;
    int ox = (dw - scaledW) / 2, oy = (dh - scaledH) / 2;

    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    // Clear ONLY the letterbox/pillarbox border (area outside the video rect),
    // and only when needed: on a geometry change (bars moved) or when the main
    // loop signalled an overlay is drawing over the bars (player_request_bar_clear
    // -> g_barClearLeft). Plain fullscreen-ish playback with no overlay skips the
    // clear entirely and runs at full render rate. The video rect itself never
    // needs a clear (the NV12 convert below fills it). The countdown spans all
    // rotating framebuffers so a dismissed overlay is wiped from every buffer.
    {
        static int s_lastSW = -1, s_lastSH = -1, s_lastOX = -1, s_lastOY = -1;
        if (scaledW != s_lastSW || scaledH != s_lastSH || ox != s_lastOX || oy != s_lastOY) {
            s_lastSW = scaledW; s_lastSH = scaledH; s_lastOX = ox; s_lastOY = oy;
            g_barClearLeft = GFX_BUFFER_COUNT + 1;
        }
        if (g_barClearLeft > 0) {
            g_barClearLeft--;
            const uint32_t BAR = 0x80000000u;
            int vy0 = oy, vy1 = oy + scaledH, vx0 = ox, vx1 = ox + scaledW;
            if (vy0 < 0) vy0 = 0; if (vy1 > dh) vy1 = dh;
            if (vx0 < 0) vx0 = 0; if (vx1 > dw) vx1 = dw;
            for (int yy = 0; yy < vy0; yy++)                   // top bar (full width)
                { uint32_t *r = fb + (size_t)yy * dw; for (int xx = 0; xx < dw; xx++) r[xx] = BAR; }
            for (int yy = vy1; yy < dh; yy++)                  // bottom bar (full width)
                { uint32_t *r = fb + (size_t)yy * dw; for (int xx = 0; xx < dw; xx++) r[xx] = BAR; }
            for (int yy = vy0; yy < vy1; yy++) {               // left + right pillars
                uint32_t *r = fb + (size_t)yy * dw;
                for (int xx = 0; xx < vx0; xx++)  r[xx] = BAR;
                for (int xx = vx1; xx < dw; xx++) r[xx] = BAR;
            }
        }
    }

    // Bob-deinterlace the HW decoder's woven interlaced output: feed the scaler
    // only the TOP field (every other source line, via half height + doubled
    // pitch) and let it stretch back to full height. scaledW/scaledH were derived
    // from the FULL frame above, so the aspect ratio is preserved.
    int jsh = sh, jyp = fr->linesize[0], juvp = fr->linesize[1];
    if (g_interlaced) { jsh = sh / 2; jyp *= 2; juvp *= 2; }
    prepare_color_tables();
    int directX = (sw == scaledW);
    if (!directX) {
        if (scaledW > PRESENT_MAP_MAX) return -1;
        prepare_present_map(sw, scaledW);
    }
    PresentJob job = { fr->data[0], fr->data[1], sw, jsh, jyp, juvp,
                       fb, dw, ox, oy, scaledW, scaledH,
                       directX ? NULL : g_presentX, directX ? NULL : g_presentUV,
                       directX };
    if (g_pwUp) {
        scePthreadMutexLock(&g_pwMtx);
        g_pwJob = job; g_pwDoneCount = 0; g_pwGen++;
        scePthreadCondBroadcast(&g_pwGo);
        while (g_pwDoneCount < PRESENT_WORKERS) scePthreadCondWait(&g_pwDone, &g_pwMtx);
        scePthreadMutexUnlock(&g_pwMtx);
    } else {
        convert_band(&job, 0, scaledH);
    }
    uint64_t presentUs = sceKernelGetProcessTime() - presentT0;
    g_presentUsTotal += presentUs;
    g_presentCalls++;
    if (presentUs > g_presentUsMax) g_presentUsMax = presentUs;
    return 0;
}

// Decode thread: demux + decode video into the ready-frame queue (and feed
// audio), as fast as the queue drains. Presentation/pacing happens on the main
// thread (render_threaded) so a heavy frame/GOP never hitches the screen.
static void *decode_thread_main(void *arg) {
    trace_mark("thread + decode self=%p", (void *)scePthreadSelf());
    (void)arg;
    while (!g_decStop) {
        if (g_seekPending) {
            if (seek_debounce_elapsed()) { apply_seek(); g_decEof = 0; }
            else sceKernelUsleep(5000);
            continue;
        }
        if (g_paused)      { sceKernelUsleep(8000); continue; }

        if (g_isHls && hls_take_variant_switch()) {
            // fMP4 quality change: reopen at the current position with the new
            // variant (it needs that variant's own init segment and a fresh demuxer).
            double cur = 0; player_progress(&cur, NULL);
            g_hlsResumeSec = cur > 1.0 ? cur : 0.0;
            g_liveRestartPending = 1;
            snprintf(g_status, sizeof(g_status), "switching quality");
            trace_mark("variant switch reopen at %.2f", cur);
            return NULL;
        }
        if (g_isHls) {
            int rgen = hls_reset_generation();
            if (rgen != g_hlsResetGen) {
                g_hlsResetGen = rgen;
                g_liveRestartPending = 1;
                snprintf(g_status, sizeof(g_status), "reopening live stream");
                return NULL;
            }
        }

        // Backpressure on the AUDIO ring. Without this the decode thread races
        // ahead (it is only gated by the video queue), fills the ring and
        // audio_write() DISCARDS the overflow — measured 0.70s of audio lost even
        // with an 8s ring, which desyncs A/V permanently. Waiting instead keeps
        // every sample. Bounded (~1s) and honours stop/seek so it can never
        // deadlock the way a frozen clock did in v03.61.
        for (int aguard = 0; aguard < 200; aguard++) {
            if (g_decStop || g_seekPending || g_paused) break;
            if (audio_fill_ms() < 6000) break;      // 6s of 8s ring: plenty of headroom
            sceKernelUsleep(5000);
        }

        int rc = av_read_frame(g_fmt, g_pkt);
        if (rc < 0) {
            if (g_preVideoCount > 0) prevideo_drain();
            if (g_useHw && g_roN > 0) ro_drain();   // flush remaining reordered frames
            g_decEof = 1; sceKernelUsleep(30000); continue;   // EOF: idle (resumable by seek)
        }

        g_pkts++;
        int sidx = g_pkt->stream_index;
        if (g_haveAudio && sidx == g_astream) {
            g_audioPkts++;
            decode_audio_pkt();
            if (g_isHls && hls_is_fmp4() && g_preVideoCount > 0)
                g_preVideoAudioAfter = 1;
            av_packet_unref(g_pkt);
            continue;
        }
        if (sidx != g_vstream) { av_packet_unref(g_pkt); continue; }
        g_videoPkts++;

        if (g_isHls && hls_is_fmp4() && g_haveAudio) {
            // MOV fragments are commonly laid out as a video run followed by an
            // audio run. At the first packet of the NEXT video run, release the
            // previous run: its complete audio cushion is already decoded.
            if (g_preVideoCount > 0 && g_preVideoAudioAfter &&
                !prevideo_drain()) {
                av_packet_unref(g_pkt);
                continue;
            }
            if (prevideo_stage(g_pkt)) { av_packet_unref(g_pkt); continue; }
            // Pathological fragment: preserve order and stay memory-bounded.
            if (!prevideo_drain()) { av_packet_unref(g_pkt); continue; }
            if (prevideo_stage(g_pkt)) { av_packet_unref(g_pkt); continue; }
        }

        decode_video_packet(g_pkt);
        av_packet_unref(g_pkt);
    }
    return NULL;
}

static int seg_ring_push(uint8_t *buf, int len, int gen) {
    scePthreadMutexLock(&g_srMtx);
    // Block while full by slot count OR by byte budget — but always allow the
    // first segment in (g_srCount == 0) so one oversized segment can't deadlock.
    while (!g_segFetchStop &&
           (g_srCount >= SEG_RING ||
            (g_srCount > 0 && g_srBytes + (long)len > SEG_BUDGET_BYTES)))
        scePthreadCondWait(&g_srNotFull, &g_srMtx);
    if (g_segFetchStop) { scePthreadMutexUnlock(&g_srMtx); return 0; }
    g_segRing[(g_srHead + g_srCount) % SEG_RING] = (SegSlot){ buf, len, gen };
    g_srCount++;
    g_srBytes += (long)len;
    scePthreadCondSignal(&g_srNotEmpty);
    scePthreadMutexUnlock(&g_srMtx);
    return 1;
}

// Blocks until a segment is ready. Returns 0 (out params set) or -1 on stop.
static int seg_ring_pop(uint8_t **buf, int *len, int *gen) {
    scePthreadMutexLock(&g_srMtx);
    while (!g_decStop && !g_seekPending && g_srCount == 0)
        scePthreadCondTimedwait(&g_srNotEmpty, &g_srMtx, 50 * 1000);
    if (g_seekPending || g_srCount == 0) { scePthreadMutexUnlock(&g_srMtx); return -1; }
    SegSlot s = g_segRing[g_srHead];
    g_segRing[g_srHead].buf = NULL;
    g_srHead = (g_srHead + 1) % SEG_RING;
    g_srCount--;
    g_srBytes -= (long)s.len;
    scePthreadCondSignal(&g_srNotFull);
    scePthreadMutexUnlock(&g_srMtx);
    *buf = s.buf; *len = s.len; *gen = s.gen;
    return 0;
}

static void seg_ring_flush(void) {
    scePthreadMutexLock(&g_srMtx);
    while (g_srCount > 0) {
        uint8_t *b = g_segRing[g_srHead].buf;
        if (b) free(b);
        g_segRing[g_srHead].buf = NULL;
        g_srHead = (g_srHead + 1) % SEG_RING;
        g_srCount--;
    }
    g_srHead = g_srCount = 0;
    g_srBytes = 0;
    scePthreadMutexUnlock(&g_srMtx);
}

static void *seg_fetch_thread_main(void *arg) {
    trace_mark("thread + segfetch self=%p", (void *)scePthreadSelf());
    (void)arg;
    while (!g_segFetchStop) {
        if (g_paused) { sceKernelUsleep(8000); continue; }
        uint8_t *buf = NULL; int len = 0, gen = 0;
        int rc = hls_next_segment(&buf, &len, &gen);
        if (g_segFetchStop) { if (buf) free(buf); break; }
        if (rc != 0 || !buf || len <= 0) { if (buf) free(buf); sceKernelUsleep(100000); continue; }
        if (!seg_ring_push(buf, len, gen)) { free(buf); break; }
    }
    return NULL;
}

// Bring up the read-ahead ring + fetch thread. On any failure, leaves
// g_segReadAhead = 0 so decode fetches inline (no read-ahead, still works).
static void seg_readahead_start(void) {
    // NEVER orphan a running fetch thread. This used to blindly zero g_segFetchUp
    // and g_segReadAhead and create a new thread; called again while one was live
    // (a fast channel-switch burst does exactly that), the old thread was never
    // joined AND its stop flag was reset to 0, so it looped forever -- refetching
    // the PREVIOUS channel and mutating the shared HLS globals (g_segIdx,
    // g_mediaSeq, refresh) underneath every channel tuned afterwards. That is why
    // a post-burst /chan did nothing: an orphan kept overwriting its state.
    // It also re-initialised g_srMtx while the orphan was still using it.
    seg_readahead_stop();
    g_srHead = g_srCount = 0; g_srBytes = 0; g_segFetchStop = 0; g_segReadAhead = 0; g_segFetchUp = 0;
    for (int i = 0; i < SEG_RING; i++) g_segRing[i].buf = NULL;
    if (scePthreadMutexInit(&g_srMtx, NULL, "ps4cast_sr") != 0) return;
    if (scePthreadCondInit(&g_srNotFull, NULL, "ps4cast_srnf") != 0) { scePthreadMutexDestroy(&g_srMtx); return; }
    if (scePthreadCondInit(&g_srNotEmpty, NULL, "ps4cast_srne") != 0) {
        scePthreadCondDestroy(&g_srNotFull); scePthreadMutexDestroy(&g_srMtx); return;
    }
    if (scePthreadCreate(&g_segFetchThread, NULL, seg_fetch_thread_main, NULL, "ps4cast_segf") != 0) {
        scePthreadCondDestroy(&g_srNotEmpty); scePthreadCondDestroy(&g_srNotFull); scePthreadMutexDestroy(&g_srMtx);
        return;
    }
    g_segFetchUp = 1; g_segReadAhead = 1;
}

// Stop + join the fetch thread and free buffered segments. Safe to call when the
// ring was never started (g_segReadAhead == 0).
static void seg_readahead_stop(void) {
    // Join whenever a thread exists, even if g_segReadAhead was cleared by a
    // re-entrant start -- otherwise the thread leaks and runs forever.
    if (!g_segReadAhead && !g_segFetchUp) return;
    g_segFetchStop = 1;
    aseg_abort();                       // unblock an in-flight segment fetch
    scePthreadMutexLock(&g_srMtx);
    scePthreadCondSignal(&g_srNotFull);
    scePthreadCondSignal(&g_srNotEmpty);
    scePthreadMutexUnlock(&g_srMtx);
    if (g_segFetchUp) { scePthreadJoin(g_segFetchThread, NULL); g_segFetchUp = 0; }
    seg_ring_flush();
    scePthreadCondDestroy(&g_srNotEmpty);
    scePthreadCondDestroy(&g_srNotFull);
    scePthreadMutexDestroy(&g_srMtx);
    g_segReadAhead = 0;
}

static int open_segment_demux(uint8_t *segBuf, int segLen, MemAvio *mem,
                              AVFormatContext **outFmt, AVIOContext **outAvio) {
    *outFmt = NULL; *outAvio = NULL;
    uint8_t *avioBuf = av_malloc(256 * 1024);
    if (!avioBuf) return -1;
    mem->buf = segBuf; mem->len = segLen; mem->pos = 0;
    AVIOContext *avio = avio_alloc_context(avioBuf, 256 * 1024, 0, mem, avio_mem_read_cb, NULL, NULL);
    if (!avio) { av_free(avioBuf); return -2; }
    avio->seekable = 0;
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) { av_freep(&avio->buffer); avio_context_free(&avio); return -3; }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // MUST stay small. This demuxer probes ONE finite segment, not a continuous
    // stream: raising it to 2MB/3s made the probe swallow the whole segment, so no
    // packets were left to decode and playback sat at fr=0 with an empty queue.
    // Audio-PID detection is handled by the MAIN demuxer's larger probe, not here.
    fmt->probesize = 512 * 1024;
    fmt->max_analyze_duration = 1 * (int64_t)AV_TIME_BASE;
    int rc = avformat_open_input(&fmt, "segment.ts", NULL, NULL);
    if (rc < 0) {
        avformat_free_context(fmt);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return rc;
    }
    avformat_find_stream_info(fmt, NULL);
    trace_mark("segdemux open ok streams=%d len=%d", (int)fmt->nb_streams, segLen);
    *outFmt = fmt;
    *outAvio = avio;
    return 0;
}

static void close_segment_demux(AVFormatContext **fmt, AVIOContext **avio) {
    if (*fmt) {
        // Custom AVIO is owned by us. Detach it before avformat_close_input so
        // libavformat cannot free or poke it during close on malformed/live TS.
        (*fmt)->pb = NULL;
        avformat_close_input(fmt);
    }
    if (*avio) {
        av_freep(&(*avio)->buffer);
        avio_context_free(avio);
    }
}

static void queue_sw_frame_us(AVRational tb) {
    for (;;) {
        int got = avcodec_receive_frame(g_vdec, g_frame);
        if (got == AVERROR(EAGAIN)) break;
        if (got < 0) { g_lastErr = got; break; }
        int64_t pts = g_frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = g_frame->pts;
        int64_t ptsUs = (pts == AV_NOPTS_VALUE) ? (int64_t)g_frames * 33333
                                                : (int64_t)(pts * av_q2d(tb) * 1000000.0);
        g_frames++;
        AVFrame *cl = av_frame_clone(g_frame);
        av_frame_unref(g_frame);
        if (!cl) continue;
        cl->pts = ptsUs;
        cl->best_effort_timestamp = ptsUs;
        if (!fq_push(cl)) break;
    }
}

static void *decode_segment_thread_main(void *arg) {
    trace_mark("thread + decode-seg self=%p", (void *)scePthreadSelf());
    (void)arg;
    while (!g_decStop) {
        if (g_seekPending) {
            if (seek_debounce_elapsed()) { apply_seek(); g_decEof = 0; }
            else sceKernelUsleep(5000);
            continue;
        }
        // Backpressure on the audio ring — the SAME guard decode_thread_main has.
        // Live HLS/IPTV runs through THIS loop, so without it the decoder races
        // ahead, the ring overflows and audio_write() discards samples, which
        // desyncs A/V permanently. Bounded (~1s) and honours stop/seek/pause so it
        // cannot deadlock.
        for (int aguard = 0; aguard < 200; aguard++) {
            if (g_decStop || g_seekPending || g_paused) break;
            if (audio_fill_ms() < 6000) break;
            sceKernelUsleep(5000);
        }
        if (g_paused) { sceKernelUsleep(8000); continue; }

        uint8_t *segBuf = NULL;
        int segLen = 0;
        int resetGen = 0;
        int rc;
        if (g_segReadAhead) rc = seg_ring_pop(&segBuf, &segLen, &resetGen);   // fetched ahead
        else                rc = hls_next_segment(&segBuf, &segLen, &resetGen); // inline fallback
        if (g_decStop) { if (segBuf) free(segBuf); break; }
        if (rc != 0 || !segBuf || segLen <= 0) {
            if (segBuf) free(segBuf);
            if (hls_at_eof()) g_decEof = 1;   // VOD finished: let the render path end playback
            sceKernelUsleep(100000); continue;
        }
        if (resetGen != g_hlsResetGen) {
            trace_mark("segdemux reset old=%d new=%d", g_hlsResetGen, resetGen);
            apply_hls_reset();   // flushes g_vdec/g_adec + audio + fq
            if (g_useHw) { ro_clear(); vdec_hw_reset(); g_lastEmitPts = AV_NOPTS_VALUE; }
            g_hlsResetGen = resetGen;
        }

        MemAvio mem;
        AVFormatContext *sfmt = NULL;
        AVIOContext *savio = NULL;
        if (open_segment_demux(segBuf, segLen, &mem, &sfmt, &savio) != 0 || !sfmt) {
            trace_mark("segdemux open fail len=%d", segLen);
            free(segBuf);
            continue;
        }

        int sv = av_find_best_stream(sfmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        int sa = g_haveAudio ? av_find_best_stream(sfmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) : -1;
        // av_find_best_stream needs a fully-classified decodable codec. On live
        // TS segments the AAC stream is sometimes not classified within the
        // analyze window and it returns STREAM_NOT_FOUND even though the PMT
        // lists an audio stream -> audio drops out for that segment. Fall back
        // to a manual codec_type scan, then to the last-known-good index.
        if (sv < 0) {
            for (unsigned i = 0; i < sfmt->nb_streams; i++)
                if (sfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { sv = (int)i; break; }
        }
        if (g_haveAudio && sa < 0) {
            for (unsigned i = 0; i < sfmt->nb_streams; i++)
                if (sfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { sa = (int)i; break; }
        }
        if (sv >= 0) g_segVideoIdx = sv;
        else if (g_segVideoIdx >= 0 && g_segVideoIdx < (int)sfmt->nb_streams) sv = g_segVideoIdx;
        if (sa >= 0) g_segAudioIdx = sa;
        else if (g_haveAudio && g_segAudioIdx >= 0 && g_segAudioIdx < (int)sfmt->nb_streams) sa = g_segAudioIdx;
        trace_mark("segdemux streams v=%d a=%d", sv, sa);
        if (g_useHw && sv >= 0)
            hw_reopen_for_params(sfmt->streams[sv]->codecpar);
        AVRational vtb = (sv >= 0) ? sfmt->streams[sv]->time_base : (AVRational){1, 90000};
        AVRational atb = (sa >= 0) ? sfmt->streams[sa]->time_base : (AVRational){1, 90000};
        int segPkts = 0, segVideo = 0, segAudio = 0;

        while (!g_decStop && !g_seekPending) {
            rc = av_read_frame(sfmt, g_pkt);
            if (rc < 0) break;
            g_pkts++;
            segPkts++;
            if (sa >= 0 && g_haveAudio && g_pkt->stream_index == sa) {
                g_audioPkts++; segAudio++;
                decode_audio_frame(g_pkt, atb);
                av_packet_unref(g_pkt);
                continue;
            }
            if (sv < 0 || g_pkt->stream_index != sv) { av_packet_unref(g_pkt); continue; }
            g_videoPkts++; segVideo++;
            if (g_useHw) {
                decode_video_hw_seg(g_pkt, vtb);
                av_packet_unref(g_pkt);
            } else {
                if (avcodec_send_packet(g_vdec, g_pkt) < 0) { av_packet_unref(g_pkt); continue; }
                av_packet_unref(g_pkt);
                queue_sw_frame_us(vtb);
            }
        }
        trace_mark("segdemux eof pkts=%d v=%d a=%d rc=%d fr=%ld q=%d", segPkts, segVideo, segAudio, rc, g_frames, g_fqCount);
        if (!g_useHw) queue_sw_frame_us(vtb);
        close_segment_demux(&sfmt, &savio);
        trace_mark("segdemux closed fr=%ld q=%d", g_frames, g_fqCount);
        free(segBuf);
    }
    return NULL;
}

// Present the due frame from the queue, synced to the audio clock (or wall clock
// if no audio). Drops earlier-due frames to hold realtime; holds the last frame
// when nothing new is due. Never blocks on decode/network.
// MEASURED DEAD END — do not retry this shape. Caching the NV12->RGB conversion
// in g_scaled and blitting it per present was SLOWER: fps 53 -> 38-40 with video
// drops climbing steadily. build_scaled_nv12_direct writes once, straight into
// the framebuffer; the cached form writes to scratch and then copies, so every
// NEW frame pays an extra full-frame write+read+write. Memory bandwidth is the
// bottleneck, and at a ~53fps loop on 30fps content the repeat presentations
// never outweigh that extra copy. A real win would have to skip the work
// ENTIRELY on repeats (per-framebuffer generation tracking), which needs care
// because overlays drawn into a buffer would otherwise go stale.

static int render_threaded(Gfx *g) {
    if (g_liveRestartPending) {
        char url[sizeof(g_playUrl)];
        strncpy(url, g_playUrl, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        g_liveRestartPending = 0;
        if (url[0]) player_play(url);
        return 0;
    }

    if (g_paused) {
        if (!g_wasPaused) { g_wasPaused = 1; g_pauseAt = sceKernelGetProcessTime(); }
        if (g_useHw && g_lastShown) { build_scaled_nv12_direct(g_lastShown, g); return 1; }
        if (g_gotFrame && g_scaled) { blit_scaled(g); return 1; }
        return 0;
    }
    if (g_wasPaused) { g_wasPaused = 0; if (g_startProc) g_startProc += sceKernelGetProcessTime() - g_pauseAt; }

    if (!g_gotFrame && g_rebuffering) {
        uint64_t elapsed = sceKernelGetProcessTime() - g_startupGateAt;
        int videoReady = g_fqCount >= HLS_START_FRAMES || g_decEof;
        // A short decoded-audio cushion prevents a pushed phone URL from
        // showing video before its audio clock exists. The timeout keeps odd or
        // silent streams from waiting forever.
        // HLS demux and decode share one producer thread. Starting with only
        // 250ms of audio made a full 50/60fps video queue hover at the 400ms
        // emergency threshold, where fq_push deliberately discards video to
        // reach the next audio packet. Build a real cushion before releasing
        // the clocks; steady-state production can then absorb segment/demux
        // jitter without trading pictures for sound.
        int startupAudioMs = g_isHls ? 1000 : 500;
        // Fragmented MOV commonly emits a long run of video samples before the
        // first audio burst. If presentation stays gated while that run fills
        // the frame queue, fq_push must discard pictures just to let demux reach
        // audio. Release the clocks only at near-full backpressure; normal HLS
        // still waits for its full audio cushion.
        int fmp4Backpressure = g_isHls && hls_is_fmp4() &&
                               g_fqCount >= FQ_SLOTS - 2;
        int audioReady = !g_haveAudio || audio_fill_ms() >= startupAudioMs ||
                         fmp4Backpressure || elapsed >= 2500000ULL || g_decEof;
        if ((!videoReady || !audioReady) && elapsed < 5000000ULL && !g_decEof)
            return 0;
        g_rebuffering = 0;
        if (!g_paused) audio_pause(0);
    }

    // Cache-pause: when the queue starves (network underrun), freeze the audio
    // clock — which holds video at the current time — and keep filling. Resume
    // once we've buffered REBUFFER_RESUME_SEC again (time-based, bitrate-aware).
    if (g_gotFrame) {
        if (g_fqCount == 0 && !g_decEof) g_emptyCnt++; else g_emptyCnt = 0;
        if (!g_rebuffering && g_emptyCnt >= 4) {
            g_rebuffering = 1; g_rebufTotal++; g_rebufHits++; audio_pause(1);
            // ABR: every rebuffer steps an HLS stream down a bitrate (downshift fast).
            if (g_isHls) hls_request_downshift();
        }
        if (g_rebuffering) {
            double ahead = g_isLocal ? 99.0
                         : ((g_bytesPerSec > 0) ? (double)httpsrc_ahead_bytes() / g_bytesPerSec : 99.0);
            int needFrames = g_gotFrame ? (FQ_SLOTS / 2) : HLS_START_FRAMES;
            int ready = g_isHls ? (g_fqCount >= needFrames)
                                : (g_fqCount >= FQ_SLOTS / 2 && ahead >= g_resumeSec);
            if (g_decEof || ready) {
                g_rebuffering = 0; g_emptyCnt = 0; audio_pause(0);
            }
        }
    }

    int useAudio = (g_haveAudio && audio_ok() && audio_has_clock() && !(g_sepAudioMode && g_sepAudioEof));
    int64_t clock;
    if (useAudio) clock = (int64_t)(audio_clock() * 1000000.0) - (int64_t)g_avSyncMs * 1000;
    else { uint64_t now = sceKernelGetProcessTime(); clock = g_gotFrame ? ((int64_t)(now - g_startProc) + g_startPts) : 0; }

    AVFrame *show = NULL; int64_t showPts = 0;
    scePthreadMutexLock(&g_fqMtx);
    while (g_fqCount > 0) {
        AVFrame *f = g_fq[g_fqHead].frame;
        int64_t pu = frame_pts_us(f);
        if (!g_gotFrame || pu <= clock) {
            if (show) { g_drops++; g_lateDrops++; av_frame_free(&show); } // earlier due frame
            show = f; showPts = pu;
            g_fq[g_fqHead].frame = NULL;
            g_fqHead = (g_fqHead + 1) % FQ_SLOTS; g_fqCount--;
            scePthreadCondSignal(&g_fqNotFull);
            if (!g_gotFrame) break;     // present the very first frame immediately
        } else break;                   // head not due yet
    }
    scePthreadMutexUnlock(&g_fqMtx);

    if (show) {
        if (!g_gotFrame) {
            g_gotFrame = 1; g_startProc = sceKernelGetProcessTime(); g_startPts = showPts;
            snprintf(g_status, sizeof(g_status), "playing %dx%d", show->width, show->height);
        }
        g_curSec = showPts / 1000000.0;
        g_lastLagUs = clock - showPts;   // >0 = presented frame is behind the clock
        g_shownGen++;
        if (g_useHw) {
            build_scaled_nv12_direct(show, g);
            if (g_lastShown) av_frame_free(&g_lastShown);
            g_lastShown = show;                 // keep ref to re-present on holds
        } else {
            build_scaled(show, g);
            blit_scaled(g);
            av_frame_free(&show);
        }
        return 1;
    }
    // EOF must win BEFORE the hold-frame return, otherwise we'd re-blit the last
    // frame forever and never mark playback finished (status stuck "playing").
    if (g_decEof && g_fqCount == 0) {
        if (g_active) {
            if (g_gotFrame) snprintf(g_status, sizeof(g_status), "finished");
            else player_set_error("no-frames", "The source opened, but no video frames could be decoded.");
        }
        g_active = 0;
        if (g_useHw && g_lastShown) { build_scaled_nv12_direct(g_lastShown, g); return 1; }
        if (g_gotFrame && g_scaled) { blit_scaled(g); return 1; }   // keep last frame on screen
        return 0;
    }
    if (g_useHw && g_lastShown) { build_scaled_nv12_direct(g_lastShown, g); return 1; }  // hold (HW)
    if (g_gotFrame && g_scaled) { blit_scaled(g); return 1; }   // hold last frame (SW)
    return 0;
}

// Decode the next video frame, pace it to its PTS, and blit. Returns 1 if a
// frame was drawn this call.
int player_render(Gfx *g) {
    if (!g_started || !g_fmt) return 0;
    if (g_threaded) return render_threaded(g);
    // Hardware decode requires the (big-stack) decode thread; there is no inline
    // hardware path, so just hold the last frame if the thread isn't running.
    if (g_useHw) { if (g_lastShown) { build_scaled_nv12_direct(g_lastShown, g); return 1; } return 0; }

    // Pause: hold on the last frame (re-blit so both buffers stay stable).
    if (g_paused) {
        if (!g_wasPaused) { g_wasPaused = 1; g_pauseAt = sceKernelGetProcessTime(); }
        if (g_gotFrame && g_scaled) { blit_scaled(g); return 1; }
        return 0;
    }
    if (g_wasPaused) {
        // Resuming: shift the pacing clock forward by the paused duration.
        g_wasPaused = 0;
        if (g_startProc) g_startProc += sceKernelGetProcessTime() - g_pauseAt;
    }

    if (g_seekPending) {
        if (seek_debounce_elapsed()) apply_seek();
        else return g_gotFrame && g_scaled ? (blit_scaled(g), 1) : 0;
    }

    for (;;) {
        int rc = av_read_frame(g_fmt, g_pkt);
        if (rc < 0) {                 // EOF or read error
            g_active = 0;
            if (g_gotFrame) snprintf(g_status, sizeof(g_status), "finished");
            else player_set_error("no-frames", "The source opened, but no video frames could be decoded.");
            return 0;
        }
        g_pkts++;
        int sidx = g_pkt->stream_index;
        if (g_haveAudio && sidx == g_astream) { g_audioPkts++; decode_audio_pkt(); av_packet_unref(g_pkt); continue; }
        if (sidx != g_vstream) { av_packet_unref(g_pkt); continue; }
        g_videoPkts++;

        if (avcodec_send_packet(g_vdec, g_pkt) < 0) { av_packet_unref(g_pkt); continue; }
        av_packet_unref(g_pkt);

        int got = avcodec_receive_frame(g_vdec, g_frame);
        if (got == AVERROR(EAGAIN)) continue;   // need more packets
        if (got < 0) { g_lastErr = got; continue; }

        g_frames++;

        int64_t pts = g_frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = g_frame->pts;
        AVRational tb = g_fmt->streams[g_vstream]->time_base;
        int64_t ptsUs = (pts == AV_NOPTS_VALUE) ? 0
                        : (int64_t)(pts * av_q2d(tb) * 1000000.0);
        g_curSec = ptsUs / 1000000.0;

        int useAudio = (g_haveAudio && audio_ok() && audio_has_clock() && !(g_sepAudioMode && g_sepAudioEof));
        if (!g_gotFrame) {
            g_startProc = sceKernelGetProcessTime(); g_startPts = ptsUs;
            g_gotFrame = 1;
            g_vdec->skip_frame = AVDISCARD_DEFAULT;
            snprintf(g_status, sizeof(g_status), "playing %dx%d", g_frame->width, g_frame->height);
        } else {
            // lag (us) > 0 = video is behind the master clock.
            int64_t lag;
            if (useAudio) lag = (int64_t)(audio_clock() * 1000000.0) - (int64_t)g_avSyncMs * 1000 - ptsUs;
            else          lag = (int64_t)sceKernelGetProcessTime() - ((int64_t)g_startProc + (ptsUs - g_startPts));
            g_lastLagUs = lag;

            // Auto/Smooth: escalate pre-decode dropping as we fall behind so a
            // heavy source (e.g. 1080p High ~10Mbps) holds realtime instead of
            // sliding into slow motion. NONREF drops B-frames; NONKEY skips whole
            // GOPs to the next keyframe for emergency catch-up.
            if      (lag > 1500000) g_vdec->skip_frame = AVDISCARD_NONKEY;
            else if (lag >  300000) g_vdec->skip_frame = AVDISCARD_NONREF;
            else                    g_vdec->skip_frame = AVDISCARD_DEFAULT;

            if (lag > 120000) {
                // Already late — drop to catch up (yield occasionally for input).
                g_drops++;
                if ((g_frames & 7) == 0) return 0;
                continue;
            }
            int64_t wait = -lag;   // video ahead of clock -> wait
            if (wait > 0 && wait < 2000000) sceKernelUsleep((unsigned)wait);
        }

        if (build_scaled(g_frame, g) == 0) { blit_scaled(g); return 1; }
        return 0;
    }
}
