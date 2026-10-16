/* airplay_loas_wrap(): raw AAC-LC frames wrapped as LATM/LOAS must decode (ffmpeg -f latm) to exactly
 * what ffmpeg decodes from the original ADTS stream.   Needs ffmpeg.
 *   cc -O1 test/airplay_loas_test.c -o t && ./t /tmp/loas */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
#include <unistd.h>
static uint8_t* rd(const char* p, size_t* n) { FILE* f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t* b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b; }
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/tmp/loas";
    char cmd[1200], p[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s && ffmpeg -v error -y -f lavfi -i 'sine=frequency=440:duration=1.5,aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo' -c:a aac -strict -2 -b:a 128k -f adts %s/a.adts && ffmpeg -v error -y -i %s/a.adts -f s16le %s/want.pcm", dir, dir, dir, dir);
    if (system(cmd)) { puts("ffmpeg failed"); return 2; }
    snprintf(p, sizeof(p), "%s/a.adts", dir); size_t n; uint8_t* adts = rd(p, &n);
    FILE* out = fopen((snprintf(p, sizeof(p), "%s/a.loas", dir), p), "wb");
    static const uint8_t asc[2] = { 0x12, 0x10 };                       /* AAC-LC, 44.1 kHz, stereo */
    size_t pos = 0; int frames = 0;
    while (pos + 7 <= n) {
        if (adts[pos] != 0xff || (adts[pos + 1] & 0xf0) != 0xf0) break;
        size_t flen = (((size_t)adts[pos + 3] & 3) << 11) | ((size_t)adts[pos + 4] << 3) | (adts[pos + 5] >> 5);
        size_t hlen = (adts[pos + 1] & 1) ? 7 : 9;
        if (flen < hlen || pos + flen > n) break;
        uint8_t buf[4096]; size_t w = airplay_loas_wrap(asc, 16, adts + pos + hlen, flen - hlen, buf, sizeof(buf));
        if (!w) { puts("wrap failed"); return 1; }
        fwrite(buf, 1, w, out); pos += flen; frames++;
    }
    fclose(out);
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -y -f latm -i %s/a.loas -f s16le %s/got.pcm", dir, dir);
    int rc = system(cmd);
    snprintf(p, sizeof(p), "%s/want.pcm", dir); size_t wn; uint8_t* want = rd(p, &wn);
    snprintf(p, sizeof(p), "%s/got.pcm", dir); size_t gn; uint8_t* got = rd(p, &gn);
    int ok = rc == 0 && want && got && gn >= wn - 8192 && !memcmp(want, got, wn < gn ? wn : gn);
    printf("%d AAC-LC frames wrapped as LOAS/LATM, decoded %zu vs %zu bytes: %s\n", frames, gn, wn, ok ? "identical ok" : "FAIL");
    return !ok;
}
