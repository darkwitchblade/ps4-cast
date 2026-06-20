#include "qr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define QR_DATA_CODEWORDS 34
#define QR_ECC_CODEWORDS 10
#define QR_TOTAL_CODEWORDS (QR_DATA_CODEWORDS + QR_ECC_CODEWORDS)

static int8_t g_mod[QR_SIZE][QR_SIZE];
static uint8_t g_func[QR_SIZE][QR_SIZE];
static uint8_t g_exp[255], g_log[256];
static int g_gf_ready = 0;

static void set_mod(int x, int y, int v, int func) {
    if (x < 0 || y < 0 || x >= QR_SIZE || y >= QR_SIZE) return;
    g_mod[y][x] = (int8_t)(v ? 1 : 0);
    if (func) g_func[y][x] = 1;
}

static int get_bit(const uint8_t *data, int bit) {
    return (data[bit >> 3] >> (7 - (bit & 7))) & 1;
}

static void bit_append(uint8_t *out, int *bitLen, unsigned val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        if ((val >> i) & 1)
            out[*bitLen >> 3] |= (uint8_t)(1u << (7 - (*bitLen & 7)));
        (*bitLen)++;
    }
}

static void gf_init(void) {
    if (g_gf_ready) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        g_exp[i] = (uint8_t)x;
        g_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    g_gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    return g_exp[(g_log[a] + g_log[b]) % 255];
}

static void rs_gen(uint8_t *gen) {
    gf_init();
    memset(gen, 0, QR_ECC_CODEWORDS + 1);
    gen[0] = 1;
    int degree = 0;
    for (int i = 0; i < QR_ECC_CODEWORDS; i++) {
        uint8_t root = g_exp[i];
        gen[degree + 1] = 0;
        for (int j = degree; j >= 0; j--) {
            gen[j + 1] ^= gf_mul(gen[j], root);
        }
        degree++;
    }
}

static void rs_encode(const uint8_t *data, uint8_t *ecc) {
    uint8_t gen[QR_ECC_CODEWORDS + 1];
    rs_gen(gen);
    memset(ecc, 0, QR_ECC_CODEWORDS);
    for (int i = 0; i < QR_DATA_CODEWORDS; i++) {
        uint8_t factor = data[i] ^ ecc[0];
        memmove(ecc, ecc + 1, QR_ECC_CODEWORDS - 1);
        ecc[QR_ECC_CODEWORDS - 1] = 0;
        for (int j = 0; j < QR_ECC_CODEWORDS; j++)
            ecc[j] ^= gf_mul(gen[j + 1], factor);
    }
}

static void draw_finder(int x, int y) {
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            int xx = x + dx, yy = y + dy;
            int v = 0;
            if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6)
                v = (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                     (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
            set_mod(xx, yy, v, 1);
        }
    }
}

static void draw_alignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            set_mod(cx + dx, cy + dy, (adx == 2 || ady == 2 || (dx == 0 && dy == 0)), 1);
        }
    }
}

static uint16_t format_bits(int mask) {
    uint32_t data = (1u << 3) | (uint32_t)mask;   // ECC level L = 01
    uint32_t rem = data << 10;
    for (int i = 14; i >= 10; i--) {
        if ((rem >> i) & 1)
            rem ^= 0x537u << (i - 10);
    }
    return (uint16_t)(((data << 10) | rem) ^ 0x5412u);
}

static void draw_format(int mask) {
    uint16_t bits = format_bits(mask);
    for (int i = 0; i <= 5; i++) set_mod(8, i, (bits >> i) & 1, 1);
    set_mod(8, 7, (bits >> 6) & 1, 1);
    set_mod(8, 8, (bits >> 7) & 1, 1);
    set_mod(7, 8, (bits >> 8) & 1, 1);
    for (int i = 9; i < 15; i++) set_mod(14 - i, 8, (bits >> i) & 1, 1);

    for (int i = 0; i < 8; i++) set_mod(QR_SIZE - 1 - i, 8, (bits >> i) & 1, 1);
    for (int i = 8; i < 15; i++) set_mod(8, QR_SIZE - 15 + i, (bits >> i) & 1, 1);
    set_mod(8, QR_SIZE - 8, 1, 1);
}

static void draw_function_patterns(void) {
    draw_finder(0, 0);
    draw_finder(QR_SIZE - 7, 0);
    draw_finder(0, QR_SIZE - 7);
    for (int i = 8; i < QR_SIZE - 8; i++) {
        set_mod(i, 6, (i & 1) == 0, 1);
        set_mod(6, i, (i & 1) == 0, 1);
    }
    draw_alignment(18, 18);
    set_mod(8, 17, 1, 1);
    draw_format(0);
}

static int penalty(void) {
    int p = 0;
    for (int y = 0; y < QR_SIZE; y++) {
        int runColor = g_mod[y][0], run = 1;
        for (int x = 1; x < QR_SIZE; x++) {
            if (g_mod[y][x] == runColor) run++;
            else { if (run >= 5) p += 3 + (run - 5); runColor = g_mod[y][x]; run = 1; }
        }
        if (run >= 5) p += 3 + (run - 5);
    }
    for (int x = 0; x < QR_SIZE; x++) {
        int runColor = g_mod[0][x], run = 1;
        for (int y = 1; y < QR_SIZE; y++) {
            if (g_mod[y][x] == runColor) run++;
            else { if (run >= 5) p += 3 + (run - 5); runColor = g_mod[y][x]; run = 1; }
        }
        if (run >= 5) p += 3 + (run - 5);
    }
    for (int y = 0; y < QR_SIZE - 1; y++)
        for (int x = 0; x < QR_SIZE - 1; x++)
            if (g_mod[y][x] == g_mod[y][x + 1] && g_mod[y][x] == g_mod[y + 1][x] && g_mod[y][x] == g_mod[y + 1][x + 1])
                p += 3;
    int dark = 0;
    for (int y = 0; y < QR_SIZE; y++)
        for (int x = 0; x < QR_SIZE; x++)
            dark += g_mod[y][x] != 0;
    int k = ((dark * 20 + QR_SIZE * QR_SIZE / 2) / (QR_SIZE * QR_SIZE) - 10);
    if (k < 0) k = -k;
    return p + k * 10;
}

static int mask_bit(int mask, int x, int y) {
    switch (mask) {
    case 0: return ((x + y) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: return ((x * y) % 2 + (x * y) % 3) == 0;
    case 6: return (((x * y) % 2 + (x * y) % 3) & 1) == 0;
    default: return (((x + y) % 2 + (x * y) % 3) & 1) == 0;
    }
}

static void place_codewords(const uint8_t *cw) {
    int bit = 0, upward = 1;
    for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
        if (right == 6) right--;
        for (int vert = 0; vert < QR_SIZE; vert++) {
            int y = upward ? (QR_SIZE - 1 - vert) : vert;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (g_func[y][x]) continue;
                int v = (bit < QR_TOTAL_CODEWORDS * 8) ? get_bit(cw, bit) : 0;
                set_mod(x, y, v, 0);
                bit++;
            }
        }
        upward = !upward;
    }
}

static void apply_mask(int mask) {
    for (int y = 0; y < QR_SIZE; y++)
        for (int x = 0; x < QR_SIZE; x++)
            if (!g_func[y][x] && mask_bit(mask, x, y))
                g_mod[y][x] ^= 1;
    draw_format(mask);
}

int qr_make_url(const char *text, QrCode *qr) {
    size_t len = strlen(text);
    if (len > 32 || !qr) return -1;

    uint8_t data[QR_DATA_CODEWORDS] = {0};
    int bits = 0;
    bit_append(data, &bits, 0x4, 4);
    bit_append(data, &bits, (unsigned)len, 8);
    for (size_t i = 0; i < len; i++) bit_append(data, &bits, (uint8_t)text[i], 8);
    int capBits = QR_DATA_CODEWORDS * 8;
    int term = capBits - bits;
    if (term > 4) term = 4;
    bit_append(data, &bits, 0, term);
    while (bits & 7) bit_append(data, &bits, 0, 1);
    for (int i = bits / 8, pad = 0; i < QR_DATA_CODEWORDS; i++, pad ^= 1)
        data[i] = pad ? 0x11 : 0xEC;

    uint8_t cw[QR_TOTAL_CODEWORDS] = {0};
    memcpy(cw, data, QR_DATA_CODEWORDS);
    rs_encode(data, cw + QR_DATA_CODEWORDS);

    int bestMask = 0, bestPenalty = 0x7fffffff;
    int8_t best[QR_SIZE][QR_SIZE];
    for (int mask = 0; mask < 8; mask++) {
        memset(g_mod, -1, sizeof(g_mod));
        memset(g_func, 0, sizeof(g_func));
        draw_function_patterns();
        place_codewords(cw);
        apply_mask(mask);
        int p = penalty();
        if (p < bestPenalty) {
            bestPenalty = p;
            bestMask = mask;
            memcpy(best, g_mod, sizeof(best));
        }
    }
    (void)bestMask;
    for (int y = 0; y < QR_SIZE; y++)
        for (int x = 0; x < QR_SIZE; x++)
            qr->m[y][x] = best[y][x] ? 1 : 0;
    return 0;
}
