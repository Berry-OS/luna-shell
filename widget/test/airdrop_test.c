/* Tests for airdrop.h: inflate against the system gzip, cpio parsing (incl. path traversal),
 * and a full sender -> receiver transfer over loopback TLS.
 *   cc -O1 -pthread test/airdrop_test.c -o airdrop_test -ldl && ./airdrop_test */
#define AIRDROP_IMPLEMENTATION
#include "../airdrop.h"
#include <unistd.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); char nm_[160]; snprintf(nm_, sizeof(nm_), __VA_ARGS__); printf("%-62s %s\n", nm_, ok_ ? "ok" : "FAIL"); fflush(stdout); if (!ok_) fails++; } while (0)

typedef struct { FILE* f; } fsrc;
static int rd_file(void* u, uint8_t* b, size_t n) { return (int)fread(b, 1, n, ((fsrc*)u)->f); }
typedef struct { uint8_t* d; size_t n, cap; } mem;
static int sink_mem(void* u, const uint8_t* b, size_t n) { mem* m = (mem*)u; if (m->n + n > m->cap) { m->cap = (m->n + n) * 2; m->d = realloc(m->d, m->cap); } memcpy(m->d + m->n, b, n); m->n += n; return 0; }

static void write_file(const char* p, const uint8_t* d, size_t n) { FILE* f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }
static uint8_t* read_file(const char* p, size_t* n) { FILE* f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t* b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b; }

static int g_ask_result = 1; static char g_ask_sender[96]; static int g_ask_nfiles; static char g_ask_file0[256]; static int g_received, g_done_ok;
static int on_ask(void* u, const airdrop_ask* a) { (void)u; snprintf(g_ask_sender, sizeof(g_ask_sender), "%s", a->sender_name); g_ask_nfiles = a->nfiles; if (a->nfiles) snprintf(g_ask_file0, sizeof(g_ask_file0), "%s", a->files[0].name); return g_ask_result; }
static void on_recv(void* u, const char* p) { (void)u; (void)p; g_received++; }
static void on_done(void* u, int ok, int sending) { (void)u; if (!sending) g_done_ok = ok; }

int main(void) {
    ad__net_init();
    char tmpl[] = "/tmp/airdrop_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    char path[512], cmd[1024];

    /* ---- inflate vs the system gzip: stored / fixed / dynamic blocks ---- */
    {
        size_t sizes[] = { 0, 1, 100, 5000, 70000, 300000 };
        for (int kind = 0; kind < 3; kind++) {
            for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
                size_t n = sizes[si];
                uint8_t* d = malloc(n + 1);
                uint32_t x = 12345u + (uint32_t)si;
                for (size_t i = 0; i < n; i++) {
                    x = x * 1664525u + 1013904223u;
                    d[i] = kind == 0 ? (uint8_t)(x >> 24)                                  /* incompressible */
                         : kind == 1 ? (uint8_t)("hello airdrop world "[i % 20])            /* very repetitive */
                         : (uint8_t)("abcdefghij klmnop\n"[(x >> 28) % 18]);                /* skewed text */
                }
                snprintf(path, sizeof(path), "%s/in.bin", dir); write_file(path, d, n);
                snprintf(cmd, sizeof(cmd), "gzip -c %s %s/in.bin > %s/in.gz", kind == 2 ? "-9" : "-1", dir, dir); system(cmd);
                snprintf(path, sizeof(path), "%s/in.gz", dir);
                fsrc s = { fopen(path, "rb") }; mem m = { 0 };
                int rc = ad__gunzip(rd_file, &s, sink_mem, &m);
                fclose(s.f);
                int ok = rc == 0 && m.n == n && (n == 0 || !memcmp(m.d, d, n));
                if (!ok || si == 0 || si == 5) CHECK(ok, "gunzip kind=%d size=%zu", kind, n);
                if (!ok) { free(m.d); free(d); goto inflate_done; }
                free(m.d); free(d);
            }
        }
        printf("%-62s ok\n", "gunzip: 18 size/kind combinations match the system gzip");
        /* a corrupted CRC must be rejected */
        snprintf(path, sizeof(path), "%s/in.gz", dir);
        size_t gn; uint8_t* gz = read_file(path, &gn); gz[gn - 6] ^= 0x55; write_file(path, gz, gn); free(gz);
        fsrc s = { fopen(path, "rb") }; mem m = { 0 };
        CHECK(ad__gunzip(rd_file, &s, sink_mem, &m) != 0, "gunzip rejects a bad CRC");
        fclose(s.f); free(m.d);
    }
inflate_done:;

    /* ---- cpio: odc + newc, padding, traversal ---- */
    {
        char dst[600]; snprintf(dst, sizeof(dst), "%s/cp", dir); ad__mkdirs(dst);
        /* newc archive from the system cpio when available */
        snprintf(cmd, sizeof(cmd), "cd %s && mkdir -p src && printf 'first file\\n' > src/a.txt && printf 'x' > src/b.bin && mkdir -p src/sub && printf 'nested!' > src/sub/c.txt && cd src && printf 'a.txt\\nb.bin\\nsub\\nsub/c.txt\\n' | cpio -o -H newc > ../n.cpio 2>/dev/null", dir);
        system(cmd);
        snprintf(path, sizeof(path), "%s/n.cpio", dir);
        size_t cn; uint8_t* cb = read_file(path, &cn);
        if (cb && cn > 100) {
            airdrop_t fake; memset(&fake, 0, sizeof(fake));
            ad__cpio* c = calloc(1, sizeof(*c)); c->ad = &fake; snprintf(c->dir, sizeof(c->dir), "%s", dst); c->max_bytes = 1 << 20;
            int rc = 0; for (size_t i = 0; i < cn && !rc; i += 7) rc = ad__cpio_feed(c, cb + i, cn - i < 7 ? cn - i : 7);   /* odd 7-byte feeds exercise every boundary */
            size_t an, bn, cn2; snprintf(path, sizeof(path), "%s/a.txt", dst); uint8_t* a = read_file(path, &an);
            snprintf(path, sizeof(path), "%s/sub/c.txt", dst); uint8_t* c2 = read_file(path, &cn2);
            snprintf(path, sizeof(path), "%s/b.bin", dst); uint8_t* b = read_file(path, &bn);
            CHECK(rc == 0 && c->state == AD_CP_END && a && an == 11 && !memcmp(a, "first file\n", 11) && b && bn == 1 && c2 && cn2 == 7 && !memcmp(c2, "nested!", 7), "cpio newc (system cpio): files, dirs, padding");
            free(a); free(b); free(c2); free(c);
        } else printf("%-62s skipped (no cpio)\n", "cpio newc");
        free(cb);
        /* traversal attempt through the odc writer */
        char evil_dir[700]; snprintf(evil_dir, sizeof(evil_dir), "%s/jail", dir); ad__mkdirs(evil_dir);
        mem arc = { 0 };
        const char* names[] = { "../escaped.txt", "/abs.txt", "ok/../../esc2.txt", "good.txt" };
        for (int i = 0; i < 4; i++) {
            char h[80]; size_t nl = strlen(names[i]) + 1;
            int k = snprintf(h, sizeof(h), "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011llo", 0, i + 1, 0100644, 0, 0, 1, 0, 0, (unsigned)nl, 4ULL);
            sink_mem(&arc, (uint8_t*)h, k); sink_mem(&arc, (const uint8_t*)names[i], nl); sink_mem(&arc, (const uint8_t*)"DATA", 4);
        }
        { char h[80]; int k = snprintf(h, sizeof(h), "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o", 0, 0, 0, 0, 0, 1, 0, 0, 11u, 0u); sink_mem(&arc, (uint8_t*)h, k); sink_mem(&arc, (const uint8_t*)"TRAILER!!!", 11); }
        airdrop_t fake; memset(&fake, 0, sizeof(fake));
        ad__cpio* c = calloc(1, sizeof(*c)); c->ad = &fake; snprintf(c->dir, sizeof(c->dir), "%s", evil_dir); c->max_bytes = 1 << 20;
        int rc = ad__cpio_feed(c, arc.d, arc.n);
        char p1[800], p2[800], p3[800], p4[800];
        snprintf(p1, sizeof(p1), "%s/escaped.txt", dir); snprintf(p2, sizeof(p2), "/abs.txt"); snprintf(p3, sizeof(p3), "%s/esc2.txt", dir); snprintf(p4, sizeof(p4), "%s/good.txt", evil_dir);
        CHECK(rc == 0 && !ad__exists(p1) && !ad__exists(p2) && !ad__exists(p3), "cpio: ../, absolute and nested .. paths are not written");
        CHECK(ad__exists(p4), "cpio: a safe name in the same archive still is");
        free(c); free(arc.d);
    }

    /* ---- full transfer over loopback TLS ---- */
    {
        char rdir[600]; snprintf(rdir, sizeof(rdir), "%s/inbox", dir);
        airdrop_callbacks rcb; memset(&rcb, 0, sizeof(rcb)); rcb.on_ask = on_ask; rcb.on_received = on_recv; rcb.on_done = on_done;
        airdrop_config rc; memset(&rc, 0, sizeof(rc)); rc.name = "Receiver Luna"; rc.port = 18771; rc.download_dir = rdir; rc.discoverable = 1;
        airdrop_t* recv_ad = airdrop_create(&rc, &rcb);
        int st = airdrop_start(recv_ad);
        CHECK(st == 0, "receiver starts (TLS identity + dual-stack listener)%s%s", st ? ": " : "", st ? airdrop_last_error(recv_ad) : "");
        if (st) { printf("%s (%d failures)\n", "FAILED", ++fails); return 1; }
        airdrop_config sc; memset(&sc, 0, sizeof(sc)); sc.name = "Sender Luna"; sc.port = 0; sc.download_dir = dir;
        airdrop_t* send_ad = airdrop_create(&sc, NULL);
        CHECK(airdrop_start(send_ad) == 0, "sender starts");
        char nm[96] = "";
        CHECK(airdrop_discover(send_ad, "::1", airdrop_port(recv_ad), nm, sizeof(nm)) == 0 && !strcmp(nm, "Receiver Luna"), "/Discover returns the receiver name (IPv6 ::1)");
        CHECK(airdrop_discover(send_ad, "127.0.0.1", airdrop_port(recv_ad), nm, sizeof(nm)) == 0, "/Discover over IPv4 (dual-stack listener)");
        /* files: 1 byte, a multi-chunk one, and an odd-sized one */
        uint8_t* big = malloc(200000 + 3); uint32_t x = 7; for (int i = 0; i < 200003; i++) { x = x * 1103515245u + 12345u; big[i] = (uint8_t)(x >> 16); }
        char f1[600], f2[600], f3[600];
        snprintf(f1, sizeof(f1), "%s/small.txt", dir); write_file(f1, (const uint8_t*)"hi", 2);
        snprintf(f2, sizeof(f2), "%s/photo.jpg", dir); write_file(f2, big, 200003);
        snprintf(f3, sizeof(f3), "%s/empty.dat", dir); write_file(f3, (const uint8_t*)"", 0);
        const char* files[] = { f1, f2, f3 };
        char err[200];
        int r = airdrop_send(send_ad, "::1", airdrop_port(recv_ad), files, 3, err, sizeof(err));
        CHECK(r == 0, "airdrop_send 3 files -> 0%s%s", r ? ": " : "", r ? err : "");
        usleep(200000);
        CHECK(g_received == 3 && g_done_ok, "receiver reports 3 files, transfer ok");
        CHECK(!strcmp(g_ask_sender, "Sender Luna") && g_ask_nfiles == 3 && !strcmp(g_ask_file0, "small.txt"), "on_ask saw sender name and file list");
        size_t n1, n2, n3; char p[700];
        snprintf(p, sizeof(p), "%s/small.txt", rdir); uint8_t* a = read_file(p, &n1);
        snprintf(p, sizeof(p), "%s/photo.jpg", rdir); uint8_t* b = read_file(p, &n2);
        snprintf(p, sizeof(p), "%s/empty.dat", rdir); uint8_t* c = read_file(p, &n3);
        CHECK(a && n1 == 2 && !memcmp(a, "hi", 2) && b && n2 == 200003 && !memcmp(b, big, 200003) && c && n3 == 0, "received files are byte-identical");
        /* second send of the same name must not overwrite */
        g_received = 0;
        r = airdrop_send(send_ad, "::1", airdrop_port(recv_ad), files, 1, err, sizeof(err));
        snprintf(p, sizeof(p), "%s/small (1).txt", rdir);
        CHECK(r == 0 && ad__exists(p), "duplicate names get a numeric suffix");
        /* decline */
        g_ask_result = 0;
        r = airdrop_send(send_ad, "::1", airdrop_port(recv_ad), files, 1, err, sizeof(err));
        CHECK(r != 0 && strstr(err, "declined"), "declined transfer is reported (%s)", err);
        g_ask_result = 1;
        free(a); free(b); free(c); free(big);
        airdrop_destroy(send_ad); airdrop_destroy(recv_ad);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); system(cmd);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
