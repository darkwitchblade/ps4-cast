// vdec_hw.h — hardware H.264 decode via libSceVideodec2 (GPU-compute decoder).
//
// Proven by the /vdecprobe research path: creates a decoder, decodes Annex B
// H.264 access units to NV12 frames on the GPU silicon (CPU stays free), ~68fps
// sustained at 1080p. This wraps that recipe in a small player-facing API.
//
// IMPORTANT: vdec_hw_decode runs sceVideodec2Decode on the CALLER's thread and
// that thread MUST have a large stack (>=8MB) — the decoder uses ~31KB frames
// and overflows a default pthread stack. The player runs its decode thread with
// a big stack when hardware decode is active.
#ifndef PS4CAST_VDEC_HW_H
#define PS4CAST_VDEC_HW_H

#include <stdint.h>

typedef struct {
    const uint8_t *y;     // luma plane
    const uint8_t *uv;    // interleaved chroma plane (NV12)
    int width, height;    // coded dimensions
    int pitch;            // stride in bytes (luma; chroma shares it)
    int64_t pts, dts;     // timestamp of THIS output frame (popped from the FIFO)
} VdecHwFrame;

// Bring up the hardware decoder for H.264 up to maxW x maxH. profile/level are
// the H.264 SPS values (e.g. 100/40 for High@4.0); pass 0 to use safe maxima.
// Returns 0 on success, <0 if unavailable (module/entitlement/memory) — caller
// then falls back to software decode. Loads the module under GoldHEN elevation
// and restores before returning.
int  vdec_hw_open(int maxW, int maxH, int profile, int level);

// Decode one Annex B access unit (with its input PTS/DTS). Returns 1 if a frame
// is ready (filled into *out, valid until the next decode call — copy it out),
// 0 if no frame was produced yet, <0 on error. The decoder can return a delayed
// picture, so out->pts/dts come from an internal FIFO (the timestamp of the
// frame actually emitted), NOT necessarily the AU just submitted. MUST be called
// on a large-stack thread.
int  vdec_hw_decode(const uint8_t *au, int len, int64_t pts, int64_t dts, VdecHwFrame *out);

void vdec_hw_close(void);

// Reset decoder state (drop all reference frames) — call on seek so the next
// access unit decodes cleanly against a fresh sequence instead of stale refs.
void vdec_hw_reset(void);

// 1 if a hardware session is currently open.
int  vdec_hw_active(void);

// One-line diagnostic for /status.
const char *vdec_hw_debug(void);

// Outstanding direct (GPU) memory bytes held by the HW decoder. Should return to
// the same baseline after each vdec_hw_open/close cycle; growth = a dmem leak.
long vdec_hw_dmem_outstanding(void);

#endif
