/* FairPlay through the separate helper process (airplay-fairplayd; no GPL code in this program).
 *   cc -O2 -o airplay-fairplayd airplay_fairplayd.c -lm
 *   cc -O1 -pthread test/airplay_helper_test.c -o t -ldl && LUNA_AIRPLAY_FAIRPLAYD=./airplay-fairplayd ./t */
/* (old header:) FairPlay through the run-time loaded helper process (no GPL code linked into this program).
 *   cc -shared -fPIC -O2 -o airplay-fairplayd airplay_fairplay_plugin.c -lm
 *   cc -O1 -pthread test/airplay_plugin_test.c -o t -ldl && LUNA_AIRPLAY_FAIRPLAYD=./airplay-fairplayd ./t */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
int main(void) {
    ap__net_init();
    int fails = 0;
    char err[200];
    airplay_fairplay_ops none;
    int rc = airplay_fairplay_spawn(&none, "/nonexistent/airplay-fairplayd", err, sizeof(err));
    printf("%-58s %s\n", "missing library reports an error", rc < 0 && strstr(err, "not found") ? "ok" : (fails++, "FAIL"));
    airplay_fairplay_ops ops;
    rc = airplay_fairplay_spawn(&ops, NULL, err, sizeof(err));
    printf("%-58s %s\n", "helper process found (LUNA_AIRPLAY_FAIRPLAYD / default paths)", rc == 0 ? "ok" : (printf("(%s) ", err), fails++, "FAIL"));
    if (rc) return 1;
    airplay_config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.name = "Helper"; cfg.port = 17410; cfg.fairplay = &ops;
    airplay_t* a = airplay_create(&cfg, NULL);
    if (airplay_start(a)) { puts("start failed"); return 1; }
    ap_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof(to)); to.sin_family = AF_INET; to.sin_port = htons((uint16_t)airplay_port(a)); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(s, (struct sockaddr*)&to, sizeof(to)); ap__set_timeout(s, 3000);
    for (int mode = 0; mode < 4; mode++) {
        uint8_t req[16] = { 0x46, 0x50, 0x4c, 0x59, 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, (uint8_t)mode, 0xbb };
        char h[256]; int n = snprintf(h, sizeof(h), "POST /fp-setup RTSP/1.0\r\nCSeq: %d\r\nContent-Length: 16\r\n\r\n", mode + 1);
        send(s, h, n, 0); send(s, (const char*)req, 16, 0);
        char r[1024]; size_t got = 0;
        for (;;) { int k = (int)recv(s, r + got, 1, 0); if (k <= 0) break; got++; if (got >= 4 && !memcmp(r + got - 4, "\r\n\r\n", 4)) break; }
        uint8_t body[142]; int ok = ap__recv_all(s, body, 142) == 0 && !memcmp(body, "FPLY", 4) && body[13] == mode;
        printf("fp-setup mode %d through the helper process: %s\n", mode, ok ? "ok" : "FAIL"); fails += !ok;
    }
    {   /* decrypt round trip: the helper answers 16 bytes for a 72-byte key message */
        uint8_t hs[164] = { 0x46, 0x50, 0x4c, 0x59, 3 }, r32[32], ek[72] = { 0 }, key[16];
        void* c = ops.create(ops.user);
        int ok = c && ops.setup(c, (const uint8_t[16]){ 0x46, 0x50, 0x4c, 0x59, 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 1, 0 }, (uint8_t[142]){0}) == 0 && ops.handshake(c, hs, r32) == 0 && ops.decrypt(c, ek, key) == 0;
        printf("%-58s %s\n", "create/setup/handshake/decrypt/destroy round trip", ok ? "ok" : (fails++, "FAIL"));
        if (c) ops.destroy(c);
    }
    airplay_destroy(a);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
