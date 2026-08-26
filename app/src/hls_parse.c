#include "hls_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hlspl_init(HlsPlaylist *pl) { memset(pl, 0, sizeof(*pl)); pl->targetDurMs = 3000; }

void hlspl_free(HlsPlaylist *pl) {
    if (pl->segs) {
        for (int i = 0; i < pl->segCount; i++) free(pl->segs[i]);
        free(pl->segs);
    }
    free(pl->initSeg);
    hlspl_init(pl);
}

int hlspl_codec_from_str(const char *codecs) {
    if (strstr(codecs, "avc1") || strstr(codecs, "avc3") || strstr(codecs, "h264")) return VC_H264;
    if (strstr(codecs, "hvc1") || strstr(codecs, "hev1") || strstr(codecs, "dvh"))  return VC_HEVC;
    if (strstr(codecs, "vp09") || strstr(codecs, "vp9"))  return VC_VP9;
    if (strstr(codecs, "av01"))                            return VC_AV1;
    return VC_OTHER;
}

// S3 signed URLs break when the PS4's TLS fingerprint meets some CDN edges;
// plain HTTP keeps those buckets working. Only rewrites amazonaws.com hosts.
void hlspl_prefer_plain_s3(char *url, int cap) {
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

// Resolve a playlist reference against its playlist URL. Handles absolute,
// scheme-relative, absolute-path and relative-to-directory references, and
// stops the base at '?' so query strings never leak into resolved paths.
void hlspl_resolve_url(const char *base, const char *ref, char *out, int cap) {
    const char *host_end = NULL;
    if (strncmp(base, "http://", 7) == 0)       host_end = strchr(base + 7, '/');
    else if (strncmp(base, "https://", 8) == 0) host_end = strchr(base + 8, '/');

    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, (size_t)cap, "%s", ref);
        hlspl_prefer_plain_s3(out, cap);
        return;
    }
    if (ref[0] == '/' && ref[1] == '/') {          // scheme-relative
        const char *colon = strchr(base, ':');
        snprintf(out, (size_t)cap, "%.*s:%s", colon ? (int)(colon - base) : 5, base, ref);
        hlspl_prefer_plain_s3(out, cap);
        return;
    }
    if (ref[0] == '/') {
        int hostlen = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, (size_t)cap, "%.*s%s", hostlen, base, ref);
        hlspl_prefer_plain_s3(out, cap);
        return;
    }
    const char *last = host_end;
    for (const char *s = host_end; s && *s; s++) {
        if (*s == '/') last = s;
        if (*s == '?') break;
    }
    int dirlen = last ? (int)(last - base + 1) : (int)strlen(base);
    snprintf(out, (size_t)cap, "%.*s%s", dirlen, base, ref);
    hlspl_prefer_plain_s3(out, cap);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

static int has_unsupported_hls_tags(const char *body) {
    // Only reject ACTUAL encryption. "#EXT-X-KEY:METHOD=NONE" explicitly means
    // the segments are NOT encrypted (RFC 8216 4.3.2.4) and is emitted by real
    // broadcasters (e.g. DW), so treating any EXT-X-KEY as encrypted rejected
    // perfectly playable streams.
    for (const char *k = strstr(body, "#EXT-X-KEY"); k; k = strstr(k + 1, "#EXT-X-KEY")) {
        const char *m = strstr(k, "METHOD=");
        const char *eol = strchr(k, '\n');
        if (!m || (eol && m > eol)) continue;
        if (strncmp(m + 7, "NONE", 4) == 0) continue;
        return 1;
    }
    return strstr(body, "#EXT-X-BYTERANGE") != NULL;
}

int hlspl_parse_media(HlsPlaylist *pl, char *body, const char *base) {
    if (has_unsupported_hls_tags(body)) return -2;
    pl->segs = malloc(sizeof(char *) * HLS_MAX_SEGMENTS);
    if (!pl->segs) return -1;
    pl->segCount = 0;
    pl->isLive = strstr(body, "#EXT-X-ENDLIST") ? 0 : 1;
    pl->targetDurMs = 3000;
    pl->mediaSeq = 0;
    pl->pendDisc = 0;
    pl->totalDurMs = 0;
    int pendingDurMs = 0;

    char resolved[2048];
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        rstrip(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') {
            const char *td = strstr(line, "#EXT-X-TARGETDURATION:");
            if (td) { int s = atoi(td + 22); if (s > 0 && s < 120) pl->targetDurMs = s * 1000; }
            const char *ms = strstr(line, "#EXT-X-MEDIA-SEQUENCE:");
            if (ms) pl->mediaSeq = atoi(ms + 22);
            const char *inf = strstr(line, "#EXTINF:");
            if (inf) {
                double sec = strtod(inf + 8, NULL);
                if (sec > 0.0 && sec < 36000.0)
                    pendingDurMs = (int)(sec * 1000.0 + 0.5);
            }
            if (strstr(line, "#EXT-X-DISCONTINUITY")) pl->pendDisc = 1;
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
                        memcpy(raw, uri, (size_t)l); raw[l] = '\0';
                        hlspl_resolve_url(base, raw, resolved, sizeof(resolved));
                        free(pl->initSeg);
                        pl->initSeg = strdup(resolved);
                    }
                }
            }
            continue;
        }
        if (pl->segCount >= HLS_MAX_SEGMENTS) break;
        hlspl_resolve_url(base, line, resolved, sizeof(resolved));
        pl->segs[pl->segCount] = strdup(resolved);
        if (pl->segs[pl->segCount]) {
            int dur = pendingDurMs > 0 ? pendingDurMs : pl->targetDurMs;
            pl->segDisc[pl->segCount] = (unsigned char)pl->pendDisc;
            pl->segDurMs[pl->segCount] = dur;
            pl->totalDurMs += dur;
            pl->pendDisc = 0;
            pendingDurMs = 0;
            pl->segCount++;
        }
    }
    return pl->segCount > 0 ? 0 : -1;
}

int hlspl_collect_variants(HlsPlaylist *pl, const char *body, const char *base) {
    pl->variantCount = 0;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF")) != NULL && pl->variantCount < HLS_MAX_VARIANTS) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        int bw = 0, height = 0, fps = 0, codec = VC_OTHER;
        const char *bwp = strstr(p, "BANDWIDTH="); if (bwp && bwp < eol) bw = atoi(bwp + 10);
        const char *rp = strstr(p, "RESOLUTION="); if (rp && rp < eol) { const char *x = strchr(rp, 'x'); if (x) height = atoi(x + 1); }
        const char *fp = strstr(p, "FRAME-RATE="); if (fp && fp < eol) fps = atoi(fp + 11);
        const char *cp = strstr(p, "CODECS=\"");
        if (cp && cp < eol) { char cbuf[128]; const char *cs = cp + 8; const char *ce = strchr(cs, '"');
            int cl = ce ? (int)(ce - cs) : 0; if (cl > 0 && cl < (int)sizeof(cbuf)) { memcpy(cbuf, cs, (size_t)cl); cbuf[cl] = '\0'; codec = hlspl_codec_from_str(cbuf); } }
        char agroup[64] = "";
        const char *ap = strstr(p, "AUDIO=\"");
        if (ap && ap < eol) { const char *as = ap + 7; const char *ae = strchr(as, '"');
            int al = ae ? (int)(ae - as) : 0; if (al > 0 && al < (int)sizeof(agroup)) { memcpy(agroup, as, (size_t)al); agroup[al] = '\0'; } }
        const char *nl = eol;
        while (nl) {
            const char *ls = nl + 1; const char *le = strchr(ls, '\n');
            int llen = le ? (int)(le - ls) : (int)strlen(ls);
            while (llen > 0 && (ls[llen-1] == '\r' || ls[llen-1] == ' ')) llen--;
            if (llen > 0 && ls[0] != '#') {
                char ref[2048]; int l = llen < (int)sizeof(ref) ? llen : (int)sizeof(ref) - 1;
                memcpy(ref, ls, (size_t)l); ref[l] = '\0';
                hlspl_resolve_url(base, ref, pl->variants[pl->variantCount].url, sizeof(pl->variants[0].url));
                pl->variants[pl->variantCount].bw = bw;
                pl->variants[pl->variantCount].height = height;
                pl->variants[pl->variantCount].fps = fps;
                pl->variants[pl->variantCount].codec = codec;
                strncpy(pl->variants[pl->variantCount].agroup, agroup, sizeof(pl->variants[0].agroup) - 1);
                pl->variants[pl->variantCount].agroup[sizeof(pl->variants[0].agroup) - 1] = '\0';
                pl->variantCount++;
                break;
            }
            nl = le;
        }
        p += 17;
    }
    for (int i = 1; i < pl->variantCount; i++) {     // insertion sort by bandwidth
        HlsVariant v = pl->variants[i]; int j = i - 1;
        while (j >= 0 && pl->variants[j].bw > v.bw) { pl->variants[j+1] = pl->variants[j]; j--; }
        pl->variants[j+1] = v;
    }
    return pl->variantCount;
}

// Variant score (lower = better) now that H.264 hardware decode is solid.
// Prefer H.264 up to 1080p, avoid 4K/HEVC/VP9/AV1, and keep 60fps as a
// cautious opt-in unless it is the only good option.
int hlspl_variant_score(const HlsVariant *v) {
    int s = 0;
    switch (v->codec) {
        case VC_H264:  s += 0;        break;
        case VC_HEVC:  s += 700000;   break;
        case VC_VP9:   s += 800000;   break;
        case VC_AV1:   s += 1000000;  break;
        default:       s += 250000;   break;
    }
    if (v->height > 1080)      s += 3000000;
    else if (v->height <= 0)   s += 20000;
    else                       s += (1080 - v->height) / 4;
    if (v->bw > 12000000)      s += (v->bw - 12000000) / 100;
    if (v->fps > 30)           s += 25000;
    int q = v->bw < 10000000 ? v->bw : 10000000;
    s += (10000000 - q) / 100000;
    return s;
}

int hlspl_pick_best(const HlsPlaylist *pl, int maxBw) {
    int best = -1, bestScore = 0x7fffffff;
    for (int i = 0; i < pl->variantCount; i++) {
        if (maxBw > 0 && pl->variants[i].bw >= maxBw) continue;
        int sc = hlspl_variant_score(&pl->variants[i]);
        if (sc < bestScore) { bestScore = sc; best = i; }
    }
    return best;
}

int hlspl_pick_start_variant(const HlsPlaylist *pl) {
    int start = -1;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height > 1080 || v->bw <= 0 || v->bw > 2500000) continue;
        if (start < 0 || v->bw > pl->variants[start].bw) start = i;
    }
    if (start >= 0) return start;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height > 1080) continue;
        if (start < 0 || (v->bw > 0 && v->bw < pl->variants[start].bw)) start = i;
    }
    if (start >= 0) return start;
    int best = hlspl_pick_best(pl, 0);
    return best < 0 ? 0 : best;
}

// fMP4 cannot replace initialization segments after FFmpeg opens its MOV
// demuxer. Select the final rendition before exposing bytes.
int hlspl_pick_fmp4_start_variant(const HlsPlaylist *pl, int autoMaxHeight) {
    int best = -1;
    int maxBw = autoMaxHeight > 720 ? 10000000 : 4000000;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height <= 0 || v->height > autoMaxHeight ||
            v->bw <= 0 || v->bw > maxBw) continue;
        if (best < 0 || v->height > pl->variants[best].height ||
            (v->height == pl->variants[best].height && v->bw > pl->variants[best].bw))
            best = i;
    }
    return best;
}
