// hls_parse.h — pure HLS playlist parsing: URL resolution, media-playlist
// segments, master-playlist variants, variant selection. No OS or network
// dependencies, so it compiles both on the PS4 and on the macOS host test
// harness (tests/host/).
#ifndef PS4CAST_HLS_PARSE_H
#define PS4CAST_HLS_PARSE_H

#include <stdint.h>

#define HLS_MAX_SEGMENTS 8192
#define HLS_MAX_VARIANTS 24

enum { VC_H264 = 0, VC_HEVC, VC_VP9, VC_AV1, VC_OTHER };
typedef struct { int bw, height, fps, codec; char url[2048]; char agroup[64]; } HlsVariant;

typedef struct {
    char        **segs;                       // resolved absolute segment URLs
    int           segCount;
    // RFC 8216: segDisc[i] = the segment AFTER an #EXT-X-DISCONTINUITY tag.
    unsigned char segDisc[HLS_MAX_SEGMENTS];
    int           segDurMs[HLS_MAX_SEGMENTS]; // EXTINF duration for VOD seeking
    int64_t       totalDurMs;
    int           pendDisc;                   // next segment starts a discontinuity
    char         *initSeg;                    // fMP4 init segment URL (EXT-X-MAP)
    int           isLive;                     // no EXT-X-ENDLIST
    int           targetDurMs;                // EXT-X-TARGETDURATION
    int           mediaSeq;                   // EXT-X-MEDIA-SEQUENCE
    HlsVariant    variants[HLS_MAX_VARIANTS];
    int           variantCount;
} HlsPlaylist;

void hlspl_init(HlsPlaylist *pl);                       // zero, no allocation
void hlspl_free(HlsPlaylist *pl);                       // release segment strings

void hlspl_prefer_plain_s3(char *url, int cap);
void hlspl_resolve_url(const char *base, const char *ref, char *out, int cap);
int  hlspl_codec_from_str(const char *codecs);          // CODECS="avc1.x,mp4a.y"

// Parse a media playlist. body is MUTATED (line tokenizing). Returns 0, or
// -1 (no segments), -2 (encrypted/byterange unsupported).
int  hlspl_parse_media(HlsPlaylist *pl, char *body, const char *base);

// Parse all master-playlist variants, sorted by bandwidth. Returns count.
int  hlspl_collect_variants(HlsPlaylist *pl, const char *body, const char *base);

// Variant selection. pick_best: only variants strictly below maxBw (0 = any).
int  hlspl_variant_score(const HlsVariant *v);
int  hlspl_pick_best(const HlsPlaylist *pl, int maxBw);
int  hlspl_pick_start_variant(const HlsPlaylist *pl);
int  hlspl_pick_fmp4_start_variant(const HlsPlaylist *pl, int autoMaxHeight);

#endif
