#include "notify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

void notify(const char *fmt, ...) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;     // standard blue toast
    req.useIconImageUri = 0;
    req.targetId = -1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
