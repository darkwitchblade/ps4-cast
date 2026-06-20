// vdec_probe.h — isolated libSceVideodec2 hardware-decode research probe.
//
// NOT wired into the player. Triggered via GET /vdecprobe so we can experiment
// with the (undocumented) sceVideodec2 API on hardware and read back exact
// return codes, without risking the working ffmpeg software path.
//
// Success criterion (deliberately minimal): create an H.264 decoder and decode
// one bundled IDR access unit to a valid frame (NV12) without crashing.
//
// The sceVideodec2 config enums (codecType/resourceType/profile) are not public,
// so every magic number is overridable via query string and there is a sweep
// mode that brute-forces codecType/resourceType through QueryDecoderMemoryInfo
// (which allocates nothing) to discover which values the library accepts.
#ifndef PS4CAST_VDEC_PROBE_H
#define PS4CAST_VDEC_PROBE_H

// Run the probe. `query` is the part of the URL after '?' (may be empty/NULL).
// Writes a human-readable multi-line report into out[]. Returns bytes written.
int vdec_probe_run(const char *query, char *out, int outcap);

#endif
