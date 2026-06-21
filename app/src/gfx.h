// gfx.h — minimal double-buffered framebuffer over sceVideoOut, plus 8x8 text.
// Trimmed from the OpenOrbis _common/graphics sample (proven init/flip sequence),
// rewritten in C and without the FreeType dependency.
#ifndef PS4CAST_GFX_H
#define PS4CAST_GFX_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    uint8_t r, g, b;
} GfxColor;

typedef struct {
    int width;
    int height;
    int depth;          // bytes per pixel (4)
    int video;          // sceVideoOut handle
    int activeIdx;      // current render target (cycles over the buffers)
    int frameBufferSize;
    off_t directMemOff;
    size_t directMemSize;
    uintptr_t videoMemSP;
    void *videoMem;
    void *frameBuffers[3];   // triple-buffered: pipeline CPU convert with scanout
    void *flipQueue;    // OrbisKernelEqueue (pointer-sized opaque handle)
    char attr[64];      // OrbisVideoOutBufferAttribute storage (over-sized, safe)
} Gfx;

// Lifecycle
int  gfx_init(Gfx *g, int width, int height);   // returns 0 on success
void gfx_present(Gfx *g, int frameID);          // submit flip + wait + swap
// Best-effort release of the display/GPU context before a fatal _exit, so the
// kernel can reclaim it and the process doesn't become unkillable. Idempotent.
void gfx_emergency_release(void);

// Drawing (operate on the active back buffer)
void gfx_clear(Gfx *g, GfxColor c);
void gfx_pixel(Gfx *g, int x, int y, GfxColor c);
void gfx_rect(Gfx *g, int x, int y, int w, int h, GfxColor c);

// Alpha-composited drawing for a modern, smooth look. `a` is 0..255 coverage
// blended over whatever is already in the buffer (read-modify-write), which is
// what gives anti-aliased edges and translucent panels.
void gfx_blend(Gfx *g, int x, int y, GfxColor c, int a);
void gfx_rect_a(Gfx *g, int x, int y, int w, int h, GfxColor c, int a);     // translucent fill
void gfx_circle(Gfx *g, int cx, int cy, int r, GfxColor c);                 // AA filled disc
void gfx_circle_a(Gfx *g, int cx, int cy, int r, GfxColor c, int a);        // AA filled disc, translucent
void gfx_round(Gfx *g, int x, int y, int w, int h, int r, GfxColor c);      // AA rounded-rect fill
void gfx_round_a(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a);
void gfx_vgrad(Gfx *g, int x, int y, int w, int h, GfxColor top, GfxColor bot); // vertical gradient
void gfx_tri(Gfx *g, int x0, int y0, int x1, int y1, int x2, int y2, GfxColor c); // filled triangle
void gfx_arc(Gfx *g, int cx, int cy, int r, int thick, int quad, GfxColor c);  // AA quarter-arc ring

// Text with letter-spacing (tracking, in pixels) for tidier headings.
int  gfx_text_tr(Gfx *g, int x, int y, const char *s, int scale, GfxColor c, int track);
int  gfx_text_tr_w(const char *s, int scale, int track);

// Text using the embedded 8x8 font. `scale` enlarges each glyph pixel into a
// scale*scale block. Returns the x advance in pixels.
int  gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c);
int  gfx_text_w(const char *s, int scale);      // measured pixel width

#endif
