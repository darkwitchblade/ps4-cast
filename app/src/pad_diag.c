#include "pad_diag.h"

#include <stdio.h>

static char g_pad_diag[240] = "pad idle";

void pad_diag_set(const char *text) {
    if (!text) text = "";
    snprintf(g_pad_diag, sizeof(g_pad_diag), "%s", text);
}

const char *pad_diag_get(void) {
    return g_pad_diag;
}
