// aseg.h — minimal blocking HTTP(S) "fetch a whole small resource" used for the
// HLS *audio* rendition (playlist + audio-only segments). It runs on its own
// socket, independent of the video read-ahead reader (httpsrc), so the two HLS
// streams (video + separate audio) don't contend for one connection.
#ifndef PS4CAST_ASEG_H
#define PS4CAST_ASEG_H

#include <stdint.h>

// Download an entire (small) resource into a malloc'd buffer. Caller frees *buf.
// http + https (BearSSL), follows up to a few redirects. Returns 0, else < 0.
int aseg_fetch(const char *url, uint8_t **buf, int *len);

// Abort a fetch in progress (Stop / new cast) so the audio thread can't wedge.
void aseg_abort(void);
// Clear a stale abort before starting a new stream (see aseg.c).
void aseg_resume(void);
// Tighten the per-fetch time budget around small PLAYLIST fetches (1) and restore
// the generous SEGMENT budget (0). A single budget for both either starves slow
// segments (continuous rebuffering) or lets a dead host block a channel switch.
void aseg_set_playlist_budget(int on);

#endif
