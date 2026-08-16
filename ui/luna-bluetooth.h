/*
 * luna-bluetooth.h — asynchronous Bluetooth backend for luna-shell
 *
 * Define LUNA_BLUETOOTH_IMPLEMENTATION in exactly one translation unit before
 * including this file.  BlueZ (bluetoothctl) / ConnMan I/O runs on an internal
 * worker thread.  The caller only queues commands and consumes snapshots.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef LUNA_BLUETOOTH_H
#define LUNA_BLUETOOTH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_BLUETOOTH_MAX_DEVICES
#define LUNA_BLUETOOTH_MAX_DEVICES 8
#endif

typedef enum LunaBluetoothBackend {
    LUNA_BLUETOOTH_NONE = 0,
    LUNA_BLUETOOTH_BLUEZ = 1,
    LUNA_BLUETOOTH_CONNMAN = 2
} LunaBluetoothBackend;

typedef struct LunaBluetoothDevice {
    char name[96];
    char id[64];      /* BD_ADDR or ConnMan service id */
    int connected;
    int paired;
} LunaBluetoothDevice;

typedef struct LunaBluetoothSnapshot {
    LunaBluetoothBackend backend;
    LunaBluetoothDevice devices[LUNA_BLUETOOTH_MAX_DEVICES];
    int count;
    int powered;
    int available;
    int busy;
    char error[96];
    unsigned long long generation;
} LunaBluetoothSnapshot;

typedef void (*LunaBluetoothNotifyFn)(void* user);

typedef struct LunaBluetoothConfig {
    LunaBluetoothNotifyFn notify;
    void* notify_user;
} LunaBluetoothConfig;

int  luna_bluetooth_init(const LunaBluetoothConfig* config);
void luna_bluetooth_shutdown(void);
int  luna_bluetooth_request_refresh(void);
int  luna_bluetooth_request_set_powered(int powered);
int  luna_bluetooth_request_scan(void);
int  luna_bluetooth_request_connect(const char* id);
int  luna_bluetooth_request_disconnect(const char* id);
int  luna_bluetooth_consume(LunaBluetoothSnapshot* out, unsigned long long* last_generation);
int  luna_bluetooth_running(void);
int  luna_bluetooth_reaper_try_lock(void);
void luna_bluetooth_reaper_unlock(void);

#ifdef __cplusplus
}
#endif

#ifdef LUNA_BLUETOOTH_IMPLEMENTATION

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#define LUNA_BLUETOOTH_QUEUE_CAP 16

typedef enum LunaBluetoothCommandType {
    LUNA_BLUETOOTH_CMD_REFRESH = 1,
    LUNA_BLUETOOTH_CMD_SET_POWERED,
    LUNA_BLUETOOTH_CMD_SCAN,
    LUNA_BLUETOOTH_CMD_CONNECT,
    LUNA_BLUETOOTH_CMD_DISCONNECT
} LunaBluetoothCommandType;

typedef struct LunaBluetoothCommand {
    LunaBluetoothCommandType type;
    int powered;
    char id[64];
} LunaBluetoothCommand;

typedef struct LunaBluetoothWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_mutex_t child_mutex;
    pthread_cond_t cond;
    int initialized;
    int running;
    LunaBluetoothCommand queue[LUNA_BLUETOOTH_QUEUE_CAP];
    unsigned head;
    unsigned tail;
    unsigned count;
    LunaBluetoothSnapshot snapshot;
    LunaBluetoothNotifyFn notify;
    void* notify_user;
} LunaBluetoothWorker;

static LunaBluetoothWorker g_luna_bluetooth;

static void luna_bluetooth_notify(void) {
    LunaBluetoothNotifyFn fn;
    void* user;
    pthread_mutex_lock(&g_luna_bluetooth.mutex);
    fn = g_luna_bluetooth.notify;
    user = g_luna_bluetooth.notify_user;
    pthread_mutex_unlock(&g_luna_bluetooth.mutex);
    if (fn) fn(user);
}

static int luna_bluetooth_command_path(const char* name, char* out, size_t out_n) {
    const char* path = getenv("PATH");
    if (!path || !name || !*name || !out || out_n == 0) return 0;
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", path);
    char* save = NULL;
    for (char* d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        int n = snprintf(out, out_n, "%s/%s", d[0] ? d : ".", name);
        if (n > 0 && (size_t)n < out_n && access(out, X_OK) == 0) return 1;
    }
    out[0] = 0;
    return 0;
}

static int luna_bluetooth_command_available(const char* name) {
    char path[512];
    return luna_bluetooth_command_path(name, path, sizeof(path));
}

static void luna_bluetooth_trim_line(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int luna_bluetooth_run_wait(const char* const argv[]) {
    if (!argv || !argv[0]) return 0;
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, (char* const*)argv, environ);
    if (rc != 0) return 0;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int luna_bluetooth_valid_addr(const char* id) {
    if (!id || strlen(id) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) {
            if (id[i] != ':') return 0;
        } else if (!isxdigit((unsigned char)id[i])) {
            return 0;
        }
    }
    return 1;
}

static int luna_bluetooth_valid_connman_service(const char* id) {
    if (!id || strncmp(id, "bluetooth_", 10) != 0) return 0;
    for (const char* p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int luna_bluetooth_powered_bluez(void) {
    FILE* f = popen("bluetoothctl show 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int powered = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Powered:") && strstr(line, "yes")) {
            powered = 1;
            break;
        }
    }
    (void)pclose(f);
    return powered;
}

static int luna_bluetooth_powered_connman(void) {
    int enabled = 0;
    FILE* f = popen("connmanctl technologies 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int in_bt = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '/') {
            in_bt = strstr(line, "technology/bluetooth") != NULL;
            continue;
        }
        if (in_bt && strstr(line, "Powered") && strstr(line, "True")) {
            enabled = 1;
            break;
        }
    }
    (void)pclose(f);
    return enabled;
}

static int luna_bluetooth_device_flag(const char* addr, const char* key) {
    if (!luna_bluetooth_valid_addr(addr)) return 0;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", addr);
    FILE* f = popen(cmd, "r");
    if (!f) return 0;
    char line[256];
    int hit = 0;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, key, key_len) == 0 && strstr(p, "yes")) {
            hit = 1;
            break;
        }
    }
    (void)pclose(f);
    return hit;
}

static void luna_bluetooth_add_device(LunaBluetoothSnapshot* state,
                                      const char* addr, const char* name,
                                      int paired_hint) {
    if (!state || state->count >= LUNA_BLUETOOTH_MAX_DEVICES) return;
    if (!luna_bluetooth_valid_addr(addr)) return;
    for (int i = 0; i < state->count; i++) {
        if (!strcmp(state->devices[i].id, addr)) {
            if (name && *name)
                snprintf(state->devices[i].name, sizeof(state->devices[i].name), "%s", name);
            if (paired_hint) state->devices[i].paired = 1;
            return;
        }
    }
    LunaBluetoothDevice* d = &state->devices[state->count++];
    snprintf(d->id, sizeof(d->id), "%s", addr);
    snprintf(d->name, sizeof(d->name), "%s", (name && *name) ? name : addr);
    d->paired = paired_hint || luna_bluetooth_device_flag(addr, "Paired:");
    d->connected = luna_bluetooth_device_flag(addr, "Connected:");
}

static void luna_bluetooth_parse_devices_output(LunaBluetoothSnapshot* state,
                                               const char* cmd, int paired_hint) {
    FILE* f = popen(cmd, "r");
    if (!f) return;
    char line[512];
    while (state->count < LUNA_BLUETOOTH_MAX_DEVICES && fgets(line, sizeof(line), f)) {
        luna_bluetooth_trim_line(line);
        /* "Device AA:BB:CC:DD:EE:FF Name here" */
        if (strncmp(line, "Device ", 7) != 0) continue;
        char* addr = line + 7;
        char* sp = strchr(addr, ' ');
        char name_buf[96] = {0};
        if (sp) {
            *sp = 0;
            snprintf(name_buf, sizeof(name_buf), "%s", sp + 1);
            luna_bluetooth_trim_line(name_buf);
        }
        luna_bluetooth_add_device(state, addr, name_buf, paired_hint);
    }
    (void)pclose(f);
}

static void luna_bluetooth_refresh_bluez(LunaBluetoothSnapshot* state) {
    state->backend = LUNA_BLUETOOTH_BLUEZ;
    state->powered = luna_bluetooth_powered_bluez();
    if (!state->powered) return;
    luna_bluetooth_parse_devices_output(state, "bluetoothctl devices Paired 2>/dev/null", 1);
    luna_bluetooth_parse_devices_output(state, "bluetoothctl devices Connected 2>/dev/null", 1);
    luna_bluetooth_parse_devices_output(state, "bluetoothctl devices 2>/dev/null", 0);
}

static char* luna_bluetooth_find_last_token(char* text, const char* needle) {
    char* last = NULL;
    char* p = text;
    while ((p = strstr(p, needle)) != NULL) {
        last = p;
        p++;
    }
    return last;
}

static void luna_bluetooth_refresh_connman(LunaBluetoothSnapshot* state) {
    state->backend = LUNA_BLUETOOTH_CONNMAN;
    state->powered = luna_bluetooth_powered_connman();
    if (!state->powered) return;

    FILE* f = popen("connmanctl services 2>/dev/null", "r");
    if (!f) {
        snprintf(state->error, sizeof(state->error), "Cannot query ConnMan");
        return;
    }
    char line[512];
    while (state->count < LUNA_BLUETOOTH_MAX_DEVICES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char flags[5] = {0};
        size_t prefix = strlen(line) < 4 ? strlen(line) : 4;
        memcpy(flags, line, prefix);
        char* label = line + prefix;
        char* service = luna_bluetooth_find_last_token(label, "bluetooth_");
        if (!service) continue;
        char id[64];
        size_t id_len = 0;
        while (service[id_len] && !isspace((unsigned char)service[id_len])) id_len++;
        if (id_len == 0 || id_len >= sizeof(id)) continue;
        memcpy(id, service, id_len);
        id[id_len] = 0;
        *service = 0;
        luna_bluetooth_trim_line(label);
        LunaBluetoothDevice* d = &state->devices[state->count++];
        snprintf(d->name, sizeof(d->name), "%.95s", *label ? label : "Bluetooth device");
        snprintf(d->id, sizeof(d->id), "%s", id);
        d->connected = strchr(flags, 'R') != NULL || strchr(flags, 'O') != NULL;
        d->paired = strchr(flags, '*') != NULL || d->connected;
    }
    (void)pclose(f);
}

static void luna_bluetooth_backend_refresh(LunaBluetoothSnapshot* state) {
    memset(state, 0, sizeof(*state));
    state->available = 1;
    if (luna_bluetooth_command_available("bluetoothctl"))
        luna_bluetooth_refresh_bluez(state);
    else if (luna_bluetooth_command_available("connmanctl"))
        luna_bluetooth_refresh_connman(state);
    else {
        state->backend = LUNA_BLUETOOTH_NONE;
        snprintf(state->error, sizeof(state->error), "bluetoothctl / ConnMan not found");
    }
}

static int luna_bluetooth_set_powered(int target, LunaBluetoothBackend backend) {
    if (backend == LUNA_BLUETOOTH_BLUEZ || luna_bluetooth_command_available("bluetoothctl")) {
        if (target) {
            /* Soft-blocked adapters need rfkill before power on succeeds. */
            if (luna_bluetooth_command_available("rfkill")) {
                const char* const unblock[] = { "rfkill", "unblock", "bluetooth", NULL };
                (void)luna_bluetooth_run_wait(unblock);
            }
            const char* const argv[] = { "bluetoothctl", "power", "on", NULL };
            int ok = luna_bluetooth_run_wait(argv);
            if (luna_bluetooth_command_available("connmanctl")) {
                const char* const cargv[] = { "connmanctl", "enable", "bluetooth", NULL };
                (void)luna_bluetooth_run_wait(cargv);
            }
            return ok || luna_bluetooth_powered_bluez();
        }
        const char* const argv[] = { "bluetoothctl", "power", "off", NULL };
        int ok = luna_bluetooth_run_wait(argv);
        if (luna_bluetooth_command_available("connmanctl")) {
            const char* const cargv[] = { "connmanctl", "disable", "bluetooth", NULL };
            (void)luna_bluetooth_run_wait(cargv);
        }
        return ok || !luna_bluetooth_powered_bluez();
    }
    if (backend == LUNA_BLUETOOTH_CONNMAN) {
        const char* const argv[] = {
            "connmanctl", target ? "enable" : "disable", "bluetooth", NULL
        };
        return luna_bluetooth_run_wait(argv);
    }
    return 0;
}

static int luna_bluetooth_do_command(const LunaBluetoothCommand* command,
                                    const LunaBluetoothSnapshot* before) {
    if (!command) return 0;
    LunaBluetoothBackend backend = before ? before->backend : LUNA_BLUETOOTH_NONE;
    int powered = before ? before->powered : 0;
    if (command->type == LUNA_BLUETOOTH_CMD_REFRESH) return 1;
    if (command->type == LUNA_BLUETOOTH_CMD_SET_POWERED) {
        int target = command->powered != 0;
        if (target == powered) return 1;
        return luna_bluetooth_set_powered(target, backend);
    }
    if (command->type == LUNA_BLUETOOTH_CMD_SCAN) {
        if (backend == LUNA_BLUETOOTH_BLUEZ || luna_bluetooth_command_available("bluetoothctl")) {
            /* Timed scan so the worker does not block indefinitely. */
            const char* const argv[] = { "bluetoothctl", "--timeout", "8", "scan", "on", NULL };
            (void)luna_bluetooth_run_wait(argv);
            return 1;
        }
        if (backend == LUNA_BLUETOOTH_CONNMAN) {
            const char* const argv[] = { "connmanctl", "scan", "bluetooth", NULL };
            return luna_bluetooth_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_BLUETOOTH_CMD_CONNECT) {
        if (luna_bluetooth_valid_addr(command->id) &&
            luna_bluetooth_command_available("bluetoothctl")) {
            const char* const argv[] = { "bluetoothctl", "connect", command->id, NULL };
            return luna_bluetooth_run_wait(argv);
        }
        if (luna_bluetooth_valid_connman_service(command->id)) {
            const char* const argv[] = { "connmanctl", "connect", command->id, NULL };
            return luna_bluetooth_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_BLUETOOTH_CMD_DISCONNECT) {
        if (luna_bluetooth_valid_addr(command->id) &&
            luna_bluetooth_command_available("bluetoothctl")) {
            const char* const argv[] = { "bluetoothctl", "disconnect", command->id, NULL };
            return luna_bluetooth_run_wait(argv);
        }
        if (luna_bluetooth_valid_connman_service(command->id)) {
            const char* const argv[] = { "connmanctl", "disconnect", command->id, NULL };
            return luna_bluetooth_run_wait(argv);
        }
        return 0;
    }
    return 0;
}

static void luna_bluetooth_publish(const LunaBluetoothSnapshot* snapshot) {
    pthread_mutex_lock(&g_luna_bluetooth.mutex);
    unsigned long long next = g_luna_bluetooth.snapshot.generation + 1;
    g_luna_bluetooth.snapshot = *snapshot;
    g_luna_bluetooth.snapshot.generation = next;
    pthread_mutex_unlock(&g_luna_bluetooth.mutex);
    luna_bluetooth_notify();
}

static void* luna_bluetooth_thread_main(void* unused) {
    (void)unused;
    for (;;) {
        LunaBluetoothCommand command;
        memset(&command, 0, sizeof(command));
        pthread_mutex_lock(&g_luna_bluetooth.mutex);
        while (g_luna_bluetooth.running && g_luna_bluetooth.count == 0)
            pthread_cond_wait(&g_luna_bluetooth.cond, &g_luna_bluetooth.mutex);
        if (!g_luna_bluetooth.running) {
            pthread_mutex_unlock(&g_luna_bluetooth.mutex);
            break;
        }
        command = g_luna_bluetooth.queue[g_luna_bluetooth.head];
        g_luna_bluetooth.head = (g_luna_bluetooth.head + 1) % LUNA_BLUETOOTH_QUEUE_CAP;
        g_luna_bluetooth.count--;
        LunaBluetoothSnapshot before = g_luna_bluetooth.snapshot;
        before.busy = 1;
        before.generation++;
        g_luna_bluetooth.snapshot = before;
        pthread_mutex_unlock(&g_luna_bluetooth.mutex);
        luna_bluetooth_notify();

        int ok = 1;
        pthread_mutex_lock(&g_luna_bluetooth.child_mutex);
        if (command.type != LUNA_BLUETOOTH_CMD_REFRESH)
            ok = luna_bluetooth_do_command(&command, &before);
        LunaBluetoothSnapshot after;
        luna_bluetooth_backend_refresh(&after);
        pthread_mutex_unlock(&g_luna_bluetooth.child_mutex);
        after.busy = 0;
        if (!ok && !after.error[0])
            snprintf(after.error, sizeof(after.error), "Bluetooth operation failed");
        luna_bluetooth_publish(&after);
    }
    return NULL;
}

static int luna_bluetooth_enqueue(LunaBluetoothCommandType type, int powered, const char* id) {
    if (!g_luna_bluetooth.initialized) return 0;
    LunaBluetoothCommand command;
    memset(&command, 0, sizeof(command));
    command.type = type;
    command.powered = powered != 0;
    if (id) snprintf(command.id, sizeof(command.id), "%s", id);

    pthread_mutex_lock(&g_luna_bluetooth.mutex);
    if (!g_luna_bluetooth.running || g_luna_bluetooth.count >= LUNA_BLUETOOTH_QUEUE_CAP) {
        pthread_mutex_unlock(&g_luna_bluetooth.mutex);
        return 0;
    }
    if (type == LUNA_BLUETOOTH_CMD_REFRESH && g_luna_bluetooth.count > 0) {
        unsigned last = (g_luna_bluetooth.tail + LUNA_BLUETOOTH_QUEUE_CAP - 1) %
                        LUNA_BLUETOOTH_QUEUE_CAP;
        if (g_luna_bluetooth.queue[last].type == LUNA_BLUETOOTH_CMD_REFRESH) {
            pthread_mutex_unlock(&g_luna_bluetooth.mutex);
            return 1;
        }
    }
    g_luna_bluetooth.queue[g_luna_bluetooth.tail] = command;
    g_luna_bluetooth.tail = (g_luna_bluetooth.tail + 1) % LUNA_BLUETOOTH_QUEUE_CAP;
    g_luna_bluetooth.count++;
    pthread_cond_signal(&g_luna_bluetooth.cond);
    pthread_mutex_unlock(&g_luna_bluetooth.mutex);
    return 1;
}

int luna_bluetooth_init(const LunaBluetoothConfig* config) {
    if (g_luna_bluetooth.initialized) return 1;
    memset(&g_luna_bluetooth, 0, sizeof(g_luna_bluetooth));
    if (pthread_mutex_init(&g_luna_bluetooth.mutex, NULL) != 0) return 0;
    if (pthread_mutex_init(&g_luna_bluetooth.child_mutex, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_bluetooth.mutex);
        return 0;
    }
    if (pthread_cond_init(&g_luna_bluetooth.cond, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_bluetooth.child_mutex);
        pthread_mutex_destroy(&g_luna_bluetooth.mutex);
        return 0;
    }
    g_luna_bluetooth.notify = config ? config->notify : NULL;
    g_luna_bluetooth.notify_user = config ? config->notify_user : NULL;
    g_luna_bluetooth.running = 1;
    g_luna_bluetooth.initialized = 1;
    g_luna_bluetooth.snapshot.available = 1;
    g_luna_bluetooth.snapshot.busy = 1;
    if (pthread_create(&g_luna_bluetooth.thread, NULL, luna_bluetooth_thread_main, NULL) != 0) {
        g_luna_bluetooth.initialized = 0;
        g_luna_bluetooth.running = 0;
        pthread_cond_destroy(&g_luna_bluetooth.cond);
        pthread_mutex_destroy(&g_luna_bluetooth.child_mutex);
        pthread_mutex_destroy(&g_luna_bluetooth.mutex);
        return 0;
    }
    (void)luna_bluetooth_request_refresh();
    return 1;
}

void luna_bluetooth_shutdown(void) {
    if (!g_luna_bluetooth.initialized) return;
    pthread_mutex_lock(&g_luna_bluetooth.mutex);
    g_luna_bluetooth.running = 0;
    pthread_cond_broadcast(&g_luna_bluetooth.cond);
    pthread_mutex_unlock(&g_luna_bluetooth.mutex);
    pthread_join(g_luna_bluetooth.thread, NULL);
    pthread_cond_destroy(&g_luna_bluetooth.cond);
    pthread_mutex_destroy(&g_luna_bluetooth.child_mutex);
    pthread_mutex_destroy(&g_luna_bluetooth.mutex);
    memset(&g_luna_bluetooth, 0, sizeof(g_luna_bluetooth));
}

int luna_bluetooth_request_refresh(void) {
    return luna_bluetooth_enqueue(LUNA_BLUETOOTH_CMD_REFRESH, 0, NULL);
}
int luna_bluetooth_request_set_powered(int powered) {
    return luna_bluetooth_enqueue(LUNA_BLUETOOTH_CMD_SET_POWERED, powered != 0, NULL);
}
int luna_bluetooth_request_scan(void) {
    return luna_bluetooth_enqueue(LUNA_BLUETOOTH_CMD_SCAN, 0, NULL);
}
int luna_bluetooth_request_connect(const char* id) {
    if (!id || !*id) return 0;
    return luna_bluetooth_enqueue(LUNA_BLUETOOTH_CMD_CONNECT, 0, id);
}
int luna_bluetooth_request_disconnect(const char* id) {
    if (!id || !*id) return 0;
    return luna_bluetooth_enqueue(LUNA_BLUETOOTH_CMD_DISCONNECT, 0, id);
}

int luna_bluetooth_consume(LunaBluetoothSnapshot* out, unsigned long long* last_generation) {
    if (!out || !g_luna_bluetooth.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_bluetooth.mutex) != 0) return 0;
    unsigned long long seen = last_generation ? *last_generation : 0;
    if (g_luna_bluetooth.snapshot.generation == seen) {
        pthread_mutex_unlock(&g_luna_bluetooth.mutex);
        return 0;
    }
    *out = g_luna_bluetooth.snapshot;
    if (last_generation) *last_generation = out->generation;
    pthread_mutex_unlock(&g_luna_bluetooth.mutex);
    return 1;
}

int luna_bluetooth_running(void) {
    return g_luna_bluetooth.initialized && g_luna_bluetooth.running;
}

int luna_bluetooth_reaper_try_lock(void) {
    if (!g_luna_bluetooth.initialized) return 1;
    return pthread_mutex_trylock(&g_luna_bluetooth.child_mutex) == 0;
}

void luna_bluetooth_reaper_unlock(void) {
    if (g_luna_bluetooth.initialized) pthread_mutex_unlock(&g_luna_bluetooth.child_mutex);
}

#endif /* LUNA_BLUETOOTH_IMPLEMENTATION */
#endif /* LUNA_BLUETOOTH_H */
