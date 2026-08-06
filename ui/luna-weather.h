/*
 * luna-weather.h — asynchronous Open-Meteo worker for Luna Shell
 *
 * Network, process, JSON and refresh scheduling work stays on one worker
 * thread.  The UI/render thread consumes completed snapshots only.
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

static char* luna_weather_curl_slurp(const char* url) {
    char cmd[1600];
    int written = snprintf(cmd, sizeof(cmd),
        "curl -sS --max-time 12 -A '%s' '%s' 2>/dev/null",
        g_luna_weather.user_agent[0] ? g_luna_weather.user_agent : "luna-shell/1.0",
        url ? url : "");
    if (written < 0 || (size_t)written >= sizeof(cmd)) return NULL;

    if (g_luna_weather.child_lock)
        g_luna_weather.child_lock(g_luna_weather.child_user);
    FILE* f = popen(cmd, "r");
    if (!f) {
        if (g_luna_weather.child_unlock)
            g_luna_weather.child_unlock(g_luna_weather.child_user);
        return NULL;
    }

    size_t cap = 8192, used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        (void)pclose(f);
        if (g_luna_weather.child_unlock)
            g_luna_weather.child_unlock(g_luna_weather.child_user);
        return NULL;
    }
    while (!feof(f)) {
        if (used + 2048 + 1 >= cap) {
            size_t next = cap * 2;
            char* grown = (char*)realloc(buf, next);
            if (!grown) {
                free(buf);
                (void)pclose(f);
                if (g_luna_weather.child_unlock)
                    g_luna_weather.child_unlock(g_luna_weather.child_user);
                return NULL;
            }
            buf = grown;
            cap = next;
        }
        size_t n = fread(buf + used, 1, 2048, f);
        if (!n) break;
        used += n;
    }
    int status = pclose(f);
    if (g_luna_weather.child_unlock)
        g_luna_weather.child_unlock(g_luna_weather.child_user);
    if (status == -1 && used == 0) {
        free(buf);
        return NULL;
    }
    buf[used] = 0;
    return buf;
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

    const char* current = strstr(weather, "\"current\"");
    const char* daily = strstr(weather, "\"daily\"");
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
    if (!out->ok) snprintf(out->err, sizeof(out->err), "Invalid weather data");
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
        result.generation = g_luna_weather.result.generation + 1;
        g_luna_weather.result = result;
        g_luna_weather.busy = 0;
        snprintf(g_luna_weather.refresh_city,
                 sizeof(g_luna_weather.refresh_city), "%s", city);
        g_luna_weather.next_refresh = luna_weather_now() +
                                      g_luna_weather.refresh_interval_sec;
        pthread_mutex_unlock(&g_luna_weather.mutex);
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
