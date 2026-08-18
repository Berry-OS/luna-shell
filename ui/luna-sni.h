/*
 * luna-sni.h — StatusNotifierItem host for the Luna Desktop tray
 *
 * Define LUNA_SNI_IMPLEMENTATION in exactly one translation unit.  D-Bus I/O
 * runs on an internal worker; the UI thread only consumes snapshots and
 * queues Activate / ContextMenu.
 *
 * Wayland has no XEmbed.  Native tray applets register via
 * org.kde.StatusNotifierWatcher (nm-applet, fcitx5, Ayatana, …).
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#ifndef LUNA_SNI_H
#define LUNA_SNI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_SNI_MAX_ITEMS
#define LUNA_SNI_MAX_ITEMS 8
#endif
#ifndef LUNA_SNI_ICON_MAX
#define LUNA_SNI_ICON_MAX 64
#endif

enum {
    LUNA_SNI_KIND_SNI = 1
};

typedef struct LunaSniItem {
    int kind;
    char service[96];
    char path[96];
    char id[64];
    char title[64];
    char tooltip[96];
    char icon_name[96];
    char icon_file[256]; /* theme PNG path, if no pixmap */
    unsigned char rgba[LUNA_SNI_ICON_MAX * LUNA_SNI_ICON_MAX * 4];
    int icon_w;
    int icon_h;
    int has_pixmap;
    unsigned generation;
} LunaSniItem;

typedef struct LunaSniSnapshot {
    LunaSniItem items[LUNA_SNI_MAX_ITEMS];
    int count;
    int available;
    unsigned long long generation;
} LunaSniSnapshot;

typedef void (*LunaSniNotifyFn)(void* user);

typedef struct LunaSniConfig {
    LunaSniNotifyFn notify;
    void* notify_user;
} LunaSniConfig;

int  luna_sni_init(const LunaSniConfig* config);
void luna_sni_shutdown(void);
int  luna_sni_consume(LunaSniSnapshot* out, unsigned long long* last_generation);
int  luna_sni_request_activate(const char* service, const char* path, int x, int y);
int  luna_sni_request_context_menu(const char* service, const char* path, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_SNI_H */

#ifdef LUNA_SNI_IMPLEMENTATION

#include <dbus/dbus.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LUNA_SNI_QUEUE_CAP 16
#define LUNA_SNI_WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define LUNA_SNI_WATCHER_PATH "/StatusNotifierWatcher"
#define LUNA_SNI_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define LUNA_SNI_ITEM_IFACE "org.kde.StatusNotifierItem"

typedef enum LunaSniCommandType {
    LUNA_SNI_CMD_ACTIVATE = 1,
    LUNA_SNI_CMD_MENU
} LunaSniCommandType;

typedef struct LunaSniCommand {
    LunaSniCommandType type;
    char service[96];
    char path[96];
    int x, y;
} LunaSniCommand;

typedef struct LunaSniLive {
    int kind;
    char service[96];
    char path[96];
    char id[64];
    char title[64];
    char tooltip[96];
    char icon_name[96];
    char icon_file[256];
    unsigned char rgba[LUNA_SNI_ICON_MAX * LUNA_SNI_ICON_MAX * 4];
    int icon_w, icon_h;
    int has_pixmap;
    unsigned generation;
} LunaSniLive;

typedef struct LunaSniWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int initialized;
    int running;
    LunaSniNotifyFn notify;
    void* notify_user;
    LunaSniCommand queue[LUNA_SNI_QUEUE_CAP];
    unsigned head, tail, qcount;
    LunaSniSnapshot snapshot;
    LunaSniLive items[LUNA_SNI_MAX_ITEMS];
    int item_count;
    DBusConnection* bus;
} LunaSniWorker;

static LunaSniWorker g_luna_sni;

static void luna_sni_notify(void) {
    LunaSniNotifyFn fn;
    void* user;
    pthread_mutex_lock(&g_luna_sni.mutex);
    fn = g_luna_sni.notify;
    user = g_luna_sni.notify_user;
    pthread_mutex_unlock(&g_luna_sni.mutex);
    if (fn) fn(user);
}

static void luna_sni_copy(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

static int luna_sni_same_item(const LunaSniLive* a, const char* service, const char* path) {
    return a && !strcmp(a->service, service ? service : "") &&
           !strcmp(a->path, path ? path : "");
}

static void luna_sni_publish_locked(void) {
    LunaSniSnapshot* s = &g_luna_sni.snapshot;
    s->count = g_luna_sni.item_count;
    s->available = 1;
    for (int i = 0; i < g_luna_sni.item_count; i++) {
        LunaSniLive* src = &g_luna_sni.items[i];
        LunaSniItem* dst = &s->items[i];
        dst->kind = src->kind;
        luna_sni_copy(dst->service, sizeof(dst->service), src->service);
        luna_sni_copy(dst->path, sizeof(dst->path), src->path);
        luna_sni_copy(dst->id, sizeof(dst->id), src->id[0] ? src->id : src->title);
        luna_sni_copy(dst->title, sizeof(dst->title), src->title);
        luna_sni_copy(dst->tooltip, sizeof(dst->tooltip), src->tooltip);
        luna_sni_copy(dst->icon_name, sizeof(dst->icon_name), src->icon_name);
        luna_sni_copy(dst->icon_file, sizeof(dst->icon_file), src->icon_file);
        dst->icon_w = src->icon_w;
        dst->icon_h = src->icon_h;
        dst->has_pixmap = src->has_pixmap;
        dst->generation = src->generation;
        if (src->has_pixmap && src->icon_w > 0 && src->icon_h > 0) {
            size_t bytes = (size_t)src->icon_w * (size_t)src->icon_h * 4u;
            if (bytes > sizeof(dst->rgba)) bytes = sizeof(dst->rgba);
            memcpy(dst->rgba, src->rgba, bytes);
        }
    }
    s->generation++;
}

static void luna_sni_publish(void) {
    pthread_mutex_lock(&g_luna_sni.mutex);
    luna_sni_publish_locked();
    pthread_mutex_unlock(&g_luna_sni.mutex);
    luna_sni_notify();
}

static int luna_sni_find(const char* service, const char* path) {
    for (int i = 0; i < g_luna_sni.item_count; i++)
        if (luna_sni_same_item(&g_luna_sni.items[i], service, path)) return i;
    return -1;
}

static void luna_sni_remove_at(int idx) {
    if (idx < 0 || idx >= g_luna_sni.item_count) return;
    memmove(&g_luna_sni.items[idx], &g_luna_sni.items[idx + 1],
            (size_t)(g_luna_sni.item_count - idx - 1) * sizeof(g_luna_sni.items[0]));
    g_luna_sni.item_count--;
}

static int luna_sni_icon_lookup(const char* name, char* out, size_t n) {
    static const char* dirs[] = {
        "/usr/share/icons/hicolor/24x24/apps",
        "/usr/share/icons/hicolor/22x22/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/icons/hicolor/24x24/status",
        "/usr/share/icons/hicolor/22x22/status",
        "/usr/share/icons/hicolor/16x16/apps",
        "/usr/share/pixmaps",
        "/usr/share/icons/hicolor/48x48/apps",
        NULL
    };
    if (!name || !*name || !out || n == 0) return 0;
    const char* base = name;
    if (name[0] == '/') {
        if (access(name, R_OK) == 0) {
            snprintf(out, n, "%s", name);
            return 1;
        }
        return 0;
    }
    for (int i = 0; dirs[i]; i++) {
        static const char* ext[] = { "png", "xpm", NULL };
        for (int e = 0; ext[e]; e++) {
            snprintf(out, n, "%s/%s.%s", dirs[i], base, ext[e]);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    out[0] = 0;
    return 0;
}

static void luna_sni_pick_pixmap(DBusMessageIter* array, LunaSniLive* item) {
    int best_w = 0, best_h = 0;
    const unsigned char* best = NULL;
    int best_len = 0;
    if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY) return;
    DBusMessageIter rec;
    dbus_message_iter_recurse(array, &rec);
    while (dbus_message_iter_get_arg_type(&rec) == DBUS_TYPE_STRUCT) {
        DBusMessageIter st;
        dbus_message_iter_recurse(&rec, &st);
        dbus_int32_t w = 0, h = 0;
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32) { dbus_message_iter_next(&rec); continue; }
        dbus_message_iter_get_basic(&st, &w);
        dbus_message_iter_next(&st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32) { dbus_message_iter_next(&rec); continue; }
        dbus_message_iter_get_basic(&st, &h);
        dbus_message_iter_next(&st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY) { dbus_message_iter_next(&rec); continue; }
        DBusMessageIter bytes;
        dbus_message_iter_recurse(&st, &bytes);
        int len = 0;
        const unsigned char* data = NULL;
        dbus_message_iter_get_fixed_array(&bytes, &data, &len);
        int score = w * h;
        int best_score = best_w * best_h;
        int want = 24 * 24;
        int better = 0;
        if (!best) better = 1;
        else if (score >= want && (best_score < want || score < best_score)) better = 1;
        else if (score < want && score > best_score) better = 1;
        if (better && data && w > 0 && h > 0 && len >= w * h * 4) {
            best_w = w; best_h = h; best = data; best_len = len;
        }
        dbus_message_iter_next(&rec);
    }
    if (!best || best_w <= 0 || best_h <= 0) return;
    int dw = best_w > LUNA_SNI_ICON_MAX ? LUNA_SNI_ICON_MAX : best_w;
    int dh = best_h > LUNA_SNI_ICON_MAX ? LUNA_SNI_ICON_MAX : best_h;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int si = (y * best_w + x) * 4;
            int di = (y * dw + x) * 4;
            if (si + 3 >= best_len) break;
            /* Network-byte-order ARGB32: A R G B */
            item->rgba[di + 0] = best[si + 1];
            item->rgba[di + 1] = best[si + 2];
            item->rgba[di + 2] = best[si + 3];
            item->rgba[di + 3] = best[si + 0];
        }
    }
    item->icon_w = dw;
    item->icon_h = dh;
    item->has_pixmap = 1;
    item->generation++;
}

static int luna_sni_prop_string(DBusMessageIter* val, char* out, size_t n) {
    if (dbus_message_iter_get_arg_type(val) != DBUS_TYPE_STRING) return 0;
    const char* s = NULL;
    dbus_message_iter_get_basic(val, &s);
    luna_sni_copy(out, n, s);
    return 1;
}

static void luna_sni_read_tooltip(DBusMessageIter* val, LunaSniLive* item) {
    /* (sa(iiay)ss)  icon-name, pixmap, title, text */
    if (dbus_message_iter_get_arg_type(val) != DBUS_TYPE_STRUCT) {
        luna_sni_prop_string(val, item->tooltip, sizeof(item->tooltip));
        return;
    }
    DBusMessageIter st;
    dbus_message_iter_recurse(val, &st);
    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
        const char* s = NULL;
        dbus_message_iter_get_basic(&st, &s);
        if (s && *s && !item->icon_name[0]) luna_sni_copy(item->icon_name, sizeof(item->icon_name), s);
    }
    dbus_message_iter_next(&st); /* pixmap */
    dbus_message_iter_next(&st);
    char title[64] = "", text[96] = "";
    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
        const char* s = NULL; dbus_message_iter_get_basic(&st, &s);
        luna_sni_copy(title, sizeof(title), s);
    }
    dbus_message_iter_next(&st);
    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
        const char* s = NULL; dbus_message_iter_get_basic(&st, &s);
        luna_sni_copy(text, sizeof(text), s);
    }
    if (text[0]) luna_sni_copy(item->tooltip, sizeof(item->tooltip), text);
    else if (title[0]) luna_sni_copy(item->tooltip, sizeof(item->tooltip), title);
}

static void luna_sni_apply_prop(LunaSniLive* item, const char* key, DBusMessageIter* val) {
    if (!strcmp(key, "Id")) luna_sni_prop_string(val, item->id, sizeof(item->id));
    else if (!strcmp(key, "Title") || !strcmp(key, "Category")) {
        if (!item->title[0] || !strcmp(key, "Title"))
            luna_sni_prop_string(val, item->title, sizeof(item->title));
    } else if (!strcmp(key, "IconName"))
        luna_sni_prop_string(val, item->icon_name, sizeof(item->icon_name));
    else if (!strcmp(key, "AttentionIconName")) {
        /* Keep as fallback; pixmap path prefers attention when present. */
        if (!item->icon_name[0]) luna_sni_prop_string(val, item->icon_name, sizeof(item->icon_name));
    } else if (!strcmp(key, "IconThemePath")) {
        /* prepend later via icon_file if IconName is relative — handled after GetAll */
        (void)val;
    } else if (!strcmp(key, "IconPixmap") || !strcmp(key, "AttentionIconPixmap")) {
        if (!item->has_pixmap || !strcmp(key, "IconPixmap"))
            luna_sni_pick_pixmap(val, item);
    } else if (!strcmp(key, "ToolTip"))
        luna_sni_read_tooltip(val, item);
}

static void luna_sni_refresh_item(LunaSniLive* item) {
    if (!g_luna_sni.bus || item->kind != LUNA_SNI_KIND_SNI) return;
    DBusMessage* msg = dbus_message_new_method_call(
        item->service, item->path, "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return;
    const char* iface = LUNA_SNI_ITEM_IFACE;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(g_luna_sni.bus, msg, 800, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return;
    }
    DBusMessageIter root;
    if (dbus_message_iter_init(reply, &root) &&
        dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        DBusMessageIter dict;
        dbus_message_iter_recurse(&root, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent, var;
            dbus_message_iter_recurse(&dict, &ent);
            const char* key = NULL;
            if (dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&ent, &key);
            dbus_message_iter_next(&ent);
            if (key && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&ent, &var);
                luna_sni_apply_prop(item, key, &var);
            }
            dbus_message_iter_next(&dict);
        }
    }
    dbus_message_unref(reply);
    if (!item->has_pixmap && item->icon_name[0])
        luna_sni_icon_lookup(item->icon_name, item->icon_file, sizeof(item->icon_file));
    if (!item->title[0]) luna_sni_copy(item->title, sizeof(item->title),
                                       item->id[0] ? item->id : "Tray");
    if (!item->tooltip[0]) luna_sni_copy(item->tooltip, sizeof(item->tooltip), item->title);
}

static void luna_sni_emit_registered(const char* key) {
    if (!g_luna_sni.bus || !key) return;
    DBusMessage* sig = dbus_message_new_signal(
        LUNA_SNI_WATCHER_PATH, LUNA_SNI_WATCHER_IFACE, "StatusNotifierItemRegistered");
    if (!sig) return;
    dbus_message_append_args(sig, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
    dbus_connection_send(g_luna_sni.bus, sig, NULL);
    dbus_message_unref(sig);
}

static void luna_sni_emit_unregistered(const char* key) {
    if (!g_luna_sni.bus || !key) return;
    DBusMessage* sig = dbus_message_new_signal(
        LUNA_SNI_WATCHER_PATH, LUNA_SNI_WATCHER_IFACE, "StatusNotifierItemUnregistered");
    if (!sig) return;
    dbus_message_append_args(sig, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
    dbus_connection_send(g_luna_sni.bus, sig, NULL);
    dbus_message_unref(sig);
}

static void luna_sni_item_key(const LunaSniLive* item, char* out, size_t n) {
    if (item->path[0] && strcmp(item->path, "/StatusNotifierItem") != 0)
        snprintf(out, n, "%s%s", item->service, item->path);
    else
        snprintf(out, n, "%s", item->service);
}

static void luna_sni_add_item(const char* service, const char* path) {
    if (!service || !*service || !path || !*path) return;
    int idx = luna_sni_find(service, path);
    if (idx < 0) {
        if (g_luna_sni.item_count >= LUNA_SNI_MAX_ITEMS) return;
        idx = g_luna_sni.item_count++;
        memset(&g_luna_sni.items[idx], 0, sizeof(g_luna_sni.items[idx]));
        g_luna_sni.items[idx].kind = LUNA_SNI_KIND_SNI;
        luna_sni_copy(g_luna_sni.items[idx].service, sizeof(g_luna_sni.items[idx].service), service);
        luna_sni_copy(g_luna_sni.items[idx].path, sizeof(g_luna_sni.items[idx].path), path);
    }
    luna_sni_refresh_item(&g_luna_sni.items[idx]);
    char key[192];
    luna_sni_item_key(&g_luna_sni.items[idx], key, sizeof(key));
    luna_sni_emit_registered(key);
    luna_sni_publish();
}

static void luna_sni_parse_register(const char* sender, const char* arg,
                                    char* service, size_t sn, char* path, size_t pn) {
    service[0] = 0; path[0] = 0;
    if (!arg || !*arg) return;
    if (arg[0] == '/') {
        luna_sni_copy(service, sn, sender);
        luna_sni_copy(path, pn, arg);
        return;
    }
    const char* slash = strchr(arg, '/');
    if (slash && arg[0] == ':') {
        size_t slen = (size_t)(slash - arg);
        if (slen >= sn) slen = sn - 1;
        memcpy(service, arg, slen);
        service[slen] = 0;
        luna_sni_copy(path, pn, slash);
        return;
    }
    luna_sni_copy(service, sn, arg);
    luna_sni_copy(path, pn, "/StatusNotifierItem");
}

static void luna_sni_append_items_array(DBusMessageIter* iter) {
    DBusMessageIter arr;
    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &arr);
    for (int i = 0; i < g_luna_sni.item_count; i++) {
        if (g_luna_sni.items[i].kind != LUNA_SNI_KIND_SNI) continue;
        char key[192];
        luna_sni_item_key(&g_luna_sni.items[i], key, sizeof(key));
        const char* p = key;
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &p);
    }
    dbus_message_iter_close_container(iter, &arr);
}

static void luna_sni_append_prop(DBusMessageIter* dict, const char* name, int type, const void* value) {
    DBusMessageIter ent, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
    dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &name);
    char sig[2] = { (char)type, 0 };
    dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, value);
    dbus_message_iter_close_container(&ent, &var);
    dbus_message_iter_close_container(dict, &ent);
}

static DBusHandlerResult luna_sni_filter(DBusConnection* conn, DBusMessage* msg, void* data) {
    (void)data;
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect") &&
        dbus_message_has_path(msg, LUNA_SNI_WATCHER_PATH)) {
        const char* xml =
            "<node><interface name=\"org.kde.StatusNotifierWatcher\">"
            "<method name=\"RegisterStatusNotifierItem\"><arg type=\"s\" name=\"service\" direction=\"in\"/></method>"
            "<method name=\"RegisterStatusNotifierHost\"><arg type=\"s\" name=\"service\" direction=\"in\"/></method>"
            "<property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>"
            "<property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>"
            "<property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>"
            "<signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>"
            "<signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>"
            "<signal name=\"StatusNotifierHostRegistered\"/>"
            "</interface></node>";
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, LUNA_SNI_WATCHER_IFACE, "RegisterStatusNotifierItem")) {
        const char* arg = "";
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
        char service[96], path[96];
        luna_sni_parse_register(dbus_message_get_sender(msg), arg, service, sizeof(service), path, sizeof(path));
        luna_sni_add_item(service, path);
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, LUNA_SNI_WATCHER_IFACE, "RegisterStatusNotifierHost")) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get") &&
        dbus_message_has_path(msg, LUNA_SNI_WATCHER_PATH)) {
        const char* iface = NULL, *name = NULL;
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter it, var;
        dbus_message_iter_init_append(reply, &it);
        if (name && !strcmp(name, "IsStatusNotifierHostRegistered")) {
            dbus_bool_t v = TRUE;
            dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
            dbus_message_iter_close_container(&it, &var);
        } else if (name && !strcmp(name, "ProtocolVersion")) {
            dbus_int32_t v = 0;
            dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "i", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v);
            dbus_message_iter_close_container(&it, &var);
        } else {
            dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "as", &var);
            luna_sni_append_items_array(&var);
            dbus_message_iter_close_container(&it, &var);
        }
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll") &&
        dbus_message_has_path(msg, LUNA_SNI_WATCHER_PATH)) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        DBusMessageIter it, dict, ent, var;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_bool_t host = TRUE;
        dbus_int32_t ver = 0;
        luna_sni_append_prop(&dict, "IsStatusNotifierHostRegistered", DBUS_TYPE_BOOLEAN, &host);
        luna_sni_append_prop(&dict, "ProtocolVersion", DBUS_TYPE_INT32, &ver);
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
        {
            const char* n = "RegisteredStatusNotifierItems";
            dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &n);
        }
        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "as", &var);
        luna_sni_append_items_array(&var);
        dbus_message_iter_close_container(&ent, &var);
        dbus_message_iter_close_container(&dict, &ent);
        dbus_message_iter_close_container(&it, &dict);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char* name = NULL, *old = NULL, *nw = NULL;
        if (dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &old,
                                  DBUS_TYPE_STRING, &nw,
                                  DBUS_TYPE_INVALID) &&
            name && (!nw || !*nw)) {
            int changed = 0;
            for (int i = g_luna_sni.item_count - 1; i >= 0; i--) {
                if (g_luna_sni.items[i].kind != LUNA_SNI_KIND_SNI) continue;
                if (strcmp(g_luna_sni.items[i].service, name) != 0) continue;
                char key[192];
                luna_sni_item_key(&g_luna_sni.items[i], key, sizeof(key));
                luna_sni_emit_unregistered(key);
                luna_sni_remove_at(i);
                changed = 1;
            }
            if (changed) luna_sni_publish();
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_has_interface(msg, LUNA_SNI_ITEM_IFACE) &&
        dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
        const char* sender = dbus_message_get_sender(msg);
        const char* path = dbus_message_get_path(msg);
        int idx = -1;
        if (sender && path) idx = luna_sni_find(sender, path);
        if (idx < 0 && path) {
            for (int i = 0; i < g_luna_sni.item_count; i++)
                if (g_luna_sni.items[i].kind == LUNA_SNI_KIND_SNI &&
                    !strcmp(g_luna_sni.items[i].path, path)) { idx = i; break; }
        }
        if (idx >= 0) {
            luna_sni_refresh_item(&g_luna_sni.items[idx]);
            luna_sni_publish();
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void luna_sni_call_item(const LunaSniCommand* cmd) {
    if (!g_luna_sni.bus || !cmd->service[0] || !cmd->path[0]) return;
    const char* member = cmd->type == LUNA_SNI_CMD_MENU ? "ContextMenu" : "Activate";
    DBusMessage* msg = dbus_message_new_method_call(cmd->service, cmd->path, LUNA_SNI_ITEM_IFACE, member);
    if (!msg) return;
    dbus_int32_t x = cmd->x, y = cmd->y;
    dbus_message_append_args(msg, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
    dbus_connection_send(g_luna_sni.bus, msg, NULL);
    dbus_connection_flush(g_luna_sni.bus);
    dbus_message_unref(msg);
}

static void* luna_sni_thread(void* arg) {
    (void)arg;
    DBusError err;
    dbus_error_init(&err);
    g_luna_sni.bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_luna_sni.bus) {
        fprintf(stderr, "[luna-sni] session bus: %s\n", err.message);
        dbus_error_free(&err);
        pthread_mutex_lock(&g_luna_sni.mutex);
        g_luna_sni.snapshot.available = 0;
        g_luna_sni.snapshot.generation++;
        pthread_mutex_unlock(&g_luna_sni.mutex);
        luna_sni_notify();
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(g_luna_sni.bus, FALSE);
    int req = dbus_bus_request_name(g_luna_sni.bus, LUNA_SNI_WATCHER_NAME,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (req != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "[luna-sni] StatusNotifierWatcher already owned\n");
        dbus_error_free(&err);
    } else {
        dbus_connection_add_filter(g_luna_sni.bus, luna_sni_filter, NULL, NULL);
        dbus_bus_add_match(g_luna_sni.bus,
            "type='method_call',interface='org.kde.StatusNotifierWatcher'", &err);
        dbus_bus_add_match(g_luna_sni.bus,
            "type='method_call',interface='org.freedesktop.DBus.Properties',path='/StatusNotifierWatcher'", &err);
        dbus_bus_add_match(g_luna_sni.bus,
            "type='method_call',interface='org.freedesktop.DBus.Introspectable',path='/StatusNotifierWatcher'", &err);
        dbus_bus_add_match(g_luna_sni.bus,
            "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", &err);
        dbus_bus_add_match(g_luna_sni.bus,
            "type='signal',interface='org.kde.StatusNotifierItem'", &err);
        dbus_error_free(&err);
        DBusMessage* host_sig = dbus_message_new_signal(
            LUNA_SNI_WATCHER_PATH, LUNA_SNI_WATCHER_IFACE, "StatusNotifierHostRegistered");
        if (host_sig) {
            dbus_connection_send(g_luna_sni.bus, host_sig, NULL);
            dbus_message_unref(host_sig);
        }
        pthread_mutex_lock(&g_luna_sni.mutex);
        g_luna_sni.snapshot.available = 1;
        g_luna_sni.snapshot.generation++;
        pthread_mutex_unlock(&g_luna_sni.mutex);
        luna_sni_notify();
        fprintf(stderr, "[luna-sni] watching StatusNotifierItem on session bus\n");
    }

    while (g_luna_sni.running) {
        LunaSniCommand cmd;
        int have_cmd = 0;
        pthread_mutex_lock(&g_luna_sni.mutex);
        if (g_luna_sni.qcount) {
            cmd = g_luna_sni.queue[g_luna_sni.head];
            g_luna_sni.head = (g_luna_sni.head + 1) % LUNA_SNI_QUEUE_CAP;
            g_luna_sni.qcount--;
            have_cmd = 1;
        }
        pthread_mutex_unlock(&g_luna_sni.mutex);
        if (have_cmd)
            luna_sni_call_item(&cmd);

        dbus_connection_read_write_dispatch(g_luna_sni.bus, 50);
    }
    if (g_luna_sni.bus) {
        dbus_connection_remove_filter(g_luna_sni.bus, luna_sni_filter, NULL);
        dbus_connection_unref(g_luna_sni.bus);
        g_luna_sni.bus = NULL;
    }
    return NULL;
}

static int luna_sni_queue(LunaSniCommandType type, const char* service, const char* path, int x, int y) {
    if (!g_luna_sni.initialized || !service || !path) return 0;
    pthread_mutex_lock(&g_luna_sni.mutex);
    if (g_luna_sni.qcount >= LUNA_SNI_QUEUE_CAP) {
        pthread_mutex_unlock(&g_luna_sni.mutex);
        return 0;
    }
    LunaSniCommand* c = &g_luna_sni.queue[(g_luna_sni.head + g_luna_sni.qcount) % LUNA_SNI_QUEUE_CAP];
    c->type = type;
    luna_sni_copy(c->service, sizeof(c->service), service);
    luna_sni_copy(c->path, sizeof(c->path), path);
    c->x = x;
    c->y = y;
    g_luna_sni.qcount++;
    pthread_mutex_unlock(&g_luna_sni.mutex);
    return 1;
}

int luna_sni_init(const LunaSniConfig* config) {
    if (g_luna_sni.initialized) return 1;
    memset(&g_luna_sni, 0, sizeof(g_luna_sni));
    pthread_mutex_init(&g_luna_sni.mutex, NULL);
    pthread_cond_init(&g_luna_sni.cond, NULL);
    g_luna_sni.running = 1;
    g_luna_sni.notify = config ? config->notify : NULL;
    g_luna_sni.notify_user = config ? config->notify_user : NULL;
    if (pthread_create(&g_luna_sni.thread, NULL, luna_sni_thread, NULL) != 0) {
        pthread_mutex_destroy(&g_luna_sni.mutex);
        pthread_cond_destroy(&g_luna_sni.cond);
        return 0;
    }
    g_luna_sni.initialized = 1;
    return 1;
}

void luna_sni_shutdown(void) {
    if (!g_luna_sni.initialized) return;
    g_luna_sni.running = 0;
    pthread_join(g_luna_sni.thread, NULL);
    pthread_mutex_destroy(&g_luna_sni.mutex);
    pthread_cond_destroy(&g_luna_sni.cond);
    memset(&g_luna_sni, 0, sizeof(g_luna_sni));
}

int luna_sni_consume(LunaSniSnapshot* out, unsigned long long* last_generation) {
    if (!out || !g_luna_sni.initialized) return 0;
    pthread_mutex_lock(&g_luna_sni.mutex);
    unsigned long long gen = g_luna_sni.snapshot.generation;
    if (last_generation && *last_generation == gen) {
        pthread_mutex_unlock(&g_luna_sni.mutex);
        return 0;
    }
    *out = g_luna_sni.snapshot;
    pthread_mutex_unlock(&g_luna_sni.mutex);
    if (last_generation) *last_generation = gen;
    return 1;
}

int luna_sni_request_activate(const char* service, const char* path, int x, int y) {
    return luna_sni_queue(LUNA_SNI_CMD_ACTIVATE, service, path, x, y);
}

int luna_sni_request_context_menu(const char* service, const char* path, int x, int y) {
    return luna_sni_queue(LUNA_SNI_CMD_MENU, service, path, x, y);
}

#endif /* LUNA_SNI_IMPLEMENTATION */
