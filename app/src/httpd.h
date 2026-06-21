// httpd.h — tiny single-threaded, non-blocking HTTP control server.
// Poll it every frame from the main loop. It never blocks the renderer.
#ifndef PS4CAST_HTTPD_H
#define PS4CAST_HTTPD_H

int  httpd_start(int port);                       // 0 on success
void httpd_poll(void);                            // service pending clients
int  httpd_take_play_request(char *out, int len); // 1 if a new URL was posted
int  httpd_take_player_request(char *out, int len);
int  httpd_take_stop_request(void);               // 1 if stop was posted
int  httpd_take_quit_request(void);               // 1 if quit was posted
int  httpd_take_next(char *out, int len);         // 1 if a queued URL (autoplay)
const char *httpd_last_push(void);                // last cast URL (HUD title)

// Shared M3U/IPTV channel store, for the on-screen D-pad channel zapper.
int  httpd_chan_count(void);
int  httpd_chan_current(void);                    // tuned channel index, -1 none
int  httpd_chan_get(int i, char *name, int nameCap, char *url, int urlCap);
void httpd_chan_group(int i, char *out, int cap);
void httpd_chan_set_current(int i);

// Per-URL resume positions (VOD): remember where playback stopped.
void httpd_resume_save(const char *url, int pos, int dur);
int  httpd_resume_get(const char *url);

#endif
