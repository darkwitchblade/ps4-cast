#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#define TRACE_PATH "/data/ps4cast_trace.txt"
#define TRACE_MAX  (64 * 1024)

const char *trace_path(void) { return TRACE_PATH; }

void trace_mark(const char *fmt, ...) {
    char b[512];
    uint64_t t = sceKernelGetProcessTime();
    int n = snprintf(b, sizeof(b), "t=%llu ", (unsigned long long)t);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - n, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    int fd = sceKernelOpen(TRACE_PATH, 0x0201 /*WRONLY|CREAT*/ | 0x0008 /*APPEND*/, 0666);
    if (fd < 0) return;
    off_t end = sceKernelLseek(fd, 0, 2 /*SEEK_END*/);
    if (end > TRACE_MAX) {
        sceKernelClose(fd);
        fd = sceKernelOpen(TRACE_PATH, 0x0201 /*WRONLY|CREAT*/ | 0x0400 /*TRUNC*/, 0666);
        if (fd < 0) return;
    }
    sceKernelWrite(fd, b, (size_t)n);
    sceKernelClose(fd);
}
