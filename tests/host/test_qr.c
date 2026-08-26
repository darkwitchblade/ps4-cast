// Host test for the lobby QR generator: dump matrices for golden comparison.
// The goldens (test_qr_golden.txt) were verified decodable with an independent
// scanner (zxing-cpp) when generated; this test pins them against regressions.
#include "../../app/src/qr.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *cases[] = {
        "http://192.168.1.4:8080",                    // short: version 2 + quiet border
        "http://192.168.1.4:8080/?t=ABCD2345",        // tokened: version 3
        "http://192.168.1.130:8080",
    };
    int failures = 0;
    for (int c = 0; c < 3; c++) {
        QrCode q;
        int rc = qr_make_url(cases[c], &q);
        if (rc != 0) { printf("case %d FAILED rc=%d\n", c, rc); failures++; continue; }
        printf("case %d rc=0\n", c);
        for (int y = 0; y < QR_SIZE; y++) {
            for (int x = 0; x < QR_SIZE; x++) putchar(q.m[y][x] ? '1' : '0');
            putchar('\n');
        }
    }
    printf(failures ? "test_qr: %d FAILURES\n" : "test_qr: generated ok\n", failures);
    return failures ? 1 : 0;
}
