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
    int activeIdx;      // 0/1 double buffer
    int frameBufferSize;
    off_t directMemOff;
    size_t directMemSize;
    uintptr_t videoMemSP;
    void *videoMem;
    void *frameBuffers[2];
    void *flipQueue;    // OrbisKernelEqueue (pointer-sized opaque handle)
    char attr[64];      // OrbisVideoOutBufferAttribute storage (over-sized, safe)
} Gfx;

// Lifecycle
int  gfx_init(Gfx *g, int width, int height);   // returns 0 on success
void gfx_present(Gfx *g, int frameID);          // submit flip + wait + swap

// Drawing (operate on the active back buffer)
void gfx_clear(Gfx *g, GfxColor c);
void gfx_pixel(Gfx *g, int x, int y, GfxColor c);
void gfx_rect(Gfx *g, int x, int y, int w, int h, GfxColor c);

// Text using the embedded 8x8 font. `scale` enlarges each glyph pixel into a
// scale*scale block. Returns the x advance in pixels.
int  gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c);
int  gfx_text_w(const char *s, int scale);      // measured pixel width

#endif
