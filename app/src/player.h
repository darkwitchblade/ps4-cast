// player.h — thin wrapper over libSceAvPlayer for URL playback + CPU blit.
#ifndef PS4CAST_PLAYER_H
#define PS4CAST_PLAYER_H

#include "gfx.h"

int  player_init(void);              // load module + allocate texture pool; 0 ok
int  player_play(const char *url);   // start streaming a URL/file; 0 ok
void player_stop(void);              // stop current playback
int  player_is_active(void);         // 1 while AvPlayer reports active (post-buffer)
int  player_started(void);           // 1 from Start until Stop (drives the pump)
int  player_render(Gfx *g);          // blit newest video frame to g; 1 if drawn
const char *player_status(void);     // short human-readable status line
void player_debug(char *out, int len); // live AvPlayer debug state (one line)

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

#endif
