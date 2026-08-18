/*
 * luna-weather.h — asynchronous Open-Meteo worker for Luna Shell
 *
 * Network (native HTTPS via OpenSSL), JSON and refresh scheduling stay on
 * one worker thread.  The UI/render thread consumes completed snapshots only.
 *
 * Define LUNA_WEATHER_IMPLEMENTATION in exactly one translation unit before
 * including this file.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */
#ifndef LUNA_WEATHER_H
#define LUNA_WEATHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_WEATHER_FORECAST_DAYS
#define LUNA_WEATHER_FORECAST_DAYS 5
#endif

typedef void (*LunaWeatherNotifyFn)(void* user);
typedef void (*LunaWeatherChildLockFn)(void* user);

typedef struct LunaWeatherData {
    char query_city[64];
    char city[64];
    char country[64];
    int code;
    float temp;
    float feels;
    float humidity;
    float wind;
    struct {
        char date[16];
        int code;
        float tmax;
        float tmin;
    } days[LUNA_WEATHER_FORECAST_DAYS];
    int ok;
    char err[96];
    unsigned long long generation;
} LunaWeatherData;

typedef struct LunaWeatherConfig {
    const char* initial_city;
    const char* user_agent;
    double refresh_interval_sec; /* <= 0 uses 30 minutes */
    const char* cache_path;      /* optional last-good snapshot; NULL disables */
    LunaWeatherNotifyFn notify;
    void* notify_user;
    /* Optional guard shared with a SIGCHLD waitpid(-1) reaper. */
    LunaWeatherChildLockFn child_lock;
    LunaWeatherChildLockFn child_unlock;
    void* child_user;
} LunaWeatherConfig;

int  luna_weather_init(const LunaWeatherConfig* config);
void luna_weather_shutdown(void);
void luna_weather_request(const char* city);
int  luna_weather_consume(LunaWeatherData* out);
int  luna_weather_busy(void);

#ifdef __cplusplus
}
#endif
#endif /* LUNA_WEATHER_H */

#ifdef LUNA_WEATHER_IMPLEMENTATION
#ifndef LUNA_WEATHER_IMPLEMENTATION_ONCE
#define LUNA_WEATHER_IMPLEMENTATION_ONCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

#ifndef LUNA_WEATHER_DEFAULT_REFRESH_SEC
#define LUNA_WEATHER_DEFAULT_REFRESH_SEC 1800.0
#endif
#ifndef LUNA_WEATHER_RETRY_MIN_SEC
#define LUNA_WEATHER_RETRY_MIN_SEC 8.0
#endif
#ifndef LUNA_WEATHER_RETRY_MAX_SEC
#define LUNA_WEATHER_RETRY_MAX_SEC 300.0
#endif
#define LUNA_WEATHER_CACHE_MAGIC 0x31584C57u /* 'WLX1' */

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef LUNA_WEATHER_HTTP_TIMEOUT_SEC
#define LUNA_WEATHER_HTTP_TIMEOUT_SEC 12
#endif
#ifndef LUNA_WEATHER_HTTP_MAX_BODY
#define LUNA_WEATHER_HTTP_MAX_BODY (256u * 1024u)
#endif

typedef struct LunaWeatherState {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int cond_uses_monotonic;
    int initialized;
    int running;
    int busy;
    char request_city[64];
    char active_city[64];
    char refresh_city[64];
    unsigned long long request_generation;
    unsigned long long handled_request_generation;
    unsigned long long consumed_result_generation;
    LunaWeatherData result;
    double next_refresh;
    double refresh_interval_sec;
    char user_agent[96];
    LunaWeatherNotifyFn notify;
    void* notify_user;
    LunaWeatherChildLockFn child_lock;
    LunaWeatherChildLockFn child_unlock;
    void* child_user;
    char cache_path[512];
    int fail_streak;
} LunaWeatherState;

static LunaWeatherState g_luna_weather;

static double luna_weather_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static struct timespec luna_weather_deadline(double monotonic_when) {
    struct timespec ts;
    double when = monotonic_when;
    if (!g_luna_weather.cond_uses_monotonic) {
        struct timespec realtime;
        if (clock_gettime(CLOCK_REALTIME, &realtime) != 0) {
            realtime.tv_sec = time(NULL);
            realtime.tv_nsec = 0;
        }
        double delay = monotonic_when - luna_weather_now();
        if (delay < 0.0) delay = 0.0;
        when = (double)realtime.tv_sec +
               (double)realtime.tv_nsec / 1000000000.0 + delay;
    }
    if (when < 0.0) when = 0.0;
    ts.tv_sec = (time_t)when;
    ts.tv_nsec = (long)((when - (double)ts.tv_sec) * 1000000000.0);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void luna_weather_notify(void) {
    if (g_luna_weather.notify)
        g_luna_weather.notify(g_luna_weather.notify_user);
}

static void luna_weather_trim_copy(char out[64], const char* in) {
    if (!in) in = "";
    while (*in && isspace((unsigned char)*in)) in++;
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) len--;
    if (len >= 64) len = 63;
    memcpy(out, in, len);
    out[len] = 0;
}

static void luna_weather_url_encode(const char* s, char* out, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; s && *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static int luna_weather_looks_json(const char* s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '{';
}

static int luna_weather_parse_url(const char* url, char* host, size_t host_n,
                                  char* path, size_t path_n, int* port) {
    if (!url || !host || host_n < 8 || !path || path_n < 2 || !port) return 0;
    if (strncmp(url, "https://", 8) != 0) return 0;
    const char* p = url + 8;
    const char* slash = strchr(p, '/');
    char* colon = NULL;
    size_t host_len;
    if (slash) host_len = (size_t)(slash - p);
    else host_len = strlen(p);
    if (host_len == 0 || host_len >= host_n) return 0;
    memcpy(host, p, host_len);
    host[host_len] = 0;
    colon = strchr(host, ':');
    *port = 443;
    if (colon) {
        *colon = 0;
        int parsed = atoi(colon + 1);
        if (parsed <= 0 || parsed > 65535) return 0;
        *port = parsed;
    }
    if (slash) {
        if (strlen(slash) >= path_n) return 0;
        snprintf(path, path_n, "%s", slash);
    } else {
        snprintf(path, path_n, "/");
    }
    return host[0] != 0;
}

static int luna_weather_wait_fd(int fd, int for_write, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = for_write ? POLLOUT : POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return 0;
    return (pfd.revents & (for_write ? POLLOUT : POLLIN)) != 0;
}

static int luna_weather_ssl_io(SSL* ssl, int fd, int writing,
                               const void* out, size_t out_n,
                               void* in, size_t in_n, size_t* in_got,
                               int timeout_ms) {
    double start = luna_weather_now();
    size_t done = 0;
    for (;;) {
        int n;
        if (writing)
            n = SSL_write(ssl, (const char*)out + done, (int)(out_n - done));
        else
            n = SSL_read(ssl, (char*)in + done, (int)(in_n - done));
        if (n > 0) {
            done += (size_t)n;
            if (writing) {
                if (done >= out_n) return 1;
                continue;
            }
            if (in_got) *in_got = done;
            return 1;
        }
        int err = SSL_get_error(ssl, n);
        double remain = (double)timeout_ms / 1000.0 - (luna_weather_now() - start);
        if (remain <= 0.0) return 0;
        int wait_ms = (int)(remain * 1000.0);
        if (wait_ms < 1) wait_ms = 1;
        if (err == SSL_ERROR_WANT_READ) {
            if (!luna_weather_wait_fd(fd, 0, wait_ms)) return 0;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (!luna_weather_wait_fd(fd, 1, wait_ms)) return 0;
        } else if (!writing && (err == SSL_ERROR_ZERO_RETURN ||
                                err == SSL_ERROR_SYSCALL)) {
            if (in_got) *in_got = done;
            return done > 0 || err == SSL_ERROR_ZERO_RETURN;
        } else {
            return 0;
        }
    }
}

static int luna_weather_decode_chunked(char* body, size_t* len) {
    if (!body || !len) return 0;
    char* src = body;
    char* dst = body;
    char* end = body + *len;
    while (src < end) {
        char* nl = memchr(src, '\n', (size_t)(end - src));
        if (!nl) return 0;
        unsigned long chunk = strtoul(src, NULL, 16);
        src = nl + 1;
        if (chunk == 0) {
            *len = (size_t)(dst - body);
            dst[*len] = 0;
            return 1;
        }
        if (src + chunk > end) return 0;
        memmove(dst, src, chunk);
        dst += chunk;
        src += chunk;
        if (src < end && *src == '\r') src++;
        if (src < end && *src == '\n') src++;
    }
    return 0;
}

static char* luna_weather_https_get(const char* url) {
    char host[256], path[768];
    int port = 443;
    if (!luna_weather_parse_url(url, host, sizeof(host), path, sizeof(path), &port))
        return NULL;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return NULL;

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno == EINPROGRESS &&
            luna_weather_wait_fd(fd, 1, LUNA_WEATHER_HTTP_TIMEOUT_SEC * 1000)) {
            int err = 0;
            socklen_t errlen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); return NULL; }
    int have_ca = SSL_CTX_set_default_verify_paths(ctx) == 1;
    SSL_CTX_set_verify(ctx, have_ca ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); close(fd); return NULL; }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (have_ca) SSL_set1_host(ssl, host);
#endif
    SSL_set_connect_state(ssl);

    double handshake_start = luna_weather_now();
    int handshake_ok = 0;
    for (;;) {
        int rc = SSL_connect(ssl);
        if (rc == 1) { handshake_ok = 1; break; }
        int err = SSL_get_error(ssl, rc);
        double remain = (double)LUNA_WEATHER_HTTP_TIMEOUT_SEC -
                        (luna_weather_now() - handshake_start);
        if (remain <= 0.0) break;
        int wait_ms = (int)(remain * 1000.0);
        if (wait_ms < 1) break;
        if (err == SSL_ERROR_WANT_READ) {
            if (!luna_weather_wait_fd(fd, 0, wait_ms)) break;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (!luna_weather_wait_fd(fd, 1, wait_ms)) break;
        } else {
            break;
        }
    }
    if (!handshake_ok) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return NULL;
    }

    const char* ua = g_luna_weather.user_agent[0]
        ? g_luna_weather.user_agent : "luna-shell/1.0";
    char req[1536];
    int req_n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, ua);
    if (req_n <= 0 || (size_t)req_n >= sizeof(req) ||
        !luna_weather_ssl_io(ssl, fd, 1, req, (size_t)req_n, NULL, 0, NULL,
                             LUNA_WEATHER_HTTP_TIMEOUT_SEC * 1000)) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return NULL;
    }

    size_t cap = 8192, used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return NULL; }
    double read_start = luna_weather_now();
    for (;;) {
        if (used + 2048 + 1 >= cap) {
            size_t next = cap * 2;
            if (next > LUNA_WEATHER_HTTP_MAX_BODY) next = LUNA_WEATHER_HTTP_MAX_BODY;
            if (next <= cap) break;
            char* grown = (char*)realloc(buf, next);
            if (!grown) { free(buf); SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return NULL; }
            buf = grown;
            cap = next;
        }
        double remain = (double)LUNA_WEATHER_HTTP_TIMEOUT_SEC -
                        (luna_weather_now() - read_start);
        if (remain <= 0.0) break;
        int wait_ms = (int)(remain * 1000.0);
        if (wait_ms < 1) break;
        size_t got = 0;
        size_t room = cap - used - 1;
        if (room > 4096) room = 4096;
        if (!luna_weather_ssl_io(ssl, fd, 0, NULL, 0, buf + used, room, &got, wait_ms))
            break;
        if (!got) break;
        used += got;
        if (used + 1 >= LUNA_WEATHER_HTTP_MAX_BODY) break;
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    if (used == 0) { free(buf); return NULL; }
    buf[used] = 0;

    char* header_end = strstr(buf, "\r\n\r\n");
    int header_skip = 4;
    if (!header_end) {
        header_end = strstr(buf, "\n\n");
        header_skip = 2;
    }
    if (!header_end) { free(buf); return NULL; }

    int status = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1 || status < 200 || status >= 300) {
        free(buf);
        return NULL;
    }
    int chunked = 0;
    char* scan = buf;
    while (scan < header_end) {
        char* line = strstr(scan, "\n");
        if (!line || line > header_end) break;
        if (!strncasecmp(scan, "Transfer-Encoding:", 18)) {
            const char* p = scan + 18;
            while (p + 7 < line) {
                if ((p[0] == 'c' || p[0] == 'C') &&
                    (p[1] == 'h' || p[1] == 'H') &&
                    (p[2] == 'u' || p[2] == 'U') &&
                    (p[3] == 'n' || p[3] == 'N') &&
                    (p[4] == 'k' || p[4] == 'K') &&
                    (p[5] == 'e' || p[5] == 'E') &&
                    (p[6] == 'd' || p[6] == 'D')) {
                    chunked = 1;
                    break;
                }
                p++;
            }
        }
        scan = line + 1;
    }

    size_t body_len = used - (size_t)(header_end + header_skip - buf);
    memmove(buf, header_end + header_skip, body_len + 1);
    if (chunked && !luna_weather_decode_chunked(buf, &body_len)) {
        free(buf);
        return NULL;
    }
    buf[body_len] = 0;
    if (!luna_weather_looks_json(buf)) {
        free(buf);
        return NULL;
    }
    return buf;
}

static char* luna_weather_curl_slurp(const char* url) {
    return luna_weather_https_get(url);
}

static void luna_weather_cache_save(const LunaWeatherData* data) {
    if (!data || !data->ok || !g_luna_weather.cache_path[0]) return;
    char tmp[576];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", g_luna_weather.cache_path) >= (int)sizeof(tmp))
        return;
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    unsigned magic = LUNA_WEATHER_CACHE_MAGIC;
    int ok = fwrite(&magic, sizeof(magic), 1, f) == 1 &&
             fwrite(data, sizeof(*data), 1, f) == 1;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, g_luna_weather.cache_path) != 0)
        unlink(tmp);
}

static int luna_weather_cache_load(LunaWeatherData* out) {
    if (!out || !g_luna_weather.cache_path[0]) return 0;
    FILE* f = fopen(g_luna_weather.cache_path, "rb");
    if (!f) return 0;
    unsigned magic = 0;
    LunaWeatherData data;
    int ok = fread(&magic, sizeof(magic), 1, f) == 1 &&
             magic == LUNA_WEATHER_CACHE_MAGIC &&
             fread(&data, sizeof(data), 1, f) == 1 &&
             data.ok;
    fclose(f);
    if (!ok) return 0;
    data.generation = 1;
    data.err[0] = 0;
    *out = data;
    return 1;
}

static double luna_weather_backoff_sec(int fail_streak) {
    int exp = fail_streak - 1;
    if (exp < 0) exp = 0;
    if (exp > 6) exp = 6;
    double delay = LUNA_WEATHER_RETRY_MIN_SEC * (double)(1 << exp);
    if (delay > LUNA_WEATHER_RETRY_MAX_SEC) delay = LUNA_WEATHER_RETRY_MAX_SEC;
    return delay;
}

static const char* luna_weather_json_after_key(const char* j, const char* key) {
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(j, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return p;
}

static int luna_weather_json_num(const char* j, const char* key, double* out) {
    const char* p = luna_weather_json_after_key(j, key);
    if (!p) return 0;
    char* end = NULL;
    *out = strtod(p, &end);
    return end && end != p;
}

static int luna_weather_json_str(const char* j, const char* key,
                                 char* out, size_t n) {
    const char* p = luna_weather_json_after_key(j, key);
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static int luna_weather_json_num_array(const char* j, const char* key,
                                       double* out, int max) {
    const char* p = luna_weather_json_after_key(j, key);
    if (!p || *p != '[') return 0;
    p++;
    int count = 0;
    while (count < max && *p && *p != ']') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']') break;
        char* end = NULL;
        out[count] = strtod(p, &end);
        if (!end || end == p) break;
        p = end;
        count++;
    }
    return count;
}

static int luna_weather_json_str_array(const char* j, const char* key,
                                       char out[][16], int max) {
    const char* p = luna_weather_json_after_key(j, key);
    if (!p || *p != '[') return 0;
    p++;
    int count = 0;
    while (count < max && *p && *p != ']') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') break;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof(out[0]))
            out[count][i++] = *p++;
        out[count][i] = 0;
        if (*p == '"') p++;
        count++;
    }
    return count;
}

static void luna_weather_fetch(const char* city, LunaWeatherData* out) {
    memset(out, 0, sizeof(*out));
    luna_weather_trim_copy(out->query_city, city);
    if (!out->query_city[0]) {
        snprintf(out->err, sizeof(out->err), "Enter a city");
        return;
    }

    char encoded[192], url[768];
    luna_weather_url_encode(out->query_city, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
        encoded);
    char* geo = luna_weather_curl_slurp(url);
    if (!geo || !geo[0]) {
        free(geo);
        snprintf(out->err, sizeof(out->err), "Network error (geocode)");
        return;
    }
    if (!strstr(geo, "\"results\"")) {
        free(geo);
        snprintf(out->err, sizeof(out->err), "City not found");
        return;
    }

    double lat = 0.0, lon = 0.0;
    if (!luna_weather_json_num(geo, "latitude", &lat) ||
        !luna_weather_json_num(geo, "longitude", &lon)) {
        free(geo);
        snprintf(out->err, sizeof(out->err), "City not found");
        return;
    }
    (void)luna_weather_json_str(geo, "name", out->city, sizeof(out->city));
    (void)luna_weather_json_str(geo, "country", out->country, sizeof(out->country));
    free(geo);

    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?"
        "latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,apparent_temperature"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&timezone=auto&forecast_days=7",
        lat, lon);
    char* weather = luna_weather_curl_slurp(url);
    if (!weather || !weather[0]) {
        free(weather);
        snprintf(out->err, sizeof(out->err), "Network error (forecast)");
        return;
    }

    const char* current = strstr(weather, "\"current\":");
    const char* daily = strstr(weather, "\"daily\":");
    double value = 0.0;
    if (current) {
        if (luna_weather_json_num(current, "temperature_2m", &value)) out->temp = (float)value;
        if (luna_weather_json_num(current, "apparent_temperature", &value)) out->feels = (float)value;
        if (luna_weather_json_num(current, "relative_humidity_2m", &value)) out->humidity = (float)value;
        if (luna_weather_json_num(current, "wind_speed_10m", &value)) out->wind = (float)value;
        if (luna_weather_json_num(current, "weather_code", &value)) out->code = (int)value;
    }

    if (daily) {
        char dates[LUNA_WEATHER_FORECAST_DAYS][16] = {{0}};
        double codes[LUNA_WEATHER_FORECAST_DAYS] = {0};
        double tmax[LUNA_WEATHER_FORECAST_DAYS] = {0};
        double tmin[LUNA_WEATHER_FORECAST_DAYS] = {0};
        int nd = luna_weather_json_str_array(daily, "time", dates,
                                             LUNA_WEATHER_FORECAST_DAYS);
        int nc = luna_weather_json_num_array(daily, "weather_code", codes,
                                             LUNA_WEATHER_FORECAST_DAYS);
        int nx = luna_weather_json_num_array(daily, "temperature_2m_max", tmax,
                                             LUNA_WEATHER_FORECAST_DAYS);
        int nn = luna_weather_json_num_array(daily, "temperature_2m_min", tmin,
                                             LUNA_WEATHER_FORECAST_DAYS);
        for (int i = 0; i < LUNA_WEATHER_FORECAST_DAYS; i++) {
            if (i < nd) snprintf(out->days[i].date, sizeof(out->days[i].date), "%s", dates[i]);
            if (i < nc) out->days[i].code = (int)codes[i];
            if (i < nx) out->days[i].tmax = (float)tmax[i];
            if (i < nn) out->days[i].tmin = (float)tmin[i];
        }
    }

    free(weather);
    out->ok = current != NULL;
    if (!out->ok)
        snprintf(out->err, sizeof(out->err), "Invalid weather data");
    else if (!out->days[0].date[0])
        snprintf(out->err, sizeof(out->err), "Forecast unavailable");
}

static void* luna_weather_thread_main(void* unused) {
    (void)unused;
    for (;;) {
        char city[64] = "";

        pthread_mutex_lock(&g_luna_weather.mutex);
        while (g_luna_weather.running) {
            double now = luna_weather_now();
            if (g_luna_weather.request_generation !=
                g_luna_weather.handled_request_generation) {
                snprintf(city, sizeof(city), "%s", g_luna_weather.request_city);
                g_luna_weather.handled_request_generation =
                    g_luna_weather.request_generation;
                break;
            }
            if (g_luna_weather.refresh_city[0] &&
                g_luna_weather.next_refresh > 0.0 &&
                now >= g_luna_weather.next_refresh) {
                snprintf(city, sizeof(city), "%s", g_luna_weather.refresh_city);
                break;
            }

            if (g_luna_weather.refresh_city[0] && g_luna_weather.next_refresh > 0.0) {
                struct timespec until = luna_weather_deadline(g_luna_weather.next_refresh);
                int rc = pthread_cond_timedwait(&g_luna_weather.cond,
                                                &g_luna_weather.mutex, &until);
                if (rc != 0 && rc != ETIMEDOUT) break;
            } else {
                (void)pthread_cond_wait(&g_luna_weather.cond,
                                        &g_luna_weather.mutex);
            }
        }
        if (!g_luna_weather.running) {
            pthread_mutex_unlock(&g_luna_weather.mutex);
            break;
        }
        g_luna_weather.busy = 1;
        snprintf(g_luna_weather.active_city,
                 sizeof(g_luna_weather.active_city), "%s", city);
        pthread_mutex_unlock(&g_luna_weather.mutex);
        luna_weather_notify();

        LunaWeatherData result;
        luna_weather_fetch(city, &result);

        pthread_mutex_lock(&g_luna_weather.mutex);
        double delay;
        int save_cache = 0;
        LunaWeatherData cached;
        memset(&cached, 0, sizeof(cached));
        if (result.ok) {
            result.generation = g_luna_weather.result.generation + 1;
            g_luna_weather.result = result;
            cached = g_luna_weather.result;
            save_cache = result.days[0].date[0] != 0;
            if (result.days[0].date[0]) {
                g_luna_weather.fail_streak = 0;
                delay = g_luna_weather.refresh_interval_sec;
            } else {
                /* Current weather arrived without a forecast — retry soon. */
                g_luna_weather.fail_streak++;
                delay = luna_weather_backoff_sec(g_luna_weather.fail_streak);
            }
        } else if (!g_luna_weather.result.ok) {
            result.generation = g_luna_weather.result.generation + 1;
            g_luna_weather.result = result;
            g_luna_weather.fail_streak++;
            delay = luna_weather_backoff_sec(g_luna_weather.fail_streak);
        } else {
            /* Keep the last good snapshot so the UI is not wiped to empty. */
            g_luna_weather.result.generation++;
            g_luna_weather.fail_streak++;
            delay = luna_weather_backoff_sec(g_luna_weather.fail_streak);
        }
        g_luna_weather.busy = 0;
        snprintf(g_luna_weather.refresh_city,
                 sizeof(g_luna_weather.refresh_city), "%s", city);
        g_luna_weather.next_refresh = luna_weather_now() + delay;
        pthread_mutex_unlock(&g_luna_weather.mutex);
        if (save_cache) luna_weather_cache_save(&cached);
        luna_weather_notify();
    }
    return NULL;
}

int luna_weather_init(const LunaWeatherConfig* config) {
    if (g_luna_weather.initialized) return 1;
    memset(&g_luna_weather, 0, sizeof(g_luna_weather));
    g_luna_weather.refresh_interval_sec =
        config && config->refresh_interval_sec > 0.0
            ? config->refresh_interval_sec : LUNA_WEATHER_DEFAULT_REFRESH_SEC;
    snprintf(g_luna_weather.user_agent, sizeof(g_luna_weather.user_agent), "%s",
             config && config->user_agent ? config->user_agent : "luna-shell/1.0");
    if (config) {
        g_luna_weather.notify = config->notify;
        g_luna_weather.notify_user = config->notify_user;
        g_luna_weather.child_lock = config->child_lock;
        g_luna_weather.child_unlock = config->child_unlock;
        g_luna_weather.child_user = config->child_user;
        if (config->cache_path && config->cache_path[0])
            snprintf(g_luna_weather.cache_path, sizeof(g_luna_weather.cache_path),
                     "%s", config->cache_path);
    }
    if (luna_weather_cache_load(&g_luna_weather.result)) {
        if (g_luna_weather.result.query_city[0])
            snprintf(g_luna_weather.refresh_city,
                     sizeof(g_luna_weather.refresh_city), "%s",
                     g_luna_weather.result.query_city);
    }

    if (pthread_mutex_init(&g_luna_weather.mutex, NULL) != 0) return 0;
    pthread_condattr_t attr;
    int attr_initialized = pthread_condattr_init(&attr) == 0;
    int use_attr = 0;
#if defined(CLOCK_MONOTONIC)
    if (attr_initialized &&
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
        use_attr = 1;
        g_luna_weather.cond_uses_monotonic = 1;
    }
#endif
    if (pthread_cond_init(&g_luna_weather.cond, use_attr ? &attr : NULL) != 0) {
        if (attr_initialized) pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&g_luna_weather.mutex);
        return 0;
    }
    if (attr_initialized) pthread_condattr_destroy(&attr);

    g_luna_weather.running = 1;
    g_luna_weather.initialized = 1;
    if (config && config->initial_city && config->initial_city[0]) {
        luna_weather_trim_copy(g_luna_weather.request_city, config->initial_city);
        g_luna_weather.request_generation = 1;
    }
    if (pthread_create(&g_luna_weather.thread, NULL,
                       luna_weather_thread_main, NULL) != 0) {
        g_luna_weather.initialized = 0;
        g_luna_weather.running = 0;
        pthread_cond_destroy(&g_luna_weather.cond);
        pthread_mutex_destroy(&g_luna_weather.mutex);
        return 0;
    }
    if (g_luna_weather.request_generation)
        pthread_cond_signal(&g_luna_weather.cond);
    return 1;
}

void luna_weather_shutdown(void) {
    if (!g_luna_weather.initialized) return;
    pthread_mutex_lock(&g_luna_weather.mutex);
    g_luna_weather.running = 0;
    pthread_cond_broadcast(&g_luna_weather.cond);
    pthread_mutex_unlock(&g_luna_weather.mutex);
    pthread_join(g_luna_weather.thread, NULL);
    pthread_cond_destroy(&g_luna_weather.cond);
    pthread_mutex_destroy(&g_luna_weather.mutex);
    memset(&g_luna_weather, 0, sizeof(g_luna_weather));
}

void luna_weather_request(const char* city) {
    if (!g_luna_weather.initialized) return;
    char trimmed[64];
    luna_weather_trim_copy(trimmed, city);
    if (!trimmed[0]) return;
    pthread_mutex_lock(&g_luna_weather.mutex);
    snprintf(g_luna_weather.request_city,
             sizeof(g_luna_weather.request_city), "%s", trimmed);
    g_luna_weather.request_generation++;
    pthread_cond_signal(&g_luna_weather.cond);
    pthread_mutex_unlock(&g_luna_weather.mutex);
}

int luna_weather_consume(LunaWeatherData* out) {
    if (!out || !g_luna_weather.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_weather.mutex) != 0) return 0;
    if (g_luna_weather.result.generation == 0 ||
        g_luna_weather.result.generation ==
            g_luna_weather.consumed_result_generation) {
        pthread_mutex_unlock(&g_luna_weather.mutex);
        return 0;
    }
    *out = g_luna_weather.result;
    g_luna_weather.consumed_result_generation = out->generation;
    pthread_mutex_unlock(&g_luna_weather.mutex);
    return 1;
}

int luna_weather_busy(void) {
    if (!g_luna_weather.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_weather.mutex) != 0) return 1;
    int busy = g_luna_weather.busy ||
               g_luna_weather.request_generation !=
                   g_luna_weather.handled_request_generation;
    pthread_mutex_unlock(&g_luna_weather.mutex);
    return busy;
}

#endif /* LUNA_WEATHER_IMPLEMENTATION_ONCE */
#endif /* LUNA_WEATHER_IMPLEMENTATION */
