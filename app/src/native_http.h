#ifndef PS4CAST_NATIVE_HTTP_H
#define PS4CAST_NATIVE_HTTP_H

#include <stdint.h>

// Lazy system HTTP fallback for HTTPS origins that reject the custom BearSSL
// client. The returned body is malloc-owned by the caller.
int native_http_fetch(const char *url, uint8_t **body, int *len,
                      int *status, uint64_t timeout_us);
void native_http_abort(void);
const char *native_http_debug(void);

#endif
