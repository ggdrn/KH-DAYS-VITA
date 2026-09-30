/* Plain FIPS 180-1 SHA-1, used once to check the user's ROM dump. */
#include "sha1.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      f = (b & c) | (~b & d),          k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d,                   k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else             f = b ^ c ^ d,                   k = 0xca62c1d6;
        t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

void sha1_init(Sha1 *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->used = 0;
}

void sha1_update(Sha1 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    if (s->used) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += take, p += take, n -= take;
        if (s->used < 64)
            return;
        block(s, s->buf);
        s->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64)
        block(s, p);
    memcpy(s->buf, p, n);
    s->used = n;
}

void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbe[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, lenbe, 8);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}
