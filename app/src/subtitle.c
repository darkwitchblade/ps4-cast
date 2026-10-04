#include "subtitle.h"
#include "subs.h"
#include "lang.h"
#include "hls.h"
#include "hls_parse.h"
#include "aseg.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <orbis/libkernel.h>

// Everything shared between threads (tracks, selection, cue lists, the fetch
// job) is under one spinlock, held only for copies -- never across a fetch,
// a decode or drawing. Threads: the opener worker (open/close), the decode
// thread (packets, seeks), the fetcher (external/HLS files), httpd (control)
// and main (clock, drawing).
static volatile int g_lock;
static void lk(void)  { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(20); }
static void ulk(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

typedef struct {
    SubTrack t;
    int stream, pid, codec;          // embedded: stream index, TS PID, AVCodecID
    int rend;                        // HLS: rendition index
} Track;

static Track   g_tracks[SUB_MAX_TRACKS];
static int     g_trackN;
static int     g_cur = -1;           // shown track id, -1 = off
static char    g_spec[2048];         // source the choice and the external file belong to
static int     g_pickId = -1;        // the user's choice for g_spec (survives reopens)
static char    g_pickName[64], g_pickLang[16];
static char    g_langPref[16];       // Settings: auto-select this language
static int64_t g_startUs;            // media timeline start; external cues are relative to it

static SubList g_ext;                // external file (kept across reopens of the same source)
static char    g_extName[64];
static int     g_haveExt;
static SubList g_live;               // embedded / HLS cues of the shown track
static char    g_status[128] = "";

// Decode thread.
static volatile int g_applyPending;
static int      g_routeStream = -1, g_routePid = -1, g_routeCodec = 0;
static AVCodecContext *g_bdec;       // bitmap decoder (PGS/DVB/DVD)
static int      g_bdecCodec, g_bdecPid;

// Fetcher.
enum { JOB_NONE, JOB_EXTERNAL, JOB_HLS };
static volatile unsigned g_jobGen;
static int      g_jobKind;
static char     g_jobUrl[2048];
static volatile int g_seekFlag;

// Main thread.
static volatile int64_t g_clockUs;
static char     g_pendSpec[2048], g_pendUrl[2048];   // sub_load_url_for before its source opened
static int      g_vx, g_vy, g_vw, g_vh;

static int is_text_codec(int c) {
    return c == AV_CODEC_ID_SUBRIP || c == AV_CODEC_ID_TEXT || c == AV_CODEC_ID_ASS ||
           c == AV_CODEC_ID_SSA || c == AV_CODEC_ID_MOV_TEXT || c == AV_CODEC_ID_WEBVTT;
}
static int is_bitmap_codec(int c) {
    return c == AV_CODEC_ID_HDMV_PGS_SUBTITLE || c == AV_CODEC_ID_DVB_SUBTITLE ||
           c == AV_CODEC_ID_DVD_SUBTITLE;
}

static const Track *find_track(int id) {
    for (int i = 0; i < g_trackN; i++) if (g_tracks[i].t.id == id) return &g_tracks[i];
    return NULL;
}

// ---- selection (caller holds the lock) -------------------------------------
static void start_job(int kind, const char *url) {
    g_jobKind = kind;
    snprintf(g_jobUrl, sizeof(g_jobUrl), "%s", url ? url : "");
    g_jobGen++;
}

static void select_locked(int id) {
    const Track *tr = find_track(id);
    if (id != -1 && !tr) id = -1;
    g_cur = id;
    subs_list_clear(&g_live);
    g_applyPending = 1;
    if (tr && id >= 100 && id < SUB_ID_EXTERNAL) {
        HlsSubRendition r[HLS_MAX_SUB_RENDITIONS];
        int n = hls_subtitle_renditions(r, HLS_MAX_SUB_RENDITIONS);
        if (tr->rend < n) start_job(JOB_HLS, r[tr->rend].uri);
    } else if (g_jobKind == JOB_HLS) {
        start_job(JOB_NONE, NULL);
    }
}

// ---- tracks ------------------------------------------------------------------
static int add_track(int id, const char *label, const char *lang) {
    if (g_trackN >= SUB_MAX_TRACKS) return -1;
    Track *t = &g_tracks[g_trackN++];
    memset(t, 0, sizeof(*t));
    t->t.id = id;
    snprintf(t->t.label, sizeof(t->t.label), "%s", label);
    snprintf(t->t.lang, sizeof(t->t.lang), "%s", lang ? lang : "");
    t->stream = t->pid = t->rend = -1;
    return g_trackN - 1;
}

static void lang_label(const char *lang, const char *name, int ordinal, char *out, int cap) {
    const char *nm = lang && lang[0] ? lang_name(lang) : NULL;
    if (name && name[0]) snprintf(out, (size_t)cap, "%s", name);
    else if (nm) snprintf(out, (size_t)cap, "%s", nm);
    else if (lang && lang[0] && strcmp(lang, "und") != 0) snprintf(out, (size_t)cap, "%s", lang);
    else snprintf(out, (size_t)cap, "Subtitles %d", ordinal);
}

void sub_stream_opened(AVFormatContext *fmt, int isHls, const char *spec, int64_t startUs) {
    lk();
    int same = spec && strcmp(spec, g_spec) == 0;
    if (!same) {
        subs_list_clear(&g_ext);
        g_haveExt = 0; g_extName[0] = '\0';
        g_pickId = -1; g_pickName[0] = g_pickLang[0] = '\0';
        snprintf(g_spec, sizeof(g_spec), "%s", spec ? spec : "");
        g_status[0] = '\0';
        if (g_jobKind == JOB_EXTERNAL) start_job(JOB_NONE, NULL);   // a fetch for the old source
    }
    subs_list_clear(&g_live);
    (void)isHls;   // HLS subtitle renditions arrive through hls.c, not this stream's tracks
    g_startUs = startUs;
    g_trackN = 0;
    int ord = 0;
    for (unsigned i = 0; fmt && i < fmt->nb_streams; i++) {
        const AVStream *st = fmt->streams[i];
        int c = st->codecpar->codec_id;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        if (!is_text_codec(c) && !(is_bitmap_codec(c) && avcodec_find_decoder(c))) continue;
        const AVDictionaryEntry *le = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *te = av_dict_get(st->metadata, "title", NULL, 0);
        char base[48], label[72];
        lang_label(le ? le->value : NULL, NULL, ++ord, base, sizeof(base));
        snprintf(label, sizeof(label), "%s%s%.24s%s", base, te && te->value[0] ? " - " : "",
                 te ? te->value : "", (st->disposition & AV_DISPOSITION_FORCED) ? " (forced)" : "");
        int k = add_track((int)i, label, le ? le->value : "");
        if (k >= 0) { g_tracks[k].stream = (int)i; g_tracks[k].pid = st->id; g_tracks[k].codec = c; }
    }
    if (isHls) {
        HlsSubRendition r[HLS_MAX_SUB_RENDITIONS];
        int n = hls_subtitle_renditions(r, HLS_MAX_SUB_RENDITIONS);
        for (int i = 0; i < n; i++) {
            char label[72];
            lang_label(r[i].lang, r[i].name, i + 1, label, sizeof(label));
            int k = add_track(100 + i, label, r[i].lang);
            if (k >= 0) g_tracks[k].rend = i;
        }
    }
    if (g_haveExt) {
        char label[72];
        snprintf(label, sizeof(label), "File: %.60s", g_extName);
        add_track(SUB_ID_EXTERNAL, label, "");
    }
    // Which track to show: the user's choice for this source (HLS renditions
    // found again by name/language), else the preferred language, else off.
    int pick = -1;
    if (same && g_pickId >= 0) {
        if (g_pickId >= 100 && g_pickId < SUB_ID_EXTERNAL) {
            for (int i = 0; i < g_trackN && pick < 0; i++) {
                const Track *t = &g_tracks[i];
                if (t->rend < 0) continue;
                HlsSubRendition r[HLS_MAX_SUB_RENDITIONS];
                int n = hls_subtitle_renditions(r, HLS_MAX_SUB_RENDITIONS);
                if (t->rend < n && strcmp(r[t->rend].name, g_pickName) == 0 &&
                    strcmp(r[t->rend].lang, g_pickLang) == 0) pick = t->t.id;
            }
        } else if (find_track(g_pickId)) {
            pick = g_pickId;
        }
    } else if (!same && g_langPref[0]) {
        for (int pass = 0; pass < 2 && pick < 0; pass++)       // prefer full over forced-only
            for (int i = 0; i < g_trackN && pick < 0; i++) {
                const Track *t = &g_tracks[i];
                int forced = strstr(t->t.label, "(forced)") != NULL;
                if ((pass == 0) == !forced && t->t.lang[0] && lang_matches(g_langPref, t->t.lang)) pick = t->t.id;
            }
    }
    select_locked(pick);
    if (g_pendUrl[0] && strcmp(g_pendSpec, g_spec) == 0) {   // a subtitle named with this source
        start_job(JOB_EXTERNAL, g_pendUrl);
        g_pendUrl[0] = g_pendSpec[0] = '\0';
    }
    ulk();
    sub_apply_pending(fmt);          // before the decode thread exists: route + discard now
}

void sub_stream_closed(void) {
    lk();
    g_trackN = 0;
    g_cur = -1;
    subs_list_clear(&g_live);
    if (g_jobKind == JOB_HLS) start_job(JOB_NONE, NULL);
    g_applyPending = 0;
    ulk();
    g_routeStream = g_routePid = -1; g_routeCodec = 0;
    if (g_bdec) avcodec_free_context(&g_bdec);
    g_bdecCodec = 0; g_bdecPid = -1;
}

// ---- decode thread -----------------------------------------------------------
void sub_apply_pending(AVFormatContext *fmt) {
    if (!g_applyPending) return;
    lk();
    g_applyPending = 0;
    const Track *t = (g_cur >= 0 && g_cur < 100) ? find_track(g_cur) : NULL;
    int old = g_routeStream;
    g_routeStream = t ? t->stream : -1;
    g_routePid = t ? t->pid : -1;
    g_routeCodec = t ? t->codec : 0;
    ulk();
    if (!fmt) return;
    // Only the chosen subtitle stream is demuxed (the rest stay discarded).
    if (old >= 0 && old < (int)fmt->nb_streams && old != g_routeStream &&
        fmt->streams[old]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        fmt->streams[old]->discard = AVDISCARD_ALL;
    if (g_routeStream >= 0 && g_routeStream < (int)fmt->nb_streams)
        fmt->streams[g_routeStream]->discard = AVDISCARD_DEFAULT;
}

int sub_stream_index(const AVFormatContext *fmt, int bySegmentPid) {
    if (g_routeStream < 0 || !fmt) return -1;
    if (!bySegmentPid) return g_routeStream < (int)fmt->nb_streams ? g_routeStream : -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && fmt->streams[i]->id == g_routePid)
            return (int)i;
    return -1;
}

static void add_live(SubCue *c) {
    lk();
    int keep = g_routeCodec != 0 && g_cur >= 0 && g_cur < 100;   // still showing an embedded track
    if (keep) {
        subs_list_add(&g_live, c);
        if (g_live.n > 400) subs_list_prune(&g_live, g_clockUs - 60LL * 1000000);
    }
    ulk();
    if (!keep) { free(c->text); free(c->argb); }
}

static void bitmap_packet(const AVStream *st, AVPacket *pkt, int64_t fallbackUs) {
    int c = st->codecpar->codec_id;
    if (!g_bdec || g_bdecCodec != c || g_bdecPid != st->id) {
        if (g_bdec) avcodec_free_context(&g_bdec);
        const AVCodec *dec = avcodec_find_decoder(c);
        if (!dec || !(g_bdec = avcodec_alloc_context3(dec))) return;
        avcodec_parameters_to_context(g_bdec, st->codecpar);
        g_bdec->pkt_timebase = st->time_base;
        if (avcodec_open2(g_bdec, dec, NULL) < 0) { avcodec_free_context(&g_bdec); return; }
        g_bdecCodec = c; g_bdecPid = st->id;
    }
    AVSubtitle sub;
    int got = 0;
    if (avcodec_decode_subtitle2(g_bdec, &sub, &got, pkt) < 0 || !got) return;
    int64_t base = sub.pts != AV_NOPTS_VALUE ? sub.pts : fallbackUs;
    int64_t start = base + (int64_t)sub.start_display_time * 1000;
    int64_t end = (sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX)
                  ? base + (int64_t)sub.end_display_time * 1000 : SUB_END_OPEN;
    // The canvas the regions are placed on: PGS/DVB announce it, DVD is 720x576/480.
    int cw = g_bdec->width > 0 ? g_bdec->width : 720, ch = g_bdec->height > 0 ? g_bdec->height : 576;
    if (sub.num_rects == 0) { lk(); subs_list_end_open(&g_live, start); ulk(); }
    for (unsigned r = 0; r < sub.num_rects; r++) {
        const AVSubtitleRect *rc = sub.rects[r];
        if (rc->type != SUBTITLE_BITMAP || rc->w <= 0 || rc->h <= 0 || !rc->data[0] || !rc->data[1]) continue;
        uint32_t *argb = malloc((size_t)rc->w * rc->h * 4);
        if (!argb) continue;
        const uint32_t *pal = (const uint32_t *)rc->data[1];
        for (int y = 0; y < rc->h; y++)
            for (int x = 0; x < rc->w; x++)
                argb[(size_t)y * rc->w + x] = pal[rc->data[0][(size_t)y * rc->linesize[0] + x]];
        SubCue cue = { start, end, NULL, argb, rc->x, rc->y, rc->w, rc->h, cw, ch };
        add_live(&cue);
    }
    avsubtitle_free(&sub);
}

void sub_packet(const AVFormatContext *fmt, AVPacket *pkt) {
    const AVStream *st = fmt->streams[pkt->stream_index];
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (pts == AV_NOPTS_VALUE) return;
    int64_t startUs = av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q);
    int c = st->codecpar->codec_id;
    if (is_bitmap_codec(c)) { bitmap_packet(st, pkt, startUs); return; }
    if (!is_text_codec(c) || !pkt->data || pkt->size <= 0) return;
    int64_t dur = pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 4000000;
    char *t = (c == AV_CODEC_ID_ASS || c == AV_CODEC_ID_SSA) ? subs_text_ass((const char *)pkt->data, pkt->size)
            : c == AV_CODEC_ID_MOV_TEXT ? subs_text_movtext(pkt->data, pkt->size)
            : subs_text_plain((const char *)pkt->data, pkt->size);
    if (!t) return;
    SubCue cue = { startUs, startUs + dur, t, NULL, 0, 0, 0, 0, 0, 0 };
    add_live(&cue);
}

void sub_seek(void) {
    lk();
    // Embedded cues re-arrive from the new position; an open-ended bitmap
    // from before the seek must not linger there. Fetched HLS cues stay; the
    // fetcher just moves its window.
    if (g_cur >= 0 && g_cur < 100) subs_list_clear(&g_live);
    g_seekFlag = 1;
    ulk();
    if (g_bdec) avcodec_flush_buffers(g_bdec);
}

// ---- fetcher ------------------------------------------------------------------
static int job_live(unsigned gen) { return g_jobGen == gen; }

static char *fetch_text(const char *url, int *rcOut) {
    uint8_t *b = NULL;
    int n = 0;
    int rc = aseg_fetch_ch(ASEG_CH_PLAYLIST, url, &b, &n);
    if (rcOut) *rcOut = rc;
    if (rc != 0 || !b || n <= 0) { free(b); return NULL; }
    char *t = realloc(b, (size_t)n + 1);
    if (!t) { free(b); return NULL; }
    t[n] = '\0';
    return t;
}

static void external_job(unsigned gen, const char *url) {
    int rc = 0;
    char *text = fetch_text(url, &rc);
    SubList tmp;
    subs_list_init(&tmp);
    int n = text ? subs_parse_file(&tmp, text, 0) : -1;
    free(text);
    lk();
    if (job_live(gen)) {
        if (n > 0) {
            subs_list_clear(&g_ext);
            subs_list_take_all(&g_ext, &tmp);
            const char *slash = strrchr(url, '/');
            snprintf(g_extName, sizeof(g_extName), "%.*s", (int)strcspn(slash ? slash + 1 : url, "?#|"),
                     slash ? slash + 1 : url);
            g_haveExt = 1;
            if (!find_track(SUB_ID_EXTERNAL)) {
                char label[72];
                snprintf(label, sizeof(label), "File: %.60s", g_extName);
                add_track(SUB_ID_EXTERNAL, label, "");
            }
            g_pickId = SUB_ID_EXTERNAL;
            select_locked(SUB_ID_EXTERNAL);
            snprintf(g_status, sizeof(g_status), "subtitles: %d cues from %.60s", n, g_extName);
        } else {
            snprintf(g_status, sizeof(g_status), "subtitles: %s (rc=%d)",
                     n == -1 && rc == 0 ? "not an SRT/WebVTT file" : "download failed", rc);
        }
    }
    ulk();
    subs_list_free(&tmp);
}

// HLS WebVTT rendition. VOD: fetch the segments around the playback position
// first (90 s ahead), then the rest as playback moves. Live: follow the
// playlist like the video does.
static void hls_job(unsigned gen, const char *url) {
    HlsPlaylist *pl = calloc(1, sizeof(*pl));
    unsigned char *done = NULL;
    int64_t *startMs = NULL;
    int nextSeq = -1;
    if (!pl) return;
    hlspl_init(pl);
    while (job_live(gen)) {
        char *body = fetch_text(url, NULL);
        if (!body) { sceKernelUsleep(2000 * 1000); continue; }
        hlspl_free(pl);
        int prc = hlspl_parse_media(pl, body, url);
        free(body);
        if (prc != 0) { sceKernelUsleep(2000 * 1000); continue; }
        if (!pl->isLive) {
            free(done); free(startMs);
            done = calloc((size_t)pl->segCount, 1);
            startMs = malloc(sizeof(int64_t) * (size_t)(pl->segCount + 1));
            if (!done || !startMs) break;
            startMs[0] = 0;
            for (int i = 0; i < pl->segCount; i++) startMs[i + 1] = startMs[i] + pl->segDurMs[i];
            int fetched = 0;
            while (job_live(gen) && fetched < pl->segCount) {
                g_seekFlag = 0;
                int64_t pos = (g_clockUs - g_startUs) / 1000;   // position in the playlist timeline
                int pick = -1;
                for (int i = 0; i < pl->segCount && pick < 0; i++)   // the window around the position
                    if (!done[i] && startMs[i + 1] >= pos - 5000 && startMs[i] <= pos + 90000) pick = i;
                if (pick < 0) { sceKernelUsleep(1000 * 1000); continue; }
                char *seg = fetch_text(pl->segs[pick], NULL);
                done[pick] = 1; fetched++;
                if (!seg) continue;
                SubList tmp;
                subs_list_init(&tmp);
                subs_parse_file(&tmp, seg, 0);
                free(seg);
                lk();
                if (job_live(gen)) subs_list_take_all(&g_live, &tmp);
                ulk();
                subs_list_free(&tmp);
            }
            // Everything is in: stay idle until the job changes.
            while (job_live(gen)) sceKernelUsleep(500 * 1000);
            break;
        }
        // Live: fetch what is new since the last refresh.
        for (int i = 0; i < pl->segCount && job_live(gen); i++) {
            int seq = pl->mediaSeq + i;
            if (seq < nextSeq) continue;
            char *seg = fetch_text(pl->segs[i], NULL);
            nextSeq = seq + 1;
            if (!seg) continue;
            SubList tmp;
            subs_list_init(&tmp);
            subs_parse_file(&tmp, seg, 0);
            free(seg);
            lk();
            if (job_live(gen)) {
                subs_list_take_all(&g_live, &tmp);
                subs_list_prune(&g_live, g_clockUs - 60LL * 1000000);
            }
            ulk();
            subs_list_free(&tmp);
        }
        int waitMs = pl->targetDurMs / 2 > 1000 ? pl->targetDurMs / 2 : 1000;
        for (int w = 0; w < waitMs / 250 && job_live(gen); w++) sceKernelUsleep(250 * 1000);
    }
    free(done); free(startMs);
    hlspl_free(pl);
    free(pl);
}

static void *fetch_main(void *arg) {
    (void)arg;
    unsigned doneGen = 0;
    for (;;) {
        lk();
        unsigned gen = g_jobGen;
        int kind = g_jobKind;
        char url[2048];
        snprintf(url, sizeof(url), "%s", g_jobUrl);
        ulk();
        if (gen == doneGen || kind == JOB_NONE) { doneGen = gen; sceKernelUsleep(200 * 1000); continue; }
        if (kind == JOB_EXTERNAL) external_job(gen, url);
        else if (kind == JOB_HLS) hls_job(gen, url);
        doneGen = gen;
    }
    return NULL;
}

// ---- control -----------------------------------------------------------------
void sub_init(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    subs_list_init(&g_ext);
    subs_list_init(&g_live);
    OrbisPthread th;
    if (scePthreadCreate(&th, NULL, fetch_main, NULL, "ps4cast_subs") != 0)
        trace_mark("subtitle fetcher thread failed");
}

int sub_tracks(SubTrack *out, int max, int *cur) {
    lk();
    int n = g_trackN < max ? g_trackN : max;
    for (int i = 0; out && i < n; i++) out[i] = g_tracks[i].t;
    if (cur) *cur = g_cur;
    ulk();
    return n;
}

int sub_select(int id) {
    lk();
    if (id != -1 && !find_track(id)) { ulk(); return -1; }
    g_pickId = id;
    g_pickName[0] = g_pickLang[0] = '\0';
    const Track *t = find_track(id);
    if (t && t->rend >= 0) {
        HlsSubRendition r[HLS_MAX_SUB_RENDITIONS];
        int n = hls_subtitle_renditions(r, HLS_MAX_SUB_RENDITIONS);
        if (t->rend < n) {
            snprintf(g_pickName, sizeof(g_pickName), "%s", r[t->rend].name);
            snprintf(g_pickLang, sizeof(g_pickLang), "%s", r[t->rend].lang);
        }
    }
    select_locked(id);
    ulk();
    return 0;
}

int sub_load_text(const char *text, const char *name) {
    SubList tmp;
    subs_list_init(&tmp);
    int n = subs_parse_file(&tmp, text, 0);
    if (n <= 0) { subs_list_free(&tmp); return -1; }
    lk();
    subs_list_clear(&g_ext);
    subs_list_take_all(&g_ext, &tmp);
    snprintf(g_extName, sizeof(g_extName), "%s", name && name[0] ? name : "subtitles");
    g_haveExt = 1;
    if (!find_track(SUB_ID_EXTERNAL)) {
        char label[72];
        snprintf(label, sizeof(label), "File: %.60s", g_extName);
        add_track(SUB_ID_EXTERNAL, label, "");
    }
    g_pickId = SUB_ID_EXTERNAL;
    select_locked(SUB_ID_EXTERNAL);
    snprintf(g_status, sizeof(g_status), "subtitles: %d cues from %.60s", n, g_extName);
    ulk();
    subs_list_free(&tmp);
    return 0;
}

int sub_load_url(const char *url) {
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) return -1;
    lk();
    start_job(JOB_EXTERNAL, url);
    snprintf(g_status, sizeof(g_status), "subtitles: downloading");
    ulk();
    return 0;
}

int sub_load_url_for(const char *spec, const char *url) {
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) return -1;
    lk();
    if (spec && strcmp(spec, g_spec) == 0 && g_trackN >= 0) {
        start_job(JOB_EXTERNAL, url);
    } else {
        snprintf(g_pendSpec, sizeof(g_pendSpec), "%s", spec ? spec : "");
        snprintf(g_pendUrl, sizeof(g_pendUrl), "%s", url);
    }
    ulk();
    return 0;
}

void sub_set_lang(const char *lang) { snprintf(g_langPref, sizeof(g_langPref), "%s", lang ? lang : ""); }
const char *sub_lang(void) { return g_langPref; }
const char *sub_status(void) { return g_status; }

int sub_text_at(int64_t mediaUs, char *out, int cap) {
    const SubCue *act[8];
    int o = 0;
    out[0] = '\0';
    lk();
    int ext = g_cur == SUB_ID_EXTERNAL;
    int n = g_cur < 0 ? 0 : subs_list_active(ext ? &g_ext : &g_live, ext ? mediaUs - g_startUs : mediaUs, act, 8);
    for (int i = 0; i < n; i++)
        if (act[i]->text && o < cap - 1) o += snprintf(out + o, (size_t)(cap - o), "%s%s", o ? "\n" : "", act[i]->text);
    ulk();
    return n;
}

// ---- drawing -----------------------------------------------------------------
void sub_set_clock(int64_t mediaUs) { g_clockUs = mediaUs; }
void sub_set_video_rect(int x, int y, int w, int h) { g_vx = x; g_vy = y; g_vw = w; g_vh = h; }

// Bitmaps are copied out of the cue list (another thread may free a cue),
// and the copy stays alive until the next frame's drawing is queued.
typedef struct { const uint32_t *src; int64_t start; int x, y, w, h, cw, ch; uint32_t *buf; int cap; uint32_t id; } Slot;
static Slot     g_slots[4];
static uint32_t g_imgSeq;

static int text_w(const char *s, void *ctx) { return gfx_text_w(s, *(int *)ctx); }

void sub_draw(Gfx *g, int avoidBottom) {
    if (g_cur < 0 || g_vw <= 0 || g_vh <= 0) return;
    const SubCue *act[8];
    char text[1024];
    int tl = 0, nb = 0;
    Slot use[4];
    lk();
    int ext = g_cur == SUB_ID_EXTERNAL;
    const SubList *l = ext ? &g_ext : &g_live;
    int n = subs_list_active(l, ext ? g_clockUs - g_startUs : g_clockUs, act, 8);
    for (int i = 0; i < n; i++) {
        const SubCue *c = act[i];
        if (c->text) {
            int k = snprintf(text + tl, sizeof(text) - (size_t)tl, "%s%s", tl ? "\n" : "", c->text);
            if (k > 0 && tl + k < (int)sizeof(text)) tl += k;
        } else if (c->argb && nb < 4) {
            Slot *s = &g_slots[nb];
            if (s->src != c->argb || s->start != c->startUs || s->w != c->w || s->h != c->h) {
                size_t px = (size_t)c->w * c->h;
                if ((int)px > s->cap) {
                    uint32_t *nbuf = realloc(s->buf, px * 4);
                    if (!nbuf) continue;
                    s->buf = nbuf; s->cap = (int)px;
                }
                memcpy(s->buf, c->argb, px * 4);
                s->src = c->argb; s->start = c->startUs;
                s->x = c->x; s->y = c->y; s->w = c->w; s->h = c->h; s->cw = c->canvasW; s->ch = c->canvasH;
                s->id = ++g_imgSeq;
            }
            use[nb++] = *s;
        }
    }
    ulk();

    for (int i = 0; i < nb; i++) {               // regions mapped from their canvas onto the picture
        const Slot *s = &use[i];
        int cw = s->cw > 0 ? s->cw : g_vw, ch = s->ch > 0 ? s->ch : g_vh;
        int dx = g_vx + (int)((int64_t)s->x * g_vw / cw), dy = g_vy + (int)((int64_t)s->y * g_vh / ch);
        int dw = (int)((int64_t)s->w * g_vw / cw), dh = (int)((int64_t)s->h * g_vh / ch);
        gfx_image(g, dx, dy, dw > 0 ? dw : 1, dh > 0 ? dh : 1, s->buf, s->w, s->h, s->id);
    }
    if (!tl) return;

    // Text: centred lines near the bottom of the picture, on a translucent
    // box for contrast on any background. The atlas folds accents to ASCII.
    int scale = g_vh >= 900 ? 6 : g_vh >= 650 ? 5 : g_vh >= 450 ? 4 : 3;
    char wrapped[1024];
    int lines = subs_wrap(text, g_vw * 86 / 100, 4, text_w, &scale, wrapped, sizeof(wrapped));
    int lineH = 8 * scale + 3 * scale;
    int bottom = g_vy + g_vh - g_vh * 7 / 100;
    if (avoidBottom > 0 && bottom > g->height - avoidBottom) bottom = g->height - avoidBottom;   // above the HUD
    int y = bottom - lines * lineH;
    const GfxColor ink = { 0xff, 0xff, 0xff }, shade = { 0, 0, 0 };
    char *p = wrapped;
    for (int i = 0; i < lines; i++) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        int w = gfx_text_w(p, scale);
        int x = g_vx + (g_vw - w) / 2;
        gfx_round_a(g, x - 3 * scale, y - scale, w + 6 * scale, lineH, 2 * scale, shade, 150);
        gfx_text(g, x, y + scale, p, scale, ink);
        y += lineH;
        if (!e) break;
        p = e + 1;
    }
}
