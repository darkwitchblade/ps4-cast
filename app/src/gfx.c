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

static int default_track(int scale) {
    (void)scale;
    return 0;
}

static uint32_t utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    if (!p || !*p) return 0;
    unsigned char c = *p++;
    if (c < 0x80) { *pp = p; return c; }
    if ((c & 0xe0) == 0xc0 && (p[0] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x1f) << 6) | (uint32_t)(p[0] & 0x3f);
        *pp = p + 1;
        return cp >= 0x80 ? cp : '?';
    }
    if ((c & 0xf0) == 0xe0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)(p[0] & 0x3f) << 6) | (uint32_t)(p[1] & 0x3f);
        *pp = p + 2;
        return (cp >= 0x800 && !(cp >= 0xd800 && cp <= 0xdfff)) ? cp : '?';
    }
    if ((c & 0xf8) == 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[0] & 0x3f) << 12) |
                      ((uint32_t)(p[1] & 0x3f) << 6) | (uint32_t)(p[2] & 0x3f);
        *pp = p + 3;
        return (cp >= 0x10000 && cp <= 0x10ffff) ? cp : '?';
    }
    *pp = p;
    return '?';
}

static unsigned char fold_cp(uint32_t cp) {
    if (cp >= FONT_FIRST && cp <= FONT_LAST) return (unsigned char)cp;
    if (cp == 0x00a0 || cp == 0x2000 || cp == 0x2001 || cp == 0x2002 || cp == 0x2003 ||
        cp == 0x2004 || cp == 0x2005 || cp == 0x2006 || cp == 0x2007 || cp == 0x2008 ||
        cp == 0x2009 || cp == 0x200a || cp == 0x202f || cp == 0x205f || cp == 0x3000)
        return ' ';
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201a || cp == 0x201b || cp == 0x2032)
        return '\'';
    if (cp == 0x201c || cp == 0x201d || cp == 0x201e || cp == 0x201f || cp == 0x2033)
        return '"';
    if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2013 || cp == 0x2014 ||
        cp == 0x2015 || cp == 0x2212)
        return '-';
    if (cp == 0x2026) return '.';
    if (cp == 0x00d7 || cp == 0x2715 || cp == 0x2716 || cp == 0x274c) return 'x';
    if (cp == 0x00f7) return '/';
    if (cp == 0x2044 || cp == 0x2215) return '/';
    if (cp == 0x00b0 || cp == 0x02da) return 'o';
    if (cp == 0x00b7 || cp == 0x2022 || cp == 0x2219 || cp == 0x25cf) return '*';
    if (cp == 0x2190 || cp == 0x2192 || cp == 0x21d0 || cp == 0x21d2) return '>';
    if (cp == 0x2191 || cp == 0x2193 || cp == 0x21d1 || cp == 0x21d3) return '|';
    if (cp == 0x00a9) return 'C';
    if (cp == 0x00ae) return 'R';
    if (cp == 0x2122) return 'T';
    if (cp == 0x20ac) return 'E';
    if (cp == 0x00a3) return 'L';
    if (cp == 0x00a5) return 'Y';

    if (cp >= 0x0660 && cp <= 0x0669) return (unsigned char)('0' + (cp - 0x0660));
    if (cp >= 0x06f0 && cp <= 0x06f9) return (unsigned char)('0' + (cp - 0x06f0));
    if (cp == 0x060c || cp == 0x066b) return ',';
    if (cp == 0x061b) return ';';
    if (cp == 0x061f) return '?';
    if (cp == 0x066a) return '%';
    if (cp == 0x0640 || (cp >= 0x064b && cp <= 0x065f) || cp == 0x0670 ||
        (cp >= 0x06d6 && cp <= 0x06ed))
        return 0;   // Arabic tatweel/diacritics; skip

    // Arabic fallback: real Arabic needs shaping + RTL layout, but a single-byte
    // transliteration keeps titles safe and recognizable in the current atlas.
    if (cp == 0x0621) return '\'';
    if (cp == 0x0622 || cp == 0x0623 || cp == 0x0625 || cp == 0x0627 || cp == 0x0671) return 'a';
    if (cp == 0x0624 || cp == 0x0648) return 'w';
    if (cp == 0x0626 || cp == 0x0649 || cp == 0x064a || cp == 0x06cc) return 'y';
    if (cp == 0x0628) return 'b';
    if (cp == 0x0629 || cp == 0x062a || cp == 0x0637) return 't';
    if (cp == 0x062b || cp == 0x0635 || cp == 0x0633 || cp == 0x0634) return 's';
    if (cp == 0x062c) return 'j';
    if (cp == 0x062d || cp == 0x0647 || cp == 0x06be || cp == 0x06c1) return 'h';
    if (cp == 0x062e || cp == 0x0643 || cp == 0x06a9) return 'k';
    if (cp == 0x062f || cp == 0x0630 || cp == 0x0636) return 'd';
    if (cp == 0x0631) return 'r';
    if (cp == 0x0632 || cp == 0x0638) return 'z';
    if (cp == 0x0639) return 'a';
    if (cp == 0x063a) return 'g';
    if (cp == 0x0641) return 'f';
    if (cp == 0x0642) return 'q';
    if (cp == 0x0644) return 'l';
    if (cp == 0x0645) return 'm';
    if (cp == 0x0646) return 'n';
    if (cp == 0x067e) return 'p';
    if (cp == 0x0686) return 'c';
    if (cp == 0x0698) return 'j';
    if (cp == 0x06af) return 'g';

    if ((cp >= 0x00c0 && cp <= 0x00c5) || (cp >= 0x0100 && cp <= 0x0105) ||
        cp == 0x01cd || cp == 0x01ce || cp == 0x0200 || cp == 0x0201 || cp == 0x0226 || cp == 0x0227)
        return (cp & 1) ? 'a' : 'A';
    if (cp == 0x00e0 || cp == 0x00e1 || cp == 0x00e2 || cp == 0x00e3 || cp == 0x00e4 || cp == 0x00e5)
        return 'a';
    if (cp == 0x00c6) return 'A';
    if (cp == 0x00e6) return 'a';
    if (cp == 0x00c7 || cp == 0x0106 || cp == 0x0108 || cp == 0x010a || cp == 0x010c) return 'C';
    if (cp == 0x00e7 || cp == 0x0107 || cp == 0x0109 || cp == 0x010b || cp == 0x010d) return 'c';
    if (cp == 0x00d0 || cp == 0x010e || cp == 0x0110) return 'D';
    if (cp == 0x00f0 || cp == 0x010f || cp == 0x0111) return 'd';
    if ((cp >= 0x00c8 && cp <= 0x00cb) || cp == 0x0112 || cp == 0x0114 || cp == 0x0116 || cp == 0x0118 || cp == 0x011a) return 'E';
    if ((cp >= 0x00e8 && cp <= 0x00eb) || cp == 0x0113 || cp == 0x0115 || cp == 0x0117 || cp == 0x0119 || cp == 0x011b) return 'e';
    if (cp == 0x011c || cp == 0x011e || cp == 0x0120 || cp == 0x0122) return 'G';
    if (cp == 0x011d || cp == 0x011f || cp == 0x0121 || cp == 0x0123) return 'g';
    if (cp == 0x0124 || cp == 0x0126) return 'H';
    if (cp == 0x0125 || cp == 0x0127) return 'h';
    if ((cp >= 0x00cc && cp <= 0x00cf) || cp == 0x0128 || cp == 0x012a || cp == 0x012c || cp == 0x012e || cp == 0x0130) return 'I';
    if ((cp >= 0x00ec && cp <= 0x00ef) || cp == 0x0129 || cp == 0x012b || cp == 0x012d || cp == 0x012f || cp == 0x0131) return 'i';
    if (cp == 0x0134) return 'J';
    if (cp == 0x0135) return 'j';
    if (cp == 0x0136) return 'K';
    if (cp == 0x0137 || cp == 0x0138) return 'k';
    if (cp == 0x0139 || cp == 0x013b || cp == 0x013d || cp == 0x013f || cp == 0x0141) return 'L';
    if (cp == 0x013a || cp == 0x013c || cp == 0x013e || cp == 0x0140 || cp == 0x0142) return 'l';
    if (cp == 0x00d1 || cp == 0x0143 || cp == 0x0145 || cp == 0x0147) return 'N';
    if (cp == 0x00f1 || cp == 0x0144 || cp == 0x0146 || cp == 0x0148 || cp == 0x0149) return 'n';
    if ((cp >= 0x00d2 && cp <= 0x00d6) || cp == 0x00d8 || cp == 0x014c || cp == 0x014e || cp == 0x0150) return 'O';
    if ((cp >= 0x00f2 && cp <= 0x00f6) || cp == 0x00f8 || cp == 0x014d || cp == 0x014f || cp == 0x0151) return 'o';
    if (cp == 0x0154 || cp == 0x0156 || cp == 0x0158) return 'R';
    if (cp == 0x0155 || cp == 0x0157 || cp == 0x0159) return 'r';
    if (cp == 0x015a || cp == 0x015c || cp == 0x015e || cp == 0x0160) return 'S';
    if (cp == 0x015b || cp == 0x015d || cp == 0x015f || cp == 0x0161 || cp == 0x00df) return 's';
    if (cp == 0x0162 || cp == 0x0164 || cp == 0x0166) return 'T';
    if (cp == 0x0163 || cp == 0x0165 || cp == 0x0167) return 't';
    if ((cp >= 0x00d9 && cp <= 0x00dc) || cp == 0x0168 || cp == 0x016a || cp == 0x016c || cp == 0x016e || cp == 0x0170 || cp == 0x0172) return 'U';
    if ((cp >= 0x00f9 && cp <= 0x00fc) || cp == 0x0169 || cp == 0x016b || cp == 0x016d || cp == 0x016f || cp == 0x0171 || cp == 0x0173) return 'u';
    if (cp == 0x00dd || cp == 0x0176 || cp == 0x0178) return 'Y';
    if (cp == 0x00fd || cp == 0x00ff || cp == 0x0177) return 'y';
    if (cp == 0x0179 || cp == 0x017b || cp == 0x017d) return 'Z';
    if (cp == 0x017a || cp == 0x017c || cp == 0x017e) return 'z';
    if (cp >= 0x0300 && cp <= 0x036f) return 0;   // combining mark; skip
    return '?';
}

static int glyph_adv(unsigned char ch, int scale) {
    int useAtlas = (scale >= 2 && scale <= FONT_MAXSCALE && FONT_DATA[scale] && FONT_ADV[scale]);
    return useAtlas ? FONT_ADV[scale][ch - FONT_FIRST] : 8 * scale;
}

int gfx_text_w(const char *s, int scale) {
    return gfx_text_tr_w(s, scale, default_track(scale));
}

// Blit one anti-aliased atlas glyph (cell x cell alpha map) in a single tight,
// pre-clipped pass. The previous path called gfx_blend() per pixel — a function
// call plus full bounds check for every one of the cell*cell pixels of every
// glyph, every frame. Clipping once and inlining the blend here is what makes
// the HUD/stats overlays nearly free to draw (no fps dip when they're up).
static void blit_glyph(Gfx *g, const unsigned char *gly, int cell, int dx, int dy, GfxColor c) {
    int W = g->width, H = g->height;
    int gx0 = dx < 0 ? -dx : 0, gy0 = dy < 0 ? -dy : 0;
    int gx1 = dx + cell > W ? W - dx : cell;
    int gy1 = dy + cell > H ? H - dy : cell;
    if (gx0 >= gx1 || gy0 >= gy1) return;
    uint32_t *fb = (uint32_t *)g->frameBuffers[g->activeIdx];
    uint32_t enc = encode(c);
    int cr = c.r, cg = c.g, cb = c.b;
    for (int gy = gy0; gy < gy1; gy++) {
        const unsigned char *row = gly + (size_t)gy * cell;
        uint32_t *drow = fb + (size_t)(dy + gy) * W + dx;
        for (int gx = gx0; gx < gx1; gx++) {
            int a = row[gx];
            if (!a) continue;
            if (a >= 255) { drow[gx] = enc; continue; }
            uint32_t e = drow[gx];
            int ia = 255 - a;
            uint32_t r = (cr * a + ((e >> 16) & 0xff) * ia) / 255;
            uint32_t gg = (cg * a + ((e >> 8) & 0xff) * ia) / 255;
            uint32_t b = (cb * a + (e & 0xff) * ia) / 255;
            drow[gx] = 0x80000000u | (r << 16) | (gg << 8) | b;
        }
    }
}

int gfx_text_tr(Gfx *g, int x, int y, const char *s, int scale, GfxColor c, int track) {
    // Anti-aliased proportional atlas for scale 2..6; scale 1 keeps the 8x8
    // bitmap. UTF-8 titles are folded to safe ASCII so metadata cannot corrupt
    // layout or walk the atlas out of bounds.
    int useAtlas = (scale >= 2 && scale <= FONT_MAXSCALE && FONT_DATA[scale] && FONT_ADV[scale]);
    int cell = 8 * scale;
    const unsigned char *atlas = useAtlas ? FONT_DATA[scale] : 0;
    int penX = x, drew = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (p && *p) {
        unsigned char ch = fold_cp(utf8_next(&p));
        if (!ch) continue;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        if (useAtlas) {
            const unsigned char *gly = atlas + (size_t)(ch - FONT_FIRST) * cell * cell;
            blit_glyph(g, gly, cell, penX, y, c);
        } else {
            const unsigned char *glyph = font8x8_basic[ch < 128 ? ch : '?'];
            for (int row = 0; row < 8; row++) {
                unsigned char bits = glyph[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (1 << col)) gfx_rect(g, penX + col * scale, y + row * scale, scale, scale, c);
            }
        }
        penX += glyph_adv(ch, scale) + track;
        drew = 1;
    }
    return penX - x - (drew ? track : 0);
}

int gfx_text_tr_w(const char *s, int scale, int track) {
    int w = 0, n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (p && *p) {
        unsigned char ch = fold_cp(utf8_next(&p));
        if (!ch) continue;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        w += glyph_adv(ch, scale);
        n++;
    }
    return w + (n > 0 ? (n - 1) * track : 0);
}

int gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c) {
    return gfx_text_tr(g, x, y, s, scale, c, default_track(scale));
}
