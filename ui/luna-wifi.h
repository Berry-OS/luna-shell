/*
 * luna-wifi.h — asynchronous Wi-Fi backend for luna-shell
 *
 * Define LUNA_WIFI_IMPLEMENTATION in exactly one translation unit before
 * including this file.  All ConnMan/NetworkManager I/O runs on an internal
 * worker thread.  The caller only queues commands and consumes immutable
 * snapshots; no UI or OpenGL function is ever called by the worker.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef LUNA_WIFI_H
#define LUNA_WIFI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_WIFI_MAX_NETWORKS
#define LUNA_WIFI_MAX_NETWORKS 8
#endif

typedef enum LunaWifiBackend {
    LUNA_WIFI_NONE = 0,
    LUNA_WIFI_CONNMAN = 1,
    LUNA_WIFI_NMCLI = 2
} LunaWifiBackend;

typedef struct LunaWifiNetwork {
    char name[96];
    char id[192];
    int connected;
    int saved;
    int secure;
    int signal;
} LunaWifiNetwork;

typedef struct LunaWifiSnapshot {
    LunaWifiBackend backend;
    LunaWifiNetwork networks[LUNA_WIFI_MAX_NETWORKS];
    int count;
    int powered;
    int available;
    int busy;
    char error[96];
    unsigned long long generation;
} LunaWifiSnapshot;

typedef void (*LunaWifiNotifyFn)(void* user);

typedef struct LunaWifiConfig {
    LunaWifiNotifyFn notify;
    void* notify_user;
} LunaWifiConfig;

int  luna_wifi_init(const LunaWifiConfig* config);
void luna_wifi_shutdown(void);
int  luna_wifi_request_refresh(void);
int  luna_wifi_request_toggle(void);
int  luna_wifi_request_set_powered(int powered);
int  luna_wifi_request_scan(void);
int  luna_wifi_request_connect(const char* id, const char* passphrase);
int  luna_wifi_request_disconnect(const char* id);
int  luna_wifi_consume(LunaWifiSnapshot* out, unsigned long long* last_generation);
int  luna_wifi_running(void);

/* luna-shell has a process-wide SIGCHLD reaper.  popen()/pclose() and the
 * interactive ConnMan helper must own their children until completion, so the
 * shell reaper uses this non-blocking guard before waitpid(-1, ...). */
int  luna_wifi_reaper_try_lock(void);
void luna_wifi_reaper_unlock(void);

#ifdef __cplusplus
}
#endif

#ifdef LUNA_WIFI_IMPLEMENTATION

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

#define LUNA_WIFI_QUEUE_CAP 16

typedef enum LunaWifiCommandType {
    LUNA_WIFI_CMD_REFRESH = 1,
    LUNA_WIFI_CMD_TOGGLE,
    LUNA_WIFI_CMD_SET_POWERED,
    LUNA_WIFI_CMD_SCAN,
    LUNA_WIFI_CMD_CONNECT,
    LUNA_WIFI_CMD_DISCONNECT
} LunaWifiCommandType;

typedef struct LunaWifiCommand {
    LunaWifiCommandType type;
    int powered;
    char id[192];
    char passphrase[128];
} LunaWifiCommand;

typedef struct LunaWifiWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_mutex_t child_mutex;
    pthread_cond_t cond;
    int initialized;
    int running;
    LunaWifiCommand queue[LUNA_WIFI_QUEUE_CAP];
    unsigned head;
    unsigned tail;
    unsigned count;
    LunaWifiSnapshot snapshot;
    LunaWifiNotifyFn notify;
    void* notify_user;
} LunaWifiWorker;

static LunaWifiWorker g_luna_wifi;

static void luna_wifi_notify(void) {
    LunaWifiNotifyFn fn;
    void* user;
    pthread_mutex_lock(&g_luna_wifi.mutex);
    fn = g_luna_wifi.notify;
    user = g_luna_wifi.notify_user;
    pthread_mutex_unlock(&g_luna_wifi.mutex);
    if (fn) fn(user);
}

static int luna_wifi_command_path(const char* name, char* out, size_t out_n) {
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

static int luna_wifi_command_available(const char* name) {
    char path[512];
    return luna_wifi_command_path(name, path, sizeof(path));
}

static void luna_wifi_trim_line(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int luna_wifi_run_wait(const char* const argv[]) {
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

static int luna_wifi_powered_connman(void) {
    int enabled = 0;
    FILE* f = popen("connmanctl technologies 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int in_wifi = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '/') {
            in_wifi = strstr(line, "technology/wifi") != NULL;
            continue;
        }
        if (in_wifi && strstr(line, "Powered") && strstr(line, "True")) {
            enabled = 1;
            break;
        }
    }
    (void)pclose(f);
    return enabled;
}

static char* luna_wifi_find_last_token(char* text, const char* needle) {
    char* last = NULL;
    char* p = text;
    while ((p = strstr(p, needle)) != NULL) {
        last = p;
        p++;
    }
    return last;
}

static void luna_wifi_refresh_connman(LunaWifiSnapshot* state) {
    state->backend = LUNA_WIFI_CONNMAN;
    state->powered = luna_wifi_powered_connman();
    if (!state->powered) return;

    FILE* f = popen("connmanctl services 2>/dev/null", "r");
    if (!f) {
        snprintf(state->error, sizeof(state->error), "Cannot query ConnMan");
        return;
    }
    char line[512];
    while (state->count < LUNA_WIFI_MAX_NETWORKS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char flags[5] = {0};
        size_t prefix = strlen(line) < 4 ? strlen(line) : 4;
        memcpy(flags, line, prefix);
        char* label = line + prefix;
        char* service = luna_wifi_find_last_token(label, "wifi_");
        if (!service) continue;
        char id[192];
        size_t id_len = 0;
        while (service[id_len] && !isspace((unsigned char)service[id_len])) id_len++;
        if (id_len == 0 || id_len >= sizeof(id)) continue;
        memcpy(id, service, id_len);
        id[id_len] = 0;
        *service = 0;
        luna_wifi_trim_line(label);

        LunaWifiNetwork* n = &state->networks[state->count++];
        snprintf(n->name, sizeof(n->name), "%.95s", *label ? label : "Hidden network");
        snprintf(n->id, sizeof(n->id), "%s", id);
        n->connected = strchr(flags, 'R') != NULL || strchr(flags, 'O') != NULL;
        n->saved = strchr(flags, '*') != NULL;
        n->secure = strstr(id, "_managed_none") == NULL;
        n->signal = -1;
    }
    (void)pclose(f);
}

static int luna_wifi_nmcli_split(char* line, char* fields[], int wanted) {
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

static void luna_wifi_refresh_nmcli(LunaWifiSnapshot* state) {
    state->backend = LUNA_WIFI_NMCLI;
    FILE* f = popen("nmcli -t -f WIFI general 2>/dev/null", "r");
    char line[512];
    if (f && fgets(line, sizeof(line), f))
        state->powered = strstr(line, "enabled") != NULL;
    if (f) (void)pclose(f);
    if (!state->powered) return;

    f = popen("nmcli -t --escape yes -f IN-USE,SSID,SIGNAL,SECURITY device wifi list 2>/dev/null", "r");
    if (!f) {
        snprintf(state->error, sizeof(state->error), "Cannot query NetworkManager");
        return;
    }
    while (state->count < LUNA_WIFI_MAX_NETWORKS && fgets(line, sizeof(line), f)) {
        luna_wifi_trim_line(line);
        char* fields[4] = {0};
        if (luna_wifi_nmcli_split(line, fields, 4) != 4) continue;
        LunaWifiNetwork* n = &state->networks[state->count++];
        snprintf(n->name, sizeof(n->name), "%s", fields[1][0] ? fields[1] : "Hidden network");
        snprintf(n->id, sizeof(n->id), "%s", fields[1]);
        n->connected = fields[0][0] == '*';
        n->saved = n->connected;
        n->secure = fields[3][0] != 0 && strcmp(fields[3], "--") != 0;
        n->signal = atoi(fields[2]);
    }
    (void)pclose(f);
}

static void luna_wifi_backend_refresh(LunaWifiSnapshot* state) {
    memset(state, 0, sizeof(*state));
    state->available = 1;
    if (luna_wifi_command_available("connmanctl"))
        luna_wifi_refresh_connman(state);
    else if (luna_wifi_command_available("nmcli"))
        luna_wifi_refresh_nmcli(state);
    else {
        state->backend = LUNA_WIFI_NONE;
        snprintf(state->error, sizeof(state->error), "ConnMan / NetworkManager not found");
    }
}

static int luna_wifi_valid_connman_service(const char* id) {
    if (!id || strncmp(id, "wifi_", 5) != 0) return 0;
    for (const char* p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int luna_wifi_write_all(int fd, const char* text) {
    size_t left = strlen(text);
    while (left) {
        ssize_t n = write(fd, text, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        text += n;
        left -= (size_t)n;
    }
    return 1;
}

static long long luna_wifi_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

typedef enum LunaWifiPtyEvent {
    LUNA_WIFI_PTY_TIMEOUT = 0,
    LUNA_WIFI_PTY_PROMPT,
    LUNA_WIFI_PTY_AGENT_READY,
    LUNA_WIFI_PTY_PASSPHRASE,
    LUNA_WIFI_PTY_CONNECTED,
    LUNA_WIFI_PTY_FAILED,
    LUNA_WIFI_PTY_IDENTITY,
    LUNA_WIFI_PTY_HIDDEN_NAME
} LunaWifiPtyEvent;

static LunaWifiPtyEvent luna_wifi_pty_wait_event(int fd, int timeout_ms) {
    char window[8192] = {0};
    size_t used = 0;
    long long deadline = luna_wifi_monotonic_ms() + timeout_ms;
    for (;;) {
        int remain = (int)(deadline - luna_wifi_monotonic_ms());
        if (remain <= 0) return LUNA_WIFI_PTY_TIMEOUT;
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLHUP };
        int ready = poll(&pfd, 1, remain);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return LUNA_WIFI_PTY_FAILED;
        }
        if (ready == 0) return LUNA_WIFI_PTY_TIMEOUT;
        char chunk[512];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) return LUNA_WIFI_PTY_FAILED;
        if (used + (size_t)n >= sizeof(window)) {
            size_t keep = sizeof(window) / 2;
            memmove(window, window + used - keep, keep);
            used = keep;
        }
        memcpy(window + used, chunk, (size_t)n);
        used += (size_t)n;
        window[used] = 0;
        if (strstr(window, "Passphrase?")) return LUNA_WIFI_PTY_PASSPHRASE;
        if (strstr(window, "Identity?") || strstr(window, "EAP username?")) return LUNA_WIFI_PTY_IDENTITY;
        if (strstr(window, "Hidden SSID name?") || strstr(window, "SSID name?")) return LUNA_WIFI_PTY_HIDDEN_NAME;
        if (strstr(window, "Connected ") || strstr(window, "Already connected")) return LUNA_WIFI_PTY_CONNECTED;
        if (strstr(window, "Agent registered")) return LUNA_WIFI_PTY_AGENT_READY;
        if (strstr(window, "Error") || strstr(window, "Failed") ||
            strstr(window, "invalid-key") || strstr(window, "Invalid key")) return LUNA_WIFI_PTY_FAILED;
        if (strstr(window, "connmanctl>")) return LUNA_WIFI_PTY_PROMPT;
    }
}

static int luna_wifi_open_connman_pty(pid_t* child_out) {
    char slave_name[128];
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        close(master);
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, master);
    if (slave > STDERR_FILENO) posix_spawn_file_actions_addclose(&actions, slave);
    char* argv[] = { (char*)"connmanctl", NULL };
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "connmanctl", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(slave);
    if (rc != 0) {
        close(master);
        return -1;
    }
    *child_out = pid;
    return master;
}

static void luna_wifi_stop_pty_child(int master, pid_t child) {
    (void)luna_wifi_write_all(master, "quit\n");
    close(master);
    for (int i = 0; i < 20; i++) {
        if (waitpid(child, NULL, WNOHANG) == child) return;
        (void)poll(NULL, 0, 50);
    }
    kill(child, SIGTERM);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int luna_wifi_connman_connect_interactive(const char* service, const char* passphrase) {
    if (!luna_wifi_valid_connman_service(service) || !passphrase || !*passphrase) return 0;
    if (strchr(passphrase, '\n') || strchr(passphrase, '\r')) return 0;
    pid_t child = -1;
    int master = luna_wifi_open_connman_pty(&child);
    if (master < 0) return 0;
    LunaWifiPtyEvent ev = luna_wifi_pty_wait_event(master, 5000);
    if (ev != LUNA_WIFI_PTY_PROMPT) {
        luna_wifi_stop_pty_child(master, child);
        return 0;
    }
    if (!luna_wifi_write_all(master, "agent on\n")) {
        luna_wifi_stop_pty_child(master, child);
        return 0;
    }
    ev = luna_wifi_pty_wait_event(master, 5000);
    if (ev != LUNA_WIFI_PTY_AGENT_READY && ev != LUNA_WIFI_PTY_PROMPT) {
        luna_wifi_stop_pty_child(master, child);
        return 0;
    }
    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd), "connect %s\n", service);
    if (n <= 0 || (size_t)n >= sizeof(cmd) || !luna_wifi_write_all(master, cmd)) {
        luna_wifi_stop_pty_child(master, child);
        return 0;
    }
    int success = 0;
    int passphrase_sent = 0;
    long long deadline = luna_wifi_monotonic_ms() + 35000;
    while (luna_wifi_monotonic_ms() < deadline) {
        ev = luna_wifi_pty_wait_event(master, (int)(deadline - luna_wifi_monotonic_ms()));
        if (ev == LUNA_WIFI_PTY_CONNECTED) { success = 1; break; }
        if (ev == LUNA_WIFI_PTY_PASSPHRASE && !passphrase_sent) {
            if (!luna_wifi_write_all(master, passphrase) || !luna_wifi_write_all(master, "\n")) break;
            passphrase_sent = 1;
            continue;
        }
        if (ev == LUNA_WIFI_PTY_PROMPT) continue;
        break;
    }
    luna_wifi_stop_pty_child(master, child);
    return success;
}

static int luna_wifi_do_command(const LunaWifiCommand* command, const LunaWifiSnapshot* before) {
    if (!command) return 0;
    LunaWifiBackend backend = before ? before->backend : LUNA_WIFI_NONE;
    int powered = before ? before->powered : 0;
    if (command->type == LUNA_WIFI_CMD_REFRESH) return 1;
    if (command->type == LUNA_WIFI_CMD_TOGGLE ||
        command->type == LUNA_WIFI_CMD_SET_POWERED) {
        int target = command->type == LUNA_WIFI_CMD_SET_POWERED
            ? (command->powered != 0) : !powered;
        if (command->type == LUNA_WIFI_CMD_SET_POWERED && target == powered)
            return 1;
        if (backend == LUNA_WIFI_CONNMAN) {
            const char* const argv[] = { "connmanctl", target ? "enable" : "disable", "wifi", NULL };
            return luna_wifi_run_wait(argv);
        }
        if (backend == LUNA_WIFI_NMCLI) {
            const char* const argv[] = { "nmcli", "radio", "wifi", target ? "on" : "off", NULL };
            return luna_wifi_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_WIFI_CMD_SCAN) {
        if (backend == LUNA_WIFI_CONNMAN) {
            const char* const argv[] = { "connmanctl", "scan", "wifi", NULL };
            return luna_wifi_run_wait(argv);
        }
        if (backend == LUNA_WIFI_NMCLI) {
            const char* const argv[] = { "nmcli", "device", "wifi", "rescan", NULL };
            return luna_wifi_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_WIFI_CMD_CONNECT) {
        if (backend == LUNA_WIFI_CONNMAN && luna_wifi_valid_connman_service(command->id)) {
            if (command->passphrase[0])
                return luna_wifi_connman_connect_interactive(command->id, command->passphrase);
            const char* const argv[] = { "connmanctl", "connect", command->id, NULL };
            return luna_wifi_run_wait(argv);
        }
        if (backend == LUNA_WIFI_NMCLI && command->id[0]) {
            if (command->passphrase[0]) {
                const char* const argv[] = { "nmcli", "device", "wifi", "connect", command->id,
                                             "password", command->passphrase, NULL };
                return luna_wifi_run_wait(argv);
            }
            const char* const argv[] = { "nmcli", "device", "wifi", "connect", command->id, NULL };
            return luna_wifi_run_wait(argv);
        }
        return 0;
    }
    if (command->type == LUNA_WIFI_CMD_DISCONNECT) {
        if (backend == LUNA_WIFI_CONNMAN && luna_wifi_valid_connman_service(command->id)) {
            const char* const argv[] = { "connmanctl", "disconnect", command->id, NULL };
            return luna_wifi_run_wait(argv);
        }
        if (backend == LUNA_WIFI_NMCLI && command->id[0]) {
            const char* const argv[] = { "nmcli", "connection", "down", "id", command->id, NULL };
            return luna_wifi_run_wait(argv);
        }
        return 0;
    }
    return 0;
}

static void luna_wifi_publish(const LunaWifiSnapshot* snapshot) {
    pthread_mutex_lock(&g_luna_wifi.mutex);
    unsigned long long next = g_luna_wifi.snapshot.generation + 1;
    g_luna_wifi.snapshot = *snapshot;
    g_luna_wifi.snapshot.generation = next;
    pthread_mutex_unlock(&g_luna_wifi.mutex);
    luna_wifi_notify();
}

static void* luna_wifi_thread_main(void* unused) {
    (void)unused;
    for (;;) {
        LunaWifiCommand command;
        memset(&command, 0, sizeof(command));
        pthread_mutex_lock(&g_luna_wifi.mutex);
        while (g_luna_wifi.running && g_luna_wifi.count == 0)
            pthread_cond_wait(&g_luna_wifi.cond, &g_luna_wifi.mutex);
        if (!g_luna_wifi.running) {
            pthread_mutex_unlock(&g_luna_wifi.mutex);
            break;
        }
        command = g_luna_wifi.queue[g_luna_wifi.head];
        g_luna_wifi.head = (g_luna_wifi.head + 1) % LUNA_WIFI_QUEUE_CAP;
        g_luna_wifi.count--;
        LunaWifiSnapshot before = g_luna_wifi.snapshot;
        before.busy = 1;
        before.generation++;
        g_luna_wifi.snapshot = before;
        pthread_mutex_unlock(&g_luna_wifi.mutex);
        luna_wifi_notify();

        int ok = 1;
        pthread_mutex_lock(&g_luna_wifi.child_mutex);
        if (command.type != LUNA_WIFI_CMD_REFRESH)
            ok = luna_wifi_do_command(&command, &before);
        LunaWifiSnapshot after;
        luna_wifi_backend_refresh(&after);
        pthread_mutex_unlock(&g_luna_wifi.child_mutex);
        after.busy = 0;
        if (!ok && !after.error[0])
            snprintf(after.error, sizeof(after.error), "Wi-Fi operation failed");
        luna_wifi_publish(&after);
        memset(command.passphrase, 0, sizeof(command.passphrase));
    }
    return NULL;
}

static int luna_wifi_enqueue(LunaWifiCommandType type, int powered, const char* id, const char* passphrase) {
    if (!g_luna_wifi.initialized) return 0;
    LunaWifiCommand command;
    memset(&command, 0, sizeof(command));
    command.type = type;
    command.powered = powered != 0;
    if (id) snprintf(command.id, sizeof(command.id), "%s", id);
    if (passphrase) snprintf(command.passphrase, sizeof(command.passphrase), "%s", passphrase);

    pthread_mutex_lock(&g_luna_wifi.mutex);
    if (!g_luna_wifi.running || g_luna_wifi.count >= LUNA_WIFI_QUEUE_CAP) {
        pthread_mutex_unlock(&g_luna_wifi.mutex);
        memset(command.passphrase, 0, sizeof(command.passphrase));
        return 0;
    }
    /* Coalesce adjacent refresh requests. */
    if (type == LUNA_WIFI_CMD_REFRESH && g_luna_wifi.count > 0) {
        unsigned last = (g_luna_wifi.tail + LUNA_WIFI_QUEUE_CAP - 1) % LUNA_WIFI_QUEUE_CAP;
        if (g_luna_wifi.queue[last].type == LUNA_WIFI_CMD_REFRESH) {
            pthread_mutex_unlock(&g_luna_wifi.mutex);
            return 1;
        }
    }
    g_luna_wifi.queue[g_luna_wifi.tail] = command;
    g_luna_wifi.tail = (g_luna_wifi.tail + 1) % LUNA_WIFI_QUEUE_CAP;
    g_luna_wifi.count++;
    pthread_cond_signal(&g_luna_wifi.cond);
    pthread_mutex_unlock(&g_luna_wifi.mutex);
    return 1;
}

int luna_wifi_init(const LunaWifiConfig* config) {
    if (g_luna_wifi.initialized) return 1;
    memset(&g_luna_wifi, 0, sizeof(g_luna_wifi));
    if (pthread_mutex_init(&g_luna_wifi.mutex, NULL) != 0) return 0;
    if (pthread_mutex_init(&g_luna_wifi.child_mutex, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_wifi.mutex);
        return 0;
    }
    if (pthread_cond_init(&g_luna_wifi.cond, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_wifi.child_mutex);
        pthread_mutex_destroy(&g_luna_wifi.mutex);
        return 0;
    }
    g_luna_wifi.notify = config ? config->notify : NULL;
    g_luna_wifi.notify_user = config ? config->notify_user : NULL;
    g_luna_wifi.running = 1;
    g_luna_wifi.initialized = 1;
    g_luna_wifi.snapshot.available = 1;
    g_luna_wifi.snapshot.busy = 1;
    if (pthread_create(&g_luna_wifi.thread, NULL, luna_wifi_thread_main, NULL) != 0) {
        g_luna_wifi.initialized = 0;
        g_luna_wifi.running = 0;
        pthread_cond_destroy(&g_luna_wifi.cond);
        pthread_mutex_destroy(&g_luna_wifi.child_mutex);
        pthread_mutex_destroy(&g_luna_wifi.mutex);
        return 0;
    }
    (void)luna_wifi_request_refresh();
    return 1;
}

void luna_wifi_shutdown(void) {
    if (!g_luna_wifi.initialized) return;
    pthread_mutex_lock(&g_luna_wifi.mutex);
    g_luna_wifi.running = 0;
    pthread_cond_broadcast(&g_luna_wifi.cond);
    pthread_mutex_unlock(&g_luna_wifi.mutex);
    pthread_join(g_luna_wifi.thread, NULL);
    for (unsigned i = 0; i < LUNA_WIFI_QUEUE_CAP; i++)
        memset(g_luna_wifi.queue[i].passphrase, 0, sizeof(g_luna_wifi.queue[i].passphrase));
    pthread_cond_destroy(&g_luna_wifi.cond);
    pthread_mutex_destroy(&g_luna_wifi.child_mutex);
    pthread_mutex_destroy(&g_luna_wifi.mutex);
    memset(&g_luna_wifi, 0, sizeof(g_luna_wifi));
}

int luna_wifi_request_refresh(void) { return luna_wifi_enqueue(LUNA_WIFI_CMD_REFRESH, 0, NULL, NULL); }
int luna_wifi_request_toggle(void) { return luna_wifi_enqueue(LUNA_WIFI_CMD_TOGGLE, 0, NULL, NULL); }
int luna_wifi_request_set_powered(int powered) {
    return luna_wifi_enqueue(LUNA_WIFI_CMD_SET_POWERED, powered != 0, NULL, NULL);
}
int luna_wifi_request_scan(void) { return luna_wifi_enqueue(LUNA_WIFI_CMD_SCAN, 0, NULL, NULL); }
int luna_wifi_request_connect(const char* id, const char* passphrase) {
    if (!id || !*id) return 0;
    return luna_wifi_enqueue(LUNA_WIFI_CMD_CONNECT, 0, id, passphrase ? passphrase : "");
}
int luna_wifi_request_disconnect(const char* id) {
    if (!id || !*id) return 0;
    return luna_wifi_enqueue(LUNA_WIFI_CMD_DISCONNECT, 0, id, NULL);
}

int luna_wifi_consume(LunaWifiSnapshot* out, unsigned long long* last_generation) {
    if (!out || !g_luna_wifi.initialized) return 0;
    /* The render thread must never wait for the worker.  A contended snapshot
     * is simply consumed on the next frame/event wake. */
    if (pthread_mutex_trylock(&g_luna_wifi.mutex) != 0) return 0;
    unsigned long long seen = last_generation ? *last_generation : 0;
    if (g_luna_wifi.snapshot.generation == seen) {
        pthread_mutex_unlock(&g_luna_wifi.mutex);
        return 0;
    }
    *out = g_luna_wifi.snapshot;
    if (last_generation) *last_generation = out->generation;
    pthread_mutex_unlock(&g_luna_wifi.mutex);
    return 1;
}

int luna_wifi_running(void) {
    return g_luna_wifi.initialized && g_luna_wifi.running;
}

int luna_wifi_reaper_try_lock(void) {
    if (!g_luna_wifi.initialized) return 1;
    return pthread_mutex_trylock(&g_luna_wifi.child_mutex) == 0;
}

void luna_wifi_reaper_unlock(void) {
    if (g_luna_wifi.initialized) pthread_mutex_unlock(&g_luna_wifi.child_mutex);
}

#endif /* LUNA_WIFI_IMPLEMENTATION */
#endif /* LUNA_WIFI_H */
