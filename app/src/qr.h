#ifndef PS4CAST_QR_H
#define PS4CAST_QR_H

#include <stdint.h>

#define QR_SIZE 29   // fits QR version 3 (tokened URLs); v2 codes are centered with a quiet border

typedef struct {
    uint8_t m[QR_SIZE][QR_SIZE];
} QrCode;

int qr_make_url(const char *text, QrCode *qr);

#endif
