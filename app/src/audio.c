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
// Stereo ring headroom. 3s was too tight: with a large network read-ahead the
// decode thread races ahead of realtime at stream start and after every seek,
// pinned g_fill at the cap, and audio_write() then DISCARDED the excess samples
// (measured on-device: 77440 frames = 1.61s of audio silently lost). That audio
// never plays, so it desyncs A/V for the rest of the segment and re-triggers on
// each seek. 8s absorbs the burst so nothing is dropped. Cost: 8*48000*2ch*2B =
// ~1.5MB. Ring depth does not add A/V latency (the clock tracks output position)
// and does not slow seeks (audio_flush empties it).
#define RING_FRAMES  (RATE * 8)
// Max silence-padding debt the clock will repay. Bounds the correction so a long
// starvation can't queue a multi-second clock pull-back.
//
// 150ms proved too tight: the starvation right after a seek generates ~228ms of
// padding, so only 150ms was repayable and a ~78ms residual desync survived every
// seek (measured). 500ms covers that with margin. Safe at this size only because
// audio_flush() now clears the debt on seek/new-stream — the runaway that dragged
// video seconds behind was an UNCLEARED debt, not a large one. Repayment is
// gradual (16 frames/grain, ~6% of realtime), so even a full 500ms slides back
// over ~8s and stays imperceptible.
#define PAD_DEBT_MAX ((uint64_t)(RATE * 500 / 1000))
#define USER_ID_SYSTEM 0xFF       // ORBIS_USER_SERVICE_USER_ID_SYSTEM

static int16_t          *g_pcm;            // ring of stereo frames (2 int16 each)
static size_t            g_head, g_fill;   // in frames
static OrbisPthreadMutex g_amtx;
static OrbisPthreadCond  g_acond;
static OrbisPthread      g_athread;
static int               g_handle = -1;
static int               g_ok = 0, g_threadUp = 0;
static int               g_devUp = 0;        // sceAudioOut device + thread opened (kept across casts)
static volatile int      g_astop = 0;
static volatile uint64_t g_contentOut = 0; // decoded content frames actually heard
static volatile uint64_t g_hwOut = 0;      // frames submitted to the audio device
static volatile double   g_base = 0;
static volatile int      g_baseSet = 0;
static volatile int      g_paused = 0;
static int               g_initRc = -999, g_openRc = -999, g_volRc = -999;
static long              g_written = 0, g_dropped = 0, g_underruns = 0;
static volatile uint64_t g_padComp = 0;   // frames of silence padding cancelled out of the clock (drift removed)
static volatile uint64_t g_padDebt = 0;   // padding not yet repaid (repaid gradually once audio flows again)

const char *audio_debug(void) {
    static char b[180];
    size_t fill = 0;
    if (g_ok) {
        scePthreadMutexLock(&g_amtx);
        fill = g_fill;
        scePthreadMutexUnlock(&g_amtx);
    }
    snprintf(b, sizeof(b),
             "ao init=%d open=%d vol=%d ok=%d pause=%d fill=%ums wr=%ld drop=%ld und=%ld out=%llums hw=%llums comp=%llums",
             g_initRc, g_openRc, g_volRc, g_ok, g_paused,
             (unsigned)(fill * 1000 / RATE), g_written, g_dropped, g_underruns,
             (unsigned long long)(g_contentOut * 1000 / RATE),
             (unsigned long long)(g_hwOut * 1000 / RATE),
             (unsigned long long)(g_padComp * 1000 / RATE));
    return b;
}

// A/V master clock, anchored to the audio DEVICE timeline (g_hwOut) so it always
// advances in realtime and can never stall playback. Silence padding would
// otherwise walk it ahead of the audio actually heard, so the output thread
// cancels that out by pulling g_base back per padded partial grain (see there).
//
// DO NOT "simplify" this to g_contentOut. That was tried (v03.61) and deadlocks:
// the decode thread blocks once the video queue is full, so it stops producing
// audio; with a pure content clock the clock then never advances, no frame is
// ever presented, the queue never drains — video froze permanently after a seek.
// The device timeline + per-grain base compensation gives the same accuracy
// without that failure mode.
double audio_clock(void) { return g_base + (double)g_hwOut / RATE; }
int    audio_has_clock(void) { return g_baseSet; }
int    audio_ok(void)    { return g_ok; }

void audio_set_base(double pts_sec) {
    if (!g_baseSet) {
        // Anchor to the real audio device timeline. During network/audio
        // underruns the device still advances by outputting padded silence, and
        // video must keep following that realtime clock instead of freezing.
        g_base = pts_sec - (double)g_hwOut / RATE;
        g_baseSet = 1;
    }
}

void audio_reset_base(double pts_sec) {
    g_base = pts_sec - (double)g_hwOut / RATE;
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
    // The timeline is re-anchored after a flush (seek/new stream), so any padding
    // debt from before it is meaningless. Clearing it is essential: leaving it
    // made the clock repay a phantom multi-second debt forever, dragging video
    // seconds BEHIND audio.
    g_padDebt = 0;
    g_padComp = 0;
    scePthreadMutexUnlock(&g_amtx);
}

unsigned audio_fill_ms(void) {
    if (!g_ok) return 0;
    scePthreadMutexLock(&g_amtx);
    size_t f = g_fill;
    scePthreadMutexUnlock(&g_amtx);
    return (unsigned)(f * 1000 / RATE);
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
        while (!g_astop && !g_ok)
            scePthreadCondWait(&g_acond, &g_amtx);
        if (g_astop) {
            scePthreadMutexUnlock(&g_amtx);
            break;
        }
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
        sceAudioOutOutput(g_handle, out);   // blocks ~GRAIN/RATE => realtime pacing (also when paused)
        // Advance the clock ONLY when not paused. A rebuffer pause must FREEZE the
        // clock — otherwise it runs ahead during the hold and every refilled video
        // frame is "late" on resume, dumping the whole queue (mass drops).
        if (!g_paused) {
            if (avail < GRAIN) {
                // Short grain: the rest was padded with silence, i.e. device time
                // carrying no content. Record it as DEBT rather than correcting the
                // clock now — the clock must keep advancing on device time while
                // audio is absent. (Freezing it is what deadlocked v03.61: the
                // decode thread blocks on a full video queue, so it stops producing
                // audio, so a content-based clock never advances, so no frame is
                // ever presented and the queue never drains -> video frozen.)
                g_underruns++;
                g_padDebt += (uint64_t)(GRAIN - avail);
                // CAP the debt. This is meant to absorb small padding artifacts,
                // not a genuine multi-second starvation (e.g. the refill after a
                // seek), where the audio timeline really did skip and re-anchoring
                // handles it. Without a cap, a 4s starve queued a 4s correction
                // that dragged video seconds behind audio.
                if (g_padDebt > PAD_DEBT_MAX) g_padDebt = PAD_DEBT_MAX;
            } else if (g_padDebt > 0) {
                // Audio is flowing again: repay the debt a little per grain so the
                // clock slides back into alignment with the audio actually heard.
                // Paid gradually (not as one step) so video re-syncs smoothly
                // instead of jumping and dumping the frame queue as "late".
                // ~16 frames/grain = 0.33ms per 5.33ms grain, so a typical ~128ms
                // startup burst is absorbed in ~2s and never becomes permanent.
                uint64_t step = g_padDebt < 16 ? g_padDebt : 16;
                g_base -= (double)step / RATE;
                g_padDebt -= step;
                g_padComp += step;
            }
            g_contentOut += (uint64_t)avail;
            g_hwOut += GRAIN;
        }
    }
    return NULL;
}

int audio_open(void) {
    // Reuse the already-open device across casts. The output format is always the
    // same (S16 stereo 48kHz, since we resample to it), so there's no need to
    // close+reopen per cast — and doing so rapidly exhausts sceAudioOut ports
    // (open started failing after many casts, wedging all audio). Keep one port
    // open for the app's lifetime; just reset the ring/clock for the new stream.
    if (g_devUp && g_handle >= 0) {
        scePthreadMutexLock(&g_amtx);
        g_head = g_fill = 0; g_contentOut = 0; g_hwOut = 0; g_base = 0; g_baseSet = 0; g_paused = 0;
        g_written = g_dropped = g_underruns = 0; g_padComp = 0; g_padDebt = 0;
        g_ok = 1;
        scePthreadCondSignal(&g_acond);
        scePthreadMutexUnlock(&g_amtx);
        return 0;
    }
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
    g_written = g_dropped = g_underruns = 0; g_padComp = 0; g_padDebt = 0;
    g_ok = 1;
    scePthreadMutexInit(&g_amtx, NULL, "ps4cast_a");
    scePthreadCondInit(&g_acond, NULL, "ps4cast_ac");
    if (scePthreadCreate(&g_athread, NULL, audio_main, NULL, "ps4cast_aud") != 0) {
        g_ok = 0;
        free(g_pcm); g_pcm = NULL; sceAudioOutClose(g_handle); g_handle = -1;
        scePthreadCondDestroy(&g_acond);
        scePthreadMutexDestroy(&g_amtx);
        return -1;
    }
    g_threadUp = 1; g_devUp = 1;
    return 0;
}

void audio_write(const int16_t *interleaved, int nframes) {
    if (!g_ok) return;
    scePthreadMutexLock(&g_amtx);
    for (int i = 0; i < nframes; i++) {
        // Ring full -> drop. Any g_dropped > 0 means audio was LOST and A/V will
        // be desynced by that much, so it should stay 0 in practice (watch the
        // "drop=" in audio_debug); if it climbs, RING_FRAMES is too small again.
        if (g_fill >= RING_FRAMES) { g_dropped += nframes - i; break; }
        size_t idx = ((g_head + g_fill) % RING_FRAMES) * 2;
        g_pcm[idx] = interleaved[i*2]; g_pcm[idx+1] = interleaved[i*2+1];
        g_fill++; g_written++;
    }
    scePthreadCondSignal(&g_acond);
    scePthreadMutexUnlock(&g_amtx);
}

// End the current playback session WITHOUT closing the device: stop accepting
// writes and clear the ring. The device + output thread stay alive (the thread
// outputs silence when idle) so the next cast reuses the same port — avoiding
// the rapid-recast sceAudioOut exhaustion. Real teardown is audio_shutdown().
void audio_close(void) {
    if (!g_devUp) { g_ok = 0; return; }
    scePthreadMutexLock(&g_amtx);
    g_head = g_fill = 0; g_contentOut = 0; g_hwOut = 0; g_base = 0; g_baseSet = 0; g_paused = 0;
    g_ok = 0;
    scePthreadCondSignal(&g_acond);
    scePthreadMutexUnlock(&g_amtx);
}

// Full teardown of the audio device + thread (app exit / fatal). Not used in the
// normal play/stop cycle so the port is reused across casts.
void audio_shutdown(void) {
    g_ok = 0;
    if (g_threadUp) {
        scePthreadMutexLock(&g_amtx);
        g_astop = 1;
        scePthreadCondSignal(&g_acond);
        scePthreadMutexUnlock(&g_amtx);
        scePthreadJoin(g_athread, NULL);
        scePthreadCondDestroy(&g_acond);
        scePthreadMutexDestroy(&g_amtx);
        g_threadUp = 0;
    }
    if (g_handle >= 0) { sceAudioOutClose(g_handle); g_handle = -1; }
    if (g_pcm) { free(g_pcm); g_pcm = NULL; }
    g_devUp = 0;
    g_head = g_fill = 0; g_contentOut = 0; g_hwOut = 0; g_baseSet = 0; g_paused = 0;
}
