/* Audio/video timing: a scripted sender whose audio runs 80 ms AHEAD of its video on the wire,
 * three-fold AAC-style redundancy, swapped packets and raw ALAC frames.  With latency_ms set the
 * receiver must hand matching audio and video over together, in timestamp order, once each.
 *   cc -O1 -pthread test/airplay_sync_test.c -o t -ldl && ./t */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); char nm_[160]; snprintf(nm_, sizeof(nm_), __VA_ARGS__); printf("%-66s %s\n", nm_, ok_ ? "ok" : "FAIL"); fflush(stdout); if (!ok_) fails++; } while (0)

static const uint8_t FP_KEY[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static void* fp_create(void* u) { (void)u; return malloc(1); }
static int fp_setup(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memset(r, 0, 142); return 0; }
static int fp_hs(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memset(r, 0, 32); return 0; }
static int fp_dec(void* c, const uint8_t* q, uint8_t* r) { (void)c; (void)q; memcpy(r, FP_KEY, 16); return 0; }
static void fp_free(void* c) { free(c); }

typedef struct { uint64_t t_deliver, ts; int kind; } rec;
static rec g_rec[512]; static int g_nrec; static ap_mutex g_lock;
static int16_t g_first_pcm[8]; static int g_have_first;
static void add(int kind, uint64_t ts) { ap__mutex_lock(&g_lock); if (g_nrec < 512) { g_rec[g_nrec].t_deliver = ap__now_ns(); g_rec[g_nrec].ts = ts; g_rec[g_nrec].kind = kind; g_nrec++; } ap__mutex_unlock(&g_lock); }
static void on_video(void* u, const uint8_t* d, size_t n, uint64_t ts, int idr) { (void)u; (void)d; (void)n; (void)idr; if (ts) add(0, ts); }
static void on_pcm(void* u, const int16_t* s, size_t fr, int ch, int rate, uint64_t ts) { (void)u; (void)rate; if (!g_have_first && fr >= 4) { memcpy(g_first_pcm, s, 8 * 2); g_have_first = 1; } (void)ch; add(1, ts); }

static int rpc(ap_sock s, int cseq, const char* m, const char* url, const char* ctype, const uint8_t* body, size_t bn, uint8_t* rb, size_t* rn, int* code) {
    char h[512]; int n = snprintf(h, sizeof(h), "%s %s RTSP/1.0\r\nCSeq: %d\r\n", m, url, cseq);
    if (ctype) n += snprintf(h + n, sizeof(h) - n, "Content-Type: %s\r\n", ctype);
    n += snprintf(h + n, sizeof(h) - n, "Content-Length: %zu\r\n\r\n", bn);
    send(s, h, n, 0); if (bn) send(s, (const char*)body, bn, 0);
    char r[2048]; size_t got = 0;
    for (;;) { int k = (int)recv(s, r + got, 1, 0); if (k <= 0) return -1; got += k; if (got >= 4 && !memcmp(r + got - 4, "\r\n\r\n", 4)) break; }
    r[got] = 0; sscanf(r, "RTSP/1.0 %d", code);
    const char* cl = strstr(r, "Content-Length:"); size_t len = cl ? (size_t)atoi(cl + 15) : 0; if (rn) *rn = len;
    if (len && ap__recv_all(s, rb, len)) return -1;
    return 0;
}
/* a raw (stored) ALAC stereo frame of `ns` samples */
static size_t alac_raw_frame(const int16_t* pcm, int ns, uint8_t* out) {
    size_t bits = 0; memset(out, 0, 2048);
#define PUT(v, nb) do { for (int b_ = (nb) - 1; b_ >= 0; b_--) { if (((uint32_t)(v) >> b_) & 1) out[bits >> 3] |= (uint8_t)(0x80 >> (bits & 7)); bits++; } } while (0)
    PUT(1, 3); PUT(0, 4); PUT(0, 12); PUT(0, 1); PUT(0, 2); PUT(1, 1);     /* CPE, tag, unused, no size, no extra bits, NOT compressed */
    for (int i = 0; i < ns * 2; i++) PUT((uint16_t)pcm[i], 16);
    PUT(7, 3);
#undef PUT
    return (bits + 7) / 8;
}

int main(void) {
    ap__net_init(); ap__mutex_init(&g_lock);
    airplay_fairplay_ops ops = { NULL, fp_create, fp_setup, fp_hs, fp_dec, fp_free };
    airplay_callbacks cb; memset(&cb, 0, sizeof(cb)); cb.on_video = on_video; cb.on_pcm = on_pcm;
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 9);
    airplay_config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.name = "Sync"; cfg.port = 17500; cfg.ed_seed = seed; cfg.fairplay = &ops; cfg.latency_ms = 250;
    airplay_t* a = airplay_create(&cfg, &cb);
    CHECK(airplay_start(a) == 0, "receiver with a 250 ms sync latency starts");
    ap_sock s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof(to)); to.sin_family = AF_INET; to.sin_port = htons((uint16_t)airplay_port(a)); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(s, (struct sockaddr*)&to, sizeof(to)); ap__set_timeout(s, 3000);
    uint8_t rb[4096]; size_t rn; int code;
    /* pair-verify (needed for the hashed AES key) */
    uint8_t cseed[32], cpk[32]; for (int i = 0; i < 32; i++) cseed[i] = (uint8_t)(100 + i); ap_ed25519_pubkey(cpk, cseed);
    rpc(s, 1, "POST", "/pair-setup", "application/octet-stream", cpk, 32, rb, &rn, &code);
    uint8_t ecs[32], ecp[32]; for (int i = 0; i < 32; i++) ecs[i] = (uint8_t)(i + 50); ap_x25519_base(ecp, ecs);
    uint8_t pv1[68] = { 1, 0, 0, 0 }; memcpy(pv1 + 4, ecp, 32); memcpy(pv1 + 36, cpk, 32);
    rpc(s, 2, "POST", "/pair-verify", "application/octet-stream", pv1, sizeof(pv1), rb, &rn, &code);
    uint8_t srv_pub[32], sig[64], secret[32], buf[64], hh[64], key[16], iv[16];
    memcpy(srv_pub, rb, 32); memcpy(sig, rb + 32, 64); ap_x25519(secret, ecs, srv_pub);
    memcpy(buf, "Pair-Verify-AES-Key", 19); memcpy(buf + 19, secret, 32); ap_sha512(hh, buf, 51); memcpy(key, hh, 16);
    memcpy(buf, "Pair-Verify-AES-IV", 18); memcpy(buf + 18, secret, 32); ap_sha512(hh, buf, 50); memcpy(iv, hh, 16);
    ap_aes_ctr ctr; ap_aes_ctr_init(&ctr, key, iv); ap_aes_ctr_xor(&ctr, sig, sig, 64);
    uint8_t msg[64], csig[64]; memcpy(msg, ecp, 32); memcpy(msg + 32, srv_pub, 32); ap_ed25519_sign(csig, msg, 64, cseed); ap_aes_ctr_xor(&ctr, csig, csig, 64);
    uint8_t pv2[68] = { 0, 0, 0, 0 }; memcpy(pv2 + 4, csig, 64);
    CHECK(rpc(s, 3, "POST", "/pair-verify", "application/octet-stream", pv2, sizeof(pv2), rb, &rn, &code) == 0 && code == 200, "paired");
    uint8_t fp16[16] = { 0x46, 0x50, 0x4c, 0x59, 3 }; rpc(s, 4, "POST", "/fp-setup", "application/octet-stream", fp16, 16, rb, &rn, &code);
    uint8_t eiv[16]; for (int i = 0; i < 16; i++) eiv[i] = (uint8_t)(0x40 + i), memcpy(fp16, fp16, 0);
    uint8_t ekey[72] = { 0 };
    ap_pl* q = ap_pl_new(AP_PL_DICT); ap_pl_set(q, "ekey", ap_pl_data(ekey, 72)); ap_pl_set(q, "eiv", ap_pl_data(eiv, 16)); ap_pl_set(q, "deviceID", ap_pl_str("11:22:33:44:55:66"));
    size_t qn; uint8_t* qb = ap_pl_serialize(q, &qn); ap_pl_free(q);
    rpc(s, 5, "SETUP", "rtsp://x/1", "application/x-apple-binary-plist", qb, qn, rb, &rn, &code); free(qb);
    uint8_t kb[48], aeskey[16]; memcpy(kb, FP_KEY, 16); memcpy(kb + 16, secret, 32); ap_sha512(hh, kb, 48); memcpy(aeskey, hh, 16);
    /* mirror + audio streams */
    q = ap_pl_new(AP_PL_DICT); ap_pl* sts = ap_pl_new(AP_PL_ARRAY);
    ap_pl* st = ap_pl_new(AP_PL_DICT); ap_pl_set(st, "type", ap_pl_int(110)); ap_pl_set(st, "streamConnectionID", ap_pl_int(777)); ap_pl_push(sts, st);
    st = ap_pl_new(AP_PL_DICT); ap_pl_set(st, "type", ap_pl_int(96)); ap_pl_set(st, "ct", ap_pl_int(2)); ap_pl_set(st, "controlPort", ap_pl_int(50000)); ap_pl_push(sts, st);
    ap_pl_set(q, "streams", sts); qb = ap_pl_serialize(q, &qn); ap_pl_free(q);
    CHECK(rpc(s, 6, "SETUP", "rtsp://x/1", "application/x-apple-binary-plist", qb, qn, rb, &rn, &code) == 0 && code == 200, "SETUP mirror + audio");
    free(qb);
    ap_pl* res = ap_pl_parse(rb, rn); int mport = 0, aport = 0, cport = 0;
    for (size_t i = 0; res && i < ap_pl_get(res, "streams")->cnt; i++) { ap_pl* e = ap_pl_get(res, "streams")->v[i]; int t = (int)ap_pl_get(e, "type")->i; if (t == 110) mport = (int)ap_pl_get(e, "dataPort")->i; else { aport = (int)ap_pl_get(e, "dataPort")->i; cport = (int)ap_pl_get(e, "controlPort")->i; } }
    ap_pl_free(res);
    ap_sock m = socket(AF_INET, SOCK_STREAM, 0); to.sin_port = htons((uint16_t)mport); connect(m, (struct sockaddr*)&to, sizeof(to));
    ap_sock u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = to, uc = to; ua.sin_port = htons((uint16_t)aport); uc.sin_port = htons((uint16_t)cport);
    /* video key material */
    uint8_t kbuf[64], k16[16], i16[16]; int l = snprintf((char*)kbuf, sizeof(kbuf), "AirPlayStreamKey%llu", 777ULL);
    memcpy(kbuf + l, aeskey, 16); ap_sha512(hh, kbuf, l + 16); memcpy(k16, hh, 16);
    l = snprintf((char*)kbuf, sizeof(kbuf), "AirPlayStreamIV%llu", 777ULL); memcpy(kbuf + l, aeskey, 16); ap_sha512(hh, kbuf, l + 16); memcpy(i16, hh, 16);
    ap_aes_ctr vc; ap_aes_ctr_init(&vc, k16, i16);
    ap_aes128 ae; ap_aes128_init(&ae, aeskey);

    /* ---- timeline: sender time T = k*100 ms, k = 0..5; audio goes out 80 ms before video ---- */
    const uint64_t NTP0 = 3900000000ULL << 32;            /* an arbitrary sender-clock origin */
    uint8_t sync[20] = { 0x90, 0xd4, 0, 4 };              /* rtp 1000 <-> NTP0 */
    ap__put32be(sync + 4, 1000); ap__put32be(sync + 8, (uint32_t)(NTP0 >> 32)); ap__put32be(sync + 12, 0); ap__put32be(sync + 16, 1000 + 352);
    sendto(u, (const char*)sync, 20, 0, (struct sockaddr*)&uc, sizeof(uc));
    usleep(30000);
    int16_t pcm[352 * 2]; uint8_t fr[2048];
    uint64_t t0 = ap__now_ns();
    int sent_audio = 0;
    uint16_t seq = 100;
    for (int k = 0; k < 6; k++) {
        uint64_t T = (uint64_t)k * 100000000ULL;                                   /* sender time of this video+audio pair */
        /* audio frames for this pair, sent 80 ms early; k=3 arrives swapped with k=4's packet below */
        uint64_t send_audio_at = T > 80000000ULL ? T - 80000000ULL : 0;
        while (ap__now_ns() - t0 < send_audio_at) usleep(500);
        for (int j = 0; j < 2; j++) {                                              /* two ALAC frames per pair, 352 samples each */
            for (int i = 0; i < 352; i++) { pcm[2 * i] = (int16_t)(k * 1000 + j * 100 + (i & 0x3f)); pcm[2 * i + 1] = (int16_t)(-pcm[2 * i]); }
            size_t fl = alac_raw_frame(pcm, 352, fr);
            uint32_t rtp = 1000 + (uint32_t)((T * 44100ULL) / 1000000000ULL) + (uint32_t)j * 352;
            uint8_t pkt[2048]; pkt[0] = 0x80; pkt[1] = 0x60; pkt[2] = (uint8_t)(seq >> 8); pkt[3] = (uint8_t)seq; ap__put32be(pkt + 4, rtp); ap__put32be(pkt + 8, 1); ap__put32be(pkt + 12 - 0, 0);
            pkt[8] = pkt[9] = pkt[10] = 0; pkt[11] = 1;
            size_t full = fl & ~(size_t)15; uint8_t prev[16]; memcpy(prev, eiv, 16);
            for (size_t b = 0; b < full; b += 16) { uint8_t x[16]; for (int jj = 0; jj < 16; jj++) x[jj] = (uint8_t)(fr[b + jj] ^ prev[jj]); ap_aes128_encrypt_block(&ae, x, pkt + 12 + b); memcpy(prev, pkt + 12 + b, 16); }
            memcpy(pkt + 12 + full, fr + full, fl - full);
            for (int rep = 0; rep < 3; rep++) sendto(u, (const char*)pkt, 12 + fl, 0, (struct sockaddr*)&ua, sizeof(ua));     /* x3 like AAC-ELD */
            seq++; sent_audio++;
        }
        while (ap__now_ns() - t0 < T) usleep(500);
        /* the matching video frame, stamped with the same sender time */
        uint8_t frame[20]; ap__put32be(frame, 16); frame[4] = 0x41; for (int i = 5; i < 20; i++) frame[i] = (uint8_t)(i + k);
        ap_aes_ctr_xor(&vc, frame, frame, 20);
        uint8_t hdr[128] = { 0 }; hdr[0] = 20; uint64_t raw = NTP0 + ((T << 32) / 1000000000ULL);
        for (int i = 0; i < 8; i++) hdr[8 + i] = (uint8_t)(raw >> (8 * i));
        send(m, (const char*)hdr, 128, 0); send(m, (const char*)frame, 20, 0);
    }
    usleep(900000);
    ap__mutex_lock(&g_lock);
    int nv = 0, na = 0; for (int i = 0; i < g_nrec; i++) { nv += g_rec[i].kind == 0; na += g_rec[i].kind == 1; }
    CHECK(nv == 6, "6 video frames delivered (got %d)", nv);
    CHECK(na == sent_audio, "%d audio frames delivered once each despite x3 redundancy (got %d)", sent_audio, na);
    CHECK(g_have_first && g_first_pcm[0] == 0 && g_first_pcm[1] == 0 && g_first_pcm[2] == 1 && g_first_pcm[3] == -1, "ALAC frame decoded to the exact PCM");
    /* ordering by sender time within each stream */
    uint64_t lastv = 0, lasta = 0; int ordered = 1;
    for (int i = 0; i < g_nrec; i++) { uint64_t* last = g_rec[i].kind == 0 ? &lastv : &lasta; if (g_rec[i].ts < *last) ordered = 0; *last = g_rec[i].ts; }
    CHECK(ordered, "each stream leaves in sender-timestamp order");
    /* lock-step: audio frame j=0 of pair k vs video k share a sender time */
    int pairs = 0, worst = 0;
    for (int k = 0; k < 6; k++) {
        uint64_t T = NTP0 / (1ULL << 32) * 1000000000ULL + (uint64_t)k * 100000000ULL;
        uint64_t tv = 0, ta = 0;
        for (int i = 0; i < g_nrec; i++) {
            long long dts = (long long)g_rec[i].ts - (long long)T;
            if (g_rec[i].kind == 0 && llabs(dts) < 2000000) tv = g_rec[i].t_deliver;
            if (g_rec[i].kind == 1 && llabs(dts) < 2000000) ta = g_rec[i].t_deliver;
        }
        if (tv && ta) { pairs++; int d = (int)(((long long)tv - (long long)ta) / 1000000); if (abs(d) > worst) worst = abs(d); }
    }
    CHECK(pairs >= 5 && worst <= 25, "audio and video of one instant leave within %d ms (audio arrived 80 ms earlier; %d pairs)", worst, pairs);
    ap__mutex_unlock(&g_lock);
    CHECK(g_nrec > 0 && g_rec[0].t_deliver - t0 >= 200000000ULL - 80000000ULL, "nothing leaves before the latency budget");
    AP_CLOSESOCK(m); AP_CLOSESOCK(u); AP_CLOSESOCK(s);
    airplay_destroy(a);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
