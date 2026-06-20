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

#endif
