#include "gfx.h"
#include "font8x8.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>   // _exit

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>

// Triple buffering lets us submit a flip and immediately render the next frame
// into a third buffer while the previous one is still scanning out, instead of
// blocking on each flip (which serialized convert+scanout to ~30Hz). Presents at
// the full 60Hz flip rate.
#define GFX_BUFFERS 3

// Pixel format used by sceVideoOut here is 32-bit; the proven sample encodes
// 0x80000000 | (r<<16)|(g<<8)|b into each uint32 of the buffer.
static inline uint32_t encode(GfxColor c) {
    return 0x80000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

int gfx_init(Gfx *g, int width, int height) {
    memset(g, 0, sizeof(*g));
    g->width  = width;
    g->height = height;
    g->depth  = 4;
    g->frameBufferSize = width * height * g->depth;
    g->activeIdx = 0;

    g->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g->video < 0)
        return -1;

    // Flip queue
    OrbisKernelEqueue q = 0;
    if (sceKernelCreateEqueue(&q, "ps4cast flip") < 0)
        return -2;
    g->flipQueue = (void *)(uintptr_t)q;
    sceVideoOutAddFlipEvent(q, g->video, 0);

    // Allocate direct memory for the frame buffers (aligned to 2MB).
    const int alignment = 0x200000;
    size_t want = (size_t)g->frameBufferSize * GFX_BUFFERS;
    g->directMemSize = (want + alignment - 1) / alignment * alignment;

    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                      g->directMemSize, alignment, 3,
                                      &g->directMemOff) < 0)
        return -3;

    if (sceKernelMapDirectMemory(&g->videoMem, g->directMemSize, 0x33, 0,
                                 g->directMemOff, alignment) < 0) {
        sceKernelReleaseDirectMemory(g->directMemOff, g->directMemSize);
        return -4;
    }

    g->videoMemSP = (uintptr_t)g->videoMem;
    for (int i = 0; i < GFX_BUFFERS; i++) {
        g->frameBuffers[i] = (void *)g->videoMemSP;
        g->videoMemSP += g->frameBufferSize;
    }

    // SRGB / 32-bit attribute, pitch == width.
    sceVideoOutSetBufferAttribute((OrbisVideoOutBufferAttribute *)g->attr,
                                  0x80000000, 1, 0, g->width, g->height, g->width);

    if (sceVideoOutRegisterBuffers(g->video, 0, (void * const *)g->frameBuffers, GFX_BUFFERS,
                                   (OrbisVideoOutBufferAttribute *)g->attr) != 0)
        return -5;

    sceVideoOutSetFlipRate(g->video, 0);
    return 0;
}

// Fail-closed: a display/GPU fault surfaces here (flip submit error, or flips
// stop completing). Rather than let the app limp on as "shadow playback" with a
// dead display — which the system eventually turns into the "error has occurred"
// dialog while our CPU threads keep running — record a precise reason and exit
// cleanly. A clean exit also avoids the system crash dialog.
static void gfx_fatal(const char *what, int rc) {
    char b[128];
    int n = snprintf(b, sizeof(b), "GFX-FAULT v" APP_VER " %s rc=%d\n", what, rc);
    int fd = sceKernelOpen("/data/ps4cast_crash.log", 0x0201 /*WRONLY|CREAT*/ | 0x0400 /*TRUNC*/, 0666);
    if (fd >= 0) { sceKernelWrite(fd, b, (size_t)n); sceKernelClose(fd); }
    _exit(0);
}

void gfx_present(Gfx *g, int frameID) {
    // A failed flip submit means the video-out/GPU rejected the frame — treat it
    // as a display fault and fail-closed instead of continuing blind.
    int rc = sceVideoOutSubmitFlip(g->video, g->activeIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameID);
    if (rc < 0) gfx_fatal("submitflip", rc);   // negative = SCE error: display rejected the flip
    // Signal a clean GPU frame boundary every frame. Without this the system can
    // never find a quiesced point to suspend the app, so closing it from the menu
    // hits CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND and crashes instead of quitting.
    sceGnmSubmitDone();

    // Advance to the next render target WITHOUT blocking on this flip. Then throttle
    // only if too many flips are still outstanding: with GFX_BUFFERS buffers we let
    // up to (GFX_BUFFERS-1) be in flight, which guarantees the buffer we're about to
    // render into next (last submitted GFX_BUFFERS-1 frames ago) has finished its
    // flip — so no tearing, while CPU convert overlaps scanout for full-rate present.
    g->activeIdx = (g->activeIdx + 1) % GFX_BUFFERS;

    // Poll flip completion with a hard ceiling: if flips stop completing the
    // display/GPU has hung — fail-closed with a precise reason (faster + clearer
    // than waiting for the generic freeze-watchdog).
    uint64_t t0 = sceKernelGetProcessTime();
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g->video, &st);
        // st.flipArg is the frameID of the last completed flip. Stop waiting once
        // the in-flight count has dropped to the buffer budget.
        if (frameID - (int)st.flipArg <= (GFX_BUFFERS - 1))
            break;
        if (sceKernelGetProcessTime() - t0 > 3ULL * 1000 * 1000)
            gfx_fatal("flip-stall", frameID - (int)st.flipArg);
        sceKernelUsleep(1000);   // 1ms poll; throttle wait is ≤1 vblank in normal use
    }
}

void gfx_pixel(Gfx *g, int x, int y, GfxColor c) {
    if (x < 0 || y < 0 || x >= g->width || y >= g->height)
        return;
    ((uint32_t *)g->frameBuffers[g->activeIdx])[y * g->width + x] = encode(c);
}

void gfx_rect(Gfx *g, int x, int y, int w, int h, GfxColor c) {
    uint32_t e = encode(c);
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > g->width)  x1 = g->width;
    if (y1 > g->height) y1 = g->height;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = fb + (size_t)yy * g->width;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = e;
    }
}

void gfx_clear(Gfx *g, GfxColor c) {
    gfx_rect(g, 0, 0, g->width, g->height, c);
}

int gfx_text_w(const char *s, int scale) {
    return (int)strlen(s) * 8 * scale;
}

int gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c) {
    int penX = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (ch >= 128) ch = '?';
        const unsigned char *glyph = font8x8_basic[ch];
        for (int row = 0; row < 8; row++) {
            unsigned char bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << col)) {   // bit 0 = leftmost pixel
                    gfx_rect(g, penX + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
        penX += 8 * scale;
    }
    return penX - x;
}
