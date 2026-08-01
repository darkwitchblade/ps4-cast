// audio.h — sceAudioOut playback with a PCM ring + its own output thread.
// Decoded/resampled S16 stereo @ 48kHz is queued with audio_write(); the output
// thread paces it at realtime (sceAudioOutOutput blocks), and audio_clock()
// exposes the master clock the video is synced to.
#ifndef PS4CAST_AUDIO_H
#define PS4CAST_AUDIO_H

#include <stdint.h>

int    audio_open(void);                 // 48kHz S16 stereo; 0 on success
void   audio_write(const int16_t *interleaved, int nframes); // nframes stereo
void   audio_set_base(double pts_sec);   // anchor the clock to the first audio pts
void   audio_flush(void);                // clear queued PCM after seek/stop
unsigned audio_fill_ms(void);            // decoded audio waiting in the ring (decode-thread backpressure)
void   audio_pause(int paused);          // pause consumption without draining PCM
double audio_clock(void);                // current audible time, seconds
int    audio_has_clock(void);            // true after decoded audio anchors the clock
int    audio_ok(void);
const char *audio_debug(void);           // one-line diagnostic
void   audio_close(void);                // end session, KEEP device open (reused next cast)
void   audio_shutdown(void);             // full teardown of device+thread (app exit)

#endif
