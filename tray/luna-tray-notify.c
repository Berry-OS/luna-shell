/*
 * luna-tray-notify — small XEmbed tray resident notification service.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 *
 * It intentionally uses only Xlib.  XEmbed is the protocol used by lxpanel
 * and other traditional X11 panels, unlike StatusNotifierItem/DBus which is
 * not enabled in every lightweight desktop.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <dbus/dbus.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ICON_SIZE 24
#define POPUP_WIDTH 340
#define POPUP_HEIGHT 108
#define XEMBED_EMBEDDED_NOTIFY 0
#define SYSTEM_TRAY_REQUEST_DOCK 0

static volatile sig_atomic_t keep_running = 1;

typedef struct {
    Display *dpy;
    Window icon, popup;
    int screen;
    Atom utf8, net_wm_name;
    char title[128], message[256];
    long long popup_until;
    unsigned next_id;
} Notifier;

static Notifier *g_notifier;

static void stop(int sig) { (void)sig; keep_running = 0; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void copy_text(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    snprintf(dst, n, "%s", src);
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t') dst[i] = ' ';
}

static void draw_icon(Display *dpy, Window icon) {
    GC gc = XCreateGC(dpy, icon, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, icon, gc, 0, 0, ICON_SIZE, ICON_SIZE);
    XSetForeground(dpy, gc, 0x2b6cb0);
    XFillArc(dpy, icon, gc, 6, 5, 12, 13, 0, 360 * 64);
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillArc(dpy, icon, gc, 9, 8, 6, 6, 0, 360 * 64);
    XSetForeground(dpy, gc, 0x1a4971);
    XFillRectangle(dpy, icon, gc, 9, 19, 6, 2);
    XFreeGC(dpy, gc);
}

static void draw_popup(Display *dpy, Window popup, const char *title,
                       const char *message) {
    GC gc = XCreateGC(dpy, popup, 0, NULL);
    XSetForeground(dpy, gc, 0x1e293b);
    XFillRectangle(dpy, popup, gc, 0, 0, POPUP_WIDTH, POPUP_HEIGHT);
    XSetForeground(dpy, gc, 0x60a5fa);
    XFillRectangle(dpy, popup, gc, 0, 0, 5, POPUP_HEIGHT);
    XSetForeground(dpy, gc, 0xf8fafc);
    XDrawString(dpy, popup, gc, 20, 30, title, (int)strlen(title));
    XSetForeground(dpy, gc, 0xcbd5e1);
    /* Xlib core fonts do not wrap text; split a long message into two rows. */
    char first[96], second[96];
    snprintf(first, sizeof(first), "%.72s", message);
    snprintf(second, sizeof(second), "%s", strlen(message) > 72 ? message + 72 : "");
    XDrawString(dpy, popup, gc, 20, 59, first, (int)strlen(first));
    if (*second) XDrawString(dpy, popup, gc, 20, 79, second, (int)strlen(second));
    XFreeGC(dpy, gc);
}

static void set_tooltip(Display *dpy, Window icon, Atom utf8, Atom net_wm_name,
                        const char *title, const char *message) {
    char text[384];
    snprintf(text, sizeof(text), "%s: %s", title, message);
    XChangeProperty(dpy, icon, net_wm_name, utf8, 8, PropModeReplace,
                    (const unsigned char *)text, (int)strlen(text));
}

static void dock(Display *dpy, Window icon, Atom tray_selection, Atom tray_opcode) {
    Window manager = XGetSelectionOwner(dpy, tray_selection);
    if (!manager) return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = manager;
    ev.xclient.message_type = tray_opcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
    ev.xclient.data.l[2] = icon;
    XSendEvent(dpy, manager, False, NoEventMask, &ev);
    XFlush(dpy);
}

static void show_notification(Notifier *n, const char *title, const char *message) {
    copy_text(n->title, sizeof(n->title), title);
    copy_text(n->message, sizeof(n->message), message);
    set_tooltip(n->dpy, n->icon, n->utf8, n->net_wm_name, n->title, n->message);
    XMoveWindow(n->dpy, n->popup, DisplayWidth(n->dpy, n->screen) - POPUP_WIDTH - 20, 32);
    XMapRaised(n->dpy, n->popup);
    draw_popup(n->dpy, n->popup, n->title, n->message);
    n->popup_until = now_ms() + 5000;
}

static DBusHandlerResult notification_dbus_handler(DBusConnection *connection,
                                                    DBusMessage *message, void *data) {
    (void)data;
    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "Notify")) {
        DBusMessageIter it;
        const char *summary = "Notification", *body = "";
        if (dbus_message_iter_init(message, &it)) {
            /* app_name, replaces_id, app_icon, summary, body, actions, hints, timeout */
            for (int field = 0; dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID; field++) {
                int type = dbus_message_iter_get_arg_type(&it);
                if ((field == 3 || field == 4) && type == DBUS_TYPE_STRING) {
                    const char *s; dbus_message_iter_get_basic(&it, &s);
                    if (field == 3) summary = s; else body = s;
                }
                dbus_message_iter_next(&it);
            }
        }
        if (g_notifier) show_notification(g_notifier, summary, body);
        DBusMessage *reply = dbus_message_new_method_return(message);
        dbus_uint32_t id = g_notifier ? ++g_notifier->next_id : 1;
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, NULL); dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "GetCapabilities")) {
        DBusMessage *reply = dbus_message_new_method_return(message);
        DBusMessageIter out, array; dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "s", &array);
        const char *cap = "body"; dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &cap);
        dbus_message_iter_close_container(&out, &array);
        dbus_connection_send(connection, reply, NULL); dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "GetServerInformation")) {
        DBusMessage *reply = dbus_message_new_method_return(message);
        const char *name = "Luna Tray Notifier", *vendor = "Project Vespera", *version = "1.0", *spec = "1.2";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor,
                                 DBUS_TYPE_STRING, &version, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, NULL); dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "CloseNotification")) {
        DBusMessage *reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, NULL); dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int send_notification(const char *title, const char *message) {
    DBusError error; dbus_error_init(&error);
    DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!bus) { fprintf(stderr, "luna-tray-notify: %s\n", error.message); dbus_error_free(&error); return 1; }
    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
    const char *app = "luna-tray-notify", *icon = "";
    dbus_uint32_t replaces = 0; dbus_int32_t timeout = -1;
    DBusMessageIter it, array, dict;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &message);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &array);
    dbus_message_iter_close_container(&it, &array);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);
    dbus_connection_send(bus, msg, NULL); dbus_connection_flush(bus);
    dbus_message_unref(msg);
    return 0;
}

static void usage(FILE *out) {
    fprintf(out, "Usage: luna-tray-notify [--title TITLE --message MESSAGE]\n"
                 "       luna-tray-notify --notify TITLE MESSAGE\n\n"
                 "Provides org.freedesktop.Notifications on the session D-Bus and places an\n"
                 "XEmbed icon in LXDE-compatible system trays.\n");
}

int main(int argc, char **argv) {
    const char *title = "Luna notifications", *message = "Notifier is ready";
    int notify = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
        if ((!strcmp(argv[i], "--title") || !strcmp(argv[i], "-t")) && i + 1 < argc) {
            title = argv[++i];
        } else if ((!strcmp(argv[i], "--message") || !strcmp(argv[i], "-m")) && i + 1 < argc) {
            message = argv[++i];
        } else if (!strcmp(argv[i], "--notify") && i + 2 < argc) {
            notify = 1; title = argv[++i]; message = argv[++i];
        } else { usage(stderr); return 2; }
    }
    if (notify) return send_notification(title, message);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "luna-tray-notify: cannot open DISPLAY\n"); return 1; }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Atom xembed = XInternAtom(dpy, "_XEMBED", False);
    Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    Atom tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    Atom manager_atom = XInternAtom(dpy, "MANAGER", False);
    Atom tray_selection = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Window icon = XCreateSimpleWindow(dpy, root, 0, 0, ICON_SIZE, ICON_SIZE, 0,
                                      BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, icon, ExposureMask | ButtonPressMask | StructureNotifyMask);
    long xembed_data[2] = { 0, 1 };
    XChangeProperty(dpy, icon, xembed_info, xembed_info, 32, PropModeReplace,
                    (unsigned char *)xembed_data, 2);
    XStoreName(dpy, icon, "Luna notifications");
    XMapRaised(dpy, icon);
    XSelectInput(dpy, root, StructureNotifyMask);
    dock(dpy, icon, tray_selection, tray_opcode);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0x1e293b;
    Window popup = XCreateWindow(dpy, root, 0, 32, POPUP_WIDTH, POPUP_HEIGHT, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &attrs);
    XSelectInput(dpy, popup, ExposureMask | ButtonPressMask);
    XStoreName(dpy, popup, "Luna notification");

    DBusError error; dbus_error_init(&error);
    DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!bus) { fprintf(stderr, "luna-tray-notify: %s\n", error.message); dbus_error_free(&error); return 1; }
    int request = dbus_bus_request_name(bus, "org.freedesktop.Notifications", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "luna-tray-notify: another notification daemon owns org.freedesktop.Notifications\n");
        return 1;
    }
    Notifier notifier = { .dpy = dpy, .icon = icon, .popup = popup, .screen = screen,
                          .utf8 = utf8, .net_wm_name = net_wm_name, .next_id = 0 };
    g_notifier = &notifier;
    dbus_connection_add_filter(bus, notification_dbus_handler, NULL, NULL);
    dbus_bus_add_match(bus, "type='method_call',interface='org.freedesktop.Notifications'", &error);
    show_notification(&notifier, title, message);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    XFlush(dpy);

    while (keep_running) {
        int dbus_fd = -1; dbus_connection_get_unix_fd(bus, &dbus_fd);
        struct pollfd fds[2] = {{ ConnectionNumber(dpy), POLLIN, 0 }, { dbus_fd, POLLIN, 0 }};
        int timeout = notifier.popup_until ? (int)(notifier.popup_until - now_ms()) : -1;
        if (timeout < 0) timeout = 0;
        poll(fds, 2, timeout);
        if (notifier.popup_until && now_ms() >= notifier.popup_until) { XUnmapWindow(dpy, popup); notifier.popup_until = 0; }
        /* libdbus may already have buffered a complete message when its
         * socket watch is not marked POLLIN.  A zero-time dispatch keeps
         * synchronous clients such as dbus-send from waiting for a timeout. */
        dbus_connection_read_write_dispatch(bus, 0);
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                if (ev.xexpose.window == icon) draw_icon(dpy, icon);
                if (ev.xexpose.window == popup) draw_popup(dpy, popup, notifier.title, notifier.message);
            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.window == icon) { XMapRaised(dpy, popup); draw_popup(dpy, popup, notifier.title, notifier.message); notifier.popup_until = now_ms() + 5000; }
                else if (ev.xbutton.window == popup) { XUnmapWindow(dpy, popup); notifier.popup_until = 0; }
            } else if (ev.type == ClientMessage && ev.xclient.message_type == manager_atom) {
                dock(dpy, icon, tray_selection, tray_opcode);
            } else if (ev.type == ClientMessage && ev.xclient.message_type == xembed &&
                       ev.xclient.data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
                draw_icon(dpy, icon);
            }
        }
        XFlush(dpy);
    }
    dbus_connection_unref(bus);
    XDestroyWindow(dpy, popup); XDestroyWindow(dpy, icon); XCloseDisplay(dpy);
    return 0;
}
