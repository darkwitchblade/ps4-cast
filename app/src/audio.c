#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <orbis/_types/audio_out.h>

#define GRAIN        256          // must match the buffer size given to Output
#define RATE         48000
#define RING_FRAMES  (RATE * 3)   // 3 seconds of stereo headroom
#define USER_ID_SYSTEM 0xFF       // ORBIS_USER_SERVICE_USER_ID_SYSTEM

static int16_t          *g_pcm;            // ring of stereo frames (2 int16 each)
static size_t            g_head, g_fill;   // in frames
static OrbisPthreadMutex g_amtx;
static OrbisPthread      g_athread;
static int               g_handle = -1;
static int               g_ok = 0, g_threadUp = 0;
static volatile int      g_astop = 0;
static volatile uint64_t g_contentOut = 0; // decoded content frames actually heard
static volatile uint64_t g_hwOut = 0;      // frames submitted to the audio device
static volatile double   g_base = 0;
static volatile int      g_baseSet = 0;
static volatile int      g_paused = 0;
static int               g_initRc = -999, g_openRc = -999, g_volRc = -999;
static long              g_written = 0, g_dropped = 0, g_underruns = 0;

const char *audio_debug(void) {
    static char b[180];
    size_t fill = 0;
    if (g_ok) {
        scePthreadMutexLock(&g_amtx);
        fill = g_fill;
        scePthreadMutexUnlock(&g_amtx);
    }
    snprintf(b, sizeof(b),
             "ao init=%d open=%d vol=%d ok=%d pause=%d fill=%ums wr=%ld drop=%ld und=%ld out=%llums hw=%llums",
             g_initRc, g_openRc, g_volRc, g_ok, g_paused,
             (unsigned)(fill * 1000 / RATE), g_written, g_dropped, g_underruns,
             (unsigned long long)(g_contentOut * 1000 / RATE),
             (unsigned long long)(g_hwOut * 1000 / RATE));
    return b;
}

double audio_clock(void) { return g_base + (double)g_contentOut / RATE; }
int    audio_ok(void)    { return g_ok; }

void audio_set_base(double pts_sec) {
    if (!g_baseSet) {
        // Account for decoded content already emitted so the clock reads pts_sec now.
        g_base = pts_sec - (double)g_contentOut / RATE;
        g_baseSet = 1;
    }
}

void audio_reset_base(double pts_sec) {
    g_base = pts_sec - (double)g_contentOut / RATE;
    g_baseSet = 1;
}

void audio_flush(void) {
    if (!g_ok) return;
    scePthreadMutexLock(&g_amtx);
    g_head = 0;
    g_fill = 0;
    g_contentOut = 0;
    g_hwOut = 0;
    g_base = 0;
    g_baseSet = 0;
    scePthreadMutexUnlock(&g_amtx);
}

void audio_pause(int paused) {
    g_paused = paused ? 1 : 0;
}

static void *audio_main(void *arg) {
    (void)arg;
    int16_t out[GRAIN * 2];
    while (!g_astop) {
        int avail = 0;
        scePthreadMutexLock(&g_amtx);
        if (!g_paused) {
            avail = g_fill < GRAIN ? (int)g_fill : GRAIN;
            for (int i = 0; i < avail; i++) {
                size_t idx = ((g_head + i) % RING_FRAMES) * 2;
                out[i*2] = g_pcm[idx]; out[i*2+1] = g_pcm[idx+1];
            }
            g_head = (g_head + avail) % RING_FRAMES;
            g_fill -= avail;
        }
        scePthreadMutexUnlock(&g_amtx);
        for (int i = avail; i < GRAIN; i++) { out[i*2] = 0; out[i*2+1] = 0; } // pad
        sceAudioOutOutput(g_handle, out);   // blocks ~GRAIN/RATE => realtime pacing
        if (!g_paused && avail < GRAIN) g_underruns++;
        g_contentOut += (uint64_t)avail;
        g_hwOut += GRAIN;
    }
    return NULL;
}

int audio_open(void) {
    audio_close();
    sceUserServiceInitialize(NULL);     // ensure user service is up
    g_initRc = sceAudioOutInit();        // 0 or already-initialized is fine
    // Use the SYSTEM user (0xFF), like the OpenOrbis audio sample — the initial
    // user id was rejected (0x809b0001).
    g_handle = sceAudioOutOpen(USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, RATE,
                               ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    g_openRc = g_handle;
    if (g_handle < 0) return -1;
    // Default volume can be 0 (silent). Force all channels to 0 dB.
    int vol[8]; for (int i = 0; i < 8; i++) vol[i] = 32768; // SCE_AUDIO_OUT_VOLUME_0dB
    g_volRc = sceAudioOutSetVolume(g_handle, 0xFFF, vol);
    g_pcm = malloc(sizeof(int16_t) * 2 * RING_FRAMES);
    if (!g_pcm) { sceAudioOutClose(g_handle); g_handle = -1; return -1; }
    g_head = g_fill = 0; g_contentOut = 0; g_hwOut = 0; g_base = 0; g_baseSet = 0; g_paused = 0; g_astop = 0;
    g_written = g_dropped = g_underruns = 0;
    scePthreadMutexInit(&g_amtx, NULL, "ps4cast_a");
    if (scePthreadCreate(&g_athread, NULL, audio_main, NULL, "ps4cast_aud") != 0) {
        free(g_pcm); g_pcm = NULL; sceAudioOutClose(g_handle); g_handle = -1;
        scePthreadMutexDestroy(&g_amtx);
        return -1;
    }
    g_threadUp = 1; g_ok = 1;
    return 0;
}

void audio_write(const int16_t *interleaved, int nframes) {
    if (!g_ok) return;
    scePthreadMutexLock(&g_amtx);
    for (int i = 0; i < nframes; i++) {
        if (g_fill >= RING_FRAMES) { g_dropped += nframes - i; break; } // >3s buffered
        size_t idx = ((g_head + g_fill) % RING_FRAMES) * 2;
        g_pcm[idx] = interleaved[i*2]; g_pcm[idx+1] = interleaved[i*2+1];
        g_fill++; g_written++;
    }
    scePthreadMutexUnlock(&g_amtx);
}

void audio_close(void) {
    g_ok = 0;
    if (g_threadUp) {
        g_astop = 1;
        scePthreadJoin(g_athread, NULL);
        scePthreadMutexDestroy(&g_amtx);
        g_threadUp = 0;
    }
    if (g_handle >= 0) { sceAudioOutClose(g_handle); g_handle = -1; }
    if (g_pcm) { free(g_pcm); g_pcm = NULL; }
    g_head = g_fill = 0; g_contentOut = 0; g_hwOut = 0; g_baseSet = 0; g_paused = 0;
}
