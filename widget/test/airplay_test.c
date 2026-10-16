/* End-to-end test of airplay.h: a scripted sender talks to the receiver over loopback.
 *   cc -O2 -pthread test/airplay_test.c -o airplay_test && ./airplay_test
 * Uses a stub FairPlay (key = constant) so it needs nothing from airplay_playfair.h. */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
#include <assert.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); char nm_[160]; snprintf(nm_, sizeof(nm_), __VA_ARGS__); printf("%-56s %s\n", nm_, ok_ ? "ok" : "FAIL"); fflush(stdout); if (!ok_) fails++; } while (0)

/* ---- stub FairPlay ---- */
static const uint8_t FP_KEY[16] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x01 };
static void* fp_create(void* u) { (void)u; return malloc(1); }
static int fp_setup(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memset(r, 0xA5, 142); return 0; }
static int fp_hs(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memset(r, 0x5A, 32); return 0; }
static int fp_dec(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memcpy(r, FP_KEY, 16); return 0; }
static void fp_free(void* c) { free(c); }

/* ---- sink ---- */
static uint8_t g_video[8][4096]; static size_t g_video_n[8]; static int g_video_cnt, g_idr[8];
static uint8_t g_audio[2048]; static size_t g_audio_n; static int g_audio_ct;
static float g_vol; static int g_client; static int g_w, g_h;
static void on_video(void* u, const uint8_t* d, size_t n, uint64_t ts, int idr) { (void)u; (void)ts; if (g_video_cnt < 8 && n <= 4096) { memcpy(g_video[g_video_cnt], d, n); g_video_n[g_video_cnt] = n; g_idr[g_video_cnt++] = idr; } }
static void on_audio(void* u, const uint8_t* d, size_t n, int ct, uint32_t ts) { (void)u; (void)ts; if (n <= sizeof(g_audio)) { memcpy(g_audio, d, n); g_audio_n = n; g_audio_ct = ct; } }
static void on_vol(void* u, float v) { (void)u; g_vol = v; }
static void on_client(void* u, int c, const char* n, const char* a) { (void)u; (void)n; (void)a; g_client += c ? 1 : -1; }
static void on_cfg(void* u, int c, int w, int h) { (void)u; (void)c; g_w = w; g_h = h; }

/* ---- a tiny RTSP client ---- */
static int rpc(ap_sock s, int cseq, const char* method, const char* url, const char* ctype, const uint8_t* body, size_t bn, uint8_t* rbody, size_t* rbn, int* code) {
    char h[512];
    int n = snprintf(h, sizeof(h), "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: AirPlay/test\r\n", method, url, cseq);
    if (ctype) n += snprintf(h + n, sizeof(h) - n, "Content-Type: %s\r\n", ctype);
    n += snprintf(h + n, sizeof(h) - n, "Content-Length: %zu\r\n\r\n", bn);
    send(s, h, n, 0); if (bn) send(s, (const char*)body, bn, 0);
    char r[2048]; size_t got = 0;
    for (;;) { int k = (int)recv(s, r + got, 1, 0); if (k <= 0) return -1; got += k; if (got >= 4 && !memcmp(r + got - 4, "\r\n\r\n", 4)) break; }
    r[got] = 0;
    sscanf(r, "RTSP/1.0 %d", code);
    const char* cl = strstr(r, "Content-Length:");
    size_t len = cl ? (size_t)atoi(cl + 15) : 0;
    if (rbn) *rbn = len;
    if (len && ap__recv_all(s, rbody, len)) return -1;
    return 0;
}

int main(void) {
    ap__net_init();
    /* -------- bplist round trip -------- */
    {
        ap_pl* d = ap_pl_new(AP_PL_DICT);
        ap_pl_set(d, "name", ap_pl_str("Luna"));
        ap_pl_set(d, "n", ap_pl_int(70000)); ap_pl_set(d, "neg", ap_pl_bool(1)); ap_pl_set(d, "r", ap_pl_real(-20.5));
        uint8_t blob[300]; for (int i = 0; i < 300; i++) blob[i] = (uint8_t)i;
        ap_pl_set(d, "blob", ap_pl_data(blob, 300));
        ap_pl* arr = ap_pl_new(AP_PL_ARRAY);
        for (int i = 0; i < 300; i++) ap_pl_push(arr, ap_pl_int(i));
        ap_pl_set(d, "arr", arr);
        size_t n; uint8_t* b = ap_pl_serialize(d, &n);
        ap_pl* e = ap_pl_parse(b, n);
        CHECK(e && ap_pl_get(e, "n") && ap_pl_get(e, "n")->i == 70000, "bplist int round trip");
        CHECK(e && ap_pl_get(e, "name") && !strcmp((char*)ap_pl_get(e, "name")->d, "Luna"), "bplist string round trip");
        CHECK(e && ap_pl_get(e, "r") && ap_pl_get(e, "r")->r == -20.5, "bplist real round trip");
        CHECK(e && ap_pl_get(e, "blob") && ap_pl_get(e, "blob")->n == 300 && ap_pl_get(e, "blob")->d[299] == (uint8_t)299, "bplist data (long header)");
        CHECK(e && ap_pl_get(e, "arr") && ap_pl_get(e, "arr")->cnt == 300 && ap_pl_get(e, "arr")->v[299]->i == 299, "bplist 300-element array (2-byte refs)");
        ap_pl_free(d); ap_pl_free(e); free(b);
        uint8_t junk[64] = "bplist00garbagegarbagegarbagegarbagegarbagegarbagegarbage";
        CHECK(ap_pl_parse(junk, sizeof(junk)) == NULL, "bplist rejects garbage");
    }

    /* -------- receiver -------- */
    airplay_fairplay_ops ops = { NULL, fp_create, fp_setup, fp_hs, fp_dec, fp_free };
    airplay_callbacks cb = { NULL, NULL, on_client, on_cfg, on_video, NULL, on_audio, on_vol };
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 3 + 1);
    airplay_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.name = "Test Luna"; cfg.port = 17000; cfg.ed_seed = seed; cfg.fairplay = &ops;
    airplay_t* a = airplay_create(&cfg, &cb);
    CHECK(a && airplay_start(a) == 0, "receiver starts");
    int port = airplay_port(a);

    ap_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof(to)); to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(s, (struct sockaddr*)&to, sizeof(to)) == 0, "sender connects to :%d", port);
    ap__set_timeout(s, 3000);
    uint8_t rb[4096]; size_t rn; int code;

    CHECK(rpc(s, 1, "GET", "/info", NULL, NULL, 0, rb, &rn, &code) == 0 && code == 200, "GET /info -> 200");
    ap_pl* info = ap_pl_parse(rb, rn);
    CHECK(info && ap_pl_get(info, "name") && !strcmp((char*)ap_pl_get(info, "name")->d, "Test Luna"), "/info carries the device name");
    CHECK(info && ap_pl_get(info, "pk") && ap_pl_get(info, "pk")->n == 32 && !memcmp(ap_pl_get(info, "pk")->d, airplay_public_key(a), 32), "/info pk == identity key");
    CHECK(info && ap_pl_get(info, "displays") && ap_pl_get(info, "displays")->cnt == 1, "/info lists a display");
    ap_pl_free(info);

    /* pair-setup */
    uint8_t cseed[32], cpk[32]; for (int i = 0; i < 32; i++) cseed[i] = (uint8_t)(200 - i);
    ap_ed25519_pubkey(cpk, cseed);
    CHECK(rpc(s, 2, "POST", "/pair-setup", "application/octet-stream", cpk, 32, rb, &rn, &code) == 0 && code == 200 && rn == 32 && !memcmp(rb, airplay_public_key(a), 32), "pair-setup returns the server key");

    /* pair-verify (sender side) */
    uint8_t ecs[32], ecp[32]; for (int i = 0; i < 32; i++) ecs[i] = (uint8_t)(i + 77);
    ap_x25519_base(ecp, ecs);
    uint8_t pv1[4 + 64] = { 1, 0, 0, 0 }; memcpy(pv1 + 4, ecp, 32); memcpy(pv1 + 36, cpk, 32);
    CHECK(rpc(s, 3, "POST", "/pair-verify", "application/octet-stream", pv1, sizeof(pv1), rb, &rn, &code) == 0 && code == 200 && rn == 96, "pair-verify step 1");
    uint8_t srv_pub[32], sig[64], secret[32], buf[64], hh[64], key[16], iv[16];
    memcpy(srv_pub, rb, 32); memcpy(sig, rb + 32, 64);
    ap_x25519(secret, ecs, srv_pub);
    memcpy(buf, "Pair-Verify-AES-Key", 19); memcpy(buf + 19, secret, 32); ap_sha512(hh, buf, 51); memcpy(key, hh, 16);
    memcpy(buf, "Pair-Verify-AES-IV", 18); memcpy(buf + 18, secret, 32); ap_sha512(hh, buf, 50); memcpy(iv, hh, 16);
    ap_aes_ctr ctr; ap_aes_ctr_init(&ctr, key, iv);
    ap_aes_ctr_xor(&ctr, sig, sig, 64);
    uint8_t msg[64]; memcpy(msg, srv_pub, 32); memcpy(msg + 32, ecp, 32);
    CHECK(ap_ed25519_verify(sig, msg, 64, airplay_public_key(a)), "server signature verifies (key agreement matches)");
    uint8_t csig[64]; memcpy(msg, ecp, 32); memcpy(msg + 32, srv_pub, 32);
    ap_ed25519_sign(csig, msg, 64, cseed);
    ap_aes_ctr_xor(&ctr, csig, csig, 64);
    uint8_t pv2[4 + 64] = { 0, 0, 0, 0 }; memcpy(pv2 + 4, csig, 64);
    CHECK(rpc(s, 4, "POST", "/pair-verify", "application/octet-stream", pv2, sizeof(pv2), rb, &rn, &code) == 0 && code == 200, "pair-verify step 2 accepted");

    /* fp-setup (stub) */
    uint8_t fp16[16] = { 0x46, 0x50, 0x4c, 0x59, 3, 1, 1 }, fp164[164] = { 0x46, 0x50, 0x4c, 0x59, 3 };
    CHECK(rpc(s, 5, "POST", "/fp-setup", "application/octet-stream", fp16, 16, rb, &rn, &code) == 0 && rn == 142, "fp-setup phase 1 (16 -> 142)");
    CHECK(rpc(s, 6, "POST", "/fp-setup", "application/octet-stream", fp164, 164, rb, &rn, &code) == 0 && rn == 32, "fp-setup phase 2 (164 -> 32)");

    /* SETUP #1: keys */
    uint8_t eiv[16]; for (int i = 0; i < 16; i++) eiv[i] = (uint8_t)(0xE0 + i);
    uint8_t ekey[72] = { 0 };
    ap_pl* q = ap_pl_new(AP_PL_DICT);
    ap_pl_set(q, "ekey", ap_pl_data(ekey, 72)); ap_pl_set(q, "eiv", ap_pl_data(eiv, 16));
    ap_pl_set(q, "deviceID", ap_pl_str("11:22:33:44:55:66")); ap_pl_set(q, "name", ap_pl_str("Test iPhone"));
    size_t qn; uint8_t* qb = ap_pl_serialize(q, &qn); ap_pl_free(q);
    CHECK(rpc(s, 7, "SETUP", "rtsp://127.0.0.1/1", "application/x-apple-binary-plist", qb, qn, rb, &rn, &code) == 0 && code == 200, "SETUP (keys + timing)");
    free(qb);
    usleep(100000);
    CHECK(g_client == 1, "on_client(connected) fired");

    /* the session AES key as the receiver derives it (FairPlay key hashed with the ECDH secret) */
    uint8_t kb[48], aeskey[16]; memcpy(kb, FP_KEY, 16); memcpy(kb + 16, secret, 32); ap_sha512(hh, kb, 48); memcpy(aeskey, hh, 16);

    /* SETUP #2: mirror stream */
    q = ap_pl_new(AP_PL_DICT);
    ap_pl* sts = ap_pl_new(AP_PL_ARRAY); ap_pl* st = ap_pl_new(AP_PL_DICT);
    ap_pl_set(st, "type", ap_pl_int(110)); ap_pl_set(st, "streamConnectionID", ap_pl_int(1234567890123LL));
    ap_pl_push(sts, st); ap_pl_set(q, "streams", sts);
    qb = ap_pl_serialize(q, &qn); ap_pl_free(q);
    CHECK(rpc(s, 8, "SETUP", "rtsp://127.0.0.1/1", "application/x-apple-binary-plist", qb, qn, rb, &rn, &code) == 0 && code == 200, "SETUP mirror stream");
    free(qb);
    ap_pl* res = ap_pl_parse(rb, rn);
    ap_pl* rs0 = res && ap_pl_get(res, "streams") && ap_pl_get(res, "streams")->cnt ? ap_pl_get(res, "streams")->v[0] : NULL;
    int dport = rs0 && ap_pl_get(rs0, "dataPort") ? (int)ap_pl_get(rs0, "dataPort")->i : 0;
    CHECK(dport > 0, "mirror dataPort returned (%d)", dport);
    ap_pl_free(res);

    ap_sock m = socket(AF_INET, SOCK_STREAM, 0);
    to.sin_port = htons((uint16_t)dport);
    CHECK(connect(m, (struct sockaddr*)&to, sizeof(to)) == 0, "mirror data connection");
    {
        /* avcC packet (type 1): 6 header bytes, SPS len BE16 at 6, SPS, 1 pps, PPS len BE16, PPS */
        uint8_t sps[] = { 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9 }, pps[] = { 0x68, 0xee, 0x3c, 0x80 };
        uint8_t avcc[64]; size_t k = 0;
        avcc[k++] = 1; avcc[k++] = 0x64; avcc[k++] = 0; avcc[k++] = 0x1f; avcc[k++] = 0xff; avcc[k++] = 0xe1;
        avcc[k++] = 0; avcc[k++] = sizeof(sps); memcpy(avcc + k, sps, sizeof(sps)); k += sizeof(sps);
        avcc[k++] = 1; avcc[k++] = 0; avcc[k++] = sizeof(pps); memcpy(avcc + k, pps, sizeof(pps)); k += sizeof(pps);
        uint8_t hdr[128] = { 0 };
        hdr[0] = (uint8_t)k; hdr[4] = 1;
        float w = 1280, hgt = 720; memcpy(hdr + 40, &w, 4); memcpy(hdr + 44, &hgt, 4);
        send(m, (const char*)hdr, 128, 0); send(m, (const char*)avcc, k, 0);
        /* an encrypted IDR frame (type 0): [len BE32][nal 0x65 ...]; AES-CTR keyed from the stream id */
        uint8_t frame[20]; ap__put32be(frame, 16); frame[4] = 0x65; for (int i = 5; i < 20; i++) frame[i] = (uint8_t)(i * 7);
        uint8_t plain[20]; memcpy(plain, frame, 20);
        uint8_t kbuf[64], k16[16], i16[16]; int l = snprintf((char*)kbuf, sizeof(kbuf), "AirPlayStreamKey%llu", 1234567890123ULL);
        memcpy(kbuf + l, aeskey, 16); ap_sha512(hh, kbuf, l + 16); memcpy(k16, hh, 16);
        l = snprintf((char*)kbuf, sizeof(kbuf), "AirPlayStreamIV%llu", 1234567890123ULL);
        memcpy(kbuf + l, aeskey, 16); ap_sha512(hh, kbuf, l + 16); memcpy(i16, hh, 16);
        ap_aes_ctr sc; ap_aes_ctr_init(&sc, k16, i16);
        ap_aes_ctr_xor(&sc, frame, frame, 20);
        memset(hdr, 0, 128); hdr[0] = 20; hdr[4] = 0; hdr[5] = 0x10; hdr[8] = 1; 
        send(m, (const char*)hdr, 128, 0); send(m, (const char*)frame, 20, 0);
        /* a second frame continues the same keystream across a non-block-aligned boundary */
        uint8_t f2[13]; ap__put32be(f2, 9); f2[4] = 0x41; for (int i = 5; i < 13; i++) f2[i] = (uint8_t)(i * 11);
        uint8_t plain2[13]; memcpy(plain2, f2, 13);
        ap_aes_ctr_xor(&sc, f2, f2, 13);
        hdr[0] = 13; hdr[5] = 0; hdr[8] = 2;
        send(m, (const char*)hdr, 128, 0); send(m, (const char*)f2, 13, 0);
        usleep(300000);
        const uint8_t sc4[4] = { 0, 0, 0, 1 };
        CHECK(g_w == 1280 && g_h == 720, "on_video_config 1280x720");
        CHECK(g_video_cnt >= 3, "3 video callbacks (params, IDR, P) got %d", g_video_cnt);
        int ok1 = g_video_cnt > 0 && g_video_n[0] == 4 + sizeof(sps) + 4 + sizeof(pps) && !memcmp(g_video[0], sc4, 4) && !memcmp(g_video[0] + 4, sps, sizeof(sps));
        CHECK(ok1, "SPS/PPS delivered as Annex-B");
        int ok2 = g_video_cnt > 1 && g_video_n[1] == 20 && !memcmp(g_video[1], sc4, 4) && !memcmp(g_video[1] + 4, plain + 4, 16) && g_idr[1];
        CHECK(ok2, "encrypted IDR decrypted + AVCC->Annex-B + idr flag");
        int ok3 = g_video_cnt > 2 && g_video_n[2] == 13 && !memcmp(g_video[2] + 4, plain2 + 4, 9) && !g_idr[2];
        CHECK(ok3, "keystream carries across frames (unaligned)");
    }
    AP_CLOSESOCK(m);

    /* SETUP #3: audio stream */
    q = ap_pl_new(AP_PL_DICT); sts = ap_pl_new(AP_PL_ARRAY); st = ap_pl_new(AP_PL_DICT);
    ap_pl_set(st, "type", ap_pl_int(96)); ap_pl_set(st, "ct", ap_pl_int(2)); ap_pl_set(st, "controlPort", ap_pl_int(50000));
    ap_pl_push(sts, st); ap_pl_set(q, "streams", sts);
    qb = ap_pl_serialize(q, &qn); ap_pl_free(q);
    CHECK(rpc(s, 9, "SETUP", "rtsp://127.0.0.1/1", "application/x-apple-binary-plist", qb, qn, rb, &rn, &code) == 0 && code == 200, "SETUP audio stream");
    free(qb);
    res = ap_pl_parse(rb, rn);
    rs0 = res && ap_pl_get(res, "streams") && ap_pl_get(res, "streams")->cnt ? ap_pl_get(res, "streams")->v[0] : NULL;
    int adport = rs0 && ap_pl_get(rs0, "dataPort") ? (int)ap_pl_get(rs0, "dataPort")->i : 0;
    CHECK(adport > 0 && rs0 && ap_pl_get(rs0, "controlPort"), "audio data/control ports returned");
    ap_pl_free(res);
    {
        ap_sock u = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ua = to; ua.sin_port = htons((uint16_t)adport);
        uint8_t pt[53], ct[53], pkt[12 + 53] = { 0x80, 0x60, 0, 7, 0, 0, 0x10, 0, 0, 0, 0, 1 };
        for (int i = 0; i < 53; i++) pt[i] = (uint8_t)(i * 5 + 3);
        ap_aes128 ae; ap_aes128_init(&ae, aeskey);
        /* CBC-encrypt the 48-byte aligned part; the 5-byte tail is sent in clear (AirPlay behaviour) */
        uint8_t prev[16]; memcpy(prev, eiv, 16);
        for (int b = 0; b < 3; b++) { uint8_t x[16]; for (int j = 0; j < 16; j++) x[j] = (uint8_t)(pt[b * 16 + j] ^ prev[j]); ap_aes128_encrypt_block(&ae, x, ct + b * 16); memcpy(prev, ct + b * 16, 16); }
        memcpy(ct + 48, pt + 48, 5);
        memcpy(pkt + 12, ct, 53);
        sendto(u, (const char*)pkt, sizeof(pkt), 0, (struct sockaddr*)&ua, sizeof(ua));
        usleep(300000);
        CHECK(g_audio_n == 53 && g_audio_ct == 2 && !memcmp(g_audio, pt, 53), "audio frame decrypted (CBC + clear tail), ct=ALAC");
        AP_CLOSESOCK(u);
    }

    uint8_t vol[] = "volume: -15.500000\r\n";
    CHECK(rpc(s, 10, "SET_PARAMETER", "rtsp://127.0.0.1/1", "text/parameters", vol, sizeof(vol) - 1, rb, &rn, &code) == 0 && code == 200, "SET_PARAMETER volume");
    usleep(50000);
    CHECK(g_vol == -15.5f, "on_volume(-15.5)");
    CHECK(rpc(s, 11, "TEARDOWN", "rtsp://127.0.0.1/1", NULL, NULL, 0, rb, &rn, &code) == 0 && code == 200, "TEARDOWN");
    AP_CLOSESOCK(s);
    usleep(300000);
    CHECK(g_client == 0, "on_client(disconnected) fired");

    /* a sender with no FairPlay key is refused, not crashed */
    {
        airplay_config c2 = cfg; c2.port = 17100; c2.fairplay = NULL;
        airplay_t* b = airplay_create(&c2, &cb);
        CHECK(airplay_start(b) == 0, "second receiver (no FairPlay) starts");
        ap_sock t = socket(AF_INET, SOCK_STREAM, 0); to.sin_port = htons((uint16_t)airplay_port(b));
        connect(t, (struct sockaddr*)&to, sizeof(to)); ap__set_timeout(t, 3000);
        CHECK(rpc(t, 1, "POST", "/fp-setup", "application/octet-stream", fp16, 16, rb, &rn, &code) == 0 && code == 501, "fp-setup without FairPlay -> 501");
        AP_CLOSESOCK(t);
        airplay_destroy(b);
    }
    airplay_destroy(a);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
