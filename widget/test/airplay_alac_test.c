/* ALAC decoder against ffmpeg-made lossless vectors (see gen_alac_vectors.py):
 *   python3 test/gen_alac_vectors.py /tmp/alacv && cc -O1 test/airplay_alac_test.c -o t && ./t /tmp/alacv */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
static uint8_t* rd(const char* p, size_t* n) { FILE* f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t* b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b; }
int main(int argc, char** argv) {
    int fails = 0;
    const char* dir = argc > 1 ? argv[1] : "/tmp/alacv";
    char p[600];
    snprintf(p, sizeof(p), "%s/alac.pcm", dir); size_t pn; uint8_t* pcm = rd(p, &pn);
    if (!pcm) { puts("no vectors (run gen_alac_vectors.py first)"); return 2; }
    const char* subs[] = { "f352", "f4096" };
    for (int si = 0; si < 2; si++) {
        snprintf(p, sizeof(p), "%s/%s/alac.cookie", dir, subs[si]); size_t cn; uint8_t* ck = rd(p, &cn);
        snprintf(p, sizeof(p), "%s/%s/alac.bin", dir, subs[si]); size_t bn; uint8_t* bin = rd(p, &bn);
        snprintf(p, sizeof(p), "%s/%s/alac.sizes", dir, subs[si]); size_t sn; char* sz = (char*)rd(p, &sn); sz[sn] = 0;
        ap_alac d; int rc = ap_alac_init(&d, ck, cn);
        printf("%-44s %s\n", "ap_alac_init from the 36-byte cookie", rc == 0 ? "ok" : (fails++, "FAIL"));
        if (rc) continue;
        int16_t* out = malloc((size_t)d.frame_len * 4); size_t off = 0, total = 0; int bad = 0, pk = 0;
        uint8_t* pp = (uint8_t*)pcm;
        for (char* tok = strtok(sz, " \n"); tok; tok = strtok(NULL, " \n")) {
            size_t len = (size_t)atoi(tok);
            int fr = ap_alac_decode(&d, bin + off, len, out, d.frame_len);
            if (fr < 0) { bad++; off += len; continue; }
            size_t bytes = (size_t)fr * 4;
            if (total + bytes > pn || memcmp(out, pp + total, bytes)) bad++;
            total += bytes; off += len; pk++;
        }
        printf("frame_len=%-5u %d packets: %s (bad=%d, decoded %zu of %zu PCM bytes)\n", d.frame_len, pk, bad == 0 && total >= pn - 16384 ? "bit-exact ok" : (fails++, "FAIL"), bad, total, pn);
        ap_alac_free(&d); free(out); free(ck); free(bin); free(sz);
    }
    {   /* the cookie AirPlay senders imply: 352-sample frames */
        static const uint8_t ap[36] = { 0,0,0,0x24, 0x61,0x6c,0x61,0x63, 0,0,0,0, 0,0,0x01,0x60, 0,0x10,0x28,0x0a, 0x0e,0x02,0x00,0xff, 0,0,0,0, 0,0,0,0, 0,0,0xac,0x44 };
        ap_alac d; int rc = ap_alac_init(&d, ap, sizeof(ap));
        int ok = rc == 0 && d.frame_len == 352 && d.channels == 2 && d.pb == 40 && d.mb == 10 && d.kb == 14;
        printf("%-44s %s\n", "AirPlay's 352-sample cookie parses", ok ? "ok" : (fails++, "FAIL")); ap_alac_free(&d);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
