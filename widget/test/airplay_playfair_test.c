/* fp-setup through the real bundled playfair (GPL) path.
 *   cc -O1 -pthread -DAIRPLAY_PLAYFAIR test/airplay_playfair_test.c -o airplay_playfair_test -lm */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
#include <unistd.h>
int main(void) {
    ap__net_init();
    airplay_config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.name = "PF"; cfg.port = 17300;
    airplay_t* a = airplay_create(&cfg, NULL);
    if (airplay_start(a)) { puts("start failed"); return 1; }
    ap_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof(to)); to.sin_family = AF_INET; to.sin_port = htons((uint16_t)airplay_port(a)); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(s, (struct sockaddr*)&to, sizeof(to)); ap__set_timeout(s, 3000);
    int fails = 0;
    for (int mode = 0; mode < 4; mode++) {
        uint8_t req[16] = { 0x46, 0x50, 0x4c, 0x59, 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, (uint8_t)mode, 0xbb };
        char h[256]; int n = snprintf(h, sizeof(h), "POST /fp-setup RTSP/1.0\r\nCSeq: %d\r\nContent-Length: 16\r\n\r\n", mode + 1);
        send(s, h, n, 0); send(s, (const char*)req, 16, 0);
        char r[1024]; size_t got = 0;
        for (;;) { int k = (int)recv(s, r + got, 1, 0); if (k <= 0) break; got++; if (got >= 4 && !memcmp(r + got - 4, "\r\n\r\n", 4)) break; }
        uint8_t body[142]; int ok = ap__recv_all(s, body, 142) == 0;
        uint8_t want[142]; ap_fairplay_t* fp = ap_fairplay_init(); ap_fairplay_setup(fp, req, want); ap_fairplay_destroy(fp);
        ok = ok && !memcmp(body, want, 142) && !memcmp(body, "FPLY", 4) && body[13] == mode;
        printf("fp-setup mode %d through the bundled playfair: %s\n", mode, ok ? "ok" : "FAIL"); fails += !ok;
    }
    airplay_destroy(a);
    return fails != 0;
}
