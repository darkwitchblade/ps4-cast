// hls.h — minimal HLS (.m3u8) reader layered on top of httpsrc.
//
// ffmpeg is built without networking, so its native HLS demuxer can't fetch
// segments. Instead we parse the playlist ourselves and present the segments
// (optionally preceded by an fMP4 init segment) as ONE continuous byte stream
// to ffmpeg's mpegts/mov demuxer via the player's custom AVIO. http + https
// (BearSSL) segments both work because all I/O goes through httpsrc.
//
// Scope: VOD and live media playlists, and master playlists (a variant is
// chosen automatically). Sequential playback; seeking restarts.
#ifndef PS4CAST_HLS_H
#define PS4CAST_HLS_H

#include <stdint.h>

// True if the URL looks like an HLS playlist (.m3u8 / .m3u, ignoring query).
int  hls_is_url(const char *url);

// Fetch + parse the playlist (following a master->variant once). Returns 0 on
// success, negative on failure.
int  hls_open(const char *url);

// Read up to len bytes of the concatenated segment stream. >0 bytes, 0 = end.
int  hls_read(uint8_t *buf, uint32_t len);
int  hls_generation(void);                         // increments at media segment boundaries
int  hls_reset_generation(void);                   // increments when live HLS jumps/skips
int  hls_is_live(void);
int  hls_can_segment_demux(void);                  // TS media playlist (live or VOD)
int  hls_at_eof(void);                             // VOD: all segments consumed
int  hls_next_segment(uint8_t **outBuf, int *outLen, int *outResetGen);

void hls_close(void);

// Separate audio rendition support. Some master playlists carry audio in its own
// rendition (#EXT-X-MEDIA:TYPE=AUDIO with a URI) rather than muxed into the video
// segments; those would otherwise play silently. When hls_has_audio() is true the
// player drives a second decode path: hls_audio_read() streams the audio
// rendition's segments (on a separate connection) as one continuous byte stream,
// to be demuxed/decoded independently and synced to the audio clock.
int  hls_has_audio(void);
int  hls_audio_read(uint8_t *buf, uint32_t len);   // >0 bytes, 0 = end
void hls_audio_abort(void);                          // unblock a stuck fetch (Stop/cast)

// Request a one-step bitrate downshift (ABR), applied at the next segment
// boundary. No-op for media (non-master) playlists or when already lowest.
void hls_request_downshift(void);

// One-line diagnostic for /status.
const char *hls_debug(void);

#endif
