#include "gfx.h"
#include "font8x8.h"
#include "font_atlas.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>   // _exit

// GFX_HOST_PREVIEW lets tools/uipreview.c compile the pure drawing primitives on
// a desktop (rendering the console UI to a PNG) without the PS4 display backend.
#ifndef GFX_HOST_PREVIEW
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#endif

// Triple buffering lets us submit a flip and immediately render the next frame
// into a third buffer while the previous one is still scanning out, instead of
// blocking on each flip. The v03.12 HW-decoder soak exonerated this as the
// CE-36329-3 root cause, so restore it for smoother 40/50/60fps presentation.
#define GFX_BUFFERS 3

// Set in gfx_init; used by gfx_emergency_release to tear down the display/GPU
// context on a fatal exit so the process stays reclaimable (not unkillable).
static Gfx *g_gfx = NULL;

// Pixel format used by sceVideoOut here is 32-bit; the proven sample encodes
// 0x80000000 | (r<<16)|(g<<8)|b into each uint32 of the buffer.
static inline uint32_t encode(GfxColor c) {
    return 0x80000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

#ifndef GFX_HOST_PREVIEW
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
    g_gfx = g;                 // for gfx_emergency_release on a fatal exit
    return 0;
}

// Best-effort release of the display + GPU context. A bare _exit() with a flip
// still registered/queued leaves the VideoOut scanout context wedged so the
// kernel cannot reclaim it — the app becomes UNKILLABLE (the system menu can't
// close it; only a power-cycle clears it). Quiescing the GPU ring and closing
// the video-out here lets a recoverable fault exit cleanly instead. Idempotent;
// safe to call from the watchdog / fatal paths. _exit still follows as the
// guaranteed fallback, so even if these calls no-op we still terminate.
void gfx_emergency_release(void) {
    Gfx *g = g_gfx;
    if (!g) return;
    g_gfx = NULL;                                  // run once
    sceGnmSubmitDone();                            // quiesce GPU work (graphics + compute)
    if (g->video >= 0) { sceVideoOutClose(g->video); g->video = -1; }
}

// Fail-closed: a display/GPU fault surfaces here (flip submit error, or flips
// stop completing). Rather than let the app limp on as "shadow playback" with a
// dead display — which the system eventually turns into the "error has occurred"
// dialog while our CPU threads keep running — record a precise reason, RELEASE
// the display/GPU context (so the exit is reclaimable, not unkillable), and exit.
static void gfx_fatal(const char *what, int rc) {
    char b[128];
    int n = snprintf(b, sizeof(b), "GFX-FAULT v" APP_VER " %s rc=%d\n", what, rc);
    int fd = sceKernelOpen("/data/ps4cast_crash.log", 0x0201 /*WRONLY|CREAT*/ | 0x0400 /*TRUNC*/, 0666);
    if (fd >= 0) { sceKernelWrite(fd, b, (size_t)n); sceKernelClose(fd); }
    gfx_emergency_release();
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

    // Advance to the next render target WITHOUT blocking on this flip. Then
    // throttle only if too many flips are still outstanding: with GFX_BUFFERS
    // buffers we let up to (GFX_BUFFERS-1) be in flight, which guarantees the
    // buffer we're about to render into next has finished its flip.
    g->activeIdx = (g->activeIdx + 1) % GFX_BUFFERS;

    // Hard ceiling: if flips stop completing the display/GPU has hung -> fail
    // closed with a precise reason (faster than the generic freeze-watchdog).
    uint64_t t0 = sceKernelGetProcessTime();
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g->video, &st);
        if (frameID - (int)st.flipArg <= (GFX_BUFFERS - 1))
            break;
        if (sceKernelGetProcessTime() - t0 > 3ULL * 1000 * 1000)
            gfx_fatal("flip-stall", frameID - (int)st.flipArg);
        sceKernelUsleep(1000);
    }
}
#endif // GFX_HOST_PREVIEW

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

// ---- modern compositing helpers ------------------------------------------
// All of these read-modify-write the back buffer so edges can be anti-aliased
// and panels can be translucent — the difference between a "homebrew" look and
// a polished one. AA coverage uses the clang sqrt intrinsic (no libm link).
void gfx_blend(Gfx *g, int x, int y, GfxColor c, int a) {
    if (x < 0 || y < 0 || x >= g->width || y >= g->height || a <= 0) return;
    uint32_t *p = &((uint32_t *)g->frameBuffers[g->activeIdx])[y * g->width + x];
    if (a >= 255) { *p = encode(c); return; }
    uint32_t e = *p;
    int dr = (e >> 16) & 0xff, dg = (e >> 8) & 0xff, db = e & 0xff;
    GfxColor o = {
        (uint8_t)((c.r * a + dr * (255 - a)) / 255),
        (uint8_t)((c.g * a + dg * (255 - a)) / 255),
        (uint8_t)((c.b * a + db * (255 - a)) / 255),
    };
    *p = encode(o);
}

// Translucent fill — the hot path for every panel/HUD/overlay. Kept tight (no
// per-pixel function call or bounds check, row-pointer walk, integer blend) so
// it doesn't steal CPU from the software video decode on the same cores.
void gfx_rect_a(Gfx *g, int x, int y, int w, int h, GfxColor c, int a) {
    if (a >= 255) { gfx_rect(g, x, y, w, h, c); return; }
    if (a <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > g->width) x1 = g->width; if (y1 > g->height) y1 = g->height;
    if (x1 <= x || y1 <= y) return;
    uint32_t ia = (uint32_t)(255 - a);
    uint32_t sr = (uint32_t)c.r * a, sg = (uint32_t)c.g * a, sb = (uint32_t)c.b * a;
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    for (int yy = y; yy < y1; yy++) {
        uint32_t *row = fb + (size_t)yy * g->width;
        for (int xx = x; xx < x1; xx++) {
            uint32_t e = row[xx];
            uint32_t r = (sr + ((e >> 16) & 0xff) * ia) / 255;
            uint32_t gg = (sg + ((e >> 8) & 0xff) * ia) / 255;
            uint32_t b = (sb + (e & 0xff) * ia) / 255;
            row[xx] = 0x80000000u | (r << 16) | (gg << 8) | b;
        }
    }
}

void gfx_circle_a(Gfx *g, int cx, int cy, int r, GfxColor c, int a) {
    if (r <= 0) return;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float cov = (float)r + 0.5f - __builtin_sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            gfx_blend(g, x, y, c, (int)(cov * a));
        }
    }
}
void gfx_circle(Gfx *g, int cx, int cy, int r, GfxColor c) { gfx_circle_a(g, cx, cy, r, c, 255); }

void gfx_round_a(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2; if (r < 0) r = 0;
    gfx_rect_a(g, x + r, y, w - 2 * r, h, c, a);          // center column
    gfx_rect_a(g, x, y + r, r, h - 2 * r, c, a);          // left strip
    gfx_rect_a(g, x + w - r, y + r, r, h - 2 * r, c, a);  // right strip
    int ccx[4] = { x + r, x + w - r - 1, x + r, x + w - r - 1 };
    int ccy[4] = { y + r, y + r, y + h - r - 1, y + h - r - 1 };
    int qx[4] = { -1, 1, -1, 1 }, qy[4] = { -1, -1, 1, 1 };
    for (int k = 0; k < 4; k++)
        for (int yy = 0; yy <= r; yy++)
            for (int xx = 0; xx <= r; xx++) {
                float cov = (float)r + 0.5f - __builtin_sqrtf((float)(xx * xx + yy * yy));
                if (cov <= 0) continue; if (cov > 1) cov = 1;
                gfx_blend(g, ccx[k] + qx[k] * xx, ccy[k] + qy[k] * yy, c, (int)(cov * a));
            }
}
void gfx_round(Gfx *g, int x, int y, int w, int h, int r, GfxColor c) { gfx_round_a(g, x, y, w, h, r, c, 255); }

void gfx_vgrad(Gfx *g, int x, int y, int w, int h, GfxColor top, GfxColor bot) {
    int oy = y, x0 = x, x1 = x + w;
    if (x0 < 0) x0 = 0; if (x1 > g->width) x1 = g->width;
    int yy0 = y < 0 ? 0 : y, yy1 = (y + h) > g->height ? g->height : (y + h);
    for (int yy = yy0; yy < yy1; yy++) {
        float t = h > 1 ? (float)(yy - oy) / (float)(h - 1) : 0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        GfxColor c = {
            (uint8_t)(top.r + (int)((bot.r - top.r) * t)),
            (uint8_t)(top.g + (int)((bot.g - top.g) * t)),
            (uint8_t)(top.b + (int)((bot.b - top.b) * t)),
        };
        uint32_t e = encode(c);
        uint32_t *row = (uint32_t *)g->frameBuffers[g->activeIdx] + (size_t)yy * g->width;
        for (int xx = x0; xx < x1; xx++) row[xx] = e;
    }
}

void gfx_tri(Gfx *g, int x0, int y0, int x1, int y1, int x2, int y2, GfxColor c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    float d = (float)((y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2));
    if (d == 0) return;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int hit = 0;                       // 2x2 supersample for smooth edges
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                float px = x + 0.25f + sx * 0.5f, py = y + 0.25f + sy * 0.5f;
                float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d;
                float b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d;
                float cc = 1 - a - b;
                if (a >= 0 && b >= 0 && cc >= 0) hit++;
            }
            if (hit) gfx_blend(g, x, y, c, hit * 255 / 4);
        }
    }
}

void gfx_arc(Gfx *g, int cx, int cy, int r, int thick, int quad, GfxColor c) {
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            int dx = x - cx, dy = y - cy;
            if (quad == 0 && !(dx <= 0 && dy <= 0)) continue;
            if (quad == 1 && !(dx >= 0 && dy <= 0)) continue;
            if (quad == 2 && !(dx >= 0 && dy >= 0)) continue;
            if (quad == 3 && !(dx <= 0 && dy >= 0)) continue;
            float dd = __builtin_sqrtf((float)(dx * dx + dy * dy));
            float outer = (float)r + 0.5f - dd;
            float inner = dd - (float)(r - thick) + 0.5f;
            float cov = outer < inner ? outer : inner;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            gfx_blend(g, x, y, c, (int)(cov * 255));
        }
    }
}

int gfx_text_w(const char *s, int scale) {
    return (int)strlen(s) * 8 * scale;
}

int gfx_text_tr(Gfx *g, int x, int y, const char *s, int scale, GfxColor c, int track) {
    // Anti-aliased atlas for scale 2..6 (cell == 8*scale, so the advance and all
    // existing layout math are unchanged); the 8x8 bitmap covers tiny scale 1.
    int useAtlas = (scale >= 2 && scale <= FONT_MAXSCALE && FONT_DATA[scale]);
    int cell = 8 * scale;
    const unsigned char *atlas = useAtlas ? FONT_DATA[scale] : 0;
    int penX = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (useAtlas) {
            if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
            const unsigned char *gly = atlas + (size_t)(ch - FONT_FIRST) * cell * cell;
            for (int gy = 0; gy < cell; gy++) {
                const unsigned char *row = gly + (size_t)gy * cell;
                for (int gx = 0; gx < cell; gx++)
                    if (row[gx]) gfx_blend(g, penX + gx, y + gy, c, row[gx]);
            }
        } else {
            if (ch >= 128) ch = '?';
            const unsigned char *glyph = font8x8_basic[ch];
            for (int row = 0; row < 8; row++) {
                unsigned char bits = glyph[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (1 << col)) gfx_rect(g, penX + col * scale, y + row * scale, scale, scale, c);
            }
        }
        penX += 8 * scale + track;
    }
    return penX - x - (s[0] ? track : 0);
}

int gfx_text_tr_w(const char *s, int scale, int track) {
    int n = (int)strlen(s);
    return n * 8 * scale + (n > 0 ? (n - 1) * track : 0);
}

int gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c) {
    return gfx_text_tr(g, x, y, s, scale, c, 0);
}
