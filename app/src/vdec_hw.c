#include "vdec_hw.h"
#include "goldhen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/GnmDriver.h>   // sceGnmSubmitDone — fence GPU compute before freeing its memory

#define SYS_dynlib_load_prx  594

// ---- sceVideodec2 ABI (same layouts proven by vdec_probe.c) ----------------
typedef struct {
    uint64_t thisSize; uint32_t resourceType, codecType, profile, maxLevel;
    int32_t maxFrameWidth, maxFrameHeight, maxDpbFrameCount; uint32_t decodePipelineDepth;
    void *computeQueue; uint64_t cpuAffinityMask; int32_t cpuThreadPriority;
    uint8_t optimizeProgressiveVideo, checkMemoryType, reserved0, reserved1; void *extraConfigInfo;
} Vdec2Config;
typedef struct {
    uint64_t thisSize, cpuMemorySize; void *cpuMemory; uint64_t gpuMemorySize; void *gpuMemory;
    uint64_t cpuGpuMemorySize; void *cpuGpuMemory; uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment, reserved0;
} Vdec2MemInfo;
typedef struct { uint64_t thisSize, cpuGpuMemorySize; void *cpuGpuMemory; } Vdec2ComputeMemInfo;
typedef struct {
    uint64_t thisSize; uint16_t computePipeId, computeQueueId; uint8_t checkMemoryType, reserved0; uint16_t reserved1;
} Vdec2ComputeConfig;
typedef struct { uint64_t thisSize; void *auData; uint64_t auSize, ptsData, dtsData, attachedData; } Vdec2Input;
typedef struct { uint64_t thisSize; void *frameBuffer; uint64_t frameBufferSize; uint8_t isAccepted, _pad[7]; } Vdec2FrameBuffer;
typedef struct {
    uint64_t thisSize; uint8_t isValid, isErrorFrame, pictureCount, _pad0;
    uint32_t codecType, frameWidth, framePitch, frameHeight, _pad1;
    void *frameBuffer; uint64_t frameBufferSize; uint32_t frameFormat, framePitchInBytes;
} Vdec2Output;

typedef int (*PFNQuery)(const Vdec2Config *, Vdec2MemInfo *);
typedef int (*PFNQueryCompute)(Vdec2ComputeMemInfo *);
typedef int (*PFNAllocCompute)(const Vdec2ComputeConfig *, const Vdec2ComputeMemInfo *, void **);
typedef int (*PFNReleaseCompute)(void *);
typedef int (*PFNCreate)(const Vdec2Config *, const Vdec2MemInfo *, void **);
typedef int (*PFNDelete)(void *);
typedef int (*PFNDecode)(void *, const Vdec2Input *, Vdec2FrameBuffer *, Vdec2Output *);
typedef int (*PFNReset)(void *);

static PFNQuery pQuery; static PFNQueryCompute pQueryCompute; static PFNAllocCompute pAllocCompute;
static PFNReleaseCompute pReleaseCompute; static PFNCreate pCreate; static PFNDelete pDelete; static PFNDecode pDecode;
static PFNReset pReset;
static int g_bound;

#define MEM_ONION   0
#define MEM_GARLIC  3
#define POOL_MAX    10

typedef struct { off_t off; size_t size; void *va; } DMem;

// Outstanding direct (GPU/onion/garlic) memory currently held by the HW decoder.
// Exposed via /status so a leak across vdec_hw_open/close cycles is observable
// (the prime suspect for the resource-accumulation hang after many casts).
static long g_dmemOutstanding;
long vdec_hw_dmem_outstanding(void) { return g_dmemOutstanding; }

static int dmem_alloc(DMem *m, size_t size, int memtype) {
    size_t align = 0x4000;
    if (!size) { m->off = 0; m->size = 0; m->va = NULL; return 0; }
    size = (size + align - 1) / align * align;
    m->off = 0; m->size = size; m->va = NULL;
    off_t off = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, memtype, &off) < 0) return -1;
    void *va = NULL;
    if (sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align) < 0) { sceKernelReleaseDirectMemory(off, size); return -2; }
    m->off = off; m->va = va; g_dmemOutstanding += (long)size; return 0;
}
static void dmem_free(DMem *m) {
    if (m->va) { sceKernelMunmap(m->va, m->size); m->va = NULL; }
    if (m->size) { sceKernelReleaseDirectMemory(m->off, m->size); g_dmemOutstanding -= (long)m->size; m->size = 0; }
}

// ---- session state --------------------------------------------------------
static void   *g_decoder;
static void   *g_computeQueue;
static DMem    g_cpu, g_gpu, g_cgpu, g_compute;
static DMem    g_pool[POOL_MAX];
static int     g_poolN, g_poolIdx;
static uint64_t g_fbSize;
static long    g_frames;
static int     g_lastErr;
static char    g_dbg[160] = "hw off";

// Input-timestamp FIFO: the decoder may emit a delayed/reordered picture, so we
// push each AU's pts/dts on submit and pop the front when a frame comes out —
// that gives the OUTPUT frame its correct timestamp (one-in-one-out at depth=1,
// but the FIFO bridges any decoder latency robustly).
#define PTS_FIFO 64
static int64_t g_fifoPts[PTS_FIFO], g_fifoDts[PTS_FIFO];
static int     g_fifoHead, g_fifoCount;
static void fifo_clear(void) { g_fifoHead = g_fifoCount = 0; }

static int bind_vdec2(void) {
    if (g_bound) return 0;
    sceSysmoduleLoadModule((enum OrbisSysModule)0x00CF);
    sceSysmoduleLoadModule((enum OrbisSysModule)0x00D0);
    const char *path = "/system/common/lib/libSceVideodec2.sprx";
    int h = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (h < 0) { int mid = -1; syscall(SYS_dynlib_load_prx, (long)path, 0L, (long)&mid, 0L, 0L, 0L); h = (mid >= 0) ? mid : h; }
    if (h < 0) return -1;
    sceKernelDlsym(h, "sceVideodec2QueryDecoderMemoryInfo", (void **)&pQuery);
    sceKernelDlsym(h, "sceVideodec2QueryComputeMemoryInfo", (void **)&pQueryCompute);
    sceKernelDlsym(h, "sceVideodec2AllocateComputeQueue",   (void **)&pAllocCompute);
    sceKernelDlsym(h, "sceVideodec2ReleaseComputeQueue",    (void **)&pReleaseCompute);
    sceKernelDlsym(h, "sceVideodec2CreateDecoder",          (void **)&pCreate);
    sceKernelDlsym(h, "sceVideodec2DeleteDecoder",          (void **)&pDelete);
    sceKernelDlsym(h, "sceVideodec2Decode",                 (void **)&pDecode);
    sceKernelDlsym(h, "sceVideodec2Reset",                  (void **)&pReset);   // optional
    if (!pQuery || !pQueryCompute || !pAllocCompute || !pReleaseCompute || !pCreate || !pDecode || !pDelete) return -2;
    g_bound = 1;
    return 0;
}

int vdec_hw_active(void) { return g_decoder != NULL; }
const char *vdec_hw_debug(void) { return g_dbg; }

int vdec_hw_open(int maxW, int maxH, int profile, int level) {
    vdec_hw_close();
    if (maxW <= 0) maxW = 1920;
    if (maxH <= 0) maxH = 1088;
    char gh[160] = "";
    goldhen_enter(gh, sizeof(gh));            // privileged module load

    int rc = bind_vdec2();
    if (rc != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw bind fail %d", rc); goldhen_restore(NULL, 0); return -1; }

    Vdec2Config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.thisSize = sizeof(cfg);
    cfg.resourceType = 1; cfg.codecType = 1;             // H.264 on the proven resource type
    cfg.profile = profile > 0 ? (uint32_t)profile : 100; // default High
    cfg.maxLevel = level > 0 ? (uint32_t)level : 51;
    cfg.maxFrameWidth = maxW; cfg.maxFrameHeight = maxH;
    cfg.maxDpbFrameCount = 8; cfg.decodePipelineDepth = 1;   // 8 covers >4-ref streams (1080p L>4.2, lower-res high-ref); ~+20MB
    cfg.cpuAffinityMask = 0x3f; cfg.cpuThreadPriority = 0x2bc;
    cfg.checkMemoryType = 0;

    // Compute queue.
    Vdec2ComputeMemInfo cmi; memset(&cmi, 0, sizeof(cmi)); cmi.thisSize = sizeof(cmi);
    if (pQueryCompute(&cmi) != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw qcompute fail"); goto fail; }
    if (dmem_alloc(&g_compute, cmi.cpuGpuMemorySize, MEM_ONION) != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw compute mem"); goto fail; }
    cmi.cpuGpuMemory = g_compute.va;
    Vdec2ComputeConfig ccfg; memset(&ccfg, 0, sizeof(ccfg)); ccfg.thisSize = sizeof(ccfg);
    if (pAllocCompute(&ccfg, &cmi, &g_computeQueue) != 0 || !g_computeQueue) { snprintf(g_dbg, sizeof(g_dbg), "hw alloc cq"); goto fail; }
    cfg.computeQueue = g_computeQueue;

    // Decoder memory.
    Vdec2MemInfo mi; memset(&mi, 0, sizeof(mi)); mi.thisSize = sizeof(mi);
    if (pQuery(&cfg, &mi) != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw query fail"); goto fail; }
    if (dmem_alloc(&g_cpu,  mi.cpuMemorySize,    MEM_ONION)  != 0 ||
        dmem_alloc(&g_gpu,  mi.gpuMemorySize,    MEM_GARLIC) != 0 ||   // gpuMemory MUST be garlic
        dmem_alloc(&g_cgpu, mi.cpuGpuMemorySize, MEM_ONION)  != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw dec mem"); goto fail; }
    mi.cpuMemory = g_cpu.va; mi.gpuMemory = g_gpu.va; mi.cpuGpuMemory = g_cgpu.va;

    if (pCreate(&cfg, &mi, &g_decoder) != 0 || !g_decoder) { snprintf(g_dbg, sizeof(g_dbg), "hw create fail"); goto fail; }

    // Output frame-buffer pool (rotated so references stay valid). Onion so the
    // player can read the NV12 back on the CPU for the NV12->BGRA scale.
    g_fbSize = mi.maxFrameBufferSize ? mi.maxFrameBufferSize : 0x400000;
    g_poolN = 8;
    for (int i = 0; i < g_poolN; i++) {
        if (dmem_alloc(&g_pool[i], g_fbSize, MEM_ONION) != 0) { snprintf(g_dbg, sizeof(g_dbg), "hw pool %d", i); goto fail; }
    }
    g_poolIdx = 0; g_frames = 0; g_lastErr = 0;
    goldhen_restore(NULL, 0);                 // decode runs unprivileged
    snprintf(g_dbg, sizeof(g_dbg), "hw H264 %dx%d p%d/l%d fb=%lluKB pool=%d",
             maxW, maxH, (int)cfg.profile, (int)cfg.maxLevel, (unsigned long long)(g_fbSize / 1024), g_poolN);
    return 0;

fail:
    goldhen_restore(NULL, 0);
    vdec_hw_close();
    return -1;
}

int vdec_hw_decode(const uint8_t *au, int len, int64_t pts, int64_t dts, VdecHwFrame *out) {
    if (!g_decoder || !au || len <= 0) return -1;
    void *fb = g_pool[g_poolIdx].va;
    g_poolIdx = (g_poolIdx + 1) % g_poolN;

    // Push this AU's timestamp (drop oldest if the FIFO somehow overflows).
    if (g_fifoCount >= PTS_FIFO) { g_fifoHead = (g_fifoHead + 1) % PTS_FIFO; g_fifoCount--; }
    int tail = (g_fifoHead + g_fifoCount) % PTS_FIFO;
    g_fifoPts[tail] = pts; g_fifoDts[tail] = dts; g_fifoCount++;

    Vdec2Input in; memset(&in, 0, sizeof(in));
    in.thisSize = sizeof(in); in.auData = (void *)au; in.auSize = (uint64_t)len;
    in.ptsData = (uint64_t)g_frames; in.dtsData = (uint64_t)g_frames;
    Vdec2FrameBuffer fbi; memset(&fbi, 0, sizeof(fbi));
    fbi.thisSize = sizeof(fbi); fbi.frameBuffer = fb; fbi.frameBufferSize = g_fbSize;
    Vdec2Output od; memset(&od, 0, sizeof(od)); od.thisSize = sizeof(od);

    int rc = pDecode(g_decoder, &in, &fbi, &od);
    if (rc != 0) {
        if (g_fifoCount > 0) g_fifoCount--;              // decode failed -> undo the push
        g_lastErr = rc; return -1;
    }
    g_frames++;
    if (!od.isValid || od.frameWidth == 0) return 0;     // accepted, picture buffered -> keep pts

    // Pop the oldest pending timestamp for the picture now being emitted.
    out->pts = out->dts = 0;
    if (g_fifoCount > 0) {
        out->pts = g_fifoPts[g_fifoHead]; out->dts = g_fifoDts[g_fifoHead];
        g_fifoHead = (g_fifoHead + 1) % PTS_FIFO; g_fifoCount--;
    }
    void *base = od.frameBuffer ? od.frameBuffer : fb;
    out->y = (const uint8_t *)base;
    out->uv = (const uint8_t *)base + (size_t)od.framePitch * od.frameHeight;
    out->width = (int)od.frameWidth;
    out->height = (int)od.frameHeight;
    out->pitch = (int)od.framePitch;
    return 1;
}

// Reset decoder reference state on seek (fresh sequence). Falls back to a
// close+reopen is not possible here without config, so if Reset is unavailable
// we just drop the pool rotation; the next keyframe re-establishes refs anyway.
void vdec_hw_reset(void) {
    if (g_decoder && pReset) pReset(g_decoder);
    g_poolIdx = 0;
    fifo_clear();
}

void vdec_hw_close(void) {
    // Fence outstanding GPU compute work BEFORE deleting the decoder / releasing
    // the compute queue / freeing garlic+onion memory. Freeing direct memory the
    // GPU is still DMA-ing into causes an unrecoverable GPU page fault (hung GPU
    // -> unkillable). It also leaves the compute queue quiesced so the system can
    // suspend/close the app without CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND.
    sceGnmSubmitDone();
    if (g_decoder && pDelete) pDelete(g_decoder);
    g_decoder = NULL;
    if (g_computeQueue && pReleaseCompute) pReleaseCompute(g_computeQueue);
    g_computeQueue = NULL;
    for (int i = 0; i < POOL_MAX; i++) dmem_free(&g_pool[i]);
    g_poolN = g_poolIdx = 0;
    dmem_free(&g_cgpu); dmem_free(&g_gpu); dmem_free(&g_cpu); dmem_free(&g_compute);
    g_frames = 0;
    fifo_clear();
    snprintf(g_dbg, sizeof(g_dbg), "hw off");
}
