/*
 * airplay_fairplayd.c — FairPlay as a separate program
 *
 * THIS PROGRAM IS GPL-3.0-ONLY (it contains UxPlay's "playfair", see airplay_playfair.h).
 * airplay.h talks to it over a pipe (airplay_fairplay_spawn()), as one independent program to
 * another: no code is linked or loaded into the program that uses airplay.h, so that program's
 * licence is not affected.  (Not legal advice; the arm's-length process boundary is the point.)
 *
 *   cc -O2 -o airplay-fairplayd airplay_fairplayd.c -lm       (Windows: cl /O2 airplay_fairplayd.c)
 *
 * Protocol on stdin/stdout, little endian:
 *   request  [u8 cmd][u32 id][u16 len][len bytes]      reply  [u8 status 0 = ok][u16 len][len bytes]
 *   cmd 0 create   -> reply u32 id         cmd 1 setup     16 -> 142     cmd 2 handshake 164 -> 32
 *   cmd 3 decrypt  72 -> 16                cmd 4 destroy
 * FairPlay is Apple's DRM; check the law that applies to you before distributing this program.
 */
#define AIRPLAY_IMPLEMENTATION
#include "airplay_playfair.h"
#include <stdio.h>
#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#endif

#define MAXC 256
static ap_fairplay_t* g_ctx[MAXC];

static int rd(void* b, size_t n) { return fread(b, 1, n, stdin) == n; }
static int reply(int status, const unsigned char* p, unsigned n) {
    unsigned char h[3] = { (unsigned char)status, (unsigned char)n, (unsigned char)(n >> 8) };
    if (fwrite(h, 1, 3, stdout) != 3 || (n && fwrite(p, 1, n, stdout) != n)) return 0;
    return fflush(stdout) == 0;
}
int main(void) {
#ifdef _WIN32
    _setmode(0, _O_BINARY); _setmode(1, _O_BINARY);
#endif
    unsigned char in[512], out[256], h[7];
    while (rd(h, 7)) {
        unsigned cmd = h[0], id = h[1] | (h[2] << 8) | (h[3] << 16) | ((unsigned)h[4] << 24), len = h[5] | (h[6] << 8);
        if (len > sizeof(in) || (len && !rd(in, len))) break;
        int ok = 0; unsigned n = 0;
        if (cmd == 0) {
            for (unsigned i = 0; i < MAXC && !ok; i++) if (!g_ctx[i]) { g_ctx[i] = ap_fairplay_init(); if (g_ctx[i]) { out[0] = (unsigned char)i; out[1] = out[2] = out[3] = 0; n = 4; ok = 1; } }
        } else if (id < MAXC && g_ctx[id]) {
            if (cmd == 1 && len == 16) { ok = ap_fairplay_setup(g_ctx[id], in, out) == 0; n = 142; }
            else if (cmd == 2 && len == 164) { ok = ap_fairplay_handshake(g_ctx[id], in, out) == 0; n = 32; }
            else if (cmd == 3 && len == 72) { ok = ap_fairplay_decrypt(g_ctx[id], in, out) == 0; n = 16; }
            else if (cmd == 4) { ap_fairplay_destroy(g_ctx[id]); g_ctx[id] = NULL; ok = 1; n = 0; }
        }
        if (!reply(ok ? 0 : 1, out, ok ? n : 0)) break;
    }
    return 0;
}
