// player.h — URL playback/control interface used by the app and web UI.
#ifndef PS4CAST_PLAYER_H
#define PS4CAST_PLAYER_H

#include "gfx.h"

int  player_init(void);              // initialize playback backend; 0 ok
int  player_play(const char *url);   // start streaming a URL/file; 0 ok
void player_stop(void);              // stop current playback
int  player_is_active(void);         // 1 while playback is active (post-buffer)
int  player_started(void);           // 1 from Start until Stop (drives the pump)
int  player_is_live(void);           // 1 if the current source is a live stream
int  player_render(Gfx *g);          // blit newest video frame to g; 1 if drawn
void player_request_bar_clear(void); // call when an overlay draws over the letterbox bars (prevents ghosting)
const char *player_status(void);     // short human-readable status line
void player_debug(char *out, int len); // live playback debug state (one line)

// Transport controls (safe to call from the http thread: they set flags the
// render loop applies).
void player_pause(int paused);       // 1 = pause, 0 = resume
int  player_is_paused(void);
void player_seek(double seconds);    // absolute position in seconds
void player_progress(double *cur, double *dur); // current/total seconds
void player_interrupt(void);         // unblock a stuck read so Stop/cast works
void player_set_hw(int on);          // runtime hardware-decode on/off (A/B testing)
int  player_hw_enabled(void);        // current hardware-decode toggle state
int  player_buffering(void);         // 1 = playing but waiting on data (stalled)
int  player_buffer_pct(void);        // read-ahead buffer level, 0-100
uint64_t player_rx_total(void);      // total network bytes pulled (active source)

// Live playback telemetry for the on-screen stats overlay.
typedef struct {
    int    hw;            // 1 = hardware H.264 decode, 0 = software
    int    hls;           // 1 = HLS source
    int    segDemux;      // 1 = HLS segment-demux path
    int    w, h;          // source resolution
    long   frames;        // decoded video frames
    long   drops;         // dropped frames
    double bitrateMbps;   // stream bitrate estimate (Mbps)
    double aheadSec;      // buffered seconds ahead of playback
    int    bufPct;        // read-ahead buffer fill, 0-100
    int    lan;           // source is on the LAN
    char   codec[24];     // codec / decoder label
} PlayerStats;
void player_stats(PlayerStats *s);

#endif
