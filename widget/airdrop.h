#ifndef AIRDROP_H
#define AIRDROP_H
/*
 * airdrop.h — AirDrop-compatible file transfer as a single-header C library
 *
 *   #define AIRDROP_IMPLEMENTATION      // in exactly one translation unit
 *   #include "airdrop.h"
 *
 * Portable (Linux / macOS / Windows); the only external piece is OpenSSL
 * (libssl / libcrypto), loaded at run time with dlopen/LoadLibrary — nothing
 * to link, and a clear error if it is missing.  Link -pthread (-ldl on old
 * glibc, ws2_32 + iphlpapi on Windows).
 *
 * What this speaks is the OpenDrop-style AirDrop protocol: mDNS service
 * "_airdrop._tcp", HTTPS POST /Discover, /Ask, /Upload with gzip'd cpio.
 * Apple devices only reach you over AWDL, which is not a standard network
 * interface: on Linux run OWL (https://github.com/seemoo-lab/owl) and pass
 * interface_name = "awdl0"; macOS has native AirDrop.  Over an ordinary LAN
 * interface (interface_name = NULL) two instances of this library — or
 * OpenDrop — find each other, but an iPhone will not.  Receiving works in
 * "Everyone" mode only (no Apple ID validation records).
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct airdrop airdrop_t;

typedef struct airdrop_peer {
    char id[32];             /* mDNS instance (12 hex chars) */
    char name[96];           /* display name, filled in after /Discover when known */
    char host[80];           /* numeric address, scope included ("fe80::1%awdl0") */
    int  port;
    uint32_t flags;
} airdrop_peer;

typedef struct airdrop_file {
    char name[256];
    char type[64];           /* UTI, e.g. "public.jpeg" */
    int  is_dir;
} airdrop_file;

typedef struct airdrop_ask {
    char sender_name[96], sender_model[64], sender_id[32];
    int nfiles; const airdrop_file* files;
    int nurls;  const char* const* urls;       /* a shared link instead of files */
    const uint8_t* icon; size_t icon_n;        /* JPEG/PNG preview, may be NULL */
} airdrop_ask;

/* Callbacks run on worker threads.  on_ask may block (e.g. for a prompt); return 1 to accept. */
typedef struct airdrop_callbacks {
    void* user;
    void (*on_log)(void* user, int level /*0 info,1 warn*/, const char* msg);
    void (*on_peer)(void* user, int added, const airdrop_peer* peer);
    int  (*on_ask)(void* user, const airdrop_ask* ask);
    void (*on_progress)(void* user, const char* name, uint64_t done, int sending);
    void (*on_received)(void* user, const char* path);
    void (*on_done)(void* user, int ok, int sending);
} airdrop_callbacks;

typedef struct airdrop_config {
    const char* name;            /* shown to others ("Luna Desktop") */
    const char* model;           /* "OpenDrop"-style model string, default "Luna" */
    const char* interface_name;  /* "awdl0" for real AirDrop, NULL = any LAN interface */
    int port;                    /* 0 = 8771 (next free if taken) */
    const char* download_dir;    /* where received files go (default "./") */
    int discoverable;            /* answer /Discover and advertise (default 1 when receiving) */
    uint64_t max_bytes;          /* refuse uploads bigger than this (0 = 16 GiB) */
} airdrop_config;

airdrop_t* airdrop_create(const airdrop_config* cfg, const airdrop_callbacks* cb);
int        airdrop_start(airdrop_t* a);              /* 0 = ok; <0: see airdrop_last_error */
void       airdrop_stop(airdrop_t* a);
void       airdrop_destroy(airdrop_t* a);
int        airdrop_port(const airdrop_t* a);
const char* airdrop_last_error(const airdrop_t* a);

/* Blocking: ask the receiver at host:port, then upload the files.  0 = delivered. */
int airdrop_send(airdrop_t* a, const char* host, int port, const char* const* paths, int npaths, char* err, size_t err_n);
/* Peers found by mDNS browsing (copied out); returns how many. */
int airdrop_peers(airdrop_t* a, airdrop_peer* out, int max);
/* Resolve a peer's display name through /Discover (blocking). */
int airdrop_discover(airdrop_t* a, const char* host, int port, char* name, size_t name_n);

#ifdef __cplusplus
}
#endif
#endif /* AIRDROP_H */

/* ======================================================================== */
#ifdef AIRDROP_IMPLEMENTATION
#ifndef AIRDROP_IMPLEMENTATION_ONCE
#define AIRDROP_IMPLEMENTATION_ONCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp
#  include <direct.h>
#else
#  include <strings.h>
#  include <dirent.h>
#  include <dlfcn.h>
#  include <net/if.h>
#  include <netdb.h>
#endif

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
typedef SOCKET ad_sock;
typedef HANDLE ad_thread;
typedef CRITICAL_SECTION ad_mutex;
#  define AD_BADSOCK INVALID_SOCKET
#  define AD_THREAD_RET DWORD WINAPI
#  define AD_CLOSESOCK(s) closesocket(s)
static void ad__mutex_init(ad_mutex* m) { InitializeCriticalSection(m); }
static void ad__mutex_lock(ad_mutex* m) { EnterCriticalSection(m); }
static void ad__mutex_unlock(ad_mutex* m) { LeaveCriticalSection(m); }
static void ad__mutex_destroy(ad_mutex* m) { DeleteCriticalSection(m); }
static int ad__thread_start(ad_thread* t, DWORD (WINAPI *fn)(LPVOID), void* arg) { *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
static void ad__thread_join(ad_thread t) { if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
static uint64_t ad__now_ns(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart); }
static void ad__net_init(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
static void ad__set_timeout(ad_sock s, int ms) { DWORD t = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t)); }
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
typedef int ad_sock;
typedef pthread_t ad_thread;
typedef pthread_mutex_t ad_mutex;
#  define AD_BADSOCK (-1)
#  define AD_THREAD_RET void*
#  define AD_CLOSESOCK(s) close(s)
static void ad__mutex_init(ad_mutex* m) { pthread_mutex_init(m, NULL); }
static void ad__mutex_lock(ad_mutex* m) { pthread_mutex_lock(m); }
static void ad__mutex_unlock(ad_mutex* m) { pthread_mutex_unlock(m); }
static void ad__mutex_destroy(ad_mutex* m) { pthread_mutex_destroy(m); }
static int ad__thread_start(ad_thread* t, void* (*fn)(void*), void* arg) { return pthread_create(t, NULL, fn, arg); }
static void ad__thread_join(ad_thread t) { pthread_join(t, NULL); }
static uint64_t ad__now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec; }
static void ad__net_init(void) {}
static void ad__set_timeout(ad_sock s, int ms) { struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
#endif

static void ad__shutdown_sock(ad_sock s) {
    if (s == AD_BADSOCK) return;
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
}

static void ad__put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t ad__get32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t ad__get32le(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint64_t ad__get64le(const uint8_t* p) { return (uint64_t)ad__get32le(p) | ((uint64_t)ad__get32le(p + 4) << 32); }
static uint16_t ad__get16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void ad__random(uint8_t* out, size_t n) {
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
    uint64_t t = ad__now_ns() ^ (uint64_t)(uintptr_t)out;
    for (size_t i = 0; i < n; i++) { t = t * 6364136223846793005ULL + 1442695040888963407ULL; out[i] = (uint8_t)(t >> 33); }
}

/* Primary IPv4 address (host byte order) found by asking the routing table. */
static int ad__local_ipv4(uint8_t ip[4]) {
    ad_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == AD_BADSOCK) return -1;
    struct sockaddr_in to, me;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET; to.sin_port = htons(9); to.sin_addr.s_addr = htonl(0xE00000FBu);   /* 224.0.0.251 */
    int ok = -1;
    if (connect(s, (struct sockaddr*)&to, sizeof(to)) == 0) {
        socklen_t l = sizeof(me);
        if (getsockname(s, (struct sockaddr*)&me, &l) == 0 && me.sin_addr.s_addr != 0) { memcpy(ip, &me.sin_addr.s_addr, 4); ok = 0; }
    }
    AD_CLOSESOCK(s);
    if (ok != 0) { ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1; }
    return ok;
}

static ad_sock ad__tcp_listen(int port, int* bound_port) {
    ad_sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == AD_BADSOCK) return AD_BADSOCK;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 8) != 0) { AD_CLOSESOCK(s); return AD_BADSOCK; }
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &l);
    if (bound_port) *bound_port = ntohs(a.sin_port);
    return s;
}
static ad_sock ad__udp_bind(int port, int* bound_port) {
    ad_sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == AD_BADSOCK) return AD_BADSOCK;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { AD_CLOSESOCK(s); return AD_BADSOCK; }
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &l);
    if (bound_port) *bound_port = ntohs(a.sin_port);
    return s;
}
static int ad__recv_all(ad_sock s, uint8_t* buf, size_t n) {
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
static int ad__send_all(ad_sock s, const uint8_t* buf, size_t n) {
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
enum { AD_PL_NULL, AD_PL_BOOL, AD_PL_INT, AD_PL_REAL, AD_PL_DATA, AD_PL_STR, AD_PL_ARRAY, AD_PL_DICT };
typedef struct ad_pl {
    int t;
    int64_t i;                 /* bool / int */
    double r;
    uint8_t* d; size_t n;      /* data / string (string is NUL-terminated, n excludes NUL) */
    struct ad_pl** v;          /* array items / dict values */
    char** k;                  /* dict keys */
    size_t cnt, cap;
} ad_pl;

static ad_pl* ad_pl_new(int t) { ad_pl* p = (ad_pl*)calloc(1, sizeof(*p)); if (p) p->t = t; return p; }
static void ad_pl_free(ad_pl* p) {
    if (!p) return;
    for (size_t i = 0; i < p->cnt; i++) { ad_pl_free(p->v[i]); if (p->k) free(p->k[i]); }
    free(p->v); free(p->k); free(p->d); free(p);
}
static ad_pl* ad_pl_int(int64_t i) { ad_pl* p = ad_pl_new(AD_PL_INT); if (p) p->i = i; return p; }
static ad_pl* ad_pl_bool(int b) { ad_pl* p = ad_pl_new(AD_PL_BOOL); if (p) p->i = b != 0; return p; }
static ad_pl* ad_pl_real(double r) { ad_pl* p = ad_pl_new(AD_PL_REAL); if (p) p->r = r; return p; }
static ad_pl* ad_pl_str(const char* s) {
    ad_pl* p = ad_pl_new(AD_PL_STR);
    if (!p) return NULL;
    p->n = strlen(s); p->d = (uint8_t*)malloc(p->n + 1);
    if (p->d) memcpy(p->d, s, p->n + 1);
    return p;
}
static ad_pl* ad_pl_data(const void* d, size_t n) {
    ad_pl* p = ad_pl_new(AD_PL_DATA);
    if (!p) return NULL;
    p->n = n; p->d = (uint8_t*)malloc(n ? n : 1);
    if (p->d && n) memcpy(p->d, d, n);
    return p;
}
static int ad__pl_grow(ad_pl* p) {
    if (p->cnt < p->cap) return 0;
    size_t nc = p->cap ? p->cap * 2 : 8;
    ad_pl** nv = (ad_pl**)realloc(p->v, nc * sizeof(*nv));
    if (!nv) return -1;
    p->v = nv;
    if (p->t == AD_PL_DICT) {
        char** nk = (char**)realloc(p->k, nc * sizeof(*nk));
        if (!nk) return -1;
        p->k = nk;
    }
    p->cap = nc;
    return 0;
}
static void ad_pl_push(ad_pl* arr, ad_pl* v) { if (arr && v && ad__pl_grow(arr) == 0) arr->v[arr->cnt++] = v; }
static void ad_pl_set(ad_pl* dict, const char* key, ad_pl* v) {
    if (!dict || !v || ad__pl_grow(dict) != 0) return;
    size_t kl = strlen(key);
    char* k = (char*)malloc(kl + 1);
    if (!k) return;
    memcpy(k, key, kl + 1);
    dict->k[dict->cnt] = k; dict->v[dict->cnt] = v; dict->cnt++;
}
static ad_pl* ad_pl_get(const ad_pl* dict, const char* key) {
    if (!dict || dict->t != AD_PL_DICT) return NULL;
    for (size_t i = 0; i < dict->cnt; i++) if (!strcmp(dict->k[i], key)) return dict->v[i];
    return NULL;
}

/* -- parse -- */
typedef struct { const uint8_t* b; size_t n; size_t off_size, ref_size, num_obj; size_t table; int depth; } ad__plctx;
static uint64_t ad__pl_be(const uint8_t* p, size_t n) { uint64_t v = 0; for (size_t i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }
static ad_pl* ad__pl_obj(ad__plctx* c, size_t idx);
static int ad__pl_len(ad__plctx* c, size_t* pos, uint8_t info, size_t* len) {
    if (info != 0xF) { *len = info; return 0; }
    if (*pos >= c->n) return -1;
    uint8_t m = c->b[(*pos)++];
    if ((m >> 4) != 1) return -1;
    size_t sz = (size_t)1 << (m & 15);
    if (sz > 8 || *pos + sz > c->n) return -1;
    *len = (size_t)ad__pl_be(c->b + *pos, sz);
    *pos += sz;
    return 0;
}
static ad_pl* ad__pl_obj(ad__plctx* c, size_t idx) {
    if (idx >= c->num_obj || ++c->depth > 32) { c->depth--; return NULL; }
    size_t off_at = c->table + idx * c->off_size;
    if (off_at + c->off_size > c->n) { c->depth--; return NULL; }
    size_t pos = (size_t)ad__pl_be(c->b + off_at, c->off_size);
    ad_pl* r = NULL;
    if (pos >= c->n) { c->depth--; return NULL; }
    uint8_t m = c->b[pos++];
    uint8_t hi = m >> 4, lo = m & 15;
    size_t len;
    switch (hi) {
    case 0x0:
        if (m == 0x08) r = ad_pl_bool(0); else if (m == 0x09) r = ad_pl_bool(1); else r = ad_pl_new(AD_PL_NULL);
        break;
    case 0x1: {
        size_t sz = (size_t)1 << lo;
        if (sz > 8 || pos + sz > c->n) break;
        r = ad_pl_int((int64_t)ad__pl_be(c->b + pos, sz));
        break; }
    case 0x2: {
        size_t sz = (size_t)1 << lo;
        if ((sz != 4 && sz != 8) || pos + sz > c->n) break;
        if (sz == 4) { uint32_t u = (uint32_t)ad__pl_be(c->b + pos, 4); float f; memcpy(&f, &u, 4); r = ad_pl_real(f); }
        else { uint64_t u = ad__pl_be(c->b + pos, 8); double d; memcpy(&d, &u, 8); r = ad_pl_real(d); }
        break; }
    case 0x4:
        if (ad__pl_len(c, &pos, lo, &len) || pos + len > c->n) break;
        r = ad_pl_data(c->b + pos, len);
        break;
    case 0x5: case 0x6: {
        if (ad__pl_len(c, &pos, lo, &len)) break;
        size_t bytes = hi == 0x6 ? len * 2 : len;
        if (pos + bytes > c->n) break;
        if (hi == 0x5) { char* s = (char*)malloc(len + 1); if (!s) break; memcpy(s, c->b + pos, len); s[len] = 0; r = ad_pl_str(s); free(s); }
        else { char* s = (char*)malloc(len + 1); if (!s) break; for (size_t i = 0; i < len; i++) { uint16_t u = ad__get16be(c->b + pos + 2 * i); s[i] = u < 128 ? (char)u : '?'; } s[len] = 0; r = ad_pl_str(s); free(s); }
        break; }
    case 0xA: case 0xD: {
        if (ad__pl_len(c, &pos, lo, &len)) break;
        size_t groups = hi == 0xD ? 2 : 1;
        if (len > c->num_obj || pos + len * groups * c->ref_size > c->n) break;
        r = ad_pl_new(hi == 0xD ? AD_PL_DICT : AD_PL_ARRAY);
        if (!r) break;
        for (size_t i = 0; i < len; i++) {
            size_t kref = hi == 0xD ? (size_t)ad__pl_be(c->b + pos + i * c->ref_size, c->ref_size) : 0;
            size_t vref = (size_t)ad__pl_be(c->b + pos + (hi == 0xD ? (len + i) : i) * c->ref_size, c->ref_size);
            ad_pl* v = ad__pl_obj(c, vref);
            if (!v) { ad_pl_free(r); r = NULL; break; }
            if (hi == 0xD) {
                ad_pl* k = ad__pl_obj(c, kref);
                if (!k || k->t != AD_PL_STR) { ad_pl_free(k); ad_pl_free(v); ad_pl_free(r); r = NULL; break; }
                ad_pl_set(r, (const char*)k->d, v);
                ad_pl_free(k);
            } else ad_pl_push(r, v);
        }
        break; }
    default: break;
    }
    c->depth--;
    return r;
}
static ad_pl* ad_pl_parse(const uint8_t* b, size_t n) {
    if (n < 8 + 32 || memcmp(b, "bplist00", 8)) return NULL;
    const uint8_t* t = b + n - 32;
    ad__plctx c;
    c.b = b; c.n = n; c.depth = 0;
    c.off_size = t[6]; c.ref_size = t[7];
    c.num_obj = (size_t)ad__pl_be(t + 8, 8);
    size_t top = (size_t)ad__pl_be(t + 16, 8);
    c.table = (size_t)ad__pl_be(t + 24, 8);
    if (!c.off_size || c.off_size > 8 || !c.ref_size || c.ref_size > 8 || !c.num_obj) return NULL;
    if (c.table >= n || c.num_obj > (n - c.table) / c.off_size) return NULL;
    return ad__pl_obj(&c, top);
}

/* -- write -- */
typedef struct { uint8_t* b; size_t n, cap; } ad__buf;
static int ad__buf_put(ad__buf* o, const void* d, size_t n) {
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
static void ad__buf_be(ad__buf* o, uint64_t v, size_t sz) { uint8_t t[8]; for (size_t i = 0; i < sz; i++) t[i] = (uint8_t)(v >> (8 * (sz - 1 - i))); ad__buf_put(o, t, sz); }
static size_t ad__pl_count(const ad_pl* p) {
    size_t c = 1;
    for (size_t i = 0; i < p->cnt; i++) c += ad__pl_count(p->v[i]) + (p->t == AD_PL_DICT ? 1 : 0);
    return c;
}
/* Serialize with object order: each node, then (for dicts) interleaved key objects. */
typedef struct { ad__buf out; size_t* offs; size_t cnt, ref; } ad__plw;
static void ad__pl_hdr(ad__buf* o, uint8_t type, size_t len) {
    if (len < 15) { uint8_t m = (uint8_t)(type | len); ad__buf_put(o, &m, 1); }
    else { uint8_t m = (uint8_t)(type | 0xF); ad__buf_put(o, &m, 1); uint8_t sz = 4; uint8_t im = (uint8_t)(0x10 | 2); (void)sz; ad__buf_put(o, &im, 1); ad__buf_be(o, len, 4); }
}
static size_t ad__pl_write_obj(ad__plw* w, const ad_pl* p);
static size_t ad__pl_write_str(ad__plw* w, const char* s) {
    size_t id = w->cnt++;
    w->offs[id] = w->out.n;
    size_t l = strlen(s);
    ad__pl_hdr(&w->out, 0x50, l);
    ad__buf_put(&w->out, s, l);
    return id;
}
static size_t ad__pl_write_obj(ad__plw* w, const ad_pl* p) {
    size_t kids[2048], keyids[2048];   /* AirPlay bodies are small; bound the container size */
    size_t id;
    size_t n = p->cnt < 2048 ? p->cnt : 2048;
    if (p->t == AD_PL_ARRAY || p->t == AD_PL_DICT) {
        for (size_t i = 0; i < n; i++) {
            if (p->t == AD_PL_DICT) keyids[i] = ad__pl_write_str(w, p->k[i]);
            kids[i] = ad__pl_write_obj(w, p->v[i]);
        }
    }
    id = w->cnt++;
    w->offs[id] = w->out.n;
    switch (p->t) {
    case AD_PL_BOOL: { uint8_t m = p->i ? 0x09 : 0x08; ad__buf_put(&w->out, &m, 1); break; }
    case AD_PL_INT: {
        if (p->i >= 0 && p->i < 256) { uint8_t m = 0x10; ad__buf_put(&w->out, &m, 1); ad__buf_be(&w->out, (uint64_t)p->i, 1); }
        else if (p->i >= 0 && p->i < 65536) { uint8_t m = 0x11; ad__buf_put(&w->out, &m, 1); ad__buf_be(&w->out, (uint64_t)p->i, 2); }
        else if (p->i >= 0 && p->i < 0x100000000LL) { uint8_t m = 0x12; ad__buf_put(&w->out, &m, 1); ad__buf_be(&w->out, (uint64_t)p->i, 4); }
        else { uint8_t m = 0x13; ad__buf_put(&w->out, &m, 1); ad__buf_be(&w->out, (uint64_t)p->i, 8); }
        break; }
    case AD_PL_REAL: { uint8_t m = 0x23; uint64_t u; memcpy(&u, &p->r, 8); ad__buf_put(&w->out, &m, 1); ad__buf_be(&w->out, u, 8); break; }
    case AD_PL_DATA: ad__pl_hdr(&w->out, 0x40, p->n); ad__buf_put(&w->out, p->d, p->n); break;
    case AD_PL_STR: ad__pl_hdr(&w->out, 0x50, p->n); ad__buf_put(&w->out, p->d, p->n); break;
    case AD_PL_ARRAY:
        ad__pl_hdr(&w->out, 0xA0, n);
        for (size_t i = 0; i < n; i++) ad__buf_be(&w->out, kids[i], w->ref);
        break;
    case AD_PL_DICT:
        ad__pl_hdr(&w->out, 0xD0, n);
        for (size_t i = 0; i < n; i++) ad__buf_be(&w->out, keyids[i], w->ref);
        for (size_t i = 0; i < n; i++) ad__buf_be(&w->out, kids[i], w->ref);
        break;
    default: { uint8_t m = 0x00; ad__buf_put(&w->out, &m, 1); break; }
    }
    return id;
}
/* Returns malloc'd bplist00 bytes; the top object is written last and referenced from the trailer. */
static uint8_t* ad_pl_serialize(const ad_pl* root, size_t* out_n) {
    ad__plw w;
    size_t total = ad__pl_count(root);
    memset(&w, 0, sizeof(w));
    w.ref = total < 256 ? 1 : 2;
    w.offs = (size_t*)calloc(total + 8, sizeof(size_t));
    if (!w.offs) return NULL;
    ad__buf_put(&w.out, "bplist00", 8);
    size_t top = ad__pl_write_obj(&w, root);
    size_t table = w.out.n;
    for (size_t i = 0; i < w.cnt; i++) ad__buf_be(&w.out, w.offs[i], 4);
    uint8_t trailer[32] = { 0 };
    trailer[6] = 4; trailer[7] = (uint8_t)w.ref;
    for (int i = 0; i < 8; i++) { trailer[8 + i] = (uint8_t)((uint64_t)w.cnt >> (8 * (7 - i))); trailer[16 + i] = (uint8_t)((uint64_t)top >> (8 * (7 - i))); trailer[24 + i] = (uint8_t)((uint64_t)table >> (8 * (7 - i))); }
    ad__buf_put(&w.out, trailer, 32);
    free(w.offs);
    *out_n = w.out.n;
    return w.out.b;
}


/* ====================================================================== */
/* TLS through OpenSSL, loaded at run time                                  */
/* ====================================================================== */
typedef struct {
    int ready; void* h_ssl; void* h_cry;
    const void* (*TLS_server_method)(void); const void* (*TLS_client_method)(void);
    void* (*SSL_CTX_new)(const void*); void (*SSL_CTX_free)(void*);
    int (*SSL_CTX_use_certificate)(void*, void*); int (*SSL_CTX_use_PrivateKey)(void*, void*);
    void (*SSL_CTX_set_verify)(void*, int, void*);
    void* (*SSL_new)(void*); void (*SSL_free)(void*); int (*SSL_set_fd)(void*, int);
    int (*SSL_accept)(void*); int (*SSL_connect)(void*);
    int (*SSL_read)(void*, void*, int); int (*SSL_write)(void*, const void*, int);
    int (*SSL_shutdown)(void*); int (*SSL_get_error)(const void*, int);
    void* (*RSA_new)(void); void* (*BN_new)(void); int (*BN_set_word)(void*, unsigned long);
    int (*RSA_generate_key_ex)(void*, int, void*, void*); void (*BN_free)(void*);
    void* (*EVP_PKEY_new)(void); int (*EVP_PKEY_assign)(void*, int, void*); void (*EVP_PKEY_free)(void*);
    void* (*X509_new)(void); void (*X509_free)(void*); int (*X509_set_version)(void*, long);
    void* (*X509_get_serialNumber)(void*); int (*ASN1_INTEGER_set)(void*, long);
    void* (*X509_getm_notBefore)(const void*); void* (*X509_getm_notAfter)(const void*);
    void* (*X509_gmtime_adj)(void*, long); int (*X509_set_pubkey)(void*, void*);
    void* (*X509_get_subject_name)(const void*);
    int (*X509_NAME_add_entry_by_txt)(void*, const char*, int, const unsigned char*, int, int, int);
    int (*X509_set_issuer_name)(void*, const void*); int (*X509_sign)(void*, void*, const void*);
    const void* (*EVP_sha256)(void);
} ad__tls_api;
static ad__tls_api ad__T;

static void* ad__dlopen_any(const char* const* names) {
    for (int i = 0; names[i]; i++) {
#ifdef _WIN32
        void* h = (void*)LoadLibraryA(names[i]);
#else
        void* h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
#endif
        if (h) return h;
    }
    return NULL;
}
static void* ad__dlsym(void* h, const char* n) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)h, n);
#else
    return dlsym(h, n);
#endif
}
#define AD_BIND(h, name) do { void* p_ = ad__dlsym(h, #name); if (!p_) return -1; memcpy((void*)&ad__T.name, &p_, sizeof(p_)); } while (0)

static int ad__tls_load(char* err, size_t en) {
    if (ad__T.ready) return 0;
#ifdef _WIN32
    static const char* const ssl_names[] = { "libssl-3-x64.dll", "libssl-3.dll", "libssl-1_1-x64.dll", "libssl-1_1.dll", NULL };
    static const char* const cry_names[] = { "libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto-1_1-x64.dll", "libcrypto-1_1.dll", NULL };
#elif defined(__APPLE__)
    static const char* const ssl_names[] = { "/opt/homebrew/lib/libssl.3.dylib", "/usr/local/lib/libssl.3.dylib", "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib", "libssl.3.dylib", NULL };
    static const char* const cry_names[] = { "/opt/homebrew/lib/libcrypto.3.dylib", "/usr/local/lib/libcrypto.3.dylib", "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib", "libcrypto.3.dylib", NULL };
#else
    static const char* const ssl_names[] = { "libssl.so.3", "libssl.so.1.1", "libssl.so", NULL };
    static const char* const cry_names[] = { "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", NULL };
#endif
    ad__T.h_cry = ad__dlopen_any(cry_names);
    ad__T.h_ssl = ad__dlopen_any(ssl_names);
    if (!ad__T.h_ssl || !ad__T.h_cry) { snprintf(err, en, "OpenSSL (libssl/libcrypto, 1.1 or 3.x) was not found"); return -1; }
    void* S = ad__T.h_ssl; void* C = ad__T.h_cry;
    int bad = 0;
    do {
        AD_BIND(S, TLS_server_method); AD_BIND(S, TLS_client_method); AD_BIND(S, SSL_CTX_new); AD_BIND(S, SSL_CTX_free);
        AD_BIND(S, SSL_CTX_use_certificate); AD_BIND(S, SSL_CTX_use_PrivateKey); AD_BIND(S, SSL_CTX_set_verify);
        AD_BIND(S, SSL_new); AD_BIND(S, SSL_free); AD_BIND(S, SSL_set_fd); AD_BIND(S, SSL_accept); AD_BIND(S, SSL_connect);
        AD_BIND(S, SSL_read); AD_BIND(S, SSL_write); AD_BIND(S, SSL_shutdown); AD_BIND(S, SSL_get_error);
        AD_BIND(C, RSA_new); AD_BIND(C, BN_new); AD_BIND(C, BN_set_word); AD_BIND(C, RSA_generate_key_ex); AD_BIND(C, BN_free);
        AD_BIND(C, EVP_PKEY_new); AD_BIND(C, EVP_PKEY_assign); AD_BIND(C, EVP_PKEY_free);
        AD_BIND(C, X509_new); AD_BIND(C, X509_free); AD_BIND(C, X509_set_version); AD_BIND(C, X509_get_serialNumber);
        AD_BIND(C, ASN1_INTEGER_set); AD_BIND(C, X509_getm_notBefore); AD_BIND(C, X509_getm_notAfter); AD_BIND(C, X509_gmtime_adj);
        AD_BIND(C, X509_set_pubkey); AD_BIND(C, X509_get_subject_name); AD_BIND(C, X509_NAME_add_entry_by_txt);
        AD_BIND(C, X509_set_issuer_name); AD_BIND(C, X509_sign); AD_BIND(C, EVP_sha256);
        bad = 0;
    } while (0);
    (void)bad;
    ad__T.ready = 1;
    return 0;
}
#undef AD_BIND

/* Self-signed RSA-2048 identity, in memory.  AirDrop peers accept self-signed certificates. */
static int ad__tls_identity(const char* cn, void** x509, void** pkey) {
    void* rsa = ad__T.RSA_new(); void* bn = ad__T.BN_new();
    if (!rsa || !bn) return -1;
    ad__T.BN_set_word(bn, 65537);
    if (ad__T.RSA_generate_key_ex(rsa, 2048, bn, NULL) != 1) return -1;
    ad__T.BN_free(bn);
    void* pk = ad__T.EVP_PKEY_new();
    if (!pk || ad__T.EVP_PKEY_assign(pk, 6 /*EVP_PKEY_RSA*/, rsa) != 1) return -1;
    void* x = ad__T.X509_new();
    if (!x) return -1;
    uint8_t r[4]; ad__random(r, 4);
    ad__T.X509_set_version(x, 2);
    ad__T.ASN1_INTEGER_set(ad__T.X509_get_serialNumber(x), (long)(((unsigned)r[0] << 16) | ((unsigned)r[1] << 8) | r[2]) + 1);
    ad__T.X509_gmtime_adj(ad__T.X509_getm_notBefore(x), -3600);
    ad__T.X509_gmtime_adj(ad__T.X509_getm_notAfter(x), 365L * 24 * 3600);
    ad__T.X509_set_pubkey(x, pk);
    void* nm = ad__T.X509_get_subject_name(x);
    ad__T.X509_NAME_add_entry_by_txt(nm, "CN", 0x1001 /*MBSTRING_UTF8*/, (const unsigned char*)cn, -1, -1, 0);
    ad__T.X509_set_issuer_name(x, nm);
    if (!ad__T.X509_sign(x, pk, ad__T.EVP_sha256())) return -1;
    *x509 = x; *pkey = pk;
    return 0;
}

/* ---- buffered connection (plain or TLS) ---- */
typedef struct {
    ad_sock s; void* ssl;
    uint8_t rb[16384]; size_t rpos, rlen;
} ad_io;

static int ad__io_raw_read(ad_io* io, uint8_t* buf, size_t n) {   /* >0 bytes, 0 eof, -1 error, -2 timeout */
    if (io->ssl) {
        int r = ad__T.SSL_read(io->ssl, buf, (int)(n > 0x40000000 ? 0x40000000 : n));
        if (r > 0) return r;
        int e = ad__T.SSL_get_error(io->ssl, r);
        if (e == 6 /*ZERO_RETURN*/) return 0;
        if (e == 2 /*WANT_READ*/ || e == 3) return -2;
#ifndef _WIN32
        if (e == 5 /*SYSCALL*/ && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
#endif
        return -1;
    }
    int r = (int)recv(io->s, (char*)buf, (int)n, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
#ifdef _WIN32
    { int e = WSAGetLastError(); if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return -2; }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
    if (errno == EINTR) return -2;
#endif
    return -1;
}
static int ad__io_read(ad_io* io, uint8_t* buf, size_t n) {
    if (io->rpos < io->rlen) {
        size_t k = io->rlen - io->rpos < n ? io->rlen - io->rpos : n;
        memcpy(buf, io->rb + io->rpos, k); io->rpos += k;
        return (int)k;
    }
    if (n >= sizeof(io->rb)) return ad__io_raw_read(io, buf, n);
    int r = ad__io_raw_read(io, io->rb, sizeof(io->rb));
    if (r <= 0) return r;
    io->rpos = 0; io->rlen = (size_t)r;
    return ad__io_read(io, buf, n);
}
static int ad__io_getc(ad_io* io) { uint8_t c; for (int tries = 0; tries < 60; ) { int r = ad__io_read(io, &c, 1); if (r == 1) return c; if (r == -2) { tries++; continue; } return -1; } return -1; }
static int ad__io_write(ad_io* io, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    size_t off = 0;
    while (off < n) {
        int w;
        if (io->ssl) {
            w = ad__T.SSL_write(io->ssl, p + off, (int)(n - off > 0x40000000 ? 0x40000000 : n - off));
            if (w <= 0) { int e = ad__T.SSL_get_error(io->ssl, w); if (e == 2 || e == 3) continue; return -1; }
        } else {
#ifdef MSG_NOSIGNAL
            w = (int)send(io->s, (const char*)p + off, (int)(n - off), MSG_NOSIGNAL);
#else
            w = (int)send(io->s, (const char*)p + off, (int)(n - off), 0);
#endif
            if (w <= 0) return -1;
        }
        off += (size_t)w;
    }
    return 0;
}
static void ad__io_close(ad_io* io) {
    if (io->ssl) { ad__T.SSL_shutdown(io->ssl); ad__T.SSL_free(io->ssl); io->ssl = NULL; }
    if (io->s != AD_BADSOCK) { AD_CLOSESOCK(io->s); io->s = AD_BADSOCK; }
}

/* ====================================================================== */
/* HTTP/1.1 (just what AirDrop needs)                                       */
/* ====================================================================== */
typedef struct {
    char method[16], path[128];
    int status;
    struct { char k[40]; char v[200]; } h[16];
    int nh;
} ad__http;
static const char* ad__h(const ad__http* m, const char* k) {
    for (int i = 0; i < m->nh; i++) if (!strcasecmp(m->h[i].k, k)) return m->h[i].v;
    return NULL;
}
/* Reads a request line / status line + headers.  0 ok, -2 idle timeout, -1 error. */
static int ad__http_head(ad_io* io, ad__http* m, int is_response) {
    char line[1024];
    int idle = 1;
    memset(m, 0, sizeof(*m));
    for (int pass = 0; ; pass++) {
        size_t n = 0;
        for (;;) {
            uint8_t c;
            int r = ad__io_read(io, &c, 1);
            if (r == -2 && idle) return -2;
            if (r == -2) continue;
            if (r <= 0) return -1;
            idle = 0;
            if (c == '\n') break;
            if (c != '\r' && n < sizeof(line) - 1) line[n++] = (char)c;
        }
        line[n] = 0;
        if (pass == 0) {
            if (is_response) { if (sscanf(line, "HTTP/%*s %d", &m->status) != 1) return -1; }
            else if (sscanf(line, "%15s %127s", m->method, m->path) != 2) return -1;
        } else if (!line[0]) return 0;
        else {
            char* colon = strchr(line, ':');
            if (colon && m->nh < 16) {
                *colon = 0;
                snprintf(m->h[m->nh].k, sizeof(m->h[0].k), "%s", line);
                char* v = colon + 1; while (*v == ' ') v++;
                snprintf(m->h[m->nh].v, sizeof(m->h[0].v), "%s", v);
                m->nh++;
            }
        }
    }
}
typedef struct { ad_io* io; int chunked; uint64_t remain; int done; } ad__body;
static void ad__body_init(ad__body* b, ad_io* io, const ad__http* m) {
    const char* te = ad__h(m, "Transfer-Encoding");
    const char* cl = ad__h(m, "Content-Length");
    memset(b, 0, sizeof(*b));
    b->io = io;
    if (te && !strcasecmp(te, "chunked")) b->chunked = 1;
    else { b->remain = cl ? strtoull(cl, NULL, 10) : 0; b->done = b->remain == 0; }
}
/* >0 bytes, 0 end of body, <0 error */
static int ad__body_read(ad__body* b, uint8_t* out, size_t n) {
    if (b->done) return 0;
    if (b->chunked && b->remain == 0) {
        char line[40]; size_t k = 0;
        for (;;) { int c = ad__io_getc(b->io); if (c < 0) return -1; if (c == '\n') break; if (c != '\r' && k < sizeof(line) - 1) line[k++] = (char)c; }
        line[k] = 0;
        if (!k) { for (;;) { int c = ad__io_getc(b->io); if (c < 0) return -1; if (c == '\n') break; } k = 0; }   /* tolerate stray CRLF */
        b->remain = strtoull(line, NULL, 16);
        if (b->remain == 0) {                                  /* last chunk: trailers up to a blank line */
            for (;;) { size_t m = 0; int c; while ((c = ad__io_getc(b->io)) >= 0 && c != '\n') m += c != '\r'; if (c < 0) return -1; if (!m) break; }
            b->done = 1; return 0;
        }
    }
    size_t want = (size_t)(b->remain < n ? b->remain : n);
    int r;
    do { r = ad__io_read(b->io, out, want); } while (r == -2);
    if (r <= 0) return -1;
    b->remain -= (uint64_t)r;
    if (b->remain == 0) {
        if (b->chunked) { int c1 = ad__io_getc(b->io), c2 = ad__io_getc(b->io); if (c1 < 0 || c2 < 0) return -1; }   /* chunk CRLF */
        else b->done = 1;
    }
    return r;
}
static int ad__body_all(ad__body* b, uint8_t** out, size_t* n, size_t max) {
    size_t cap = 4096, len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len == cap) { if (cap >= max) { free(buf); return -1; } cap *= 2; uint8_t* nb = (uint8_t*)realloc(buf, cap); if (!nb) { free(buf); return -1; } buf = nb; }
        int r = ad__body_read(b, buf + len, cap - len);
        if (r < 0) { free(buf); return -1; }
        if (r == 0) break;
        len += (size_t)r;
    }
    *out = buf; *n = len;
    return 0;
}
static int ad__send_status(ad_io* io, int code, const char* text, const char* ctype, const uint8_t* body, size_t n, int close_after) {
    char h[320];
    int k = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n", code, text, n);
    if (ctype && n) k += snprintf(h + k, sizeof(h) - (size_t)k, "Content-Type: %s\r\n", ctype);
    if (close_after) k += snprintf(h + k, sizeof(h) - (size_t)k, "Connection: close\r\n");
    k += snprintf(h + k, sizeof(h) - (size_t)k, "\r\n");
    if (ad__io_write(io, h, (size_t)k)) return -1;
    return n ? ad__io_write(io, body, n) : 0;
}

/* ====================================================================== */
/* CRC-32, inflate (RFC 1951), gzip (RFC 1952)                              */
/* ====================================================================== */
static uint32_t ad__crc_tab[256];
static int ad__crc_ready;
static uint32_t ad__crc32(uint32_t crc, const uint8_t* p, size_t n) {
    if (!ad__crc_ready) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; ad__crc_tab[i] = c; }
        ad__crc_ready = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = ad__crc_tab[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

typedef int (*ad__readfn)(void* u, uint8_t* buf, size_t n);    /* >0 bytes, 0 eof, <0 error */
typedef int (*ad__sinkfn)(void* u, const uint8_t* buf, size_t n);   /* 0 ok */
#define AD_WIN 32768
typedef struct { short count[16]; short symbol[288]; } ad__huff;
typedef struct {
    ad__readfn rd; void* rd_u; ad__sinkfn sink; void* sink_u;
    uint8_t in[8192]; size_t inpos, inlen;
    uint32_t bitbuf; int bitcnt;
    uint8_t* win; uint64_t wtotal;
    uint32_t crc; uint64_t out_total;
    int err;
} ad__inf;

static int ad__inf_byte(ad__inf* s) {
    if (s->inpos == s->inlen) {
        int r = s->rd(s->rd_u, s->in, sizeof(s->in));
        if (r <= 0) { s->err = -1; return -1; }
        s->inpos = 0; s->inlen = (size_t)r;
    }
    return s->in[s->inpos++];
}
static int ad__inf_bits(ad__inf* s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        int b = ad__inf_byte(s);
        if (b < 0) return -1;
        v |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need; s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
static int ad__inf_out(ad__inf* s, uint8_t c) {
    s->win[s->wtotal & (AD_WIN - 1)] = c;
    s->wtotal++;
    if ((s->wtotal & (AD_WIN - 1)) == 0) {
        if (s->sink(s->sink_u, s->win, AD_WIN)) return -1;
        s->crc = ad__crc32(s->crc, s->win, AD_WIN);
        s->out_total += AD_WIN;
    }
    return 0;
}
static int ad__huff_build(ad__huff* h, const short* length, int n) {
    short offs[16];
    for (int l = 0; l <= 15; l++) h->count[l] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l <= 15; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return left; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static int ad__huff_decode(ad__inf* s, const ad__huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = ad__inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -2;
}
static int ad__inf_codes(ad__inf* s, const ad__huff* lc, const ad__huff* dc) {
    static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    for (;;) {
        int sym = ad__huff_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (ad__inf_out(s, (uint8_t)sym)) return -1; }
        else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = ad__inf_bits(s, lext[sym]); if (e < 0) return -1;
            int len = lens[sym] + e;
            int ds = ad__huff_decode(s, dc); if (ds < 0 || ds >= 30) return -1;
            e = ad__inf_bits(s, dext[ds]); if (e < 0) return -1;
            uint32_t dist = (uint32_t)(dists[ds] + e);
            if (dist > s->wtotal) return -1;
            while (len--) if (ad__inf_out(s, s->win[(s->wtotal - dist) & (AD_WIN - 1)])) return -1;
        }
    }
}
static int ad__inf_block_dynamic(ad__inf* s) {
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short lengths[320];
    ad__huff lc, dc;
    int nlen = ad__inf_bits(s, 5), ndist = ad__inf_bits(s, 5), ncode = ad__inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    int idx;
    for (idx = 0; idx < ncode; idx++) { int b = ad__inf_bits(s, 3); if (b < 0) return -1; lengths[order[idx]] = (short)b; }
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (ad__huff_build(&lc, lengths, 19) != 0) return -1;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = ad__huff_decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return -1; len = lengths[idx - 1]; rep = ad__inf_bits(s, 2); if (rep < 0) return -1; rep += 3; }
            else if (sym == 17) { rep = ad__inf_bits(s, 3); if (rep < 0) return -1; rep += 3; }
            else { rep = ad__inf_bits(s, 7); if (rep < 0) return -1; rep += 11; }
            if (idx + rep > nlen + ndist) return -1;
            while (rep--) lengths[idx++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = ad__huff_build(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = ad__huff_build(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return ad__inf_codes(s, &lc, &dc);
}
static int ad__inf_block_fixed(ad__inf* s) {
    static ad__huff lc, dc; static int ready;
    if (!ready) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        ad__huff_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        ad__huff_build(&dc, l, 30);
        ready = 1;
    }
    return ad__inf_codes(s, &lc, &dc);
}
/* Raw deflate stream; output goes to the sink in order. 0 = ok. */
static int ad__inflate(ad__inf* s) {
    int last;
    do {
        last = ad__inf_bits(s, 1);
        int type = ad__inf_bits(s, 2);
        if (last < 0 || type < 0) return -1;
        int r;
        if (type == 0) {
            s->bitbuf = 0; s->bitcnt = 0;
            int b0 = ad__inf_byte(s), b1 = ad__inf_byte(s), b2 = ad__inf_byte(s), b3 = ad__inf_byte(s);
            if (b3 < 0) return -1;
            unsigned len = (unsigned)(b0 | (b1 << 8)), nlen = (unsigned)(b2 | (b3 << 8));
            if (len != (~nlen & 0xffff)) return -1;
            while (len--) { int b = ad__inf_byte(s); if (b < 0 || ad__inf_out(s, (uint8_t)b)) return -1; }
            r = 0;
        } else if (type == 1) r = ad__inf_block_fixed(s);
        else if (type == 2) r = ad__inf_block_dynamic(s);
        else r = -1;
        if (r) return -1;
    } while (!last);
    return 0;
}
/* gzip member: header, deflate data, CRC/length trailer.  Output flushed in order to `sink`. */
static int ad__gunzip(ad__readfn rd, void* rd_u, ad__sinkfn sink, void* sink_u) {
    ad__inf* s = (ad__inf*)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->win = (uint8_t*)malloc(AD_WIN);
    if (!s->win) { free(s); return -1; }
    s->rd = rd; s->rd_u = rd_u; s->sink = sink; s->sink_u = sink_u;
    int rc = -1;
    int id1 = ad__inf_byte(s), id2 = ad__inf_byte(s), cm = ad__inf_byte(s), flg = ad__inf_byte(s);
    if (id1 != 0x1f || id2 != 0x8b || cm != 8 || flg < 0) goto out;
    for (int i = 0; i < 6; i++) if (ad__inf_byte(s) < 0) goto out;      /* mtime, xfl, os */
    if (flg & 4) { int xl = ad__inf_byte(s), xh = ad__inf_byte(s); if (xh < 0) goto out; for (int n = xl | (xh << 8); n > 0; n--) if (ad__inf_byte(s) < 0) goto out; }
    if (flg & 8) { int c; do { c = ad__inf_byte(s); } while (c > 0); if (c < 0) goto out; }
    if (flg & 16) { int c; do { c = ad__inf_byte(s); } while (c > 0); if (c < 0) goto out; }
    if (flg & 2) { ad__inf_byte(s); ad__inf_byte(s); }
    if (ad__inflate(s)) goto out;
    {   /* flush what is left in the window */
        size_t rem = (size_t)(s->wtotal & (AD_WIN - 1));
        if (rem) { if (sink(sink_u, s->win, rem)) goto out; s->crc = ad__crc32(s->crc, s->win, rem); s->out_total += rem; }
    }
    s->bitbuf = 0; s->bitcnt = 0;
    uint32_t crc = 0, isz = 0;
    for (int i = 0; i < 4; i++) { int b = ad__inf_byte(s); if (b < 0) goto out; crc |= (uint32_t)b << (8 * i); }
    for (int i = 0; i < 4; i++) { int b = ad__inf_byte(s); if (b < 0) goto out; isz |= (uint32_t)b << (8 * i); }
    if (crc != s->crc || isz != (uint32_t)s->out_total) goto out;
    rc = 0;
out:
    free(s->win); free(s);
    return rc;
}

/* ====================================================================== */
/* cpio (odc / newc) reader and odc writer                                  */
/* ====================================================================== */
enum { AD_CP_MAGIC, AD_CP_HDR, AD_CP_NAME, AD_CP_NPAD, AD_CP_DATA, AD_CP_DPAD, AD_CP_END, AD_CP_ERR };
typedef struct airdrop airdrop_t;
typedef struct {
    airdrop_t* ad;
    char dir[1024];
    int state, fmt; size_t have, hdr_len, namesize;
    uint8_t hdr[112];
    char name[1024];
    uint64_t filesize, remaining, written_total, max_bytes;
    unsigned mode;
    size_t skip;
    FILE* fp; char path[2048];
    int ok_files;
} ad__cpio;

static uint64_t ad__num(const uint8_t* p, size_t n, int base) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    return v;
}
static void ad__mkdirs(const char* path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0;
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/'; }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}
/* Relative path made of safe components only; returns 0 and the cleaned path, or -1. */
static int ad__safe_rel(const char* in, char* out, size_t cap) {
    size_t o = 0;
    const char* p = in;
    if (!*p) return -1;
    while (*p) {
        while (*p == '/' ) p++;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 0) break;
        if ((l == 1 && p[0] == '.') ) { p = e; continue; }
        if (l == 2 && p[0] == '.' && p[1] == '.') return -1;
        for (size_t i = 0; i < l; i++) if (p[i] == '\\' || p[i] == ':' || (unsigned char)p[i] < 32) return -1;
        if (o + l + 2 >= cap) return -1;
        if (o) out[o++] = '/';
        memcpy(out + o, p, l); o += l;
        p = e;
    }
    out[o] = 0;
    return o ? 0 : -1;
}
static int ad__exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
static void ad__unique_path(char* path, size_t cap) {
    if (!ad__exists(path)) return;
    char base[2048], ext[64] = "";
    snprintf(base, sizeof(base), "%s", path);
    char* slash = strrchr(base, '/');
    char* dot = strrchr(base, '.');
    if (dot && (!slash || dot > slash)) { snprintf(ext, sizeof(ext), "%s", dot); *dot = 0; }
    for (int i = 1; i < 10000; i++) {
        snprintf(path, cap, "%s (%d)%s", base, i, ext);
        if (!ad__exists(path)) return;
    }
}

static void ad__log(airdrop_t* a, int level, const char* fmt, ...);
static void ad__emit_progress(airdrop_t* a, const char* name, uint64_t done, int sending);
static void ad__emit_received(airdrop_t* a, const char* path);

static int ad__cpio_finish_entry(ad__cpio* c) {
    if (c->fp) { fclose(c->fp); c->fp = NULL; c->ok_files++; ad__emit_received(c->ad, c->path); }
    size_t dpad = c->fmt == 2 ? (size_t)((4 - (c->filesize & 3)) & 3) : 0;
    c->skip = dpad;
    c->state = dpad ? AD_CP_DPAD : AD_CP_MAGIC;
    c->have = 0;
    return 0;
}
static int ad__cpio_begin_entry(ad__cpio* c) {
    c->have = 0;
    if (!strcmp(c->name, "TRAILER!!!")) { c->state = AD_CP_END; return 0; }
    char rel[1024], full[2048];
    c->fp = NULL;
    int regular = (c->mode & 0170000) == 0100000, isdir = (c->mode & 0170000) == 0040000;
    if ((regular || isdir) && ad__safe_rel(c->name, rel, sizeof(rel)) == 0) {
        snprintf(full, sizeof(full), "%s/%s", c->dir, rel);
        if (isdir) ad__mkdirs(full);
        else {
            char* slash = strrchr(full, '/');
            if (slash) { *slash = 0; ad__mkdirs(full); *slash = '/'; }
            ad__unique_path(full, sizeof(full));
            c->fp = fopen(full, "wb");
            if (!c->fp) return -1;
            snprintf(c->path, sizeof(c->path), "%s", full);
        }
    } else if (regular || isdir) {
        ad__log(c->ad, 1, "refusing unsafe path in archive: %s", c->name);
    }
    c->remaining = c->filesize;
    if (c->remaining == 0) return ad__cpio_finish_entry(c);
    c->state = AD_CP_DATA;
    return 0;
}
static int ad__cpio_feed(ad__cpio* c, const uint8_t* p, size_t n) {
    while (n) {
        switch (c->state) {
        case AD_CP_MAGIC: {
            size_t k = 6 - c->have < n ? 6 - c->have : n;
            memcpy(c->hdr + c->have, p, k); c->have += k; p += k; n -= k;
            if (c->have == 6) {
                if (!memcmp(c->hdr, "070707", 6)) { c->fmt = 1; c->hdr_len = 76; }
                else if (!memcmp(c->hdr, "070701", 6) || !memcmp(c->hdr, "070702", 6)) { c->fmt = 2; c->hdr_len = 110; }
                else return -1;
                c->state = AD_CP_HDR;
            }
            break; }
        case AD_CP_HDR: {
            size_t k = c->hdr_len - c->have < n ? c->hdr_len - c->have : n;
            memcpy(c->hdr + c->have, p, k); c->have += k; p += k; n -= k;
            if (c->have == c->hdr_len) {
                if (c->fmt == 1) { c->mode = (unsigned)ad__num(c->hdr + 18, 6, 8); c->namesize = (size_t)ad__num(c->hdr + 59, 6, 8); c->filesize = ad__num(c->hdr + 65, 11, 8); }
                else { c->mode = (unsigned)ad__num(c->hdr + 14, 8, 16); c->filesize = ad__num(c->hdr + 54, 8, 16); c->namesize = (size_t)ad__num(c->hdr + 94, 8, 16); }
                if (c->namesize == 0 || c->namesize > sizeof(c->name) - 1) return -1;
                if (c->filesize > c->max_bytes || c->written_total + c->filesize > c->max_bytes) return -1;
                c->have = 0; c->state = AD_CP_NAME;
            }
            break; }
        case AD_CP_NAME: {
            size_t k = c->namesize - c->have < n ? c->namesize - c->have : n;
            memcpy(c->name + c->have, p, k); c->have += k; p += k; n -= k;
            if (c->have == c->namesize) {
                c->name[c->namesize] = 0;
                c->skip = c->fmt == 2 ? (4 - ((110 + c->namesize) & 3)) & 3 : 0;
                if (c->skip) { c->state = AD_CP_NPAD; c->have = 0; }
                else if (ad__cpio_begin_entry(c)) return -1;
            }
            break; }
        case AD_CP_NPAD: {
            size_t k = c->skip < n ? c->skip : n;
            c->skip -= k; p += k; n -= k;
            if (!c->skip && ad__cpio_begin_entry(c)) return -1;
            break; }
        case AD_CP_DATA: {
            size_t k = c->remaining < n ? (size_t)c->remaining : n;
            if (c->fp && fwrite(p, 1, k, c->fp) != k) return -1;
            c->remaining -= k; c->written_total += k; p += k; n -= k;
            if (c->fp) ad__emit_progress(c->ad, c->name, c->filesize - c->remaining, 0);
            if (!c->remaining) ad__cpio_finish_entry(c);
            break; }
        case AD_CP_DPAD: {
            size_t k = c->skip < n ? c->skip : n;
            c->skip -= k; p += k; n -= k;
            if (!c->skip) { c->state = AD_CP_MAGIC; c->have = 0; }
            break; }
        case AD_CP_END: return 0;
        default: return -1;
        }
    }
    return 0;
}

/* ---- sender-side archive writer: odc cpio inside a gzip file made of stored blocks ---- */
typedef struct { ad_io* io; uint8_t buf[32768 + 64]; size_t n; uint32_t crc; uint64_t total; int err; uint64_t sent; airdrop_t* ad; } ad__gzw;
static void ad__chunk(ad__gzw* g, const uint8_t* d, size_t n) {
    if (g->err || !n) return;
    char h[24]; int k = snprintf(h, sizeof(h), "%zx\r\n", n);
    if (ad__io_write(g->io, h, (size_t)k) || ad__io_write(g->io, d, n) || ad__io_write(g->io, "\r\n", 2)) g->err = 1;
}
static void ad__gz_emit(ad__gzw* g, const uint8_t* d, size_t n) {     /* raw gzip bytes out, coalesced */
    while (n) {
        size_t room = sizeof(g->buf) - g->n, k = room < n ? room : n;
        memcpy(g->buf + g->n, d, k); g->n += k; d += k; n -= k;
        if (g->n == sizeof(g->buf)) { ad__chunk(g, g->buf, g->n); g->n = 0; }
    }
}
static void ad__gz_data(ad__gzw* g, const uint8_t* d, size_t n) {     /* uncompressed -> stored deflate blocks */
    g->crc = ad__crc32(g->crc, d, n); g->total += n;
    while (n) {
        size_t k = n < 65535 ? n : 65535;
        uint8_t hdr[5] = { 0, (uint8_t)k, (uint8_t)(k >> 8), (uint8_t)~k, (uint8_t)(~k >> 8) };
        ad__gz_emit(g, hdr, 5); ad__gz_emit(g, d, k);
        d += k; n -= k;
    }
}
static void ad__odc(ad__gzw* g, const char* name, unsigned mode, uint64_t size, unsigned ino) {
    char h[80];
    size_t nl = strlen(name) + 1;
    int k = snprintf(h, sizeof(h), "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011llo", 0, ino & 0777777, mode & 0777777, 0, 0, 1, 0, 0, (unsigned)nl, (unsigned long long)size);
    ad__gz_data(g, (const uint8_t*)h, (size_t)k);
    ad__gz_data(g, (const uint8_t*)name, nl);
}

/* ====================================================================== */
/* The AirDrop endpoint                                                     */
/* ====================================================================== */
#define AD_MAX_CONN 8
#define AD_MAX_PEERS 32
typedef struct ad_conn ad_conn;
struct airdrop {
    airdrop_config cfg; airdrop_callbacks cb;
    char name[96], model[64], dir[1024], err[200], service_id[16];
    void *tls_server_ctx, *tls_client_ctx, *x509, *pkey;
    ad_sock listen_sock; int port;
    volatile int running, started;
    ad_thread th_accept; ad_mutex lock;
    ad_conn* conns[AD_MAX_CONN];
    airdrop_peer peers[AD_MAX_PEERS]; uint64_t peer_seen[AD_MAX_PEERS]; uint8_t peer_ntf[AD_MAX_PEERS]; int npeers;
    ad_sock mdns4, mdns6; ad_thread th_mdns; int mdns_running;
    int ifindex; uint8_t ip4[4]; uint8_t ip6[16]; int have_ip6, have_ip4;
};
struct ad_conn { airdrop_t* a; ad_io io; ad_thread th; volatile int alive; int accepted; char peer[64]; };

#ifdef _WIN32
#  define AD_THREAD_RV 0
#else
#  define AD_THREAD_RV NULL
#endif

static void ad__log(airdrop_t* a, int level, const char* fmt, ...) {
    if (!a->cb.on_log) return;
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    a->cb.on_log(a->cb.user, level, b);
}
static void ad__emit_progress(airdrop_t* a, const char* name, uint64_t done, int sending) { if (a->cb.on_progress) a->cb.on_progress(a->cb.user, name, done, sending); }
static void ad__emit_received(airdrop_t* a, const char* path) { ad__log(a, 0, "received %s", path); if (a->cb.on_received) a->cb.on_received(a->cb.user, path); }

static const char* ad__uti(const char* name) {
    static const struct { const char* ext; const char* uti; } t[] = {
        { ".jpg", "public.jpeg" }, { ".jpeg", "public.jpeg" }, { ".png", "public.png" }, { ".gif", "com.compuserve.gif" },
        { ".heic", "public.heic" }, { ".tif", "public.tiff" }, { ".tiff", "public.tiff" }, { ".webp", "org.webmproject.webp" },
        { ".mov", "com.apple.quicktime-movie" }, { ".mp4", "public.mpeg-4" }, { ".m4v", "com.apple.m4v-video" },
        { ".mp3", "public.mp3" }, { ".m4a", "com.apple.m4a-audio" }, { ".wav", "com.microsoft.waveform-audio" },
        { ".pdf", "com.adobe.pdf" }, { ".txt", "public.plain-text" }, { ".html", "public.html" }, { ".zip", "public.zip-archive" },
        { ".rtf", "public.rtf" }, { ".md", "net.daringfireball.markdown" }, { ".csv", "public.comma-separated-values-text" },
    };
    const char* dot = strrchr(name, '.');
    if (dot) for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcasecmp(dot, t[i].ext)) return t[i].uti;
    return "public.data";
}

/* ---- request handlers (receiver) ---- */
static int ad__read_plist_body(ad_conn* c, ad__body* b, ad_pl** out) {
    uint8_t* d = NULL; size_t n = 0;
    *out = NULL;
    if (ad__body_all(b, &d, &n, 8u << 20) != 0) return -1;
    if (n) *out = ad_pl_parse(d, n);
    free(d);
    (void)c;
    return 0;
}
static void ad__send_plist(ad_conn* c, int code, const char* text, ad_pl* pl) {
    size_t n = 0;
    uint8_t* b = pl ? ad_pl_serialize(pl, &n) : NULL;
    ad_pl_free(pl);
    ad__send_status(&c->io, code, text, "application/octet-stream", b, b ? n : 0, 0);
    free(b);
}
static void ad__h_discover(ad_conn* c, ad__body* body) {
    airdrop_t* a = c->a;
    ad_pl* req = NULL;
    ad__read_plist_body(c, body, &req);
    ad_pl_free(req);
    if (!a->cfg.discoverable && a->cfg.discoverable != 0) { }
    ad_pl* d = ad_pl_new(AD_PL_DICT);
    static const char caps[] = "{\"Version\":1}";
    ad_pl_set(d, "ReceiverMediaCapabilities", ad_pl_data(caps, sizeof(caps) - 1));
    ad_pl_set(d, "ReceiverComputerName", ad_pl_str(a->name));
    ad_pl_set(d, "ReceiverModelName", ad_pl_str(a->model));
    ad__send_plist(c, 200, "OK", d);
}
static void ad__h_ask(ad_conn* c, ad__body* body) {
    airdrop_t* a = c->a;
    ad_pl* req = NULL;
    if (ad__read_plist_body(c, body, &req) != 0 || !req) { ad__send_status(&c->io, 400, "Bad Request", NULL, NULL, 0, 1); return; }
    airdrop_ask ask; memset(&ask, 0, sizeof(ask));
    ad_pl* v;
    if ((v = ad_pl_get(req, "SenderComputerName")) && v->t == AD_PL_STR) snprintf(ask.sender_name, sizeof(ask.sender_name), "%s", (const char*)v->d);
    if ((v = ad_pl_get(req, "SenderModelName")) && v->t == AD_PL_STR) snprintf(ask.sender_model, sizeof(ask.sender_model), "%s", (const char*)v->d);
    if ((v = ad_pl_get(req, "SenderID")) && v->t == AD_PL_STR) snprintf(ask.sender_id, sizeof(ask.sender_id), "%s", (const char*)v->d);
    if ((v = ad_pl_get(req, "FileIcon")) && v->t == AD_PL_DATA) { ask.icon = v->d; ask.icon_n = v->n; }
    airdrop_file files[64]; const char* urls[32];
    ad_pl* fl = ad_pl_get(req, "Files");
    if (fl && fl->t == AD_PL_ARRAY) {
        for (size_t i = 0; i < fl->cnt && ask.nfiles < 64; i++) {
            airdrop_file* f = &files[ask.nfiles++]; memset(f, 0, sizeof(*f));
            ad_pl* x;
            if ((x = ad_pl_get(fl->v[i], "FileName")) && x->t == AD_PL_STR) snprintf(f->name, sizeof(f->name), "%s", (const char*)x->d);
            if ((x = ad_pl_get(fl->v[i], "FileType")) && x->t == AD_PL_STR) snprintf(f->type, sizeof(f->type), "%s", (const char*)x->d);
            if ((x = ad_pl_get(fl->v[i], "FileIsDirectory"))) f->is_dir = (int)x->i;
        }
        ask.files = files;
    }
    ad_pl* it = ad_pl_get(req, "Items");
    if (it && it->t == AD_PL_ARRAY) for (size_t i = 0; i < it->cnt && ask.nurls < 32; i++) if (it->v[i]->t == AD_PL_STR) urls[ask.nurls++] = (const char*)it->v[i]->d;
    if (ask.nurls) ask.urls = urls;
    int ok = a->cb.on_ask ? a->cb.on_ask(a->cb.user, &ask) : 1;
    ad_pl_free(req);
    if (!ok) { ad__log(a, 0, "declined an incoming transfer from %s", ask.sender_name); ad__send_status(&c->io, 406, "Not Acceptable", NULL, NULL, 0, 1); return; }
    c->accepted = 1;
    ad_pl* d = ad_pl_new(AD_PL_DICT);
    ad_pl_set(d, "ReceiverModelName", ad_pl_str(a->model));
    ad_pl_set(d, "ReceiverComputerName", ad_pl_str(a->name));
    ad__send_plist(c, 200, "OK", d);
}
static int ad__body_rd(void* u, uint8_t* buf, size_t n) { return ad__body_read((ad__body*)u, buf, n); }
static int ad__cpio_sink(void* u, const uint8_t* d, size_t n) { return ad__cpio_feed((ad__cpio*)u, d, n); }
static void ad__h_upload(ad_conn* c, const ad__http* m, ad__body* body) {
    airdrop_t* a = c->a;
    const char* ct = ad__h(m, "Content-Type");
    if (!c->accepted) { ad__send_status(&c->io, 403, "Forbidden", NULL, NULL, 0, 1); return; }
    if (!ct || strcasecmp(ct, "application/x-cpio")) { ad__send_status(&c->io, 406, "Not Acceptable", "application/x-cpio", NULL, 0, 1); return; }
    const char* ex = ad__h(m, "Expect");
    if (ex && !strcasecmp(ex, "100-continue")) ad__io_write(&c->io, "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n", 42);
    ad__cpio* cp = (ad__cpio*)calloc(1, sizeof(*cp));
    if (!cp) { ad__send_status(&c->io, 500, "Internal Server Error", NULL, NULL, 0, 1); return; }
    cp->ad = a; snprintf(cp->dir, sizeof(cp->dir), "%s", a->dir);
    cp->max_bytes = a->cfg.max_bytes ? a->cfg.max_bytes : (16ULL << 30);
    int rc = ad__gunzip(ad__body_rd, body, ad__cpio_sink, cp);
    if (cp->fp) { fclose(cp->fp); remove(cp->path); }
    int ok = rc == 0 && cp->state == AD_CP_END;
    int files = cp->ok_files;
    free(cp);
    c->accepted = 0;
    if (ok) ad__send_status(&c->io, 200, "OK", NULL, NULL, 0, 1);
    else ad__send_status(&c->io, 400, "Bad Request", NULL, NULL, 0, 1);
    ad__log(a, ok ? 0 : 1, "upload %s (%d file%s)", ok ? "complete" : "failed", files, files == 1 ? "" : "s");
    if (a->cb.on_done) a->cb.on_done(a->cb.user, ok, 0);
}
static AD_THREAD_RET ad__conn_thread(void* arg) {
    ad_conn* c = (ad_conn*)arg;
    airdrop_t* a = c->a;
    ad__set_timeout(c->io.s, 1000);
    if (ad__T.SSL_accept(c->io.ssl) != 1) { ad__log(a, 1, "TLS handshake failed (%s)", c->peer); goto out; }
    while (a->running) {
        ad__http m;
        int r = ad__http_head(&c->io, &m, 0);
        if (r == -2) continue;
        if (r != 0) break;
        ad__body body;
        ad__body_init(&body, &c->io, &m);
        if (!strcmp(m.method, "POST") && !strcmp(m.path, "/Discover")) ad__h_discover(c, &body);
        else if (!strcmp(m.method, "POST") && !strcmp(m.path, "/Ask")) ad__h_ask(c, &body);
        else if (!strcmp(m.method, "POST") && !strcmp(m.path, "/Upload")) { ad__h_upload(c, &m, &body); break; }
        else { uint8_t tmp[512]; while (ad__body_read(&body, tmp, sizeof(tmp)) > 0) {} ad__send_status(&c->io, 404, "Not Found", NULL, NULL, 0, 0); }
    }
out:
    ad__io_close(&c->io);
    c->alive = 0;
    return AD_THREAD_RV;
}

static ad_sock ad__listen6(int port, int* bound) {
    ad_sock s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s == AD_BADSOCK) return ad__tcp_listen(port, bound);               /* no IPv6: IPv4 only */
    int off = 0, on = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    struct sockaddr_in6 a6; memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6; a6.sin6_addr = in6addr_any; a6.sin6_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&a6, sizeof(a6)) != 0 || listen(s, 8) != 0) { AD_CLOSESOCK(s); return AD_BADSOCK; }
    socklen_t l = sizeof(a6);
    getsockname(s, (struct sockaddr*)&a6, &l);
    if (bound) *bound = ntohs(a6.sin6_port);
    return s;
}
static AD_THREAD_RET ad__accept_thread(void* arg) {
    airdrop_t* a = (airdrop_t*)arg;
    while (a->running) {
        fd_set fds; struct timeval tv = { 0, 250000 };
        FD_ZERO(&fds); FD_SET(a->listen_sock, &fds);
        if (select((int)a->listen_sock + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_storage from; socklen_t fl = sizeof(from);
        ad_sock s = accept(a->listen_sock, (struct sockaddr*)&from, &fl);
        if (s == AD_BADSOCK) continue;
        ad__mutex_lock(&a->lock);
        int slot = -1;
        for (int i = 0; i < AD_MAX_CONN; i++) {
            if (a->conns[i] && !a->conns[i]->alive) { ad__thread_join(a->conns[i]->th); free(a->conns[i]); a->conns[i] = NULL; }
            if (!a->conns[i] && slot < 0) slot = i;
        }
        ad_conn* c = slot >= 0 ? (ad_conn*)calloc(1, sizeof(*c)) : NULL;
        if (!c) { AD_CLOSESOCK(s); ad__mutex_unlock(&a->lock); continue; }
        c->a = a; c->io.s = s; c->alive = 1;
        void* ssl = ad__T.SSL_new(a->tls_server_ctx);
        if (!ssl || !ad__T.SSL_set_fd(ssl, (int)s)) { if (ssl) ad__T.SSL_free(ssl); AD_CLOSESOCK(s); free(c); ad__mutex_unlock(&a->lock); continue; }
        c->io.ssl = ssl;
        if (from.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6*)&from)->sin6_addr, c->peer, sizeof(c->peer));
        else inet_ntop(AF_INET, &((struct sockaddr_in*)&from)->sin_addr, c->peer, sizeof(c->peer));
        a->conns[slot] = c;
        if (ad__thread_start(&c->th, ad__conn_thread, c) != 0) { ad__io_close(&c->io); free(c); a->conns[slot] = NULL; }
        ad__mutex_unlock(&a->lock);
    }
    return AD_THREAD_RV;
}

/* ====================================================================== */
/* mDNS: advertise "_airdrop._tcp" and browse for others (IPv6 + IPv4)      */
/* ====================================================================== */
static size_t ad__dns_name(uint8_t* o, const char* label0, const char* rest) {
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
static size_t ad__dns_rr(uint8_t* o, const uint8_t* name, size_t nn, uint16_t type, uint16_t cls, uint32_t ttl, const uint8_t* rd, size_t rn) {
    size_t n = 0;
    memcpy(o, name, nn); n += nn;
    o[n++] = (uint8_t)(type >> 8); o[n++] = (uint8_t)type; o[n++] = (uint8_t)(cls >> 8); o[n++] = (uint8_t)cls;
    ad__put32be(o + n, ttl); n += 4;
    o[n++] = (uint8_t)(rn >> 8); o[n++] = (uint8_t)rn;
    memcpy(o + n, rd, rn); n += rn;
    return n;
}
/* Reads a (possibly compressed) name into dotted text; advances *pos past the name. */
static int ad__dns_read_name(const uint8_t* m, size_t n, size_t* pos, char* out, size_t cap) {
    size_t p = *pos, o = 0, end = 0; int jumped = 0, hops = 0;
    for (;;) {
        if (p >= n || ++hops > 64) return -1;
        uint8_t l = m[p];
        if (!l) { p++; break; }
        if ((l & 0xC0) == 0xC0) { if (p + 1 >= n) return -1; size_t ptr = (size_t)(((l & 0x3F) << 8) | m[p + 1]); if (!jumped) end = p + 2; jumped = 1; p = ptr; continue; }
        if (l > 63 || p + 1 + l > n || o + l + 2 >= cap) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, m + p + 1, l); o += l; p += 1 + (size_t)l;
    }
    out[o] = 0;
    *pos = jumped ? end : p;
    return 0;
}
static size_t ad__mdns_build(airdrop_t* a, uint8_t* pkt, uint32_t ttl) {
    uint8_t nm_type[64], nm_inst[96], host[96], rd[300], enumn[64];
    char hostname[64];
    snprintf(hostname, sizeof(hostname), "luna-airdrop-%s.local", a->service_id);
    size_t type_n = ad__dns_name(nm_type, NULL, "_airdrop._tcp.local");
    size_t inst_n = ad__dns_name(nm_inst, a->service_id, "_airdrop._tcp.local");
    size_t host_n = ad__dns_name(host, NULL, hostname);
    size_t enum_n = ad__dns_name(enumn, NULL, "_services._dns-sd._udp.local");
    memset(pkt, 0, 12);
    pkt[2] = 0x84;
    size_t n = 12; int an = 0;
    n += ad__dns_rr(pkt + n, nm_type, type_n, 12, 0x0001, ttl, nm_inst, inst_n); an++;
    rd[0] = rd[1] = rd[2] = rd[3] = 0; rd[4] = (uint8_t)(a->port >> 8); rd[5] = (uint8_t)a->port;
    memcpy(rd + 6, host, host_n);
    n += ad__dns_rr(pkt + n, nm_inst, inst_n, 33, 0x8001, ttl, rd, 6 + host_n); an++;
    uint8_t txt[32]; int tl = snprintf((char*)txt + 1, sizeof(txt) - 1, "flags=%d", 0x88);
    txt[0] = (uint8_t)tl;
    n += ad__dns_rr(pkt + n, nm_inst, inst_n, 16, 0x8001, ttl, txt, (size_t)tl + 1); an++;
    if (a->have_ip6) { n += ad__dns_rr(pkt + n, host, host_n, 28, 0x8001, ttl, a->ip6, 16); an++; }
    if (a->have_ip4) { n += ad__dns_rr(pkt + n, host, host_n, 1, 0x8001, ttl, a->ip4, 4); an++; }
    n += ad__dns_rr(pkt + n, enumn, enum_n, 12, 0x0001, ttl, nm_type, type_n); an++;
    pkt[6] = (uint8_t)(an >> 8); pkt[7] = (uint8_t)an;
    return n;
}
static size_t ad__mdns_query(uint8_t* pkt) {
    memset(pkt, 0, 12);
    pkt[5] = 1;
    size_t n = 12 + ad__dns_name(pkt + 12, NULL, "_airdrop._tcp.local");
    pkt[n++] = 0; pkt[n++] = 12; pkt[n++] = 0; pkt[n++] = 1;
    return n;
}

/* interface address discovery */
static void ad__find_addrs(airdrop_t* a) {
    a->have_ip4 = a->have_ip6 = 0;
    a->ifindex = 0;
#ifndef _WIN32
    const char* want = a->cfg.interface_name;
    if (want && want[0]) a->ifindex = (int)if_nametoindex(want);
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || !(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            if (want && want[0] && strcmp(p->ifa_name, want)) continue;
            if (!a->have_ip6 && p->ifa_addr->sa_family == AF_INET6) {
                const struct in6_addr* x = &((struct sockaddr_in6*)p->ifa_addr)->sin6_addr;
                if (x->s6_addr[0] == 0xfe && (x->s6_addr[1] & 0xc0) == 0x80) {          /* link-local: reachable on this link */
                    memcpy(a->ip6, x, 16); a->have_ip6 = 1;
                    if (!a->ifindex) a->ifindex = (int)if_nametoindex(p->ifa_name);
                }
            } else if (!a->have_ip4 && p->ifa_addr->sa_family == AF_INET && !(want && want[0] && 0)) {
                memcpy(a->ip4, &((struct sockaddr_in*)p->ifa_addr)->sin_addr, 4); a->have_ip4 = 1;
                if (!a->ifindex) a->ifindex = (int)if_nametoindex(p->ifa_name);
            }
        }
        freeifaddrs(ifa);
    }
    /* AWDL carries IPv6 only: an IPv4 address from another interface would be misleading */
    if (want && !strncmp(want, "awdl", 4)) a->have_ip4 = 0;
#else
    a->have_ip4 = ad__local_ipv4(a->ip4) == 0;
#endif
}
static int ad__peer_upsert(airdrop_t* a, const char* id, const char* host, int port, uint32_t flags, int* is_new) {
    int i;
    for (i = 0; i < a->npeers; i++) if (!strcmp(a->peers[i].id, id)) break;
    *is_new = 0;
    if (i == a->npeers) { if (a->npeers >= AD_MAX_PEERS) return -1; a->npeers++; memset(&a->peers[i], 0, sizeof(a->peers[i])); a->peer_ntf[i] = 0; snprintf(a->peers[i].id, sizeof(a->peers[i].id), "%s", id); *is_new = 1; }
    a->peer_seen[i] = ad__now_ns();
    if (host && host[0]) snprintf(a->peers[i].host, sizeof(a->peers[i].host), "%s", host);
    if (port) a->peers[i].port = port;
    if (flags) a->peers[i].flags = flags;
    return i;
}
typedef struct { char inst[96]; char target[96]; int port; uint32_t flags; char addr[80]; } ad__seen;
static void ad__mdns_parse(airdrop_t* a, const uint8_t* m, size_t n) {
    if (n < 12 || !(m[2] & 0x80)) return;
    size_t pos = 12;
    int qd = (m[4] << 8) | m[5], rr = ((m[6] << 8) | m[7]) + ((m[8] << 8) | m[9]) + ((m[10] << 8) | m[11]);
    char name[300];
    for (int i = 0; i < qd; i++) { if (ad__dns_read_name(m, n, &pos, name, sizeof(name)) || pos + 4 > n) return; pos += 4; }
    ad__seen seen[8]; int ns = 0;
    struct { char host[96]; char addr[80]; } addrs[16]; int na = 0;
    for (int i = 0; i < rr && i < 64; i++) {
        if (ad__dns_read_name(m, n, &pos, name, sizeof(name)) || pos + 10 > n) break;
        uint16_t type = ad__get16be(m + pos); uint32_t ttl = ad__get32be(m + pos + 4); uint16_t rl = ad__get16be(m + pos + 8);
        pos += 10;
        if (pos + rl > n) break;
        size_t rp = pos;
        if (type == 12 && !strcasecmp(name, "_airdrop._tcp.local") && ns < 8) {
            char inst[300]; size_t q = rp;
            if (!ad__dns_read_name(m, n, &q, inst, sizeof(inst))) {
                char* dot = strstr(inst, "._airdrop._tcp.local");
                if (dot) { *dot = 0; snprintf(seen[ns].inst, sizeof(seen[ns].inst), "%s", inst); seen[ns].port = 0; seen[ns].flags = 0; seen[ns].target[0] = 0; seen[ns].addr[0] = 0; ns++;
                           if (ttl == 0) { ad__mutex_lock(&a->lock); for (int k = 0; k < a->npeers; k++) if (!strcmp(a->peers[k].id, inst)) { airdrop_peer gone = a->peers[k]; int was = a->peer_ntf[k]; a->npeers--; a->peers[k] = a->peers[a->npeers]; a->peer_seen[k] = a->peer_seen[a->npeers]; a->peer_ntf[k] = a->peer_ntf[a->npeers]; ad__mutex_unlock(&a->lock); if (was && a->cb.on_peer) a->cb.on_peer(a->cb.user, 0, &gone); ad__mutex_lock(&a->lock); break; } ad__mutex_unlock(&a->lock); ns--; } }
            }
        } else if (type == 33) {
            char inst[300]; snprintf(inst, sizeof(inst), "%s", name); char* dot = strstr(inst, "._airdrop._tcp.local");
            if (dot && rl > 6) { *dot = 0; size_t q = rp + 6; char tgt[300];
                for (int k = 0; k < ns; k++) if (!strcmp(seen[k].inst, inst)) { seen[k].port = ad__get16be(m + rp + 4); if (!ad__dns_read_name(m, n, &q, tgt, sizeof(tgt))) snprintf(seen[k].target, sizeof(seen[k].target), "%s", tgt); } }
        } else if (type == 16) {
            char inst[300]; snprintf(inst, sizeof(inst), "%s", name); char* dot = strstr(inst, "._airdrop._tcp.local");
            if (dot) { *dot = 0; for (int k = 0; k < ns; k++) if (!strcmp(seen[k].inst, inst)) { size_t q = rp; while (q < rp + rl) { size_t l = m[q++]; if (q + l > rp + rl) break; if (l > 6 && !strncmp((const char*)m + q, "flags=", 6)) seen[k].flags = (uint32_t)atoi((const char*)m + q + 6); q += l; } } }
        } else if ((type == 28 && rl == 16) || (type == 1 && rl == 4)) {
            if (na < 16) {
                snprintf(addrs[na].host, sizeof(addrs[na].host), "%s", name);
                if (type == 28) { char b[64]; inet_ntop(AF_INET6, m + rp, b, sizeof(b));
                    const uint8_t* x = m + rp; int ll = x[0] == 0xfe && (x[1] & 0xc0) == 0x80;
                    char ifn[IF_NAMESIZE] = ""; if (ll && a->ifindex) if_indextoname((unsigned)a->ifindex, ifn);
                    snprintf(addrs[na].addr, sizeof(addrs[na].addr), "%s%s%s", b, ll && ifn[0] ? "%" : "", ll ? ifn : ""); }
                else inet_ntop(AF_INET, m + rp, addrs[na].addr, sizeof(addrs[na].addr));
                na++;
            }
        }
        pos += rl;
    }
    for (int k = 0; k < ns; k++) {
        if (!strcmp(seen[k].inst, a->service_id)) continue;                    /* ourselves */
        const char* addr = NULL;
        for (int j = 0; j < na; j++) if (!strcasecmp(addrs[j].host, seen[k].target)) { if (!addr || strchr(addrs[j].addr, ':')) addr = addrs[j].addr; }
        ad__mutex_lock(&a->lock);
        int is_new; int idx = ad__peer_upsert(a, seen[k].inst, addr, seen[k].port, seen[k].flags, &is_new);
        airdrop_peer snap; if (idx >= 0) snap = a->peers[idx];
        int complete = idx >= 0 && a->peers[idx].host[0] && a->peers[idx].port;
        int fire = complete && !a->peer_ntf[idx];
        if (fire) a->peer_ntf[idx] = 1;
        (void)is_new;
        ad__mutex_unlock(&a->lock);
        if (fire && a->cb.on_peer) a->cb.on_peer(a->cb.user, 1, &snap);
    }
}
static ad_sock ad__mdns_sock(airdrop_t* a, int v6) {
    ad_sock s = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (s == AD_BADSOCK) return s;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
    if (v6) {
        int on = 1; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&on, sizeof(on));
        struct sockaddr_in6 b; memset(&b, 0, sizeof(b)); b.sin6_family = AF_INET6; b.sin6_port = htons(5353); b.sin6_addr = in6addr_any;
        if (bind(s, (struct sockaddr*)&b, sizeof(b)) != 0) { AD_CLOSESOCK(s); return AD_BADSOCK; }
        struct ipv6_mreq mr; memset(&mr, 0, sizeof(mr)); inet_pton(AF_INET6, "ff02::fb", &mr.ipv6mr_multiaddr); mr.ipv6mr_interface = (unsigned)a->ifindex;
        setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, (const char*)&mr, sizeof(mr));
        unsigned ifx = (unsigned)a->ifindex; int hops = 255;
        if (ifx) setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&ifx, sizeof(ifx));
        setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char*)&hops, sizeof(hops));
    } else {
        struct sockaddr_in b; memset(&b, 0, sizeof(b)); b.sin_family = AF_INET; b.sin_port = htons(5353); b.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr*)&b, sizeof(b)) != 0) { AD_CLOSESOCK(s); return AD_BADSOCK; }
        struct ip_mreq mr; mr.imr_multiaddr.s_addr = htonl(0xE00000FBu);
        if (a->have_ip4) memcpy(&mr.imr_interface.s_addr, a->ip4, 4); else mr.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr));
        unsigned char ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        if (a->have_ip4) setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)a->ip4, 4);
    }
    return s;
}
static void ad__mdns_send(airdrop_t* a, const uint8_t* pkt, size_t n) {
    if (a->mdns4 != AD_BADSOCK) { struct sockaddr_in g; memset(&g, 0, sizeof(g)); g.sin_family = AF_INET; g.sin_port = htons(5353); g.sin_addr.s_addr = htonl(0xE00000FBu); sendto(a->mdns4, (const char*)pkt, (int)n, 0, (struct sockaddr*)&g, sizeof(g)); }
    if (a->mdns6 != AD_BADSOCK) { struct sockaddr_in6 g; memset(&g, 0, sizeof(g)); g.sin6_family = AF_INET6; g.sin6_port = htons(5353); inet_pton(AF_INET6, "ff02::fb", &g.sin6_addr); g.sin6_scope_id = (unsigned)a->ifindex; sendto(a->mdns6, (const char*)pkt, (int)n, 0, (struct sockaddr*)&g, sizeof(g)); }
}
static AD_THREAD_RET ad__mdns_thread(void* arg) {
    airdrop_t* a = (airdrop_t*)arg;
    uint8_t pkt[2048], in[4096];
    uint64_t next_announce = 0, next_query = 0; int announces = 0;
    while (a->mdns_running) {
        uint64_t now = ad__now_ns();
        if (a->cfg.discoverable && announces < 3 && now >= next_announce) { size_t n = ad__mdns_build(a, pkt, 4500); ad__mdns_send(a, pkt, n); announces++; next_announce = now + 1000000000ULL; }
        if (now >= next_query) {
            size_t n = ad__mdns_query(pkt); ad__mdns_send(a, pkt, n); next_query = now + 5000000000ULL;
            for (;;) {                                                      /* drop peers that stopped answering (60 s) */
                airdrop_peer gone; int have = 0, was = 0;
                ad__mutex_lock(&a->lock);
                for (int k = 0; k < a->npeers; k++) if (now - a->peer_seen[k] > 60000000000ULL) {
                    gone = a->peers[k]; was = a->peer_ntf[k]; have = 1; a->npeers--;
                    a->peers[k] = a->peers[a->npeers]; a->peer_seen[k] = a->peer_seen[a->npeers]; a->peer_ntf[k] = a->peer_ntf[a->npeers];
                    break;
                }
                ad__mutex_unlock(&a->lock);
                if (!have) break;
                if (was && a->cb.on_peer) a->cb.on_peer(a->cb.user, 0, &gone);
            }
        }
        fd_set fds; struct timeval tv = { 0, 200000 }; int mx = 0;
        FD_ZERO(&fds);
        if (a->mdns4 != AD_BADSOCK) { FD_SET(a->mdns4, &fds); mx = (int)a->mdns4; }
        if (a->mdns6 != AD_BADSOCK) { FD_SET(a->mdns6, &fds); if ((int)a->mdns6 > mx) mx = (int)a->mdns6; }
        if (select(mx + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        ad_sock socks[2] = { a->mdns4, a->mdns6 };
        for (int k = 0; k < 2; k++) {
            if (socks[k] == AD_BADSOCK || !FD_ISSET(socks[k], &fds)) continue;
            int n = (int)recv(socks[k], (char*)in, sizeof(in), 0);
            if (n < 12) continue;
            if (!(in[2] & 0x80)) {                                          /* a query: answer when it names us */
                if (!a->cfg.discoverable) continue;
                size_t pos = 12; int qd = (in[4] << 8) | in[5], hit = 0; char nm[300];
                for (int i = 0; i < qd && i < 8; i++) { if (ad__dns_read_name(in, (size_t)n, &pos, nm, sizeof(nm)) || pos + 4 > (size_t)n) break; pos += 4;
                    char inst[120]; snprintf(inst, sizeof(inst), "%s._airdrop._tcp.local", a->service_id);
                    if (!strcasecmp(nm, "_airdrop._tcp.local") || !strcasecmp(nm, inst) || !strcasecmp(nm, "_services._dns-sd._udp.local")) hit = 1; }
                if (hit) { size_t m = ad__mdns_build(a, pkt, 4500); ad__mdns_send(a, pkt, m); }
            } else ad__mdns_parse(a, in, (size_t)n);
        }
    }
    if (a->cfg.discoverable) { size_t n = ad__mdns_build(a, pkt, 0); ad__mdns_send(a, pkt, n); }      /* goodbye */
    return AD_THREAD_RV;
}


/* ---- lifecycle ---- */
airdrop_t* airdrop_create(const airdrop_config* cfg, const airdrop_callbacks* cb) {
    airdrop_t* a = (airdrop_t*)calloc(1, sizeof(*a));
    if (!a) return NULL;
    if (cfg) a->cfg = *cfg;
    if (cb) a->cb = *cb;
    snprintf(a->name, sizeof(a->name), "%s", cfg && cfg->name && cfg->name[0] ? cfg->name : "Luna Desktop");
    snprintf(a->model, sizeof(a->model), "%s", cfg && cfg->model && cfg->model[0] ? cfg->model : "Luna");
    snprintf(a->dir, sizeof(a->dir), "%s", cfg && cfg->download_dir && cfg->download_dir[0] ? cfg->download_dir : ".");
    size_t dl = strlen(a->dir);
    while (dl > 1 && a->dir[dl - 1] == '/') a->dir[--dl] = 0;
    uint8_t r[6]; ad__random(r, 6);
    snprintf(a->service_id, sizeof(a->service_id), "%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
    if (!cfg) a->cfg.discoverable = 1;
    ad__mutex_init(&a->lock);
    a->listen_sock = a->mdns4 = a->mdns6 = AD_BADSOCK;
    return a;
}
const char* airdrop_last_error(const airdrop_t* a) { return a ? a->err : ""; }
int airdrop_port(const airdrop_t* a) { return a ? a->port : 0; }

int airdrop_start(airdrop_t* a) {
    if (!a || a->started) return -1;
    ad__net_init();
    if (ad__tls_load(a->err, sizeof(a->err)) != 0) return -2;
    a->tls_server_ctx = ad__T.SSL_CTX_new(ad__T.TLS_server_method());
    a->tls_client_ctx = ad__T.SSL_CTX_new(ad__T.TLS_client_method());
    if (!a->tls_server_ctx || !a->tls_client_ctx) { snprintf(a->err, sizeof(a->err), "SSL_CTX_new failed"); return -2; }
    if (ad__tls_identity(a->name, &a->x509, &a->pkey) != 0 ||
        ad__T.SSL_CTX_use_certificate(a->tls_server_ctx, a->x509) != 1 || ad__T.SSL_CTX_use_PrivateKey(a->tls_server_ctx, a->pkey) != 1) {
        snprintf(a->err, sizeof(a->err), "could not create the TLS identity"); return -2;
    }
    ad__T.SSL_CTX_set_verify(a->tls_client_ctx, 0, NULL);        /* peers use self-signed certificates */
    ad__mkdirs(a->dir);
    a->listen_sock = ad__listen6(a->cfg.port ? a->cfg.port : 8771, &a->port);
    if (a->listen_sock == AD_BADSOCK) a->listen_sock = ad__listen6(0, &a->port);
    if (a->listen_sock == AD_BADSOCK) { snprintf(a->err, sizeof(a->err), "cannot listen"); return -3; }
    a->running = 1;
    if (ad__thread_start(&a->th_accept, ad__accept_thread, a) != 0) { a->running = 0; return -4; }
    a->started = 1;
    ad__log(a, 0, "AirDrop \"%s\" listening on port %d", a->name, a->port);
    ad__find_addrs(a);
    a->mdns4 = a->have_ip4 ? ad__mdns_sock(a, 0) : AD_BADSOCK;
    a->mdns6 = a->have_ip6 ? ad__mdns_sock(a, 1) : AD_BADSOCK;
    if (a->mdns4 != AD_BADSOCK || a->mdns6 != AD_BADSOCK) {
        a->mdns_running = 1;
        if (ad__thread_start(&a->th_mdns, ad__mdns_thread, a) != 0) a->mdns_running = 0;
    }
    if (!a->mdns_running) ad__log(a, 1, "mDNS unavailable%s: peers cannot be discovered", a->cfg.interface_name ? " (is the interface up? for awdl0 run OWL)" : "");
    return 0;
}
void airdrop_stop(airdrop_t* a) {
    if (!a || !a->started) return;
    a->running = 0;
    if (a->mdns_running) { a->mdns_running = 0; ad__thread_join(a->th_mdns); }
    if (a->mdns4 != AD_BADSOCK) { AD_CLOSESOCK(a->mdns4); a->mdns4 = AD_BADSOCK; }
    if (a->mdns6 != AD_BADSOCK) { AD_CLOSESOCK(a->mdns6); a->mdns6 = AD_BADSOCK; }
    ad__thread_join(a->th_accept);
    ad__mutex_lock(&a->lock);
    for (int i = 0; i < AD_MAX_CONN; i++) if (a->conns[i]) ad__shutdown_sock(a->conns[i]->io.s);
    ad__mutex_unlock(&a->lock);
    for (int i = 0; i < AD_MAX_CONN; i++) if (a->conns[i]) { ad__thread_join(a->conns[i]->th); free(a->conns[i]); a->conns[i] = NULL; }
    AD_CLOSESOCK(a->listen_sock); a->listen_sock = AD_BADSOCK;
    a->started = 0;
}
void airdrop_destroy(airdrop_t* a) {
    if (!a) return;
    airdrop_stop(a);
    if (ad__T.ready) {
        if (a->tls_server_ctx) ad__T.SSL_CTX_free(a->tls_server_ctx);
        if (a->tls_client_ctx) ad__T.SSL_CTX_free(a->tls_client_ctx);
        if (a->x509) ad__T.X509_free(a->x509);
        if (a->pkey) ad__T.EVP_PKEY_free(a->pkey);
    }
    ad__mutex_destroy(&a->lock);
    free(a);
}

/* ---- sender ---- */
static int ad__connect(airdrop_t* a, const char* host, int port, ad_io* io, char* err, size_t en) {
    struct addrinfo hints, *res = NULL;
    char ps[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(err, en, "cannot resolve %s", host); return -1; }
    ad_sock s = AD_BADSOCK;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == AD_BADSOCK) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        AD_CLOSESOCK(s); s = AD_BADSOCK;
    }
    freeaddrinfo(res);
    if (s == AD_BADSOCK) { snprintf(err, en, "cannot connect to %s:%d", host, port); return -1; }
    memset(io, 0, sizeof(*io));
    io->s = s;
    io->ssl = ad__T.SSL_new(a->tls_client_ctx);
    if (!io->ssl || !ad__T.SSL_set_fd(io->ssl, (int)s) || ad__T.SSL_connect(io->ssl) != 1) { snprintf(err, en, "TLS handshake with %s failed", host); ad__io_close(io); return -1; }
    ad__set_timeout(s, 30000);
    return 0;
}
static int ad__post(ad_io* io, const char* path, const uint8_t* body, size_t n, ad__http* resp, uint8_t** rbody, size_t* rn) {
    char h[400];
    int k = snprintf(h, sizeof(h), "POST %s HTTP/1.1\r\nHost: airdrop\r\nContent-Type: application/octet-stream\r\nConnection: keep-alive\r\nAccept: */*\r\nUser-Agent: AirDrop/1.0\r\nAccept-Language: en-us\r\nContent-Length: %zu\r\n\r\n", path, n);
    if (ad__io_write(io, h, (size_t)k) || (n && ad__io_write(io, body, n))) return -1;
    int r;
    do { r = ad__http_head(io, resp, 1); } while (r == -2);
    if (r) return -1;
    ad__body b; ad__body_init(&b, io, resp);
    uint8_t* d = NULL; size_t dn = 0;
    if (ad__body_all(&b, &d, &dn, 8u << 20)) return -1;
    if (rbody) { *rbody = d; *rn = dn; } else free(d);
    return 0;
}
int airdrop_discover(airdrop_t* a, const char* host, int port, char* name, size_t name_n) {
    char err[160]; ad_io io; ad__http resp; uint8_t* rb = NULL; size_t rn = 0;
    if (!a || !a->started) return -1;
    if (ad__connect(a, host, port, &io, err, sizeof(err))) return -1;
    ad_pl* d = ad_pl_new(AD_PL_DICT);
    size_t n; uint8_t* b = ad_pl_serialize(d, &n); ad_pl_free(d);
    int rc = ad__post(&io, "/Discover", b, n, &resp, &rb, &rn);
    free(b);
    int ok = -1;
    if (rc == 0 && resp.status == 200 && rb) {
        ad_pl* r = ad_pl_parse(rb, rn);
        ad_pl* nm = r ? ad_pl_get(r, "ReceiverComputerName") : NULL;
        if (nm && nm->t == AD_PL_STR) { snprintf(name, name_n, "%s", (const char*)nm->d); ok = 0; }
        ad_pl_free(r);
    }
    free(rb);
    ad__io_close(&io);
    return ok;
}
int airdrop_send(airdrop_t* a, const char* host, int port, const char* const* paths, int npaths, char* err, size_t en) {
    char e[200] = "";
    ad_io io; ad__http resp; uint8_t* rb = NULL; size_t rn = 0;
    int ok = -1;
    if (!err) { err = e; en = sizeof(e); }
    err[0] = 0;
    if (!a || !a->started) { snprintf(err, en, "AirDrop is not running"); return -1; }
    if (ad__connect(a, host, port, &io, err, en)) return -1;
    /* 1. /Discover */
    ad_pl* d = ad_pl_new(AD_PL_DICT);
    size_t n; uint8_t* b = ad_pl_serialize(d, &n); ad_pl_free(d);
    int rc = ad__post(&io, "/Discover", b, n, &resp, &rb, &rn); free(b); free(rb); rb = NULL;
    if (rc || resp.status != 200) { snprintf(err, en, "the receiver is not accepting AirDrop from everyone"); goto done; }
    /* 2. /Ask */
    d = ad_pl_new(AD_PL_DICT);
    ad_pl_set(d, "SenderComputerName", ad_pl_str(a->name)); ad_pl_set(d, "BundleID", ad_pl_str("com.apple.finder"));
    ad_pl_set(d, "SenderModelName", ad_pl_str(a->model)); ad_pl_set(d, "SenderID", ad_pl_str(a->service_id)); ad_pl_set(d, "ConvertMediaFormats", ad_pl_bool(0));
    ad_pl* files = ad_pl_new(AD_PL_ARRAY);
    uint64_t sizes[64];
    if (npaths > 64) npaths = 64;
    for (int i = 0; i < npaths; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0 || !(st.st_mode & S_IFREG)) { snprintf(err, en, "cannot send %s (not a regular file)", paths[i]); ad_pl_free(files); ad_pl_free(d); goto done; }
        sizes[i] = (uint64_t)st.st_size;
        const char* base = strrchr(paths[i], '/');
#ifdef _WIN32
        const char* bs = strrchr(paths[i], '\\'); if (bs && (!base || bs > base)) base = bs;
#endif
        base = base ? base + 1 : paths[i];
        char bom[300]; snprintf(bom, sizeof(bom), "./%s", base);
        ad_pl* f = ad_pl_new(AD_PL_DICT);
        ad_pl_set(f, "FileName", ad_pl_str(base)); ad_pl_set(f, "FileType", ad_pl_str(ad__uti(base)));
        ad_pl_set(f, "FileBomPath", ad_pl_str(bom)); ad_pl_set(f, "FileIsDirectory", ad_pl_bool(0)); ad_pl_set(f, "ConvertMediaFormats", ad_pl_int(0));
        ad_pl_push(files, f);
    }
    ad_pl_set(d, "Files", files);
    b = ad_pl_serialize(d, &n); ad_pl_free(d);
    rc = ad__post(&io, "/Ask", b, n, &resp, &rb, &rn); free(b); free(rb); rb = NULL;
    if (rc) { snprintf(err, en, "the receiver closed the connection"); goto done; }
    if (resp.status != 200) { snprintf(err, en, resp.status == 406 ? "the receiver declined the transfer" : "the receiver refused the transfer (%d)", resp.status); goto done; }
    /* 3. /Upload: chunked gzip(cpio) */
    {
        char h[300];
        int k = snprintf(h, sizeof(h), "POST /Upload HTTP/1.1\r\nHost: airdrop\r\nContent-Type: application/x-cpio\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nUser-Agent: AirDrop/1.0\r\n\r\n");
        if (ad__io_write(&io, h, (size_t)k)) { snprintf(err, en, "write failed"); goto done; }
        ad__gzw* g = (ad__gzw*)calloc(1, sizeof(*g));
        if (!g) goto done;
        g->io = &io; g->ad = a;
        static const uint8_t gzh[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
        ad__gz_emit(g, gzh, 10);
        for (int i = 0; i < npaths && !g->err; i++) {
            FILE* f = fopen(paths[i], "rb");
            if (!f) { g->err = 1; break; }
            const char* base = strrchr(paths[i], '/'); base = base ? base + 1 : paths[i];
            char nm[300]; snprintf(nm, sizeof(nm), "./%s", base);
            ad__odc(g, nm, 0100644, sizes[i], (unsigned)(i + 1));
            uint8_t buf[32768]; uint64_t sent = 0; size_t r;
            while (!g->err && (r = fread(buf, 1, sizeof(buf), f)) > 0) { ad__gz_data(g, buf, r); sent += r; ad__emit_progress(a, base, sent, 1); }
            fclose(f);
            if (sent != sizes[i]) g->err = 1;                       /* file changed while sending: the archive would be corrupt */
        }
        ad__odc(g, "TRAILER!!!", 0, 0, 0);
        { uint8_t fin[5] = { 1, 0, 0, 0xff, 0xff }; ad__gz_emit(g, fin, 5); }       /* final empty stored block */
        uint8_t tr[8]; for (int i = 0; i < 4; i++) { tr[i] = (uint8_t)(g->crc >> (8 * i)); tr[4 + i] = (uint8_t)((uint32_t)g->total >> (8 * i)); }
        ad__gz_emit(g, tr, 8);
        if (g->n) { ad__chunk(g, g->buf, g->n); g->n = 0; }
        if (!g->err) ad__io_write(&io, "0\r\n\r\n", 5);
        int gerr = g->err;
        free(g);
        if (gerr) { snprintf(err, en, "sending failed"); goto done; }
        do { rc = ad__http_head(&io, &resp, 1); } while (rc == -2);
        if (rc || resp.status != 200) { snprintf(err, en, "the receiver rejected the upload"); goto done; }
        ok = 0;
    }
done:
    ad__io_close(&io);
    if (a->cb.on_done) a->cb.on_done(a->cb.user, ok == 0, 1);
    return ok;
}
int airdrop_peers(airdrop_t* a, airdrop_peer* out, int max) {
    if (!a) return 0;
    ad__mutex_lock(&a->lock);
    int n = a->npeers < max ? a->npeers : max;
    for (int i = 0; i < n; i++) out[i] = a->peers[i];
    ad__mutex_unlock(&a->lock);
    return n;
}

#endif /* AIRDROP_IMPLEMENTATION_ONCE */
#endif /* AIRDROP_IMPLEMENTATION */
