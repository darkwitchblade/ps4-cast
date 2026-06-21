#include "notify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

// Debug toasts default OFF: a shipped receiver should show a clean TV screen,
// not a stream of "Cast 1/4", "first frame", "ffmpeg HxW" diagnostics. The web
// UI Settings sheet toggles this (POST /debug) and it persists across launches.
static int g_debug = 0;

void notify_set_debug(int on) { g_debug = on ? 1 : 0; }
int  notify_get_debug(void)   { return g_debug; }

static void notify_send(const char *msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;     // standard blue toast
    req.useIconImageUri = 0;
    req.targetId = -1;
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void notify(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    notify_send(msg);
}

void notify_dbg(const char *fmt, ...) {
    if (!g_debug) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    notify_send(msg);
}
