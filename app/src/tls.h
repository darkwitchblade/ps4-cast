// tls.h — minimal TLS 1.2 client over an existing OrbisNet TCP socket (BearSSL).
//
// Certificates are NOT validated (no trust store bundled): this is a LAN media
// caster where the user chooses the source, so we accept any server cert to keep
// the build self-contained. Confidentiality is preserved; authenticity is not.
#ifndef PS4CAST_TLS_H
#define PS4CAST_TLS_H

#include <stdint.h>
#include <stddef.h>

typedef struct tls_ctx tls_ctx;

// Wrap an already-connected TCP socket in TLS and run the handshake against
// `host` (for SNI). Returns a context, or NULL on failure.
tls_ctx *tls_open(int sock, const char *host);

// Same, but with a read deadline armed before the handshake. Required when the
// socket is non-blocking (see tls.c).
tls_ctx *tls_open_bounded(int sock, const char *host, uint64_t deadlineUs);

// Read up to len bytes of plaintext. >0 = bytes, 0 = clean EOF, <0 = error.
int  tls_read(tls_ctx *t, uint8_t *buf, int len);

// Write len bytes of plaintext (all of it). 0 ok, <0 error.
int  tls_write(tls_ctx *t, const uint8_t *buf, int len);

// Bound how long reads may block, as an absolute sceKernelGetProcessTime value
// (0 = unbounded). Needed because BearSSL can loop internally on a trickling
// peer and never return to the caller's own budget check.
void tls_set_read_deadline(tls_ctx *t, uint64_t absUs);

// Last BearSSL engine error code (for diagnostics).
int  tls_last_error(tls_ctx *t);

void tls_close(tls_ctx *t);

#endif
