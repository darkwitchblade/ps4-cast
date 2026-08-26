// qr.c — lobby QR generation. The actual encoding is done by the vendored,
// spec-verified Nayuki generator (qrcodegen.c, MIT); the previous hand-rolled
// encoder produced codes no scanner could read (invalid format info + data
// placement) and capped at 32 chars, which turned the tokened lobby URL into
// a blank white card.
#include "qr.h"
#include "qrcodegen.h"

#include <string.h>

int qr_make_url(const char *text, QrCode *qr) {
    if (!text || !qr) return -1;
    // Version 2 (25x25, 32 bytes) for short URLs, version 3 (29x29, 53 bytes)
    // so the pairing token always fits. boostEcl upgrades short URLs to
    // ECC Medium for better scan reliability without growing the symbol.
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(3)];
    uint8_t buf[qrcodegen_BUFFER_LEN_FOR_VERSION(3)];
    if (!qrcodegen_encodeText(text, temp, buf, qrcodegen_Ecc_LOW,
                              2, 3, qrcodegen_Mask_AUTO, true))
        return -1;

    int size = qrcodegen_getSize(buf);
    memset(qr->m, 0, sizeof(qr->m));
    int off = (QR_SIZE - size) / 2;          // 2-module quiet border for v2
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            qr->m[off + y][off + x] = qrcodegen_getModule(buf, x, y) ? 1 : 0;
    return 0;
}
