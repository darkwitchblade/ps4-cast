#ifndef PS4CAST_QR_H
#define PS4CAST_QR_H

#include <stdint.h>

#define QR_SIZE 25

typedef struct {
    uint8_t m[QR_SIZE][QR_SIZE];
} QrCode;

int qr_make_url(const char *text, QrCode *qr);

#endif
