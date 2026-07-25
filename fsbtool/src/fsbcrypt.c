#include "fsbcrypt.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int fsb_has_magic(const uint8_t *buf, size_t size) {
    if (size < 4) return 0;
    return buf[0] == 'F' && buf[1] == 'S' && buf[2] == 'B' &&
           (buf[3] >= '1' && buf[3] <= '5');
}

/* Parse key text into raw bytes. "0x..." or an even-length all-hex string is
 * interpreted as hex; otherwise the ASCII bytes are used verbatim. */
static int parse_key(const char *key, uint8_t *out, int max) {
    size_t n = strlen(key);
    const char *h = key;
    if (n >= 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) { h += 2; n -= 2; }
    int is_hex = (n > 0 && (n % 2) == 0);
    for (size_t i = 0; i < n && is_hex; ++i) if (!isxdigit((unsigned char)h[i])) is_hex = 0;

    if (is_hex && (h != key || (key[0] == '0' && (key[1] | 32) == 'x'))) {
        int klen = 0;
        for (size_t i = 0; i + 1 < n + 1 && klen < max; i += 2) {
            char t[3] = { h[i], h[i + 1], 0 };
            out[klen++] = (uint8_t)strtol(t, NULL, 16);
        }
        return klen;
    }
    int klen = (int)strlen(key); if (klen > max) klen = max;
    memcpy(out, key, klen);
    return klen;
}

/* Apply scheme `s` to one byte given key byte kb. */
static uint8_t apply(int s, uint8_t b, uint8_t kb) {
    switch (s) {
        case 1: return b ^ kb;                                   /* plain XOR */
        case 2: { uint8_t x = b ^ kb; return (uint8_t)((x << 4) | (x >> 4)); } /* XOR then nibble-swap */
        case 3: { uint8_t x = (uint8_t)((b << 4) | (b >> 4)); return x ^ kb; } /* nibble-swap then XOR */
        default: return b;
    }
}

int fsb_try_decrypt(uint8_t *buf, size_t size, const char *key) {
    uint8_t k[256];
    int klen = parse_key(key, k, sizeof k);
    if (klen <= 0) return 0;

    const int NSCHEMES = 3;
    for (int s = 1; s <= NSCHEMES; ++s) {
        /* test only the first bytes to avoid corrupting on a wrong guess */
        uint8_t probe[8];
        size_t pn = size < sizeof probe ? size : sizeof probe;
        for (size_t i = 0; i < pn; ++i) probe[i] = apply(s, buf[i], k[i % klen]);
        if (fsb_has_magic(probe, pn)) {
            for (size_t i = 0; i < size; ++i) buf[i] = apply(s, buf[i], k[i % klen]);
            return s;
        }
    }
    return 0;
}
