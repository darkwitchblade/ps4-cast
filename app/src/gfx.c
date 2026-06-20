#include "gfx.h"
#include "font8x8.h"

#include <string.h>
#include <errno.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>

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

    // Allocate direct memory for two frame buffers (aligned to 2MB).
    const int alignment = 0x200000;
    size_t want = (size_t)g->frameBufferSize * 2;
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
    for (int i = 0; i < 2; i++) {
        g->frameBuffers[i] = (void *)g->videoMemSP;
        g->videoMemSP += g->frameBufferSize;
    }

    // SRGB / 32-bit attribute, pitch == width.
    sceVideoOutSetBufferAttribute((OrbisVideoOutBufferAttribute *)g->attr,
                                  0x80000000, 1, 0, g->width, g->height, g->width);

    if (sceVideoOutRegisterBuffers(g->video, 0, (void * const *)g->frameBuffers, 2,
                                   (OrbisVideoOutBufferAttribute *)g->attr) != 0)
        return -5;

    sceVideoOutSetFlipRate(g->video, 0);
    return 0;
}

void gfx_present(Gfx *g, int frameID) {
    sceVideoOutSubmitFlip(g->video, g->activeIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameID);
    // Signal a clean GPU frame boundary every frame. Without this the system can
    // never find a quiesced point to suspend the app, so closing it from the menu
    // hits CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND and crashes instead of quitting.
    sceGnmSubmitDone();

    // Wait until this frame is actually on screen.
    OrbisKernelEqueue q = (OrbisKernelEqueue)(uintptr_t)g->flipQueue;
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g->video, &st);
        if (st.flipArg == frameID)
            break;
        OrbisKernelEvent ev;
        int out = 0;
        if (sceKernelWaitEqueue(q, &ev, 1, &out, 0) != 0)
            break;
    }

    g->activeIdx = (g->activeIdx + 1) % 2;
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
