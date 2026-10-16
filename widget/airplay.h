/*
 * airplay.h — AirPlay receiver (mirroring, audio, URL) as a single-header C library
 *
 *   #define AIRPLAY_IMPLEMENTATION      // in exactly one translation unit
 *   #include "airplay.h"
 *
 * Portable (Linux / macOS / Windows), no external dependencies: sockets,
 * threads, mDNS responder, bplist, RTSP/HTTP, curve25519/ed25519, SHA-512 and
 * AES are all in this file.  The only third-party piece is FairPlay key
 * handling, supplied either by
 *   - #define AIRPLAY_PLAYFAIR  before the implementation (pulls in
 *     airplay_playfair.h — GPL-3.0, see that file), or
 *   - airplay_fairplay_ops in airplay_config (bring your own).
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 */
#ifndef AIRPLAY_H
#define AIRPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- crypto primitives (exposed for tests and for airdrop.h-style reuse) ---- */
void ap_sha512(uint8_t out[64], const uint8_t* in, size_t n);
void ap_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void ap_x25519_base(uint8_t out[32], const uint8_t scalar[32]);
void ap_ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]);
void ap_ed25519_sign(uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t seed[32]);
int  ap_ed25519_verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pk[32]);

typedef struct { uint32_t rk[44]; } ap_aes128;
void ap_aes128_init(ap_aes128* c, const uint8_t key[16]);
void ap_aes128_encrypt_block(const ap_aes128* c, const uint8_t in[16], uint8_t out[16]);
void ap_aes128_decrypt_block(const ap_aes128* c, const uint8_t in[16], uint8_t out[16]);
void ap_aes128_cbc_decrypt(const ap_aes128* c, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n);

/* ALAC (Apple Lossless) decoder for the 16-bit AirPlay streams.  `cookie` is the 36-byte magic
 * cookie ("alac" atom) or its 24-byte ALACSpecificConfig; decode() returns frames per channel
 * written to `out` (interleaved int16), or <0 on a damaged frame. */
typedef struct ap_alac {
    uint32_t frame_len; int bits, channels, pb, mb, kb;
    int32_t* buf[2]; int32_t* err[2]; int32_t* extra[2];
} ap_alac;
int  ap_alac_init(ap_alac* d, const uint8_t* cookie, size_t n);
int  ap_alac_decode(ap_alac* d, const uint8_t* in, size_t n, int16_t* out, size_t max_frames);
void ap_alac_free(ap_alac* d);

/* Streaming AES-128-CTR (keystream position carries across calls). */
typedef struct { ap_aes128 aes; uint8_t ctr[16]; uint8_t ks[16]; int used; } ap_aes_ctr;
void ap_aes_ctr_init(ap_aes_ctr* c, const uint8_t key[16], const uint8_t iv[16]);
void ap_aes_ctr_xor(ap_aes_ctr* c, const uint8_t* in, uint8_t* out, size_t n);


/* ---- the receiver ------------------------------------------------------ */
typedef struct airplay airplay_t;

enum { AIRPLAY_AUDIO_PCM = 0, AIRPLAY_AUDIO_ALAC = 2, AIRPLAY_AUDIO_AAC_LC = 4, AIRPLAY_AUDIO_AAC_ELD = 8 };

/* FairPlay key handling.  With AIRPLAY_PLAYFAIR defined these are filled in
 * from airplay_playfair.h (GPL-3.0); otherwise supply your own.  All of them
 * are called from the connection thread. */
struct airplay_fairplay_ops;
/* FairPlay in a SEPARATE PROCESS (airplay_fairplayd.c, GPL-3.0) — the arm's-length option: nothing of it is
 * linked or loaded into your program.  Starts `path` (NULL = search $LUNA_AIRPLAY_FAIRPLAYD, next to the
 * program, ./, ~/.local/lib, /usr/local/lib/luna, /usr/lib/luna, /usr/libexec/luna, $PATH) and talks to it
 * over a pipe.  One helper per process; calls are serialised.  Returns 0, or <0 with a message in err. */
int airplay_fairplay_spawn(struct airplay_fairplay_ops* ops, const char* path, char* err, size_t err_n);

typedef struct airplay_fairplay_ops {
    void* user;
    void* (*create)(void* user);
    int   (*setup)(void* ctx, const uint8_t req[16], uint8_t res[142]);
    int   (*handshake)(void* ctx, const uint8_t req[164], uint8_t res[32]);
    int   (*decrypt)(void* ctx, const uint8_t ekey[72], uint8_t aeskey[16]);   /* 0 = ok */
    void  (*destroy)(void* ctx);
} airplay_fairplay_ops;

/* Callbacks are invoked from worker threads; keep them short and thread-safe. */
typedef struct airplay_callbacks {
    void* user;
    void (*on_log)(void* user, int level /*0 info,1 warn*/, const char* msg);
    void (*on_client)(void* user, int connected, const char* name, const char* addr);
    void (*on_video_config)(void* user, int codec /*0 = H.264*/, int width, int height);
    /* Annex-B H.264; SPS/PPS are delivered in-band before each IDR. */
    /* ts_ns is on the sender's clock (see latency_ms for synchronised delivery). */
    void (*on_video)(void* user, const uint8_t* annexb, size_t len, uint64_t ts_ns, int is_idr);
    void (*on_audio_start)(void* user, int codec, int sample_rate);
    /* One decrypted audio frame (ALAC / AAC-ELD ...) — decoding is the sink's job. */
    void (*on_audio)(void* user, const uint8_t* frame, size_t len, int codec, uint32_t rtp_ts);
    void (*on_volume)(void* user, float db);
    /* Decoded audio (ALAC): interleaved int16, `ts_ns` on the sender's clock.  When set, ALAC frames
     * are decoded here and on_audio is not called for them. */
    void (*on_pcm)(void* user, const int16_t* samples, size_t frames, int channels, int rate, uint64_t ts_ns);
    /* Like on_audio plus the sender-clock timestamp in ns (0 until the first sync); wins over on_audio. */
    void (*on_audio_ex)(void* user, const uint8_t* frame, size_t len, int codec, uint32_t rtp_ts, uint64_t ts_ns);
} airplay_callbacks;

typedef struct airplay_config {
    const char* name;                 /* shown on the sender ("Luna AirPlay") */
    int port;                         /* RTSP/HTTP port, 0 = 7000 (falls back to any free port) */
    const char* device_id;            /* "AA:BB:CC:DD:EE:FF", NULL = derived from the identity key */
    const uint8_t* ed_seed;           /* 32-byte identity seed to persist across runs, NULL = random */
    int width, height, fps;           /* advertised display (default 1920x1080@30) */
    int enable_mirroring, enable_audio;   /* both 0 = both on */
    const airplay_fairplay_ops* fairplay;
    /* A/V sync: > 0 holds every frame until sender_time + offset + latency_ms, so audio and video
     * leave in step (callbacks then run on a scheduler thread).  0 = deliver on arrival. */
    int latency_ms;
    int av_offset_ms;             /* > 0 delays video relative to audio (compensates a slower sink) */
} airplay_config;

/* Raw NTP timestamp (seconds<<32 | fraction) from the sender -> nanoseconds. */
uint64_t       airplay_ntp_to_ns(uint64_t ntp);
/* Wrap one raw AAC frame as LATM/LOAS with an in-band config so `ffmpeg/ffplay -f latm` can decode it.
 * asc = AudioSpecificConfig (AAC-LC 44.1k stereo: 12 10, bits 16; AAC-ELD: f8 e8 50 00, bits 28).
 * Returns the bytes written to `out`, or 0 if `cap` is too small. */
size_t         airplay_loas_wrap(const uint8_t* asc, int asc_bits, const uint8_t* frame, size_t n, uint8_t* out, size_t cap);

airplay_t*     airplay_create(const airplay_config* cfg, const airplay_callbacks* cb);
int            airplay_start(airplay_t* a);      /* listens + advertises via mDNS; 0 = ok */
void           airplay_stop(airplay_t* a);
void           airplay_destroy(airplay_t* a);
int            airplay_port(const airplay_t* a);
const char*    airplay_device_id(const airplay_t* a);
const uint8_t* airplay_public_key(const airplay_t* a);   /* 32-byte ed25519 identity */

#ifdef __cplusplus
}
#endif
#endif /* AIRPLAY_H */

/* ======================================================================== */
#ifdef AIRPLAY_IMPLEMENTATION
#ifndef AIRPLAY_IMPLEMENTATION_ONCE
#define AIRPLAY_IMPLEMENTATION_ONCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <signal.h>
#  include <sys/wait.h>
#endif

/* ---------------------------------------------------------------- SHA-512 */
static const uint64_t ap__k512[80] = {
0x428a2f98d728ae22ULL,
0x7137449123ef65cdULL,
0xb5c0fbcfec4d3b2fULL,
0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,
0x59f111f1b605d019ULL,
0x923f82a4af194f9bULL,
0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,
0x12835b0145706fbeULL,
0x243185be4ee4b28cULL,
0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,
0x80deb1fe3b1696b1ULL,
0x9bdc06a725c71235ULL,
0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,
0xefbe4786384f25e3ULL,
0x0fc19dc68b8cd5b5ULL,
0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,
0x4a7484aa6ea6e483ULL,
0x5cb0a9dcbd41fbd4ULL,
0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,
0xa831c66d2db43210ULL,
0xb00327c898fb213fULL,
0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,
0xd5a79147930aa725ULL,
0x06ca6351e003826fULL,
0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,
0x2e1b21385c26c926ULL,
0x4d2c6dfc5ac42aedULL,
0x53380d139d95b3dfULL,
0x650a73548baf63deULL,
0x766a0abb3c77b2a8ULL,
0x81c2c92e47edaee6ULL,
0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,
0xa81a664bbc423001ULL,
0xc24b8b70d0f89791ULL,
0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,
0xd69906245565a910ULL,
0xf40e35855771202aULL,
0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,
0x1e376c085141ab53ULL,
0x2748774cdf8eeb99ULL,
0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,
0x4ed8aa4ae3418acbULL,
0x5b9cca4f7763e373ULL,
0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,
0x78a5636f43172f60ULL,
0x84c87814a1f0ab72ULL,
0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,
0xa4506cebde82bde9ULL,
0xbef9a3f7b2c67915ULL,
0xc67178f2e372532bULL,
0xca273eceea26619cULL,
0xd186b8c721c0c207ULL,
0xeada7dd6cde0eb1eULL,
0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,
0x0a637dc5a2c898a6ULL,
0x113f9804bef90daeULL,
0x1b710b35131c471bULL,
0x28db77f523047d84ULL,
0x32caab7b40c72493ULL,
0x3c9ebe0a15c9bebcULL,
0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,
0x597f299cfc657e2aULL,
0x5fcb6fab3ad6faecULL,
0x6c44198c4a475817ULL
};
#define AP_ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void ap__sha512_block(uint64_t h[8], const uint8_t* p) {
    uint64_t w[80], a, b, c, d, e, f, g, hh, t1, t2;
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = AP_ROR64(w[i - 15], 1) ^ AP_ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = AP_ROR64(w[i - 2], 19) ^ AP_ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 80; i++) {
        t1 = hh + (AP_ROR64(e, 14) ^ AP_ROR64(e, 18) ^ AP_ROR64(e, 41)) + ((e & f) ^ (~e & g)) + ap__k512[i] + w[i];
        t2 = (AP_ROR64(a, 28) ^ AP_ROR64(a, 34) ^ AP_ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void ap_sha512(uint8_t out[64], const uint8_t* in, size_t n) {
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    size_t i = 0;
    for (; i + 128 <= n; i += 128) ap__sha512_block(h, in + i);
    uint8_t buf[256];
    size_t rem = n - i;
    memset(buf, 0, sizeof(buf));
    if (rem) memcpy(buf, in + i, rem);
    buf[rem] = 0x80;
    size_t total = rem + 1 + 16 <= 128 ? 128 : 256;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) buf[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    for (size_t o = 0; o < total; o += 128) ap__sha512_block(h, buf + o);
    for (int j = 0; j < 8; j++)
        for (int k = 0; k < 8; k++) out[j * 8 + k] = (uint8_t)(h[j] >> (56 - 8 * k));
}

/* ------------------------------------------- curve25519 / ed25519 (TweetNaCl design) */
typedef int64_t ap_gf[16];
static const ap_gf ap__gf0 = {0}, ap__gf1 = {1}, ap__121665 = {0xDB41, 1};
static const ap_gf ap__D  = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const ap_gf ap__D2 = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const ap_gf ap__X  = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const ap_gf ap__Y  = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const ap_gf ap__I  = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};
static const uint8_t ap__L[32] = { 0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10 };

static void ap__set25519(ap_gf r, const ap_gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static void ap__car25519(ap_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void ap__sel25519(ap_gf p, ap_gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void ap__pack25519(uint8_t* o, const ap_gf n) {
    int i, j, b;
    ap_gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    ap__car25519(t); ap__car25519(t); ap__car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        ap__sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static int ap__neq25519(const ap_gf a, const ap_gf b) {
    uint8_t c[32], d[32];
    ap__pack25519(c, a); ap__pack25519(d, b);
    return memcmp(c, d, 32) != 0;
}
static uint8_t ap__par25519(const ap_gf a) { uint8_t d[32]; ap__pack25519(d, a); return d[0] & 1; }
static void ap__unpack25519(ap_gf o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void ap__A(ap_gf o, const ap_gf a, const ap_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void ap__Z(ap_gf o, const ap_gf a, const ap_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void ap__M(ap_gf o, const ap_gf a, const ap_gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    ap__car25519(o); ap__car25519(o);
}
static void ap__S(ap_gf o, const ap_gf a) { ap__M(o, a, a); }
static void ap__inv25519(ap_gf o, const ap_gf in) {
    ap_gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { ap__S(c, c); if (a != 2 && a != 4) ap__M(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
static void ap__pow2523(ap_gf o, const ap_gf in) {
    ap_gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 250; a >= 0; a--) { ap__S(c, c); if (a != 1) ap__M(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void ap_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t x[80], r;
    ap_gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    ap__unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        ap__sel25519(a, b, (int)r); ap__sel25519(c, d, (int)r);
        ap__A(e, a, c); ap__Z(a, a, c); ap__A(c, b, d); ap__Z(b, b, d);
        ap__S(d, e); ap__S(f, a); ap__M(a, c, a); ap__M(c, b, e);
        ap__A(e, a, c); ap__Z(a, a, c); ap__S(b, a); ap__Z(c, d, f);
        ap__M(a, c, ap__121665); ap__A(a, a, d); ap__M(c, c, a);
        ap__M(a, d, f); ap__M(d, b, x); ap__S(b, e);
        ap__sel25519(a, b, (int)r); ap__sel25519(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    ap__inv25519(x + 32, x + 32);
    ap__M(x + 16, x + 16, x + 32);
    ap__pack25519(out, x + 16);
}
void ap_x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t nine[32] = { 9 };
    ap_x25519(out, scalar, nine);
}

static void ap__ed_add(ap_gf p[4], ap_gf q[4]) {
    ap_gf a, b, c, d, t, e, f, g, h;
    ap__Z(a, p[1], p[0]); ap__Z(t, q[1], q[0]); ap__M(a, a, t);
    ap__A(b, p[0], p[1]); ap__A(t, q[0], q[1]); ap__M(b, b, t);
    ap__M(c, p[3], q[3]); ap__M(c, c, ap__D2);
    ap__M(d, p[2], q[2]); ap__A(d, d, d);
    ap__Z(e, b, a); ap__Z(f, d, c); ap__A(g, d, c); ap__A(h, b, a);
    ap__M(p[0], e, f); ap__M(p[1], h, g); ap__M(p[2], g, f); ap__M(p[3], e, h);
}
static void ap__ed_cswap(ap_gf p[4], ap_gf q[4], uint8_t b) { for (int i = 0; i < 4; i++) ap__sel25519(p[i], q[i], b); }
static void ap__ed_pack(uint8_t* r, ap_gf p[4]) {
    ap_gf tx, ty, zi;
    ap__inv25519(zi, p[2]);
    ap__M(tx, p[0], zi); ap__M(ty, p[1], zi);
    ap__pack25519(r, ty);
    r[31] ^= (uint8_t)(ap__par25519(tx) << 7);
}
static void ap__ed_scalarmult(ap_gf p[4], ap_gf q[4], const uint8_t* s) {
    ap__set25519(p[0], ap__gf0); ap__set25519(p[1], ap__gf1); ap__set25519(p[2], ap__gf1); ap__set25519(p[3], ap__gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        ap__ed_cswap(p, q, b);
        ap__ed_add(q, p); ap__ed_add(p, p);
        ap__ed_cswap(p, q, b);
    }
}
static void ap__ed_scalarbase(ap_gf p[4], const uint8_t* s) {
    ap_gf q[4];
    ap__set25519(q[0], ap__X); ap__set25519(q[1], ap__Y); ap__set25519(q[2], ap__gf1); ap__M(q[3], ap__X, ap__Y);
    ap__ed_scalarmult(p, q, s);
}
static void ap__modL(uint8_t* r, int64_t x[64]) {
    int64_t carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ap__L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * ap__L[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * ap__L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = (uint8_t)(x[i] & 255); }
}
static void ap__reduce(uint8_t* r) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    ap__modL(r, x);
}
static void ap__ed_expand(uint8_t d[64], const uint8_t seed[32]) {
    ap_sha512(d, seed, 32);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
}
void ap_ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]) {
    uint8_t d[64];
    ap_gf p[4];
    ap__ed_expand(d, seed);
    ap__ed_scalarbase(p, d);
    ap__ed_pack(pk, p);
}
void ap_ed25519_sign(uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t seed[32]) {
    uint8_t d[64], h[64], r[64], pk[32];
    int64_t x[64];
    ap_gf p[4];
    uint8_t* buf = (uint8_t*)malloc(n + 64);
    if (!buf) { memset(sig, 0, 64); return; }
    ap__ed_expand(d, seed);
    ap_ed25519_pubkey(pk, seed);
    memcpy(buf + 32, d + 32, 32);
    memcpy(buf + 64, msg, n);
    ap_sha512(r, buf + 32, n + 32);
    ap__reduce(r);
    ap__ed_scalarbase(p, r);
    ap__ed_pack(buf, p);              /* R */
    memcpy(buf + 32, pk, 32);         /* A */
    ap_sha512(h, buf, n + 64);
    ap__reduce(h);
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++) x[i + j] += (int64_t)h[i] * (uint64_t)d[j];
    memcpy(sig, buf, 32);
    ap__modL(sig + 32, x);
    free(buf);
}
static int ap__ed_unpackneg(ap_gf r[4], const uint8_t p[32]) {
    ap_gf t, chk, num, den, den2, den4, den6;
    ap__set25519(r[2], ap__gf1);
    ap__unpack25519(r[1], p);
    ap__S(num, r[1]); ap__M(den, num, ap__D);
    ap__Z(num, num, r[2]); ap__A(den, r[2], den);
    ap__S(den2, den); ap__S(den4, den2); ap__M(den6, den4, den2);
    ap__M(t, den6, num); ap__M(t, t, den);
    ap__pow2523(t, t);
    ap__M(t, t, num); ap__M(t, t, den); ap__M(t, t, den); ap__M(r[0], t, den);
    ap__S(chk, r[0]); ap__M(chk, chk, den);
    if (ap__neq25519(chk, num)) ap__M(r[0], r[0], ap__I);
    ap__S(chk, r[0]); ap__M(chk, chk, den);
    if (ap__neq25519(chk, num)) return -1;
    if (ap__par25519(r[0]) == (p[31] >> 7)) ap__Z(r[0], ap__gf0, r[0]);
    ap__M(r[3], r[0], r[1]);
    return 0;
}
int ap_ed25519_verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pk[32]) {
    uint8_t t[32], h[64];
    ap_gf p[4], q[4];
    if (ap__ed_unpackneg(q, pk)) return 0;
    uint8_t* buf = (uint8_t*)malloc(n + 64);
    if (!buf) return 0;
    memcpy(buf, sig, 32);
    memcpy(buf + 32, pk, 32);
    memcpy(buf + 64, msg, n);
    ap_sha512(h, buf, n + 64);
    free(buf);
    ap__reduce(h);
    ap__ed_scalarmult(p, q, h);
    ap__ed_scalarbase(q, sig + 32);
    ap__ed_add(p, q);
    ap__ed_pack(t, p);
    return memcmp(sig, t, 32) == 0;
}

/* -------------------------------------------------------------------- AES-128 */
static const uint8_t ap__sbox[256] = { 0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static const uint8_t ap__isbox[256] = { 0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d };
static uint8_t ap__xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t ap__mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = ap__xt(a); b >>= 1; }
    return r;
}
void ap_aes128_init(ap_aes128* c, const uint8_t key[16]) {
    uint8_t rcon = 1;
    uint8_t w[176];
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { w[i - 4], w[i - 3], w[i - 2], w[i - 1] };
        if (i % 16 == 0) {
            uint8_t k = t[0];
            t[0] = (uint8_t)(ap__sbox[t[1]] ^ rcon); t[1] = ap__sbox[t[2]]; t[2] = ap__sbox[t[3]]; t[3] = ap__sbox[k];
            rcon = ap__xt(rcon);
        }
        for (int j = 0; j < 4; j++) w[i + j] = (uint8_t)(w[i - 16 + j] ^ t[j]);
    }
    for (int i = 0; i < 44; i++) c->rk[i] = (uint32_t)w[4 * i] | ((uint32_t)w[4 * i + 1] << 8) | ((uint32_t)w[4 * i + 2] << 16) | ((uint32_t)w[4 * i + 3] << 24);
}
static void ap__add_rk(uint8_t s[16], const uint32_t* rk) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) s[4 * i + j] ^= (uint8_t)(rk[i] >> (8 * j));
}
void ap_aes128_encrypt_block(const ap_aes128* c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    ap__add_rk(s, c->rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = ap__sbox[s[i]];
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) t[4 * col + row] = s[4 * ((col + row) & 3) + row];
        if (r < 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t* a = t + 4 * col;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                s[4 * col + 0] = (uint8_t)(ap__xt(a0) ^ (ap__xt(a1) ^ a1) ^ a2 ^ a3);
                s[4 * col + 1] = (uint8_t)(a0 ^ ap__xt(a1) ^ (ap__xt(a2) ^ a2) ^ a3);
                s[4 * col + 2] = (uint8_t)(a0 ^ a1 ^ ap__xt(a2) ^ (ap__xt(a3) ^ a3));
                s[4 * col + 3] = (uint8_t)((ap__xt(a0) ^ a0) ^ a1 ^ a2 ^ ap__xt(a3));
            }
        } else memcpy(s, t, 16);
        ap__add_rk(s, c->rk + 4 * r);
    }
    memcpy(out, s, 16);
}
void ap_aes128_decrypt_block(const ap_aes128* c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    ap__add_rk(s, c->rk + 40);
    for (int r = 9; r >= 0; r--) {
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) t[4 * ((col + row) & 3) + row] = s[4 * col + row];
        for (int i = 0; i < 16; i++) s[i] = ap__isbox[t[i]];
        ap__add_rk(s, c->rk + 4 * r);
        if (r > 0) {
            for (int col = 0; col < 4; col++) {
                uint8_t* a = s + 4 * col;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(ap__mul(a0, 14) ^ ap__mul(a1, 11) ^ ap__mul(a2, 13) ^ ap__mul(a3, 9));
                a[1] = (uint8_t)(ap__mul(a0, 9) ^ ap__mul(a1, 14) ^ ap__mul(a2, 11) ^ ap__mul(a3, 13));
                a[2] = (uint8_t)(ap__mul(a0, 13) ^ ap__mul(a1, 9) ^ ap__mul(a2, 14) ^ ap__mul(a3, 11));
                a[3] = (uint8_t)(ap__mul(a0, 11) ^ ap__mul(a1, 13) ^ ap__mul(a2, 9) ^ ap__mul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}
void ap_aes128_cbc_decrypt(const ap_aes128* c, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n) {
    uint8_t prev[16], cur[16], tmp[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        memcpy(cur, in + i, 16);
        ap_aes128_decrypt_block(c, cur, tmp);
        for (int j = 0; j < 16; j++) out[i + j] = (uint8_t)(tmp[j] ^ prev[j]);
        memcpy(prev, cur, 16);
    }
    /* a trailing partial block is not encrypted by AirPlay: copy through */
    size_t full = n & ~(size_t)15;
    if (n > full && out != in) memcpy(out + full, in + full, n - full);
}
void ap_aes_ctr_init(ap_aes_ctr* c, const uint8_t key[16], const uint8_t iv[16]) {
    ap_aes128_init(&c->aes, key);
    memcpy(c->ctr, iv, 16);
    c->used = 16;
}
void ap_aes_ctr_xor(ap_aes_ctr* c, const uint8_t* in, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (c->used == 16) {
            ap_aes128_encrypt_block(&c->aes, c->ctr, c->ks);
            for (int j = 15; j >= 0; j--) if (++c->ctr[j]) break;     /* big-endian counter */
            c->used = 0;
        }
        out[i] = (uint8_t)(in[i] ^ c->ks[c->used++]);
    }
}


/* ------------------------------------------------------------- ALAC decoder */
typedef struct { const uint8_t* p; size_t nbits, pos; } ap__br;
static uint32_t ap__brbits(ap__br* b, int n) {                 /* n <= 25 */
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t at = b->pos++;
        int bit = at < b->nbits ? (b->p[at >> 3] >> (7 - (at & 7))) & 1 : 0;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}
static uint32_t ap__brlong(ap__br* b, int n) { uint32_t v = 0; while (n > 16) { v = (v << 16) | ap__brbits(b, 16); n -= 16; } return (v << n) | ap__brbits(b, n); }
static int32_t ap__sext(uint32_t v, int bits) { return bits >= 32 ? (int32_t)v : (int32_t)(v << (32 - bits)) >> (32 - bits); }
static int ap__log2(unsigned v) { int r = 0; while (v >>= 1) r++; return r; }
static unsigned ap__alac_scalar(ap__br* b, int k, int bps) {
    unsigned x = 0;
    while (x < 9 && ap__brbits(b, 1)) x++;
    if (x > 8) return ap__brlong(b, bps);
    if (k != 1) {
        size_t save = b->pos;
        unsigned extra = ap__brbits(b, k);
        x = (x << k) - x;
        if (extra > 1) x += extra - 1;
        else b->pos = save + (size_t)(k - 1);
    }
    return x;
}
int ap_alac_init(ap_alac* d, const uint8_t* c, size_t n) {
    memset(d, 0, sizeof(*d));
    if (n >= 36) { c += 12; n -= 12; }
    if (n < 24) return -1;
    d->frame_len = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
    d->bits = c[5]; d->pb = c[6]; d->mb = c[7]; d->kb = c[8]; d->channels = c[9];
    if (d->frame_len == 0 || d->frame_len > 16384 || d->bits != 16 || d->channels < 1 || d->channels > 2) return -2;
    for (int i = 0; i < d->channels; i++) {
        d->buf[i] = (int32_t*)calloc(d->frame_len, 4); d->err[i] = (int32_t*)calloc(d->frame_len, 4); d->extra[i] = (int32_t*)calloc(d->frame_len, 4);
        if (!d->buf[i] || !d->err[i] || !d->extra[i]) { ap_alac_free(d); return -3; }
    }
    return 0;
}
void ap_alac_free(ap_alac* d) { for (int i = 0; i < 2; i++) { free(d->buf[i]); free(d->err[i]); free(d->extra[i]); d->buf[i] = d->err[i] = d->extra[i] = NULL; } }
static void ap__alac_lpc(const int32_t* e, int32_t* out, int n, int bps, int16_t* coefs, int order, int quant) {
    out[0] = e[0];
    if (n <= 1) return;
    if (!order) { memcpy(out + 1, e + 1, (size_t)(n - 1) * 4); return; }
    if (order == 31) { for (int i = 1; i < n; i++) out[i] = ap__sext((uint32_t)(out[i - 1] + e[i]), bps); return; }
    int i;
    for (i = 1; i <= order && i < n; i++) out[i] = ap__sext((uint32_t)(out[i - 1] + e[i]), bps);
    const int32_t* pred = out;
    for (; i < n; i++) {
        int32_t val = 0, ev = e[i];
        int d = *pred++;
        for (int j = 0; j < order; j++) val += (pred[j] - d) * coefs[j];
        val = (val + (1 << (quant - 1))) >> quant;
        val += d + ev;
        out[i] = ap__sext((uint32_t)val, bps);
        int es = (ev > 0) - (ev < 0);
        if (es) {
            for (int j = 0; j < order && ev * es > 0; j++) {
                int v = d - pred[j];
                int sg = ((v > 0) - (v < 0)) * es;
                coefs[j] = (int16_t)(coefs[j] - sg);
                v *= sg;
                ev -= (v >> quant) * (j + 1);
            }
        }
    }
}
int ap_alac_decode(ap_alac* d, const uint8_t* in, size_t n, int16_t* out, size_t max_frames) {
    ap__br b = { in, n * 8, 0 };
    int frames = -1;
    while (b.pos + 3 <= b.nbits) {
        int element = (int)ap__brbits(&b, 3);
        if (element == 7) break;
        if (element > 1) return -1;                                   /* SCE / CPE only */
        int channels = element + 1;
        if (channels > d->channels) return -1;
        ap__brbits(&b, 4); ap__brbits(&b, 12);
        int has_size = (int)ap__brbits(&b, 1);
        int extra_bits = (int)ap__brbits(&b, 2) << 3;
        int bps = d->bits - extra_bits + channels - 1;
        if (bps > 32) return -1;
        int compressed = !ap__brbits(&b, 1);
        uint32_t ns = has_size ? ap__brlong(&b, 32) : d->frame_len;
        if (ns == 0 || ns > d->frame_len || ns > max_frames) return -1;
        int decorr_shift = 0, decorr_w = 0;
        if (compressed) {
            int lpc_mode[2], lpc_quant[2], mult[2], order[2];
            int16_t coefs[2][32];
            decorr_shift = (int)ap__brbits(&b, 8); decorr_w = (int)ap__brbits(&b, 8);
            for (int ch = 0; ch < channels; ch++) {
                lpc_mode[ch] = (int)ap__brbits(&b, 4); lpc_quant[ch] = (int)ap__brbits(&b, 4);
                mult[ch] = (int)ap__brbits(&b, 3); order[ch] = (int)ap__brbits(&b, 5);
                if (order[ch] > 31 || lpc_quant[ch] == 0) return -1;
                for (int i = order[ch] - 1; i >= 0; i--) coefs[ch][i] = (int16_t)ap__sext(ap__brbits(&b, 16), 16);
            }
            if (extra_bits) for (uint32_t i = 0; i < ns; i++) for (int ch = 0; ch < channels; ch++) d->extra[ch][i] = (int32_t)ap__brbits(&b, extra_bits);
            for (int ch = 0; ch < channels; ch++) {                       /* adaptive Rice residuals */
                int history = d->mb, sign_mod = 0, pbm = d->pb * mult[ch] / 4;
                int32_t* o = d->err[ch];
                for (uint32_t i = 0; i < ns; ) {
                    int k = ap__log2((unsigned)(history >> 9) + 3);
                    if (k > d->kb) k = d->kb;
                    unsigned x = ap__alac_scalar(&b, k, bps) + (unsigned)sign_mod;
                    sign_mod = 0;
                    o[i++] = (int32_t)((x >> 1) ^ (unsigned)-(int)(x & 1));
                    if (x > 0xffff) history = 0xffff;
                    else history += (int)x * pbm - ((history * pbm) >> 9);
                    if (history < 128 && i < ns) {
                        int k2 = 7 - ap__log2((unsigned)history) + ((history + 16) >> 6);
                        if (k2 > d->kb) k2 = d->kb;
                        int block = (int)ap__alac_scalar(&b, k2, 16);
                        if (block > 0) { if ((uint32_t)block > ns - i) return -1; memset(o + i, 0, (size_t)block * 4); i += (uint32_t)block; }
                        if (block <= 0xffff) sign_mod = 1;
                        history = 0;
                    }
                }
            }
            for (int ch = 0; ch < channels; ch++) {
                if (lpc_mode[ch] == 15) ap__alac_lpc(d->err[ch], d->err[ch], (int)ns, bps, NULL, 31, 0);
                ap__alac_lpc(d->err[ch], d->buf[ch], (int)ns, bps, coefs[ch], order[ch], lpc_quant[ch]);
            }
        } else {                                                          /* stored as-is */
            for (uint32_t i = 0; i < ns; i++) for (int ch = 0; ch < channels; ch++) d->buf[ch][i] = ap__sext(ap__brlong(&b, d->bits), d->bits);
            extra_bits = 0;
        }
        if (channels == 2 && decorr_shift) {
            for (uint32_t i = 0; i < ns; i++) {
                int32_t a = d->buf[0][i], bb = d->buf[1][i];
                a -= (bb * decorr_w) >> decorr_shift;
                bb += a;
                d->buf[0][i] = bb; d->buf[1][i] = a;
            }
        }
        for (uint32_t i = 0; i < ns; i++) for (int ch = 0; ch < channels; ch++) {
            int32_t s = d->buf[ch][i];
            if (extra_bits) s = (s << extra_bits) | d->extra[ch][i];
            out[(size_t)i * (size_t)d->channels + (size_t)ch] = (int16_t)s;
        }
        frames = (int)ns;
        if (channels == d->channels) break;                              /* one element carries every channel */
    }
    return frames;
}

/* ====================================================================== */
/* Platform layer: sockets, threads, time                                  */
/* ====================================================================== */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <windows.h>
typedef SOCKET ap_sock;
typedef HANDLE ap_thread;
typedef CRITICAL_SECTION ap_mutex;
#  define AP_BADSOCK INVALID_SOCKET
#  define AP_THREAD_RET DWORD WINAPI
#  define AP_CLOSESOCK(s) closesocket(s)
static void ap__mutex_init(ap_mutex* m) { InitializeCriticalSection(m); }
static void ap__mutex_lock(ap_mutex* m) { EnterCriticalSection(m); }
static void ap__mutex_unlock(ap_mutex* m) { LeaveCriticalSection(m); }
static void ap__mutex_destroy(ap_mutex* m) { DeleteCriticalSection(m); }
static int ap__thread_start(ap_thread* t, DWORD (WINAPI *fn)(LPVOID), void* arg) { *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
static void ap__thread_join(ap_thread t) { if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
static void ap__sleep_ns(uint64_t ns) { Sleep((DWORD)((ns + 999999ULL) / 1000000ULL)); }
static uint64_t ap__now_ns(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart); }
static void ap__net_init(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
static void ap__set_timeout(ap_sock s, int ms) { DWORD t = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t)); }
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
typedef int ap_sock;
typedef pthread_t ap_thread;
typedef pthread_mutex_t ap_mutex;
#  define AP_BADSOCK (-1)
#  define AP_THREAD_RET void*
#  define AP_CLOSESOCK(s) close(s)
static void ap__mutex_init(ap_mutex* m) { pthread_mutex_init(m, NULL); }
static void ap__mutex_lock(ap_mutex* m) { pthread_mutex_lock(m); }
static void ap__mutex_unlock(ap_mutex* m) { pthread_mutex_unlock(m); }
static void ap__mutex_destroy(ap_mutex* m) { pthread_mutex_destroy(m); }
static int ap__thread_start(ap_thread* t, void* (*fn)(void*), void* arg) { return pthread_create(t, NULL, fn, arg); }
static void ap__thread_join(ap_thread t) { pthread_join(t, NULL); }
static void ap__sleep_ns(uint64_t ns) { struct timespec ts = { (time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL) }; nanosleep(&ts, NULL); }
static uint64_t ap__now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec; }
static void ap__net_init(void) {}
static void ap__set_timeout(ap_sock s, int ms) { struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
#endif

static void ap__shutdown_sock(ap_sock s) {
    if (s == AP_BADSOCK) return;
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
}

static void ap__put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t ap__get32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t ap__get32le(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint64_t ap__get64le(const uint8_t* p) { return (uint64_t)ap__get32le(p) | ((uint64_t)ap__get32le(p + 4) << 32); }
static uint16_t ap__get16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void ap__random(uint8_t* out, size_t n) {
    /* Seed material only; nothing here must be unpredictable to a remote party beyond key generation,
     * so prefer the OS CSPRNG and fall back to time+address entropy. */
#ifdef _WIN32
    typedef BOOLEAN (WINAPI *rtlgen_t)(PVOID, ULONG);
    HMODULE h = LoadLibraryA("advapi32.dll");
    rtlgen_t f = h ? (rtlgen_t)GetProcAddress(h, "SystemFunction036") : NULL;
    if (f && f(out, (ULONG)n)) return;
#else
    FILE* fp = fopen("/dev/urandom", "rb");
    if (fp) { size_t got = fread(out, 1, n, fp); fclose(fp); if (got == n) return; }
#endif
    uint64_t t = ap__now_ns() ^ (uint64_t)(uintptr_t)out;
    for (size_t i = 0; i < n; i++) { t = t * 6364136223846793005ULL + 1442695040888963407ULL; out[i] = (uint8_t)(t >> 33); }
}

/* Primary IPv4 address (host byte order) found by asking the routing table. */
static int ap__local_ipv4(uint8_t ip[4]) {
    ap_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == AP_BADSOCK) return -1;
    struct sockaddr_in to, me;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET; to.sin_port = htons(9); to.sin_addr.s_addr = htonl(0xE00000FBu);   /* 224.0.0.251 */
    int ok = -1;
    if (connect(s, (struct sockaddr*)&to, sizeof(to)) == 0) {
        socklen_t l = sizeof(me);
        if (getsockname(s, (struct sockaddr*)&me, &l) == 0 && me.sin_addr.s_addr != 0) { memcpy(ip, &me.sin_addr.s_addr, 4); ok = 0; }
    }
    AP_CLOSESOCK(s);
    if (ok != 0) { ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1; }
    return ok;
}

static ap_sock ap__tcp_listen(int port, int* bound_port) {
    ap_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == AP_BADSOCK) return AP_BADSOCK;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 8) != 0) { AP_CLOSESOCK(s); return AP_BADSOCK; }
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &l);
    if (bound_port) *bound_port = ntohs(a.sin_port);
    return s;
}
static ap_sock ap__udp_bind(int port, int* bound_port) {
    ap_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == AP_BADSOCK) return AP_BADSOCK;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { AP_CLOSESOCK(s); return AP_BADSOCK; }
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &l);
    if (bound_port) *bound_port = ntohs(a.sin_port);
    return s;
}
static int ap__recv_all(ap_sock s, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = (int)recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return -2;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) { if (got == 0) return -2; continue; }
        if (errno == EINTR) continue;
#endif
        return -1;
    }
    return 0;
}
static int ap__send_all(ap_sock s, const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
#ifdef MSG_NOSIGNAL
        int r = (int)send(s, (const char*)buf + sent, (int)(n - sent), MSG_NOSIGNAL);
#else
        int r = (int)send(s, (const char*)buf + sent, (int)(n - sent), 0);
#endif
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

/* ====================================================================== */
/* Binary plist (bplist00) — just enough for AirPlay                       */
/* ====================================================================== */
enum { AP_PL_NULL, AP_PL_BOOL, AP_PL_INT, AP_PL_REAL, AP_PL_DATA, AP_PL_STR, AP_PL_ARRAY, AP_PL_DICT };
typedef struct ap_pl {
    int t;
    int64_t i;                 /* bool / int */
    double r;
    uint8_t* d; size_t n;      /* data / string (string is NUL-terminated, n excludes NUL) */
    struct ap_pl** v;          /* array items / dict values */
    char** k;                  /* dict keys */
    size_t cnt, cap;
} ap_pl;

static ap_pl* ap_pl_new(int t) { ap_pl* p = (ap_pl*)calloc(1, sizeof(*p)); if (p) p->t = t; return p; }
static void ap_pl_free(ap_pl* p) {
    if (!p) return;
    for (size_t i = 0; i < p->cnt; i++) { ap_pl_free(p->v[i]); if (p->k) free(p->k[i]); }
    free(p->v); free(p->k); free(p->d); free(p);
}
static ap_pl* ap_pl_int(int64_t i) { ap_pl* p = ap_pl_new(AP_PL_INT); if (p) p->i = i; return p; }
static ap_pl* ap_pl_bool(int b) { ap_pl* p = ap_pl_new(AP_PL_BOOL); if (p) p->i = b != 0; return p; }
static ap_pl* ap_pl_real(double r) { ap_pl* p = ap_pl_new(AP_PL_REAL); if (p) p->r = r; return p; }
static ap_pl* ap_pl_str(const char* s) {
    ap_pl* p = ap_pl_new(AP_PL_STR);
    if (!p) return NULL;
    p->n = strlen(s); p->d = (uint8_t*)malloc(p->n + 1);
    if (p->d) memcpy(p->d, s, p->n + 1);
    return p;
}
static ap_pl* ap_pl_data(const void* d, size_t n) {
    ap_pl* p = ap_pl_new(AP_PL_DATA);
    if (!p) return NULL;
    p->n = n; p->d = (uint8_t*)malloc(n ? n : 1);
    if (p->d && n) memcpy(p->d, d, n);
    return p;
}
static int ap__pl_grow(ap_pl* p) {
    if (p->cnt < p->cap) return 0;
    size_t nc = p->cap ? p->cap * 2 : 8;
    ap_pl** nv = (ap_pl**)realloc(p->v, nc * sizeof(*nv));
    if (!nv) return -1;
    p->v = nv;
    if (p->t == AP_PL_DICT) {
        char** nk = (char**)realloc(p->k, nc * sizeof(*nk));
        if (!nk) return -1;
        p->k = nk;
    }
    p->cap = nc;
    return 0;
}
static void ap_pl_push(ap_pl* arr, ap_pl* v) { if (arr && v && ap__pl_grow(arr) == 0) arr->v[arr->cnt++] = v; }
static void ap_pl_set(ap_pl* dict, const char* key, ap_pl* v) {
    if (!dict || !v || ap__pl_grow(dict) != 0) return;
    size_t kl = strlen(key);
    char* k = (char*)malloc(kl + 1);
    if (!k) return;
    memcpy(k, key, kl + 1);
    dict->k[dict->cnt] = k; dict->v[dict->cnt] = v; dict->cnt++;
}
static ap_pl* ap_pl_get(const ap_pl* dict, const char* key) {
    if (!dict || dict->t != AP_PL_DICT) return NULL;
    for (size_t i = 0; i < dict->cnt; i++) if (!strcmp(dict->k[i], key)) return dict->v[i];
    return NULL;
}

/* -- parse -- */
typedef struct { const uint8_t* b; size_t n; size_t off_size, ref_size, num_obj; size_t table; int depth; } ap__plctx;
static uint64_t ap__pl_be(const uint8_t* p, size_t n) { uint64_t v = 0; for (size_t i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }
static ap_pl* ap__pl_obj(ap__plctx* c, size_t idx);
static int ap__pl_len(ap__plctx* c, size_t* pos, uint8_t info, size_t* len) {
    if (info != 0xF) { *len = info; return 0; }
    if (*pos >= c->n) return -1;
    uint8_t m = c->b[(*pos)++];
    if ((m >> 4) != 1) return -1;
    size_t sz = (size_t)1 << (m & 15);
    if (sz > 8 || *pos + sz > c->n) return -1;
    *len = (size_t)ap__pl_be(c->b + *pos, sz);
    *pos += sz;
    return 0;
}
static ap_pl* ap__pl_obj(ap__plctx* c, size_t idx) {
    if (idx >= c->num_obj || ++c->depth > 32) { c->depth--; return NULL; }
    size_t off_at = c->table + idx * c->off_size;
    if (off_at + c->off_size > c->n) { c->depth--; return NULL; }
    size_t pos = (size_t)ap__pl_be(c->b + off_at, c->off_size);
    ap_pl* r = NULL;
    if (pos >= c->n) { c->depth--; return NULL; }
    uint8_t m = c->b[pos++];
    uint8_t hi = m >> 4, lo = m & 15;
    size_t len;
    switch (hi) {
    case 0x0:
        if (m == 0x08) r = ap_pl_bool(0); else if (m == 0x09) r = ap_pl_bool(1); else r = ap_pl_new(AP_PL_NULL);
        break;
    case 0x1: {
        size_t sz = (size_t)1 << lo;
        if (sz > 8 || pos + sz > c->n) break;
        r = ap_pl_int((int64_t)ap__pl_be(c->b + pos, sz));
        break; }
    case 0x2: {
        size_t sz = (size_t)1 << lo;
        if ((sz != 4 && sz != 8) || pos + sz > c->n) break;
        if (sz == 4) { uint32_t u = (uint32_t)ap__pl_be(c->b + pos, 4); float f; memcpy(&f, &u, 4); r = ap_pl_real(f); }
        else { uint64_t u = ap__pl_be(c->b + pos, 8); double d; memcpy(&d, &u, 8); r = ap_pl_real(d); }
        break; }
    case 0x4:
        if (ap__pl_len(c, &pos, lo, &len) || pos + len > c->n) break;
        r = ap_pl_data(c->b + pos, len);
        break;
    case 0x5: case 0x6: {
        if (ap__pl_len(c, &pos, lo, &len)) break;
        size_t bytes = hi == 0x6 ? len * 2 : len;
        if (pos + bytes > c->n) break;
        if (hi == 0x5) { char* s = (char*)malloc(len + 1); if (!s) break; memcpy(s, c->b + pos, len); s[len] = 0; r = ap_pl_str(s); free(s); }
        else { char* s = (char*)malloc(len + 1); if (!s) break; for (size_t i = 0; i < len; i++) { uint16_t u = ap__get16be(c->b + pos + 2 * i); s[i] = u < 128 ? (char)u : '?'; } s[len] = 0; r = ap_pl_str(s); free(s); }
        break; }
    case 0xA: case 0xD: {
        if (ap__pl_len(c, &pos, lo, &len)) break;
        size_t groups = hi == 0xD ? 2 : 1;
        if (len > c->num_obj || pos + len * groups * c->ref_size > c->n) break;
        r = ap_pl_new(hi == 0xD ? AP_PL_DICT : AP_PL_ARRAY);
        if (!r) break;
        for (size_t i = 0; i < len; i++) {
            size_t kref = hi == 0xD ? (size_t)ap__pl_be(c->b + pos + i * c->ref_size, c->ref_size) : 0;
            size_t vref = (size_t)ap__pl_be(c->b + pos + (hi == 0xD ? (len + i) : i) * c->ref_size, c->ref_size);
            ap_pl* v = ap__pl_obj(c, vref);
            if (!v) { ap_pl_free(r); r = NULL; break; }
            if (hi == 0xD) {
                ap_pl* k = ap__pl_obj(c, kref);
                if (!k || k->t != AP_PL_STR) { ap_pl_free(k); ap_pl_free(v); ap_pl_free(r); r = NULL; break; }
                ap_pl_set(r, (const char*)k->d, v);
                ap_pl_free(k);
            } else ap_pl_push(r, v);
        }
        break; }
    default: break;
    }
    c->depth--;
    return r;
}
static ap_pl* ap_pl_parse(const uint8_t* b, size_t n) {
    if (n < 8 + 32 || memcmp(b, "bplist00", 8)) return NULL;
    const uint8_t* t = b + n - 32;
    ap__plctx c;
    c.b = b; c.n = n; c.depth = 0;
    c.off_size = t[6]; c.ref_size = t[7];
    c.num_obj = (size_t)ap__pl_be(t + 8, 8);
    size_t top = (size_t)ap__pl_be(t + 16, 8);
    c.table = (size_t)ap__pl_be(t + 24, 8);
    if (!c.off_size || c.off_size > 8 || !c.ref_size || c.ref_size > 8 || !c.num_obj) return NULL;
    if (c.table >= n || c.num_obj > (n - c.table) / c.off_size) return NULL;
    return ap__pl_obj(&c, top);
}

/* -- write -- */
typedef struct { uint8_t* b; size_t n, cap; } ap__buf;
static int ap__buf_put(ap__buf* o, const void* d, size_t n) {
    if (o->n + n > o->cap) {
        size_t nc = o->cap ? o->cap : 256;
        while (nc < o->n + n) nc *= 2;
        uint8_t* nb = (uint8_t*)realloc(o->b, nc);
        if (!nb) return -1;
        o->b = nb; o->cap = nc;
    }
    if (n) memcpy(o->b + o->n, d, n);
    o->n += n;
    return 0;
}
static void ap__buf_be(ap__buf* o, uint64_t v, size_t sz) { uint8_t t[8]; for (size_t i = 0; i < sz; i++) t[i] = (uint8_t)(v >> (8 * (sz - 1 - i))); ap__buf_put(o, t, sz); }
static size_t ap__pl_count(const ap_pl* p) {
    size_t c = 1;
    for (size_t i = 0; i < p->cnt; i++) c += ap__pl_count(p->v[i]) + (p->t == AP_PL_DICT ? 1 : 0);
    return c;
}
/* Serialize with object order: each node, then (for dicts) interleaved key objects. */
typedef struct { ap__buf out; size_t* offs; size_t cnt, ref; } ap__plw;
static void ap__pl_hdr(ap__buf* o, uint8_t type, size_t len) {
    if (len < 15) { uint8_t m = (uint8_t)(type | len); ap__buf_put(o, &m, 1); }
    else { uint8_t m = (uint8_t)(type | 0xF); ap__buf_put(o, &m, 1); uint8_t sz = 4; uint8_t im = (uint8_t)(0x10 | 2); (void)sz; ap__buf_put(o, &im, 1); ap__buf_be(o, len, 4); }
}
static size_t ap__pl_write_obj(ap__plw* w, const ap_pl* p);
static size_t ap__pl_write_str(ap__plw* w, const char* s) {
    size_t id = w->cnt++;
    w->offs[id] = w->out.n;
    size_t l = strlen(s);
    ap__pl_hdr(&w->out, 0x50, l);
    ap__buf_put(&w->out, s, l);
    return id;
}
static size_t ap__pl_write_obj(ap__plw* w, const ap_pl* p) {
    size_t kids[2048], keyids[2048];   /* AirPlay bodies are small; bound the container size */
    size_t id;
    size_t n = p->cnt < 2048 ? p->cnt : 2048;
    if (p->t == AP_PL_ARRAY || p->t == AP_PL_DICT) {
        for (size_t i = 0; i < n; i++) {
            if (p->t == AP_PL_DICT) keyids[i] = ap__pl_write_str(w, p->k[i]);
            kids[i] = ap__pl_write_obj(w, p->v[i]);
        }
    }
    id = w->cnt++;
    w->offs[id] = w->out.n;
    switch (p->t) {
    case AP_PL_BOOL: { uint8_t m = p->i ? 0x09 : 0x08; ap__buf_put(&w->out, &m, 1); break; }
    case AP_PL_INT: {
        if (p->i >= 0 && p->i < 256) { uint8_t m = 0x10; ap__buf_put(&w->out, &m, 1); ap__buf_be(&w->out, (uint64_t)p->i, 1); }
        else if (p->i >= 0 && p->i < 65536) { uint8_t m = 0x11; ap__buf_put(&w->out, &m, 1); ap__buf_be(&w->out, (uint64_t)p->i, 2); }
        else if (p->i >= 0 && p->i < 0x100000000LL) { uint8_t m = 0x12; ap__buf_put(&w->out, &m, 1); ap__buf_be(&w->out, (uint64_t)p->i, 4); }
        else { uint8_t m = 0x13; ap__buf_put(&w->out, &m, 1); ap__buf_be(&w->out, (uint64_t)p->i, 8); }
        break; }
    case AP_PL_REAL: { uint8_t m = 0x23; uint64_t u; memcpy(&u, &p->r, 8); ap__buf_put(&w->out, &m, 1); ap__buf_be(&w->out, u, 8); break; }
    case AP_PL_DATA: ap__pl_hdr(&w->out, 0x40, p->n); ap__buf_put(&w->out, p->d, p->n); break;
    case AP_PL_STR: ap__pl_hdr(&w->out, 0x50, p->n); ap__buf_put(&w->out, p->d, p->n); break;
    case AP_PL_ARRAY:
        ap__pl_hdr(&w->out, 0xA0, n);
        for (size_t i = 0; i < n; i++) ap__buf_be(&w->out, kids[i], w->ref);
        break;
    case AP_PL_DICT:
        ap__pl_hdr(&w->out, 0xD0, n);
        for (size_t i = 0; i < n; i++) ap__buf_be(&w->out, keyids[i], w->ref);
        for (size_t i = 0; i < n; i++) ap__buf_be(&w->out, kids[i], w->ref);
        break;
    default: { uint8_t m = 0x00; ap__buf_put(&w->out, &m, 1); break; }
    }
    return id;
}
/* Returns malloc'd bplist00 bytes; the top object is written last and referenced from the trailer. */
static uint8_t* ap_pl_serialize(const ap_pl* root, size_t* out_n) {
    ap__plw w;
    size_t total = ap__pl_count(root);
    memset(&w, 0, sizeof(w));
    w.ref = total < 256 ? 1 : 2;
    w.offs = (size_t*)calloc(total + 8, sizeof(size_t));
    if (!w.offs) return NULL;
    ap__buf_put(&w.out, "bplist00", 8);
    size_t top = ap__pl_write_obj(&w, root);
    size_t table = w.out.n;
    for (size_t i = 0; i < w.cnt; i++) ap__buf_be(&w.out, w.offs[i], 4);
    uint8_t trailer[32] = { 0 };
    trailer[6] = 4; trailer[7] = (uint8_t)w.ref;
    for (int i = 0; i < 8; i++) { trailer[8 + i] = (uint8_t)((uint64_t)w.cnt >> (8 * (7 - i))); trailer[16 + i] = (uint8_t)((uint64_t)top >> (8 * (7 - i))); trailer[24 + i] = (uint8_t)((uint64_t)table >> (8 * (7 - i))); }
    ap__buf_put(&w.out, trailer, 32);
    free(w.offs);
    *out_n = w.out.n;
    return w.out.b;
}

/* ====================================================================== */
/* mDNS responder (RFC 6762, just the service records AirPlay needs)       */
/* ====================================================================== */
typedef struct {
    char instance[200];          /* service instance label (may contain spaces) */
    char type[40];               /* "_airplay._tcp" */
    int port;
    uint8_t txt[1500]; size_t txt_n;
} ap__svc;

static void ap__txt_add(ap__svc* s, const char* k, const char* v) {
    size_t kl = strlen(k), vl = strlen(v), l = kl + 1 + vl;
    if (l > 255 || s->txt_n + 1 + l > sizeof(s->txt)) return;
    s->txt[s->txt_n++] = (uint8_t)l;
    memcpy(s->txt + s->txt_n, k, kl); s->txt_n += kl;
    s->txt[s->txt_n++] = '=';
    memcpy(s->txt + s->txt_n, v, vl); s->txt_n += vl;
}
/* Append a dotted name as labels; `label0` (if set) is emitted as ONE label even if it holds dots. */
static size_t ap__dns_name(uint8_t* o, const char* label0, const char* rest) {
    size_t n = 0;
    if (label0) { size_t l = strlen(label0); if (l > 63) l = 63; o[n++] = (uint8_t)l; memcpy(o + n, label0, l); n += l; }
    const char* p = rest;
    while (p && *p) {
        const char* dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l > 63) l = 63;
        o[n++] = (uint8_t)l; memcpy(o + n, p, l); n += l;
        p = dot ? dot + 1 : NULL;
    }
    o[n++] = 0;
    return n;
}
static size_t ap__dns_rr(uint8_t* o, const uint8_t* name, size_t name_n, uint16_t type, uint16_t cls, uint32_t ttl, const uint8_t* rd, size_t rd_n) {
    size_t n = 0;
    memcpy(o, name, name_n); n += name_n;
    o[n++] = (uint8_t)(type >> 8); o[n++] = (uint8_t)type;
    o[n++] = (uint8_t)(cls >> 8); o[n++] = (uint8_t)cls;
    ap__put32be(o + n, ttl); n += 4;
    o[n++] = (uint8_t)(rd_n >> 8); o[n++] = (uint8_t)rd_n;
    memcpy(o + n, rd, rd_n); n += rd_n;
    return n;
}
/* Builds PTR + SRV + TXT + A for one service into `pkt` (response header included). */
static size_t ap__mdns_build(uint8_t* pkt, const ap__svc* s, const char* host, const uint8_t ip[4], uint32_t ttl) {
    uint8_t nm_type[128], nm_inst[300], tgt[128], rd[1600], tmp[300];
    char type_dom[64];
    snprintf(type_dom, sizeof(type_dom), "%s.local", s->type);
    size_t type_n = ap__dns_name(nm_type, NULL, type_dom);
    size_t inst_n = ap__dns_name(nm_inst, s->instance, type_dom);
    char host_dom[160];
    snprintf(host_dom, sizeof(host_dom), "%s.local", host);
    size_t host_n = ap__dns_name(tgt, NULL, host_dom);
    memset(pkt, 0, 12);
    pkt[2] = 0x84; pkt[3] = 0x00;
    size_t n = 12;
    int an = 0;
    n += ap__dns_rr(pkt + n, nm_type, type_n, 12, 0x0001, ttl, nm_inst, inst_n); an++;
    /* SRV */
    rd[0] = rd[1] = rd[2] = rd[3] = 0; rd[4] = (uint8_t)(s->port >> 8); rd[5] = (uint8_t)s->port;
    memcpy(rd + 6, tgt, host_n);
    n += ap__dns_rr(pkt + n, nm_inst, inst_n, 33, 0x8001, ttl, rd, 6 + host_n); an++;
    n += ap__dns_rr(pkt + n, nm_inst, inst_n, 16, 0x8001, ttl, s->txt, s->txt_n ? s->txt_n : 1); an++;
    n += ap__dns_rr(pkt + n, tgt, host_n, 1, 0x8001, ttl, ip, 4); an++;
    /* DNS-SD service enumeration */
    size_t enum_n = ap__dns_name(tmp, NULL, "_services._dns-sd._udp.local");
    n += ap__dns_rr(pkt + n, tmp, enum_n, 12, 0x0001, ttl, nm_type, type_n); an++;
    pkt[6] = (uint8_t)(an >> 8); pkt[7] = (uint8_t)an;
    return n;
}
/* Does the DNS message contain a question for any of our names?  (compressed names handled) */
static int ap__dns_qname(const uint8_t* m, size_t n, size_t* pos, char* out, size_t cap) {
    size_t p = *pos, o = 0; int jumped = 0, hops = 0; size_t end = 0;
    for (;;) {
        if (p >= n || ++hops > 64) return -1;
        uint8_t l = m[p];
        if (l == 0) { p++; break; }
        if ((l & 0xC0) == 0xC0) { if (p + 1 >= n) return -1; size_t ptr = (size_t)(((l & 0x3F) << 8) | m[p + 1]); if (!jumped) end = p + 2; jumped = 1; p = ptr; continue; }
        if (l > 63 || p + 1 + l > n) return -1;
        if (o + l + 2 >= cap) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, m + p + 1, l); o += l;
        p += 1 + (size_t)l;
    }
    out[o] = 0;
    *pos = jumped ? end : p;
    return 0;
}

typedef struct { ap__svc svc[2]; int nsvc; char host[96]; uint8_t ip[4]; } ap__mdns_cfg;

/* ====================================================================== */
/* The receiver                                                             */
/* ====================================================================== */
#define AP_MAX_CONN 8
typedef struct ap_conn ap_conn;
struct airplay {
    airplay_config cfg;
    airplay_callbacks cb;
    airplay_fairplay_ops fp;
    char name[96];
    char device_id[18];              /* "AA:BB:CC:DD:EE:FF" */
    char pk_hex[65];
    uint8_t ed_seed[32], ed_pk[32];
    ap__mdns_cfg mdns;
    ap_sock listen_sock, mdns_sock;
    int port;
    volatile int running;
    ap_thread th_accept, th_mdns;
    ap_mutex lock;
    ap_conn* conns[AP_MAX_CONN];
    int started;
    /* A/V sync scheduler */
    struct ap__ev* q_head; int q_n;
    ap_mutex q_lock; ap_thread th_sched; volatile int sched_run;
    int64_t offset_ns; int have_offset;
};

struct ap_conn {
    airplay_t* a;
    ap_sock s;
    ap_thread th;
    volatile int alive;
    char peer[48];
    /* pairing */
    int pair_state;                  /* 0 none, 1 setup, 2 handshake, 3 verified */
    uint8_t ed_theirs[32], ecdh_ours[32], ecdh_theirs[32], ecdh_secret[32];
    int have_secret;
    /* FairPlay + keys */
    void* fp;
    uint8_t aeskey[16], aesiv[16];
    int have_keys;
    /* streams */
    volatile int mirror_run, audio_run;
    ap_sock mirror_listen, audio_data, audio_ctrl;
    ap_thread th_mirror, th_audio;
    int mirror_port, audio_port, audio_ctrl_port;
    int audio_ct;
    uint64_t stream_id;
    int announced;
    /* audio timing */
    int have_sync; uint32_t sync_rtp; uint64_t sync_ns;
    uint32_t seen[256]; ap_alac alac; int alac_ok;
};

#ifdef _WIN32
#  define AP_THREAD_RV 0
#else
#  define AP_THREAD_RV NULL
#endif

static void ap__log(airplay_t* a, int level, const char* fmt, ...) {
    if (!a->cb.on_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    a->cb.on_log(a->cb.user, level, buf);
}

/* ---- HTTP/RTSP request/response ---- */
typedef struct {
    char method[16], url[512], proto[16];
    struct { char k[48]; char v[512]; } h[24];
    int nh;
    uint8_t* body; size_t body_n;
} ap__req;
typedef struct {
    int code; const char* status;
    const char* ctype;
    uint8_t* body; size_t body_n;
    int close_after;
    char extra[3][160]; int nextra;
} ap__resp;

static const char* ap__hdr(const ap__req* r, const char* k) {
    for (int i = 0; i < r->nh; i++) if (!strcasecmp(r->h[i].k, k)) return r->h[i].v;
    return NULL;
}
static void ap__req_free(ap__req* r) { free(r->body); r->body = NULL; }
static int ap__read_request(ap_conn* c, ap__req* r) {
    memset(r, 0, sizeof(*r));
    uint8_t hdr[16384];
    size_t n = 0;
    int have_end = 0;
    while (n < sizeof(hdr) - 1) {
        int rr = (int)recv(c->s, (char*)hdr + n, 1, 0);       /* byte-wise: headers are short, body length is exact */
        if (rr <= 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (rr < 0 && (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) && n == 0) return -2;
#else
            if (rr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && n == 0) return -2;
            if (rr < 0 && errno == EINTR) continue;
#endif
            return -1;
        }
        n++;
        if (n >= 4 && !memcmp(hdr + n - 4, "\r\n\r\n", 4)) { have_end = 1; break; }
    }
    if (!have_end) return -1;
    hdr[n] = 0;
    char* line = (char*)hdr;
    char* eol = strstr(line, "\r\n");
    if (!eol) return -1;
    *eol = 0;
    if (sscanf(line, "%15s %511s %15s", r->method, r->url, r->proto) != 3) return -1;
    char* p = eol + 2;
    while (*p && strncmp(p, "\r\n", 2)) {
        char* e = strstr(p, "\r\n");
        if (!e) break;
        *e = 0;
        char* colon = strchr(p, ':');
        if (colon && r->nh < 24) {
            *colon = 0;
            snprintf(r->h[r->nh].k, sizeof(r->h[r->nh].k), "%s", p);
            const char* v = colon + 1;
            while (*v == ' ') v++;
            snprintf(r->h[r->nh].v, sizeof(r->h[r->nh].v), "%s", v);
            r->nh++;
        }
        p = e + 2;
    }
    const char* cl = ap__hdr(r, "Content-Length");
    size_t len = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (len > (4u << 20)) return -1;
    if (len) {
        r->body = (uint8_t*)malloc(len);
        if (!r->body) return -1;
        if (ap__recv_all(c->s, r->body, len) != 0) { free(r->body); r->body = NULL; return -1; }
        r->body_n = len;
    }
    return 0;
}
static int ap__send_response(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    char head[1024];
    const char* cseq = ap__hdr(rq, "CSeq");
    int n = snprintf(head, sizeof(head), "%s %d %s\r\nServer: AirTunes/220.68\r\n", rq->proto, rs->code, rs->status);
    if (cseq) n += snprintf(head + n, sizeof(head) - (size_t)n, "CSeq: %s\r\n", cseq);
    for (int i = 0; i < rs->nextra; i++) n += snprintf(head + n, sizeof(head) - (size_t)n, "%s\r\n", rs->extra[i]);
    if (rs->ctype && rs->body_n) n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Type: %s\r\n", rs->ctype);
    n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Length: %zu\r\n\r\n", rs->body_n);
    int rc = ap__send_all(c->s, (const uint8_t*)head, (size_t)n);
    if (rc == 0 && rs->body_n) rc = ap__send_all(c->s, rs->body, rs->body_n);
    return rc;
}
static void ap__resp_header(ap__resp* rs, const char* line) {
    if (rs->nextra < 3) snprintf(rs->extra[rs->nextra++], sizeof(rs->extra[0]), "%s", line);
}
static void ap__resp_plist(ap__resp* rs, ap_pl* pl) {
    size_t n = 0;
    uint8_t* b = ap_pl_serialize(pl, &n);
    ap_pl_free(pl);
    rs->body = b; rs->body_n = b ? n : 0;
    rs->ctype = "application/x-apple-binary-plist";
}

/* ---- FairPlay in a separate process (pipe protocol: see airplay_fairplayd.c) ---- */
typedef struct {
    int up;
#ifdef _WIN32
    HANDLE wr, rd, proc;
#else
    int wr, rd; pid_t pid;
#endif
    ap_mutex lock;
} ap__fpd;
static ap__fpd ap__fpd_g;
typedef struct { ap__fpd* d; uint32_t id; } ap__fpdc;

static int ap__fpd_io(void* buf, size_t n, int write_side) {
    uint8_t* p = (uint8_t*)buf;
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
        DWORD k = 0;
        BOOL ok = write_side ? WriteFile(ap__fpd_g.wr, p + off, (DWORD)(n - off), &k, NULL) : ReadFile(ap__fpd_g.rd, p + off, (DWORD)(n - off), &k, NULL);
        if (!ok || k == 0) return -1;
#else
        ssize_t k = write_side ? write(ap__fpd_g.wr, p + off, n - off) : read(ap__fpd_g.rd, p + off, n - off);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
#endif
        off += (size_t)k;
    }
    return 0;
}
/* One request/reply round trip; returns the reply length, or <0 on failure. */
static int ap__fpd_call(uint8_t cmd, uint32_t id, const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    if (!ap__fpd_g.up) return -1;
    uint8_t h[7] = { cmd, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24), (uint8_t)n, (uint8_t)(n >> 8) };
    uint8_t r[3];
    int rc = -1;
    ap__mutex_lock(&ap__fpd_g.lock);
    if (ap__fpd_io(h, 7, 1) == 0 && (n == 0 || ap__fpd_io((void*)in, n, 1) == 0) && ap__fpd_io(r, 3, 0) == 0) {
        size_t rl = (size_t)(r[1] | (r[2] << 8));
        if (rl <= cap && (rl == 0 || ap__fpd_io(out, rl, 0) == 0)) rc = r[0] == 0 ? (int)rl : -2;
    }
    if (rc == -1) ap__fpd_g.up = 0;                                   /* the helper died: stop calling it */
    ap__mutex_unlock(&ap__fpd_g.lock);
    return rc;
}
static void* ap__fpdc_create(void* u) {
    (void)u;
    uint8_t out[8];
    if (ap__fpd_call(0, 0, NULL, 0, out, sizeof(out)) != 4) return NULL;
    ap__fpdc* c = (ap__fpdc*)calloc(1, sizeof(*c));
    if (c) { c->d = &ap__fpd_g; c->id = (uint32_t)out[0] | ((uint32_t)out[1] << 8) | ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24); }
    return c;
}
static int ap__fpdc_setup(void* c, const uint8_t* q, uint8_t* r) { return ap__fpd_call(1, ((ap__fpdc*)c)->id, q, 16, r, 142) == 142 ? 0 : -1; }
static int ap__fpdc_handshake(void* c, const uint8_t* q, uint8_t* r) { return ap__fpd_call(2, ((ap__fpdc*)c)->id, q, 164, r, 32) == 32 ? 0 : -1; }
static int ap__fpdc_decrypt(void* c, const uint8_t* q, uint8_t* r) { return ap__fpd_call(3, ((ap__fpdc*)c)->id, q, 72, r, 16) == 16 ? 0 : -1; }
static void ap__fpdc_destroy(void* c) { if (!c) return; uint8_t o[4]; ap__fpd_call(4, ((ap__fpdc*)c)->id, NULL, 0, o, sizeof(o)); free(c); }

static int ap__fpd_start(const char* path) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return -1;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    char cmd[1100]; snprintf(cmd, sizeof(cmd), "\"%s\"", path);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_r); CloseHandle(out_w);
    if (!ok) { CloseHandle(in_w); CloseHandle(out_r); return -1; }
    CloseHandle(pi.hThread);
    ap__fpd_g.wr = in_w; ap__fpd_g.rd = out_r; ap__fpd_g.proc = pi.hProcess;
    return 0;
#else
    int a[2], b[2];
    if (access(path, X_OK) != 0) return -1;
    if (pipe(a) != 0) return -1;
    if (pipe(b) != 0) { close(a[0]); close(a[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(a[0]); close(a[1]); close(b[0]); close(b[1]); return -1; }
    if (pid == 0) {
        dup2(a[0], 0); dup2(b[1], 1);
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
        execl(path, path, (char*)NULL);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    ap__fpd_g.wr = a[1]; ap__fpd_g.rd = b[0]; ap__fpd_g.pid = pid;
    fcntl(a[1], F_SETFD, FD_CLOEXEC); fcntl(b[0], F_SETFD, FD_CLOEXEC);
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}
int airplay_fairplay_spawn(airplay_fairplay_ops* ops, const char* path, char* err, size_t en) {
    if (!ap__fpd_g.up) {
        char cand[8][1100]; int nc = 0;
#ifdef _WIN32
        const char* name = "airplay-fairplayd.exe";
#else
        const char* name = "airplay-fairplayd";
#endif
        if (path && path[0]) snprintf(cand[nc++], sizeof(cand[0]), "%s", path);
        else {
            const char* e = getenv("LUNA_AIRPLAY_FAIRPLAYD");
            if (e && e[0]) snprintf(cand[nc++], sizeof(cand[0]), "%s", e);
#ifdef __linux__
            char exe[1024]; ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n > 0) { exe[n] = 0; char* sl = strrchr(exe, '/'); if (sl) { *sl = 0; snprintf(cand[nc++], sizeof(cand[0]), "%s/%s", exe, name); } }
#endif
            snprintf(cand[nc++], sizeof(cand[0]), "./%s", name);
            const char* home = getenv("HOME");
            if (home && home[0]) snprintf(cand[nc++], sizeof(cand[0]), "%s/.local/lib/%s", home, name);
            snprintf(cand[nc++], sizeof(cand[0]), "/usr/local/lib/luna/%s", name);
            snprintf(cand[nc++], sizeof(cand[0]), "/usr/lib/luna/%s", name);
            snprintf(cand[nc++], sizeof(cand[0]), "/usr/libexec/luna/%s", name);
            const char* pth = getenv("PATH");
            if (pth) { char* cp = strdup(pth); char* sv = NULL; for (char* d = strtok_r(cp, ":", &sv); d && nc < 8; d = strtok_r(NULL, ":", &sv)) { snprintf(cand[nc++], sizeof(cand[0]), "%s/%s", d, name); break; } free(cp); }
        }
        static int lock_ready;
        if (!lock_ready) { ap__mutex_init(&ap__fpd_g.lock); lock_ready = 1; }
        int started = 0;
        for (int i = 0; i < nc && !started; i++) started = ap__fpd_start(cand[i]) == 0;
        if (!started) { snprintf(err, en, "FairPlay helper (%s) not found", name); return -1; }
        ap__fpd_g.up = 1;
    }
    memset(ops, 0, sizeof(*ops));
    ops->create = ap__fpdc_create; ops->setup = ap__fpdc_setup; ops->handshake = ap__fpdc_handshake;
    ops->decrypt = ap__fpdc_decrypt; ops->destroy = ap__fpdc_destroy;
    return 0;
}

#ifdef AIRPLAY_PLAYFAIR
#  include "airplay_playfair.h"
static void* ap__pf_create(void* u) { (void)u; return ap_fairplay_init(); }
static int ap__pf_setup(void* c, const uint8_t* q, uint8_t* r) { return ap_fairplay_setup((ap_fairplay_t*)c, q, r); }
static int ap__pf_handshake(void* c, const uint8_t* q, uint8_t* r) { return ap_fairplay_handshake((ap_fairplay_t*)c, q, r); }
static int ap__pf_decrypt(void* c, const uint8_t* q, uint8_t* r) { return ap_fairplay_decrypt((ap_fairplay_t*)c, q, r); }
static void ap__pf_destroy(void* c) { ap_fairplay_destroy((ap_fairplay_t*)c); }
#endif

/* ---- pair-verify helpers (legacy pairing: ed25519 identity + x25519 session) ---- */
static void ap__pv_keys(const ap_conn* c, uint8_t key[16], uint8_t iv[16]) {
    uint8_t buf[64], h[64];
    size_t kl = strlen("Pair-Verify-AES-Key"), il = strlen("Pair-Verify-AES-IV");
    memcpy(buf, "Pair-Verify-AES-Key", kl); memcpy(buf + kl, c->ecdh_secret, 32);
    ap_sha512(h, buf, kl + 32); memcpy(key, h, 16);
    memcpy(buf, "Pair-Verify-AES-IV", il); memcpy(buf + il, c->ecdh_secret, 32);
    ap_sha512(h, buf, il + 32); memcpy(iv, h, 16);
}

/* ---- request handlers ---- */
static void ap__h_info(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    airplay_t* a = c->a;
    ap_pl* d = ap_pl_new(AP_PL_DICT);
    char macs[18];
    snprintf(macs, sizeof(macs), "%s", a->device_id);
    ap_pl_set(d, "deviceID", ap_pl_str(macs));
    ap_pl_set(d, "macAddress", ap_pl_str(macs));
    ap_pl_set(d, "pk", ap_pl_data(a->ed_pk, 32));
    ap_pl_set(d, "features", ap_pl_int((int64_t)0x5A7FFEE6));
    ap_pl_set(d, "name", ap_pl_str(a->name));
    ap_pl_set(d, "pi", ap_pl_str("2e388006-13ba-4041-9a67-25dd4a43d536"));
    ap_pl_set(d, "vv", ap_pl_int(2));
    ap_pl_set(d, "statusFlags", ap_pl_int(68));
    ap_pl_set(d, "keepAliveLowPower", ap_pl_int(1));
    ap_pl_set(d, "sourceVersion", ap_pl_str("220.68"));
    ap_pl_set(d, "keepAliveSendStatsAsBody", ap_pl_bool(1));
    ap_pl_set(d, "model", ap_pl_str("AppleTV3,2"));
    if (ap__hdr(rq, "CSeq")) {
        ap_pl_set(d, "initialVolume", ap_pl_real(-20.0));
        ap_pl* lat = ap_pl_new(AP_PL_ARRAY);
        for (int t = 100; t <= 101; t++) {
            ap_pl* e = ap_pl_new(AP_PL_DICT);
            ap_pl_set(e, "type", ap_pl_int(t)); ap_pl_set(e, "inputLatencyMicros", ap_pl_int(0));
            ap_pl_set(e, "audioType", ap_pl_str("default")); ap_pl_set(e, "outputLatencyMicros", ap_pl_bool(0));
            ap_pl_push(lat, e);
        }
        ap_pl_set(d, "audioLatencies", lat);
        ap_pl* fmts = ap_pl_new(AP_PL_ARRAY);
        for (int t = 100; t <= 101; t++) {
            ap_pl* e = ap_pl_new(AP_PL_DICT);
            ap_pl_set(e, "type", ap_pl_int(t)); ap_pl_set(e, "audioInputFormats", ap_pl_int(0x3fffffc)); ap_pl_set(e, "audioOutputFormats", ap_pl_int(0x3fffffc));
            ap_pl_push(fmts, e);
        }
        ap_pl_set(d, "audioFormats", fmts);
        ap_pl* disp = ap_pl_new(AP_PL_ARRAY);
        ap_pl* e = ap_pl_new(AP_PL_DICT);
        int w = a->cfg.width ? a->cfg.width : 1920, h = a->cfg.height ? a->cfg.height : 1080, fps = a->cfg.fps ? a->cfg.fps : 30;
        ap_pl_set(e, "uuid", ap_pl_str("e0ff8a27-6738-3d56-8a16-cc53aacee925"));
        ap_pl_set(e, "widthPhysical", ap_pl_int(0)); ap_pl_set(e, "heightPhysical", ap_pl_int(0));
        ap_pl_set(e, "width", ap_pl_int(w)); ap_pl_set(e, "height", ap_pl_int(h));
        ap_pl_set(e, "widthPixels", ap_pl_int(w)); ap_pl_set(e, "heightPixels", ap_pl_int(h));
        ap_pl_set(e, "rotation", ap_pl_bool(0)); ap_pl_set(e, "refreshRate", ap_pl_real(1.0 / fps));
        ap_pl_set(e, "maxFPS", ap_pl_int(fps)); ap_pl_set(e, "overscanned", ap_pl_bool(0)); ap_pl_set(e, "features", ap_pl_int(14));
        ap_pl_push(disp, e);
        ap_pl_set(d, "displays", disp);
    }
    ap__resp_plist(rs, d);
}

static void ap__h_pair_setup(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    if (rq->body_n != 32) { rs->code = 400; rs->status = "Bad Request"; return; }
    memcpy(c->ed_theirs, rq->body, 32);
    c->pair_state = 1;
    rs->body = (uint8_t*)malloc(32);
    if (!rs->body) return;
    memcpy(rs->body, c->a->ed_pk, 32); rs->body_n = 32;
    rs->ctype = "application/octet-stream";
}
static void ap__h_pair_verify(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    airplay_t* a = c->a;
    const uint8_t* d = rq->body;
    if (rq->body_n < 4) { rs->code = 400; rs->status = "Bad Request"; return; }
    if (d[0] == 1) {                                          /* step 1: ECDH keys in, our key + encrypted signature out */
        if (rq->body_n != 4 + 32 + 32) { rs->code = 400; rs->status = "Bad Request"; return; }
        memcpy(c->ecdh_theirs, d + 4, 32);
        memcpy(c->ed_theirs, d + 36, 32);
        uint8_t seed[32];
        ap__random(seed, 32);
        memcpy(c->ecdh_ours, seed, 32);
        uint8_t pub[32];
        ap_x25519_base(pub, c->ecdh_ours);
        ap_x25519(c->ecdh_secret, c->ecdh_ours, c->ecdh_theirs);
        c->have_secret = 1;
        uint8_t msg[64], sig[64], key[16], iv[16];
        memcpy(msg, pub, 32); memcpy(msg + 32, c->ecdh_theirs, 32);
        ap_ed25519_sign(sig, msg, 64, a->ed_seed);
        ap__pv_keys(c, key, iv);
        ap_aes_ctr ctr;
        ap_aes_ctr_init(&ctr, key, iv);
        ap_aes_ctr_xor(&ctr, sig, sig, 64);
        c->pair_state = 2;
        rs->body = (uint8_t*)malloc(96);
        if (!rs->body) return;
        memcpy(rs->body, pub, 32); memcpy(rs->body + 32, sig, 64); rs->body_n = 96;
        rs->ctype = "application/octet-stream";
    } else if (d[0] == 0) {                                   /* step 2: client's encrypted signature */
        if (rq->body_n != 4 + 64 || c->pair_state != 2) { rs->code = 400; rs->status = "Bad Request"; return; }
        uint8_t key[16], iv[16], sig[64], zero[64] = { 0 }, msg[64], pub[32];
        ap__pv_keys(c, key, iv);
        ap_aes_ctr ctr;
        ap_aes_ctr_init(&ctr, key, iv);
        ap_aes_ctr_xor(&ctr, zero, zero, 64);                 /* skip the keystream used for our signature */
        ap_aes_ctr_xor(&ctr, d + 4, sig, 64);
        ap_x25519_base(pub, c->ecdh_ours);
        memcpy(msg, c->ecdh_theirs, 32); memcpy(msg + 32, pub, 32);
        if (!ap_ed25519_verify(sig, msg, 64, c->ed_theirs)) {
            ap__log(a, 1, "pair-verify: signature rejected (%s)", c->peer);
            rs->code = 470; rs->status = "Connection Authorization Required"; rs->close_after = 1;
            return;
        }
        c->pair_state = 3;
        rs->ctype = "application/octet-stream";
    } else { rs->code = 400; rs->status = "Bad Request"; }
}
static void ap__h_fp_setup(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    airplay_t* a = c->a;
    if (!a->fp.setup) { rs->code = 501; rs->status = "Not Implemented"; return; }
    if (!c->fp && a->fp.create) c->fp = a->fp.create(a->fp.user);
    if (rq->body_n == 16) {
        rs->body = (uint8_t*)calloc(1, 142);
        if (rs->body && a->fp.setup(c->fp, rq->body, rs->body) == 0) rs->body_n = 142;
        else { free(rs->body); rs->body = NULL; rs->code = 500; rs->status = "FairPlay failed"; }
    } else if (rq->body_n == 164) {
        rs->body = (uint8_t*)calloc(1, 32);
        if (rs->body && a->fp.handshake(c->fp, rq->body, rs->body) == 0) rs->body_n = 32;
        else { free(rs->body); rs->body = NULL; rs->code = 500; rs->status = "FairPlay failed"; }
    } else { rs->code = 400; rs->status = "Bad Request"; return; }
    rs->ctype = "application/octet-stream";
}

/* ---- streams ---- */
static void ap__stop_streams(ap_conn* c, int mirror, int audio) {
    if (mirror && c->mirror_run) {
        c->mirror_run = 0;
        if (c->mirror_listen != AP_BADSOCK) ap__shutdown_sock(c->mirror_listen);
        ap__thread_join(c->th_mirror);
        if (c->mirror_listen != AP_BADSOCK) { AP_CLOSESOCK(c->mirror_listen); c->mirror_listen = AP_BADSOCK; }
    }
    if (audio && c->audio_run) {
        c->audio_run = 0;
        ap__thread_join(c->th_audio);
        if (c->audio_data != AP_BADSOCK) { AP_CLOSESOCK(c->audio_data); c->audio_data = AP_BADSOCK; }
        if (c->audio_ctrl != AP_BADSOCK) { AP_CLOSESOCK(c->audio_ctrl); c->audio_ctrl = AP_BADSOCK; }
    }
}

typedef struct { ap_conn* c; ap_sock data; ap_aes_ctr ctr; uint8_t* sps_pps; size_t sps_pps_n; uint64_t sps_ts; int want_prepend; } ap__mirror;

/* ---- sender-clock helpers ---- */
uint64_t airplay_ntp_to_ns(uint64_t ntp) {
    uint64_t sec = ntp >> 32, frac = ntp & 0xffffffffu;
    return sec * 1000000000ULL + ((frac * 1000000000ULL) >> 32);
}
static uint64_t ap__rtp_to_ns(const ap_conn* c, uint32_t rtp) {
    if (!c->have_sync) return 0;
    int32_t d = (int32_t)(rtp - c->sync_rtp);                          /* wraps correctly across 2^32 */
    int64_t ns = (int64_t)c->sync_ns + (int64_t)d * 1000000000LL / 44100;
    return ns < 0 ? 0 : (uint64_t)ns;
}

/* ---- A/V scheduler: hold events until sender_time + offset + latency ---- */
enum { AP_EV_VIDEO, AP_EV_PCM, AP_EV_FRAME };
typedef struct ap__ev {
    struct ap__ev* next;
    uint64_t due, ts_ns; int kind, idr, codec, channels, rate; uint32_t rtp;
    uint8_t* data; size_t n;
} ap__ev;
static void ap__ev_free(ap__ev* e) { if (e) { free(e->data); free(e); } }
static void ap__deliver(airplay_t* a, ap__ev* e) {
    switch (e->kind) {
    case AP_EV_VIDEO: if (a->cb.on_video) a->cb.on_video(a->cb.user, e->data, e->n, e->ts_ns, e->idr); break;
    case AP_EV_PCM: if (a->cb.on_pcm) a->cb.on_pcm(a->cb.user, (const int16_t*)e->data, e->n, e->channels, e->rate, e->ts_ns); break;
    default:
        if (a->cb.on_audio_ex) a->cb.on_audio_ex(a->cb.user, e->data, e->n, e->codec, e->rtp, e->ts_ns);
        else if (a->cb.on_audio) a->cb.on_audio(a->cb.user, e->data, e->n, e->codec, e->rtp);
    }
}
/* One shared offset estimate for both streams: the minimum of (arrival - sender time), creeping
 * upward slowly so clock drift is followed. */
static void ap__offset_sample(airplay_t* a, int64_t sample) {
    if (!a->have_offset || sample < a->offset_ns) { a->offset_ns = sample; a->have_offset = 1; }
    else a->offset_ns += (sample - a->offset_ns) >> 9;
}
static void ap__emit(airplay_t* a, int kind, const void* data, size_t n, uint64_t ts_ns, int idr, int codec, int channels, int rate, uint32_t rtp) {
    ap__ev* e = (ap__ev*)calloc(1, sizeof(*e));
    if (!e) return;
    e->kind = kind; e->ts_ns = ts_ns; e->idr = idr; e->codec = codec; e->channels = channels; e->rate = rate; e->rtp = rtp; e->n = n;
    size_t bytes = kind == AP_EV_PCM ? n * (size_t)channels * 2 : n;
    e->data = (uint8_t*)malloc(bytes ? bytes : 1);
    if (!e->data) { free(e); return; }
    if (bytes) memcpy(e->data, data, bytes);
    if (a->cfg.latency_ms <= 0 || !a->sched_run) { ap__deliver(a, e); ap__ev_free(e); return; }
    uint64_t now = ap__now_ns();
    ap__mutex_lock(&a->q_lock);
    if (ts_ns) ap__offset_sample(a, (int64_t)now - (int64_t)ts_ns);
    int64_t base = ts_ns && a->have_offset ? (int64_t)ts_ns + a->offset_ns : (int64_t)now;       /* no timestamp yet: play on arrival */
    base += (int64_t)a->cfg.latency_ms * 1000000LL;
    if (kind == AP_EV_VIDEO) base += (int64_t)a->cfg.av_offset_ms * 1000000LL;
    e->due = (uint64_t)(base < 0 ? 0 : base);
    int drop = a->q_n >= 1024 || (kind != AP_EV_VIDEO && e->due + 300000000ULL < now);              /* audio that is already 300 ms late is useless */
    if (!drop) {
        ap__ev** pp = &a->q_head;
        while (*pp && (*pp)->due <= e->due) pp = &(*pp)->next;                                      /* sorted by due time: reorders for free */
        e->next = *pp; *pp = e; a->q_n++;
    }
    ap__mutex_unlock(&a->q_lock);
    if (drop) ap__ev_free(e);
}
static AP_THREAD_RET ap__sched_thread(void* arg) {
    airplay_t* a = (airplay_t*)arg;
    while (a->sched_run) {
        ap__ev* e = NULL;
        uint64_t now = ap__now_ns(), wait = 5000000ULL;
        ap__mutex_lock(&a->q_lock);
        if (a->q_head) {
            if (a->q_head->due <= now) { e = a->q_head; a->q_head = e->next; a->q_n--; }
            else if (a->q_head->due - now < wait) wait = a->q_head->due - now;
        }
        ap__mutex_unlock(&a->q_lock);
        if (e) { ap__deliver(a, e); ap__ev_free(e); continue; }
        ap__sleep_ns(wait > 500000ULL ? wait - 200000ULL : 300000ULL);
    }
    ap__mutex_lock(&a->q_lock);
    while (a->q_head) { ap__ev* e = a->q_head; a->q_head = e->next; ap__ev_free(e); }
    a->q_n = 0;
    ap__mutex_unlock(&a->q_lock);
    return AP_THREAD_RV;
}

size_t airplay_loas_wrap(const uint8_t* asc, int asc_bits, const uint8_t* frame, size_t n, uint8_t* out, size_t cap) {
    uint8_t body[1500];
    size_t bits = 0;
    if (n > 1200 || asc_bits > 64) return 0;
    memset(body, 0, sizeof(body));
#define AP_PUT(v, nb) do { for (int b_ = (nb) - 1; b_ >= 0; b_--) { if (((v) >> b_) & 1) body[bits >> 3] |= (uint8_t)(0x80 >> (bits & 7)); bits++; } } while (0)
    AP_PUT(0u, 1);                       /* useSameStreamMux = 0: the config is in-band */
    AP_PUT(0u, 1);                       /* audioMuxVersion */
    AP_PUT(1u, 1);                       /* allStreamsSameTimeFraming */
    AP_PUT(0u, 6); AP_PUT(0u, 4); AP_PUT(0u, 3);        /* numSubFrames, numProgram, numLayer */
    for (int i = 0; i < asc_bits; i++) { unsigned v = (asc[i >> 3] >> (7 - (i & 7))) & 1; AP_PUT(v, 1); }
    AP_PUT(0u, 3);                       /* frameLengthType */
    AP_PUT(0xFFu, 8);                    /* latmBufferFullness */
    AP_PUT(0u, 1); AP_PUT(0u, 1);        /* otherDataPresent, crcCheckPresent */
    size_t left = n;
    while (left >= 255) { AP_PUT(0xFFu, 8); left -= 255; }
    AP_PUT((unsigned)left, 8);           /* PayloadLengthInfo */
    for (size_t i = 0; i < n; i++) AP_PUT((unsigned)frame[i], 8);
#undef AP_PUT
    size_t len = (bits + 7) / 8;
    if (len > 8191 || len + 3 > cap) return 0;
    out[0] = 0x56; out[1] = (uint8_t)(0xE0 | (len >> 8)); out[2] = (uint8_t)len;     /* 0x2B7 sync + 13-bit length */
    memcpy(out + 3, body, len);
    return len + 3;
}


static size_t ap__avcc_to_annexb(uint8_t* p, size_t n) {
    size_t i = 0;
    while (i + 4 <= n) {
        uint32_t l = ap__get32be(p + i);
        p[i] = 0; p[i + 1] = 0; p[i + 2] = 0; p[i + 3] = 1;
        if (l > n - i - 4) return 0;                          /* corrupt: caller drops the frame */
        i += 4 + (size_t)l;
    }
    return i == n ? n : 0;
}
static AP_THREAD_RET ap__mirror_thread(void* arg) {
    ap_conn* c = (ap_conn*)arg;
    airplay_t* a = c->a;
    ap_sock s = AP_BADSOCK;
    /* accept the sender's data connection */
    ap__set_timeout(c->mirror_listen, 500);
    while (c->mirror_run && s == AP_BADSOCK) {
        fd_set fds; struct timeval tv = { 0, 300000 };
        FD_ZERO(&fds); FD_SET(c->mirror_listen, &fds);
        if (select((int)c->mirror_listen + 1, &fds, NULL, NULL, &tv) > 0) s = accept(c->mirror_listen, NULL, NULL);
    }
    if (s == AP_BADSOCK) return AP_THREAD_RV;
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    ap__set_timeout(s, 1000);
    /* AES-CTR key/iv: SHA-512("AirPlayStreamKey<id>" || aeskey)[:16], same for IV */
    uint8_t buf[64], h[64], key[16], iv[16];
    int l = snprintf((char*)buf, sizeof(buf), "AirPlayStreamKey%llu", (unsigned long long)c->stream_id);
    memcpy(buf + l, c->aeskey, 16); ap_sha512(h, buf, (size_t)l + 16); memcpy(key, h, 16);
    l = snprintf((char*)buf, sizeof(buf), "AirPlayStreamIV%llu", (unsigned long long)c->stream_id);
    memcpy(buf + l, c->aeskey, 16); ap_sha512(h, buf, (size_t)l + 16); memcpy(iv, h, 16);
    ap_aes_ctr ctr;
    ap_aes_ctr_init(&ctr, key, iv);
    uint8_t* sps_pps = NULL; size_t sps_pps_n = 0; uint64_t sps_ts = 0; int have_sps = 0;
    uint8_t hdr[128];
    while (c->mirror_run) {
        int r = ap__recv_all(s, hdr, 128);
        if (r == -2) continue;
        if (r != 0) break;
        uint32_t size = ap__get32le(hdr);
        uint8_t type = hdr[4];
        uint64_t ts = ap__get64le(hdr + 8);
        if (size > (16u << 20)) break;
        uint8_t* pay = (uint8_t*)malloc(size ? size : 1);
        if (!pay) break;
        size_t got = 0; int bad = 0;
        while (got < size && c->mirror_run) {
            int rr = (int)recv(s, (char*)pay + got, (int)(size - got), 0);
            if (rr > 0) { got += (size_t)rr; continue; }
#ifdef _WIN32
            int e = WSAGetLastError(); if (rr < 0 && (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)) continue;
#else
            if (rr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
#endif
            bad = 1; break;
        }
        if (bad || got < size) { free(pay); break; }
        if (type == 0) {                                      /* encrypted frame: AVCC length-prefixed NALs */
            ap_aes_ctr_xor(&ctr, pay, pay, size);
            if (size && ap__avcc_to_annexb(pay, size)) {
                int idr = 0;
                for (size_t i = 0; i + 4 < size; ) { int nt = pay[i + 4] & 31; if (nt == 5) { idr = 1; break; } if (nt == 1) break; uint32_t nl = 0; size_t j = i; (void)nl; j += 4; while (j + 4 <= size && !(pay[j] == 0 && pay[j + 1] == 0 && pay[j + 2] == 0 && pay[j + 3] == 1)) j++; i = j; }
                if (have_sps && sps_ts == ts) {
                    uint8_t* out = (uint8_t*)malloc(sps_pps_n + size);
                    if (out) { memcpy(out, sps_pps, sps_pps_n); memcpy(out + sps_pps_n, pay, size);
                               ap__emit(a, AP_EV_VIDEO, out, sps_pps_n + size, airplay_ntp_to_ns(ts), idr, 0, 0, 0, 0);
                               free(out); }
                    have_sps = 0;
                } else ap__emit(a, AP_EV_VIDEO, pay, size, airplay_ntp_to_ns(ts), idr, 0, 0, 0, 0);
            }
        } else if (type == 1 && size >= 11) {                 /* avcC: SPS + PPS, plain */
            uint32_t sps_n = ap__get16be(pay + 6);
            if (8u + sps_n + 3 <= size) {
                uint32_t pps_n = ap__get16be(pay + sps_n + 9);
                if (sps_n + 11u + pps_n <= size) {
                    free(sps_pps);
                    sps_pps_n = 4 + sps_n + 4 + pps_n;
                    sps_pps = (uint8_t*)malloc(sps_pps_n);
                    if (sps_pps) {
                        static const uint8_t sc[4] = { 0, 0, 0, 1 };
                        memcpy(sps_pps, sc, 4); memcpy(sps_pps + 4, pay + 8, sps_n);
                        memcpy(sps_pps + 4 + sps_n, sc, 4); memcpy(sps_pps + 8 + sps_n, pay + sps_n + 11, pps_n);
                        sps_ts = ts; have_sps = 1;
                        float w, hh;
                        memcpy(&w, hdr + 40, 4); memcpy(&hh, hdr + 44, 4);
                        if (a->cb.on_video_config) a->cb.on_video_config(a->cb.user, 0, (int)w, (int)hh);
                        /* the parameter sets alone are useful to a decoder that wants to be primed early */
                        ap__emit(a, AP_EV_VIDEO, sps_pps, sps_pps_n, airplay_ntp_to_ns(ts), 0, 0, 0, 0, 0);
                        have_sps = 0;
                    }
                }
            }
        }
        /* type 2 (heartbeat) and 5 (stats plist) need no action */
        free(pay);
    }
    free(sps_pps);
    AP_CLOSESOCK(s);
    return AP_THREAD_RV;
}

static AP_THREAD_RET ap__audio_thread(void* arg) {
    ap_conn* c = (ap_conn*)arg;
    airplay_t* a = c->a;
    ap_aes128 aes;
    ap_aes128_init(&aes, c->aeskey);
    uint8_t pkt[2048], out[2048];
    int16_t pcm[16384 * 2];
    for (int i = 0; i < 256; i++) c->seen[i] = 0xFFFFFFFFu;
    static const uint8_t cookie[36] = { 0,0,0,0x24, 'a','l','a','c', 0,0,0,0, 0,0,0x01,0x60, 0,0x10,0x28,0x0a, 0x0e,0x02,0x00,0xff, 0,0,0,0, 0,0,0,0, 0,0,0xac,0x44 };
    c->alac_ok = c->audio_ct == 2 && a->cb.on_pcm && ap_alac_init(&c->alac, cookie, sizeof(cookie)) == 0;
    while (c->audio_run) {
        fd_set fds; struct timeval tv = { 0, 200000 };
        FD_ZERO(&fds); FD_SET(c->audio_data, &fds);
        int mx = (int)c->audio_data;
        if (c->audio_ctrl != AP_BADSOCK) { FD_SET(c->audio_ctrl, &fds); if ((int)c->audio_ctrl > mx) mx = (int)c->audio_ctrl; }
        if (select(mx + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        if (c->audio_ctrl != AP_BADSOCK && FD_ISSET(c->audio_ctrl, &fds)) {
            int n = (int)recv(c->audio_ctrl, (char*)pkt, sizeof(pkt), 0);
            if (n >= 20 && (pkt[1] & 0x7f) == 0x54) {                  /* sync: rtp <-> sender clock */
                uint64_t ntp = ((uint64_t)ap__get32be(pkt + 8) << 32) | ap__get32be(pkt + 12);
                c->sync_rtp = ap__get32be(pkt + 4); c->sync_ns = airplay_ntp_to_ns(ntp); c->have_sync = 1;
            }
        }
        if (!FD_ISSET(c->audio_data, &fds)) continue;
        int n = (int)recv(c->audio_data, (char*)pkt, sizeof(pkt), 0);
        if (n <= 12) continue;                                          /* header only = "no data" */
        uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
        uint32_t rtp_ts = ap__get32be(pkt + 4);
        size_t len = (size_t)n - 12;
        if (len == 4 && !memcmp(pkt + 12, "\x00\x68\x34\x00", 4)) continue;     /* AAC-ELD start-of-stream marker */
        if (c->audio_ct == 2 && len == 32) continue;                              /* ALAC format-info packet */
        if (c->seen[seq & 255] == (uint32_t)seq + 1u) continue;                   /* AAC-ELD sends every frame three times */
        c->seen[seq & 255] = (uint32_t)seq + 1u;
        ap_aes128_cbc_decrypt(&aes, c->aesiv, pkt + 12, out, len);
        uint64_t ts = ap__rtp_to_ns(c, rtp_ts);
        if (c->alac_ok) {
            int fr = ap_alac_decode(&c->alac, out, len, pcm, c->alac.frame_len);
            if (fr > 0) ap__emit(a, AP_EV_PCM, pcm, (size_t)fr, ts, 0, 2, 2, 44100, rtp_ts);
        } else if (a->cb.on_audio_ex || a->cb.on_audio) ap__emit(a, AP_EV_FRAME, out, len, ts, 0, c->audio_ct, 0, 0, rtp_ts);
    }
    if (c->alac_ok) { ap_alac_free(&c->alac); c->alac_ok = 0; }
    return AP_THREAD_RV;
}

static void ap__h_setup(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    airplay_t* a = c->a;
    ap_pl* req = rq->body_n ? ap_pl_parse(rq->body, rq->body_n) : NULL;
    if (!req) { rs->code = 400; rs->status = "Bad Request"; return; }
    ap_pl* res = ap_pl_new(AP_PL_DICT);
    ap_pl* ekey = ap_pl_get(req, "ekey");
    ap_pl* eiv = ap_pl_get(req, "eiv");
    if (ekey && eiv && ekey->t == AP_PL_DATA && eiv->t == AP_PL_DATA && ekey->n >= 72 && eiv->n >= 16) {
        /* first SETUP: session keys */
        memcpy(c->aesiv, eiv->d, 16);
        if (!a->fp.decrypt || !c->fp || a->fp.decrypt(c->fp, ekey->d, c->aeskey) != 0) {
            ap__log(a, 1, "SETUP: FairPlay key decrypt unavailable or failed (%s)", c->peer);
            ap_pl_free(req); ap_pl_free(res);
            rs->code = 500; rs->status = "FairPlay failed"; rs->close_after = 1;
            return;
        }
        if (c->have_secret) {                                 /* legacy pairing: key = SHA-512(aeskey || ecdh_secret)[:16] */
            uint8_t b[48], h[64];
            memcpy(b, c->aeskey, 16); memcpy(b + 16, c->ecdh_secret, 32);
            ap_sha512(h, b, 48); memcpy(c->aeskey, h, 16);
        }
        c->have_keys = 1;
        ap_pl* nm = ap_pl_get(req, "name");
        ap_pl* dev = ap_pl_get(req, "deviceID");
        if (!c->announced && a->cb.on_client) {
            c->announced = 1;
            a->cb.on_client(a->cb.user, 1, nm && nm->t == AP_PL_STR ? (const char*)nm->d : (dev && dev->t == AP_PL_STR ? (const char*)dev->d : "AirPlay"), c->peer);
        }
        ap_pl_set(res, "timingPort", ap_pl_int(0));
        ap_pl_set(res, "eventPort", ap_pl_int(0));
    }
    ap_pl* streams = ap_pl_get(req, "streams");
    if (streams && streams->t == AP_PL_ARRAY) {
        ap_pl* out = ap_pl_new(AP_PL_ARRAY);
        for (size_t i = 0; i < streams->cnt; i++) {
            ap_pl* st = streams->v[i];
            ap_pl* ty = ap_pl_get(st, "type");
            if (!ty || ty->t != AP_PL_INT || !c->have_keys) continue;
            if (ty->i == 110 && a->cfg.enable_mirroring) {
                ap_pl* id = ap_pl_get(st, "streamConnectionID");
                c->stream_id = id ? (uint64_t)id->i : 0;
                ap__stop_streams(c, 1, 0);
                c->mirror_listen = ap__tcp_listen(0, &c->mirror_port);
                if (c->mirror_listen == AP_BADSOCK) continue;
                c->mirror_run = 1;
                if (ap__thread_start(&c->th_mirror, ap__mirror_thread, c) != 0) { c->mirror_run = 0; continue; }
                ap_pl* e = ap_pl_new(AP_PL_DICT);
                ap_pl_set(e, "dataPort", ap_pl_int(c->mirror_port)); ap_pl_set(e, "type", ap_pl_int(110));
                ap_pl_push(out, e);
            } else if (ty->i == 96 && a->cfg.enable_audio) {
                ap_pl* ct = ap_pl_get(st, "ct");
                c->audio_ct = ct ? (int)ct->i : 2;
                ap__stop_streams(c, 0, 1);
                c->audio_data = ap__udp_bind(0, &c->audio_port);
                c->audio_ctrl = ap__udp_bind(0, &c->audio_ctrl_port);
                if (c->audio_data == AP_BADSOCK) continue;
                c->audio_run = 1;
                if (ap__thread_start(&c->th_audio, ap__audio_thread, c) != 0) { c->audio_run = 0; continue; }
                if (a->cb.on_audio_start) a->cb.on_audio_start(a->cb.user, c->audio_ct, 44100);
                ap_pl* e = ap_pl_new(AP_PL_DICT);
                ap_pl_set(e, "dataPort", ap_pl_int(c->audio_port)); ap_pl_set(e, "controlPort", ap_pl_int(c->audio_ctrl_port)); ap_pl_set(e, "type", ap_pl_int(96));
                ap_pl_push(out, e);
            }
        }
        ap_pl_set(res, "streams", out);
    }
    ap_pl_free(req);
    ap__resp_plist(rs, res);
}

static void ap__h_set_parameter(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    (void)rs;
    const char* ct = ap__hdr(rq, "Content-Type");
    if (ct && strstr(ct, "text/parameters") && rq->body_n) {
        char t[256];
        size_t n = rq->body_n < sizeof(t) - 1 ? rq->body_n : sizeof(t) - 1;
        memcpy(t, rq->body, n); t[n] = 0;
        if (!strncmp(t, "volume:", 7) && c->a->cb.on_volume) c->a->cb.on_volume(c->a->cb.user, (float)atof(t + 7));
    }
}

static void ap__h_teardown(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    ap_pl* req = rq->body_n ? ap_pl_parse(rq->body, rq->body_n) : NULL;
    ap_pl* streams = req ? ap_pl_get(req, "streams") : NULL;
    int mirror = 0, audio = 0;
    if (streams && streams->t == AP_PL_ARRAY) {
        for (size_t i = 0; i < streams->cnt; i++) {
            ap_pl* ty = ap_pl_get(streams->v[i], "type");
            if (ty && ty->i == 110) mirror = 1; else if (ty && ty->i == 96) audio = 1;
        }
    } else { mirror = audio = 1; rs->close_after = 1; }
    ap_pl_free(req);
    ap__stop_streams(c, mirror, audio);
}

static void ap__dispatch(ap_conn* c, const ap__req* rq, ap__resp* rs) {
    rs->code = 200; rs->status = "OK";
    const char* m = rq->method;
    const char* u = rq->url;
    if (!strcmp(m, "GET") && !strncmp(u, "/info", 5)) ap__h_info(c, rq, rs);
    else if (!strcmp(m, "POST") && !strcmp(u, "/pair-setup")) ap__h_pair_setup(c, rq, rs);
    else if (!strcmp(m, "POST") && !strcmp(u, "/pair-verify")) ap__h_pair_verify(c, rq, rs);
    else if (!strcmp(m, "POST") && !strcmp(u, "/fp-setup")) ap__h_fp_setup(c, rq, rs);
    else if (!strcmp(m, "OPTIONS")) ap__resp_header(rs, "Public: SETUP, RECORD, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
    else if (!strcmp(m, "SETUP")) ap__h_setup(c, rq, rs);
    else if (!strcmp(m, "RECORD")) { ap__resp_header(rs, "Audio-Latency: 11025"); ap__resp_header(rs, "Audio-Jack-Status: connected; type=analog"); }
    else if (!strcmp(m, "SET_PARAMETER")) ap__h_set_parameter(c, rq, rs);
    else if (!strcmp(m, "TEARDOWN")) ap__h_teardown(c, rq, rs);
    else if (!strcmp(m, "FLUSH") || !strcmp(m, "GET_PARAMETER") || !strcmp(m, "POST") || !strcmp(m, "GET")) { /* acknowledged, nothing to do */ }
    else { rs->code = 501; rs->status = "Not Implemented"; }
}

static AP_THREAD_RET ap__conn_thread(void* arg) {
    ap_conn* c = (ap_conn*)arg;
    airplay_t* a = c->a;
    ap__set_timeout(c->s, 1000);
    while (a->running && c->alive) {
        ap__req rq;
        int r = ap__read_request(c, &rq);
        if (r == -2) continue;
        if (r != 0) break;
        ap__resp rs;
        memset(&rs, 0, sizeof(rs));
        ap__dispatch(c, &rq, &rs);
        int sent = ap__send_response(c, &rq, &rs);
        int close_after = rs.close_after;
        free(rs.body);
        ap__req_free(&rq);
        if (sent != 0 || close_after) break;
    }
    ap__stop_streams(c, 1, 1);
    if (c->announced && a->cb.on_client) a->cb.on_client(a->cb.user, 0, "", c->peer);
    if (c->fp && a->fp.destroy) a->fp.destroy(c->fp);
    c->fp = NULL;
    AP_CLOSESOCK(c->s);
    c->s = AP_BADSOCK;
    c->alive = 0;
    return AP_THREAD_RV;
}

static AP_THREAD_RET ap__accept_thread(void* arg) {
    airplay_t* a = (airplay_t*)arg;
    while (a->running) {
        fd_set fds; struct timeval tv = { 0, 250000 };
        FD_ZERO(&fds); FD_SET(a->listen_sock, &fds);
        if (select((int)a->listen_sock + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ap_sock s = accept(a->listen_sock, (struct sockaddr*)&from, &fl);
        if (s == AP_BADSOCK) continue;
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        ap__mutex_lock(&a->lock);
        int slot = -1;
        for (int i = 0; i < AP_MAX_CONN; i++) {                 /* reap finished connections, find a free slot */
            if (a->conns[i] && !a->conns[i]->alive) { ap__thread_join(a->conns[i]->th); free(a->conns[i]); a->conns[i] = NULL; }
            if (!a->conns[i] && slot < 0) slot = i;
        }
        if (slot < 0) { AP_CLOSESOCK(s); ap__mutex_unlock(&a->lock); continue; }
        ap_conn* c = (ap_conn*)calloc(1, sizeof(*c));
        if (!c) { AP_CLOSESOCK(s); ap__mutex_unlock(&a->lock); continue; }
        c->a = a; c->s = s; c->alive = 1;
        c->mirror_listen = c->audio_data = c->audio_ctrl = AP_BADSOCK;
        inet_ntop(AF_INET, &from.sin_addr, c->peer, sizeof(c->peer));
        a->conns[slot] = c;
        if (ap__thread_start(&c->th, ap__conn_thread, c) != 0) { AP_CLOSESOCK(s); free(c); a->conns[slot] = NULL; }
        ap__mutex_unlock(&a->lock);
    }
    return AP_THREAD_RV;
}

/* ---- mDNS thread ---- */
static AP_THREAD_RET ap__mdns_thread(void* arg) {
    airplay_t* a = (airplay_t*)arg;
    struct sockaddr_in grp;
    memset(&grp, 0, sizeof(grp));
    grp.sin_family = AF_INET; grp.sin_port = htons(5353); grp.sin_addr.s_addr = htonl(0xE00000FBu);
    uint8_t pkt[4096], q[4096];
    uint64_t next_announce = 0; int announces = 0;
    while (a->running) {
        uint64_t now = ap__now_ns();
        if (announces < 3 && now >= next_announce) {           /* unsolicited announcements: now, +1s, +2s */
            for (int i = 0; i < a->mdns.nsvc; i++) {
                size_t n = ap__mdns_build(pkt, &a->mdns.svc[i], a->mdns.host, a->mdns.ip, 4500);
                sendto(a->mdns_sock, (const char*)pkt, (int)n, 0, (struct sockaddr*)&grp, sizeof(grp));
            }
            announces++; next_announce = now + 1000000000ULL;
        }
        fd_set fds; struct timeval tv = { 0, 200000 };
        FD_ZERO(&fds); FD_SET(a->mdns_sock, &fds);
        if (select((int)a->mdns_sock + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int n = (int)recv(a->mdns_sock, (char*)q, sizeof(q), 0);
        if (n < 12 || (q[2] & 0x80)) continue;                  /* only queries */
        size_t pos = 12;
        int qd = (q[4] << 8) | q[5];
        int hit[2] = { 0, 0 };
        for (int i = 0; i < qd && i < 16; i++) {
            char name[300];
            if (ap__dns_qname(q, (size_t)n, &pos, name, sizeof(name))) break;
            if (pos + 4 > (size_t)n) break;
            pos += 4;
            for (int k = 0; k < a->mdns.nsvc; k++) {
                char tdom[80], idom[320], hdom[160];
                snprintf(tdom, sizeof(tdom), "%s.local", a->mdns.svc[k].type);
                snprintf(idom, sizeof(idom), "%s.%s", a->mdns.svc[k].instance, tdom);
                snprintf(hdom, sizeof(hdom), "%s.local", a->mdns.host);
                if (!strcasecmp(name, tdom) || !strcasecmp(name, idom) || !strcasecmp(name, hdom) || !strcasecmp(name, "_services._dns-sd._udp.local")) hit[k] = 1;
            }
        }
        for (int k = 0; k < a->mdns.nsvc; k++) if (hit[k]) {
            size_t m = ap__mdns_build(pkt, &a->mdns.svc[k], a->mdns.host, a->mdns.ip, 4500);
            sendto(a->mdns_sock, (const char*)pkt, (int)m, 0, (struct sockaddr*)&grp, sizeof(grp));
        }
    }
    for (int i = 0; i < a->mdns.nsvc; i++) {                    /* goodbye */
        size_t n = ap__mdns_build(pkt, &a->mdns.svc[i], a->mdns.host, a->mdns.ip, 0);
        sendto(a->mdns_sock, (const char*)pkt, (int)n, 0, (struct sockaddr*)&grp, sizeof(grp));
    }
    return AP_THREAD_RV;
}

static ap_sock ap__mdns_socket(const uint8_t ip[4]) {
    ap_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == AP_BADSOCK) return AP_BADSOCK;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(5353); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { AP_CLOSESOCK(s); return AP_BADSOCK; }
    struct ip_mreq mr;
    mr.imr_multiaddr.s_addr = htonl(0xE00000FBu);
    memcpy(&mr.imr_interface.s_addr, ip, 4);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr));
    unsigned char ttl = 255, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)ip, 4);
    return s;
}

/* ---- public API ---- */
static void ap__hex(char* out, const uint8_t* in, size_t n) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", in[i]); }

airplay_t* airplay_create(const airplay_config* cfg, const airplay_callbacks* cb) {
    airplay_t* a = (airplay_t*)calloc(1, sizeof(*a));
    if (!a) return NULL;
    if (cfg) a->cfg = *cfg;
    if (cb) a->cb = *cb;
    snprintf(a->name, sizeof(a->name), "%s", cfg && cfg->name && cfg->name[0] ? cfg->name : "Luna AirPlay");
    if (!a->cfg.enable_mirroring && !a->cfg.enable_audio) a->cfg.enable_mirroring = a->cfg.enable_audio = 1;
    /* identity */
    if (cfg && cfg->ed_seed) memcpy(a->ed_seed, cfg->ed_seed, 32); else ap__random(a->ed_seed, 32);
    ap_ed25519_pubkey(a->ed_pk, a->ed_seed);
    ap__hex(a->pk_hex, a->ed_pk, 32);
    uint8_t mac[6];
    if (cfg && cfg->device_id && strlen(cfg->device_id) == 17) snprintf(a->device_id, sizeof(a->device_id), "%s", cfg->device_id);
    else {
        uint8_t h[64]; ap_sha512(h, a->ed_pk, 32);          /* stable per identity */
        memcpy(mac, h, 6); mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);
        snprintf(a->device_id, sizeof(a->device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    /* FairPlay: caller-provided, else the bundled GPL playfair when opted in */
    if (cfg && cfg->fairplay) a->fp = *cfg->fairplay;
#ifdef AIRPLAY_PLAYFAIR
    else {
        a->fp.create = ap__pf_create; a->fp.setup = ap__pf_setup; a->fp.handshake = ap__pf_handshake;
        a->fp.decrypt = ap__pf_decrypt; a->fp.destroy = ap__pf_destroy;
    }
#endif
    ap__mutex_init(&a->lock); ap__mutex_init(&a->q_lock);
    a->listen_sock = a->mdns_sock = AP_BADSOCK;
    return a;
}

int airplay_start(airplay_t* a) {
    if (!a || a->started) return -1;
    ap__net_init();
    a->listen_sock = ap__tcp_listen(a->cfg.port ? a->cfg.port : 7000, &a->port);
    if (a->listen_sock == AP_BADSOCK) a->listen_sock = ap__tcp_listen(0, &a->port);
    if (a->listen_sock == AP_BADSOCK) return -2;
    ap__local_ipv4(a->mdns.ip);
    /* host + service records */
    char macflat[13]; int j = 0;
    for (const char* p = a->device_id; *p; p++) if (*p != ':') macflat[j++] = *p;
    macflat[j] = 0;
    snprintf(a->mdns.host, sizeof(a->mdns.host), "luna-airplay-%s", macflat);
    ap__svc* air = &a->mdns.svc[0]; ap__svc* raop = &a->mdns.svc[1];
    memset(air, 0, sizeof(*air)); memset(raop, 0, sizeof(*raop));
    snprintf(air->instance, sizeof(air->instance), "%s", a->name);
    snprintf(air->type, sizeof(air->type), "_airplay._tcp");
    air->port = a->port;
    ap__txt_add(air, "deviceid", a->device_id); ap__txt_add(air, "features", "0x5A7FFEE6,0x0");
    ap__txt_add(air, "flags", "0x4"); ap__txt_add(air, "model", "AppleTV3,2"); ap__txt_add(air, "pk", a->pk_hex);
    ap__txt_add(air, "pi", "2e388006-13ba-4041-9a67-25dd4a43d536"); ap__txt_add(air, "srcvers", "220.68"); ap__txt_add(air, "vv", "2"); ap__txt_add(air, "pw", "false");
    snprintf(raop->instance, sizeof(raop->instance), "%s@%s", macflat, a->name);
    snprintf(raop->type, sizeof(raop->type), "_raop._tcp");
    raop->port = a->port;
    ap__txt_add(raop, "txtvers", "1"); ap__txt_add(raop, "ch", "2"); ap__txt_add(raop, "cn", "0,1,2,3"); ap__txt_add(raop, "da", "true");
    ap__txt_add(raop, "et", "0,3,5"); ap__txt_add(raop, "vv", "2"); ap__txt_add(raop, "ft", "0x5A7FFEE6,0x0"); ap__txt_add(raop, "am", "AppleTV3,2");
    ap__txt_add(raop, "md", "0,1,2"); ap__txt_add(raop, "rhd", "5.6.0.0"); ap__txt_add(raop, "pw", "false"); ap__txt_add(raop, "sf", "0x4");
    ap__txt_add(raop, "sr", "44100"); ap__txt_add(raop, "ss", "16"); ap__txt_add(raop, "sv", "false"); ap__txt_add(raop, "tp", "UDP");
    ap__txt_add(raop, "vs", "220.68"); ap__txt_add(raop, "vn", "65537"); ap__txt_add(raop, "pk", a->pk_hex);
    a->mdns.nsvc = 2;
    a->mdns_sock = ap__mdns_socket(a->mdns.ip);
    a->running = 1;
    if (a->cfg.latency_ms > 0) { a->sched_run = 1; if (ap__thread_start(&a->th_sched, ap__sched_thread, a) != 0) a->sched_run = 0; }
    if (ap__thread_start(&a->th_accept, ap__accept_thread, a) != 0) { a->running = 0; return -3; }
    if (a->mdns_sock != AP_BADSOCK) ap__thread_start(&a->th_mdns, ap__mdns_thread, a);
    else ap__log(a, 1, "mDNS socket unavailable: receiver will not be discoverable");
    a->started = 1;
    ap__log(a, 0, "AirPlay receiver \"%s\" listening on %u.%u.%u.%u:%d", a->name, a->mdns.ip[0], a->mdns.ip[1], a->mdns.ip[2], a->mdns.ip[3], a->port);
    return 0;
}

void airplay_stop(airplay_t* a) {
    if (!a || !a->started) return;
    a->running = 0;
    if (a->sched_run) { a->sched_run = 0; ap__thread_join(a->th_sched); }
    ap__thread_join(a->th_accept);
    if (a->mdns_sock != AP_BADSOCK) { ap__thread_join(a->th_mdns); AP_CLOSESOCK(a->mdns_sock); a->mdns_sock = AP_BADSOCK; }
    ap__mutex_lock(&a->lock);
    for (int i = 0; i < AP_MAX_CONN; i++) if (a->conns[i]) { ap__shutdown_sock(a->conns[i]->s); }
    ap__mutex_unlock(&a->lock);
    for (int i = 0; i < AP_MAX_CONN; i++) if (a->conns[i]) { ap__thread_join(a->conns[i]->th); free(a->conns[i]); a->conns[i] = NULL; }
    AP_CLOSESOCK(a->listen_sock); a->listen_sock = AP_BADSOCK;
    a->started = 0;
}
void airplay_destroy(airplay_t* a) { if (!a) return; airplay_stop(a); ap__mutex_destroy(&a->lock); ap__mutex_destroy(&a->q_lock); free(a); }
int airplay_port(const airplay_t* a) { return a ? a->port : 0; }
const char* airplay_device_id(const airplay_t* a) { return a ? a->device_id : ""; }
const uint8_t* airplay_public_key(const airplay_t* a) { return a ? a->ed_pk : NULL; }

#endif /* AIRPLAY_IMPLEMENTATION_ONCE */
#endif /* AIRPLAY_IMPLEMENTATION */
