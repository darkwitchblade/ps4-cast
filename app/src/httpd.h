// httpd.h — tiny single-threaded, non-blocking HTTP control server.
// Poll it every frame from the main loop. It never blocks the renderer.
#ifndef PS4CAST_HTTPD_H
#define PS4CAST_HTTPD_H

int  httpd_start(int port);                       // 0 on success
void httpd_poll(void);                            // service pending clients
int  httpd_take_play_request(char *out, int len); // 1 if a new URL was posted
// Returns 1 for a normal cast/local file and 2 for an IPTV channel selection.
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
// On-screen filtering for large IPTV lists: narrow by starting letter ('#' =
// non-alphabetic, 0 = off) and/or favourites. Positions are FILTERED indices;
// httpd_chan_filter_abs() maps them back to absolute channel indices.
void httpd_chan_filter(char letter, int favOnly);
char httpd_chan_filter_letter(void);
int  httpd_chan_filter_fav(void);
int  httpd_chan_filter_count(void);
int  httpd_chan_filter_abs(int n);
int  httpd_chan_is_fav(int i);
void httpd_chan_toggle_fav(int i);
int  httpd_chan_letter_has(char letter);
// Bouquet rail: row 0 = All, 1 = Favourites, 2+ = playlist groups.
int  httpd_chan_rail_count(void);
void httpd_chan_rail_name(int row, char *out, int cap);
void httpd_chan_rail_select(int row);

// Per-URL resume positions (VOD): remember where playback stopped.
void httpd_resume_save(const char *url, int pos, int dur);
int  httpd_resume_get(const char *url);

// Pairing token shown on the TV; gates state-changing HTTP endpoints.
const char *httpd_token(void);
int  httpd_pairing_required(void);

#endif
