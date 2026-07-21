#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

static uint32_t rotr32(uint32_t x, unsigned n);
static void sha256_block(struct sha256_ctx *c, const unsigned char *p);
static void sha256_update_cstr(struct sha256_ctx *ctx, const char *s);

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256_ctx *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7], cc = c->h[2];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void hold_sha256_init(struct sha256_ctx *c) {
    c->h[0] = 0x6a09e667U; c->h[1] = 0xbb67ae85U; c->h[2] = 0x3c6ef372U; c->h[3] = 0xa54ff53aU;
    c->h[4] = 0x510e527fU; c->h[5] = 0x9b05688cU; c->h[6] = 0x1f83d9abU; c->h[7] = 0x5be0cd19U;
    c->len = 0;
    c->off = 0;
}

void hold_sha256_update(struct sha256_ctx *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += (uint64_t)n * 8U;
    while (n > 0) {
        size_t take = 64 - c->off;
        if (take > n) take = n;
        memcpy(c->buf + c->off, p, take);
        c->off += take;
        p += take;
        n -= take;
        if (c->off == 64) {
            sha256_block(c, c->buf);
            c->off = 0;
        }
    }
}

void hold_sha256_final(struct sha256_ctx *c, unsigned char out[32]) {
    c->buf[c->off++] = 0x80;
    if (c->off > 56) {
        while (c->off < 64) c->buf[c->off++] = 0;
        sha256_block(c, c->buf);
        c->off = 0;
    }
    while (c->off < 56) c->buf[c->off++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->off++] = (unsigned char)(c->len >> (i * 8));
    sha256_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

void hold_hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n) {
    static const char hex[] = "0123456789abcdef";
    if (out_n < n * 2 + 1) {
        if (out_n > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static void sha256_update_cstr(struct sha256_ctx *ctx, const char *s) {
    hold_sha256_update(ctx, s, strlen(s));
}

void hold_sha256_update_nul_field(struct sha256_ctx *ctx, const char *s) {
    static const unsigned char nul = 0;
    sha256_update_cstr(ctx, s ? s : "");
    hold_sha256_update(ctx, &nul, 1);
}
