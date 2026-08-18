/*
 * luna-clipboard — Wayland clipboard manager for Luna Desktop
 *
 * Persists the seat selection so copy/paste keeps working after the source
 * client closes, loses focus, or clears its offer. Uses wl_data_device
 * (no wlr-data-control required) against luna-compositor.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#define HIST_MAX       32
#define CLIP_MAX_BYTES (64 * 1024 * 1024)
#define CLIP_MAX_MIMES 64
#define CLIP_MAX_TOTAL_BYTES (128 * 1024 * 1024)

static volatile sig_atomic_t g_running = 1;

static const char *const PREFERRED_MIMES[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "TEXT",
    "STRING",
    NULL
};

typedef struct {
    char  *data;
    size_t len;
    char  *mime; /* primary representation (used by the history UI) */
    struct {
        char *mime;
        char *data;
        size_t len;
    } *variants;
    size_t variant_count;
} ClipEntry;

typedef struct {
    struct wl_display             *display;
    struct wl_registry            *registry;
    struct wl_seat                *seat;
    struct wl_data_device_manager *ddm;
    struct wl_data_device         *device;
    struct wl_data_source         *source;
    struct wl_data_offer          *offer;
    char                         **offer_mimes;
    size_t                         offer_mime_count;
    uint32_t                       seat_name;
    uint32_t                       ddm_name;
    bool                           own_selection;
    /* Set just before set_selection; cleared when the compositor echoes our
     * offer back.  Distinguishes our echo from a real GTK/Qt takeover that
     * can race ahead of (or without) data_source.cancelled. */
    bool                           expect_self_offer;
    bool                           verbose;
    ClipEntry                      current;
    ClipEntry                      history[HIST_MAX];
    int                            hist_count;
    int                            cmd_fd;
} App;

static App g;
static struct wl_data_offer *g_pending_offer = NULL;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void die(const char *msg) {
    fprintf(stderr, "luna-clipboard: %s\n", msg);
    exit(1);
}

static void clip_free(ClipEntry *e) {
    for (size_t i = 0; i < e->variant_count; i++) {
        free(e->variants[i].mime);
        free(e->variants[i].data);
    }
    free(e->variants);
    free(e->data);
    free(e->mime);
    e->data = NULL;
    e->mime = NULL;
    e->len = 0;
    e->variants = NULL;
    e->variant_count = 0;
}

static bool mime_is_text(const char *mime) {
    return mime && (!strncmp(mime, "text/", 5) || !strcmp(mime, "UTF8_STRING") ||
                    !strcmp(mime, "TEXT") || !strcmp(mime, "STRING"));
}

static bool mime_is_plain_text(const char *mime) {
    if (!mime)
        return false;
    for (int i = 0; PREFERRED_MIMES[i]; i++)
        if (!strcmp(mime, PREFERRED_MIMES[i]))
            return true;
    return false;
}

static bool clip_has_mime(const ClipEntry *e, const char *mime) {
    for (size_t i = 0; i < e->variant_count; i++)
        if (!strcmp(e->variants[i].mime, mime))
            return true;
    return false;
}

static const void *clip_payload(const ClipEntry *e, const char *mime,
                                size_t *len) {
    for (size_t i = 0; i < e->variant_count; i++) {
        if (!strcmp(e->variants[i].mime, mime)) {
            *len = e->variants[i].len;
            return e->variants[i].data;
        }
    }
    /* History files written by older versions contain only the MIME that was
     * preferred by the copying client.  All plain-text aliases carry the same
     * bytes, so allow a paste target to request a different common alias. */
    if (mime_is_plain_text(mime)) {
        for (int preferred = 0; PREFERRED_MIMES[preferred]; preferred++) {
            for (size_t i = 0; i < e->variant_count; i++) {
                if (!strcmp(e->variants[i].mime, PREFERRED_MIMES[preferred])) {
                    *len = e->variants[i].len;
                    return e->variants[i].data;
                }
            }
        }
    }
    *len = 0;
    return NULL;
}

static bool clip_add(ClipEntry *e, const char *mime, const void *data, size_t len,
                     bool primary) {
    if (!mime || !data || !len || e->variant_count >= CLIP_MAX_MIMES)
        return false;
    for (size_t i = 0; i < e->variant_count; i++)
        if (!strcmp(e->variants[i].mime, mime))
            return true;
    void *nv = realloc(e->variants, (e->variant_count + 1) * sizeof(*e->variants));
    if (!nv)
        return false;
    e->variants = nv;
    size_t i = e->variant_count;
    e->variants[i].mime = strdup(mime);
    e->variants[i].data = malloc(len);
    if (!e->variants[i].mime || !e->variants[i].data) {
        free(e->variants[i].mime);
        free(e->variants[i].data);
        return false;
    }
    memcpy(e->variants[i].data, data, len);
    e->variants[i].len = len;
    e->variant_count++;

    if (primary || !e->data || (mime_is_text(mime) && !mime_is_text(e->mime))) {
        char *copy = malloc(len + 1);
        char *type = strdup(mime);
        if (copy && type) {
            memcpy(copy, data, len);
            copy[len] = '\0';
            free(e->data);
            free(e->mime);
            e->data = copy;
            e->mime = type;
            e->len = len;
        } else {
            free(copy);
            free(type);
        }
    }
    return true;
}

static void clip_set(ClipEntry *e, const char *mime, const void *data, size_t len) {
    clip_free(e);
    clip_add(e, mime ? mime : "text/plain;charset=utf-8", data, len, true);
}

static void clip_copy(ClipEntry *dst, const ClipEntry *src) {
    clip_free(dst);
    for (size_t i = 0; i < src->variant_count; i++)
        clip_add(dst, src->variants[i].mime, src->variants[i].data,
                 src->variants[i].len, src->mime && !strcmp(src->mime, src->variants[i].mime));
}

static bool clip_equal(const ClipEntry *a, const ClipEntry *b) {
    if (a->variant_count != b->variant_count)
        return false;
    for (size_t i = 0; i < a->variant_count; i++) {
        bool found = false;
        for (size_t j = 0; j < b->variant_count; j++) {
            if (!strcmp(a->variants[i].mime, b->variants[j].mime) &&
                a->variants[i].len == b->variants[j].len &&
                !memcmp(a->variants[i].data, b->variants[j].data,
                        a->variants[i].len)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void history_push(const ClipEntry *e) {
    if (!e->data || e->len == 0)
        return;
    /* Dedup against the most recent entry. */
    if (g.hist_count > 0 && clip_equal(&g.history[0], e))
        return;
    if (g.hist_count == HIST_MAX)
        clip_free(&g.history[HIST_MAX - 1]);
    else
        g.hist_count++;
    for (int i = g.hist_count - 1; i > 0; i--)
        g.history[i] = g.history[i - 1];
    memset(&g.history[0], 0, sizeof(g.history[0]));
    clip_copy(&g.history[0], e);
}

/* Durable history directory under XDG_DATA_HOME (not cache). */
static void history_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg && xdg[0] == '/')
        snprintf(buf, n, "%s/luna-clipboard", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.local/share/luna-clipboard", home);
    else
        snprintf(buf, n, "/tmp/luna-clipboard-%d", (int)getuid());
}

/* Pre-migration location; read-only fallback / one-shot migrate source. */
static void history_path_legacy_cache(char *buf, size_t n) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg && xdg[0] == '/')
        snprintf(buf, n, "%s/luna-clipboard", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.cache/luna-clipboard", home);
    else
        snprintf(buf, n, "/tmp/luna-clipboard-%d", (int)getuid());
}

/* Single-instance lock under XDG_RUNTIME_DIR so luna-session + luna-shell
 * auto-start cannot race two managers onto the same seat. */
static int acquire_singleton_lock(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[512];
    if (rt && *rt)
        snprintf(path, sizeof(path), "%s/luna-clipboard.lock", rt);
    else
        snprintf(path, sizeof(path), "/tmp/luna-clipboard-%d.lock", (int)getuid());
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    /* Keep fd open for the process lifetime. */
    return fd;
}

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len;
    if (!path || path[0] != '/')
        return -1;
    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void history_save(void) {
    char dir[512], path[576], tmp[580];
    history_path(dir, sizeof(dir));
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "luna-clipboard: cannot create %s: %s\n", dir, strerror(errno));
        return;
    }
    snprintf(path, sizeof(path), "%s/history", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "luna-clipboard: cannot write %s: %s\n", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < g.hist_count; i++) {
        ClipEntry *e = &g.history[i];
        if (!e->data)
            continue;
        fprintf(f, "%zu\n%s\n", e->len, e->mime ? e->mime : "text/plain");
        fwrite(e->data, 1, e->len, f);
        fputc('\n', f);
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        fprintf(stderr, "luna-clipboard: write failed %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "luna-clipboard: cannot replace %s: %s\n", path, strerror(errno));
        unlink(tmp);
    }
}

static void become_selection_owner(void); /* forward */

static void history_load(void) {
    char dir[512], path[576], legacy_dir[512], legacy_path[576];
    int migrated = 0;
    history_path(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/history", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* One-shot migration from the old XDG_CACHE_HOME location. */
        history_path_legacy_cache(legacy_dir, sizeof(legacy_dir));
        snprintf(legacy_path, sizeof(legacy_path), "%s/history", legacy_dir);
        f = fopen(legacy_path, "rb");
        if (f)
            migrated = 1;
    }
    if (!f)
        return;
    /* File stores newest-first, matching history[0]. */
    while (g.hist_count < HIST_MAX) {
        char lenbuf[64], mime[256];
        if (!fgets(lenbuf, sizeof(lenbuf), f))
            break;
        size_t len = (size_t)strtoul(lenbuf, NULL, 10);
        if (!fgets(mime, sizeof(mime), f))
            break;
        size_t ml = strlen(mime);
        while (ml > 0 && (mime[ml - 1] == '\n' || mime[ml - 1] == '\r'))
            mime[--ml] = 0;
        if (len == 0 || len > CLIP_MAX_BYTES)
            break;
        char *data = malloc(len + 1);
        if (!data)
            break;
        if (fread(data, 1, len, f) != len) {
            free(data);
            break;
        }
        data[len] = 0;
        int c = fgetc(f);
        (void)c;
        clip_set(&g.history[g.hist_count], mime, data, len);
        free(data);
        g.hist_count++;
    }
    fclose(f);
    if (g.hist_count > 0 && g.history[0].data)
        clip_copy(&g.current, &g.history[0]);
    if (migrated && g.hist_count > 0) {
        history_save();
        unlink(legacy_path);
        if (g.verbose)
            fprintf(stderr, "luna-clipboard: migrated history → %s\n", path);
    }
}

static void cmd_sock_path(char *buf, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
        snprintf(buf, n, "%s/luna-clipboard.sock", rt);
    else
        snprintf(buf, n, "/tmp/luna-clipboard-%d.sock", (int)getuid());
}

static int open_cmd_socket(void) {
    char path[512];
    cmd_sock_path(path, sizeof(path));
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, n + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static void write_clip_entry(int fd, const ClipEntry *e) {
    if (!e || !e->data || e->len == 0)
        return;
    char hdr[320];
    int n = snprintf(hdr, sizeof(hdr), "%zu\n%s\n", e->len,
                     e->mime ? e->mime : "text/plain");
    if (n > 0)
        (void)write(fd, hdr, (size_t)n);
    (void)write(fd, e->data, e->len);
    (void)write(fd, "\n", 1);
}

static void handle_cmd_line(const char *line, int reply_fd) {
    if (!line || !*line)
        return;
    if (!strcmp(line, "list")) {
        /* In-memory history — the menu must work even when history_save fails. */
        if (g.hist_count > 0) {
            for (int i = 0; i < g.hist_count; i++)
                write_clip_entry(reply_fd, &g.history[i]);
        } else if (g.current.data) {
            write_clip_entry(reply_fd, &g.current);
        }
    } else if (!strncmp(line, "select ", 7)) {
        int idx = atoi(line + 7);
        if (idx < 0 || idx >= g.hist_count || !g.history[idx].data)
            return;
        clip_copy(&g.current, &g.history[idx]);
        /* Move selected entry to front of history. */
        ClipEntry picked = g.history[idx];
        for (int i = idx; i > 0; i--)
            g.history[i] = g.history[i - 1];
        g.history[0] = picked;
        history_save();
        become_selection_owner();
        if (g.verbose)
            fprintf(stderr, "luna-clipboard: re-selected history[%d] (%zu bytes)\n",
                    idx, g.current.len);
    } else if (!strcmp(line, "clear")) {
        for (int i = 0; i < g.hist_count; i++)
            clip_free(&g.history[i]);
        g.hist_count = 0;
        clip_free(&g.current);
        history_save();
        if (g.source) {
            wl_data_source_destroy(g.source);
            g.source = NULL;
            g.own_selection = false;
        }
        if (g.verbose)
            fprintf(stderr, "luna-clipboard: history cleared\n");
    }
}

static void poll_cmd_socket(void) {
    if (g.cmd_fd < 0)
        return;
    for (;;) {
        int cfd = accept4(g.cmd_fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0)
            break;
        char buf[256];
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(cfd);
            continue;
        }
        buf[n] = 0;
        for (char *p = buf; *p; ) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            handle_cmd_line(p, cfd);
            if (!nl) break;
            p = nl + 1;
        }
        close(cfd);
    }
}

static void offer_mimes_clear(void) {
    for (size_t i = 0; i < g.offer_mime_count; i++)
        free(g.offer_mimes[i]);
    free(g.offer_mimes);
    g.offer_mimes = NULL;
    g.offer_mime_count = 0;
}

static void offer_mimes_add(const char *mime) {
    if (!mime || !*mime)
        return;
    for (size_t i = 0; i < g.offer_mime_count; i++) {
        if (strcmp(g.offer_mimes[i], mime) == 0)
            return;
    }
    char **n = realloc(g.offer_mimes, (g.offer_mime_count + 1) * sizeof(char *));
    if (!n)
        return;
    g.offer_mimes = n;
    g.offer_mimes[g.offer_mime_count] = strdup(mime);
    if (g.offer_mimes[g.offer_mime_count])
        g.offer_mime_count++;
}

static const char *pick_mime(void) {
    for (int i = 0; PREFERRED_MIMES[i]; i++) {
        for (size_t j = 0; j < g.offer_mime_count; j++) {
            if (strcmp(g.offer_mimes[j], PREFERRED_MIMES[i]) == 0)
                return g.offer_mimes[j];
        }
    }
    return g.offer_mime_count ? g.offer_mimes[0] : NULL;
}

static int receive_offer(struct wl_data_offer *offer, const char *mime,
                         char **out, size_t *out_len, int timeout_ms) {
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    /* Make the read end non-blocking so we can interleave wl_display_dispatch
     * while the source client writes (never roundtrip from inside a listener). */
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0)
        fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);

    wl_data_offer_receive(offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(g.display);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        return -1;
    }

    for (;;) {
        ssize_t n = read(fds[0], buf + len, cap - len);
        if (n > 0) {
            len += (size_t)n;
            if (len > CLIP_MAX_BYTES) {
                free(buf);
                close(fds[0]);
                return -1;
            }
            if (len + 4096 > cap) {
                size_t ncap = cap * 2;
                if (ncap > CLIP_MAX_BYTES + 4096)
                    ncap = CLIP_MAX_BYTES + 4096;
                char *nbuf = realloc(buf, ncap);
                if (!nbuf) {
                    free(buf);
                    close(fds[0]);
                    return -1;
                }
                buf = nbuf;
                cap = ncap;
            }
            continue;
        }
        if (n == 0)
            break; /* writer closed */
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            free(buf);
            close(fds[0]);
            return -1;
        }

        /* Wait for pipe data or Wayland events (source may need dispatch). */
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        wl_display_flush(g.display);

        struct pollfd pfds[2];
        pfds[0].fd = wl_display_get_fd(g.display);
        pfds[0].events = POLLIN;
        pfds[1].fd = fds[0];
        pfds[1].events = POLLIN;
        int pr = poll(pfds, 2, timeout_ms > 0 ? timeout_ms : 400);
        if (pr < 0) {
            wl_display_cancel_read(g.display);
            if (errno == EINTR)
                continue;
            free(buf);
            close(fds[0]);
            return -1;
        }
        if (pr == 0) {
            wl_display_cancel_read(g.display);
            /* Writer never closed; keep a complete payload rather than
             * discarding a successful image/text transfer. */
            if (len > 0)
                break;
            free(buf);
            close(fds[0]);
            if (g.verbose)
                fprintf(stderr, "luna-clipboard: receive timed out\n");
            return -1;
        }
        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(g.display) < 0) {
                free(buf);
                close(fds[0]);
                return -1;
            }
            wl_display_dispatch_pending(g.display);
        } else {
            wl_display_cancel_read(g.display);
        }
    }

    close(fds[0]);
    *out = buf;
    *out_len = len;
    return 0;
}

/* ── data_offer ── */

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
    (void)data;
    (void)offer;
    offer_mimes_add(mime);
}

static void offer_source_actions(void *data, struct wl_data_offer *o, uint32_t a) {
    (void)data;
    (void)o;
    (void)a;
}

static void offer_action(void *data, struct wl_data_offer *o, uint32_t a) {
    (void)data;
    (void)o;
    (void)a;
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

/* ── data_source (we own the seat selection) ── */

static void source_target(void *data, struct wl_data_source *s, const char *mime) {
    (void)data;
    (void)s;
    (void)mime;
}

static void source_send(void *data, struct wl_data_source *s,
                        const char *mime, int32_t fd) {
    (void)data;
    (void)s;
    size_t payload_len = 0;
    const char *payload = clip_payload(&g.current, mime, &payload_len);
    if (payload && payload_len > 0) {
        const char *p = payload;
        size_t left = payload_len;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            p += n;
            left -= (size_t)n;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s) {
    (void)data;
    if (g.source == s) {
        wl_data_source_destroy(s);
        g.source = NULL;
        g.own_selection = false;
        g.expect_self_offer = false;
        if (g.verbose)
            fprintf(stderr, "luna-clipboard: selection cancelled\n");
    }
}

static void source_dnd_drop_performed(void *data, struct wl_data_source *s) {
    (void)data;
    (void)s;
}

static void source_dnd_finished(void *data, struct wl_data_source *s) {
    (void)data;
    (void)s;
}

static void source_action(void *data, struct wl_data_source *s, uint32_t a) {
    (void)data;
    (void)s;
    (void)a;
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

static void become_selection_owner(void) {
    if (!g.ddm || !g.device || !g.current.data)
        return;
    if (g.source) {
        wl_data_source_destroy(g.source);
        g.source = NULL;
    }
    g.source = wl_data_device_manager_create_data_source(g.ddm);
    if (!g.source)
        return;
    wl_data_source_add_listener(g.source, &source_listener, NULL);
    /* Preserve exact representations, and keep plain text interoperable with
     * clients that choose a different alias (also repairs old history rows). */
    for (size_t i = 0; i < g.current.variant_count; i++)
        wl_data_source_offer(g.source, g.current.variants[i].mime);
    bool has_plain_text = false;
    for (size_t i = 0; i < g.current.variant_count; i++)
        if (mime_is_plain_text(g.current.variants[i].mime)) {
            has_plain_text = true;
            break;
        }
    if (has_plain_text) {
        for (int i = 0; PREFERRED_MIMES[i]; i++)
            if (!clip_has_mime(&g.current, PREFERRED_MIMES[i]))
                wl_data_source_offer(g.source, PREFERRED_MIMES[i]);
    }
    g.own_selection = true;
    /* Expect an echo only briefly: luna-compositor skips re-offering to the
     * owner.  Clear after flush so a later real GTK offer is never mistaken
     * for our own echo. */
    g.expect_self_offer = true;
    wl_data_device_set_selection(g.device, g.source, 0);
    wl_display_flush(g.display);
    g.expect_self_offer = false;
    if (g.verbose)
        fprintf(stderr, "luna-clipboard: owning selection (%zu bytes, %s)\n",
                g.current.len, g.current.mime ? g.current.mime : "?");
}

static void take_offer(struct wl_data_offer *offer) {
    const char *preferred = pick_mime();
    if (!preferred) {
        wl_data_offer_destroy(offer);
        offer_mimes_clear();
        return;
    }
    ClipEntry incoming = {0};
    size_t total = 0;
    /* Preferred first (so the history preview appears immediately), then every
     * other advertised type — PNG, HTML, URI lists, GTK buffer formats, … */
    for (size_t pass = 0; pass <= g.offer_mime_count; pass++) {
        const char *mime = pass == 0 ? preferred : g.offer_mimes[pass - 1];
        if (!strcmp(mime, preferred) && pass != 0)
            continue;
        char *data = NULL;
        size_t len = 0;
        if (receive_offer(offer, mime, &data, &len, pass == 0 ? 2000 : 400) == 0 && data && len &&
            len <= CLIP_MAX_BYTES && total + len <= CLIP_MAX_TOTAL_BYTES) {
            if (clip_add(&incoming, mime, data, len, pass == 0))
                total += len;
        }
        free(data);
    }
    wl_data_offer_destroy(offer);
    offer_mimes_clear();

    /* Ignore empty / identical content. */
    if (!incoming.data) {
        clip_free(&incoming);
        return;
    }
    if (clip_equal(&g.current, &incoming)) {
        clip_free(&incoming);
        /* Still re-own so the seat keeps a living source. */
        if (!g.own_selection)
            become_selection_owner();
        return;
    }

    clip_free(&g.current);
    g.current = incoming;
    history_push(&g.current);
    history_save();
    become_selection_owner();
}

/* ── data_device ── */

static void device_data_offer(void *data, struct wl_data_device *dev,
                              struct wl_data_offer *id) {
    (void)data;
    (void)dev;
    offer_mimes_clear();
    if (g.offer) {
        wl_data_offer_destroy(g.offer);
        g.offer = NULL;
    }
    g.offer = id;
    wl_data_offer_add_listener(id, &offer_listener, NULL);
}

static void device_enter(void *d, struct wl_data_device *dev, uint32_t serial,
                         struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *offer) {
    (void)d;
    (void)dev;
    (void)serial;
    (void)surf;
    (void)x;
    (void)y;
    (void)offer;
}

static void device_leave(void *d, struct wl_data_device *dev) {
    (void)d;
    (void)dev;
}

static void device_motion(void *d, struct wl_data_device *dev, uint32_t time,
                          wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)dev;
    (void)time;
    (void)x;
    (void)y;
}

static void device_drop(void *d, struct wl_data_device *dev) {
    (void)d;
    (void)dev;
}

static void device_selection(void *data, struct wl_data_device *dev,
                             struct wl_data_offer *offer) {
    (void)data;
    (void)dev;
    if (!offer) {
        if (g_pending_offer) {
            wl_data_offer_destroy(g_pending_offer);
            g_pending_offer = NULL;
        }
        g.offer = NULL;
        offer_mimes_clear();
        g.own_selection = false;
        g.expect_self_offer = false;
        return;
    }
    /* Echo of our own set_selection — do not re-read (would loop). */
    if (g.expect_self_offer) {
        g.expect_self_offer = false;
        wl_data_offer_destroy(offer);
        if (g.offer == offer)
            g.offer = NULL;
        offer_mimes_clear();
        return;
    }
    /* Defer ingest to the main loop.  Receiving (and blocking on the pipe)
     * from inside this listener prevented the source client from serving
     * data_source.send, so history stayed empty. */
    if (g_pending_offer && g_pending_offer != offer)
        wl_data_offer_destroy(g_pending_offer);
    g.own_selection = false;
    g.offer = NULL;
    g_pending_offer = offer;
    if (g.verbose)
        fprintf(stderr, "luna-clipboard: selection offer queued (%zu mimes)\n",
                g.offer_mime_count);
}

static void drain_pending_offer(void) {
    if (!g_pending_offer)
        return;
    struct wl_data_offer *offer = g_pending_offer;
    g_pending_offer = NULL;
    take_offer(offer);
}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

/* ── registry ── */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, "wl_seat") == 0) {
        uint32_t ver = version < 7 ? version : 7;
        g.seat = wl_registry_bind(reg, name, &wl_seat_interface, ver);
        g.seat_name = name;
    } else if (strcmp(iface, "wl_data_device_manager") == 0) {
        uint32_t ver = version < 3 ? version : 3;
        g.ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface, ver);
        g.ddm_name = name;
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void usage(const char *argv0) {
    fprintf(stderr,
        "luna-clipboard — Luna Desktop clipboard manager\n"
        "usage: %s [-v|--verbose] [--clear-history]\n"
        "\n"
        "Runs in the foreground and owns the Wayland seat selection so\n"
        "clipboard contents survive after the copying app exits.\n"
        "History: $XDG_DATA_HOME/luna-clipboard/history\n"
        "Commands: $XDG_RUNTIME_DIR/luna-clipboard.sock  (list / select N / clear)\n"
        "Opt out from luna-session with LUNA_CLIPBOARD=none\n",
        argv0);
}

int main(int argc, char **argv) {
    int clear_hist = 0;
    g.cmd_fd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            g.verbose = true;
        else if (!strcmp(argv[i], "--clear-history"))
            clear_hist = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (clear_hist) {
        char dir[512], path[576];
        history_path(dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/history", dir);
        unlink(path);
        history_path_legacy_cache(dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/history", dir);
        unlink(path);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int lock_fd = acquire_singleton_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "luna-clipboard: already running\n");
        return 0;
    }

    g.display = wl_display_connect(NULL);
    if (!g.display)
        die("cannot connect to Wayland display (is WAYLAND_DISPLAY set?)");

    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    wl_display_roundtrip(g.display);

    if (!g.seat || !g.ddm)
        die("compositor lacks wl_seat or wl_data_device_manager");

    g.device = wl_data_device_manager_get_data_device(g.ddm, g.seat);
    if (!g.device)
        die("get_data_device failed");
    wl_data_device_add_listener(g.device, &device_listener, NULL);
    wl_display_roundtrip(g.display);

    history_load();
    g.cmd_fd = open_cmd_socket();
    if (g.cmd_fd < 0)
        fprintf(stderr, "luna-clipboard: warning: command socket unavailable\n");
    if (g.current.data)
        become_selection_owner();

    fprintf(stderr, "luna-clipboard: watching seat selection\n");

    while (g_running) {
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        wl_display_flush(g.display);

        struct pollfd pfds[2];
        int np = 1;
        pfds[0].fd = wl_display_get_fd(g.display);
        pfds[0].events = POLLIN;
        if (g.cmd_fd >= 0) {
            pfds[1].fd = g.cmd_fd;
            pfds[1].events = POLLIN;
            np = 2;
        }
        int pr = poll(pfds, np, 1000);
        if (pr < 0) {
            wl_display_cancel_read(g.display);
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            wl_display_cancel_read(g.display);
            drain_pending_offer();
            poll_cmd_socket();
            continue;
        }
        if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(g.display) < 0)
                break;
            if (wl_display_dispatch_pending(g.display) < 0)
                break;
            drain_pending_offer();
        } else {
            wl_display_cancel_read(g.display);
        }
        if (np > 1 && (pfds[1].revents & POLLIN))
            poll_cmd_socket();
    }

    if (g.cmd_fd >= 0) {
        char path[512];
        cmd_sock_path(path, sizeof(path));
        close(g.cmd_fd);
        unlink(path);
    }

    if (g.source)
        wl_data_source_destroy(g.source);
    if (g.device)
        wl_data_device_destroy(g.device);
    if (g.seat)
        wl_seat_destroy(g.seat);
    if (g.ddm)
        wl_data_device_manager_destroy(g.ddm);
    if (g.registry)
        wl_registry_destroy(g.registry);
    wl_display_disconnect(g.display);

    clip_free(&g.current);
    for (int i = 0; i < g.hist_count; i++)
        clip_free(&g.history[i]);
    offer_mimes_clear();
    return 0;
}
