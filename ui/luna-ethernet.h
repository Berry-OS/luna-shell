/*
 * luna-ethernet.h — asynchronous wired-Ethernet backend for luna-shell
 *
 * Define LUNA_ETHERNET_IMPLEMENTATION in exactly one translation unit before
 * including this file.  ConnMan/NetworkManager I/O runs on an internal worker
 * thread.  The caller only queues commands and consumes immutable snapshots.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef LUNA_ETHERNET_H
#define LUNA_ETHERNET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_ETHERNET_MAX_LINKS
#define LUNA_ETHERNET_MAX_LINKS 4
#endif

typedef enum LunaEthernetBackend {
    LUNA_ETHERNET_NONE = 0,
    LUNA_ETHERNET_CONNMAN = 1,
    LUNA_ETHERNET_NMCLI = 2
} LunaEthernetBackend;

typedef struct LunaEthernetLink {
    char name[96];
    char id[192];
    int connected;
    int available; /* carrier / cable present when known */
} LunaEthernetLink;

typedef struct LunaEthernetSnapshot {
    LunaEthernetBackend backend;
    LunaEthernetLink links[LUNA_ETHERNET_MAX_LINKS];
    int count;
    int powered;
    int available;
    int busy;
    char error[96];
    unsigned long long generation;
} LunaEthernetSnapshot;

typedef void (*LunaEthernetNotifyFn)(void* user);

typedef struct LunaEthernetConfig {
    LunaEthernetNotifyFn notify;
    void* notify_user;
} LunaEthernetConfig;

int  luna_ethernet_init(const LunaEthernetConfig* config);
void luna_ethernet_shutdown(void);
int  luna_ethernet_request_refresh(void);
int  luna_ethernet_request_set_powered(int powered);
int  luna_ethernet_request_connect(const char* id);
int  luna_ethernet_request_disconnect(const char* id);
int  luna_ethernet_consume(LunaEthernetSnapshot* out, unsigned long long* last_generation);
int  luna_ethernet_running(void);
int  luna_ethernet_reaper_try_lock(void);
void luna_ethernet_reaper_unlock(void);

#ifdef __cplusplus
}
#endif

#ifdef LUNA_ETHERNET_IMPLEMENTATION

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#define LUNA_ETHERNET_QUEUE_CAP 16

typedef enum LunaEthernetCommandType {
    LUNA_ETHERNET_CMD_REFRESH = 1,
    LUNA_ETHERNET_CMD_SET_POWERED,
    LUNA_ETHERNET_CMD_CONNECT,
    LUNA_ETHERNET_CMD_DISCONNECT
} LunaEthernetCommandType;

typedef struct LunaEthernetCommand {
    LunaEthernetCommandType type;
    int powered;
    char id[192];
} LunaEthernetCommand;

typedef struct LunaEthernetWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_mutex_t child_mutex;
    pthread_cond_t cond;
    int initialized;
    int running;
    LunaEthernetCommand queue[LUNA_ETHERNET_QUEUE_CAP];
    unsigned head;
    unsigned tail;
    unsigned count;
    LunaEthernetSnapshot snapshot;
    LunaEthernetNotifyFn notify;
    void* notify_user;
} LunaEthernetWorker;

static LunaEthernetWorker g_luna_ethernet;

static void luna_ethernet_notify(void) {
    LunaEthernetNotifyFn fn;
    void* user;
    pthread_mutex_lock(&g_luna_ethernet.mutex);
    fn = g_luna_ethernet.notify;
    user = g_luna_ethernet.notify_user;
    pthread_mutex_unlock(&g_luna_ethernet.mutex);
    if (fn) fn(user);
}

static int luna_ethernet_command_path(const char* name, char* out, size_t out_n) {
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

static int luna_ethernet_command_available(const char* name) {
    char path[512];
    return luna_ethernet_command_path(name, path, sizeof(path));
}

static void luna_ethernet_trim_line(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int luna_ethernet_run_wait(const char* const argv[]) {
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

static int luna_ethernet_powered_connman(void) {
    int enabled = 0;
    FILE* f = popen("connmanctl technologies 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int in_eth = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '/') {
            in_eth = strstr(line, "technology/ethernet") != NULL;
            continue;
        }
        if (in_eth && strstr(line, "Powered") && strstr(line, "True")) {
            enabled = 1;
            break;
        }
    }
    (void)pclose(f);
    return enabled;
}

static char* luna_ethernet_find_last_token(char* text, const char* needle) {
    char* last = NULL;
    char* p = text;
    while ((p = strstr(p, needle)) != NULL) {
        last = p;
        p++;
    }
    return last;
}

static int luna_ethernet_sysfs_carrier(const char* iface) {
    char path[256];
    if (snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface) >= (int)sizeof(path))
        return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int carrier = 0;
    if (fscanf(f, "%d", &carrier) != 1) carrier = -1;
    fclose(f);
    return carrier;
}

static void luna_ethernet_refresh_connman(LunaEthernetSnapshot* state) {
    state->backend = LUNA_ETHERNET_CONNMAN;
    state->powered = luna_ethernet_powered_connman();

    FILE* f = popen("connmanctl services 2>/dev/null", "r");
    if (!f) {
        snprintf(state->error, sizeof(state->error), "Cannot query ConnMan");
        return;
    }
    char line[512];
    while (state->count < LUNA_ETHERNET_MAX_LINKS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char flags[5] = {0};
        size_t prefix = strlen(line) < 4 ? strlen(line) : 4;
        memcpy(flags, line, prefix);
        char* label = line + prefix;
        char* service = luna_ethernet_find_last_token(label, "ethernet_");
        if (!service) continue;
        char id[192];
        size_t id_len = 0;
        while (service[id_len] && !isspace((unsigned char)service[id_len])) id_len++;
        if (id_len == 0 || id_len >= sizeof(id)) continue;
        memcpy(id, service, id_len);
        id[id_len] = 0;
        *service = 0;
        luna_ethernet_trim_line(label);

        LunaEthernetLink* n = &state->links[state->count++];
        snprintf(n->name, sizeof(n->name), "%.95s", *label ? label : "Wired");
        snprintf(n->id, sizeof(n->id), "%s", id);
        n->connected = strchr(flags, 'R') != NULL || strchr(flags, 'O') != NULL;
        n->available = 1;
    }
    (void)pclose(f);

    /* When ConnMan has no ethernet_* service yet (cable unplugged), still
     * surface physical interfaces so the menu is not empty. */
    if (state->count == 0) {
        FILE* net = popen("ls -1 /sys/class/net 2>/dev/null", "r");
        if (net) {
            char iface[64];
            while (state->count < LUNA_ETHERNET_MAX_LINKS && fgets(iface, sizeof(iface), net)) {
                luna_ethernet_trim_line(iface);
                if (!iface[0] || !strcmp(iface, "lo")) continue;
                char wireless[256];
                snprintf(wireless, sizeof(wireless), "/sys/class/net/%s/wireless", iface);
                struct stat st;
                if (stat(wireless, &st) == 0) continue;
                /* Skip bridges/tunnels/virtuals with a common prefix. */
                if (!strncmp(iface, "docker", 6) || !strncmp(iface, "veth", 4) ||
                    !strncmp(iface, "br", 2) || !strncmp(iface, "virbr", 5) ||
                    !strncmp(iface, "tun", 3) || !strncmp(iface, "tap", 3) ||
                    !strncmp(iface, "wg", 2) || !strncmp(iface, "wlan", 4))
                    continue;
                LunaEthernetLink* n = &state->links[state->count++];
                snprintf(n->name, sizeof(n->name), "%s", iface);
                snprintf(n->id, sizeof(n->id), "iface:%s", iface);
                int carrier = luna_ethernet_sysfs_carrier(iface);
                n->available = carrier > 0;
                char oper[256];
                snprintf(oper, sizeof(oper), "/sys/class/net/%s/operstate", iface);
                FILE* of = fopen(oper, "r");
                char state_buf[24] = {0};
                if (of) {
                    (void)fgets(state_buf, sizeof(state_buf), of);
                    fclose(of);
                }
                n->connected = strncmp(state_buf, "up", 2) == 0;
            }
            (void)pclose(net);
        }
    }
}

static int luna_ethernet_nmcli_split(char* line, char* fields[], int wanted) {
    int field = 0;
    char* src = line;
    char* dst = line;
    fields[field++] = dst;
    while (*src) {
        if (*src == '\\' && src[1]) {
            src++;
            *dst++ = *src++;
            continue;
        }
        if (*src == ':' && field < wanted) {
            *dst++ = 0;
            src++;
            fields[field++] = dst;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = 0;
    return field;
}

static void luna_ethernet_refresh_nmcli(LunaEthernetSnapshot* state) {
    state->backend = LUNA_ETHERNET_NMCLI;
    state->powered = 1; /* Ethernet has no global radio in NetworkManager. */

    FILE* f = popen("nmcli -t --escape yes -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null", "r");
    if (!f) {
        snprintf(state->error, sizeof(state->error), "Cannot query NetworkManager");
        return;
    }
    char line[512];
    while (state->count < LUNA_ETHERNET_MAX_LINKS && fgets(line, sizeof(line), f)) {
        luna_ethernet_trim_line(line);
        char* fields[4] = {0};
        if (luna_ethernet_nmcli_split(line, fields, 4) != 4) continue;
        if (strcmp(fields[1], "ethernet") != 0) continue;
        LunaEthernetLink* n = &state->links[state->count++];
        if (fields[3][0] && strcmp(fields[3], "--") != 0)
            snprintf(n->name, sizeof(n->name), "%s", fields[3]);
        else
            snprintf(n->name, sizeof(n->name), "%s", fields[0]);
        snprintf(n->id, sizeof(n->id), "%s", fields[0]);
        n->connected = strcmp(fields[2], "connected") == 0;
        n->available = strcmp(fields[2], "unavailable") != 0;
    }
    (void)pclose(f);
}

static void luna_ethernet_backend_refresh(LunaEthernetSnapshot* state) {
    memset(state, 0, sizeof(*state));
    state->available = 1;
    if (luna_ethernet_command_available("connmanctl"))
        luna_ethernet_refresh_connman(state);
    else if (luna_ethernet_command_available("nmcli"))
        luna_ethernet_refresh_nmcli(state);
    else {
        state->backend = LUNA_ETHERNET_NONE;
        snprintf(state->error, sizeof(state->error), "ConnMan / NetworkManager not found");
    }
}

static int luna_ethernet_valid_connman_service(const char* id) {
    if (!id || strncmp(id, "ethernet_", 9) != 0) return 0;
    for (const char* p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int luna_ethernet_valid_iface_id(const char* id) {
    if (!id || strncmp(id, "iface:", 6) != 0) return 0;
    const char* name = id + 6;
    if (!*name || strlen(name) > 15) return 0;
    for (const char* p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int luna_ethernet_do_command(const LunaEthernetCommand* command,
                                   const LunaEthernetSnapshot* before) {
    if (!command) return 0;
    LunaEthernetBackend backend = before ? before->backend : LUNA_ETHERNET_NONE;
    int powered = before ? before->powered : 0;
    if (command->type == LUNA_ETHERNET_CMD_REFRESH) return 1;
    if (command->type == LUNA_ETHERNET_CMD_SET_POWERED) {
        int target = command->powered != 0;
        if (target == powered) return 1;
        if (backend == LUNA_ETHERNET_CONNMAN) {
            const char* const argv[] = {
                "connmanctl", target ? "enable" : "disable", "ethernet", NULL
            };
            return luna_ethernet_run_wait(argv);
        }
        /* NetworkManager has no ethernet radio switch. */
        return backend == LUNA_ETHERNET_NMCLI;
    }
    if (command->type == LUNA_ETHERNET_CMD_CONNECT) {
        if (backend == LUNA_ETHERNET_CONNMAN &&
            luna_ethernet_valid_connman_service(command->id)) {
            const char* const argv[] = { "connmanctl", "connect", command->id, NULL };
            return luna_ethernet_run_wait(argv);
        }
        if (backend == LUNA_ETHERNET_CONNMAN &&
            luna_ethernet_valid_iface_id(command->id)) {
            /* No ConnMan service yet — bring the link up so DHCP can start. */
            const char* iface = command->id + 6;
            const char* const argv[] = { "ip", "link", "set", "dev", iface, "up", NULL };
            return luna_ethernet_run_wait(argv);
        }
        if (backend == LUNA_ETHERNET_NMCLI && command->id[0]) {
            const char* const argv[] = { "nmcli", "device", "connect", command->id, NULL };
            return luna_ethernet_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_ETHERNET_CMD_DISCONNECT) {
        if (backend == LUNA_ETHERNET_CONNMAN &&
            luna_ethernet_valid_connman_service(command->id)) {
            const char* const argv[] = { "connmanctl", "disconnect", command->id, NULL };
            return luna_ethernet_run_wait(argv);
        }
        if (backend == LUNA_ETHERNET_CONNMAN &&
            luna_ethernet_valid_iface_id(command->id)) {
            const char* iface = command->id + 6;
            const char* const argv[] = { "ip", "link", "set", "dev", iface, "down", NULL };
            return luna_ethernet_run_wait(argv);
        }
        if (backend == LUNA_ETHERNET_NMCLI && command->id[0]) {
            const char* const argv[] = { "nmcli", "device", "disconnect", command->id, NULL };
            return luna_ethernet_run_wait(argv);
        }
        return 0;
    }
    return 0;
}

static void luna_ethernet_publish(const LunaEthernetSnapshot* snapshot) {
    pthread_mutex_lock(&g_luna_ethernet.mutex);
    unsigned long long next = g_luna_ethernet.snapshot.generation + 1;
    g_luna_ethernet.snapshot = *snapshot;
    g_luna_ethernet.snapshot.generation = next;
    pthread_mutex_unlock(&g_luna_ethernet.mutex);
    luna_ethernet_notify();
}

static void* luna_ethernet_thread_main(void* unused) {
    (void)unused;
    for (;;) {
        LunaEthernetCommand command;
        memset(&command, 0, sizeof(command));
        pthread_mutex_lock(&g_luna_ethernet.mutex);
        while (g_luna_ethernet.running && g_luna_ethernet.count == 0)
            pthread_cond_wait(&g_luna_ethernet.cond, &g_luna_ethernet.mutex);
        if (!g_luna_ethernet.running) {
            pthread_mutex_unlock(&g_luna_ethernet.mutex);
            break;
        }
        command = g_luna_ethernet.queue[g_luna_ethernet.head];
        g_luna_ethernet.head = (g_luna_ethernet.head + 1) % LUNA_ETHERNET_QUEUE_CAP;
        g_luna_ethernet.count--;
        LunaEthernetSnapshot before = g_luna_ethernet.snapshot;
        before.busy = 1;
        before.generation++;
        g_luna_ethernet.snapshot = before;
        pthread_mutex_unlock(&g_luna_ethernet.mutex);
        luna_ethernet_notify();

        int ok = 1;
        pthread_mutex_lock(&g_luna_ethernet.child_mutex);
        if (command.type != LUNA_ETHERNET_CMD_REFRESH)
            ok = luna_ethernet_do_command(&command, &before);
        LunaEthernetSnapshot after;
        luna_ethernet_backend_refresh(&after);
        pthread_mutex_unlock(&g_luna_ethernet.child_mutex);
        after.busy = 0;
        if (!ok && !after.error[0])
            snprintf(after.error, sizeof(after.error), "Ethernet operation failed");
        luna_ethernet_publish(&after);
    }
    return NULL;
}

static int luna_ethernet_enqueue(LunaEthernetCommandType type, int powered, const char* id) {
    if (!g_luna_ethernet.initialized) return 0;
    LunaEthernetCommand command;
    memset(&command, 0, sizeof(command));
    command.type = type;
    command.powered = powered != 0;
    if (id) snprintf(command.id, sizeof(command.id), "%s", id);

    pthread_mutex_lock(&g_luna_ethernet.mutex);
    if (!g_luna_ethernet.running || g_luna_ethernet.count >= LUNA_ETHERNET_QUEUE_CAP) {
        pthread_mutex_unlock(&g_luna_ethernet.mutex);
        return 0;
    }
    if (type == LUNA_ETHERNET_CMD_REFRESH && g_luna_ethernet.count > 0) {
        unsigned last = (g_luna_ethernet.tail + LUNA_ETHERNET_QUEUE_CAP - 1) % LUNA_ETHERNET_QUEUE_CAP;
        if (g_luna_ethernet.queue[last].type == LUNA_ETHERNET_CMD_REFRESH) {
            pthread_mutex_unlock(&g_luna_ethernet.mutex);
            return 1;
        }
    }
    g_luna_ethernet.queue[g_luna_ethernet.tail] = command;
    g_luna_ethernet.tail = (g_luna_ethernet.tail + 1) % LUNA_ETHERNET_QUEUE_CAP;
    g_luna_ethernet.count++;
    pthread_cond_signal(&g_luna_ethernet.cond);
    pthread_mutex_unlock(&g_luna_ethernet.mutex);
    return 1;
}

int luna_ethernet_init(const LunaEthernetConfig* config) {
    if (g_luna_ethernet.initialized) return 1;
    memset(&g_luna_ethernet, 0, sizeof(g_luna_ethernet));
    if (pthread_mutex_init(&g_luna_ethernet.mutex, NULL) != 0) return 0;
    if (pthread_mutex_init(&g_luna_ethernet.child_mutex, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_ethernet.mutex);
        return 0;
    }
    if (pthread_cond_init(&g_luna_ethernet.cond, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_ethernet.child_mutex);
        pthread_mutex_destroy(&g_luna_ethernet.mutex);
        return 0;
    }
    g_luna_ethernet.notify = config ? config->notify : NULL;
    g_luna_ethernet.notify_user = config ? config->notify_user : NULL;
    g_luna_ethernet.running = 1;
    g_luna_ethernet.initialized = 1;
    g_luna_ethernet.snapshot.available = 1;
    g_luna_ethernet.snapshot.busy = 1;
    if (pthread_create(&g_luna_ethernet.thread, NULL, luna_ethernet_thread_main, NULL) != 0) {
        g_luna_ethernet.initialized = 0;
        g_luna_ethernet.running = 0;
        pthread_cond_destroy(&g_luna_ethernet.cond);
        pthread_mutex_destroy(&g_luna_ethernet.child_mutex);
        pthread_mutex_destroy(&g_luna_ethernet.mutex);
        return 0;
    }
    (void)luna_ethernet_request_refresh();
    return 1;
}

void luna_ethernet_shutdown(void) {
    if (!g_luna_ethernet.initialized) return;
    pthread_mutex_lock(&g_luna_ethernet.mutex);
    g_luna_ethernet.running = 0;
    pthread_cond_broadcast(&g_luna_ethernet.cond);
    pthread_mutex_unlock(&g_luna_ethernet.mutex);
    pthread_join(g_luna_ethernet.thread, NULL);
    pthread_cond_destroy(&g_luna_ethernet.cond);
    pthread_mutex_destroy(&g_luna_ethernet.child_mutex);
    pthread_mutex_destroy(&g_luna_ethernet.mutex);
    memset(&g_luna_ethernet, 0, sizeof(g_luna_ethernet));
}

int luna_ethernet_request_refresh(void) {
    return luna_ethernet_enqueue(LUNA_ETHERNET_CMD_REFRESH, 0, NULL);
}
int luna_ethernet_request_set_powered(int powered) {
    return luna_ethernet_enqueue(LUNA_ETHERNET_CMD_SET_POWERED, powered != 0, NULL);
}
int luna_ethernet_request_connect(const char* id) {
    if (!id || !*id) return 0;
    return luna_ethernet_enqueue(LUNA_ETHERNET_CMD_CONNECT, 0, id);
}
int luna_ethernet_request_disconnect(const char* id) {
    if (!id || !*id) return 0;
    return luna_ethernet_enqueue(LUNA_ETHERNET_CMD_DISCONNECT, 0, id);
}

int luna_ethernet_consume(LunaEthernetSnapshot* out, unsigned long long* last_generation) {
    if (!out || !g_luna_ethernet.initialized) return 0;
    if (pthread_mutex_trylock(&g_luna_ethernet.mutex) != 0) return 0;
    unsigned long long seen = last_generation ? *last_generation : 0;
    if (g_luna_ethernet.snapshot.generation == seen) {
        pthread_mutex_unlock(&g_luna_ethernet.mutex);
        return 0;
    }
    *out = g_luna_ethernet.snapshot;
    if (last_generation) *last_generation = out->generation;
    pthread_mutex_unlock(&g_luna_ethernet.mutex);
    return 1;
}

int luna_ethernet_running(void) {
    return g_luna_ethernet.initialized && g_luna_ethernet.running;
}

int luna_ethernet_reaper_try_lock(void) {
    if (!g_luna_ethernet.initialized) return 1;
    return pthread_mutex_trylock(&g_luna_ethernet.child_mutex) == 0;
}

void luna_ethernet_reaper_unlock(void) {
    if (g_luna_ethernet.initialized) pthread_mutex_unlock(&g_luna_ethernet.child_mutex);
}

#endif /* LUNA_ETHERNET_IMPLEMENTATION */
#endif /* LUNA_ETHERNET_H */
