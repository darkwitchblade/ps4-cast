// player_ff.c — ffmpeg-based player backend (software decode).
//
// Drop-in replacement for the libSceAvPlayer player.c (same player.h API),
// selected with USE_FFMPEG. ffmpeg is built with networking DISABLED, so input
// is fed through a custom AVIO backed by our sceNet reader (httpsrc) — which is
// also where TLS/https lives. This avoids the PS4 hardware-decoder entitlement
// wall that AvPlayer hit: ffmpeg decodes in software on the CPU.
#include "player.h"
#include "httpsrc.h"
#include "hls.h"
#include "aseg.h"
#include "audio.h"
#include "notify.h"
#include "vdec_hw.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
static AVBSFContext     *g_bsf = NULL;
static AVPacket         *g_hwPkt = NULL;
// Reorder buffer: the low-delay hardware decoder emits frames in DECODE order,
// so B-frames arrive after their display slot. We hold up to `video_delay`
// frames and release them in PTS (display) order. video_delay==0 (no B-frames)
// adds zero latency.
#define HW_REORDER 20
static AVFrame          *g_ro[HW_REORDER];
static int               g_roN = 0;
static int               g_hwReorder = 0;
static int64_t           g_lastEmitPts = AV_NOPTS_VALUE;  // adaptive reorder: grow window if a frame arrives late
static int               g_hwEnabled = 1;                 // runtime HW on/off (toggle via httpd for A/B testing)

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

static int   g_started = 0;        // intent: from play() until stop/EOF
static int   g_active  = 0;        // currently decoding
static int   g_gotFrame = 0;
static char  g_status[160] = "idle";
static char  g_playUrl[2048];

static int64_t  g_pos = 0;         // byte cursor for AVIO
static uint64_t g_startProc = 0;   // wall clock at first presented frame (us)
static int64_t  g_startPts  = 0;   // pts of first frame (us)

// transport control (set from http thread, applied in player_render)
static volatile int    g_paused = 0;
static int             g_wasPaused = 0;
static uint64_t        g_pauseAt = 0;
static volatile int    g_seekPending = 0;
static volatile double g_seekTo = 0;
static double          g_curSec = 0;   // current playback position
static double          g_durSec = 0;   // total duration

// debug counters
static long g_pkts = 0, g_frames = 0;
static long g_drops = 0, g_audioPkts = 0, g_videoPkts = 0;
static int  g_lastErr = 0;
static int64_t g_lastLagUs = 0;
static int  g_isHls = 0;   // HLS (m3u8): non-seekable concatenated segments
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
#define HLS_START_FRAMES 16

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
static volatile int      g_rebuffering = 0;    // cache-pause: holding for buffer
static int               g_emptyCnt = 0;       // debounce queue-empty
static int               g_rebufHits = 0;      // consecutive rebuffers (for HLS ABR)
static int               g_rebufTotal = 0;     // total rebuffers this playback (telemetry)
static double            g_bytesPerSec = 0;    // bitrate estimate for time-based buffering
static double            g_resumeSec = 2.0;    // buffered-seconds needed to resume (LAN vs remote)

static void  fq_flush(void);
static void  ro_clear(void);
static void  apply_hls_reset(void);
static void  seg_readahead_start(void);
static void  seg_readahead_stop(void);

// ---- live HLS segment read-ahead -----------------------------------------
// A dedicated fetch thread pulls TS segments ahead of decode into a small ring,
// so the decode thread never blocks on the ~1.4s per-segment network fetch.
// Raw segments are ~1MB each (far cheaper to buffer than decoded frames). The
// ring depth is the "headstart" that rides over fetch-latency dips. If the
// thread/ring fails to start, decode falls back to fetching inline.
#define SEG_RING 3
typedef struct { uint8_t *buf; int len; int gen; } SegSlot;
static SegSlot           g_segRing[SEG_RING];
static int               g_srHead, g_srCount;
static OrbisPthreadMutex g_srMtx;
static OrbisPthreadCond  g_srNotFull, g_srNotEmpty;
static OrbisPthread      g_segFetchThread;
static volatile int      g_segFetchStop;
static int               g_segFetchUp;       // fetch thread joined-state tracking
static int               g_segReadAhead;     // 1 = decode pops ring; 0 = inline fetch
static void *decode_segment_thread_main(void *arg);
static void  present_pool_start(void);
static void  present_pool_stop(void);
static void *decode_thread_main(void *arg);
static int   setup_separate_audio(void);
static void *audio_thread_main(void *arg);

// ---- custom AVIO: plain file/stream via httpsrc, or HLS via hls ------------
static int avio_read_cb(void *o, uint8_t *buf, int size) {
    (void)o;
    int n;
    if (g_isHls) {
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
    uint64_t sz = httpsrc_size();
    if (whence == AVSEEK_SIZE) return sz ? (int64_t)sz : -1;
    if (whence == SEEK_SET)      g_pos = off;
    else if (whence == SEEK_CUR) g_pos += off;
    else if (whence == SEEK_END) g_pos = (int64_t)sz + off;
    else return -1;
    return g_pos;
}

const char *player_status(void) { return g_status; }
int player_init(void) { return 0; } // nothing global to set up for ffmpeg

void player_stop(void) {
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
        seg_readahead_stop();           // join fetch thread, free buffered segs, destroy ring
        fq_flush();
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
    if (g_isHls) hls_close(); else httpsrc_close();
    g_isHls = 0;
    g_hlsSegDemux = 0;
    g_vstream = -1; g_active = 0; g_started = 0; g_gotFrame = 0;
    g_pos = 0; g_startProc = 0; g_scaledW = g_scaledH = 0;
    g_paused = 0; g_wasPaused = 0; g_seekPending = 0; g_curSec = 0; g_durSec = 0;
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
    char startUrl[2048];
    snprintf(startUrl, sizeof(startUrl), "%s", url ? url : "");
    player_stop();
    strncpy(g_playUrl, startUrl, sizeof(g_playUrl) - 1);
    g_playUrl[sizeof(g_playUrl) - 1] = '\0';
    g_pkts = g_frames = g_drops = g_audioPkts = g_videoPkts = 0; g_lastErr = 0; g_lastLagUs = 0;

    // Open the source. HLS (.m3u8) goes through the segment-streaming layer;
    // everything else (mp4/mov/mkv/avi/ts/... over http/https) via httpsrc.
    g_isHls = hls_is_url(startUrl);
    int orc = g_isHls ? hls_open(startUrl) : httpsrc_open(startUrl);
    if (orc != 0) {
        snprintf(g_status, sizeof(g_status), "source: %s", g_isHls ? hls_debug() : httpsrc_debug());
        g_isHls = 0;
        return -1;
    }
    g_pos = 0;

    uint8_t *aviobuf = av_malloc(AVIO_BUFSZ);
    if (!aviobuf) { snprintf(g_status, sizeof(g_status), "oom avio"); return -1; }
    g_avio = avio_alloc_context(aviobuf, AVIO_BUFSZ, 0, NULL, avio_read_cb, NULL, avio_seek_cb);
    if (!g_avio) { av_free(aviobuf); snprintf(g_status, sizeof(g_status), "oom avioctx"); return -1; }
    if (g_isHls) g_avio->seekable = 0;  // concatenated segment stream

    g_fmt = avformat_alloc_context();
    g_fmt->pb = g_avio;
    g_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Probe further so the audio stream in a concatenated MPEG-TS (HLS) is
    // reliably detected — the default probe can stop before the audio PID and
    // leave HLS playing silently.
    g_fmt->probesize = 8 * 1024 * 1024;
    g_fmt->max_analyze_duration = 6 * (int64_t)AV_TIME_BASE;

    int rc = avformat_open_input(&g_fmt, "stream", NULL, NULL);
    if (rc < 0) {
        snprintf(g_status, sizeof(g_status), "demux open failed %d", rc);
        player_stop(); return -2;
    }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) {
        snprintf(g_status, sizeof(g_status), "no stream info");
        player_stop(); return -3;
    }

    const AVCodec *dec = NULL;
    g_vstream = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (g_vstream < 0 || !dec) {
        snprintf(g_status, sizeof(g_status), "no video stream");
        player_stop(); return -4;
    }

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
                    notify("PS4 Cast: audio %s %dHz", adec->name, g_adec->sample_rate);
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
    // Hardware H.264: direct MP4 (needs the mp4->annexb bitstream filter) or live
    // HLS via the segment-demux path (MPEG-TS packets are already annex-b, so no
    // bsf). The plain AVIO-concatenated HLS path (no seg-demux) stays software.
    int hwEligible = g_hwEnabled && vpar->codec_id == AV_CODEC_ID_H264 &&
                     (!g_isHls || g_hlsSegDemux);
    if (hwEligible) {
        int bsfOk = 1;
        if (!g_isHls) {     // MP4/AVCC source -> convert to annex-b for the decoder
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
                g_roN = 0; g_lastEmitPts = AV_NOPTS_VALUE;
                g_hwReorder = vpar->video_delay;          // B-frame reorder depth (adapts at runtime)
                if (g_hwReorder < 0) g_hwReorder = 0;
                if (g_hwReorder >= HW_REORDER) g_hwReorder = HW_REORDER - 1;
                notify("PS4 Cast: HW H.264 %dx%d reorder=%d %s (%s)", vpar->width, vpar->height,
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
            snprintf(g_status, sizeof(g_status), "decoder open failed (%s)", dec->name);
            player_stop(); return -5;
        }
    }

    g_frame = av_frame_alloc();
    g_pkt   = av_packet_alloc();
    g_srcW = g_useHw ? vpar->width  : g_vdec->width;
    g_srcH = g_useHw ? vpar->height : g_vdec->height;
    g_durSec = (g_fmt->duration > 0) ? (double)g_fmt->duration / AV_TIME_BASE : 0;
    g_hlsSegGen = g_isHls ? hls_generation() : 0;
    g_hlsResetGen = g_isHls ? hls_reset_generation() : 0;
    g_hlsSegDemux = g_isHls && hls_can_segment_demux();
    g_bytesPerSec = (g_fmt->bit_rate > 0) ? (double)g_fmt->bit_rate / 8.0
                  : (g_durSec > 0 && httpsrc_size() > 0) ? (double)httpsrc_size() / g_durSec
                  : 0;
    // Bigger cushion for public/CDN links (4s) than LAN (1.5s) — 2s was too
    // small for remote HTTPS streams.
    g_resumeSec = (!g_isHls && httpsrc_is_lan()) ? 1.5 : 4.0;
    g_rebuffering = g_isHls ? 1 : 0;
    g_emptyCnt = 0; g_rebufHits = 0; g_rebufTotal = 0;

    g_started = 1; g_active = 1; g_gotFrame = 0;
    if (g_isHls) audio_pause(1);  // startup headstart: fill frames/audio before first presentation
    snprintf(g_status, sizeof(g_status), "buffering %s %dx%d", dec->name, g_srcW, g_srcH);
    notify("PS4 Cast: ffmpeg %s %dx%d", dec->name, g_srcW, g_srcH);

#if PLAYER_DECODE_THREAD
    // Start the decode thread; main thread will only present. On failure fall
    // back to the single-thread decode-and-present path (g_threaded stays 0).
    g_threaded = 0; g_decStop = 0; g_decEof = 0; g_fqHead = g_fqCount = 0;
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

void player_pause(int paused) {
    g_paused = paused ? 1 : 0;
    audio_pause(g_paused);
}
int  player_is_paused(void)   { return g_paused; }
void player_seek(double seconds) {
    if (seconds < 0) seconds = 0;
    if (g_durSec > 0 && seconds > g_durSec) seconds = g_durSec;
    g_seekTo = seconds;
    g_seekPending = 1;
}
// Unblock a stuck network read (called from the http thread on Stop / new cast)
// so an underrun stall never traps the app.
void player_interrupt(void) {
    if (!g_started && !g_active) return;
    httpsrc_abort();
    hls_audio_abort();
}

// True when we've started playing but no fresh frame is available — i.e. the
// decoder/network can't keep up. Drives the on-screen "Buffering" indicator.
int player_buffering(void) {
    if (!g_started || g_paused) return 0;
    if (g_threaded) return g_rebuffering;
    return 0;   // single-thread path blocks instead of reporting
}
int player_buffer_pct(void) { return httpsrc_fill_pct(); }

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

// Apply a pending seek. In threaded mode this runs on the decode thread; in
// inline mode on the render thread. Either way the ffmpeg context is owned by
// the caller.
static void apply_seek(void) {
    double sec = g_seekTo;
    g_seekPending = 0;
    int64_t ts = (int64_t)(sec * AV_TIME_BASE);
    if (av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        fq_flush();
        if (g_vdec) avcodec_flush_buffers(g_vdec);
        if (g_useHw && g_bsf) av_bsf_flush(g_bsf);   // next AU after seek is a keyframe
        if (g_useHw) { ro_clear(); vdec_hw_reset(); g_lastEmitPts = AV_NOPTS_VALUE; }  // fresh refs
        if (g_adec) avcodec_flush_buffers(g_adec);
        if (g_swr) { swr_close(g_swr); swr_init(g_swr); }
        if (g_haveAudio) {
            audio_flush();
            audio_reset_base(sec);
        }
        g_sepAudioEof = 0;
        g_gotFrame = 0;          // re-anchor the pacing clock on the next frame
        g_curSec = sec;
        g_active = 1;
        snprintf(g_status, sizeof(g_status), "seeking %.0fs", sec);
    }
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
    g_decEof = 0;
    g_lastLagUs = 0;
    snprintf(g_status, sizeof(g_status), "buffering live");
}

void player_debug(char *out, int len) {
    double ahead = (g_bytesPerSec > 0) ? (double)httpsrc_ahead_bytes() / g_bytesPerSec : 0;
    snprintf(out, len,
             "ff%s%s%s %dx%d | fr=%ld drop=%ld q=%d/%d ra=%d/%d rb=%d ahead=%.1fs lag=%lldms er=%d | as=%d%s%s %s | %s",
             g_useHw ? "/HW" : "", g_isHls ? (g_hlsSegDemux ? "/hls-seg" : "/hls") : "", g_threaded ? "/T" : "", g_srcW, g_srcH,
             g_frames, g_drops, g_fqCount, FQ_SLOTS, g_srCount, SEG_RING, g_rebufTotal, ahead,
             (long long)(g_lastLagUs / 1000), g_lastErr,
             g_sepAudioMode ? g_aastream : g_astream, g_sepAudioMode ? "/sep" : "",
             (g_sepAudioMode && g_sepAudioEof) ? "/eof" : "",
             audio_debug(),
             g_isHls ? hls_debug() : httpsrc_debug());
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
    if (scaledW != g_scaledW || scaledH != g_scaledH || !g_scaled) {
        free(g_scaled);
        g_scaled = malloc((size_t)scaledW * scaledH * 4);
        if (!g_scaled) return -1;
        g_scaledW = scaledW; g_scaledH = scaledH;
        if (g_sws) { sws_freeContext(g_sws); g_sws = NULL; }
    }
    // Rebuild sws if the SOURCE geometry/format changed too (not just the output)
    // — otherwise a discontinuity-driven resolution change reads with wrong strides.
    if (g_sws && (sw != g_swsSrcW || sh != g_swsSrcH || (int)srcFmt != g_swsSrcFmt)) {
        sws_freeContext(g_sws); g_sws = NULL;
    }
    if (!g_sws) {
        // FAST_BILINEAR: the software present (HLS path) was dropping frames because
        // the single-threaded 720p->1080p upscale couldn't sustain 30fps. Fast
        // bilinear is materially cheaper at near-identical quality for upscales.
        g_sws = sws_getContext(sw, sh, srcFmt,
                               scaledW, scaledH, AV_PIX_FMT_BGRA,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g_sws) return -1;
        g_swsSrcW = sw; g_swsSrcH = sh; g_swsSrcFmt = (int)srcFmt;
    }
    uint8_t *dst[4] = { g_scaled, NULL, NULL, NULL };
    int dstStride[4] = { g_scaledW * 4, 0, 0, 0 };
    sws_scale(g_sws, (const uint8_t * const *)fr->data, fr->linesize,
              0, sh, dst, dstStride);
    return 0;
}

static int64_t frame_pts_us(AVFrame *f) {
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = f->pts;
    if (pts == AV_NOPTS_VALUE) return 0;
    if (g_hlsSegDemux) return pts;   // segment demux stores queue PTS directly in us
    return (int64_t)(pts * av_q2d(g_fmt->streams[g_vstream]->time_base) * 1000000.0);
}

static void blit_scaled(Gfx *g) {
    int dw = g->width, dh = g->height;
    int ox = (dw - g_scaledW) / 2, oy = (dh - g_scaledH) / 2;
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
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
        if (apts != AV_NOPTS_VALUE) audio_set_base((double)apts * av_q2d(atb));
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
    notify("PS4 Cast: audio %s %dHz (HLS rendition)", adec->name, g_adec->sample_rate);
    return 0;
}

// Push a ready frame onto the queue (blocks if full; frees it and returns 0 if
// stopping/seeking). Shared by the software and hardware decode paths.
static int fq_push(AVFrame *cl) {
    scePthreadMutexLock(&g_fqMtx);
    while (!g_decStop && !g_seekPending && g_fqCount >= FQ_SLOTS)
        scePthreadCondWait(&g_fqNotFull, &g_fqMtx);
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
    g_lastEmitPts = f->pts;
    fq_push(f);                         // frees f if stopping
}
static void ro_drain(void) { while (g_roN > 0) ro_emit_one(); }     // EOF: flush in order
static void ro_clear(void) { for (int i = 0; i < g_roN; i++) av_frame_free(&g_ro[i]); g_roN = 0; }

// Hardware H.264 path: convert the packet to Annex B, decode each access unit on
// the GPU silicon into NV12, copy it into an AVFrame, and feed it through the
// reorder buffer -> queue -> sync/scale/blit path (sws converts NV12->BGRA).
static void decode_video_hw(AVPacket *pkt) {
    if (av_bsf_send_packet(g_bsf, pkt) < 0) return;
    while (av_bsf_receive_packet(g_bsf, g_hwPkt) == 0) {
        VdecHwFrame hf;
        int got = vdec_hw_decode(g_hwPkt->data, g_hwPkt->size, g_hwPkt->pts, g_hwPkt->dts, &hf);
        av_packet_unref(g_hwPkt);
        if (got != 1) continue;
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

// HLS segment-demux hardware path: MPEG-TS packets are already annex-b (SPS/PPS
// in-band, one access unit per packet), so feed them straight to the GPU decoder
// — no bitstream filter. Unlike the direct path, the queue stores PTS in
// microseconds (frame_pts_us returns it verbatim for seg-demux), so convert from
// the segment time_base here before the reorder buffer.
static void decode_video_hw_seg(AVPacket *pkt, AVRational vtb) {
    VdecHwFrame hf;
    int got = vdec_hw_decode(pkt->data, pkt->size, pkt->pts, pkt->dts, &hf);
    if (got != 1) return;
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
} PresentJob;

static OrbisPthread       g_pwTh[PRESENT_WORKERS];
static int                g_pwStarted[PRESENT_WORKERS];   // which slots actually launched
static OrbisPthreadMutex  g_pwMtx;
static OrbisPthreadCond   g_pwGo, g_pwDone;
static volatile int       g_pwGen, g_pwDoneCount, g_pwStop, g_pwUp;
static PresentJob         g_pwJob;

// BT.709 limited-range YUV->RGB, integer fixed-point, nearest-neighbour scale.
static void convert_band(const PresentJob *j, int b0, int b1) {
    for (int oy = b0; oy < b1; oy++) {
        int srcY = (int)((int64_t)oy * j->sh / j->scaledH);
        if (srcY >= j->sh) srcY = j->sh - 1;
        const uint8_t *yrow  = j->y  + (size_t)srcY * j->ypitch;
        const uint8_t *uvrow = j->uv + (size_t)(srcY >> 1) * j->uvpitch;
        uint32_t *out = j->dst + (size_t)(j->oy + oy) * j->dstPitch + j->ox;
        for (int ox = 0; ox < j->scaledW; ox++) {
            int srcX = (int)((int64_t)ox * j->sw / j->scaledW);
            if (srcX >= j->sw) srcX = j->sw - 1;
            int uvx = srcX & ~1;
            int c = (yrow[srcX] - 16) * 298;
            int U = uvrow[uvx] - 128, V = uvrow[uvx + 1] - 128;
            int R = (c + 459 * V) >> 8;
            int G = (c - 55 * U - 136 * V) >> 8;
            int B = (c + 541 * U) >> 8;
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            out[ox] = 0x80000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
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

// NV12 -> g_scaled (BGRA), parallel when the pool is up, serial otherwise.
static int build_scaled_nv12(AVFrame *fr, Gfx *g) {
    int dw = g->width, dh = g->height, sw = fr->width, sh = fr->height;
    if (sw <= 0 || sh <= 0) return -1;
    int scaledW = dw, scaledH = (int)((int64_t)dw * sh / sw);
    if (scaledH > dh) { scaledH = dh; scaledW = (int)((int64_t)dh * sw / sh); }
    if (scaledW < 1) scaledW = 1; if (scaledH < 1) scaledH = 1;
    if (scaledW != g_scaledW || scaledH != g_scaledH || !g_scaled) {
        free(g_scaled);
        g_scaled = malloc((size_t)scaledW * scaledH * 4);
        if (!g_scaled) { g_scaledW = g_scaledH = 0; return -1; }
        g_scaledW = scaledW; g_scaledH = scaledH;
        if (g_sws) { sws_freeContext(g_sws); g_sws = NULL; }
    }
    PresentJob job = { fr->data[0], fr->data[1], sw, sh, fr->linesize[0], fr->linesize[1],
                       (uint32_t *)g_scaled, scaledW, 0, 0, scaledW, scaledH };
    if (g_pwUp) {
        scePthreadMutexLock(&g_pwMtx);
        g_pwJob = job; g_pwDoneCount = 0; g_pwGen++;
        scePthreadCondBroadcast(&g_pwGo);
        while (g_pwDoneCount < PRESENT_WORKERS) scePthreadCondWait(&g_pwDone, &g_pwMtx);
        scePthreadMutexUnlock(&g_pwMtx);
    } else {
        convert_band(&job, 0, scaledH);
    }
    return 0;
}

// NV12 -> active framebuffer directly. This fuses color conversion, scaling, and
// final blit for the hardware path.
static int build_scaled_nv12_direct(AVFrame *fr, Gfx *g) {
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
    if (scaledW != dw || scaledH != dh) {
        for (int i = 0, n = dw * dh; i < n; i++) fb[i] = 0x80000000u;
    }

    PresentJob job = { fr->data[0], fr->data[1], sw, sh, fr->linesize[0], fr->linesize[1],
                       fb, dw, ox, oy, scaledW, scaledH };
    if (g_pwUp) {
        scePthreadMutexLock(&g_pwMtx);
        g_pwJob = job; g_pwDoneCount = 0; g_pwGen++;
        scePthreadCondBroadcast(&g_pwGo);
        while (g_pwDoneCount < PRESENT_WORKERS) scePthreadCondWait(&g_pwDone, &g_pwMtx);
        scePthreadMutexUnlock(&g_pwMtx);
    } else {
        convert_band(&job, 0, scaledH);
    }
    return 0;
}

// Decode thread: demux + decode video into the ready-frame queue (and feed
// audio), as fast as the queue drains. Presentation/pacing happens on the main
// thread (render_threaded) so a heavy frame/GOP never hitches the screen.
static void *decode_thread_main(void *arg) {
    (void)arg;
    while (!g_decStop) {
        if (g_seekPending) { apply_seek(); g_decEof = 0; }
        if (g_paused)      { sceKernelUsleep(8000); continue; }

        if (g_isHls) {
            int rgen = hls_reset_generation();
            if (rgen != g_hlsResetGen) {
                g_hlsResetGen = rgen;
                g_liveRestartPending = 1;
                snprintf(g_status, sizeof(g_status), "reopening live stream");
                return NULL;
            }
        }

        int rc = av_read_frame(g_fmt, g_pkt);
        if (rc < 0) {
            if (g_useHw && g_roN > 0) ro_drain();   // flush remaining reordered frames
            g_decEof = 1; sceKernelUsleep(30000); continue;   // EOF: idle (resumable by seek)
        }

        g_pkts++;
        int sidx = g_pkt->stream_index;
        if (g_haveAudio && sidx == g_astream) { g_audioPkts++; decode_audio_pkt(); av_packet_unref(g_pkt); continue; }
        if (sidx != g_vstream) { av_packet_unref(g_pkt); continue; }
        g_videoPkts++;

        if (g_useHw) {
            if (g_isHls) {
                int gen = hls_generation();
                if (gen != g_hlsSegGen) {
                    ro_clear();
                    if (g_bsf) av_bsf_flush(g_bsf);
                    vdec_hw_reset();
                    g_lastEmitPts = AV_NOPTS_VALUE;
                    g_hlsSegGen = gen;
                }
            }
            decode_video_hw(g_pkt); av_packet_unref(g_pkt); continue;
        }

        if (avcodec_send_packet(g_vdec, g_pkt) < 0) { av_packet_unref(g_pkt); continue; }
        av_packet_unref(g_pkt);

        for (;;) {
            int got = avcodec_receive_frame(g_vdec, g_frame);
            if (got == AVERROR(EAGAIN)) break;
            if (got < 0) { g_lastErr = got; break; }
            g_frames++;
            AVFrame *cl = av_frame_clone(g_frame);
            av_frame_unref(g_frame);
            if (!cl) continue;
            if (!fq_push(cl)) break;
        }
    }
    return NULL;
}

static int seg_ring_push(uint8_t *buf, int len, int gen) {
    scePthreadMutexLock(&g_srMtx);
    while (!g_segFetchStop && g_srCount >= SEG_RING)
        scePthreadCondWait(&g_srNotFull, &g_srMtx);
    if (g_segFetchStop) { scePthreadMutexUnlock(&g_srMtx); return 0; }
    g_segRing[(g_srHead + g_srCount) % SEG_RING] = (SegSlot){ buf, len, gen };
    g_srCount++;
    scePthreadCondSignal(&g_srNotEmpty);
    scePthreadMutexUnlock(&g_srMtx);
    return 1;
}

// Blocks until a segment is ready. Returns 0 (out params set) or -1 on stop.
static int seg_ring_pop(uint8_t **buf, int *len, int *gen) {
    scePthreadMutexLock(&g_srMtx);
    while (!g_decStop && g_srCount == 0)
        scePthreadCondWait(&g_srNotEmpty, &g_srMtx);
    if (g_srCount == 0) { scePthreadMutexUnlock(&g_srMtx); return -1; }
    SegSlot s = g_segRing[g_srHead];
    g_segRing[g_srHead].buf = NULL;
    g_srHead = (g_srHead + 1) % SEG_RING;
    g_srCount--;
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
    scePthreadMutexUnlock(&g_srMtx);
}

static void *seg_fetch_thread_main(void *arg) {
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
    g_srHead = g_srCount = 0; g_segFetchStop = 0; g_segReadAhead = 0; g_segFetchUp = 0;
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
    if (!g_segReadAhead) return;
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
    (void)arg;
    while (!g_decStop) {
        if (g_paused) { sceKernelUsleep(8000); continue; }

        uint8_t *segBuf = NULL;
        int segLen = 0;
        int resetGen = 0;
        int rc;
        if (g_segReadAhead) rc = seg_ring_pop(&segBuf, &segLen, &resetGen);   // fetched ahead
        else                rc = hls_next_segment(&segBuf, &segLen, &resetGen); // inline fallback
        if (g_decStop) { if (segBuf) free(segBuf); break; }
        if (rc != 0 || !segBuf || segLen <= 0) { if (segBuf) free(segBuf); sceKernelUsleep(100000); continue; }
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
        AVRational vtb = (sv >= 0) ? sfmt->streams[sv]->time_base : (AVRational){1, 90000};
        AVRational atb = (sa >= 0) ? sfmt->streams[sa]->time_base : (AVRational){1, 90000};
        int segPkts = 0, segVideo = 0, segAudio = 0;

        while (!g_decStop) {
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

    if (g_isHls && !g_gotFrame && g_rebuffering) {
        if (g_fqCount < HLS_START_FRAMES && !g_decEof) return 0;
        g_rebuffering = 0;
        audio_pause(0);
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
            double ahead = (g_bytesPerSec > 0) ? (double)httpsrc_ahead_bytes() / g_bytesPerSec : 99.0;
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
    if (useAudio) clock = (int64_t)(audio_clock() * 1000000.0);
    else { uint64_t now = sceKernelGetProcessTime(); clock = g_gotFrame ? ((int64_t)(now - g_startProc) + g_startPts) : 0; }

    AVFrame *show = NULL; int64_t showPts = 0;
    scePthreadMutexLock(&g_fqMtx);
    while (g_fqCount > 0) {
        AVFrame *f = g_fq[g_fqHead].frame;
        int64_t pu = frame_pts_us(f);
        if (!g_gotFrame || pu <= clock) {
            if (show) { g_drops++; av_frame_free(&show); }   // drop an earlier-due frame
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
        if (g_active) snprintf(g_status, sizeof(g_status), g_gotFrame ? "finished" : "no frames decoded");
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

    if (g_seekPending) apply_seek();

    for (;;) {
        int rc = av_read_frame(g_fmt, g_pkt);
        if (rc < 0) {                 // EOF or read error
            g_active = 0;
            snprintf(g_status, sizeof(g_status), g_gotFrame ? "finished" : "no frames decoded");
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
            if (useAudio) lag = (int64_t)(audio_clock() * 1000000.0) - ptsUs;
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
