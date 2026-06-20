// compat_ff.c — small libc shims for symbols ffmpeg references that the
// OpenOrbis libc does not provide. Only compiled with USE_FFMPEG.
#include <stddef.h>

// Let the libc heap grow without a fixed cap — ffmpeg's decoder/demuxer and our
// scaled framebuffers need far more than the small default heap. These weak
// symbols are read by the OpenOrbis libc at startup.
unsigned int sceLibcHeapExtendedAlloc = 1;  // enable on-demand growth
size_t       sceLibcHeapSize          = 0;   // 0 + extended = no fixed limit

// BSD sysctl: ffmpeg's cpu-count probe. Report "not available" so it falls back.
int sysctl(int *name, unsigned int namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen) {
    (void)name; (void)namelen; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;
}
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                 const void *newp, size_t newlen) {
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;
}
