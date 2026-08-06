/*
 * luna-monitor.h — asynchronous system/widget monitor for Luna Shell
 *
 * The worker owns all periodic procfs/sysfs/filesystem reads.  The render
 * thread only consumes immutable snapshots and never waits for worker I/O.
 *
 * Define LUNA_MONITOR_IMPLEMENTATION in exactly one translation unit before
 * including this file.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */
#ifndef LUNA_MONITOR_H
#define LUNA_MONITOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_MONITOR_STATE_MAX
#define LUNA_MONITOR_STATE_MAX (256u * 1024u)
#endif

typedef void (*LunaMonitorNotifyFn)(void* user);

typedef struct LunaMonitorSnapshot {
    float cpu;
    int memory;
    int battery;
    char network[16];
    float disk_used;
    double disk_free_gb;
    int disk_valid;
    char mb_clock[64];
    char widget_time[32];
    char widget_date[64];
    unsigned long long generation;
} LunaMonitorSnapshot;

typedef struct LunaMonitorConfig {
    const char* state_path;
    double status_interval_sec; /* <= 0 uses 1 second */
    LunaMonitorNotifyFn notify;
    void* notify_user;
} LunaMonitorConfig;

int  luna_monitor_init(const LunaMonitorConfig* config);
void luna_monitor_shutdown(void);
void luna_monitor_request_state(void);
int  luna_monitor_consume_status(LunaMonitorSnapshot* out);
int  luna_monitor_consume_state(char* out, size_t out_n, size_t* len_out);

/* Intended for one-shot About/System Information population, not periodic UI. */
unsigned long luna_monitor_memory_total_kb(void);

#ifdef __cplusplus
}
#endif
#endif /* LUNA_MONITOR_H */

#ifdef LUNA_MONITOR_IMPLEMENTATION
#ifndef LUNA_MONITOR_IMPLEMENTATION_ONCE
#define LUNA_MONITOR_IMPLEMENTATION_ONCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct LunaMonitorState {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int cond_uses_monotonic;
    int initialized;
    int running;
    int state_requested;
    char state_path[PATH_MAX];
    char state_text[LUNA_MONITOR_STATE_MAX];
    size_t state_len;
    unsigned long long state_generation;
    unsigned long long consumed_state_generation;
    off_t state_size;
    struct timespec state_mtime;
    LunaMonitorSnapshot status;
    unsigned long long consumed_status_generation;
    double status_interval_sec;
    double last_power_net;
    double last_disk;
    LunaMonitorNotifyFn notify;
    void* notify_user;
    unsigned long long cpu_prev_idle;
    unsigned long long cpu_prev_total;
    int cpu_initialized;
} LunaMonitorState;

static LunaMonitorState g_luna_monitor;

static double luna_monitor_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static struct timespec luna_monitor_deadline(double monotonic_when) {
    struct timespec ts;
    double when = monotonic_when;
    if (!g_luna_monitor.cond_uses_monotonic) {
        struct timespec realtime;
        if (clock_gettime(CLOCK_REALTIME, &realtime) != 0) {
            realtime.tv_sec = time(NULL);
            realtime.tv_nsec = 0;
        }
        double delay = monotonic_when - luna_monitor_now();
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

static void luna_monitor_notify(void) {
    if (g_luna_monitor.notify)
        g_luna_monitor.notify(g_luna_monitor.notify_user);
}

static float luna_monitor_read_cpu_percent(void) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0.0f;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return 0.0f;

    unsigned long long idle = v[3] + v[4];
    unsigned long long total = 0;
    for (int i = 0; i < 8; i++) total += v[i];

    if (!g_luna_monitor.cpu_initialized) {
        g_luna_monitor.cpu_prev_idle = idle;
        g_luna_monitor.cpu_prev_total = total;
        g_luna_monitor.cpu_initialized = 1;
        return 0.0f;
    }

    unsigned long long didle = idle - g_luna_monitor.cpu_prev_idle;
    unsigned long long dtotal = total - g_luna_monitor.cpu_prev_total;
    g_luna_monitor.cpu_prev_idle = idle;
    g_luna_monitor.cpu_prev_total = total;
    if (dtotal == 0 || didle > dtotal) return 0.0f;
    return 100.0f * (float)(dtotal - didle) / (float)dtotal;
}

static int luna_monitor_read_memory(unsigned long* total_kb_out) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) {
        if (total_kb_out) *total_kb_out = 0;
        return 0;
    }
    unsigned long total = 0, avail = 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        (void)sscanf(line, "MemTotal: %lu kB", &total);
        (void)sscanf(line, "MemAvailable: %lu kB", &avail);
    }
    fclose(f);
    if (total_kb_out) *total_kb_out = total;
    if (!total) return 0;
    return (int)(100.0 * (double)(total - avail) / (double)total);
}

unsigned long luna_monitor_memory_total_kb(void) {
    unsigned long total = 0;
    (void)luna_monitor_read_memory(&total);
    return total;
}

static int luna_monitor_read_battery_percent(void) {
    static const char* paths[] = {
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/BAT1/capacity",
        "/sys/class/power_supply/BAT2/capacity",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        int pct = -1;
        if (fscanf(f, "%d", &pct) != 1) pct = -1;
        fclose(f);
        if (pct >= 0) return pct;
    }
    return -1;
}

static void luna_monitor_read_network(char out[16]) {
    DIR* d = opendir("/sys/class/net");
    if (!d) {
        snprintf(out, 16, "Offline");
        return;
    }
    struct dirent* de;
    int wired_up = 0, wifi_up = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo")) continue;
        char path[PATH_MAX];
        int written = snprintf(path, sizeof(path),
                               "/sys/class/net/%s/operstate", de->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char state[24] = "";
        if (!fgets(state, sizeof(state), f)) state[0] = 0;
        fclose(f);
        if (strncmp(state, "up", 2)) continue;
        written = snprintf(path, sizeof(path),
                           "/sys/class/net/%s/wireless", de->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) == 0) wifi_up = 1;
        else wired_up = 1;
    }
    closedir(d);
    snprintf(out, 16, "%s", wifi_up ? "Wi-Fi" : wired_up ? "Ethernet" : "Offline");
}

static void luna_monitor_publish_status(void) {
    LunaMonitorSnapshot snap;
    pthread_mutex_lock(&g_luna_monitor.mutex);
    snap = g_luna_monitor.status;
    pthread_mutex_unlock(&g_luna_monitor.mutex);

    double now_mono = luna_monitor_now();

    snap.cpu = luna_monitor_read_cpu_percent();
    snap.memory = luna_monitor_read_memory(NULL);

    if (now_mono - g_luna_monitor.last_power_net >= 5.0) {
        snap.battery = luna_monitor_read_battery_percent();
        luna_monitor_read_network(snap.network);
        g_luna_monitor.last_power_net = now_mono;
    }

    if (now_mono - g_luna_monitor.last_disk >= 30.0) {
        struct statvfs vfs;
        snap.disk_valid = 0;
        if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
            double total = (double)vfs.f_blocks * (double)vfs.f_frsize;
            double avail = (double)vfs.f_bavail * (double)vfs.f_frsize;
            snap.disk_used = (float)(100.0 * (total - avail) / total);
            snap.disk_free_gb = avail / (1024.0 * 1024.0 * 1024.0);
            snap.disk_valid = 1;
        }
        g_luna_monitor.last_disk = now_mono;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info)) {
        (void)strftime(snap.mb_clock, sizeof(snap.mb_clock),
                       "%a %b %e  %H:%M", &tm_info);
        (void)strftime(snap.widget_time, sizeof(snap.widget_time),
                       "%H:%M", &tm_info);
        (void)strftime(snap.widget_date, sizeof(snap.widget_date),
                       "%A, %B %e", &tm_info);
    }

    pthread_mutex_lock(&g_luna_monitor.mutex);
    snap.generation = g_luna_monitor.status.generation + 1;
    g_luna_monitor.status = snap;
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    luna_monitor_notify();
}

static void luna_monitor_read_state_file(void) {
    char path[PATH_MAX];
    pthread_mutex_lock(&g_luna_monitor.mutex);
    snprintf(path, sizeof(path), "%s", g_luna_monitor.state_path);
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    if (!path[0]) return;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return;

    pthread_mutex_lock(&g_luna_monitor.mutex);
    int same = st.st_size == g_luna_monitor.state_size &&
               st.st_mtim.tv_sec == g_luna_monitor.state_mtime.tv_sec &&
               st.st_mtim.tv_nsec == g_luna_monitor.state_mtime.tv_nsec;
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    if (same) return;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char* local = (char*)malloc(LUNA_MONITOR_STATE_MAX);
    if (!local) {
        close(fd);
        return;
    }
    size_t used = 0;
    while (used + 1 < LUNA_MONITOR_STATE_MAX) {
        ssize_t n = read(fd, local + used, LUNA_MONITOR_STATE_MAX - used - 1);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    if (used == 0) {
        free(local);
        return;
    }
    local[used] = 0;

    pthread_mutex_lock(&g_luna_monitor.mutex);
    memcpy(g_luna_monitor.state_text, local, used + 1);
    g_luna_monitor.state_len = used;
    g_luna_monitor.state_size = st.st_size;
    g_luna_monitor.state_mtime = st.st_mtim;
    g_luna_monitor.state_generation++;
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    free(local);
    luna_monitor_notify();
}

static void* luna_monitor_thread_main(void* unused) {
    (void)unused;
    double next_status = 0.0;

    for (;;) {
        int state_requested = 0;
        int publish_status = 0;

        pthread_mutex_lock(&g_luna_monitor.mutex);
        while (g_luna_monitor.running) {
            double now = luna_monitor_now();
            if (g_luna_monitor.state_requested) {
                state_requested = 1;
                g_luna_monitor.state_requested = 0;
                break;
            }
            if (now >= next_status) {
                publish_status = 1;
                next_status = now + g_luna_monitor.status_interval_sec;
                break;
            }
            struct timespec until = luna_monitor_deadline(next_status);
            int rc = pthread_cond_timedwait(&g_luna_monitor.cond,
                                            &g_luna_monitor.mutex, &until);
            if (rc != 0 && rc != ETIMEDOUT) break;
        }
        int running = g_luna_monitor.running;
        pthread_mutex_unlock(&g_luna_monitor.mutex);
        if (!running) break;

        if (state_requested) luna_monitor_read_state_file();
        if (publish_status) luna_monitor_publish_status();
    }
    return NULL;
}

int luna_monitor_init(const LunaMonitorConfig* config) {
    if (g_luna_monitor.initialized) return 1;
    memset(&g_luna_monitor, 0, sizeof(g_luna_monitor));
    g_luna_monitor.state_size = -1;
    g_luna_monitor.state_mtime.tv_sec = -1;
    g_luna_monitor.status.battery = -1;
    snprintf(g_luna_monitor.status.network,
             sizeof(g_luna_monitor.status.network), "Offline");
    g_luna_monitor.status_interval_sec =
        config && config->status_interval_sec > 0.0
            ? config->status_interval_sec : 1.0;
    if (config) {
        if (config->state_path)
            snprintf(g_luna_monitor.state_path,
                     sizeof(g_luna_monitor.state_path), "%s", config->state_path);
        g_luna_monitor.notify = config->notify;
        g_luna_monitor.notify_user = config->notify_user;
    }

    if (pthread_mutex_init(&g_luna_monitor.mutex, NULL) != 0) return 0;

    pthread_condattr_t attr;
    int attr_initialized = pthread_condattr_init(&attr) == 0;
    int use_attr = 0;
#if defined(CLOCK_MONOTONIC)
    if (attr_initialized &&
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
        use_attr = 1;
        g_luna_monitor.cond_uses_monotonic = 1;
    }
#endif
    if (pthread_cond_init(&g_luna_monitor.cond, use_attr ? &attr : NULL) != 0) {
        if (attr_initialized) pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&g_luna_monitor.mutex);
        return 0;
    }
    if (attr_initialized) pthread_condattr_destroy(&attr);

    g_luna_monitor.last_power_net = -1e9;
    g_luna_monitor.last_disk = -1e9;
    g_luna_monitor.running = 1;
    g_luna_monitor.state_requested = 1;
    g_luna_monitor.initialized = 1;
    if (pthread_create(&g_luna_monitor.thread, NULL,
                       luna_monitor_thread_main, NULL) != 0) {
        g_luna_monitor.initialized = 0;
        g_luna_monitor.running = 0;
        pthread_cond_destroy(&g_luna_monitor.cond);
        pthread_mutex_destroy(&g_luna_monitor.mutex);
        return 0;
    }
    return 1;
}

void luna_monitor_shutdown(void) {
    if (!g_luna_monitor.initialized) return;
    pthread_mutex_lock(&g_luna_monitor.mutex);
    g_luna_monitor.running = 0;
    pthread_cond_broadcast(&g_luna_monitor.cond);
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    pthread_join(g_luna_monitor.thread, NULL);
    pthread_cond_destroy(&g_luna_monitor.cond);
    pthread_mutex_destroy(&g_luna_monitor.mutex);
    memset(&g_luna_monitor, 0, sizeof(g_luna_monitor));
}

void luna_monitor_request_state(void) {
    if (!g_luna_monitor.initialized) return;
    pthread_mutex_lock(&g_luna_monitor.mutex);
    g_luna_monitor.state_requested = 1;
    pthread_cond_signal(&g_luna_monitor.cond);
    pthread_mutex_unlock(&g_luna_monitor.mutex);
}

int luna_monitor_consume_status(LunaMonitorSnapshot* out) {
    if (!out || !g_luna_monitor.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_monitor.mutex) != 0) return 0;
    if (g_luna_monitor.status.generation ==
        g_luna_monitor.consumed_status_generation) {
        pthread_mutex_unlock(&g_luna_monitor.mutex);
        return 0;
    }
    *out = g_luna_monitor.status;
    g_luna_monitor.consumed_status_generation = out->generation;
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    return 1;
}

int luna_monitor_consume_state(char* out, size_t out_n, size_t* len_out) {
    if (!out || out_n == 0 || !g_luna_monitor.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_monitor.mutex) != 0) return 0;
    if (g_luna_monitor.state_generation ==
            g_luna_monitor.consumed_state_generation ||
        g_luna_monitor.state_len == 0) {
        pthread_mutex_unlock(&g_luna_monitor.mutex);
        return 0;
    }
    size_t len = g_luna_monitor.state_len;
    if (len >= out_n) len = out_n - 1;
    memcpy(out, g_luna_monitor.state_text, len);
    out[len] = 0;
    g_luna_monitor.consumed_state_generation =
        g_luna_monitor.state_generation;
    pthread_mutex_unlock(&g_luna_monitor.mutex);
    if (len_out) *len_out = len;
    return 1;
}

#endif /* LUNA_MONITOR_IMPLEMENTATION_ONCE */
#endif /* LUNA_MONITOR_IMPLEMENTATION */
