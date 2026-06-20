#include "vdec_probe.h"
#include "goldhen.h"
#include "vdec_sample_h264.h"
#include "vdec_sample_1080.h"
#include "vdec_sample_gop.h"
#include "vdec_sample_hevc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>          // syscall()
#include <signal.h>
#include <setjmp.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

#define SYS_dynlib_load_prx  594

// Standard FreeBSD signal numbers (PS4 is FreeBSD-derived). Defined locally so
// we don't depend on which sys header pulls them in.
#ifndef SIGILL
#define SIGILL  4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGFPE
#define SIGFPE  8
#endif
#ifndef SIGBUS
#define SIGBUS  10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x0040
#endif

// In-process fault guard for the crash-prone sceVideodec2Decode call. If Decode
// raises a usermode CPU fault (SIGSEGV/SIGBUS/...), we catch it, record the
// signal + faulting address, and siglongjmp back instead of letting the kernel
// kill the app — so the probe still returns a report AND the app survives (no
// more crash-dialog + relaunch per test). A GPU-side hang can't be caught this
// way; if it still hard-crashes, that tells us the fault is asynchronous GPU.
static sigjmp_buf       g_vdecJmp;
static volatile int     g_vdecSig;
static volatile uintptr_t g_vdecAddr;

static void vdec_sig_handler(int sig, struct __siginfo *info, void *uap) {
    (void)uap;
    g_vdecSig  = sig;
    g_vdecAddr = info ? (uintptr_t)info->si_addr : 0;
    siglongjmp(g_vdecJmp, 1);
}

static const char *signame(int s) {
    switch (s) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    default:      return "SIG?";
    }
}

// ---- sceVideodec2 ABI (reverse-engineered struct layouts) ------------------
// The local OpenOrbis header only has void stubs, so we mirror shadPS4's
// reverse-engineered struct layouts and resolve the real functions at RUNTIME
// via sceKernelDlsym (NOT a static -lSceVideodec2 link, which would force the
// loader to auto-load that sysmodule unprivileged at boot and could brick
// launch). This keeps the probe entirely optional and the app's boot safe.

typedef struct {                 // 0x48
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    int32_t  maxFrameWidth;
    int32_t  maxFrameHeight;
    int32_t  maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void    *computeQueue;
    uint64_t cpuAffinityMask;
    int32_t  cpuThreadPriority;
    uint8_t  optimizeProgressiveVideo;
    uint8_t  checkMemoryType;
    uint8_t  reserved0;
    uint8_t  reserved1;
    void    *extraConfigInfo;
} Vdec2Config;

typedef struct {                 // 0x48
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void    *cpuMemory;
    uint64_t gpuMemorySize;
    void    *gpuMemory;
    uint64_t cpuGpuMemorySize;
    void    *cpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
    uint32_t reserved0;
} Vdec2MemInfo;

typedef struct {                 // 0x18
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void    *cpuGpuMemory;
} Vdec2ComputeMemInfo;

typedef struct {                 // 0x10
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    uint8_t  checkMemoryType;
    uint8_t  reserved0;
    uint16_t reserved1;
} Vdec2ComputeConfig;

typedef struct {                 // 0x30
    uint64_t thisSize;
    void    *auData;
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
} Vdec2Input;

typedef struct {                 // 0x20
    uint64_t thisSize;
    void    *frameBuffer;
    uint64_t frameBufferSize;
    uint8_t  isAccepted;
    uint8_t  _pad[7];
} Vdec2FrameBuffer;

typedef struct {                 // 0x38
    uint64_t thisSize;
    uint8_t  isValid;
    uint8_t  isErrorFrame;
    uint8_t  pictureCount;
    uint8_t  _pad0;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    uint32_t _pad1;
    void    *frameBuffer;
    uint64_t frameBufferSize;
    uint32_t frameFormat;
    uint32_t framePitchInBytes;
} Vdec2Output;

typedef int (*PFNQuery) (const Vdec2Config *, Vdec2MemInfo *);
typedef int (*PFNQueryCompute)(Vdec2ComputeMemInfo *);
typedef int (*PFNAllocCompute)(const Vdec2ComputeConfig *, const Vdec2ComputeMemInfo *, void **);
typedef int (*PFNReleaseCompute)(void *);
typedef int (*PFNCreate)(const Vdec2Config *, const Vdec2MemInfo *, void **);
typedef int (*PFNDelete)(void *);
typedef int (*PFNDecode)(void *, const Vdec2Input *, Vdec2FrameBuffer *, Vdec2Output *);

static PFNQuery  pQuery;
static PFNQueryCompute  pQueryCompute;
static PFNAllocCompute  pAllocCompute;
static PFNReleaseCompute pReleaseCompute;
static PFNCreate pCreate;
static PFNDelete pDelete;
static PFNDecode pDecode;
static int       g_bound;
static volatile int g_probe_busy;

// PS4 direct-memory types (ORBIS_KERNEL_WB_ONION=0, WC_GARLIC=3). player.c notes
// the hardware decoder needs CPU-cached, decoder-coherent WB_ONION memory, so we
// default every region to onion; types stay overridable for experiments.
#define MEM_ONION   0
#define MAP_PROT_RWALL 0x33

// Resolve the four sceVideodec2 entry points at runtime. Returns 0 on success.
static int bind_vdec2(char *err, int errcap) {
    if (g_bound) return 0;
    sceSysmoduleLoadModule((enum OrbisSysModule)0x00CF);   // VIDEODEC2
    sceSysmoduleLoadModule((enum OrbisSysModule)0x00D0);   // VDECWRAP

    const char *path = "/system/common/lib/libSceVideodec2.sprx";
    int handle = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        int mid = -1;
        syscall(SYS_dynlib_load_prx, (long)path, 0L, (long)&mid, 0L, 0L, 0L);
        handle = (mid >= 0) ? mid : handle;
    }
    if (handle < 0) { snprintf(err, errcap, "loadmodule failed 0x%x", (unsigned)handle); return -1; }

    sceKernelDlsym(handle, "sceVideodec2QueryDecoderMemoryInfo", (void **)&pQuery);
    sceKernelDlsym(handle, "sceVideodec2QueryComputeMemoryInfo", (void **)&pQueryCompute);
    sceKernelDlsym(handle, "sceVideodec2AllocateComputeQueue",   (void **)&pAllocCompute);
    sceKernelDlsym(handle, "sceVideodec2ReleaseComputeQueue",    (void **)&pReleaseCompute);
    sceKernelDlsym(handle, "sceVideodec2CreateDecoder",          (void **)&pCreate);
    sceKernelDlsym(handle, "sceVideodec2DeleteDecoder",          (void **)&pDelete);
    sceKernelDlsym(handle, "sceVideodec2Decode",                 (void **)&pDecode);
    if (!pQuery || !pQueryCompute || !pAllocCompute || !pReleaseCompute ||
        !pCreate || !pDecode || !pDelete) {
        snprintf(err, errcap, "dlsym fail h=0x%x q=%p qc=%p ac=%p rc=%p c=%p d=%p del=%p",
                 (unsigned)handle, (void *)pQuery, (void *)pQueryCompute,
                 (void *)pAllocCompute, (void *)pReleaseCompute,
                 (void *)pCreate, (void *)pDecode, (void *)pDelete);
        return -2;
    }
    g_bound = 1;
    return 0;
}

// ---- small helpers --------------------------------------------------------

typedef struct { off_t off; size_t size; void *va; } DMem;

static int dmem_alloc(DMem *m, size_t size, int memtype) {
    size_t align = 0x4000;                       // direct-memory page alignment
    if (size == 0) { m->off = 0; m->size = 0; m->va = NULL; return 0; }
    size = (size + align - 1) / align * align;
    m->off = 0; m->size = size; m->va = NULL;
    off_t off = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, memtype, &off) < 0)
        return -1;
    void *va = NULL;
    if (sceKernelMapDirectMemory(&va, size, MAP_PROT_RWALL, 0, off, align) < 0) {
        sceKernelReleaseDirectMemory(off, size);
        return -2;
    }
    m->off = off; m->va = va;
    return 0;
}
static void dmem_free(DMem *m) {
    if (m->va)   { sceKernelMunmap(m->va, m->size); m->va = NULL; }
    if (m->size) { sceKernelReleaseDirectMemory(m->off, m->size); m->size = 0; }
}

// Extract an integer query param: key=NN (decimal) or key=0xNN (hex).
static int qparam(const char *q, const char *key, int def) {
    if (!q) return def;
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(q, pat);
    if (!p) return def;
    p += strlen(pat);
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    return (int)strtol(p, NULL, base);
}

static uint64_t qparam64(const char *q, const char *key, uint64_t def) {
    if (!q) return def;
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(q, pat);
    if (!p) return def;
    p += strlen(pat);
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    return strtoull(p, NULL, base);
}

static const char *vdec2_errname(int rc) {
    switch ((unsigned)rc) {
    case 0x00000000: return "OK";
    case 0x811d0100: return "API_FAIL";
    case 0x811d0101: return "STRUCT_SIZE";
    case 0x811d0102: return "ARGUMENT_POINTER";
    case 0x811d0103: return "DECODER_INSTANCE";
    case 0x811d0104: return "MEMORY_SIZE";
    case 0x811d0105: return "MEMORY_POINTER";
    case 0x811d0106: return "FRAME_BUFFER_SIZE";
    case 0x811d0107: return "FRAME_BUFFER_POINTER";
    case 0x811d0108: return "FRAME_BUFFER_ALIGNMENT";
    case 0x811d0109: return "NOT_ONION_MEMORY";
    case 0x811d010a: return "NOT_GARLIC_MEMORY";
    case 0x811d010b: return "NOT_DIRECT_MEMORY";
    case 0x811d010c: return "MEMORY_INFO";
    case 0x811d010d: return "ACCESS_UNIT_SIZE";
    case 0x811d010e: return "ACCESS_UNIT_POINTER";
    case 0x811d010f: return "OUTPUT_INFO";
    case 0x811d0110: return "COMPUTE_QUEUE";
    case 0x811d0111: return "FATAL_STATE";
    case 0x811d0112: return "PRESET_VALUE";
    case 0x811d0200: return "CONFIG_INFO";
    case 0x811d0201: return "COMPUTE_PIPE_ID";
    case 0x811d0202: return "COMPUTE_QUEUE_ID";
    case 0x811d0203: return "RESOURCE_TYPE";
    case 0x811d0204: return "CODEC_TYPE";
    case 0x811d0205: return "PROFILE_LEVEL";
    case 0x811d0206: return "PIPELINE_DEPTH";
    case 0x811d0207: return "AFFINITY_MASK";
    case 0x811d0208: return "THREAD_PRIORITY";
    case 0x811d0209: return "DPB_FRAME_COUNT";
    case 0x811d020a: return "FRAME_WIDTH_HEIGHT";
    case 0x811d020b: return "EXTRA_CONFIG_INFO";
    case 0x811d0300: return "NEW_SEQUENCE";
    case 0x811d0301: return "ACCESS_UNIT";
    case 0x811d0302: return "OVERSIZE_DECODE";
    case 0x811d0303: return "INVALID_SEQUENCE";
    case 0x811d0304: return "FATAL_STREAM";
    default: return "UNKNOWN";
    }
}

static int qhas_value(const char *q, const char *key, const char *value) {
    if (!q) return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(q, pat);
    if (!p) return 0;
    p += strlen(pat);
    return strncmp(p, value, strlen(value)) == 0;
}

static int find_start_code(const uint8_t *p, int off, int size, int *sc_len) {
    for (int i = off; i + 3 < size; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            *sc_len = 3;
            return i;
        }
        if (i + 4 < size && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            *sc_len = 4;
            return i;
        }
    }
    return -1;
}

static int build_au_variant(uint8_t *dst, int cap, int avcc, int stripsei,
                            const uint8_t *src, int size) {
    int out = 0;
    int sc_len = 0;
    int sc = find_start_code(src, 0, size, &sc_len);
    while (sc >= 0) {
        int nal = sc + sc_len;
        int next_len = 0;
        int next = find_start_code(src, nal, size, &next_len);
        int end = (next >= 0) ? next : size;
        while (end > nal && src[end - 1] == 0) end--;
        int nal_size = end - nal;
        int nal_type = nal_size > 0 ? (src[nal] & 0x1f) : -1;

        if (nal_size > 0 && !(stripsei && nal_type == 6)) {
            if (avcc) {
                if (out + 4 + nal_size > cap) return -1;
                dst[out++] = (uint8_t)((nal_size >> 24) & 0xff);
                dst[out++] = (uint8_t)((nal_size >> 16) & 0xff);
                dst[out++] = (uint8_t)((nal_size >> 8) & 0xff);
                dst[out++] = (uint8_t)(nal_size & 0xff);
            } else {
                if (out + 4 + nal_size > cap) return -1;
                dst[out++] = 0; dst[out++] = 0; dst[out++] = 0; dst[out++] = 1;
            }
            memcpy(dst + out, src + nal, nal_size);
            out += nal_size;
        }

        if (next < 0) break;
        sc = next;
        sc_len = next_len;
    }
    return out;
}

// Persist a buffer to /data so a hard (uncatchable, e.g. GPU) crash still leaves
// the pre-decode report on disk for the next boot to read back via /vdeclog.
#define VDEC_LOG_PATH "/data/ps4cast_vdec.log"
static void persist_log(const char *buf, int len) {
    int fd = sceKernelOpen(VDEC_LOG_PATH, 0x0201 /*WRONLY|CREAT*/ | 0x0400 /*TRUNC*/, 0666);
    if (fd < 0) return;
    sceKernelWrite(fd, buf, (size_t)len);
    sceKernelClose(fd);
}

extern uint64_t sceKernelGetProcessTime(void);   // microseconds, monotonic

// Decode benchmark: run pDecode N times on a mutable copy of the input (varying
// PTS), timing each call. Iteration 0 is warmup (decoder/GPU init); steady-state
// stats are collected over iterations >=1 so we report the real per-frame cost.
typedef struct {
    void *decoder; const Vdec2Input *in; Vdec2FrameBuffer *fbi; Vdec2Output *od;
    int guard; int loop;
    // GOP mode (sequential I+P decode through a rotating frame-buffer pool)
    int gop; const uint8_t *gopbuf; void **fbpool; int poolN; uint64_t fbSize;
    // single-AU results
    int rc; int faulted; int iters;
    uint64_t warmup_us, sum_us, min_us, max_us;
    // GOP results
    uint64_t i_us, p_sum, p_min, p_max; int p_count; int frames_done;
    unsigned lastW, lastH; int lastValid; void *lastFb; unsigned lastPitch;
} DecodeArgs;

static void run_decode_loop(DecodeArgs *a) {
    Vdec2Input li = *a->in;                   // mutable copy (vary PTS per call)
    a->min_us = (uint64_t)-1; a->max_us = 0; a->sum_us = 0;
    a->warmup_us = 0; a->iters = 0; a->rc = 0;
    int loop = a->loop < 1 ? 1 : a->loop;
    for (int i = 0; i < loop; i++) {
        li.ptsData = (uint64_t)i; li.dtsData = (uint64_t)i;
        uint64_t t0 = sceKernelGetProcessTime();
        a->rc = pDecode(a->decoder, &li, a->fbi, a->od);
        uint64_t dt = sceKernelGetProcessTime() - t0;
        a->iters = i + 1;
        if (a->rc != 0) break;                // stop the benchmark on first error
        if (i == 0) { a->warmup_us = dt; }
        else {
            a->sum_us += dt;
            if (dt < a->min_us) a->min_us = dt;
            if (dt > a->max_us) a->max_us = dt;
        }
    }
    if (a->min_us == (uint64_t)-1) a->min_us = a->warmup_us;   // loop==1 case
}

// Decode the bundled GOP (I + P...) in order, rotating output buffers through a
// pool so reference frames stay valid. Times each frame and splits stats by type
// (I vs P) — this is the realistic measurement the all-IDR loop couldn't give.
static void run_gop_loop(DecodeArgs *a) {
    Vdec2Input li = *a->in;
    Vdec2Output od;
    a->i_us = 0; a->p_sum = 0; a->p_min = (uint64_t)-1; a->p_max = 0;
    a->p_count = 0; a->frames_done = 0; a->rc = 0;
    for (int i = 0; i < g_vdecGopCount; i++) {
        const VdecGopFrame *fr = &g_vdecGopFrames[i];
        li.auData = (void *)(a->gopbuf + fr->offset);
        li.auSize = fr->size;
        li.ptsData = (uint64_t)i; li.dtsData = (uint64_t)i;
        Vdec2FrameBuffer fbi; memset(&fbi, 0, sizeof(fbi));
        fbi.thisSize = sizeof(fbi);
        fbi.frameBuffer = a->fbpool[i % a->poolN];
        fbi.frameBufferSize = a->fbSize;
        memset(&od, 0, sizeof(od)); od.thisSize = sizeof(od);
        uint64_t t0 = sceKernelGetProcessTime();
        a->rc = pDecode(a->decoder, &li, &fbi, &od);
        uint64_t dt = sceKernelGetProcessTime() - t0;
        a->frames_done = i + 1;
        if (a->rc != 0) break;
        if (fr->type == 'I') a->i_us = dt;
        else { a->p_sum += dt; if (dt < a->p_min) a->p_min = dt; if (dt > a->p_max) a->p_max = dt; a->p_count++; }
        a->lastW = od.frameWidth; a->lastH = od.frameHeight;
        a->lastPitch = od.framePitch; a->lastValid = od.isValid; a->lastFb = od.frameBuffer;
    }
    if (a->p_min == (uint64_t)-1) a->p_min = 0;
}

// Run on a dedicated thread with a LARGE stack (the decoder uses ~31KB frames
// that overflow the small httpd stack -> page fault). The CPU-fault guard wraps
// the whole run on the worker thread. Caller fills `a`, reads results from `a`.
static void *decode_thread_fn(void *p) {
    DecodeArgs *a = (DecodeArgs *)p;
    if (a->guard) {
        int sigs[6] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT };
        struct sigaction old[6], sa;
        memset(&sa, 0, sizeof(sa));
        sa.__sa_handler.__sa_sigaction = vdec_sig_handler;   // header's sa_sigaction macro is broken
        sa.sa_flags = SA_SIGINFO;
        for (int i = 0; i < 6; i++) sigaction(sigs[i], &sa, &old[i]);
        g_vdecSig = 0; g_vdecAddr = 0;
        if (sigsetjmp(g_vdecJmp, 1) == 0) { if (a->gop) run_gop_loop(a); else run_decode_loop(a); }
        else a->faulted = 1;
        for (int i = 0; i < 6; i++) sigaction(sigs[i], &old[i], NULL);
    } else {
        if (a->gop) run_gop_loop(a); else run_decode_loop(a);
    }
    return NULL;
}

// Returns 0 if the decode thread ran (results in *a), -1 if it couldn't start.
static int run_on_big_stack(DecodeArgs *a, size_t stackSize) {
    OrbisPthreadAttr attr;
    if (scePthreadAttrInit(&attr) != 0) return -1;
    scePthreadAttrSetstacksize(&attr, stackSize);
    OrbisPthread th;
    int cr = scePthreadCreate(&th, &attr, decode_thread_fn, a, "ps4cast_vdecdec");
    scePthreadAttrDestroy(&attr);
    if (cr != 0) return -1;
    scePthreadJoin(th, NULL);
    return 0;
}

// Sanity-check the decoded NV12 surface: stride-sample the Y plane for min/max/avg
// (a real picture has spread; a zeroed buffer is flat) and show a few raw bytes
// from the Y and interleaved-UV planes.
static int dump_nv12(char *out, int cap, const uint8_t *buf, int w, int h, int pitch) {
    if (!buf || w <= 0 || h <= 0 || pitch <= 0) return 0;
    int ymin = 255, ymax = 0, count = 0; unsigned long ysum = 0;
    for (int y = 0; y < h; y += 8) {
        const uint8_t *row = buf + (size_t)y * pitch;
        for (int x = 0; x < w; x += 8) {
            uint8_t v = row[x];
            if (v < ymin) ymin = v;
            if (v > ymax) ymax = v;
            ysum += v; count++;
        }
    }
    int yavg = count ? (int)(ysum / count) : 0;
    int n = 0;
    n += snprintf(out + n, cap - n,
                  "NV12 Y-plane: min=%d max=%d avg=%d spread=%d => %s\n",
                  ymin, ymax, yavg, ymax - ymin,
                  (ymax - ymin) > 8 ? "REAL IMAGE DATA" : "FLAT (suspect empty)");
    n += snprintf(out + n, cap - n, "  Y[0..15]:");
    for (int i = 0; i < 16; i++) n += snprintf(out + n, cap - n, " %02x", buf[i]);
    const uint8_t *uv = buf + (size_t)h * pitch;          // interleaved UV plane
    n += snprintf(out + n, cap - n, "\n  UV[0..15]:");
    for (int i = 0; i < 16; i++) n += snprintf(out + n, cap - n, " %02x", uv[i]);
    n += snprintf(out + n, cap - n, "\n");
    return n;
}

// ---- the probe ------------------------------------------------------------

static void fill_config(Vdec2Config *c, const char *q) {
    memset(c, 0, sizeof(*c));
    int hevcMode = qparam(q, "hevc", 0) || qparam(q, "hevcsweep", 0) || qhas_value(q, "src", "hevc");
    c->thisSize             = sizeof(*c);
    c->resourceType         = qparam(q, "rtype",   0);
    c->codecType            = qparam(q, "codec",   1);
    c->profile              = qparam(q, "profile", hevcMode ? 1 : 0);
    c->maxLevel             = qparam(q, "level",   hevcMode ? 120 : 0);
    c->maxFrameWidth        = qparam(q, "w",    hevcMode ? 640 : 1920);
    c->maxFrameHeight       = qparam(q, "h",    hevcMode ? 360 : 1080);
    c->maxDpbFrameCount     = qparam(q, "dpb",     5);
    c->decodePipelineDepth  = qparam(q, "depth",   4);
    c->cpuAffinityMask      = qparam64(q, "affinity", 0x3f);
    c->cpuThreadPriority    = qparam(q, "prio", 0x2bc);
    c->checkMemoryType      = (uint8_t)qparam(q, "checkmem", 0);
}

// Sweep codecType x resourceType through QueryDecoderMemoryInfo (allocates
// nothing, decodes nothing — lowest crash risk) to discover which combinations
// the library accepts (rc==0).
static int sweep(const char *q, char *out, int cap) {
    int n = 0;
    int maxCodec = qparam(q, "maxcodec", 16);
    if (maxCodec < 0) maxCodec = 0;
    if (maxCodec > 64) maxCodec = 64;
    n += snprintf(out + n, cap - n, "VDEC2 SWEEP (QueryDecoderMemoryInfo rc per codec x rtype, codec=0..%d)\n", maxCodec);
    for (int codec = 0; codec <= maxCodec && n < cap - 140; codec++) {
        n += snprintf(out + n, cap - n, "codec=%-2d:", codec);
        for (int rtype = 0; rtype <= 3; rtype++) {
            Vdec2Config c; fill_config(&c, q);
            c.codecType = codec; c.resourceType = rtype;
            Vdec2MemInfo mi; memset(&mi, 0, sizeof(mi)); mi.thisSize = sizeof(mi);
            int rc = pQuery(&c, &mi);
            n += snprintf(out + n, cap - n, " r%d=0x%08x(%s)", rtype, (unsigned)rc, vdec2_errname(rc));
        }
        n += snprintf(out + n, cap - n, "\n");
    }
    return n;
}

int vdec_probe_run(const char *query, char *out, int outcap) {
    if (__sync_lock_test_and_set(&g_probe_busy, 1)) {
        return snprintf(out, outcap, "BUSY: a vdec probe is already running. Try again in a few seconds.\n");
    }

    const char *q = query ? query : "";
    int n = 0;
    int rc = 0;
    void *decoder = NULL;
    void *computeQueue = NULL;
    DMem cpu = {0}, gpu = {0}, cgpu = {0}, fb = {0}, au = {0}, computeMem = {0};

    int elev = qparam(q, "elev", 1);
    char gh[160] = "";
    if (elev) goldhen_enter(gh, sizeof(gh));

#define VDEC_RETURN() do { __sync_lock_release(&g_probe_busy); return n; } while (0)

    char berr[200] = "";
    int brc = bind_vdec2(berr, sizeof(berr));
    n += snprintf(out + n, outcap - n, "bind sceVideodec2: rc=%d %s elev='%s'\n",
                  brc, brc ? berr : "ok", gh);
    if (brc != 0) {
        n += snprintf(out + n, outcap - n,
                      "ABORT: could not resolve the library (entitlement / module not loadable).\n");
        if (elev) goldhen_restore(NULL, 0);
        VDEC_RETURN();
    }

    if (qparam(q, "sweep", 0) || qparam(q, "hevcsweep", 0)) {
        if (qparam(q, "hevcsweep", 0)) {
            n += snprintf(out + n, outcap - n,
                          "HEVC sweep mode: 640x360/profile=1/level=120. Decode candidates with hevc=1&codec=<OK>&rtype=<OK>&gpumem=3&danger=1.\n");
        }
        n += sweep(q, out + n, outcap - n);
        if (elev) goldhen_restore(NULL, 0);
        VDEC_RETURN();
    }
    int stage = qparam(q, "stage", 99);
    if (stage <= 1) {
        n += snprintf(out + n, outcap - n, "STOP at requested stage 1 (bind only).\n");
        if (elev) goldhen_restore(NULL, 0);
        VDEC_RETURN();
    }

    // Single configured attempt.
    Vdec2Config cfg; fill_config(&cfg, q);
    n += snprintf(out + n, outcap - n,
                  "config: rtype=%u codec=%u profile=%u level=%u %dx%d dpb=%d depth=%u affinity=0x%llx prio=%d checkmem=%d\n",
                  cfg.resourceType, cfg.codecType, cfg.profile, cfg.maxLevel,
                  cfg.maxFrameWidth, cfg.maxFrameHeight, cfg.maxDpbFrameCount,
                  cfg.decodePipelineDepth, (unsigned long long)cfg.cpuAffinityMask,
                  cfg.cpuThreadPriority, cfg.checkMemoryType);

    Vdec2ComputeMemInfo cmi; memset(&cmi, 0, sizeof(cmi)); cmi.thisSize = sizeof(cmi);
    Vdec2MemInfo mi;
    if (qparam(q, "nocompute", 0)) {
        n += snprintf(out + n, outcap - n, "nocompute=1: skipping compute queue allocation.\n");
        goto query_decoder;
    }
    rc = pQueryCompute(&cmi);
    n += snprintf(out + n, outcap - n,
                  "QueryComputeMemoryInfo: rc=0x%08x(%s) cpugpu=%lluKB\n",
                  (unsigned)rc, vdec2_errname(rc), (unsigned long long)(cmi.cpuGpuMemorySize / 1024));
    if (rc != 0) { n += snprintf(out + n, outcap - n, "STOP at QueryCompute.\n"); goto done; }
    if (stage <= 2) { n += snprintf(out + n, outcap - n, "STOP at requested stage 2.\n"); goto done; }

    if (!qparam(q, "danger", 0)) {
        n += snprintf(out + n, outcap - n,
                      "STOP: stages >=3 allocate/own decoder compute resources and crashed this console once.\n"
                      "Re-run with danger=1 only for a deliberate one-at-a-time hardware-decoder experiment.\n");
        goto done;
    }

    int compT = qparam(q, "compmem", MEM_ONION);
    int ec = dmem_alloc(&computeMem, cmi.cpuGpuMemorySize, compT);
    n += snprintf(out + n, outcap - n, "compute dmem(t%d)=%d\n", compT, ec);
    if (ec) { n += snprintf(out + n, outcap - n, "STOP: compute dmem alloc failed.\n"); goto done; }
    cmi.cpuGpuMemory = computeMem.va;
    if (stage <= 3) { n += snprintf(out + n, outcap - n, "STOP at requested stage 3.\n"); goto done; }

    Vdec2ComputeConfig ccfg; memset(&ccfg, 0, sizeof(ccfg));
    ccfg.thisSize = sizeof(ccfg);
    ccfg.computePipeId = (uint16_t)qparam(q, "pipe", 0);
    ccfg.computeQueueId = (uint16_t)qparam(q, "queue", 0);
    ccfg.checkMemoryType = (uint8_t)qparam(q, "checkmem", 0);
    rc = pAllocCompute(&ccfg, &cmi, &computeQueue);
    n += snprintf(out + n, outcap - n,
                  "AllocateComputeQueue: rc=0x%08x(%s) pipe=%u queue=%u q=%p\n",
                  (unsigned)rc, vdec2_errname(rc), ccfg.computePipeId, ccfg.computeQueueId, computeQueue);
    if (rc != 0 || !computeQueue) { n += snprintf(out + n, outcap - n, "STOP at AllocateComputeQueue.\n"); goto done; }
    cfg.computeQueue = computeQueue;
    if (stage <= 4) { n += snprintf(out + n, outcap - n, "STOP at requested stage 4.\n"); goto done; }

query_decoder:
    memset(&mi, 0, sizeof(mi)); mi.thisSize = sizeof(mi);
    // Query after compute-queue allocation too: some firmware paths size decoder
    // memory based on cfg.computeQueue/resource ownership.
    rc = pQuery(&cfg, &mi);
    n += snprintf(out + n, outcap - n,
                  "QueryDecoderMemoryInfo: rc=0x%08x(%s) cpu=%lluKB gpu=%lluKB cpugpu=%lluKB fb=%lluKB align=0x%x\n",
                  (unsigned)rc, vdec2_errname(rc),
                  (unsigned long long)(mi.cpuMemorySize / 1024),
                  (unsigned long long)(mi.gpuMemorySize / 1024),
                  (unsigned long long)(mi.cpuGpuMemorySize / 1024),
                  (unsigned long long)(mi.maxFrameBufferSize / 1024),
                  (unsigned)mi.frameBufferAlignment);
    if (rc != 0) { n += snprintf(out + n, outcap - n, "STOP at Query.\n"); goto done; }
    if (stage <= 5) { n += snprintf(out + n, outcap - n, "STOP at requested stage 5.\n"); goto done; }
    if (qparam(q, "nocompute", 0) && !qparam(q, "danger", 0)) {
        n += snprintf(out + n, outcap - n,
                      "STOP: nocompute create/decode is disabled unless danger=1.\n");
        goto done;
    }

    int cpuT = qparam(q, "cpumem", MEM_ONION);
    int gpuT = qparam(q, "gpumem", MEM_ONION);
    int cgT  = qparam(q, "cgmem",  MEM_ONION);
    int fbT  = qparam(q, "fbmem",  MEM_ONION);
    int e1 = dmem_alloc(&cpu,  mi.cpuMemorySize, cpuT);
    int e2 = dmem_alloc(&gpu,  mi.gpuMemorySize, gpuT);
    int e3 = dmem_alloc(&cgpu, mi.cpuGpuMemorySize, cgT);
    uint64_t fbSize = (uint64_t)qparam(q, "fbsz", 0);
    if (!fbSize) fbSize = mi.maxFrameBufferSize ? mi.maxFrameBufferSize : 0x400000;
    int e4 = dmem_alloc(&fb,   fbSize, fbT);
    int e5 = dmem_alloc(&au,   0x40000, MEM_ONION);   // 256KB: fits a 1080p IDR
    n += snprintf(out + n, outcap - n,
                  "dmem alloc: cpu(t%d)=%d gpu(t%d)=%d cpugpu(t%d)=%d fb(t%d,%lluKB)=%d au=%d\n",
                  cpuT, e1, gpuT, e2, cgT, e3, fbT,
                  (unsigned long long)(fbSize / 1024), e4, e5);
    if (e1 || e2 || e3 || e4 || e5) { n += snprintf(out + n, outcap - n, "STOP: dmem alloc failed.\n"); goto done; }
    if (stage <= 6) { n += snprintf(out + n, outcap - n, "STOP at requested stage 6.\n"); goto done; }

    mi.cpuMemory    = cpu.va;
    mi.gpuMemory    = gpu.va;
    mi.cpuGpuMemory = cgpu.va;

    rc = pCreate(&cfg, &mi, &decoder);
    n += snprintf(out + n, outcap - n, "CreateDecoder: rc=0x%08x(%s) decoder=%p\n",
                  (unsigned)rc, vdec2_errname(rc), decoder);
    if (rc != 0 || !decoder) { n += snprintf(out + n, outcap - n, "STOP at CreateDecoder.\n"); goto done; }
    if (stage <= 7) { n += snprintf(out + n, outcap - n, "STOP at requested stage 7.\n"); goto done; }

    // GOP benchmark: decode the bundled 30-frame I+P sequence through a rotating
    // frame-buffer pool. This gives REAL per-frame-type timing (the all-IDR loop
    // could not) — the honest answer to "how fast in actual playback".
    if (qparam(q, "gop", 0)) {
        DMem gopm = {0};
        if (dmem_alloc(&gopm, g_vdecGop_len, MEM_ONION) != 0) {
            n += snprintf(out + n, outcap - n, "STOP: gop dmem alloc failed.\n"); goto done; }
        memcpy(gopm.va, g_vdecGop, g_vdecGop_len);
        int poolN = qparam(q, "pool", 6); if (poolN < 2) poolN = 2; if (poolN > 12) poolN = 12;
        uint64_t fbSize = mi.maxFrameBufferSize ? mi.maxFrameBufferSize : 0x400000;
        int fbT = qparam(q, "fbmem", MEM_ONION);
        DMem pool[12] = {0}; void *poolva[12]; int perr = 0;
        for (int i = 0; i < poolN; i++) {
            if (dmem_alloc(&pool[i], fbSize, fbT) != 0) { perr = 1; break; }
            poolva[i] = pool[i].va;
        }
        if (perr) {
            for (int i = 0; i < poolN; i++) dmem_free(&pool[i]); dmem_free(&gopm);
            n += snprintf(out + n, outcap - n, "STOP: frame-buffer pool alloc failed (need %d x %lluKB).\n",
                          poolN, (unsigned long long)(fbSize / 1024));
            goto done;
        }
        Vdec2Input gin; memset(&gin, 0, sizeof(gin)); gin.thisSize = sizeof(gin);
        int guard = qparam(q, "guard", 1);
        size_t stackSz = (size_t)qparam(q, "stackkb", 8192) * 1024;
        DecodeArgs st; memset(&st, 0, sizeof(st));
        st.decoder = decoder; st.in = &gin; st.guard = guard; st.gop = 1;
        st.gopbuf = (const uint8_t *)gopm.va; st.fbpool = poolva; st.poolN = poolN; st.fbSize = fbSize;
        n += snprintf(out + n, outcap - n,
                      "GOP benchmark: %d frames (1 I + %d P), pool=%d, fb=%lluKB(t%d), stack=%uKB\n",
                      g_vdecGopCount, g_vdecGopCount - 1, poolN,
                      (unsigned long long)(fbSize / 1024), fbT, (unsigned)(stackSz / 1024));
        persist_log(out, n);
        int gtrc = run_on_big_stack(&st, stackSz);
        for (int i = 0; i < poolN; i++) dmem_free(&pool[i]);
        dmem_free(&gopm);
        if (gtrc != 0) { n += snprintf(out + n, outcap - n, "STOP: gop thread create failed.\n"); goto done; }
        if (st.faulted) {
            n += snprintf(out + n, outcap - n, "GOP FAULTED at frame %d: %s addr=0x%lx\n",
                          st.frames_done, signame(g_vdecSig), (unsigned long)g_vdecAddr);
        } else if (st.rc != 0) {
            n += snprintf(out + n, outcap - n, "GOP stopped at frame %d: rc=0x%08x(%s)\n",
                          st.frames_done, (unsigned)st.rc, vdec2_errname(st.rc));
        } else {
            unsigned long iu = (unsigned long)st.i_us;
            unsigned long pavg = st.p_count ? (unsigned long)(st.p_sum / st.p_count) : 0;
            unsigned long gopUs = (unsigned long)(st.i_us + st.p_sum);
            unsigned long realfps = gopUs ? (unsigned long)((uint64_t)st.frames_done * 1000000ULL / gopUs) : 0;
            n += snprintf(out + n, outcap - n,
                          "GOP OK: %d/%d frames, last %ux%u valid=%d\n"
                          "  I-frame : %lu us (%lu fps)\n"
                          "  P-frame : avg=%lu us min=%lu max=%lu (%lu fps)  [%d frames]\n"
                          "  REAL-WORLD sustained (1 I + %d P): ~%lu fps, CPU-free\n",
                          st.frames_done, g_vdecGopCount, st.lastW, st.lastH, st.lastValid,
                          iu, iu ? 1000000UL / iu : 0,
                          pavg, (unsigned long)st.p_min, (unsigned long)st.p_max,
                          pavg ? 1000000UL / pavg : 0, st.p_count,
                          st.p_count, realfps);
        }
        persist_log(out, n);
        goto done;
    }

    // Feed the bundled IDR access unit. Keep direct memory as the default, but
    // allow heap AU probes: the real API may copy AU bytes synchronously rather
    // than DMA-read them, and a heap pointer lets us test that ABI assumption.
    uint8_t *heapAu = NULL;
    void *auPtr = au.va;
    if (qparam(q, "auheap", 0)) {
        heapAu = (uint8_t *)malloc(0x40000);
        if (!heapAu) { n += snprintf(out + n, outcap - n, "STOP: heap AU alloc failed.\n"); goto done; }
        auPtr = heapAu;
    }
    int auAvcc = qhas_value(q, "auform", "avcc");
    int auStripSei = qparam(q, "stripsei", 0);
    // Pick the bundled sample:
    //   default / src=360 : H.264 640x360 baseline
    //   src=1080          : H.264 1080p High
    //   src=hevc          : HEVC 640x360 intra AU (research only; codec enum unknown)
    const uint8_t *auSrc = g_vdecSampleH264; int auSrcLen = (int)g_vdecSampleH264_len;
    const char *sampleName = "h264-360";
    if (qparam(q, "src", 0) == 1080) { auSrc = g_vdecSample1080; auSrcLen = (int)g_vdecSample1080_len; }
    if (qhas_value(q, "src", "hevc") || qparam(q, "src", 0) == 265 || qparam(q, "hevc", 0)) {
        auSrc = g_vdecSampleHevc;
        auSrcLen = (int)g_vdecSampleHevc_len;
        sampleName = "hevc-360";
    } else if (qparam(q, "src", 0) == 1080) {
        sampleName = "h264-1080";
    }
    int auSize = build_au_variant((uint8_t *)auPtr, 0x40000, auAvcc, auStripSei, auSrc, auSrcLen);
    if (auSize <= 0) { n += snprintf(out + n, outcap - n, "STOP: AU variant build failed (%d).\n", auSize); goto done; }
    Vdec2Input in; memset(&in, 0, sizeof(in));
    in.thisSize = sizeof(in);
    in.auData   = auPtr;
    in.auSize   = (uint64_t)auSize;

    Vdec2FrameBuffer fbi; memset(&fbi, 0, sizeof(fbi));
    fbi.thisSize = sizeof(fbi);
    fbi.frameBuffer = fb.va;
    fbi.frameBufferSize = fb.size;

    Vdec2Output od; memset(&od, 0, sizeof(od));
    od.thisSize = (uint64_t)qparam(q, "outsz", sizeof(od));

    n += snprintf(out + n, outcap - n,
                  "Decode input: sample=%s au=%p heap=%d ausz=%u fb=%p fbsz=%llu outsz=0x%llx\n",
                  sampleName, auPtr, qparam(q, "auheap", 0), (unsigned)auSize,
                  fb.va, (unsigned long long)fbi.frameBufferSize,
                  (unsigned long long)od.thisSize);
    int preview = auSize < 16 ? auSize : 16;
    n += snprintf(out + n, outcap - n, "AU first%d:", preview);
    for (int i = 0; i < preview && n < outcap - 8; i++) n += snprintf(out + n, outcap - n, " %02x", ((uint8_t *)auPtr)[i]);
    n += snprintf(out + n, outcap - n, "\n");
    if (stage <= 8) { n += snprintf(out + n, outcap - n, "STOP at requested stage 8 (pre-decode).\n"); goto done; }

    // Persist everything up to the call: if Decode hard-crashes uncatchably
    // (async GPU), the report survives on /data and /vdeclog reads it next boot.
    n += snprintf(out + n, outcap - n, ">>> calling sceVideodec2Decode now...\n");
    persist_log(out, n);

    // Run on a big-stack worker (the decoder overflows the small httpd stack),
    // guard on that worker. loop>1 benchmarks steady-state throughput (warmup
    // excluded). stackkb/loop overridable.
    int guard = qparam(q, "guard", 1);
    int loop = qparam(q, "loop", 1); if (loop < 1) loop = 1; if (loop > 1000) loop = 1000;
    size_t stackSz = (size_t)qparam(q, "stackkb", 8192) * 1024;
    n += snprintf(out + n, outcap - n, "(decode on dedicated thread, stack=%uKB, guard=%d, loop=%d)\n",
                  (unsigned)(stackSz / 1024), guard, loop);
    DecodeArgs st; memset(&st, 0, sizeof(st));
    st.decoder = decoder; st.in = &in; st.fbi = &fbi; st.od = &od; st.guard = guard; st.loop = loop;
    int trc = run_on_big_stack(&st, stackSz);
    rc = st.rc;
    if (trc != 0) { n += snprintf(out + n, outcap - n, "STOP: decode thread create failed.\n"); persist_log(out, n); goto done; }
    if (st.faulted) {
        n += snprintf(out + n, outcap - n,
                      "Decode FAULTED (caught in-process): %s addr=0x%lx — app stays alive.\n",
                      signame(g_vdecSig), (unsigned long)g_vdecAddr);
        persist_log(out, n);
        goto done;
    }

    n += snprintf(out + n, outcap - n,
                  "Decode: rc=0x%08x(%s) accepted=%d | valid=%d err=%d pics=%d %ux%u pitch=%u fmt=0x%x fb=%p\n",
                  (unsigned)rc, vdec2_errname(rc), fbi.isAccepted, od.isValid, od.isErrorFrame, od.pictureCount,
                  od.frameWidth, od.frameHeight, od.framePitch, od.frameFormat, od.frameBuffer);
    if (rc == 0 && od.isValid && od.frameWidth > 0) {
        unsigned long warm = (unsigned long)st.warmup_us;
        n += snprintf(out + n, outcap - n,
                      "SUCCESS: hardware decoded %s %ux%u. warmup=%lu us.\n",
                      sampleName, od.frameWidth, od.frameHeight, warm);
        if (st.iters > 1) {
            int timed = st.iters - 1;
            unsigned long avg = (unsigned long)(st.sum_us / (timed ? timed : 1));
            n += snprintf(out + n, outcap - n,
                          "BENCHMARK over %d steady frames: avg=%lu us min=%lu max=%lu  => ~%lu fps (CPU-free, HW silicon)\n",
                          timed, avg, (unsigned long)st.min_us, (unsigned long)st.max_us,
                          avg ? (1000000UL / avg) : 0);
        }
        // Verify there's a real picture in the NV12 surface.
        const uint8_t *fbuf = od.frameBuffer ? (const uint8_t *)od.frameBuffer : (const uint8_t *)fb.va;
        int pitch = od.framePitch ? (int)od.framePitch : (int)od.frameWidth;
        n += dump_nv12(out + n, outcap - n, fbuf, (int)od.frameWidth, (int)od.frameHeight, pitch);
    } else {
        n += snprintf(out + n, outcap - n, "Decode rc/valid issue at iter %d (see codes).\n", st.iters);
    }
    persist_log(out, n);

done:
    if (heapAu) free(heapAu);
    if (qparam(q, "nocleanup", 0)) {
        n += snprintf(out + n, outcap - n,
                      "nocleanup=1: intentionally leaving decoder/resources allocated for crash isolation.\n");
    } else {
        if (decoder) pDelete(decoder);
        if (computeQueue) pReleaseCompute(computeQueue);
        dmem_free(&au); dmem_free(&fb); dmem_free(&cgpu); dmem_free(&gpu); dmem_free(&cpu); dmem_free(&computeMem);
    }
    if (elev && !qparam(q, "keepjb", 0)) goldhen_restore(NULL, 0);
    VDEC_RETURN();
#undef VDEC_RETURN
}
