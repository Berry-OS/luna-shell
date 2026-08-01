/*
 * luna-shell — Luna Desktop shell (macOS-style desktop environment)
 *
 * Runs fullscreen on top of luna-compositor (Wayland) right after kernel
 * boot — no Xorg, no Weston. Renders the whole desktop (menu bar, dock,
 * launchpad, widgets) with the Luna UI HTML/CSS engine and launches GTK
 * apps that connect to the same compositor.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */

#define LUNA_UI_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/mman.h>

/* luna-ui.h is host-neutral: it takes GL entry points and time/cursor/close
 * callbacks through LunaPlatform and never requires GLFW. We include a real
 * desktop-GL header only for the GL types and enum constants — every gl* call
 * (including the GL 1.0/1.1 core ones) is routed through luna-ui.h's function
 * pointers, resolved via get_proc()/eglGetProcAddress(), so they all share the
 * one dispatch table eglMakeCurrent() populated instead of the libGL symbols'
 * separate GLX current-context slot (see luna-ui.h). Both backends below
 * (KMS/DRM/GBM/EGL, and Wayland/EGL) provide the platform + input glue that
 * GLFW used to. */
#include <GL/gl.h>
#include <GL/glext.h>
#include "luna-ui.h"

#define LUNA_CUR_IMPLEMENTATION
#include "luna-cur.h"

/* Backend selection headers.
 * KMS_BACKEND drives the display directly via DRM/KMS + GBM + EGL and reads
 * input via libinput — this is the path used on a bare console (dri),
 * with no compositor running. WL_BACKEND is a minimal Wayland/EGL client —
 * used whenever a compositor is present (WAYLAND_DISPLAY is set), which
 * includes running under Wayback, since Wayback is itself a Wayland
 * compositor (Xwayland-rootful over wlroots) and exposes a Wayland socket. */
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifdef LUNA_BACKEND_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#endif

/* Host-side aliases for the few key/modifier codes luna-shell.c itself
 * checks directly (menu shortcuts) but that luna-ui.h's neutral enum does
 * not define. Values match GLFW's, per the convention luna-ui.h documents,
 * so this file reads the same way it always has. */
#define LUNA_KEY_LEFT_SUPER  343
#define LUNA_KEY_RIGHT_SUPER 347
#define LUNA_KEY_COMMA       44
#define LUNA_KEY_F4          293
#define LUNA_MOD_CONTROL     0x0002
#define LUNA_MOD_ALT         0x0004
#define LUNA_MOD_SUPER       0x0008

#define LUNA_SHELL_VERSION "1.2"
#define MAX_WINDOWS    128
#define MAX_WIN_SLOTS  12
#define MAX_TRAY_SLOTS 8
#define MAX_SWITCHER_SLOTS 12
#define MAX_WIFI_NETWORKS 8

static int g_should_close = 0;
static int g_desktop_mode = 0;
static int g_fullscreen = 0;

static const char* g_layout_path = NULL;
static const char* g_css_path = NULL;
static const char* default_css =
#include "luna-shell.css.h"
;
static const char* default_html =
#include "luna-shell.html.h"
;

/* ── Applications (dock & launchpad) ──
 * default_cmd is the fallback; cmd[] is the mutable runtime command
 * (overridden by LUNA_APP_<NAME> env vars and by the settings dialog). */
typedef struct {
    const char* key;
    const char* name;
    const char* env;
    const char* default_cmd;
    char        cmd[256];
    pid_t       pid;
} LunaApp;

static LunaApp g_apps[] = {
    { .key = "files",    .name = "Files",     .env = "LUNA_APP_FILES",    .default_cmd = "pcmanfm"              },
    { .key = "terminal", .name = "Terminal",  .env = "LUNA_APP_TERMINAL", .default_cmd = "sakura"               },
    { .key = "browser",  .name = "Browser",   .env = "LUNA_APP_BROWSER",  .default_cmd = "firefox"              },
    { .key = "editor",   .name = "Editor",    .env = "LUNA_APP_EDITOR",   .default_cmd = "gedit"                },
    { .key = "music",    .name = "Music",     .env = "LUNA_APP_MUSIC",    .default_cmd = "gnome-music"          },
    { .key = "settings", .name = "Settings",  .env = "LUNA_APP_SETTINGS", .default_cmd = "gnome-control-center" },
    { .key = "demo",     .name = "GTK Demo",  .env = "LUNA_APP_DEMO",     .default_cmd = "gtk4-demo"            },
    { .key = "hello",    .name = "Hello GTK", .env = "LUNA_APP_HELLO",    .default_cmd = "hello-gtk"            },
};
#define APP_COUNT ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

/* ── Persistent settings ── */

typedef struct {
    char wallpaper[32]; /* "night" | "ocean" | "forest" | "sunset" */
    char hostname[64];
    char cursor_theme[64]; /* "aero" | "miku" | custom theme dir name */
    char kb_layout[64];    /* XKB layout, e.g. "jp,us" | "us" | "de,us" */
    int  window_gap;       /* tiled window inset in pixels: 0 | 8 | 16 */
    int  edge_snap;
    int  titlebar_double_click;
    int  super_shortcuts;
    int  dock_magnification;
    int  session_restore;
} LunaSettings;

static LunaSettings g_settings;
static LunaCurTheme g_cur_theme;
static int g_cursor_reload_pending = 0;

static void cursor_theme_reload(const char* name);
static void cursor_theme_tick_and_refresh(void);

static void settings_path(char* buf, size_t n) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/root";
    snprintf(buf, n, "%s/.config/luna-shell/settings.conf", home);
}

static void init_app_cmds(void) {
    for (int i = 0; i < APP_COUNT; i++) {
        const char* env = g_apps[i].env ? getenv(g_apps[i].env) : NULL;
        if (env && *env)
            snprintf(g_apps[i].cmd, sizeof(g_apps[i].cmd), "%s", env);
        else
            snprintf(g_apps[i].cmd, sizeof(g_apps[i].cmd), "%s", g_apps[i].default_cmd);
    }
}

static void settings_defaults(void) {
    snprintf(g_settings.wallpaper, sizeof(g_settings.wallpaper), "night");
    snprintf(g_settings.hostname, sizeof(g_settings.hostname), "Luna Desktop");
    snprintf(g_settings.cursor_theme, sizeof(g_settings.cursor_theme), "aero");
    g_settings.window_gap = 8;
    g_settings.edge_snap = 1;
    g_settings.titlebar_double_click = 1;
    g_settings.super_shortcuts = 1;
    g_settings.dock_magnification = 1;
    g_settings.session_restore = 1;
    /* Prefer env / locale-aware default already applied by apply_xkb_session_env. */
    {
        const char* lay = getenv("XKB_DEFAULT_LAYOUT");
        if (lay && *lay)
            snprintf(g_settings.kb_layout, sizeof(g_settings.kb_layout), "%s", lay);
        else
            snprintf(g_settings.kb_layout, sizeof(g_settings.kb_layout), "jp,us");
    }
}

static void settings_load(void) {
    settings_defaults();
    init_app_cmds();
    char path[512];
    settings_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512], section[64] = "";
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            snprintf(section, sizeof(section), "%.*s", (int)(sizeof(section)-1), line + 1);
            char* e = strchr(section, ']'); if (e) *e = 0;
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line, *val = eq + 1;
        if (!strcmp(section, "apps")) {
            for (int i = 0; i < APP_COUNT; i++)
                if (!strcmp(g_apps[i].key, key))
                    snprintf(g_apps[i].cmd, sizeof(g_apps[i].cmd), "%s", val);
        } else if (!strcmp(section, "shell")) {
            if (!strcmp(key, "wallpaper"))
                snprintf(g_settings.wallpaper, sizeof(g_settings.wallpaper), "%s", val);
            else if (!strcmp(key, "hostname"))
                snprintf(g_settings.hostname, sizeof(g_settings.hostname), "%s", val);
            else if (!strcmp(key, "cursor_theme"))
                snprintf(g_settings.cursor_theme, sizeof(g_settings.cursor_theme), "%s", val);
            else if (!strcmp(key, "kb_layout"))
                snprintf(g_settings.kb_layout, sizeof(g_settings.kb_layout), "%s", val);
            else if (!strcmp(key, "window_gap"))
                g_settings.window_gap = atoi(val);
            else if (!strcmp(key, "edge_snap"))
                g_settings.edge_snap = atoi(val) != 0;
            else if (!strcmp(key, "titlebar_double_click"))
                g_settings.titlebar_double_click = atoi(val) != 0;
            else if (!strcmp(key, "super_shortcuts"))
                g_settings.super_shortcuts = atoi(val) != 0;
            else if (!strcmp(key, "dock_magnification"))
                g_settings.dock_magnification = atoi(val) != 0;
            else if (!strcmp(key, "session_restore"))
                g_settings.session_restore = atoi(val) != 0;
        }
    }
    fclose(f);
}

static void ensure_config_dir(void) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/root";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/luna-shell", home);
    mkdir(dir, 0755);
}

static void settings_save(void) {
    ensure_config_dir();
    char path[512];
    settings_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[luna-shell] cannot write settings: %s\n", path);
        return;
    }
    fprintf(f, "# Luna Shell settings — auto-generated\n");
    fprintf(f, "[apps]\n");
    for (int i = 0; i < APP_COUNT; i++)
        fprintf(f, "%s=%s\n", g_apps[i].key, g_apps[i].cmd);
    fprintf(f, "\n[shell]\n");
    fprintf(f, "wallpaper=%s\n", g_settings.wallpaper);
    fprintf(f, "hostname=%s\n", g_settings.hostname);
    fprintf(f, "cursor_theme=%s\n", g_settings.cursor_theme);
    fprintf(f, "kb_layout=%s\n", g_settings.kb_layout);
    fprintf(f, "window_gap=%d\n", g_settings.window_gap);
    fprintf(f, "edge_snap=%d\n", g_settings.edge_snap);
    fprintf(f, "titlebar_double_click=%d\n", g_settings.titlebar_double_click);
    fprintf(f, "super_shortcuts=%d\n", g_settings.super_shortcuts);
    fprintf(f, "dock_magnification=%d\n", g_settings.dock_magnification);
    fprintf(f, "session_restore=%d\n", g_settings.session_restore);
    fclose(f);
}

/* Session restore state — impl after app_launch / window list helpers. */
static double g_session_restore_at = 0.0;
static int    g_session_restore_done = 0;
static double g_session_restore_deadline = 0.0;
static int    g_session_restore_active = 0;
static void session_save(void);
static void session_restore_schedule(void);
static void session_restore_tick(void);

#define MAX_RESTORE_WINDOWS 32
typedef struct {
    char app_id[64];
    char title[96];
    int x;
    int y;
    int minimized;
    int maximized;
    int fullscreen;
    int applied;
} LunaRestoreWindow;
static LunaRestoreWindow g_restore_windows[MAX_RESTORE_WINDOWS];
static int g_restore_window_count = 0;

/* ── Element indices resolved after layout load ── */
static int g_luna_menu_idx = -1;
static int g_cc_idx        = -1;
static int g_launchpad_idx = -1;
static int g_about_idx     = -1;
static int g_about_box_idx = -1;
static int g_confirm_idx   = -1;
static int g_confirm_box_idx = -1;
static int g_toast_idx     = -1;
static int g_lp_search_idx = -1;
static int g_settings_idx          = -1;
static int g_settings_sheet_idx    = -1;
static int g_settings_panel_apps   = -1;
static int g_settings_panel_disp   = -1;
static int g_settings_panel_wm     = -1;
static int g_stab_apps_idx         = -1;
static int g_stab_disp_idx         = -1;
static int g_stab_wm_idx           = -1;
static int g_win_menu_idx  = -1;
static int g_clip_menu_idx = -1;
static int g_mb_clip_idx   = -1;
static uint32_t g_win_menu_target = 0;
/* Cached menubar hit-test indices (avoid repeated ID lookups in mouse hook) */
static int g_mb_logo_idx  = -1;
static int g_mb_cc_idx    = -1;
static int g_mb_wifi_idx  = -1;
static int g_wifi_menu_idx = -1;
static int g_wifi_selected = -1;

/* ── Per-frame child element cache (populated once in bind_indices) ──
 * Eliminates O(n) element scans from update_window_list_ui,
 * update_tray_ui, update_switcher_ui, win_menu_set_maximize_label,
 * and update_stats which run at 8–120 Hz. */
static int g_win_label_idx[MAX_WIN_SLOTS];          /* span.win_label child of win_N */
static int g_tray_glyph_idx[MAX_TRAY_SLOTS];        /* span.tray_glyph child of tray_N */
static int g_mb_app_idx             = -1;           /* mb_app element */
static int g_mb_bat_icon_idx        = -1;           /* luna_icon child of mb_bat */
static int g_mb_wifi_icon_idx       = -1;           /* luna_icon child of mb_wifi text span */
static int g_sw_title_idx[MAX_SWITCHER_SLOTS];      /* span.sw_title child of sw_N */
static int g_sw_app_idx[MAX_SWITCHER_SLOTS];        /* span.sw_app child of sw_N */
static int g_wm_maximize_label_idx  = -1;           /* mi_label child of wm_maximize */
static int g_wm_fullscreen_label_idx = -1;          /* mi_label child of wm_fullscreen */

typedef enum { WIFI_NONE, WIFI_CONNMAN, WIFI_NMCLI } WifiBackend;
typedef struct {
    char name[96];
    char id[192];
    int connected;
    int signal;
} WifiNetwork;
static WifiBackend g_wifi_backend = WIFI_NONE;
static WifiNetwork g_wifi_networks[MAX_WIFI_NETWORKS];
static int g_wifi_count = 0;
static int g_wifi_powered = 0;

static double g_last_shell_poll = -10.0;
static struct timespec g_shell_state_mtime = { .tv_sec = -1, .tv_nsec = 0 };
static off_t g_shell_state_size = -1;

/* Cached system readings — updated in update_stats(), read everywhere else
 * to avoid synchronous sysfs I/O on every window-list or tray update. */
static int  g_cached_bat = -1;
static char g_cached_net[16] = "Offline";

/* ── Compositor window list + system tray (via luna-shell/state.json) ── */

typedef struct {
    uint32_t id;
    char     title[96];
    char     app_id[64];
    int      x;
    int      y;
    int      focused;
    int      minimized;
    int      maximized;
    int      fullscreen;
} LunaWinEntry;

typedef struct {
    char id[64];
    char label[48];
    char icon[24];
    char tooltip[96];
    uint32_t surface_id; /* parsed from id when applicable */
} LunaTrayEntry;

/* Keep the complete compositor snapshot, not only the handful of chips that
 * fit in the menu bar.  Window lookup, Dock grouping, session restore and the
 * Alt-Tab overlay must continue to work when more than MAX_WIN_SLOTS windows
 * are open.  A fixed array keeps this bounded and allocation-free. */
static LunaWinEntry  g_wins[MAX_WINDOWS];
static int           g_win_count = 0;
static LunaTrayEntry g_tray[MAX_TRAY_SLOTS];
static int           g_tray_count = 0;
static char          g_shell_state_path[512];
static char          g_shell_sock_path[512];
static int           g_win_slot_idx[MAX_WIN_SLOTS];  /* element index for win_0.. */
static int           g_tray_slot_idx[MAX_TRAY_SLOTS];  /* element index for tray_0.. */
static uint32_t      g_win_slot_id[MAX_WIN_SLOTS];    /* compositor surface id shown in slot */
static char          g_tray_slot_key[MAX_TRAY_SLOTS][64];

/* ── Dock magnification (macOS-style fisheye) ──
 * Cached at bind time: element indices of every .dock_icon square. Each frame
 * dock_magnify_tick() sets their transform_scale from the cursor's horizontal
 * distance so the hovered icon grows and its neighbours taper off, exactly
 * like the macOS Dock. The Luna UI engine already eases cur_scale toward
 * transform_scale, so the motion comes out smooth for free. */
#define MAX_DOCK_ICONS 16
static int g_dock_icon_idx[MAX_DOCK_ICONS];
static int g_dock_icon_count = 0;
static int g_dock_root_idx = -1;
static int g_about_sheet_max = 0;
static int g_settings_sheet_max = 0;

static void app_set_dot(LunaApp* app, int running);
static int read_battery_percent(void);
static const char* read_net_status(void);
static void set_hidden(int idx, int hidden);
static int elem_idx_of(LunaElement* e);
static void toast_show(const char* title, const char* msg, double secs);
static void on_control_center(LunaElement* e);

static void shell_paths_init(void) {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) xdg = "/tmp";
    snprintf(g_shell_state_path, sizeof(g_shell_state_path),
             "%s/luna-shell/state.json", xdg);
    snprintf(g_shell_sock_path, sizeof(g_shell_sock_path),
             "%s/luna-shell/luna-shell.sock", xdg);
}

static void shell_send_cmd(const char* cmd) {
    if (!cmd || !*cmd) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t splen = strlen(g_shell_sock_path);
    if (splen >= sizeof(addr.sun_path)) {
        close(fd);
        return;
    }
    memcpy(addr.sun_path, g_shell_sock_path, splen + 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    /* Commands are small, but stream writes are still allowed to be partial.
     * MSG_NOSIGNAL also prevents a compositor restart from killing the shell
     * with SIGPIPE between connect() and send(). */
    char wire[128];
    int wn = snprintf(wire, sizeof(wire), "%s\n", cmd);
    if (wn > 0 && (size_t)wn < sizeof(wire)) {
        size_t off = 0, len = (size_t)wn;
        while (off < len) {
            ssize_t sent = send(fd, wire + off, len - off, MSG_NOSIGNAL);
            if (sent > 0) {
                off += (size_t)sent;
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
    close(fd);
}

/* Native tray items are deliberately not Wayland surfaces.  A status item
 * owns no pixels on Wayland (the shell owns the panel), so a tiny Unix socket
 * is both cheaper and safer than keeping a hidden xdg/layer surface alive.
 * Service ids are restricted before they become a pathname. */
static void tray_send_action(const char* id, const char* action) {
    static const char prefix[] = "service:";
    if (!id || strncmp(id, prefix, sizeof(prefix) - 1) != 0 || !action || !*action)
        return;
    const char* name = id + sizeof(prefix) - 1;
    if (!*name) return;
    for (const char* p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return;

    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) xdg = "/tmp";
    char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
    int n = snprintf(path, sizeof(path), "%s/luna-shell/tray-%s.sock", xdg, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, (size_t)n + 1);
    (void)sendto(fd, action, strlen(action), MSG_DONTWAIT,
                 (struct sockaddr*)&addr, sizeof(addr));
    close(fd);
}

static const char* tray_glyph(const char* icon) {
    /* LunaSymbols (Font Awesome) codepoints as UTF-8. */
    if (!icon || !*icon) return "\uf111";                 /* circle */
    if (!strcmp(icon, "terminal")) return "\uf120";       /* terminal */
    if (!strcmp(icon, "browser"))  return "\uf0ac";       /* globe */
    if (!strcmp(icon, "files"))    return "\uf07b";       /* folder */
    if (!strcmp(icon, "editor"))   return "\uf1c9";       /* file-code */
    if (!strcmp(icon, "music"))    return "\uf001";       /* music */
    if (!strcmp(icon, "settings")) return "\uf013";       /* gear */
    if (!strcmp(icon, "gtk"))      return "\uf1b2";       /* cube */
    if (!strcmp(icon, "wifi"))     return "\uf1eb";       /* wifi */
    if (!strcmp(icon, "bat"))      return "\uf240";       /* battery-full */
    if (!strcmp(icon, "clip"))     return "\uf0ea";       /* copy / clipboard */
    if (!strncmp(icon, "app_active", 10) || !strncmp(icon, "terminal_active", 15)) {
        if (strstr(icon, "terminal")) return "\uf120";
        return "\uf111";
    }
    return "\uf111";
}

static void parse_tray_surface_id(LunaTrayEntry* t) {
    t->surface_id = 0;
    if (!strncmp(t->id, "win:", 4)) {
        t->surface_id = (uint32_t)strtoul(t->id + 4, NULL, 10);
    } else if (!strncmp(t->id, "app:", 4)) {
        for (int i = 0; i < g_win_count; i++) {
            if (!strcmp(g_wins[i].app_id, t->id + 4)) {
                t->surface_id = g_wins[i].id;
                return;
            }
        }
    }
}

static int g_switcher_ids[MAX_SWITCHER_SLOTS];
static int g_switcher_count = 0;
static int g_switcher_index = 0;
static int g_switcher_visible = 0;
static int g_pending_menu_id = 0;
static int g_pending_menu_x = 0;
static int g_pending_menu_y = 0;
static uint64_t g_shell_state_hash = 0;
static int g_shell_state_changed = 0;

/* Wayland layer-surface dirty bits — skip eglSwapBuffers when unchanged.
 * Continuous full-screen swaps were flooding the compositor (client flicker)
 * and delaying Firefox's WaitFlushedEvent / frame callbacks. */
static uint32_t g_surf_dirty = 0xffffffffu;
static double   g_last_bg_paint = 0.0;
static int      g_wl_poll_timeout_ms = 0;
/* Single-surface backends used to repaint the whole desktop unconditionally.
 * Keep a separate bit for them: g_surf_dirty describes Wayland layers, while
 * this flag says that the KMS/X11 framebuffer needs another complete frame. */
static int      g_frame_dirty = 1;
/* Event wait used by the single-surface backends.  KMS and X11 both need to
 * sleep when the framebuffer is clean; otherwise X11 busy-spins and a KMS
 * session without libinput does the same. */
static int      g_single_poll_timeout_ms = 0;
static void shell_request_repaint(int surf_idx); /* -1 = all; else LUNA_SURF_* */

static uint64_t hash_shell_snapshot(const LunaWinEntry* wins, int wc,
                                    const LunaTrayEntry* tray, int tc,
                                    int sw_count, int sw_idx, const uint32_t* sw_ids) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < wc; i++) {
        h = h * 0x100000001b3ULL ^ wins[i].id;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].focused;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].minimized;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].maximized;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].fullscreen;
        h = h * 0x100000001b3ULL ^ (uint64_t)(uint32_t)wins[i].x;
        h = h * 0x100000001b3ULL ^ (uint64_t)(uint32_t)wins[i].y;
        for (const char* p = wins[i].title; *p; p++)
            h = h * 0x100000001b3ULL ^ (uint8_t)*p;
        for (const char* p = wins[i].app_id; *p; p++)
            h = h * 0x100000001b3ULL ^ (uint8_t)*p;
    }
    for (int i = 0; i < tc; i++) {
        for (const char* p = tray[i].id; *p; p++)
            h = h * 0x100000001b3ULL ^ (uint8_t)*p;
        for (const char* p = tray[i].icon; *p; p++)
            h = h * 0x100000001b3ULL ^ (uint8_t)*p;
    }
    h = h * 0x100000001b3ULL ^ (uint64_t)sw_count;
    h = h * 0x100000001b3ULL ^ (uint64_t)sw_idx;
    for (int i = 0; i < sw_count; i++)
        h = h * 0x100000001b3ULL ^ sw_ids[i];
    return h;
}

static void load_shell_state(void) {
    /* The compositor rewrites this snapshot only when its state changes.
     * Avoid opening and parsing it at the 8--20 Hz polling rate while the
     * desktop is otherwise idle: on the direct KMS backend that synchronous
     * I/O runs on the render thread and periodically makes us miss a vblank. */
    struct stat st;
    if (stat(g_shell_state_path, &st) != 0) return;
    if (st.st_size == g_shell_state_size &&
        st.st_mtim.tv_sec == g_shell_state_mtime.tv_sec &&
        st.st_mtim.tv_nsec == g_shell_state_mtime.tv_nsec)
        return;

    FILE* f = fopen(g_shell_state_path, "r");
    if (!f) return;
    LunaWinEntry wins[MAX_WINDOWS];
    LunaTrayEntry tray[MAX_TRAY_SLOTS];
    int wc = 0, tc = 0;
    int sw_count = 0, sw_idx = 0;
    uint32_t sw_ids[MAX_SWITCHER_SLOTS];
    int have_menu = 0;
    unsigned menu_id = 0;
    int menu_x = 0, menu_y = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'W' && line[1] == '\t') {
            if (wc >= MAX_WINDOWS) continue;
            unsigned id = 0;
            int focused = 0, minimized = 0, maximized = 0, fullscreen = 0;
            int wx = 0, wy = 0;
            char title[96]; title[0] = 0;
            char app_id[64]; app_id[0] = 0;

            char* p = line + 2;
            char* tab1 = strchr(p, '\t'); if (!tab1) continue;
            *tab1 = 0;
            id = (unsigned)strtoul(p, NULL, 10);
            p = tab1 + 1;

            char* tab2 = strchr(p, '\t'); if (!tab2) continue;
            *tab2 = 0;
            snprintf(title, sizeof(title), "%s", p);
            p = tab2 + 1;

            char* tab3 = strchr(p, '\t'); if (!tab3) continue;
            *tab3 = 0;
            snprintf(app_id, sizeof(app_id), "%s", p);
            p = tab3 + 1;

            {
                int parsed = sscanf(p, "%d\t%d\t%d\t%d\t%d\t%d",
                                    &focused, &minimized, &maximized, &fullscreen, &wx, &wy);
                if (parsed < 2) continue;
                if (parsed < 3) maximized = 0;
                if (parsed < 4) fullscreen = 0;
                if (parsed < 5) wx = 0;
                if (parsed < 6) wy = 0;
            }

            wins[wc].id = (uint32_t)id;
            snprintf(wins[wc].title, sizeof(wins[wc].title), "%s", title);
            snprintf(wins[wc].app_id, sizeof(wins[wc].app_id), "%s", app_id);
            wins[wc].x = wx;
            wins[wc].y = wy;
            wins[wc].focused = focused;
            wins[wc].minimized = minimized;
            wins[wc].maximized = maximized;
            wins[wc].fullscreen = fullscreen;
            wc++;
        } else if (line[0] == 'T' && line[1] == '\t') {
            if (tc >= MAX_TRAY_SLOTS) continue;
            char id[64] = "", label[48] = "", icon[24] = "", tooltip[96] = "";
            if (sscanf(line + 2, "%63[^\t]\t%47[^\t]\t%23[^\t]\t%95[^\t]",
                       id, label, icon, tooltip) >= 2) {
                snprintf(tray[tc].id, sizeof(tray[tc].id), "%s", id);
                snprintf(tray[tc].label, sizeof(tray[tc].label), "%s", label);
                snprintf(tray[tc].icon, sizeof(tray[tc].icon), "%s", icon);
                snprintf(tray[tc].tooltip, sizeof(tray[tc].tooltip), "%s",
                         tooltip[0] ? tooltip : label);
                tray[tc].surface_id = 0;
                tc++;
            }
        } else if (line[0] == 'M' && line[1] == '\t') {
            if (sscanf(line + 2, "%u\t%d\t%d", &menu_id, &menu_x, &menu_y) == 3) {
                have_menu = 1;
            }
        } else if (line[0] == 'S' && line[1] == '\t') {
            char* p = line + 2;
            char* tab = strchr(p, '\t');
            if (!tab) continue;
            *tab = 0;
            sw_idx = atoi(p);
            p = tab + 1;
            sw_count = 0;
            while (*p && sw_count < MAX_SWITCHER_SLOTS) {
                sw_ids[sw_count++] = (uint32_t)strtoul(p, &p, 10);
                if (*p == ',') p++;
            }
        }
    }
    fclose(f);
    g_shell_state_size = st.st_size;
    g_shell_state_mtime = st.st_mtim;

    uint64_t h = hash_shell_snapshot(wins, wc, tray, tc, sw_count, sw_idx, sw_ids);
    int state_changed = (h != g_shell_state_hash);
    if (state_changed) {
        g_shell_state_hash = h;
        g_win_count = wc;
        memcpy(g_wins, wins, (size_t)wc * sizeof(LunaWinEntry));
        for (int i = 0; i < tc; i++) parse_tray_surface_id(&tray[i]);
        g_tray_count = tc;
        memcpy(g_tray, tray, (size_t)tc * sizeof(LunaTrayEntry));
        if (sw_count > 0) {
            g_switcher_count = sw_count;
            g_switcher_index = sw_idx;
            for (int i = 0; i < sw_count; i++)
                g_switcher_ids[i] = (int)sw_ids[i];
            g_switcher_visible = 1;
        } else {
            g_switcher_visible = 0;
            g_switcher_count = 0;
        }
    }

    if (have_menu) {
        g_pending_menu_id = (int)menu_id;
        g_pending_menu_x = menu_x;
        g_pending_menu_y = menu_y;
    }
    /* Stash whether UI widgets need a refresh (used by poll_shell_state). */
    g_shell_state_changed = state_changed;
}

static void win_slot_style(int slot, LunaWinEntry* w) {
    if (slot < 0) return;
    luna_remove_class(slot, "active");
    luna_remove_class(slot, "minimized");
    if (w->focused) luna_add_class(slot, "active");
    if (w->minimized) luna_add_class(slot, "minimized");
    luna_update_element_style(slot);
}

static void tray_slot_style(int slot, LunaTrayEntry* t) {
    if (slot < 0) return;
    const char* icons[] = {"icon_terminal","icon_browser","icon_files","icon_editor",
                           "icon_music","icon_settings","icon_gtk","icon_wifi","icon_bat","icon_app"};
    for (size_t i = 0; i < sizeof(icons)/sizeof(icons[0]); i++)
        luna_remove_class(slot, icons[i]);
    char cls[32];
    snprintf(cls, sizeof(cls), "icon_%s", t->icon);
    /* strip _active suffix for CSS class */
    char* suf = strstr(cls, "_active");
    if (suf) *suf = 0;
    luna_add_class(slot, cls);
    if (strstr(t->icon, "_active"))
        luna_add_class(slot, "active");
    else
        luna_remove_class(slot, "active");
    luna_update_element_style(slot);
}

static void update_window_list_ui(void) {
    /* Focused first, then usable windows, then minimized ones.  This avoids a
     * minimized window occupying one of the scarce chips while a visible
     * window is hidden, without sorting or allocating on the refresh path. */
    int order[MAX_WIN_SLOTS];
    int n = 0;
    for (int i = 0; i < g_win_count && n < MAX_WIN_SLOTS; i++)
        if (g_wins[i].focused) order[n++] = i;
    for (int i = 0; i < g_win_count && n < MAX_WIN_SLOTS; i++)
        if (!g_wins[i].focused && !g_wins[i].minimized) order[n++] = i;
    for (int i = 0; i < g_win_count && n < MAX_WIN_SLOTS; i++)
        if (!g_wins[i].focused && g_wins[i].minimized) order[n++] = i;

    for (int s = 0; s < MAX_WIN_SLOTS; s++) {
        int idx = g_win_slot_idx[s];
        if (idx < 0) continue;
        if (s >= n) {
            set_hidden(idx, 1);
            g_win_slot_id[s] = 0;
            continue;
        }
        LunaWinEntry* w = &g_wins[order[s]];
        g_win_slot_id[s] = w->id;
        set_hidden(idx, 0);
        if (g_win_label_idx[s] >= 0)
            luna_set_text(g_win_label_idx[s], w->title);
        win_slot_style(idx, w);
    }
    /* macOS-style active app name next to the logo */
    {
        int app_idx = g_mb_app_idx;
        if (app_idx >= 0) {
            const char* name = "Desktop";
            for (int i = 0; i < g_win_count; i++) {
                if (g_wins[i].focused && !g_wins[i].minimized) {
                    if (g_wins[i].app_id[0])
                        name = g_wins[i].app_id;
                    else if (g_wins[i].title[0])
                        name = g_wins[i].title;
                    break;
                }
            }
            /* Prefer a short display name: last dotted component of app_id */
            char shortbuf[96];
            if (strcmp(name, "Desktop") != 0 && strchr(name, '.')) {
                const char* last = strrchr(name, '.');
                if (last && last[1]) {
                    snprintf(shortbuf, sizeof(shortbuf), "%s", last + 1);
                    name = shortbuf;
                }
            }
            luna_set_text(app_idx, name);
        }
    }
    luna_mark_layout_dirty();
    shell_request_repaint(1); /* LUNA_SURF_MENUBAR */
}

static int str_has_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < n) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (!a || a != b) break;
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}

static int is_browser_cmd(const LunaApp* app, const char* cmd) {
    if (app && !strcmp(app->key, "browser")) return 1;
    return cmd && (str_has_ci(cmd, "firefox") || str_has_ci(cmd, "chrom"));
}

/* Remove one exact path entry from LD_LIBRARY_PATH. */
static void ld_library_path_remove_entry(const char* remove_path) {
    const char* cur = getenv("LD_LIBRARY_PATH");
    if (!cur || !*cur || !remove_path || !*remove_path) return;

    char out[2048];
    size_t out_len = 0;
    const char* p = cur;
    while (*p) {
        const char* colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        int keep = !(strlen(remove_path) == len && !strncmp(p, remove_path, len));
        if (keep && len > 0) {
            if (out_len > 0 && out_len < sizeof(out) - 1)
                out[out_len++] = ':';
            if (out_len + len >= sizeof(out))
                break;
            memcpy(out + out_len, p, len);
            out_len += len;
            out[out_len] = 0;
        }
        if (!colon) break;
        p = colon + 1;
    }
    if (out_len > 0)
        setenv("LD_LIBRARY_PATH", out, 1);
    else
        unsetenv("LD_LIBRARY_PATH");
}

/* Browser clients must use the system libwayland stack.
 * Remove known vendored paths even when launcher vars are absent. */
static void sanitize_browser_ld_library_path(void) {
    const char* configured = getenv("LUNA_WAYLAND_CLIENT_LIBPATH");
    if (configured && *configured)
        ld_library_path_remove_entry(configured);
    ld_library_path_remove_entry("/usr/local/lib/luna");
    ld_library_path_remove_entry("/usr/local/lib/luna/");
}

static int app_id_matches_key(const char* app_id, const char* key) {
    if (!app_id || !*app_id || !key || !*key) return 0;
    if (str_has_ci(app_id, key)) return 1;
    if (!strcmp(key, "files"))
        return str_has_ci(app_id, "pcmanfm") || str_has_ci(app_id, "thunar") ||
               str_has_ci(app_id, "nautilus") || str_has_ci(app_id, "nemo") ||
               str_has_ci(app_id, "caja") || str_has_ci(app_id, "org.gnome.Nautilus");
    if (!strcmp(key, "terminal"))
        return str_has_ci(app_id, "sakura") || str_has_ci(app_id, "terminal") ||
               str_has_ci(app_id, "kitty") || str_has_ci(app_id, "alacritty") ||
               str_has_ci(app_id, "xterm") || str_has_ci(app_id, "foot");
    if (!strcmp(key, "browser"))
        return str_has_ci(app_id, "firefox") || str_has_ci(app_id, "chrome") ||
               str_has_ci(app_id, "chromium") || str_has_ci(app_id, "brave") ||
               str_has_ci(app_id, "epiphany") || str_has_ci(app_id, "org.mozilla");
    if (!strcmp(key, "editor"))
        return str_has_ci(app_id, "gedit") || str_has_ci(app_id, "mousepad") ||
               str_has_ci(app_id, "leafpad") || str_has_ci(app_id, "TextEditor") ||
               str_has_ci(app_id, "code") || str_has_ci(app_id, "kate");
    if (!strcmp(key, "music"))
        return str_has_ci(app_id, "music") || str_has_ci(app_id, "rhythmbox") ||
               str_has_ci(app_id, "spotify");
    if (!strcmp(key, "settings"))
        return str_has_ci(app_id, "control-center") || str_has_ci(app_id, "Settings") ||
               str_has_ci(app_id, "gnome-control");
    return 0;
}

static LunaWinEntry* find_win_for_app(LunaApp* app) {
    LunaWinEntry* focused = NULL;
    LunaWinEntry* visible = NULL;
    LunaWinEntry* any = NULL;
    int focused_idx = -1;
    for (int i = 0; i < g_win_count; i++) {
        if (!app_id_matches_key(g_wins[i].app_id, app->key)) continue;
        if (!any) any = &g_wins[i];
        if (!g_wins[i].minimized && !visible) visible = &g_wins[i];
        if (g_wins[i].focused) {
            focused = &g_wins[i];
            focused_idx = i;
        }
    }
    /* Repeated dock clicks cycle through this app's windows (macOS Dock). */
    if (focused && focused_idx >= 0) {
        for (int step = 1; step <= g_win_count; step++) {
            int j = (focused_idx + step) % g_win_count;
            if (!app_id_matches_key(g_wins[j].app_id, app->key)) continue;
            if (g_wins[j].minimized) continue;
            if (&g_wins[j] != focused) return &g_wins[j];
        }
        return focused;
    }
    if (visible) return visible;
    return any;
}

static void update_dock_dots(void) {
    for (int i = 0; i < APP_COUNT; i++) {
        int running = (g_apps[i].pid > 0);
        if (!running) {
            for (int w = 0; w < g_win_count; w++) {
                if (app_id_matches_key(g_wins[w].app_id, g_apps[i].key)) {
                    running = 1;
                    break;
                }
            }
        }
        app_set_dot(&g_apps[i], running);
    }
    shell_request_repaint(2); /* dock dots */
}

static void update_switcher_ui(void) {
    int root = luna_get_element_by_id("switcher");
    if (root < 0) return;
    if (!g_switcher_visible || g_switcher_count <= 0) {
        set_hidden(root, 1);
        return;
    }
    set_hidden(root, 0);
    for (int s = 0; s < MAX_SWITCHER_SLOTS; s++) {
        char id[16];
        snprintf(id, sizeof(id), "sw_%d", s);
        int idx = luna_get_element_by_id(id);
        if (idx < 0) continue;
        if (s >= g_switcher_count) {
            set_hidden(idx, 1);
            continue;
        }
        set_hidden(idx, 0);
        uint32_t sid = (uint32_t)g_switcher_ids[s];
        const char* title = "Window";
        const char* app = "";
        for (int i = 0; i < g_win_count; i++) {
            if (g_wins[i].id == sid) {
                title = g_wins[i].title[0] ? g_wins[i].title : "Window";
                app = g_wins[i].app_id;
                break;
            }
        }
        if (g_sw_title_idx[s] >= 0) luna_set_text(g_sw_title_idx[s], title);
        if (g_sw_app_idx[s] >= 0)   luna_set_text(g_sw_app_idx[s], app && app[0] ? app : "app");
        luna_remove_class(idx, "active");
        if (s == g_switcher_index) luna_add_class(idx, "active");
        luna_update_element_style(idx);
    }
    luna_mark_layout_dirty();
    shell_request_repaint(0); /* switcher lives in bg layer */
}

static void position_menu_at(int menu_idx, float x, float y) {
    if (menu_idx < 0) return;
    LunaElement* m = luna_element_at(menu_idx);
    float mw = m->w > 1.0f ? m->w : (m->css_width > 1.0f ? m->css_width : 200.0f);
    float mh = m->h > 1.0f ? m->h : (m->css_height > 1.0f ? m->css_height : 160.0f);
    if (x + mw > luna_window_width - 8.0f) x = luna_window_width - mw - 8.0f;
    if (y + mh > luna_window_height - 8.0f) y = luna_window_height - mh - 8.0f;
    if (x < 6.0f) x = 6.0f;
    if (y < 28.0f) y = 28.0f;
    m->rel_x = floorf(x);
    m->rel_y = floorf(y);
    m->pos_overridden_x = 1;
    m->pos_overridden_y = 1;
    m->pct_left = 0;
    m->pct_top = 0;
    m->has_left = 1;
    m->has_top = 1;
    /* A CSS right/bottom inset belongs to the stylesheet's default position.
     * Once the shell takes ownership of a popup position, leave a single
     * unambiguous pair of insets.  In particular, clip_menu has `right` in
     * CSS; retaining it here made the next layout pass choose a different
     * horizontal anchor from the frame that opened the menu. */
    m->has_right = 0;
    m->has_bottom = 0;
    m->raw_left = m->rel_x;
    m->raw_top = m->rel_y;
    luna_mark_layout_dirty();
}

static void win_menu_set_maximize_label(LunaWinEntry* w) {
    const char* label  = (w && (w->maximized || w->fullscreen)) ? "Restore" : "Maximize";
    const char* flabel = (w && w->fullscreen) ? "Exit Fullscreen" : "Fullscreen";
    if (g_wm_maximize_label_idx >= 0)  luna_set_text(g_wm_maximize_label_idx,  label);
    if (g_wm_fullscreen_label_idx >= 0) luna_set_text(g_wm_fullscreen_label_idx, flabel);
}

static void update_tray_ui(void) {
    /* Reserve 2 slots for built-in network/battery indicators at the end. */
    int builtin = 2;
    int app_slots = MAX_TRAY_SLOTS - builtin;
    if (app_slots < 1) app_slots = 1;
    int have_wifi_service = 0;
    for (int i = 0; i < g_tray_count; i++)
        if (!strcmp(g_tray[i].id, "service:luna-wifi")) {
            have_wifi_service = 1;
            break;
        }

    for (int s = 0; s < app_slots; s++) {
        int idx = g_tray_slot_idx[s];
        if (idx < 0) continue;
        if (s >= g_tray_count) {
            set_hidden(idx, 1);
            g_tray_slot_key[s][0] = 0;
            continue;
        }
        LunaTrayEntry* t = &g_tray[s];
        snprintf(g_tray_slot_key[s], sizeof(g_tray_slot_key[s]), "%.63s", t->id);
        set_hidden(idx, 0);
        if (g_tray_glyph_idx[s] >= 0)
            luna_set_text(g_tray_glyph_idx[s], tray_glyph(t->icon));
        tray_slot_style(idx, t);
    }

    /* Built-in tray: Wi-Fi + battery — use cached values to avoid per-call I/O. */
    char buf[32];
    int bat = g_cached_bat;
    int wifi_idx = g_tray_slot_idx[app_slots];
    int bat_idx  = g_tray_slot_idx[app_slots + 1];
    if (wifi_idx >= 0) {
        if (have_wifi_service) {
            /* The native service occupies an ordinary app slot.  Do not draw
             * the old synthetic indicator alongside it. */
            set_hidden(wifi_idx, 1);
        } else {
            set_hidden(wifi_idx, 0);
            LunaTrayEntry tw = { .id = "builtin:wifi", .icon = "wifi" };
            snprintf(tw.tooltip, sizeof(tw.tooltip), "%s", g_cached_net);
            tray_slot_style(wifi_idx, &tw);
            if (g_tray_glyph_idx[app_slots] >= 0)
                luna_set_text(g_tray_glyph_idx[app_slots], tray_glyph("wifi"));
        }
    }
    if (bat_idx >= 0) {
        set_hidden(bat_idx, 0);
        LunaTrayEntry tb = { .id = "builtin:bat", .icon = "bat" };
        if (bat >= 0) snprintf(tb.tooltip, sizeof(tb.tooltip), "Battery %d%%", bat);
        else snprintf(tb.tooltip, sizeof(tb.tooltip), "AC Power");
        tray_slot_style(bat_idx, &tb);
        if (g_tray_glyph_idx[app_slots + 1] >= 0)
            luna_set_text(g_tray_glyph_idx[app_slots + 1], tray_glyph("bat"));
        (void)buf;
    }
    luna_mark_layout_dirty();
    shell_request_repaint(1); /* menubar tray */
}

static double g_last_clock = -10.0;
static double g_last_stats = -10.0;
static double g_last_disk  = -60.0;
static double g_now = 0.0;
static double g_toast_deadline = 0.0;
static char   g_lp_query[160] = "";

/* Pending confirmation action */
enum { ACT_NONE = 0, ACT_SHUTDOWN, ACT_RESTART, ACT_LOGOUT };
static int g_pending_action = ACT_NONE;

/* /proc/stat sampling for CPU% */
static unsigned long long g_cpu_prev_idle = 0, g_cpu_prev_total = 0;

/* ── Small helpers ── */

static int elem_idx_of(LunaElement* e) {
    for (int i = 0; i < luna_element_count(); i++)
        if (luna_element_at(i) == e) return i;
    return -1;
}

static int ci_contains(const char* hay, const char* needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Toggle visibility via the "hidden" class so display_mode is recomputed. */
static void set_hidden(int idx, int hidden) {
    if (idx < 0) return;
    if (hidden) luna_add_class(idx, "hidden");
    else luna_remove_class(idx, "hidden");
    luna_update_element_style(idx);
    luna_mark_layout_dirty();
    /* Mark the KMS/X11 framebuffer dirty.  WL callers own surface-specific
     * shell_request_repaint() calls; blasting all surfaces here was the main
     * cause of full-desktop repaints on every window-list / tray update.
     * Overlay show/hide is handled by wl_surfs_update() via was_shown. */
    g_frame_dirty = 1;
}

static int is_shown(int idx) {
    if (idx < 0) return 0;
    LunaElement* e = luna_element_at(idx);
    return e && strstr(e->class_name, "hidden") == NULL;
}

/* Wire handler onto element and every descendant (no event bubbling in engine). */
static void wire_subtree(int root, LunaEventHandler fn) {
    if (root < 0) return;
    luna_set_on_click(root, fn);
    for (int i = 0; i < luna_element_count(); i++) {
        for (int p = luna_element_at(i)->parent_idx; p != -1; p = luna_element_at(p)->parent_idx) {
            if (p == root) { luna_set_on_click(i, fn); break; }
        }
    }
}

/* Wi-Fi control intentionally uses argv, never a shell command assembled from
 * an SSID or password.  This avoids shell injection and quoting bugs. */
static void wifi_exec(const char* const argv[]) {
    pid_t pid = fork();
    if (pid != 0) return;
    execvp(argv[0], (char* const*)argv);
    _exit(127);
}

/* ConnMan obtains Wi-Fi credentials through an Agent, not through `config`.
 * Run connmanctl interactively and feed the agent response over stdin.  Keeping
 * the passphrase out of argv also prevents it being exposed by process tools. */
static int wifi_connman_connect(const char* service, const char* passphrase) {
    if (!service || !*service || !passphrase ||
        strchr(service, '\n') || strchr(service, '\r') ||
        strchr(passphrase, '\n') || strchr(passphrase, '\r')) return 0;

    int input[2];
    if (pipe(input) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(input[0]);
        close(input[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(input[0], STDIN_FILENO);
        close(input[0]);
        close(input[1]);
        execlp("connmanctl", "connmanctl", (char*)NULL);
        _exit(127);
    }

    close(input[0]);
    FILE* f = fdopen(input[1], "w");
    if (!f) {
        close(input[1]);
        return 0;
    }
    fprintf(f, "agent on\nconnect %s\n%s\nquit\n", service, passphrase);
    fclose(f);
    return 1;
}

static int command_available(const char* name) {
    const char* path = getenv("PATH");
    if (!path) return 0;
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char* d = strtok(copy, ":"); d; d = strtok(NULL, ":")) {
        char file[512];
        snprintf(file, sizeof(file), "%s/%s", d, name);
        if (access(file, X_OK) == 0) return 1;
    }
    return 0;
}

static void trim_line(char* s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void wifi_refresh(void) {
    g_wifi_count = 0;
    g_wifi_powered = 0;
    g_wifi_backend = command_available("connmanctl") ? WIFI_CONNMAN :
                     (command_available("nmcli") ? WIFI_NMCLI : WIFI_NONE);
    FILE* f = NULL;
    char line[512];
    if (g_wifi_backend == WIFI_CONNMAN) {
        f = popen("connmanctl technologies 2>/dev/null", "r");
        int in_wifi = 0;
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '/') in_wifi = strstr(line, "/technology/wifi") != NULL;
                else if (in_wifi && strstr(line, "Powered") && strstr(line, "True")) g_wifi_powered = 1;
            }
            pclose(f);
        }
        if (!g_wifi_powered) return;
        f = popen("connmanctl services 2>/dev/null", "r");
        if (!f) return;
        while (g_wifi_count < MAX_WIFI_NETWORKS && fgets(line, sizeof(line), f)) {
            char* path = strstr(line, "wifi_");
            if (!path) continue;
            char* begin = path;
            while (begin > line && !isspace((unsigned char)begin[-1])) begin--;
            if (begin > line && begin[-1] == '/') begin--;
            char id[192];
            snprintf(id, sizeof(id), "%s", begin);
            trim_line(id);
            *begin = 0;
            char* label = line;
            while (*label && (isspace((unsigned char)*label) || strchr("*AOR", *label))) label++;
            trim_line(label);
            WifiNetwork* n = &g_wifi_networks[g_wifi_count++];
            snprintf(n->name, sizeof(n->name), "%.95s", *label ? label : "Hidden network");
            snprintf(n->id, sizeof(n->id), "%s", id);
            n->connected = strchr(line, '*') != NULL;
            n->signal = -1;
        }
        pclose(f);
    } else if (g_wifi_backend == WIFI_NMCLI) {
        f = popen("nmcli -t -f WIFI general 2>/dev/null", "r");
        if (f && fgets(line, sizeof(line), f)) g_wifi_powered = strstr(line, "enabled") != NULL;
        if (f) pclose(f);
        if (!g_wifi_powered) return;
        f = popen("nmcli -t -f IN-USE,SSID,SIGNAL device wifi list 2>/dev/null", "r");
        if (!f) return;
        while (g_wifi_count < MAX_WIFI_NETWORKS && fgets(line, sizeof(line), f)) {
            trim_line(line);
            char* first = strchr(line, ':');
            char* last = strrchr(line, ':');
            if (!first || !last || first == last) continue;
            *first++ = 0; *last++ = 0;
            WifiNetwork* n = &g_wifi_networks[g_wifi_count++];
            snprintf(n->name, sizeof(n->name), "%s", *first ? first : "Hidden network");
            snprintf(n->id, sizeof(n->id), "%s", first);
            n->connected = line[0] == '*';
            n->signal = atoi(last);
        }
        pclose(f);
    }
}

static void wifi_update_ui(void) {
    int p = luna_get_element_by_id("wifi_power");
    if (p >= 0) {
        if (g_wifi_powered) luna_add_class(p, "on"); else luna_remove_class(p, "on");
        luna_update_element_style(p);
    }
    int st = luna_get_element_by_id("wifi_status");
    if (st >= 0) luna_set_text(st, g_wifi_backend == WIFI_NONE ? "ConnMan / NetworkManager not found" :
        (!g_wifi_powered ? "Wi-Fi is turned off" : (g_wifi_count ? "Available networks" : "No networks found")));
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        char id[24]; snprintf(id, sizeof(id), "wifi_%d", i);
        int row = luna_get_element_by_id(id);
        if (row < 0) continue;
        if (i >= g_wifi_count) { set_hidden(row, 1); continue; }
        set_hidden(row, 0);
        if (g_wifi_networks[i].connected) luna_add_class(row, "connected"); else luna_remove_class(row, "connected");
        for (int j = 0; j < luna_element_count(); j++) {
            LunaElement* child = luna_element_at(j);
            if (child->parent_idx != row) continue;
            if (strstr(child->class_name, "mi_label")) luna_set_text(j, g_wifi_networks[i].name);
            if (strstr(child->class_name, "wifi_signal")) {
                char sig[20];
                if (g_wifi_networks[i].connected) snprintf(sig, sizeof(sig), "Connected");
                else if (g_wifi_networks[i].signal >= 0) snprintf(sig, sizeof(sig), "%d%%", g_wifi_networks[i].signal);
                else sig[0] = 0;
                luna_set_text(j, sig);
            }
        }
        luna_update_element_style(row);
    }
    luna_mark_layout_dirty();
}

static void center_element(int idx) {
    if (idx < 0) return;
    LunaElement* e = luna_element_at(idx);
    /* The sheet's offsets are relative to its containing overlay.  Using the
     * overlay's resolved size keeps centring correct after a compositor resize
     * and for non-fullscreen modal hosts, rather than relying on bootstrap
     * window dimensions. */
    float host_w = luna_window_width;
    float host_h = luna_window_height;
    if (e->parent_idx >= 0) {
        LunaElement* host = luna_element_at(e->parent_idx);
        if (host && host->w > 1.0f) host_w = host->w;
        if (host && host->h > 1.0f) host_h = host->h;
    }
    float w = e->css_width > 0 ? e->css_width : (e->w > 0 ? e->w : e->raw_w);
    float h = e->css_height > 0 ? e->css_height : (e->h > 0 ? e->h : e->raw_h);
    if (w <= 1.0f && e->has_css_width) w = e->css_width;
    if (h <= 1.0f && e->has_css_height) h = e->css_height;
    /* Fall back to known dialog sizes when layout hasn't resolved yet. */
    if (w <= 1.0f) w = 360.0f;
    if (h <= 1.0f) h = 240.0f;
    /* Center in the same viewport coordinate system used by a dragged sheet.
     * Use a slightly elevated optical center, but never allow a sheet to
     * start outside the usable desktop. */
    e->rel_x = floorf((host_w - w) * 0.5f);
    e->rel_y = floorf((host_h - h) * 0.42f);
    if (e->rel_x < 12.0f) e->rel_x = 12.0f;
    if (e->rel_y < 36.0f) e->rel_y = 36.0f;
    if (e->rel_x + w > host_w - 12.0f)
        e->rel_x = fmaxf(12.0f, host_w - w - 12.0f);
    if (e->rel_y + h > host_h - 12.0f)
        e->rel_y = fmaxf(36.0f, host_h - h - 12.0f);
    e->pos_overridden_x = 1;
    e->pos_overridden_y = 1;
    e->pct_left = 0;
    e->pct_top = 0;
    e->has_left = 1;
    e->has_top = 1;
    e->raw_left = e->rel_x;
    e->raw_top = e->rel_y;
    e->margin_left = 0;
    e->margin_top = 0;
    luna_mark_layout_dirty();
}

static void position_menu_near(int menu_idx, int anchor_idx, float fallback_x) {
    if (menu_idx < 0) return;
    LunaElement* m = luna_element_at(menu_idx);
    float x = fallback_x;
    float y = 32.0f;
    float mw = m->css_width > 1.0f ? m->css_width :
               (m->w > 1.0f ? m->w : 240.0f);
    if (anchor_idx >= 0) {
        LunaElement* a = luna_element_at(anchor_idx);
        x = a->x;
        y = a->y + a->h + 4.0f;
        if (y < 32.0f) y = 32.0f;
    }
    if (x + mw > luna_window_width - 8.0f)
        x = luna_window_width - mw - 8.0f;
    if (x < 6.0f) x = 6.0f;
    m->rel_x = floorf(x);
    m->rel_y = floorf(y);
    m->pos_overridden_x = 1;
    m->pos_overridden_y = 1;
    m->pct_left = 0;
    m->pct_top = 0;
    m->has_left = 1;
    m->has_top = 1;
    m->has_right = 0;
    m->has_bottom = 0;
    m->raw_left = m->rel_x;
    m->raw_top = m->rel_y;
    luna_mark_layout_dirty();
}

/* The control center has a fixed visual width.  Position it from the right
 * edge explicitly instead of relying on the CSS right inset: this also keeps
 * Wi-Fi and Control Center clicks opening the same, correctly aligned panel. */
static void position_control_center(void) {
    if (g_cc_idx < 0) return;
    LunaElement* m = luna_element_at(g_cc_idx);
    float w = m->css_width > 1.0f ? m->css_width : (m->w > 1.0f ? m->w : 324.0f);
    m->rel_x = floorf(luna_window_width - w - 8.0f);
    m->rel_y = 36.0f;
    if (m->rel_x < 6.0f) m->rel_x = 6.0f;
    m->pos_overridden_x = 1;
    m->pos_overridden_y = 1;
    m->has_left = 1;
    m->has_top = 1;
    m->has_right = 0;
    m->pct_left = 0;
    m->pct_top = 0;
    m->raw_left = m->rel_x;
    m->raw_top = m->rel_y;
    luna_mark_layout_dirty();
}

/* ── Toast notifications ── */

static void toast_show(const char* title, const char* msg, double secs) {
    int t = luna_get_element_by_id("toast_title");
    int m = luna_get_element_by_id("toast_msg");
    if (t != -1) luna_set_text(t, title);
    if (m != -1) luna_set_text(m, msg);
    set_hidden(g_toast_idx, 0);
    g_toast_deadline = g_now + secs;
}

static void on_toast_close(LunaElement* e) {
    (void)e;
    set_hidden(g_toast_idx, 1);
    g_toast_deadline = 0.0;
}

/* ── App launching / session env ── */

/* True when LUNA_IM_WAYLAND opts into in-process gim/whiz instead of
 * compositor text-input-v3 → whiz-im-wayland (apps/whiz/im-wayland.c). */
static int luna_im_use_gim(void) {
    const char* im_wl = getenv("LUNA_IM_WAYLAND");
    return im_wl && (!strcmp(im_wl, "0") || !strcmp(im_wl, "no") ||
                     !strcmp(im_wl, "false") || !strcmp(im_wl, "off") ||
                     !strcmp(im_wl, "gim") || !strcmp(im_wl, "whiz"));
}

/* Toolkit + Japanese IME env for the Wayland session.
 * Console / Berry X logins often export GTK_IM_MODULE=gim; that bypasses
 * text-input-v3 so whiz-im-wayland never sees key events.  Default is to
 * force GTK_IM_MODULE=wayland (overwrite=1).  Opt out: LUNA_IM_WAYLAND=0. */
static void apply_xkb_session_env(void) {
    /* Prefer saved settings, then existing env, then locale-based default. */
    if (g_settings.kb_layout[0])
        setenv("XKB_DEFAULT_LAYOUT", g_settings.kb_layout, 1);
    else if (!getenv("XKB_DEFAULT_LAYOUT") || !getenv("XKB_DEFAULT_LAYOUT")[0]) {
        const char* lang = getenv("LC_ALL");
        if (!lang || !*lang) lang = getenv("LC_CTYPE");
        if (!lang || !*lang) lang = getenv("LANG");
        if (!lang) lang = "";
        if (!strncasecmp(lang, "ja", 2))
            setenv("XKB_DEFAULT_LAYOUT", "jp,us", 0);
        else if (!strncasecmp(lang, "ko", 2))
            setenv("XKB_DEFAULT_LAYOUT", "kr,us", 0);
        else if (!strncasecmp(lang, "de", 2))
            setenv("XKB_DEFAULT_LAYOUT", "de,us", 0);
        else if (!strncasecmp(lang, "fr", 2))
            setenv("XKB_DEFAULT_LAYOUT", "fr,us", 0);
        else
            setenv("XKB_DEFAULT_LAYOUT", "us", 0);
    }
    const char* lay = getenv("XKB_DEFAULT_LAYOUT");
    if (lay && strchr(lay, ',') &&
        (!getenv("XKB_DEFAULT_OPTIONS") || !getenv("XKB_DEFAULT_OPTIONS")[0]))
        setenv("XKB_DEFAULT_OPTIONS", "grp:alt_shift_toggle", 0);
}

static void apply_keyboard_layout(const char* layout) {
    if (!layout || !*layout) return;
    snprintf(g_settings.kb_layout, sizeof(g_settings.kb_layout), "%s", layout);
    setenv("XKB_DEFAULT_LAYOUT", layout, 1);
    if (strchr(layout, ',') &&
        (!getenv("XKB_DEFAULT_OPTIONS") || !getenv("XKB_DEFAULT_OPTIONS")[0]))
        setenv("XKB_DEFAULT_OPTIONS", "grp:alt_shift_toggle", 0);
    /* Ask compositor to rebuild wl_keyboard.keymap for all clients. */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "keymap %s", layout);
    shell_send_cmd(cmd);
}

static void settings_mark_kb(const char* layout) {
    static const char* ids[] = {
        "kb_jp_us", "kb_us", "kb_jp", "kb_de_us", "kb_fr_us", "kb_kr_us", NULL
    };
    static const char* vals[] = {
        "jp,us", "us", "jp", "de,us", "fr,us", "kr,us", NULL
    };
    for (int i = 0; ids[i]; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx < 0) continue;
        if (vals[i] && layout && !strcmp(vals[i], layout))
            luna_add_class(idx, "selected");
        else
            luna_remove_class(idx, "selected");
        luna_update_element_style(idx);
    }
}

static void on_kb_select(LunaElement* e) {
    int idx = elem_idx_of(e);
    const char* layout = NULL;
    for (int i = idx; i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (id[0] == 'k' && id[1] == 'b' && id[2] == '_') {
            if (!strcmp(id, "kb_jp_us")) layout = "jp,us";
            else if (!strcmp(id, "kb_us")) layout = "us";
            else if (!strcmp(id, "kb_jp")) layout = "jp";
            else if (!strcmp(id, "kb_de_us")) layout = "de,us";
            else if (!strcmp(id, "kb_fr_us")) layout = "fr,us";
            else if (!strcmp(id, "kb_kr_us")) layout = "kr,us";
            break;
        }
    }
    if (!layout) return;
    apply_keyboard_layout(layout);
    settings_mark_kb(layout);
    settings_save();
    toast_show("Keyboard", layout, 2.0);
}

static void apply_toolkit_session_env(void) {
    if (!getenv("LANG") || !getenv("LANG")[0])
        setenv("LANG", "ja_JP.UTF-8", 0);
    /* Pure Wayland session — force overwrite.  Console logins leave
     * XDG_SESSION_TYPE=tty from logind; Chrome then picks ozone/x11 and
     * dies with "Missing X server or $DISPLAY" unless --ozone-platform is
     * passed.  ${VAR:-wayland} / setenv(..., 0) do NOT clear tty. */
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    /* Not GNOME: portals map org.gnome.desktop.wm.preferences button-layout
     * (GNOME default appmenu:close) onto gtk-decoration-layout and hide
     * minimize/maximize in CSD HeaderBars. */
    if (!getenv("XDG_CURRENT_DESKTOP"))
        setenv("XDG_CURRENT_DESKTOP", "Luna", 0);
    if (!getenv("XDG_SESSION_DESKTOP"))
        setenv("XDG_SESSION_DESKTOP", "luna", 0);
    if (!getenv("GDK_BACKEND"))
        setenv("GDK_BACKEND", "wayland", 0);
    if (!getenv("MOZ_ENABLE_WAYLAND"))
        setenv("MOZ_ENABLE_WAYLAND", "1", 0);
    if (!getenv("MOZ_LAYERS_ALLOW_SOFTWARE_GL"))
        setenv("MOZ_LAYERS_ALLOW_SOFTWARE_GL", "1", 0);
    if (!getenv("QT_QPA_PLATFORM"))
        setenv("QT_QPA_PLATFORM", "wayland", 0);
    if (!getenv("GDK_SCALE"))
        setenv("GDK_SCALE", "1", 0);
    if (!getenv("GDK_DPI_SCALE"))
        setenv("GDK_DPI_SCALE", "1", 0);
    if (!getenv("QT_SCALE_FACTOR"))
        setenv("QT_SCALE_FACTOR", "1", 0);
    /* Berry often exports GSK_RENDERER=vulkan.  Luna has no Vulkan WSI /
     * dmabuf path for clients — GTK4 then stalls and Firefox's crash /
     * Troubleshoot dialogs never paint.  Prefer Cairo (CPU) unless the
     * user explicitly set something other than vulkan. */
    {
        const char* gsk = getenv("GSK_RENDERER");
        if (!gsk || !*gsk || !strcasecmp(gsk, "vulkan"))
            setenv("GSK_RENDERER", "cairo", 1);
    }
    /* GTK CSD WindowControls (minimize/maximize/close) when portals absent. */
    {
        const char* layout = getenv("LUNA_GTK_BUTTON_LAYOUT");
        if (!layout || !*layout)
            layout = "icon:minimize,maximize,close";
        static const char* vers[] = { "gtk-3.0", "gtk-4.0", NULL };
        const char* home = getenv("HOME");
        if (!home || !*home) home = "/root";
        for (int i = 0; vers[i]; i++) {
            char dir[512], ini[576];
            snprintf(dir, sizeof(dir), "%s/.config/%s", home, vers[i]);
            snprintf(ini, sizeof(ini), "%s/settings.ini", dir);
            mkdir(dir, 0755);
            if (access(ini, F_OK) != 0) {
                FILE* f = fopen(ini, "w");
                if (f) {
                    fprintf(f, "[Settings]\ngtk-decoration-layout=%s\n", layout);
                    fclose(f);
                }
            }
        }
    }

    if (luna_im_use_gim()) {
        const char* gtk_im = getenv("GTK_IM_MODULE");
        const char* qt_im = getenv("QT_IM_MODULE");
        if (!gtk_im || !*gtk_im || !strcmp(gtk_im, "wayland") ||
            !strcmp(gtk_im, "none") || !strcmp(gtk_im, "simple"))
            setenv("GTK_IM_MODULE", "gim", 1);
        if (!qt_im || !*qt_im || !strcmp(qt_im, "wayland") ||
            !strcmp(qt_im, "none"))
            setenv("QT_IM_MODULE", "gim", 1);
        if (!getenv("QT4_IM_MODULE"))
            setenv("QT4_IM_MODULE", "whiz", 0);
        if (!getenv("CLUTTER_IM_MODULE"))
            setenv("CLUTTER_IM_MODULE", "whiz", 0);
        if (!getenv("XMODIFIERS"))
            setenv("XMODIFIERS", "@im=whiz", 0);
    } else {
        setenv("GTK_IM_MODULE", "wayland", 1);
        unsetenv("QT_IM_MODULE");
        unsetenv("QT4_IM_MODULE");
        unsetenv("CLUTTER_IM_MODULE");
        unsetenv("XMODIFIERS");
    }
}

static void child_session_env(void) {
    const char* preload = getenv("LUNA_WAYLAND_CLIENT_PRELOAD");
    const char* libpath = getenv("LUNA_WAYLAND_CLIENT_LIBPATH");
    /* Only preload the vendored client when the session explicitly asked
     * (LUNA_WAYLAND_CLIENT_PRELOAD).  Auto-loading /usr/local/lib/luna breaks
     * Mesa EGL in Firefox (prepare_read/queue stubs → FEATURE_FAILURE_NO_DISPLAY).
     * luna-session defaults to system libwayland for that reason. */
    /* Prefer a real XCursor theme GTK can load.  Forcing "aero" when the
     * theme is missing leaves clients with a NULL cursor glyph. */
    if (!getenv("XCURSOR_THEME")) {
        if (access("/usr/share/icons/aero/cursors/left_ptr", R_OK) == 0)
            setenv("XCURSOR_THEME", "aero", 0);
        else if (access("/usr/share/icons/Adwaita/cursors/left_ptr", R_OK) == 0)
            setenv("XCURSOR_THEME", "Adwaita", 0);
    }
    if (!getenv("XCURSOR_SIZE"))
        setenv("XCURSOR_SIZE", "24", 0);

    apply_xkb_session_env();
    apply_toolkit_session_env();

    /* Compositor holds DRM master — client GBM/EGL sees fd=-1 and Firefox
     * stalls in WaitFlushedEvent.  Force llvmpipe for child processes only
     * (luna-shell itself is started without this via luna-session). */
    if (!getenv("LIBGL_ALWAYS_SOFTWARE"))
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    if (!getenv("MESA_LOADER_DRIVER_OVERRIDE"))
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe", 0);
    if (!getenv("MOZ_WEBRENDER"))
        setenv("MOZ_WEBRENDER", "0", 0);
    if (!getenv("MOZ_ACCELERATED"))
        setenv("MOZ_ACCELERATED", "0", 0);
    /* Belt-and-suspenders for Mesa software path (Firefox WaitFlushedEvent). */
    if (!getenv("GALLIUM_DRIVER"))
        setenv("GALLIUM_DRIVER", "llvmpipe", 0);

    if (libpath && *libpath) {
        const char* existing = getenv("LD_LIBRARY_PATH");
        char buf[1024];
        if (existing && *existing)
            snprintf(buf, sizeof(buf), "%s:%s", libpath, existing);
        else
            snprintf(buf, sizeof(buf), "%s", libpath);
        setenv("LD_LIBRARY_PATH", buf, 1);
    }
    if (preload && *preload)
        setenv("LD_PRELOAD", preload, 1);
    /* Firefox needs an explicit Wayland opt-in on some builds. */
    if (!getenv("MOZ_ENABLE_WAYLAND"))
        setenv("MOZ_ENABLE_WAYLAND", "1", 0);
    if (!getenv("GDK_BACKEND"))
        setenv("GDK_BACKEND", "wayland", 0);
}

/* Return 1 if a process whose /proc/pid/comm matches `name` is alive. */
static int helper_comm_running(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        char* end = NULL;
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
        (void)strtoul(ent->d_name, &end, 10);
        if (!end || *end) continue;
        /* d_name can be up to NAME_MAX bytes; leave room for /proc//comm. */
        char path[320], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            size_t n = strlen(comm);
            while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r'))
                comm[--n] = 0;
            if (!strcmp(comm, name)) found = 1;
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

static void spawn_session_helper(const char* cmd) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        child_session_env();
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            if (fd > 2) close(fd);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
}

/* When luna-shell is started under WAYLAND_DISPLAY without luna-session
 * (or helpers exited), bring up whiz-im-wayland + luna-clipboard. */
static void ensure_wayland_helpers(void) {
    if (!getenv("WAYLAND_DISPLAY"))
        return;
    const char* no = getenv("LUNA_NO_HELPERS");
    if (no && (!strcmp(no, "1") || !strcmp(no, "yes") || !strcmp(no, "true")))
        return;

    if (!luna_im_use_gim()) {
        const char* im = getenv("LUNA_INPUT_METHOD");
        if (!im || !*im) im = "whiz-im-wayland";
        if (strcmp(im, "none") && strcmp(im, "0")) {
            const char* base = strrchr(im, '/');
            base = base ? base + 1 : im;
            if (!helper_comm_running(base)) {
                fprintf(stderr, "[luna-shell] starting input method (%s)\n", im);
                spawn_session_helper(im);
            }
        }
    }

    {
        const char* clip = getenv("LUNA_CLIPBOARD");
        if (!clip || !*clip) clip = "luna-clipboard";
        if (strcmp(clip, "none") && strcmp(clip, "0")) {
            const char* base = strrchr(clip, '/');
            base = base ? base + 1 : clip;
            if (!helper_comm_running(base)) {
                fprintf(stderr, "[luna-shell] starting clipboard manager (%s)\n", clip);
                spawn_session_helper(clip);
            }
        }
    }
}

static LunaApp* resolve_app(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        const char* key = NULL;
        if (!strncmp(id, "dock_", 5)) key = id + 5;
        else if (!strncmp(id, "lp_", 3)) key = id + 3;
        if (key) {
            for (int i = 0; i < APP_COUNT; i++)
                if (!strcmp(g_apps[i].key, key)) return &g_apps[i];
        }
    }
    return NULL;
}

static void app_set_dot(LunaApp* app, int running) {
    char dot_id[64];
    snprintf(dot_id, sizeof(dot_id), "dot_%s", app->key);
    int idx = luna_get_element_by_id(dot_id);
    if (idx != -1) luna_element_at(idx)->opacity = running ? 1.0f : 0.0f;
}

static void app_launch(LunaApp* app) {
    const char* cmd = app->cmd[0] ? app->cmd : app->default_cmd;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        child_session_env();
        /* Vendored libwayland stubs break Mesa's prepare_read in Firefox/Chrome. */
        if (is_browser_cmd(app, cmd)) {
            unsetenv("LD_PRELOAD");
            unsetenv("LUNA_WAYLAND_CLIENT_PRELOAD");
            sanitize_browser_ld_library_path();
            unsetenv("LUNA_WAYLAND_CLIENT_LIBPATH");
            setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            setenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe", 1);
            setenv("MOZ_WEBRENDER", "0", 1);
            setenv("MOZ_ACCELERATED", "0", 1);
            setenv("MOZ_ENABLE_WAYLAND", "1", 1);
            setenv("GALLIUM_DRIVER", "llvmpipe", 1);
            /* Belt-and-suspenders: console may still have tty/vulkan. */
            setenv("XDG_SESSION_TYPE", "wayland", 1);
            setenv("GSK_RENDERER", "cairo", 1);
            /* Prefer Firefox CSD chrome; compositor still offers SSD when asked.
             * Soft-disable HW video decode paths that stall under DRM master. */
            setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 0);
            /* Future HW toggle: LUNA_BROWSER_HW=1 keeps browser GPU path on. */
            if (getenv("LUNA_BROWSER_HW") &&
                (!strcmp(getenv("LUNA_BROWSER_HW"), "1") ||
                 !strcmp(getenv("LUNA_BROWSER_HW"), "true") ||
                 !strcmp(getenv("LUNA_BROWSER_HW"), "yes"))) {
                unsetenv("LIBGL_ALWAYS_SOFTWARE");
                unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
                unsetenv("GALLIUM_DRIVER");
                unsetenv("MOZ_WEBRENDER");
                unsetenv("MOZ_ACCELERATED");
            }
        }
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        app->pid = pid;
        app_set_dot(app, 1);
        char msg[320];
        snprintf(msg, sizeof(msg), "Starting \"%s\"", cmd);
        toast_show(app->name, msg, 4.0);
        fprintf(stderr, "[luna-shell] launch %s: %s (pid %d)\n", app->name, cmd, (int)pid);
    }
}

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < APP_COUNT; i++) {
            if (g_apps[i].pid == pid) {
                g_apps[i].pid = 0;
                app_set_dot(&g_apps[i], 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                    toast_show(g_apps[i].name, "App not installed (LUNA_APP_*)", 5.0);
            }
        }
    }
}

static void spawn_command(const char* cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        child_session_env();
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
}

/* ── Overlays: Luna menu / Control Center / Launchpad ── */

static void dismiss_luna_menu(int trap_idx) {
    (void)trap_idx;
    if (!is_shown(g_luna_menu_idx)) return;
    set_hidden(g_luna_menu_idx, 1);
}

static void dismiss_cc(int trap_idx) {
    (void)trap_idx;
    if (!is_shown(g_cc_idx)) return;
    set_hidden(g_cc_idx, 1);
}

static void dismiss_win_menu(void) {
    if (is_shown(g_win_menu_idx)) set_hidden(g_win_menu_idx, 1);
    g_win_menu_target = 0;
}

static void dismiss_clip_menu(void) {
    if (is_shown(g_clip_menu_idx)) set_hidden(g_clip_menu_idx, 1);
}

static void dismiss_wifi_menu(void) {
    if (is_shown(g_wifi_menu_idx)) set_hidden(g_wifi_menu_idx, 1);
    g_wifi_selected = -1;
    set_hidden(luna_get_element_by_id("wifi_credentials"), 1);
}

static void dismiss_popovers(void) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
}

static void on_luna_menu(LunaElement* e) {
    (void)e;
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    if (is_shown(g_luna_menu_idx)) { dismiss_luna_menu(g_luna_menu_idx); return; }
    set_hidden(g_luna_menu_idx, 0);
}

static void on_control_center(LunaElement* e) {
    (void)e;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    if (is_shown(g_cc_idx)) { dismiss_cc(g_cc_idx); return; }
    set_hidden(g_cc_idx, 0);
    position_control_center();
}

static void on_wifi_menu(LunaElement* e) {
    (void)e;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    if (is_shown(g_wifi_menu_idx)) { dismiss_wifi_menu(); return; }
    wifi_refresh();
    wifi_update_ui();
    set_hidden(g_wifi_menu_idx, 0);
    position_menu_near(g_wifi_menu_idx, g_mb_wifi_idx, luna_window_width - 330.0f);
}

static void on_wifi_power(LunaElement* e) {
    (void)e;
    if (g_wifi_backend == WIFI_CONNMAN) {
        const char* const argv[] = { "connmanctl", g_wifi_powered ? "disable" : "enable", "wifi", NULL };
        wifi_exec(argv);
    } else if (g_wifi_backend == WIFI_NMCLI) {
        const char* const argv[] = { "nmcli", "radio", "wifi", g_wifi_powered ? "off" : "on", NULL };
        wifi_exec(argv);
    }
    g_wifi_powered = !g_wifi_powered;
    wifi_update_ui();
}

static int wifi_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!strncmp(id, "wifi_", 5) && isdigit((unsigned char)id[5])) return atoi(id + 5);
    }
    return -1;
}

static void on_wifi_network(LunaElement* e) {
    int i = wifi_row_number(e);
    if (i < 0 || i >= g_wifi_count) return;
    WifiNetwork* n = &g_wifi_networks[i];
    if (n->connected) {
        if (g_wifi_backend == WIFI_CONNMAN) {
            const char* const argv[] = { "connmanctl", "disconnect", n->id, NULL }; wifi_exec(argv);
        } else {
            const char* const argv[] = { "nmcli", "connection", "down", "id", n->name, NULL }; wifi_exec(argv);
        }
        toast_show("Wi-Fi", "Disconnecting...", 2.0);
        dismiss_wifi_menu();
        return;
    }
    g_wifi_selected = i;
    int label = luna_get_element_by_id("wifi_selected");
    if (label >= 0) { char text[150]; snprintf(text, sizeof(text), "Connect to %s", n->name); luna_set_text(label, text); }
    int pass = luna_get_element_by_id("wifi_password");
    if (pass >= 0) luna_set_value(pass, "");
    set_hidden(luna_get_element_by_id("wifi_credentials"), 0);
    luna_mark_layout_dirty();
}

static void on_wifi_connect(LunaElement* e) {
    (void)e;
    if (g_wifi_selected < 0 || g_wifi_selected >= g_wifi_count) return;
    WifiNetwork* n = &g_wifi_networks[g_wifi_selected];
    int pi = luna_get_element_by_id("wifi_password");
    const char* password = pi >= 0 ? luna_get_value(pi) : "";
    if (g_wifi_backend == WIFI_CONNMAN) {
        if (!wifi_connman_connect(n->id, password ? password : "")) {
            toast_show("Wi-Fi", "Could not start connection", 2.0);
            return;
        }
    } else if (g_wifi_backend == WIFI_NMCLI) {
        if (password && *password) {
            const char* const connect[] = { "nmcli", "device", "wifi", "connect", n->name, "password", password, NULL }; wifi_exec(connect);
        } else {
            const char* const connect[] = { "nmcli", "device", "wifi", "connect", n->name, NULL }; wifi_exec(connect);
        }
    }
    if (pi >= 0) luna_set_value(pi, "");
    toast_show("Wi-Fi", "Connecting...", 2.0);
    dismiss_wifi_menu();
}

static void on_wifi_scan(LunaElement* e) {
    (void)e;
    if (g_wifi_backend == WIFI_CONNMAN) {
        const char* const argv[] = { "connmanctl", "scan", "wifi", NULL }; wifi_exec(argv);
    } else if (g_wifi_backend == WIFI_NMCLI) {
        const char* const argv[] = { "nmcli", "device", "wifi", "rescan", NULL }; wifi_exec(argv);
    }
    toast_show("Wi-Fi", "Scanning for networks...", 2.0);
}

static void launchpad_close(void) {
    if (!is_shown(g_launchpad_idx)) return;
    set_hidden(g_launchpad_idx, 1);
}

static void on_launchpad_open(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    set_hidden(g_launchpad_idx, 0);
}

static void on_launchpad_close(LunaElement* e) {
    (void)e;
    launchpad_close();
}

static void on_launch_app(LunaElement* e) {
    LunaApp* app = resolve_app(e);
    launchpad_close();
    if (app) app_launch(app);
}

static void on_dock_click(LunaElement* e) {
    LunaApp* app = resolve_app(e);
    if (!app) return;
    int force_launch = (luna_last_click_mods() & LUNA_MOD_ALT) != 0;
    LunaWinEntry* w = force_launch ? NULL : find_win_for_app(app);
    if (!w) {
        app_launch(app);
        return;
    }
    char cmd[64];
    if (w->focused && !w->minimized)
        snprintf(cmd, sizeof(cmd), "minimize %u", w->id);
    else
        snprintf(cmd, sizeof(cmd), "activate %u", w->id);
    shell_send_cmd(cmd);
}

static void on_trash(LunaElement* e) {
    (void)e;
    toast_show("Trash", "Trash is empty.", 3.0);
}

/* Dismiss popovers when clicking outside them. */
static int hit_inside(int hit, int root) {
    for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx)
        if (p == root) return 1;
    return 0;
}

static void on_mouse_release_hook(int hit, int drag_moved) {
    if (drag_moved || hit < 0) return;
    if (is_shown(g_luna_menu_idx) &&
        !hit_inside(hit, g_luna_menu_idx) &&
        !hit_inside(hit, g_mb_logo_idx))
        dismiss_luna_menu(g_luna_menu_idx);
    if (is_shown(g_cc_idx) &&
        !hit_inside(hit, g_cc_idx) &&
        !hit_inside(hit, g_mb_cc_idx) &&
        !hit_inside(hit, g_mb_wifi_idx))
        dismiss_cc(g_cc_idx);
    if (is_shown(g_wifi_menu_idx) &&
        !hit_inside(hit, g_wifi_menu_idx) &&
        !hit_inside(hit, g_mb_wifi_idx))
        dismiss_wifi_menu();
    if (is_shown(g_win_menu_idx) && !hit_inside(hit, g_win_menu_idx)) {
        /* Keep open when the click was on a win_item (handler opens/repositions). */
        int on_win = 0;
        for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx) {
            const char* id = luna_element_at(p)->id;
            if (id[0]=='w' && id[1]=='i' && id[2]=='n' && id[3]=='_') { on_win = 1; break; }
        }
        if (!on_win) dismiss_win_menu();
    }
    if (is_shown(g_clip_menu_idx) &&
        !hit_inside(hit, g_clip_menu_idx) &&
        !hit_inside(hit, g_mb_clip_idx))
        dismiss_clip_menu();
}

/* ── About window ── */

static void on_about(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    set_hidden(g_about_idx, 0);
    center_element(g_about_box_idx >= 0 ? g_about_box_idx : g_about_idx);
}

static void on_about_close(LunaElement* e) {
    (void)e;
    set_hidden(g_about_idx, 1);
    g_about_sheet_max = 0;
}

static void sheet_toggle_maximize(int box_idx, int* max_flag) {
    if (box_idx < 0 || !max_flag) return;
    LunaElement* e = luna_element_at(box_idx);
    if (*max_flag) {
        *max_flag = 0;
        e->has_css_width = 0;
        e->has_css_height = 0;
        center_element(box_idx);
        return;
    }
    *max_flag = 1;
    float margin = 24.0f;
    float top = 36.0f;
    e->rel_x = margin;
    e->rel_y = top;
    e->css_width = luna_window_width - margin * 2.0f;
    e->css_height = luna_window_height - top - margin;
    e->has_css_width = 1;
    e->has_css_height = 1;
    e->pos_overridden_x = 1;
    e->pos_overridden_y = 1;
    e->pct_left = 0;
    e->pct_top = 0;
    e->has_left = 1;
    e->has_top = 1;
    e->raw_left = e->rel_x;
    e->raw_top = e->rel_y;
    luna_mark_layout_dirty();
}

static void on_about_min(LunaElement* e) {
    (void)e;
    on_about_close(NULL);
}

static void on_about_max(LunaElement* e) {
    (void)e;
    int box = g_about_box_idx >= 0 ? g_about_box_idx : g_about_idx;
    sheet_toggle_maximize(box, &g_about_sheet_max);
}

/* ── Settings dialog ── */

static void apply_wallpaper(const char* theme) {
    int idx = luna_get_element_by_id("wallpaper");
    if (idx < 0) return;
    /* Remove all theme classes; night is the default gradient (no extra class). */
    luna_remove_class(idx, "ocean");
    luna_remove_class(idx, "forest");
    luna_remove_class(idx, "sunset");
    if (strcmp(theme, "night") != 0)
        luna_add_class(idx, theme);
    luna_update_element_style(idx);
    luna_mark_layout_dirty();
}

static void settings_mark_wallpaper(const char* theme) {
    const char* ids[] = { "wp_night", "wp_ocean", "wp_forest", "wp_sunset" };
    for (int i = 0; i < 4; i++) {
        int ti = luna_get_element_by_id(ids[i]);
        if (ti < 0) continue;
        if (!strcmp(ids[i] + 3, theme)) /* "wp_night"+3 = "night" */
            luna_add_class(ti, "selected");
        else
            luna_remove_class(ti, "selected");
        luna_update_element_style(ti);
    }
    luna_mark_layout_dirty();
}

static void settings_mark_cursor(const char* theme) {
    const char* ids[] = { "cur_aero", "cur_miku" };
    const char* names[] = { "aero", "miku" };
    const char* sel = theme && *theme ? theme : "aero";
    /* "builtin" / "none" map to the embedded Aero set. */
    if (!strcmp(sel, "builtin") || !strcmp(sel, "none") || !strcmp(sel, "default-vector"))
        sel = "aero";
    for (int i = 0; i < 2; i++) {
        int ti = luna_get_element_by_id(ids[i]);
        if (ti < 0) continue;
        if (!strcmp(names[i], sel))
            luna_add_class(ti, "selected");
        else
            luna_remove_class(ti, "selected");
        luna_update_element_style(ti);
    }
    luna_mark_layout_dirty();
}

static void settings_mark_toggle(const char* id, int enabled) {
    int idx = luna_get_element_by_id(id);
    if (idx < 0) return;
    if (enabled) luna_add_class(idx, "on");
    else luna_remove_class(idx, "on");
    luna_update_element_style(idx);
}

static void settings_mark_gap(void) {
    const int gaps[] = { 0, 8, 16 };
    for (int i = 0; i < 3; i++) {
        char id[24];
        snprintf(id, sizeof(id), "wm_gap_%d", gaps[i]);
        int idx = luna_get_element_by_id(id);
        if (idx < 0) continue;
        if (g_settings.window_gap == gaps[i]) luna_add_class(idx, "selected");
        else luna_remove_class(idx, "selected");
        luna_update_element_style(idx);
    }
}

static void apply_wm_settings(void) {
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "wm_config gap %d", g_settings.window_gap);
    shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config edge_snap %d", g_settings.edge_snap);
    shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_double_click %d", g_settings.titlebar_double_click);
    shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config super_shortcuts %d", g_settings.super_shortcuts);
    shell_send_cmd(cmd);
}

static void settings_populate_ui(void) {
    /* Fill app command inputs */
    for (int i = 0; i < APP_COUNT; i++) {
        char input_id[64];
        snprintf(input_id, sizeof(input_id), "pref_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(input_id);
        if (idx >= 0) luna_set_value(idx, g_apps[i].cmd);
    }
    /* Hostname */
    int h = luna_get_element_by_id("pref_hostname");
    if (h >= 0) luna_set_value(h, g_settings.hostname);
    /* Wallpaper selection markers */
    settings_mark_wallpaper(g_settings.wallpaper);
    settings_mark_cursor(g_settings.cursor_theme);
    settings_mark_kb(g_settings.kb_layout);
    settings_mark_toggle("wm_snap", g_settings.edge_snap);
    settings_mark_toggle("wm_double_click", g_settings.titlebar_double_click);
    settings_mark_toggle("wm_shortcuts", g_settings.super_shortcuts);
    settings_mark_toggle("wm_dock_mag", g_settings.dock_magnification);
    settings_mark_toggle("wm_restore", g_settings.session_restore);
    settings_mark_gap();
    /* Show apps tab by default */
    set_hidden(g_settings_panel_apps, 0);
    set_hidden(g_settings_panel_disp, 1);
    set_hidden(g_settings_panel_wm, 1);
    if (g_stab_apps_idx >= 0) { luna_add_class(g_stab_apps_idx, "active"); luna_update_element_style(g_stab_apps_idx); }
    if (g_stab_disp_idx  >= 0) { luna_remove_class(g_stab_disp_idx, "active"); luna_update_element_style(g_stab_disp_idx); }
    if (g_stab_wm_idx    >= 0) { luna_remove_class(g_stab_wm_idx, "active"); luna_update_element_style(g_stab_wm_idx); }
}

static void on_settings_open(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    settings_populate_ui();
    set_hidden(g_settings_idx, 0);
    center_element(g_settings_sheet_idx >= 0 ? g_settings_sheet_idx : g_settings_idx);
}

static void on_settings_close(LunaElement* e) {
    (void)e;
    set_hidden(g_settings_idx, 1);
    g_settings_sheet_max = 0;
}

static void on_settings_min(LunaElement* e) {
    (void)e;
    on_settings_close(NULL);
}

static void on_settings_max(LunaElement* e) {
    (void)e;
    int box = g_settings_sheet_idx >= 0 ? g_settings_sheet_idx : g_settings_idx;
    sheet_toggle_maximize(box, &g_settings_sheet_max);
}

static void on_settings_save(LunaElement* e) {
    (void)e;
    /* Read app commands back from inputs */
    for (int i = 0; i < APP_COUNT; i++) {
        char input_id[64];
        snprintf(input_id, sizeof(input_id), "pref_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(input_id);
        if (idx >= 0) {
            const char* v = luna_get_value(idx);
            if (v && *v) snprintf(g_apps[i].cmd, sizeof(g_apps[i].cmd), "%s", v);
        }
    }
    /* Hostname */
    int h = luna_get_element_by_id("pref_hostname");
    if (h >= 0) {
        const char* v = luna_get_value(h);
        if (v && *v) snprintf(g_settings.hostname, sizeof(g_settings.hostname), "%s", v);
    }
    settings_save();
    apply_wallpaper(g_settings.wallpaper);
    cursor_theme_reload(g_settings.cursor_theme);
    apply_keyboard_layout(g_settings.kb_layout);
    apply_wm_settings();
    g_cursor_reload_pending = 1;
    set_hidden(g_settings_idx, 1);
    toast_show("Settings", "Settings saved successfully.", 3.0);
}

static void on_settings_tab(LunaElement* e) {
    int idx = elem_idx_of(e);
    /* Walk up to find a .stab element */
    int tab_idx = -1;
    for (int i = idx; i >= 0; i = luna_element_at(i)->parent_idx) {
        if (strstr(luna_element_at(i)->class_name, "stab") &&
            luna_element_at(i)->id[0]) { tab_idx = i; break; }
    }
    if (tab_idx < 0) return;
    const char* id = luna_element_at(tab_idx)->id;
    int is_apps = !strcmp(id, "stab_apps");
    int is_disp = !strcmp(id, "stab_disp");
    int is_wm = !strcmp(id, "stab_wm");

    if (g_stab_apps_idx >= 0) {
        if (is_apps) luna_add_class(g_stab_apps_idx, "active");
        else luna_remove_class(g_stab_apps_idx, "active");
        luna_update_element_style(g_stab_apps_idx);
    }
    if (g_stab_disp_idx >= 0) {
        if (is_disp) luna_add_class(g_stab_disp_idx, "active");
        else luna_remove_class(g_stab_disp_idx, "active");
        luna_update_element_style(g_stab_disp_idx);
    }
    if (g_stab_wm_idx >= 0) {
        if (is_wm) luna_add_class(g_stab_wm_idx, "active");
        else luna_remove_class(g_stab_wm_idx, "active");
        luna_update_element_style(g_stab_wm_idx);
    }
    set_hidden(g_settings_panel_apps, !is_apps);
    set_hidden(g_settings_panel_disp, !is_disp);
    set_hidden(g_settings_panel_wm, !is_wm);
}

static void on_wm_toggle(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (cand && !strncmp(cand, "wm_", 3)) { id = cand; break; }
    }
    if (!id) return;
    int* value = NULL;
    if (!strcmp(id, "wm_snap")) value = &g_settings.edge_snap;
    else if (!strcmp(id, "wm_double_click")) value = &g_settings.titlebar_double_click;
    else if (!strcmp(id, "wm_shortcuts")) value = &g_settings.super_shortcuts;
    else if (!strcmp(id, "wm_dock_mag")) value = &g_settings.dock_magnification;
    else if (!strcmp(id, "wm_restore")) value = &g_settings.session_restore;
    if (!value) return;
    *value = !*value;
    settings_mark_toggle(id, *value);
    apply_wm_settings();
    settings_save();
    shell_request_repaint(-1);
}

static void on_wm_gap(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (cand && !strncmp(cand, "wm_gap_", 7)) { id = cand; break; }
    }
    if (!id) return;
    int gap = atoi(id + 7);
    if (gap != 0 && gap != 8 && gap != 16) return;
    g_settings.window_gap = gap;
    settings_mark_gap();
    apply_wm_settings();
    settings_save();
}

static void on_wallpaper_select(LunaElement* e) {
    int idx = elem_idx_of(e);
    const char* theme = NULL;
    for (int i = idx; i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (id[0] == 'w' && id[1] == 'p' && id[2] == '_' && id[3]) { theme = id + 3; break; }
    }
    if (!theme) return;
    snprintf(g_settings.wallpaper, sizeof(g_settings.wallpaper), "%s", theme);
    settings_mark_wallpaper(theme);
    apply_wallpaper(theme);
}

static void on_cursor_select(LunaElement* e) {
    int idx = elem_idx_of(e);
    const char* theme = NULL;
    for (int i = idx; i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!id || !id[0]) continue;
        if (id[0] == 'c' && id[1] == 'u' && id[2] == 'r' && id[3] == '_' && id[4]) {
            theme = id + 4;
            break;
        }
    }
    if (!theme) return;
    snprintf(g_settings.cursor_theme, sizeof(g_settings.cursor_theme), "%s", theme);
    settings_mark_cursor(theme);
    cursor_theme_reload(theme);
    g_cursor_reload_pending = 1;
    settings_save();
    char msg[80];
    snprintf(msg, sizeof(msg), "Theme: %s", theme);
    toast_show("Cursor", msg, 2.0);
}

/* ── Power / session actions with confirmation ── */

static void on_restart(LunaElement* e);
static void on_shutdown(LunaElement* e);
static void on_logout(LunaElement* e);

static void confirm_open(int action) {
    dismiss_popovers();
    g_pending_action = action;
    int t  = luna_get_element_by_id("confirm_title");
    int m  = luna_get_element_by_id("confirm_msg");
    int ok = luna_get_element_by_id("confirm_ok");
    int icon = luna_get_element_by_id("confirm_icon");
    int danger = (action == ACT_SHUTDOWN || action == ACT_RESTART);
    if (ok >= 0) {
        if (danger) { luna_add_class(ok, "danger"); luna_remove_class(ok, "primary"); }
        else         { luna_remove_class(ok, "danger"); luna_add_class(ok, "primary"); }
        luna_update_element_style(ok);
    }
    switch (action) {
    case ACT_SHUTDOWN:
        if (t >= 0)  luna_set_text(t,  "Shut Down?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close. Save your work before continuing.");
        if (ok >= 0) luna_set_text(ok, "Shut Down");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x80\x91"); /* power-off, U+F011 */
            luna_remove_class(icon, "restart");
            luna_remove_class(icon, "logout");
            luna_update_element_style(icon);
        }
        break;
    case ACT_RESTART:
        if (t >= 0)  luna_set_text(t,  "Restart?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close, then the computer will restart.");
        if (ok >= 0) luna_set_text(ok, "Restart");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x80\xa1"); /* rotate, U+F021 */
            luna_add_class(icon, "restart");
            luna_remove_class(icon, "logout");
            luna_update_element_style(icon);
        }
        break;
    case ACT_LOGOUT:
        if (t >= 0)  luna_set_text(t,  "Log Out?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close and the session will end.");
        if (ok >= 0) luna_set_text(ok, "Log Out");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x82\x8b"); /* sign-out, U+F08B */
            luna_remove_class(icon, "restart");
            luna_add_class(icon, "logout");
            luna_update_element_style(icon);
        }
        break;
    }
    set_hidden(g_confirm_idx, 0);
    center_element(g_confirm_box_idx >= 0 ? g_confirm_box_idx : g_confirm_idx);
}

static void on_shutdown(LunaElement* e) { (void)e; confirm_open(ACT_SHUTDOWN); }
static void on_restart(LunaElement* e)  { (void)e; confirm_open(ACT_RESTART); }
static void on_logout(LunaElement* e)   { (void)e; confirm_open(ACT_LOGOUT); }

static void on_confirm_cancel(LunaElement* e) {
    (void)e;
    g_pending_action = ACT_NONE;
    set_hidden(g_confirm_idx, 1);
}

static void on_confirm_ok(LunaElement* e) {
    (void)e;
    int action = g_pending_action;
    g_pending_action = ACT_NONE;
    set_hidden(g_confirm_idx, 1);
    /* Persist open dock apps before leaving / powering off. */
    session_save();
    switch (action) {
    case ACT_SHUTDOWN: spawn_command("systemctl poweroff"); break;
    case ACT_RESTART:  spawn_command("systemctl reboot");  break;
    case ACT_LOGOUT:   g_should_close = 1; break;
    }
}

/* ── Control Center toggles & sliders ── */

static void on_cc_toggle(LunaElement* e) {
    int idx = -1;
    for (int i = elem_idx_of(e); i != -1; i = luna_element_at(i)->parent_idx) {
        if (strstr(luna_element_at(i)->class_name, "cc_toggle")) { idx = i; break; }
    }
    if (idx == -1) return;
    LunaElement* t = luna_element_at(idx);
    int now_on = strstr(t->class_name, "on") == NULL;
    if (now_on) luna_add_class(idx, "on");
    else         luna_remove_class(idx, "on");
    /* Keep the knob's layout position stable and let the .on CSS transform
     * provide the entire animation.  Overriding rel_x here used to add a
     * second 18 px movement on top of translateX(), and made the transformed
     * child jump out from under the pointer during release hit-testing. */
    luna_update_element_style(idx);
    luna_mark_layout_dirty();
    if (!strcmp(t->id, "cc_wifi")) {
        wifi_refresh();
        on_wifi_power(e);
    }
}

static void slider_tick(const char* thumb_id, const char* fill_id, const char* track_id) {
    int ti = luna_get_element_by_id(thumb_id);
    int fi = luna_get_element_by_id(fill_id);
    int ki = luna_get_element_by_id(track_id);
    if (ti == -1 || fi == -1 || ki == -1) return;
    LunaElement* th = luna_element_at(ti);
    LunaElement* tr = luna_element_at(ki);
    float track_w = tr->w > 0 ? tr->w : 268.0f;
    float max_x = track_w - 19.0f;
    if (th->rel_x < 1.0f)    th->rel_x = 1.0f;
    if (th->rel_x > max_x)   th->rel_x = max_x;
    th->rel_y = 1.0f;
    th->pos_overridden_x = 1;
    th->pos_overridden_y = 1;
    LunaElement* fill = luna_element_at(fi);
    fill->css_width   = th->rel_x + 9.0f;
    fill->has_css_width = 1;
}

/* ── System status: clock, CPU, memory, disk, battery, network ── */

static float read_cpu_percent(void) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0.0f;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);
    fclose(f);
    if (n < 4) return 0.0f;
    unsigned long long idle  = v[3] + v[4];
    unsigned long long total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    unsigned long long didle  = idle  - g_cpu_prev_idle;
    unsigned long long dtotal = total - g_cpu_prev_total;
    g_cpu_prev_idle  = idle;
    g_cpu_prev_total = total;
    if (dtotal == 0) return 0.0f;
    return 100.0f * (float)(dtotal - didle) / (float)dtotal;
}

static int read_mem_percent(unsigned long* total_kb_out) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    unsigned long total = 0, avail = 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %lu kB",     &total);
        sscanf(line, "MemAvailable: %lu kB", &avail);
    }
    fclose(f);
    if (total_kb_out) *total_kb_out = total;
    if (!total) return 0;
    return (int)(100.0 * (double)(total - avail) / (double)total);
}

static int read_battery_percent(void) {
    static const char* paths[] = {
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/BAT1/capacity",
        "/sys/class/power_supply/BAT2/capacity",
    };
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        int pct = -1;
        if (fscanf(f, "%d", &pct) != 1) pct = -1;
        fclose(f);
        if (pct >= 0) return pct;
    }
    return -1;
}

static const char* read_net_status(void) {
    DIR* d = opendir("/sys/class/net");
    if (!d) return "Offline";
    struct dirent* de;
    int wired_up = 0, wifi_up = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo")) continue;
        char path[300];
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", de->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char state[24] = "";
        if (!fgets(state, sizeof(state), f)) state[0] = 0;
        fclose(f);
        if (strncmp(state, "up", 2)) continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", de->d_name);
        struct stat st;
        if (stat(path, &st) == 0) wifi_up = 1;
        else wired_up = 1;
    }
    closedir(d);
    if (wifi_up) return "Wi-Fi";
    if (wired_up) return "Ethernet";
    return "Offline";
}

/* Per-frame Dock magnification. Runs before luna_update() so the engine eases
 * cur_scale toward the transform_scale we set here. Growth is centered (no
 * vertical lift) so the enlarged icons stay inside the dock's fixed-height
 * layer surface — no clipping, no input-region carving needed. */
static void dock_magnify_tick(void) {
    if (!g_settings.dock_magnification) {
        for (int i = 0; i < g_dock_icon_count; i++) {
            LunaElement* icon = luna_element_at(g_dock_icon_idx[i]);
            if (icon) icon->transform_scale = 1.0f;
        }
        return;
    }
    if (g_dock_icon_count == 0) return;

    /* The dock only reacts while the pointer hovers its vertical band; away
     * from it every icon relaxes back to 1.0×. */
    int active = 0;
    if (g_dock_root_idx >= 0) {
        LunaElement* d = luna_element_at(g_dock_root_idx);
        if (d && !d->display_none && strstr(d->class_name, "hidden") == NULL) {
            float top = d->y, bot = d->y + d->h;
            active = (g_luna_my >= top - 96.0 && g_luna_my <= bot + 24.0);
        }
    }

    /* Peak scale and Gaussian falloff width (in document px). Kept modest so
     * a 52px icon at 1.32× (≈69px) still fits the 80px dock surface. */
    const float peak  = 1.32f;
    const float sigma = 66.0f;
    int changed = 0;
    for (int i = 0; i < g_dock_icon_count; i++) {
        LunaElement* e = luna_element_at(g_dock_icon_idx[i]);
        if (!e) continue;
        float scale = 1.0f;
        if (active) {
            float cx = e->x + e->w * 0.5f;
            float dx = (float)g_luna_mx - cx;
            float g  = expf(-(dx * dx) / (2.0f * sigma * sigma));
            scale = 1.0f + (peak - 1.0f) * g;
        }
        float ty = -(scale - 1.0f) * e->h * 0.18f;
        if (fabsf(e->transform_scale - scale) > 0.001f ||
            fabsf(e->transform_ty - ty) > 0.01f)
            changed = 1;
        e->transform_scale = scale;
        /* A whisper of lift for the biggest icons, safely inside the surface. */
        e->transform_ty = ty;
        e->transform_tx = 0.0f;
    }
    if (changed) shell_request_repaint(2); /* LUNA_SURF_DOCK */
}

static void poll_shell_state(void) {
    double interval = g_switcher_visible ? 0.05 : 0.12;
    if (g_now - g_last_shell_poll < interval) return;
    g_last_shell_poll = g_now;
    g_shell_state_changed = 0;
    load_shell_state();
    if (g_shell_state_changed) {
        update_window_list_ui();
        update_tray_ui();
        update_dock_dots();
        update_switcher_ui();
    }

    if (g_pending_menu_id > 0) {
        uint32_t wid = (uint32_t)g_pending_menu_id;
        const char* title = "Window";
        LunaWinEntry* w = NULL;
        for (int i = 0; i < g_win_count; i++) {
            if (g_wins[i].id == wid) {
                w = &g_wins[i];
                title = w->title[0] ? w->title : "Window";
                break;
            }
        }
        g_win_menu_target = wid;
        win_menu_set_maximize_label(w);
        int t = luna_get_element_by_id("win_menu_title");
        if (t >= 0) luna_set_text(t, title);
        dismiss_luna_menu(g_luna_menu_idx);
        dismiss_cc(g_cc_idx);
        dismiss_clip_menu();
        position_menu_at(g_win_menu_idx, (float)g_pending_menu_x, (float)g_pending_menu_y);
        set_hidden(g_win_menu_idx, 0);
        g_pending_menu_id = 0;
    }
}

static void session_path(char* buf, size_t n) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/root";
    snprintf(buf, n, "%s/.config/luna-shell/session", home);
}

static void session_save(void) {
    if (!g_settings.session_restore) return;
    ensure_config_dir();
    /* Force a fresh window list so shutdown captures the latest set. */
    {
        double prev = g_last_shell_poll;
        g_last_shell_poll = -1.0;
        poll_shell_state();
        g_last_shell_poll = prev;
    }

    char path[512];
    session_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[luna-shell] cannot write session: %s\n", path);
        return;
    }
    fprintf(f, "# Luna Shell session — apps to restore on next login\n");
    int saved[APP_COUNT];
    memset(saved, 0, sizeof(saved));
    int apps_n = 0;
    int wins_n = 0;
    for (int i = 0; i < g_win_count; i++) {
        const char* app_id = g_wins[i].app_id;
        if (!app_id || !*app_id) continue;
        for (int a = 0; a < APP_COUNT; a++) {
            if (saved[a]) continue;
            if (!app_id_matches_key(app_id, g_apps[a].key)) continue;
            fprintf(f, "app\t%s\n", g_apps[a].key);
            saved[a] = 1;
            apps_n++;
            break;
        }
        char app_id_clean[64];
        char title_clean[96];
        snprintf(app_id_clean, sizeof(app_id_clean), "%s", g_wins[i].app_id);
        snprintf(title_clean, sizeof(title_clean), "%s", g_wins[i].title);
        for (size_t k = 0; app_id_clean[k]; k++)
            if (app_id_clean[k] == '\t' || app_id_clean[k] == '\n' || app_id_clean[k] == '\r')
                app_id_clean[k] = ' ';
        for (size_t k = 0; title_clean[k]; k++)
            if (title_clean[k] == '\t' || title_clean[k] == '\n' || title_clean[k] == '\r')
                title_clean[k] = ' ';
        fprintf(f, "win\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n",
                app_id_clean,
                title_clean,
                g_wins[i].x,
                g_wins[i].y,
                g_wins[i].minimized ? 1 : 0,
                g_wins[i].maximized ? 1 : 0,
                g_wins[i].fullscreen ? 1 : 0);
        wins_n++;
    }
    fclose(f);
    fprintf(stderr, "[luna-shell] session saved (%d app(s), %d window(s)) → %s\n",
            apps_n, wins_n, path);
}

static void session_restore_schedule(void) {
    if (!g_settings.session_restore) {
        g_session_restore_done = 1;
        g_session_restore_active = 0;
        return;
    }
    if (getenv("LUNA_NO_SESSION_RESTORE")) {
        g_session_restore_done = 1;
        return;
    }
    /* Caller sets g_now just before this; delay until compositor IPC is warm. */
    g_session_restore_at = g_now + 1.5;
    g_session_restore_deadline = 0.0;
    g_session_restore_active = 0;
    g_restore_window_count = 0;
    g_session_restore_done = 0;
}

static void session_restore_tick(void) {
    if (!g_session_restore_done && g_session_restore_at > 0.0 && g_now >= g_session_restore_at) {
        g_session_restore_done = 1;
        g_session_restore_at = 0.0;

        char path[512];
        session_path(path, sizeof(path));
        FILE* f = fopen(path, "r");
        if (!f) return;

        char line[512];
        int launched = 0;
        int seen[APP_COUNT];
        memset(seen, 0, sizeof(seen));
        g_restore_window_count = 0;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#') continue;
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            const char* kind = line;
            const char* val = tab + 1;
            if (!*val) continue;
            if (!strcmp(kind, "app")) {
                for (int a = 0; a < APP_COUNT; a++) {
                    if (strcmp(g_apps[a].key, val) != 0) continue;
                    if (seen[a]) break;
                    seen[a] = 1;
                    if (find_win_for_app(&g_apps[a])) break;
                    app_launch(&g_apps[a]);
                    launched++;
                    break;
                }
            } else if (!strcmp(kind, "win") && g_restore_window_count < MAX_RESTORE_WINDOWS) {
                LunaRestoreWindow* rw = &g_restore_windows[g_restore_window_count];
                char app_id[64] = "";
                char title[96] = "";
                int x = 0, y = 0, minimized = 0, maximized = 0, fullscreen = 0;
                int parsed = sscanf(val, "%63[^\t]\t%95[^\t]\t%d\t%d\t%d\t%d\t%d",
                                    app_id, title, &x, &y, &minimized, &maximized, &fullscreen);
                if (parsed >= 4) {
                    snprintf(rw->app_id, sizeof(rw->app_id), "%s", app_id);
                    snprintf(rw->title, sizeof(rw->title), "%s", title);
                    rw->x = x;
                    rw->y = y;
                    rw->minimized = (parsed >= 5) ? minimized : 0;
                    rw->maximized = (parsed >= 6) ? maximized : 0;
                    rw->fullscreen = (parsed >= 7) ? fullscreen : 0;
                    rw->applied = 0;
                    g_restore_window_count++;
                }
            }
        }
        fclose(f);
        if (launched > 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Restoring %d app(s)…", launched);
            toast_show("Session", msg, 4.0);
            fprintf(stderr, "[luna-shell] session restore: launched %d app(s)\n", launched);
        }
        if (g_restore_window_count > 0) {
            g_session_restore_active = 1;
            g_session_restore_deadline = g_now + 12.0;
        }
    }

    if (g_session_restore_active) {
        int pending = 0;
        for (int r = 0; r < g_restore_window_count; r++) {
            LunaRestoreWindow* rw = &g_restore_windows[r];
            if (rw->applied) continue;
            pending++;
            int best = -1;
            for (int i = 0; i < g_win_count; i++) {
                if (!rw->app_id[0] || strcmp(g_wins[i].app_id, rw->app_id) != 0)
                    continue;
                if (rw->title[0] && strcmp(g_wins[i].title, rw->title) == 0) {
                    best = i;
                    break;
                }
                if (best < 0) best = i;
            }
            if (best >= 0) {
                uint32_t wid = g_wins[best].id;
                char cmd[96];
                if (rw->fullscreen) {
                    snprintf(cmd, sizeof(cmd), "fullscreen %u", wid);
                    shell_send_cmd(cmd);
                } else if (rw->maximized) {
                    snprintf(cmd, sizeof(cmd), "maximize %u", wid);
                    shell_send_cmd(cmd);
                } else {
                    snprintf(cmd, sizeof(cmd), "move %u %d %d", wid, rw->x, rw->y);
                    shell_send_cmd(cmd);
                }
                if (rw->minimized) {
                    snprintf(cmd, sizeof(cmd), "minimize %u", wid);
                    shell_send_cmd(cmd);
                }
                rw->applied = 1;
                pending--;
            }
        }
        if (pending <= 0 || g_now >= g_session_restore_deadline) {
            g_session_restore_active = 0;
            g_session_restore_deadline = 0.0;
        }
    }
}

static uint32_t win_id_from_element(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0] == 'w' && id[1] == 'i' && id[2] == 'n' && id[3] == '_') {
            int slot = atoi(id + 4);
            if (slot >= 0 && slot < MAX_WIN_SLOTS)
                return g_win_slot_id[slot];
        }
    }
    return 0;
}

static void win_menu_open(uint32_t wid, int anchor_idx, const char* title) {
    if (!wid || g_win_menu_idx < 0) return;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_clip_menu();
    g_win_menu_target = wid;
    LunaWinEntry* w = NULL;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].id == wid) { w = &g_wins[i]; break; }
    }
    win_menu_set_maximize_label(w);
    int t = luna_get_element_by_id("win_menu_title");
    if (t >= 0) luna_set_text(t, title && title[0] ? title : "Window");
    position_menu_near(g_win_menu_idx, anchor_idx, 200.0f);
    set_hidden(g_win_menu_idx, 0);
}

static void on_win_menu_action(LunaElement* e) {
    uint32_t wid = g_win_menu_target;
    const char* id = NULL;
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* cand = luna_element_at(idx)->id;
        if (cand[0] == 'w' && cand[1] == 'm' && cand[2] == '_') { id = cand; break; }
    }
    LunaWinEntry* w = NULL;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].id == wid) { w = &g_wins[i]; break; }
    }
    dismiss_win_menu();
    if (!wid || !id) return;
    char cmd[64];
    if (!strcmp(id, "wm_activate"))
        snprintf(cmd, sizeof(cmd), "activate %u", wid);
    else if (!strcmp(id, "wm_minimize"))
        snprintf(cmd, sizeof(cmd), "minimize %u", wid);
    else if (!strcmp(id, "wm_maximize")) {
        if (w && w->fullscreen)
            snprintf(cmd, sizeof(cmd), "unfullscreen %u", wid);
        else if (w && w->maximized)
            snprintf(cmd, sizeof(cmd), "unmaximize %u", wid);
        else
            snprintf(cmd, sizeof(cmd), "toggle_maximize %u", wid);
    } else if (!strcmp(id, "wm_fullscreen")) {
        if (w && w->fullscreen)
            snprintf(cmd, sizeof(cmd), "unfullscreen %u", wid);
        else
            snprintf(cmd, sizeof(cmd), "fullscreen %u", wid);
    } else if (!strcmp(id, "wm_tile_left"))
        snprintf(cmd, sizeof(cmd), "tile_left %u", wid);
    else if (!strcmp(id, "wm_tile_right"))
        snprintf(cmd, sizeof(cmd), "tile_right %u", wid);
    else if (!strcmp(id, "wm_center"))
        snprintf(cmd, sizeof(cmd), "center %u", wid);
    else if (!strcmp(id, "wm_close"))
        snprintf(cmd, sizeof(cmd), "close %u", wid);
    else
        return;
    shell_send_cmd(cmd);
}

static void on_win_click(LunaElement* e) {
    uint32_t wid = win_id_from_element(e);
    if (!wid) return;
    int slot_idx = -1;
    LunaWinEntry* w = NULL;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].id == wid) { w = &g_wins[i]; break; }
    }
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0] == 'w' && id[1] == 'i' && id[2] == 'n' && id[3] == '_') {
            int slot = atoi(id + 4);
            if (slot >= 0 && slot < MAX_WIN_SLOTS)
                slot_idx = g_win_slot_idx[slot];
            break;
        }
    }

    /* Right-click → window context menu */
    if (luna_last_click_button() == LUNA_MOUSE_BUTTON_RIGHT) {
        win_menu_open(wid, slot_idx, w ? w->title : NULL);
        return;
    }

    dismiss_win_menu();
    dismiss_clip_menu();
    char cmd[64];
    /* Taskbar-style toggle: focused window minimizes; otherwise activate. */
    if (w && w->focused && !w->minimized)
        snprintf(cmd, sizeof(cmd), "minimize %u", wid);
    else
        snprintf(cmd, sizeof(cmd), "activate %u", wid);
    shell_send_cmd(cmd);
}

/* ── Clipboard history menu ── */

#define CLIP_MENU_SLOTS 8

static void clip_history_path(char* buf, size_t n) {
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/luna-clipboard/history", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.cache/luna-clipboard/history", home);
    else
        snprintf(buf, n, "/tmp/luna-clipboard-%d/history", (int)getuid());
}

static void clip_cmd_sock_path(char* buf, size_t n) {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
        snprintf(buf, n, "%s/luna-clipboard.sock", rt);
    else
        snprintf(buf, n, "/tmp/luna-clipboard-%d.sock", (int)getuid());
}

static void clip_send_cmd(const char* cmd) {
    char path[512];
    clip_cmd_sock_path(path, sizeof(path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t splen = strlen(path);
    if (splen >= sizeof(addr.sun_path)) { close(fd); return; }
    memcpy(addr.sun_path, path, splen + 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    size_t n = strlen(cmd);
    if (n > 0) {
        send(fd, cmd, n, 0);
        send(fd, "\n", 1, 0);
    }
    close(fd);
}

static void clip_preview_text(const char* data, size_t len, char* out, size_t out_n) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < out_n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            if (j + 1 < out_n) out[j++] = ' ';
        } else if (c >= 0x20) {
            out[j++] = (char)c;
        }
        if (j >= 64) break;
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
    if (!out[0]) snprintf(out, out_n, "(empty)");
}

static int clip_populate_menu(void) {
    char path[576];
    clip_history_path(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    int count = 0;
    int empty_idx = luna_get_element_by_id("clip_empty");

    for (int s = 0; s < CLIP_MENU_SLOTS; s++) {
        char id[32];
        snprintf(id, sizeof(id), "clip_%d", s);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) set_hidden(idx, 1);
    }

    if (!f) {
        if (empty_idx >= 0) set_hidden(empty_idx, 0);
        return 0;
    }

    while (count < CLIP_MENU_SLOTS) {
        char lenbuf[64], mime[256];
        if (!fgets(lenbuf, sizeof(lenbuf), f)) break;
        size_t len = (size_t)strtoul(lenbuf, NULL, 10);
        if (!fgets(mime, sizeof(mime), f)) break;
        if (len > 2 * 1024 * 1024) break;
        /* Clipboard previews only need one record at a time.  Keep the
         * maximum-sized workspace in BSS so opening this menu never churns
         * the allocator (formerly one malloc/free, up to 2 MiB, per row). */
        static char data[2 * 1024 * 1024 + 1];
        if (fread(data, 1, len, f) != len) break;
        data[len] = 0;
        int c = fgetc(f); /* trailing newline */
        (void)c;

        char id[32];
        snprintf(id, sizeof(id), "clip_%d", count);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) {
            set_hidden(idx, 0);
            char preview[80];
            clip_preview_text(data, len, preview, sizeof(preview));
            for (int i = 0; i < luna_element_count(); i++) {
                LunaElement* el = luna_element_at(i);
                if (el->parent_idx == idx && strstr(el->class_name, "clip_preview")) {
                    luna_set_text(i, preview);
                    break;
                }
            }
        }
        count++;
    }
    fclose(f);
    if (empty_idx >= 0) set_hidden(empty_idx, count == 0 ? 0 : 1);
    return count;
}

static void on_clipboard_menu(LunaElement* e) {
    (void)e;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    if (is_shown(g_clip_menu_idx)) { dismiss_clip_menu(); return; }
    clip_populate_menu();
    position_menu_near(g_clip_menu_idx, g_mb_clip_idx, luna_window_width - 300.0f);
    set_hidden(g_clip_menu_idx, 0);
}

static void on_clip_select(LunaElement* e) {
    int slot = -1;
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0]=='c' && id[1]=='l' && id[2]=='i' && id[3]=='p' && id[4]=='_' &&
            id[5] >= '0' && id[5] <= '9') {
            slot = atoi(id + 5);
            break;
        }
    }
    dismiss_clip_menu();
    if (slot < 0) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "select %d", slot);
    clip_send_cmd(cmd);
    toast_show("Clipboard", "Selection restored", 2.0);
}

static void on_clip_clear(LunaElement* e) {
    (void)e;
    dismiss_clip_menu();
    clip_send_cmd("clear");
    toast_show("Clipboard", "History cleared", 2.0);
}

static void on_tray_click(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0] == 't' && id[1] == 'r' && id[2] == 'a' && id[3] == 'y' && id[4] == '_') {
            int slot = atoi(id + 5);
            int app_slots = MAX_TRAY_SLOTS - 2;
            if (slot == app_slots) {
                /* Keep the built-in tray icon consistent with the Wi-Fi item
                 * in the menubar.  Opening Control Center here made the
                 * actual network list unreachable when luna-shell was
                 * started directly and this was the only visible Wi-Fi
                 * affordance. */
                on_wifi_menu(e);
                return;
            }
            if (slot == app_slots + 1) {
                toast_show("Power", read_battery_percent() >= 0 ? "On battery" : "AC connected", 2.5);
                return;
            }
            if (slot >= 0 && slot < app_slots && g_tray[slot].surface_id) {
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "activate %u", g_tray[slot].surface_id);
                shell_send_cmd(cmd);
                return;
            }
            if (slot >= 0 && slot < g_tray_count &&
                !strncmp(g_tray[slot].id, "service:", 8)) {
                /* A native item has no client to activate.  It receives the
                 * action directly; Wi-Fi also opens the shell-owned network
                 * menu so direct and service-backed launches behave alike. */
                tray_send_action(g_tray[slot].id, "activate");
                if (!strcmp(g_tray[slot].id, "service:luna-wifi"))
                    on_wifi_menu(e);
                return;
            }
            if (slot >= 0 && slot < g_tray_count && g_tray[slot].tooltip[0]) {
                toast_show(g_tray[slot].label, g_tray[slot].tooltip, 3.0);
            }
            return;
        }
    }
}

/* Returns 1 if the bar width actually changed.
 *
 * These fills are leaf nodes whose width cannot affect a sibling or parent.
 * Updating them used to mark the entire document layout dirty every time a
 * CPU sample changed.  On KMS that full layout pass landed on the
 * render thread and showed up as a regular hitch.  Keep the resolved width in
 * sync directly; the next unrelated layout pass will preserve it through the
 * matching css_width value. */
static int set_bar_fill(const char* fill_id, float pct) {
    int fi = luna_get_element_by_id(fill_id);
    if (fi == -1) return 0;
    LunaElement* fill = luna_element_at(fi);
    float bar_w = 176.0f;
    int p = fill->parent_idx;
    if (p != -1 && luna_element_at(p)->w > 0) bar_w = luna_element_at(p)->w;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float nw = 2.0f + (bar_w - 2.0f) * pct / 100.0f;
    if (fabsf(fill->css_width - nw) < 0.5f && fill->has_css_width) return 0;
    fill->css_width = nw;
    fill->has_css_width = 1;
    fill->w = nw;
    return 1;
}

static void update_clock(void) {
    if (g_now - g_last_clock < 1.0) return;
    g_last_clock = g_now;
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return;
    char buf[64];
    int changed = 0;
    int idx = luna_get_element_by_id("mb_clock");
    if (idx != -1 && strftime(buf, sizeof(buf), "%a %b %e  %H:%M", tm_info) > 0) {
        if (strcmp(luna_element_at(idx)->text, buf) != 0) {
            luna_set_text(idx, buf);
            changed = 1;
        }
    }
    idx = luna_get_element_by_id("wg_time");
    if (idx != -1 && strftime(buf, sizeof(buf), "%H:%M", tm_info) > 0) {
        if (strcmp(luna_element_at(idx)->text, buf) != 0) {
            luna_set_text(idx, buf);
            changed = 1;
        }
    }
    idx = luna_get_element_by_id("wg_date");
    if (idx != -1 && strftime(buf, sizeof(buf), "%A, %B %e", tm_info) > 0) {
        if (strcmp(luna_element_at(idx)->text, buf) != 0) {
            luna_set_text(idx, buf);
            changed = 1;
        }
    }
    if (changed) {
        shell_request_repaint(1); /* menubar */
        shell_request_repaint(0); /* widgets sit in document; bg if KMS */
    }
}

static int text_would_change(int idx, const char* buf) {
    if (idx < 0 || !buf) return 0;
    LunaElement* e = luna_element_at(idx);
    return e && strcmp(e->text, buf) != 0;
}

/* Spread procfs/sysfs/filesystem reads across five one-second phases so that
 * no rendered frame pays for more than one status-I/O class.  CPU, memory,
 * battery and network remain fresh enough for desktop chrome; disk is still
 * limited to one statvfs call per 30 seconds. */
static void update_stats(void) {
    if (g_now - g_last_stats < 1.0) return;
    g_last_stats = g_now;
    char buf[64];
    int dirty_mb = 0, dirty_bg = 0, dirty_cc = 0;

    /* One I/O class per tick prevents a periodic cluster of blocking procfs,
     * sysfs and filesystem calls from landing in the same rendered frame. */
    static int phase = 0;
    int idx;
    switch (phase) {
    case 0: {
        float cpu = read_cpu_percent();
        idx = luna_get_element_by_id("st_cpu_val");
        snprintf(buf, sizeof(buf), "%d%%", (int)cpu);
        if (text_would_change(idx, buf)) { luna_set_text(idx, buf); dirty_bg = 1; }
        if (set_bar_fill("st_cpu_fill", cpu)) dirty_bg = 1;
        if (set_bar_fill("cc_cpu_fill", cpu)) dirty_cc = 1;
        break;
    }
    case 1: {
        int mem = read_mem_percent(NULL);
        idx = luna_get_element_by_id("st_mem_val");
        snprintf(buf, sizeof(buf), "%d%%", mem);
        if (text_would_change(idx, buf)) { luna_set_text(idx, buf); dirty_bg = 1; }
        if (set_bar_fill("st_mem_fill", (float)mem)) dirty_bg = 1;
        if (set_bar_fill("cc_mem_fill", (float)mem)) dirty_cc = 1;
        break;
    }
    case 2: {
        g_cached_bat = read_battery_percent();
        idx = luna_get_element_by_id("mb_bat");
        const char* icon = g_cached_bat >= 0 ? "\uf240" : "\uf1e6";
        if (g_cached_bat >= 0) snprintf(buf, sizeof(buf), "%d%%", g_cached_bat);
        else snprintf(buf, sizeof(buf), "AC");
        /* Keep icon and label in their browser-equivalent DOM nodes.  Packing
           both into mb_bat duplicates the child glyph and defeats flex's
           anonymous text-item layout. */
        if (g_mb_bat_icon_idx >= 0) luna_set_text(g_mb_bat_icon_idx, icon);
        if (text_would_change(idx, buf)) { luna_set_text(idx, buf); dirty_mb = 1; }
        break;
    }
    case 3:
        snprintf(g_cached_net, sizeof(g_cached_net), "%s", read_net_status());
        idx = luna_get_element_by_id("mb_wifi");
        if (g_mb_wifi_icon_idx >= 0) luna_set_text(g_mb_wifi_icon_idx, "\uf1eb");
        if (text_would_change(idx, g_cached_net)) {
            luna_set_text(idx, g_cached_net);
            dirty_mb = 1;
        }
        break;
    case 4:
        if (g_now - g_last_disk >= 30.0) {
            g_last_disk = g_now;
            struct statvfs vfs;
            if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
                double total = (double)vfs.f_blocks * vfs.f_frsize;
                double avail = (double)vfs.f_bavail * vfs.f_frsize;
                double used_pct = 100.0 * (total - avail) / total;
                idx = luna_get_element_by_id("st_disk_val");
                snprintf(buf, sizeof(buf), "%.0fG free", avail / (1024.0 * 1024.0 * 1024.0));
                if (text_would_change(idx, buf)) { luna_set_text(idx, buf); dirty_bg = 1; }
                if (set_bar_fill("st_disk_fill", (float)used_pct)) dirty_bg = 1;
            }
        }
        break;
    }
    phase = (phase + 1) % 5;

    if (dirty_mb) shell_request_repaint(1); /* menubar */
    if (dirty_bg) shell_request_repaint(0); /* widgets on bg_layer */
    /* control_center is g_surfs[6]; bit is ignored on non-WL backends. */
    if (dirty_cc && is_shown(g_cc_idx)) shell_request_repaint(6);
}

/* Bound event sleep by the next scheduled shell job.  Input descriptors wake
 * poll immediately, so longer idle sleeps do not add input latency. */
static int shell_wait_timeout_ms(int max_ms, double repaint_deadline) {
    double next = g_now + (double)max_ms / 1000.0;
#define SOONER(deadline) do { \
        double d_ = (deadline); \
        if (d_ > 0.0 && d_ < next) next = d_; \
    } while (0)
    SOONER(g_last_shell_poll + (g_switcher_visible ? 0.05 : 0.12));
    SOONER(g_last_clock + 1.0);
    SOONER(g_last_stats + 1.0);
    SOONER(g_toast_deadline);
    SOONER(g_session_restore_at);
    SOONER(repaint_deadline);
    if (g_cur_theme.active_role >= 0 &&
        g_cur_theme.active_role < LUNA_CUR_MAX_ROLES) {
        LunaCurAnim* a = &g_cur_theme.roles[g_cur_theme.active_role];
        if (a->loaded && a->nframes > 1)
            SOONER(a->frame_until);
    }
#undef SOONER
    double remain = next - g_now;
    if (remain <= 0.0) return 0;
    int ms = (int)ceil(remain * 1000.0);
    if (ms < 1) ms = 1;
    if (ms > max_ms) ms = max_ms;
    return ms;
}

static void update_launchpad_filter(void) {
    if (g_lp_search_idx == -1 || !is_shown(g_launchpad_idx)) return;
    const char* q = luna_get_value(g_lp_search_idx);
    if (!q) q = "";
    if (!strcmp(q, g_lp_query)) return;
    snprintf(g_lp_query, sizeof(g_lp_query), "%s", q);
    for (int i = 0; i < APP_COUNT; i++) {
        char tile_id[64];
        snprintf(tile_id, sizeof(tile_id), "lp_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(tile_id);
        if (idx != -1)
            luna_element_at(idx)->display_none = !ci_contains(g_apps[i].name, q);
    }
    luna_mark_layout_dirty();
}

static void fill_about_info(void) {
    char buf[192];
    struct utsname un;
    int idx = luna_get_element_by_id("about_kernel");
    if (idx != -1 && uname(&un) == 0) {
        snprintf(buf, sizeof(buf), "Kernel   %s %s", un.sysname, un.release);
        luna_set_text(idx, buf);
    }
    idx = luna_get_element_by_id("about_mem");
    unsigned long total_kb = 0;
    read_mem_percent(&total_kb);
    if (idx != -1 && total_kb > 0) {
        snprintf(buf, sizeof(buf), "Memory   %.1f GB", (double)total_kb / (1024.0 * 1024.0));
        luna_set_text(idx, buf);
    }
}

/* ── Wiring ── */

static void register_handlers(void) {
    luna_register_js_handler("onLunaMenu",      on_luna_menu);
    luna_register_js_handler("onControlCenter", on_control_center);
    luna_register_js_handler("onAbout",         on_about);
    luna_register_js_handler("onAboutClose",    on_about_close);
    luna_register_js_handler("onAboutMin",      on_about_min);
    luna_register_js_handler("onAboutMax",      on_about_max);
    luna_register_js_handler("onSettingsOpen",  on_settings_open);
    luna_register_js_handler("onSettingsClose", on_settings_close);
    luna_register_js_handler("onSettingsMin",   on_settings_min);
    luna_register_js_handler("onSettingsMax",   on_settings_max);
    luna_register_js_handler("onSettingsSave",  on_settings_save);
    luna_register_js_handler("onSettingsTab",   on_settings_tab);
    luna_register_js_handler("onWmToggle",      on_wm_toggle);
    luna_register_js_handler("onWmGap",         on_wm_gap);
    luna_register_js_handler("onWpSelect",      on_wallpaper_select);
    luna_register_js_handler("onKbSelect",      on_kb_select);
    luna_register_js_handler("onCurSelect",     on_cursor_select);
    luna_register_js_handler("onRestart",       on_restart);
    luna_register_js_handler("onShutdown",      on_shutdown);
    luna_register_js_handler("onLogout",        on_logout);
    luna_register_js_handler("onConfirmCancel", on_confirm_cancel);
    luna_register_js_handler("onConfirmOk",     on_confirm_ok);
    luna_register_js_handler("onLaunchpadOpen", on_launchpad_open);
    luna_register_js_handler("onLaunchpadClose",on_launchpad_close);
    luna_register_js_handler("onLaunchApp",     on_launch_app);
    luna_register_js_handler("onDockClick",     on_dock_click);
    luna_register_js_handler("onTrash",         on_trash);
    luna_register_js_handler("onToastClose",    on_toast_close);
    luna_register_js_handler("onCcToggle",      on_cc_toggle);
    luna_register_js_handler("onWinClick",      on_win_click);
    luna_register_js_handler("onTrayClick",     on_tray_click);
    luna_register_js_handler("onWinMenuAction", on_win_menu_action);
    luna_register_js_handler("onClipboardMenu", on_clipboard_menu);
    luna_register_js_handler("onClipSelect",    on_clip_select);
    luna_register_js_handler("onClipClear",     on_clip_clear);
}

static void bind_indices(void) {
    g_luna_menu_idx     = luna_get_element_by_id("luna_menu");
    g_cc_idx            = luna_get_element_by_id("control_center");
    g_launchpad_idx     = luna_get_element_by_id("launchpad");
    g_about_idx         = luna_get_element_by_id("about_win");
    g_about_box_idx     = luna_get_element_by_id("about_box");
    g_confirm_idx       = luna_get_element_by_id("confirm_overlay");
    g_confirm_box_idx   = luna_get_element_by_id("confirm_box");
    g_toast_idx         = luna_get_element_by_id("toast");
    g_lp_search_idx     = luna_get_element_by_id("lp_search");
    g_settings_idx      = luna_get_element_by_id("settings_win");
    g_settings_sheet_idx = luna_get_element_by_id("settings_sheet");
    g_settings_panel_apps = luna_get_element_by_id("settings_panel_apps");
    g_settings_panel_disp = luna_get_element_by_id("settings_panel_disp");
    g_settings_panel_wm   = luna_get_element_by_id("settings_panel_wm");
    g_stab_apps_idx     = luna_get_element_by_id("stab_apps");
    g_stab_disp_idx     = luna_get_element_by_id("stab_disp");
    g_stab_wm_idx       = luna_get_element_by_id("stab_wm");
    g_win_menu_idx      = luna_get_element_by_id("win_menu");
    g_clip_menu_idx     = luna_get_element_by_id("clip_menu");
    g_mb_logo_idx       = luna_get_element_by_id("mb_logo");
    g_mb_cc_idx         = luna_get_element_by_id("mb_cc");
    g_mb_wifi_idx       = luna_get_element_by_id("mb_wifi");
    g_wifi_menu_idx     = luna_get_element_by_id("wifi_menu");

    /* The redesigned markup keeps the about box's visual class but no longer
     * includes sheet_box.  The shared class supplies its absolute positioning,
     * stacking and clipping, all of which the C-side centering code relies on. */
    if (g_about_box_idx >= 0) {
        luna_add_class(g_about_box_idx, "sheet_box");
        luna_update_element_style(g_about_box_idx);
    }

    /* Drag handles used to advertise draggable="1" in the markup.  Keep this
     * behavior in the shell so presentation-only HTML changes cannot disable
     * moving either sheet. */
    {
        const char* drag_ids[] = { "about_drag", "settings_drag", "confirm_drag" };
        for (size_t i = 0; i < sizeof(drag_ids) / sizeof(drag_ids[0]); i++) {
            int idx = luna_get_element_by_id(drag_ids[i]);
            if (idx < 0) continue;
            LunaElement* drag = luna_element_at(idx);
            drag->is_draggable = 1;
            drag->drag_mode = 1; /* move the containing sheet */
            /* The handles overlap the title/content boundary.  Keep them
             * above the body in the native hit-test order; CSS-only stacking
             * is too late for a press received during a relayout.  Traffic
             * light controls use z-index 20, so they remain clickable. */
            drag->z_index = 10;
        }
    }
    g_mb_clip_idx       = luna_get_element_by_id("mb_clip");

    /* Wire dock items */
    for (int i = 0; i < APP_COUNT; i++) {
        char id[64];
        snprintf(id, sizeof(id), "dock_%s", g_apps[i].key);
        wire_subtree(luna_get_element_by_id(id), on_dock_click);
        snprintf(id, sizeof(id), "lp_%s", g_apps[i].key);
        wire_subtree(luna_get_element_by_id(id), on_launch_app);
        app_set_dot(&g_apps[i], 0);
    }
    wire_subtree(luna_get_element_by_id("mb_logo"),       on_luna_menu);
    wire_subtree(luna_get_element_by_id("mb_wifi"),       on_wifi_menu);
    wire_subtree(luna_get_element_by_id("mb_cc"),         on_control_center);
    wire_subtree(luna_get_element_by_id("mi_about"),      on_about);
    wire_subtree(luna_get_element_by_id("mi_settings"),   on_settings_open);
    wire_subtree(luna_get_element_by_id("mi_launchpad"),  on_launchpad_open);
    wire_subtree(luna_get_element_by_id("mi_restart"),    on_restart);
    wire_subtree(luna_get_element_by_id("mi_shutdown"),   on_shutdown);
    wire_subtree(luna_get_element_by_id("mi_logout"),     on_logout);
    wire_subtree(luna_get_element_by_id("dock_launchpad"),on_launchpad_open);
    wire_subtree(luna_get_element_by_id("dock_trash"),    on_trash);
    wire_subtree(luna_get_element_by_id("toast_close"),   on_toast_close);
    wire_subtree(luna_get_element_by_id("cc_wifi"),       on_cc_toggle);
    wire_subtree(luna_get_element_by_id("cc_bt"),         on_cc_toggle);
    wire_subtree(luna_get_element_by_id("cc_night"),      on_cc_toggle);
    wire_subtree(luna_get_element_by_id("wifi_power"),    on_wifi_power);
    wire_subtree(luna_get_element_by_id("wifi_connect"),  on_wifi_connect);
    wire_subtree(luna_get_element_by_id("wifi_scan"),     on_wifi_scan);
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        char id[16]; snprintf(id, sizeof(id), "wifi_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_wifi_network);
    }

    /* Wire wallpaper selection thumbs */
    const char* wp_ids[] = {"wp_night","wp_ocean","wp_forest","wp_sunset"};
    for (int i = 0; i < 4; i++)
        wire_subtree(luna_get_element_by_id(wp_ids[i]), on_wallpaper_select);
    {
        const char* cur_ids[] = { "cur_aero", "cur_miku" };
        for (int i = 0; i < 2; i++)
            wire_subtree(luna_get_element_by_id(cur_ids[i]), on_cursor_select);
    }
    {
        const char* kb_ids[] = {
            "kb_jp_us", "kb_us", "kb_jp", "kb_de_us", "kb_fr_us", "kb_kr_us"
        };
        for (int i = 0; i < 6; i++)
            wire_subtree(luna_get_element_by_id(kb_ids[i]), on_kb_select);
    }

    /* Window list + system tray slots */
    for (int i = 0; i < MAX_WIN_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "win_%d", i);
        g_win_slot_idx[i] = luna_get_element_by_id(id);
        wire_subtree(g_win_slot_idx[i], on_win_click);
    }
    for (int i = 0; i < MAX_TRAY_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "tray_%d", i);
        g_tray_slot_idx[i] = luna_get_element_by_id(id);
        wire_subtree(g_tray_slot_idx[i], on_tray_click);
    }

    /* Do not expose the static HTML preview chips while the first compositor
     * snapshot is still pending.  poll_shell_state() replaces these with the
     * actual window and tray entries on its first successful read. */
    update_window_list_ui();
    update_tray_ui();

    /* Wire settings tab buttons */
    wire_subtree(g_stab_apps_idx, on_settings_tab);
    wire_subtree(g_stab_disp_idx, on_settings_tab);
    wire_subtree(g_stab_wm_idx, on_settings_tab);
    {
        const char* toggle_ids[] = {
            "wm_snap", "wm_double_click", "wm_shortcuts", "wm_dock_mag", "wm_restore"
        };
        for (size_t i = 0; i < sizeof(toggle_ids) / sizeof(toggle_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(toggle_ids[i]), on_wm_toggle);
        const char* gap_ids[] = { "wm_gap_0", "wm_gap_8", "wm_gap_16" };
        for (size_t i = 0; i < sizeof(gap_ids) / sizeof(gap_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(gap_ids[i]), on_wm_gap);
    }

    wire_subtree(g_mb_clip_idx, on_clipboard_menu);
    wire_subtree(luna_get_element_by_id("clip_clear"), on_clip_clear);
    for (int i = 0; i < CLIP_MENU_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "clip_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_clip_select);
    }
    wire_subtree(luna_get_element_by_id("wm_activate"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_minimize"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_maximize"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_fullscreen"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_tile_left"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_tile_right"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_center"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("wm_close"), on_win_menu_action);
    wire_subtree(luna_get_element_by_id("tl_min"), on_about_min);
    wire_subtree(luna_get_element_by_id("tl_max"), on_about_max);
    wire_subtree(luna_get_element_by_id("stl_min"), on_settings_min);
    wire_subtree(luna_get_element_by_id("stl_max"), on_settings_max);
    wire_subtree(luna_get_element_by_id("tl_close"), on_about_close);
    wire_subtree(luna_get_element_by_id("about_backdrop"), on_about_close);
    wire_subtree(luna_get_element_by_id("stl_close"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_backdrop"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_cancel"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_ok"), on_settings_save);
    wire_subtree(luna_get_element_by_id("confirm_backdrop"), on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("ctl_close"),        on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("confirm_cancel"), on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("confirm_ok"), on_confirm_ok);

    /* Cache dock geometry for magnification: the dock root and every icon
     * square (.dock_icon, excluding the .dock_dot running indicators). */
    g_dock_root_idx = luna_get_element_by_id("dock");
    g_dock_icon_count = 0;
    for (int i = 0; i < luna_element_count() && g_dock_icon_count < MAX_DOCK_ICONS; i++) {
        LunaElement* e = luna_element_at(i);
        if (strstr(e->class_name, "dock_icon"))
            g_dock_icon_idx[g_dock_icon_count++] = i;
    }

    /* ── Hot-path child element cache ──
     * One linear pass at bind time; O(1) lookups in every render tick. */
    g_mb_app_idx = luna_get_element_by_id("mb_app");
    memset(g_win_label_idx,  -1, sizeof(g_win_label_idx));
    memset(g_tray_glyph_idx, -1, sizeof(g_tray_glyph_idx));
    memset(g_sw_title_idx,   -1, sizeof(g_sw_title_idx));
    memset(g_sw_app_idx,     -1, sizeof(g_sw_app_idx));
    g_mb_bat_icon_idx        = -1;
    g_mb_wifi_icon_idx       = -1;
    g_wm_maximize_label_idx  = -1;
    g_wm_fullscreen_label_idx = -1;

    int mb_bat_parent  = luna_get_element_by_id("mb_bat");
    int mb_wifi_parent = luna_get_element_by_id("mb_wifi");
    int wm_max_parent  = luna_get_element_by_id("wm_maximize");
    int wm_fs_parent   = luna_get_element_by_id("wm_fullscreen");

    /* Collect sw_N parent indices once to avoid repeated ID lookups inside loop */
    int sw_parent[MAX_SWITCHER_SLOTS];
    for (int s = 0; s < MAX_SWITCHER_SLOTS; s++) {
        char id[16]; snprintf(id, sizeof(id), "sw_%d", s);
        sw_parent[s] = luna_get_element_by_id(id);
    }

    int n = luna_element_count();
    for (int i = 0; i < n; i++) {
        LunaElement* e = luna_element_at(i);
        int p = e->parent_idx;

        /* win_label children */
        if (strstr(e->class_name, "win_label")) {
            for (int s = 0; s < MAX_WIN_SLOTS; s++)
                if (g_win_slot_idx[s] == p) { g_win_label_idx[s] = i; break; }
            continue;
        }
        /* tray_glyph children */
        if (strstr(e->class_name, "tray_glyph")) {
            for (int s = 0; s < MAX_TRAY_SLOTS; s++)
                if (g_tray_slot_idx[s] == p) { g_tray_glyph_idx[s] = i; break; }
            continue;
        }
        /* switcher title / app labels */
        if (strstr(e->class_name, "sw_title")) {
            for (int s = 0; s < MAX_SWITCHER_SLOTS; s++)
                if (sw_parent[s] == p) { g_sw_title_idx[s] = i; break; }
            continue;
        }
        if (strstr(e->class_name, "sw_app")) {
            for (int s = 0; s < MAX_SWITCHER_SLOTS; s++)
                if (sw_parent[s] == p) { g_sw_app_idx[s] = i; break; }
            continue;
        }
        /* mb_bat / mb_wifi icons and wm_* mi_labels */
        if (strstr(e->class_name, "luna_icon")) {
            if (p == mb_bat_parent)  { g_mb_bat_icon_idx  = i; continue; }
            if (p == mb_wifi_parent) { g_mb_wifi_icon_idx = i; continue; }
        }
        if (strstr(e->class_name, "mi_label")) {
            if (p == wm_max_parent) { g_wm_maximize_label_idx  = i; continue; }
            if (p == wm_fs_parent)  { g_wm_fullscreen_label_idx = i; continue; }
        }
    }

    /* Initial Control Center knob positions (wifi & bt start "on") */
    int k = luna_get_element_by_id("cc_wifi_knob");
    if (k != -1) { luna_element_at(k)->rel_x = 21.0f; luna_element_at(k)->pos_overridden_x = 1; }
    k = luna_get_element_by_id("cc_bt_knob");
    if (k != -1) { luna_element_at(k)->rel_x = 21.0f; luna_element_at(k)->pos_overridden_x = 1; }
}

/* ── Display backends ──────────────────────────────────────────────────
 * GLFW is gone. Two backends implement the same small LunaBackend
 * interface:
 *   - KMS/DRM + GBM + EGL + libinput  — bare console, no compositor (dri).
 *   - Wayland + EGL + wl_seat         — any Wayland compositor, including
 *                                       Wayback (Xwayland-rootful over
 *                                       wlroots), since it exposes a normal
 *                                       Wayland socket like any other
 *                                       compositor.
 * Selection is automatic at runtime, based on WAYLAND_DISPLAY.
 * ──────────────────────────────────────────────────────────────────── */

#include <sys/mman.h>

typedef struct {
    int  (*start)(void);
    void (*get_fb_size)(int* w, int* h);
    void (*swap_buffers)(void);
    void (*poll_events)(void);
    void (*set_cursor)(int cursor_type);
    void (*terminate)(void);
} LunaBackend;

static const LunaBackend* g_backend = NULL;

static double plat_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static struct timespec t0;
    static int started = 0;
    if (!started) { t0 = ts; started = 1; }
    return (double)(ts.tv_sec - t0.tv_sec) + (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;
}
/* eglGetProcAddress() is only spec-guaranteed to resolve *extension*
 * functions — core entry points like glGenTextures/glCreateShader are
 * allowed to come back NULL depending on the driver. luna-ui.h loads
 * essentially all GL calls (core and extension alike) as function
 * pointers through this callback, so an unresolved core function here
 * means a NULL-pointer call (and a crash) the moment it's first used —
 * typically right after the first texture/shader is created, which is
 * why this can show up only after fonts have already loaded fine.
 * glfwGetProcAddress used to paper over this with exactly this kind of
 * dlsym fallback; do the same here. */
static void* g_gl_lib_handle = NULL;
static void* plat_proc(const char* n) {
    void* p = (void*)eglGetProcAddress(n);
    if (p) return p;
    /* eglGetProcAddress() is only spec-guaranteed for extension functions on
     * old EGL; core entry points can return NULL on some drivers.  Fall back
     * to dlsym on a vendor-neutral lib so we don't mix GLX and EGL dispatch
     * tables.  RTLD_LOCAL prevents the opened library from polluting the
     * global symbol table (which would re-route direct gl* calls in the main
     * loop through the wrong dispatch, giving a NULL-context crash in gallium). */
    if (!g_gl_lib_handle) {
        /* libOpenGL.so.0 is the EGL-compatible, GLX-free vendor-neutral library
         * (available since Mesa 17.3 / glvnd).  Fall back to libGL.so.1 only
         * if libOpenGL is absent — but keep RTLD_LOCAL either way. */
        g_gl_lib_handle = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!g_gl_lib_handle)
            g_gl_lib_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!g_gl_lib_handle)
            g_gl_lib_handle = dlopen("libGL.so",   RTLD_NOW | RTLD_LOCAL);
    }
    if (g_gl_lib_handle) p = dlsym(g_gl_lib_handle, n);
    if (!p) fprintf(stderr, "[luna-shell] warning: could not resolve GL symbol %s\n", n);
    return p;
}
static void  plat_close(void)         { g_should_close = 1; }
static void  plat_iconify(void)       { /* no window-manager chrome in either backend */ }
static void  plat_maximize(void)      { /* both backends already run fullscreen */ }
static void  plat_cursor(int type) {
    if (g_backend && g_backend->set_cursor) g_backend->set_cursor(type);
}

static void take_timestamped_screenshot(void) {
    char path[512];
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info && strftime(path, sizeof(path), "luna_%Y%m%d_%H%M%S.png", tm_info) > 0)
        luna_take_screenshot(path);
    else
        luna_take_screenshot("luna.png");
}

/* Shared key-press routing (menu shortcuts), used by both backends —
 * this is the old on_key() body, minus the GLFWwindow* parameter. */
static void dispatch_key(int key, int scancode, int action, int mods) {
    if (action == LUNA_PRESS) {
        /* Quit chords — handle here so they win even over text-field editing. */
        if (key == LUNA_KEY_BACKSPACE &&
            (mods & LUNA_MOD_CONTROL) && (mods & LUNA_MOD_ALT)) {
            g_should_close = 1;
            return;
        }
        if (key == LUNA_KEY_F4 && (mods & LUNA_MOD_ALT)) {
            /* Close focused app window; do not kill the whole desktop. */
            for (int i = 0; i < g_win_count; i++) {
                if (g_wins[i].focused && !g_wins[i].minimized) {
                    char cmd[64];
                    snprintf(cmd, sizeof(cmd), "close %u", g_wins[i].id);
                    shell_send_cmd(cmd);
                    return;
                }
            }
            return;
        }
        if (key == LUNA_KEY_ESCAPE) {
            if (is_shown(g_settings_idx))    { on_settings_close(NULL); return; }
            if (is_shown(g_launchpad_idx))   { launchpad_close();       return; }
            if (is_shown(g_confirm_idx))     { on_confirm_cancel(NULL); return; }
            if (is_shown(g_about_idx))       { on_about_close(NULL);    return; }
            if (is_shown(g_win_menu_idx))    { dismiss_win_menu();      return; }
            if (is_shown(g_clip_menu_idx))   { dismiss_clip_menu();     return; }
            if (is_shown(g_luna_menu_idx))   { dismiss_luna_menu(g_luna_menu_idx); return; }
            if (is_shown(g_cc_idx))          { dismiss_cc(g_cc_idx);    return; }
        }
        /* Super (or bare F4) toggles Launchpad — Alt+F4 is quit above. */
        if (key == LUNA_KEY_LEFT_SUPER || key == LUNA_KEY_RIGHT_SUPER ||
            (key == LUNA_KEY_F4 && !(mods & LUNA_MOD_ALT))) {
            if (is_shown(g_launchpad_idx)) launchpad_close();
            else on_launchpad_open(NULL);
            return;
        }
        if (key == LUNA_KEY_COMMA && (mods & LUNA_MOD_SUPER)) {
            on_settings_open(NULL);
            return;
        }
        if (key == LUNA_KEY_F12) { take_timestamped_screenshot(); return; }
    }
    luna_key(key, scancode, action, mods);
}

/* xkbcommon keysym -> luna-ui.h's neutral key code. Shared by the KMS
 * (libinput) and Wayland (wl_keyboard) backends. */
static int xkb_keysym_to_luna_key(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Escape:    return LUNA_KEY_ESCAPE;
        case XKB_KEY_Return:    return LUNA_KEY_ENTER;
        case XKB_KEY_KP_Enter:  return LUNA_KEY_KP_ENTER;
        case XKB_KEY_Tab:       return LUNA_KEY_TAB;
        case XKB_KEY_BackSpace: return LUNA_KEY_BACKSPACE;
        case XKB_KEY_Delete:    return LUNA_KEY_DELETE;
        case XKB_KEY_Right:     return LUNA_KEY_RIGHT;
        case XKB_KEY_Left:      return LUNA_KEY_LEFT;
        case XKB_KEY_Down:      return LUNA_KEY_DOWN;
        case XKB_KEY_Up:        return LUNA_KEY_UP;
        case XKB_KEY_Page_Up:   return LUNA_KEY_PAGE_UP;
        case XKB_KEY_Page_Down: return LUNA_KEY_PAGE_DOWN;
        case XKB_KEY_Home:      return LUNA_KEY_HOME;
        case XKB_KEY_End:       return LUNA_KEY_END;
        case XKB_KEY_F4:        return LUNA_KEY_F4;
        case XKB_KEY_F12:       return LUNA_KEY_F12;
        case XKB_KEY_comma:     return LUNA_KEY_COMMA;
        case XKB_KEY_space:     return LUNA_KEY_SPACE;
        case XKB_KEY_Super_L:   return LUNA_KEY_LEFT_SUPER;
        case XKB_KEY_Super_R:   return LUNA_KEY_RIGHT_SUPER;
        default:
            if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return (int)(sym - XKB_KEY_a) + 'A';
            if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return (int)(sym - XKB_KEY_0) + '0';
            return -1; /* not one of the codes luna-shell/luna-ui look at */
    }
}

static int xkb_mod_bits(struct xkb_state* st) {
    if (!st) return 0;
    int mods = 0;
    if (xkb_state_mod_name_is_active(st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= LUNA_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE) > 0) mods |= LUNA_MOD_CONTROL;
    if (xkb_state_mod_name_is_active(st, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE) > 0) mods |= LUNA_MOD_ALT;
    if (xkb_state_mod_name_is_active(st, XKB_MOD_NAME_LOGO,  XKB_STATE_MODS_EFFECTIVE) > 0) mods |= LUNA_MOD_SUPER;
    return mods;
}

/* ══════════════════════ KMS / DRM / GBM / EGL backend ══════════════════ */

static struct {
    int fd;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc* saved_crtc;
    int width, height;

    struct gbm_device*  gbm_dev;
    struct gbm_surface* gbm_surf;
    struct gbm_bo*      prev_bo;

    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    int flip_pending;

    struct udev*      udev;
    struct libinput*  li;
    struct xkb_context* xkb_ctx;
    struct xkb_keymap*  xkb_keymap;
    struct xkb_state*   xkb_state;

    double mouse_x, mouse_y;

    /* DRM hardware cursor (ARGB dumb buffer + cursor plane). */
    uint32_t  cursor_handle;
    uint32_t  cursor_pitch;
    uint64_t  cursor_size;
    uint32_t* cursor_map;
    int       cursor_w, cursor_h;
    int       cursor_hot_x, cursor_hot_y;
    int       cursor_type;
    int       cursor_ok;       /* dumb buffer mapped */
    int       cursor_shown;    /* drmModeSetCursor2 succeeded */
} g_kms;

/* 64×64 is the size most KMS drivers accept for the cursor plane. */
#define KMS_CURSOR_SIZE 64

static void kms_cursor_paint(int cursor_type);
static void kms_cursor_move(void);
static int  kms_cursor_init(void);
static void kms_cursor_show(void);
static void kms_cursor_fini(void);

/* Blit the active theme frame into a destination ARGB buffer.
 * Returns 1 on success (caller should skip the built-in glyph). */
static int cursor_blit_theme(uint32_t* dst, int dst_w, int dst_h, int stride_px,
                             int cursor_type, int* hot_x, int* hot_y) {
    int requested_role = cursor_type;
    if (!luna_cur_theme_select(&g_cur_theme, requested_role)) {
        /* A third-party theme may intentionally provide only its arrow.  Do
         * not turn every unsupported hover role into a transparent cursor. */
        if (!luna_cur_theme_select(&g_cur_theme, 0)) {
            g_cur_theme.active_role = requested_role;
            return 0;
        }
    }
    const LunaCurFrame* f = luna_cur_theme_frame(&g_cur_theme);
    if (!f || !f->argb || f->w <= 0 || f->h <= 0) {
        g_cur_theme.active_role = requested_role;
        return 0;
    }

    memset(dst, 0, (size_t)dst_h * (size_t)stride_px * sizeof(uint32_t));

    /* Fit large Windows cursors instead of cropping their right/bottom edges.
     * Preserve aspect ratio, and use nearest-neighbour sampling so pixel-art
     * themes remain crisp. */
    double scale = 1.0;
    if (f->w > dst_w || f->h > dst_h) {
        double sx = (double)dst_w / (double)f->w;
        double sy = (double)dst_h / (double)f->h;
        scale = sx < sy ? sx : sy;
    }
    int copy_w = (int)floor((double)f->w * scale + 0.5);
    int copy_h = (int)floor((double)f->h * scale + 0.5);
    if (copy_w < 1) copy_w = 1;
    if (copy_h < 1) copy_h = 1;
    for (int y = 0; y < copy_h; y++) {
        int sy = y * f->h / copy_h;
        uint32_t* row = dst + y * stride_px;
        for (int x = 0; x < copy_w; x++)
            row[x] = f->argb[sy * f->w + x * f->w / copy_w];
    }
    *hot_x = (int)floor((double)f->hot_x * scale + 0.5);
    *hot_y = (int)floor((double)f->hot_y * scale + 0.5);
    if (*hot_x < 0) *hot_x = 0;
    if (*hot_y < 0) *hot_y = 0;
    if (*hot_x >= dst_w) *hot_x = dst_w / 2;
    if (*hot_y >= dst_h) *hot_y = dst_h / 2;
    g_cur_theme.active_role = requested_role;
    return 1;
}

static void cursor_theme_reload(const char* name) {
    const char* theme = name && *name ? name : "aero";
    /* Keep env in sync with GUI/settings so GTK children and reloads agree.
     * Only push XCURSOR_THEME when an on-disk XCursor tree exists — embedded
     * luna themes are not always installed under /usr/share/icons. */
    setenv("LUNA_CURSOR_THEME", theme, 1);
    if ((!strcmp(theme, "aero") || !strcmp(theme, "builtin")) &&
        access("/usr/share/icons/aero/cursors/left_ptr", R_OK) == 0)
        setenv("XCURSOR_THEME", "aero", 1);
    else if (strcmp(theme, "aero") != 0 && strcmp(theme, "builtin") != 0) {
        char path[512];
        snprintf(path, sizeof(path), "/usr/share/icons/%s/cursors/left_ptr", theme);
        if (access(path, R_OK) == 0)
            setenv("XCURSOR_THEME", theme, 1);
    }
    luna_cur_theme_load(&g_cur_theme, theme);
}

static void cursor_theme_tick_and_refresh(void) {
    if (g_cursor_reload_pending) {
        g_cursor_reload_pending = 0;
        if (g_backend && g_backend->set_cursor)
            g_backend->set_cursor(g_cur_theme.active_role);
    }
    if (!luna_cur_theme_tick(&g_cur_theme, g_now)) return;
    /* Re-push the current role so animated .ani frames advance. */
    if (g_backend && g_backend->set_cursor)
        g_backend->set_cursor(g_cur_theme.active_role);
}

/* Paint a simple ARGB cursor glyph into the dumb buffer.
 * Types match luna-ui: 0=arrow 1=pointer 2=text 3=crosshair 4=ew 5=ns. */
static void kms_cursor_paint(int cursor_type) {
    if (!g_kms.cursor_map) return;
    int stride = (int)(g_kms.cursor_pitch / 4);
    uint32_t* px = g_kms.cursor_map;
    memset(px, 0, (size_t)g_kms.cursor_size);

    g_kms.cursor_type = cursor_type;
    g_kms.cursor_hot_x = 1;
    g_kms.cursor_hot_y = 1;

    if (cursor_blit_theme(px, KMS_CURSOR_SIZE, KMS_CURSOR_SIZE, stride,
                          cursor_type, &g_kms.cursor_hot_x, &g_kms.cursor_hot_y)) {
        if (g_kms.cursor_shown) {
            if (drmModeSetCursor2(g_kms.fd, g_kms.crtc_id, g_kms.cursor_handle,
                                  g_kms.cursor_w, g_kms.cursor_h,
                                  g_kms.cursor_hot_x, g_kms.cursor_hot_y) != 0)
                drmModeSetCursor(g_kms.fd, g_kms.crtc_id, g_kms.cursor_handle,
                                 g_kms.cursor_w, g_kms.cursor_h);
            kms_cursor_move();
        }
        return;
    }

    fprintf(stderr, "[luna-shell/kms] cursor theme missing pixels for type %d\n", cursor_type);
}

static int kms_cursor_init(void) {
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = KMS_CURSOR_SIZE;
    creq.height = KMS_CURSOR_SIZE;
    creq.bpp    = 32;
    if (drmIoctl(g_kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        fprintf(stderr, "[luna-shell/kms] CREATE_DUMB(cursor) failed: %s\n", strerror(errno));
        return 0;
    }
    g_kms.cursor_handle = creq.handle;
    g_kms.cursor_pitch  = creq.pitch;
    g_kms.cursor_size   = creq.size;
    g_kms.cursor_w      = KMS_CURSOR_SIZE;
    g_kms.cursor_h      = KMS_CURSOR_SIZE;

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(g_kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        fprintf(stderr, "[luna-shell/kms] MAP_DUMB(cursor) failed: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        drmIoctl(g_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        g_kms.cursor_handle = 0;
        return 0;
    }
    g_kms.cursor_map = mmap(NULL, (size_t)creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            g_kms.fd, (off_t)mreq.offset);
    if (g_kms.cursor_map == MAP_FAILED) {
        fprintf(stderr, "[luna-shell/kms] mmap(cursor) failed: %s\n", strerror(errno));
        g_kms.cursor_map = NULL;
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        drmIoctl(g_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        g_kms.cursor_handle = 0;
        return 0;
    }
    g_kms.cursor_ok = 1;
    kms_cursor_paint(0);
    return 1;
}

static void kms_cursor_show(void) {
    if (!g_kms.cursor_ok || g_kms.cursor_shown) return;
    if (drmModeSetCursor2(g_kms.fd, g_kms.crtc_id, g_kms.cursor_handle,
                          g_kms.cursor_w, g_kms.cursor_h,
                          g_kms.cursor_hot_x, g_kms.cursor_hot_y) != 0) {
        if (drmModeSetCursor(g_kms.fd, g_kms.crtc_id, g_kms.cursor_handle,
                             g_kms.cursor_w, g_kms.cursor_h) != 0) {
            fprintf(stderr, "[luna-shell/kms] drmModeSetCursor failed: %s"
                            " (no hardware cursor plane?)\n", strerror(errno));
            return;
        }
    }
    g_kms.cursor_shown = 1;
    kms_cursor_move();
    fprintf(stderr, "[luna-shell/kms] hardware cursor enabled\n");
}

static void kms_cursor_move(void) {
    if (!g_kms.cursor_shown) return;
    drmModeMoveCursor(g_kms.fd, g_kms.crtc_id,
                      (int)g_kms.mouse_x - g_kms.cursor_hot_x,
                      (int)g_kms.mouse_y - g_kms.cursor_hot_y);
}

static void kms_cursor_fini(void) {
    if (g_kms.cursor_shown) {
        drmModeSetCursor(g_kms.fd, g_kms.crtc_id, 0, 0, 0);
        g_kms.cursor_shown = 0;
    }
    if (g_kms.cursor_map) {
        munmap(g_kms.cursor_map, (size_t)g_kms.cursor_size);
        g_kms.cursor_map = NULL;
    }
    if (g_kms.cursor_handle) {
        struct drm_mode_destroy_dumb dreq = { .handle = g_kms.cursor_handle };
        drmIoctl(g_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        g_kms.cursor_handle = 0;
    }
    g_kms.cursor_ok = 0;
}

static int kms_open_drm_device(void) {
    static const char* nodes[] = { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL };
    for (int i = 0; nodes[i]; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        if (res) { drmModeFreeResources(res); return fd; }
        close(fd);
    }
    return -1;
}

static int kms_find_display(void) {
    drmModeRes* res = drmModeGetResources(g_kms.fd);
    if (!res) { fprintf(stderr, "[luna-shell/kms] drmModeGetResources failed\n"); return 0; }

    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(g_kms.fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "[luna-shell/kms] no connected display found on this DRM device\n");
        drmModeFreeResources(res);
        return 0;
    }

    drmModeModeInfo* best = &conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { best = &conn->modes[i]; break; }
    g_kms.mode   = *best;
    g_kms.width  = best->hdisplay;
    g_kms.height = best->vdisplay;
    g_kms.conn_id = conn->connector_id;

    drmModeEncoder* enc = conn->encoder_id ? drmModeGetEncoder(g_kms.fd, conn->encoder_id) : NULL;
    uint32_t crtc_id = 0;
    if (enc && enc->crtc_id) {
        crtc_id = enc->crtc_id;
    } else {
        for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
            drmModeEncoder* e = drmModeGetEncoder(g_kms.fd, conn->encoders[i]);
            if (!e) continue;
            for (int j = 0; j < res->count_crtcs; j++)
                if (e->possible_crtcs & (1u << j)) { crtc_id = res->crtcs[j]; break; }
            drmModeFreeEncoder(e);
        }
    }
    if (enc) drmModeFreeEncoder(enc);
    if (!crtc_id) {
        fprintf(stderr, "[luna-shell/kms] no usable CRTC for the connected display\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return 0;
    }
    g_kms.crtc_id = crtc_id;
    g_kms.saved_crtc = drmModeGetCrtc(g_kms.fd, g_kms.crtc_id);

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return 1;
}

static int kms_init_gbm_egl(void) {
    g_kms.gbm_dev = gbm_create_device(g_kms.fd);
    if (!g_kms.gbm_dev) { fprintf(stderr, "[luna-shell/kms] gbm_create_device failed\n"); return 0; }

    g_kms.gbm_surf = gbm_surface_create(g_kms.gbm_dev, g_kms.width, g_kms.height,
                                         GBM_FORMAT_XRGB8888,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!g_kms.gbm_surf) { fprintf(stderr, "[luna-shell/kms] gbm_surface_create failed\n"); return 0; }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_kms.dpy = get_plat_dpy
        ? get_plat_dpy(EGL_PLATFORM_GBM_KHR, g_kms.gbm_dev, NULL)
        : eglGetDisplay((EGLNativeDisplayType)g_kms.gbm_dev);
    if (g_kms.dpy == EGL_NO_DISPLAY) { fprintf(stderr, "[luna-shell/kms] eglGetDisplay failed\n"); return 0; }

    EGLint major, minor;
    if (!eglInitialize(g_kms.dpy, &major, &minor)) {
        fprintf(stderr, "[luna-shell/kms] eglInitialize failed\n"); return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[luna-shell/kms] eglBindAPI(EGL_OPENGL_API) failed\n"); return 0;
    }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!eglChooseConfig(g_kms.dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "[luna-shell/kms] eglChooseConfig failed\n"); return 0;
    }

    /* Same version ladder as the old GLFW path: try GL 4.5 first
     * (native glCreateVertexArrays), fall back to 4.1 / 3.3 core. */
    static const int versions[][2] = { {4,5}, {4,1}, {3,3} };
    g_kms.ctx = EGL_NO_CONTEXT;
    for (int i = 0; i < (int)(sizeof(versions)/sizeof(versions[0])); i++) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, versions[i][0],
            EGL_CONTEXT_MINOR_VERSION, versions[i][1],
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        g_kms.ctx = eglCreateContext(g_kms.dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (g_kms.ctx != EGL_NO_CONTEXT) {
            fprintf(stderr, "[luna-shell/kms] OpenGL %d.%d context\n", versions[i][0], versions[i][1]);
            break;
        }
    }
    if (g_kms.ctx == EGL_NO_CONTEXT) { fprintf(stderr, "[luna-shell/kms] eglCreateContext failed\n"); return 0; }

    g_kms.surf = eglCreateWindowSurface(g_kms.dpy, cfg, (EGLNativeWindowType)g_kms.gbm_surf, NULL);
    if (g_kms.surf == EGL_NO_SURFACE) { fprintf(stderr, "[luna-shell/kms] eglCreateWindowSurface failed\n"); return 0; }

    if (!eglMakeCurrent(g_kms.dpy, g_kms.surf, g_kms.surf, g_kms.ctx)) {
        fprintf(stderr, "[luna-shell/kms] eglMakeCurrent failed\n"); return 0;
    }
    /* KMS has no window-system compositor to pace us.  drmModePageFlip() below
     * is the one (and only) vblank throttle.  Asking EGL to wait as well can
     * serialize the render at eglSwapBuffers and then wait for a second vblank
     * at PageFlip, which presents as a very regular 30 Hz hitch on console. */
    eglSwapInterval(g_kms.dpy, 0);
    return 1;
}

static int li_open_restricted(const char* path, int flags, void* user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}
static void li_close_restricted(int fd, void* user_data) { (void)user_data; close(fd); }
static const struct libinput_interface g_li_iface = {
    .open_restricted  = li_open_restricted,
    .close_restricted = li_close_restricted,
};

static int kms_init_input(void) {
    g_kms.udev = udev_new();
    if (!g_kms.udev) return 0;
    g_kms.li = libinput_udev_create_context(&g_li_iface, NULL, g_kms.udev);
    if (!g_kms.li) return 0;
    const char* seat = getenv("LUNA_SEAT");
    if (!seat) seat = "seat0";
    if (libinput_udev_assign_seat(g_kms.li, seat) != 0) {
        fprintf(stderr, "[luna-shell/kms] libinput_udev_assign_seat(%s) failed"
                        " (needs a running seatd/logind session, or CAP_SYS_ADMIN)\n", seat);
        return 0;
    }

    g_kms.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rules = {
        .rules   = getenv("XKB_DEFAULT_RULES"),
        .model   = getenv("XKB_DEFAULT_MODEL"),
        .layout  = getenv("XKB_DEFAULT_LAYOUT"),
        .variant = getenv("XKB_DEFAULT_VARIANT"),
        .options = getenv("XKB_DEFAULT_OPTIONS"),
    };
    g_kms.xkb_keymap = xkb_keymap_new_from_names(g_kms.xkb_ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_kms.xkb_keymap) /* fall back to a bare default (US) layout */
        g_kms.xkb_keymap = xkb_keymap_new_from_names(g_kms.xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_kms.xkb_keymap) return 0;
    g_kms.xkb_state = xkb_state_new(g_kms.xkb_keymap);
    return g_kms.xkb_state != NULL;
}

static void kms_process_input(void) {
    if (!g_kms.li) return;
    if (libinput_dispatch(g_kms.li) != 0) return;
    struct libinput_event* ev;
    while ((ev = libinput_get_event(g_kms.li))) {
        switch (libinput_event_get_type(ev)) {
        case LIBINPUT_EVENT_POINTER_MOTION: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            g_kms.mouse_x += libinput_event_pointer_get_dx(p);
            g_kms.mouse_y += libinput_event_pointer_get_dy(p);
            if (g_kms.mouse_x < 0) g_kms.mouse_x = 0;
            if (g_kms.mouse_y < 0) g_kms.mouse_y = 0;
            if (g_kms.mouse_x > g_kms.width)  g_kms.mouse_x = g_kms.width;
            if (g_kms.mouse_y > g_kms.height) g_kms.mouse_y = g_kms.height;
            kms_cursor_move();
            luna_mouse_move(g_kms.mouse_x, g_kms.mouse_y);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            g_kms.mouse_x = libinput_event_pointer_get_absolute_x_transformed(p, g_kms.width);
            g_kms.mouse_y = libinput_event_pointer_get_absolute_y_transformed(p, g_kms.height);
            kms_cursor_move();
            luna_mouse_move(g_kms.mouse_x, g_kms.mouse_y);
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            int btn;
            switch (libinput_event_pointer_get_button(p)) {
                case BTN_LEFT:   btn = 0; break;
                case BTN_RIGHT:  btn = 1; break;
                case BTN_MIDDLE: btn = 2; break;
                default:         btn = 3; break;
            }
            int action = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED
                       ? LUNA_PRESS : LUNA_RELEASE;
            luna_mouse_button(btn, action, xkb_mod_bits(g_kms.xkb_state), g_kms.mouse_x, g_kms.mouse_y);
            break;
        }
        case LIBINPUT_EVENT_POINTER_AXIS: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            double yv = 0, xv = 0;
            if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
                yv = libinput_event_pointer_get_axis_value(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
                xv = libinput_event_pointer_get_axis_value(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
            luna_scroll(-xv / 10.0, -yv / 10.0);
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            if (!g_kms.xkb_state) break;
            struct libinput_event_keyboard* k = libinput_event_get_keyboard_event(ev);
            uint32_t code = libinput_event_keyboard_get_key(k);
            int pressed = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
            xkb_state_update_key(g_kms.xkb_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            xkb_keysym_t sym = xkb_state_key_get_one_sym(g_kms.xkb_state, code + 8);
            int mods = xkb_mod_bits(g_kms.xkb_state);
            int lkey = xkb_keysym_to_luna_key(sym);
            if (lkey != -1)
                dispatch_key(lkey, (int)code, pressed ? LUNA_PRESS : LUNA_RELEASE, mods);
            if (pressed) {
                uint32_t cp = xkb_state_key_get_utf32(g_kms.xkb_state, code + 8);
                if (cp >= 32 && cp != 127) luna_char(cp);
            }
            break;
        }
        default: break;
        }
        libinput_event_destroy(ev);
    }
}

static void kms_fb_destroy_cb(struct gbm_bo* bo, void* data) {
    (void)bo;
    uint32_t* fb_id = data;
    drmModeRmFB(g_kms.fd, *fb_id);
    free(fb_id);
}

static uint32_t kms_fb_for_bo(struct gbm_bo* bo) {
    uint32_t* fb_id = gbm_bo_get_user_data(bo);
    if (fb_id) return *fb_id;

    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    fb_id = malloc(sizeof(uint32_t));
    if (!fb_id) return 0;
    if (drmModeAddFB(g_kms.fd, g_kms.width, g_kms.height, 24, 32, stride, handle, fb_id)) {
        fprintf(stderr, "[luna-shell/kms] drmModeAddFB failed: %s\n", strerror(errno));
        free(fb_id);
        return 0;
    }
    gbm_bo_set_user_data(bo, fb_id, kms_fb_destroy_cb);
    return *fb_id;
}

static void kms_page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
    (void)fd; (void)frame; (void)sec; (void)usec; (void)data;
    g_kms.flip_pending = 0;
}

static void kms_swap_buffers(void) {
    if (!eglSwapBuffers(g_kms.dpy, g_kms.surf)) {
        fprintf(stderr, "[luna-shell/kms] eglSwapBuffers failed (EGL 0x%x)\n",
                eglGetError());
        return;
    }
    struct gbm_bo* bo = gbm_surface_lock_front_buffer(g_kms.gbm_surf);
    if (!bo) return;
    uint32_t fb_id = kms_fb_for_bo(bo);
    if (!fb_id) {
        gbm_surface_release_buffer(g_kms.gbm_surf, bo);
        return;
    }

    if (!g_kms.prev_bo) {
        /* First frame: set the mode directly instead of page-flipping. */
        if (drmModeSetCrtc(g_kms.fd, g_kms.crtc_id, fb_id, 0, 0,
                           &g_kms.conn_id, 1, &g_kms.mode) != 0) {
            fprintf(stderr, "[luna-shell/kms] initial modeset failed: %s\n",
                    strerror(errno));
            gbm_surface_release_buffer(g_kms.gbm_surf, bo);
            return;
        }
        /* Cursor plane needs an active CRTC — enable after the first modeset. */
        kms_cursor_show();
    } else {
        g_kms.flip_pending = 1;
        int flip_queued = drmModePageFlip(g_kms.fd, g_kms.crtc_id, fb_id,
                                          DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0;
        if (flip_queued) {
            while (g_kms.flip_pending) {
                struct pollfd pfds[2];
                int nfd = 1;
                pfds[0].fd = g_kms.fd;
                pfds[0].events = POLLIN;
                /* Keep draining libinput while waiting for the flip so the
                 * pointer/keyboard never "freeze", and so we don't lose the
                 * quit chord between frames. */
                if (g_kms.li) {
                    pfds[1].fd = libinput_get_fd(g_kms.li);
                    pfds[1].events = POLLIN;
                    nfd = 2;
                }
                int pr = poll(pfds, nfd, -1);
                if (pr < 0) { if (errno == EINTR) continue; break; }
                if (nfd > 1 && (pfds[1].revents & POLLIN))
                    kms_process_input();
                if (pfds[0].revents & POLLIN) {
                    drmEventContext evctx = { .version = 2, .page_flip_handler = kms_page_flip_handler };
                    if (drmHandleEvent(g_kms.fd, &evctx) != 0) break;
                }
            }
        }
        /* The old BO is still being scanned out until the flip event arrives.
         * Releasing it after a failed/incomplete page flip lets GBM recycle the
         * visible buffer and manifests as intermittent console flicker. */
        if (!flip_queued) {
            fprintf(stderr, "[luna-shell/kms] page flip failed: %s\n",
                    strerror(errno));
            g_kms.flip_pending = 0;
            gbm_surface_release_buffer(g_kms.gbm_surf, bo);
            return;
        }
        if (g_kms.flip_pending) {
            /* The kernel accepted the flip, so either BO may be active now.
             * Keep both locked and leave cleanly rather than recycling a
             * potentially scanned-out buffer after an event-channel failure. */
            fprintf(stderr, "[luna-shell/kms] page flip completion failed\n");
            g_should_close = 1;
            return;
        }
        /* The scanout BO is handed straight from EGL/GBM to KMS: no CPU copy.
         * Release the previous BO only after the flip fence/event says KMS is
         * finished with it.  Cursor planes are independent of primary-plane
         * flips, so do not re-program/move the cursor on every frame; those two
         * synchronous DRM ioctls were needless render-thread jitter. */
        gbm_surface_release_buffer(g_kms.gbm_surf, g_kms.prev_bo);
    }
    g_kms.prev_bo = bo;
}

static int kms_backend_start(void) {
    g_kms.fd = kms_open_drm_device();
    if (g_kms.fd < 0) {
        fprintf(stderr, "[luna-shell/kms] no usable /dev/dri/cardN found"
                        " (permissions? try running as root or in the video group)\n");
        return 0;
    }
    if (!kms_find_display())   { close(g_kms.fd); return 0; }
    if (!kms_init_gbm_egl())   return 0;
    if (!kms_init_input())
        fprintf(stderr, "[luna-shell/kms] libinput setup failed —"
                        " continuing without keyboard/mouse input\n");

    g_kms.mouse_x = g_kms.width  / 2.0;
    g_kms.mouse_y = g_kms.height / 2.0;
    if (!kms_cursor_init())
        fprintf(stderr, "[luna-shell/kms] hardware cursor init failed — pointer will be invisible\n");
    luna_window_width  = (float)g_kms.width;
    luna_window_height = (float)g_kms.height;
    fprintf(stderr, "[luna-shell/kms] %dx%d @ %s\n", g_kms.width, g_kms.height, g_kms.mode.name);
    fprintf(stderr, "[luna-shell/kms] quit with Ctrl+Alt+Backspace\n");
    return 1;
}

static void kms_backend_get_fb_size(int* w, int* h) { *w = g_kms.width; *h = g_kms.height; }
static void kms_backend_poll_events(void) {
    if (g_single_poll_timeout_ms > 0) {
        struct pollfd pfd = {
            .fd = g_kms.li ? libinput_get_fd(g_kms.li) : -1,
            .events = POLLIN,
        };
        int pr;
        do {
            /* fd=-1 makes poll a timer if libinput initialization failed,
             * instead of leaving the render loop at 100% CPU. */
            pr = poll(&pfd, 1, g_single_poll_timeout_ms);
        } while (pr < 0 && errno == EINTR);
    }
    kms_process_input();
}
static void kms_backend_set_cursor(int cursor_type) {
    if (!g_kms.cursor_ok) return;
    if (cursor_type == g_kms.cursor_type && g_kms.cursor_shown) return;
    kms_cursor_paint(cursor_type);
}

static void kms_backend_terminate(void) {
    kms_cursor_fini();
    if (g_kms.li)         libinput_unref(g_kms.li);
    if (g_kms.udev)       udev_unref(g_kms.udev);
    if (g_kms.xkb_state)  xkb_state_unref(g_kms.xkb_state);
    if (g_kms.xkb_keymap) xkb_keymap_unref(g_kms.xkb_keymap);
    if (g_kms.xkb_ctx)    xkb_context_unref(g_kms.xkb_ctx);
    if (g_kms.prev_bo)    gbm_surface_release_buffer(g_kms.gbm_surf, g_kms.prev_bo);
    if (g_kms.saved_crtc) {
        drmModeSetCrtc(g_kms.fd, g_kms.saved_crtc->crtc_id, g_kms.saved_crtc->buffer_id,
                       g_kms.saved_crtc->x, g_kms.saved_crtc->y, &g_kms.conn_id, 1, &g_kms.saved_crtc->mode);
        drmModeFreeCrtc(g_kms.saved_crtc);
    }
    if (g_kms.surf != EGL_NO_SURFACE) eglDestroySurface(g_kms.dpy, g_kms.surf);
    if (g_kms.ctx  != EGL_NO_CONTEXT) eglDestroyContext(g_kms.dpy, g_kms.ctx);
    if (g_kms.dpy  != EGL_NO_DISPLAY) eglTerminate(g_kms.dpy);
    if (g_kms.gbm_surf) gbm_surface_destroy(g_kms.gbm_surf);
    if (g_kms.gbm_dev)  gbm_device_destroy(g_kms.gbm_dev);
    if (g_kms.fd >= 0)  close(g_kms.fd);
}

static const LunaBackend g_kms_backend = {
    .start        = kms_backend_start,
    .get_fb_size  = kms_backend_get_fb_size,
    .swap_buffers = kms_swap_buffers,
    .poll_events  = kms_backend_poll_events,
    .set_cursor   = kms_backend_set_cursor,
    .terminate    = kms_backend_terminate,
};

/* ══════════════════════ Wayland / EGL layer-shell multi-surface backend ══ */

/* ── Per-surface descriptor ── */
#define LUNA_SURF_BG      0
#define LUNA_SURF_MENUBAR 1
#define LUNA_SURF_DOCK    2
#define LUNA_SURF_FIRST_OL 3  /* first overlay */

typedef struct {
    const char*                    name;
    const char*                    root_id;
    uint32_t                       layer;
    uint32_t                       anchor;
    int32_t                        exclusive_zone;
    int32_t                        margin_top, margin_right, margin_bottom, margin_left;
    int                            fixed_w, fixed_h;   /* 0 = fill from anchor */
    int                            is_overlay;         /* map/unmap on demand */
    int                            is_kbd;             /* keyboard interactivity */
    /* runtime */
    int                            root_idx;
    struct wl_surface*             wl_surf;
    struct zwlr_layer_surface_v1*  layer_surf;
    struct wl_egl_window*          egl_win;
    EGLSurface                     egl_surf;
    int                            configured;
    int                            surf_w, surf_h;
    float                          doc_x, doc_y;
    int                            was_shown;          /* previous is_shown() state */
} LunaSurface;

#define ZWLR_ANCHOR_TOP    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
#define ZWLR_ANCHOR_BOTTOM ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
#define ZWLR_ANCHOR_LEFT   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
#define ZWLR_ANCHOR_RIGHT  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
#define ZWLR_ANCHOR_ALL    (ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_BOTTOM|ZWLR_ANCHOR_LEFT|ZWLR_ANCHOR_RIGHT)

static LunaSurface g_surfs[] = {
    /* bg: wallpaper only. exclusive_zone=0 + empty input region so GTK/xdg
     * windows above it receive pointer/keyboard; never steal the seat. */
    { .name="bg",           .root_id="bg_layer",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, .anchor=ZWLR_ANCHOR_ALL,
      .exclusive_zone=0, .is_kbd=0 },
    /* Keep the layer surface and its exclusive zone exactly as tall as the
     * 32px #menubar CSS box.  A 28px surface clipped the window chips and let
     * toplevel windows occupy their bottom four pixels, making the two layers
     * appear to overlap. */
    { .name="menubar",      .root_id="menubar",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT|ZWLR_ANCHOR_RIGHT,
      .exclusive_zone=32, .fixed_h=32 },
    /* dock: floating bar at the bottom */
    { .name="dock",         .root_id="dock",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_BOTTOM,
      .exclusive_zone=92, .margin_bottom=12, .fixed_w=542, .fixed_h=80 },
    /* overlays — full-screen so click-outside dismiss works */
    { .name="luna_menu",    .root_id="luna_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="win_menu",     .root_id="win_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="clip_menu",    .root_id="clip_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="cc",           .root_id="control_center",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="launchpad",    .root_id="launchpad",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="settings",     .root_id="settings_win",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="about",        .root_id="about_win",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="confirm",      .root_id="confirm_overlay",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    /* toast: small fixed surface, TOP|RIGHT anchored */
    { .name="toast",        .root_id="toast",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_RIGHT,
      .margin_top=38, .margin_right=14, .fixed_w=340, .fixed_h=76,
      .is_overlay=1 },
    /* wifi_menu: full-output overlay so position_menu_near works on Wayland */
    { .name="wifi_menu",    .root_id="wifi_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
};
#define LUNA_SURF_COUNT (int)(sizeof(g_surfs)/sizeof(g_surfs[0]))

static void shell_request_repaint(int surf_idx) {
    g_frame_dirty = 1;
    if (surf_idx < 0) {
        g_surf_dirty = (1u << LUNA_SURF_COUNT) - 1u;
        return;
    }
    if (surf_idx < LUNA_SURF_COUNT)
        g_surf_dirty |= (1u << surf_idx);
}

/* ── Global Wayland / EGL state ── */
static struct {
    struct wl_display*          display;
    struct wl_registry*         registry;
    struct wl_compositor*       compositor;
    struct wl_shm*              shm;
    struct wl_seat*             seat;
    struct wl_pointer*          pointer;
    struct wl_keyboard*         keyboard;
    struct xdg_wm_base*         wm_base;
    struct zwlr_layer_shell_v1* layer_shell;

    EGLDisplay  dpy;
    EGLContext  ctx;

    struct xkb_context* xkb_ctx;
    struct xkb_keymap*  xkb_keymap;
    struct xkb_state*   xkb_state;

    int    mods;
    double mouse_x, mouse_y;       /* document coordinates for luna-ui */
    double pointer_x, pointer_y;   /* local coordinates of g_pointer_surface */

    /* Soft cursor surface (wl_shm ARGB8888 + wl_pointer.set_cursor). */
    struct wl_surface* cursor_surf;
    struct wl_buffer*  cursor_buf;
    uint32_t*          cursor_pixels;
    size_t             cursor_bytes;
    int                cursor_fd;
    int                cursor_hot_x, cursor_hot_y;
    int                cursor_type;
    uint32_t           pointer_serial; /* last enter serial for set_cursor */
    int                pointer_entered;
} g_wl;

#define WL_CURSOR_SIZE 32

static void wl_cursor_paint(int cursor_type);
static int  wl_cursor_init(void);
static void wl_cursor_apply(void);
static void wl_cursor_fini(void);

static EGLConfig         g_wl_egl_cfg;
static LunaSurface*      g_pointer_surface = NULL;   /* surface under the cursor */

/* wl_pointer coordinates are local to the entered layer surface, whereas
 * luna-ui hit-tests in document coordinates.  Keep this conversion tied to
 * the current layout instead of relying on doc_x/doc_y from the previous
 * render frame: a layer configure can resize the document and deliver pointer
 * events in the same Wayland dispatch. */
static void wl_surface_doc_origin(const LunaSurface* s, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    if (!s || s->root_idx < 0) return;

    LunaElement* e = luna_element_at(s->root_idx);
    if (!e) return;
    *x = e->x;
    *y = e->y;
    /* Full-output overlay coordinates already match document coordinates. */
    if (s->is_overlay && !s->fixed_w && !s->fixed_h) {
        *x = 0.0f;
        *y = 0.0f;
    }
}

static void wl_refresh_pointer_doc_pos(void) {
    float ox, oy;
    wl_surface_doc_origin(g_pointer_surface, &ox, &oy);
    g_wl.mouse_x = g_wl.pointer_x + ox;
    g_wl.mouse_y = g_wl.pointer_y + oy;
}

static void wlp_enter(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* surf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p;
    g_wl.pointer_serial = s;
    g_wl.pointer_entered = 1;
    g_pointer_surface = NULL;
    for (int i = 0; i < LUNA_SURF_COUNT; i++)
        if (g_surfs[i].wl_surf == surf) { g_pointer_surface = &g_surfs[i]; break; }
    g_wl.pointer_x = wl_fixed_to_double(x);
    g_wl.pointer_y = wl_fixed_to_double(y);
    wl_refresh_pointer_doc_pos();
    wl_cursor_apply();
    luna_mouse_move(g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_leave(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* surf) {
    (void)d;(void)surf;
    g_pointer_surface = NULL;
    g_wl.pointer_entered = 0;
    /* Release the cursor role immediately so GTK (or the compositor default)
     * can own it.  Keeping our glyph after leave is what made the pointer
     * vanish on top of client windows. */
    if (p && g_wl.pointer)
        wl_pointer_set_cursor(p, s, NULL, 0, 0);
}
static void wlp_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    g_wl.pointer_x = wl_fixed_to_double(x);
    g_wl.pointer_y = wl_fixed_to_double(y);
    wl_refresh_pointer_doc_pos();
    /* set_cursor is needed on enter and when the glyph changes, not for every
     * motion event.  Avoiding a Wayland request per sample removes pointer
     * latency on high-polling-rate mice. */
    luna_mouse_move(g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_button(void* d, struct wl_pointer* p, uint32_t s, uint32_t t, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)s; (void)t;
    int btn;
    switch (button) {
        case BTN_LEFT:   btn = 0; break;
        case BTN_RIGHT:  btn = 1; break;
        case BTN_MIDDLE: btn = 2; break;
        default:         btn = 3; break;
    }
    int action = state == WL_POINTER_BUTTON_STATE_PRESSED ? LUNA_PRESS : LUNA_RELEASE;
    /* wl_pointer.button has no position.  Rebase the most recent surface-local
     * position now, so an output/layout configure immediately before a click
     * cannot send the stale document coordinate to luna-ui. */
    wl_refresh_pointer_doc_pos();
    luna_mouse_button(btn, action, g_wl.mods, g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)t;
    double v = wl_fixed_to_double(value) / 10.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) luna_scroll(0, -v);
    else luna_scroll(-v, 0);
}
/* wl_pointer v5+ events.  luna-compositor (and every modern compositor) sends
 * a frame after every motion/button/axis group.  We bind wl_seat at version 5,
 * so leaving these as NULL makes libwayland jump to a null fnptr the moment
 * the mouse moves — the classic "displays fine until the pointer moves" crash. */
static void wlp_frame(void* d, struct wl_pointer* p) {
    (void)d; (void)p;
}
static void wlp_axis_source(void* d, struct wl_pointer* p, uint32_t source) {
    (void)d; (void)p; (void)source;
}
static void wlp_axis_stop(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis) {
    (void)d; (void)p; (void)t; (void)axis;
}
static void wlp_axis_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t discrete) {
    (void)d; (void)p; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener g_wl_pointer_listener = {
    .enter = wlp_enter, .leave = wlp_leave, .motion = wlp_motion,
    .button = wlp_button, .axis = wlp_axis,
    .frame = wlp_frame, .axis_source = wlp_axis_source,
    .axis_stop = wlp_axis_stop, .axis_discrete = wlp_axis_discrete,
};

static void wlk_keymap(void* d, struct wl_keyboard* k, uint32_t format, int fd, uint32_t size) {
    (void)d; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        struct xkb_keymap* km = xkb_keymap_new_from_string(g_wl.xkb_ctx, map,
                                    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map, size);
        if (km) {
            if (g_wl.xkb_state)  xkb_state_unref(g_wl.xkb_state);
            if (g_wl.xkb_keymap) xkb_keymap_unref(g_wl.xkb_keymap);
            g_wl.xkb_keymap = km;
            g_wl.xkb_state  = xkb_state_new(km);
        }
    }
    close(fd);
}
static void wlk_enter(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* surf, struct wl_array* keys) {
    (void)d; (void)k; (void)s; (void)surf; (void)keys;
}
static void wlk_leave(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* surf) { (void)d;(void)k;(void)s;(void)surf; }
static void wlk_key(void* d, struct wl_keyboard* k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)s; (void)t;
    if (!g_wl.xkb_state) return;
    int pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    /* Keep the xkb key state in sync so get_one_sym / utf32 see the press. */
    xkb_state_update_key(g_wl.xkb_state, key + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    g_wl.mods = xkb_mod_bits(g_wl.xkb_state);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_wl.xkb_state, key + 8);
    int lkey = xkb_keysym_to_luna_key(sym);
    if (lkey != -1)
        dispatch_key(lkey, (int)key, pressed ? LUNA_PRESS : LUNA_RELEASE, g_wl.mods);
    if (pressed) {
        uint32_t cp = xkb_state_key_get_utf32(g_wl.xkb_state, key + 8);
        if (cp >= 32 && cp != 127) luna_char(cp);
    }
}
static void wlk_modifiers(void* d, struct wl_keyboard* k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)k; (void)s;
    if (g_wl.xkb_state) {
        xkb_state_update_mask(g_wl.xkb_state, dep, lat, lck, 0, 0, grp);
        g_wl.mods = xkb_mod_bits(g_wl.xkb_state);
    }
}
static void wlk_repeat_info(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d;(void)k;(void)rate;(void)delay; }
static const struct wl_keyboard_listener g_wl_keyboard_listener = {
    .keymap = wlk_keymap, .enter = wlk_enter, .leave = wlk_leave,
    .key = wlk_key, .modifiers = wlk_modifiers, .repeat_info = wlk_repeat_info,
};

static void wls_capabilities(void* d, struct wl_seat* seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl.pointer) {
        g_wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_wl.pointer, &g_wl_pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
        g_wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_wl.keyboard, &g_wl_keyboard_listener, NULL);
    }
}
static void wls_name(void* d, struct wl_seat* seat, const char* name) { (void)d;(void)seat;(void)name; }
static const struct wl_seat_listener g_wl_seat_listener = { .capabilities = wls_capabilities, .name = wls_name };

static void wm_base_ping(void* d, struct xdg_wm_base* wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener g_xdg_wm_base_listener = { .ping = wm_base_ping };

/* layer-shell surface configure: compositor sends the actual size */
static void layer_surf_configure(void* d, struct zwlr_layer_surface_v1* ls,
                                  uint32_t serial, uint32_t w, uint32_t h) {
    LunaSurface* s = (LunaSurface*)d;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    int new_w = w > 0 ? (int)w : s->surf_w;
    int new_h = h > 0 ? (int)h : s->surf_h;
    int size_changed = new_w != s->surf_w || new_h != s->surf_h;
    if (w > 0) s->surf_w = new_w;
    if (h > 0) s->surf_h = new_h;
    /* Layer-shell may configure an overlay after its first visible frame.
     * Keep wl_egl_window and the drawing viewport in lockstep with that
     * configure.  Previously only surf_w/surf_h changed, so the compositor
     * scaled the old buffer into the new full-screen layer and popups appeared
     * to jump after opening. */
    if (size_changed && s->egl_win && s->surf_w > 0 && s->surf_h > 0) {
        wl_egl_window_resize(s->egl_win, s->surf_w, s->surf_h, 0, 0);
        shell_request_repaint((int)(s - g_surfs));
    }
    /* bg surface gives us the output resolution */
    if (s == &g_surfs[LUNA_SURF_BG] && w > 0 && h > 0) {
        /* The first layer-shell configure is the authoritative desktop size.
         * Go through luna_resize so the CSS viewport is relaid out once, not
         * left with positions calculated for the bootstrap 1440x900 size. */
        luna_resize((float)w, (float)h);
    }
    s->configured = 1;
}
static void layer_surf_closed(void* d, struct zwlr_layer_surface_v1* ls) {
    (void)ls; (void)d; g_should_close = 1;
}
static const struct zwlr_layer_surface_v1_listener g_layer_surf_listener = {
    .configure = layer_surf_configure,
    .closed    = layer_surf_closed,
};

static void wl_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        g_wl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        g_wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        g_wl.seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(g_wl.seat, &g_wl_seat_listener, NULL);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g_wl.wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wl.wm_base, &g_xdg_wm_base_listener, NULL);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t bver = ver < 4 ? ver : 4;
        g_wl.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, bver);
    }
}
static void wl_registry_global_remove(void* d, struct wl_registry* reg, uint32_t name) { (void)d;(void)reg;(void)name; }
static const struct wl_registry_listener g_wl_registry_listener = {
    .global = wl_registry_global, .global_remove = wl_registry_global_remove,
};

/* ── Create a single layer-shell surface ── */
static int surf_wl_create(LunaSurface* s) {
    s->wl_surf = wl_compositor_create_surface(g_wl.compositor);
    if (!s->wl_surf) return 0;
    s->layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_wl.layer_shell, s->wl_surf, NULL, s->layer, "luna-shell");
    if (!s->layer_surf) { wl_surface_destroy(s->wl_surf); s->wl_surf = NULL; return 0; }
    zwlr_layer_surface_v1_add_listener(s->layer_surf, &g_layer_surf_listener, s);
    zwlr_layer_surface_v1_set_anchor(s->layer_surf, s->anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surf, s->exclusive_zone);
    if (s->fixed_w || s->fixed_h)
        zwlr_layer_surface_v1_set_size(s->layer_surf, (uint32_t)s->fixed_w, (uint32_t)s->fixed_h);
    if (s->margin_top || s->margin_right || s->margin_bottom || s->margin_left)
        zwlr_layer_surface_v1_set_margin(s->layer_surf,
            s->margin_top, s->margin_right, s->margin_bottom, s->margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_surf,
        s->is_kbd
            ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
            : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    /* Wallpaper must not eat hits: an empty input region lets xdg_toplevels
     * (and the dock/menubar above) receive the pointer. */
    if (s == &g_surfs[LUNA_SURF_BG] && g_wl.compositor) {
        struct wl_region* empty = wl_compositor_create_region(g_wl.compositor);
        wl_surface_set_input_region(s->wl_surf, empty);
        if (empty) wl_region_destroy(empty);
    }
    wl_surface_commit(s->wl_surf);
    return 1;
}

/* ── Create EGL window + surface for a configured layer surface ── */
static int surf_egl_create(LunaSurface* s) {
    if (s->egl_surf != EGL_NO_SURFACE) return 1;   /* already have one */
    if (s->surf_w <= 0 || s->surf_h <= 0) return 0;
    s->egl_win = wl_egl_window_create(s->wl_surf, s->surf_w, s->surf_h);
    if (!s->egl_win) return 0;
    s->egl_surf = eglCreateWindowSurface(g_wl.dpy, g_wl_egl_cfg,
                                          (EGLNativeWindowType)s->egl_win, NULL);
    if (s->egl_surf == EGL_NO_SURFACE) {
        wl_egl_window_destroy(s->egl_win); s->egl_win = NULL; return 0;
    }
    return 1;
}

/* ── Drop the EGL window of a hidden overlay ──
 * Each full-screen overlay costs a whole buffer chain (1920x1200x4 x N) on the
 * GPU, and the driver keeps them alive for as long as the EGLSurface exists.
 * Seven overlays that are hidden almost all of the time are not worth a
 * permanent ~200 MB, so their buffers are released on hide and reallocated on
 * the next open.  The layer surface itself stays around, so no re-negotiation
 * with the compositor is needed. */
static void surf_egl_destroy(LunaSurface* s) {
    if (s->egl_surf == EGL_NO_SURFACE) return;
    /* Never destroy the surface the context is currently bound to. */
    if (eglGetCurrentSurface(EGL_DRAW) == s->egl_surf)
        eglMakeCurrent(g_wl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_wl.dpy, s->egl_surf);
    s->egl_surf = EGL_NO_SURFACE;
    if (s->egl_win) { wl_egl_window_destroy(s->egl_win); s->egl_win = NULL; }
}

/* Report a Wayland connection error and stop the main loop.
 *
 * Once the compositor has raised a protocol error the connection is dead, but
 * libwayland keeps handing out proxies and Mesa keeps trying to allocate
 * buffers on them — its Wayland/EGL path then walks into create_wl_buffer()
 * with a NULL image and segfaults inside libgallium, which is what the
 * "segfault at 0 ... in libgallium" report looks like.  Checking the display
 * error after every swap turns that into a readable message plus a clean exit. */
static int wl_check_error(const char* where) {
    if (!g_wl.display || g_should_close) return g_should_close;
    int err = wl_display_get_error(g_wl.display);
    if (!err) return 0;
    if (err == EPROTO) {
        const struct wl_interface* iface = NULL;
        uint32_t obj_id = 0;
        uint32_t code = wl_display_get_protocol_error(g_wl.display, &iface, &obj_id);
        fprintf(stderr,
                "[luna-shell/wl] compositor raised a protocol error during %s: "
                "%s@%u code %u\n", where,
                iface ? iface->name : "(unknown)", obj_id, code);
        if (iface && !strncmp(iface->name, "zwp_linux_", 10))
            fprintf(stderr,
                    "[luna-shell/wl] the compositor could not import our GPU buffer.\n"
                    "  Leave LUNA_ENABLE_DMABUF unset (default) so the compositor uses wl_shm.\n");
    } else {
        fprintf(stderr, "[luna-shell/wl] Wayland connection lost during %s: %s\n",
                where, strerror(err));
    }
    g_should_close = 1;
    return 1;
}

/* ── Free a layer surface (used when unmap is permanent, e.g. on close) ── */
static void surf_destroy(LunaSurface* s) {
    surf_egl_destroy(s);
    if (s->layer_surf) { zwlr_layer_surface_v1_destroy(s->layer_surf); s->layer_surf = NULL; }
    if (s->wl_surf)    { wl_surface_destroy(s->wl_surf);       s->wl_surf    = NULL; }
    s->configured = 0; s->was_shown = 0;
}

/* ── Show / hide an overlay ──
 * Hidden overlays are unmapped (attach a NULL buffer) rather than left mapped
 * and fully transparent: an unmapped surface costs the compositor nothing per
 * frame, while a mapped full-screen one is blended over the whole screen on
 * every single composite.  The input region is cleared as well so clicks fall
 * through to whatever is underneath. */
static void surf_set_shown(LunaSurface* s, int active) {
    if (!s->wl_surf) return;
    if (active) {
        wl_surface_set_input_region(s->wl_surf, NULL);
        /* Buffer + map happen on the next wl_surf_render()/eglSwapBuffers(). */
        if (!surf_egl_create(s))
            fprintf(stderr, "[luna-shell/wl] could not allocate '%s' overlay surface\n", s->name);
    } else {
        struct wl_region* empty = wl_compositor_create_region(g_wl.compositor);
        wl_surface_set_input_region(s->wl_surf, empty);
        if (empty) wl_region_destroy(empty);
        wl_surface_attach(s->wl_surf, NULL, 0, 0);   /* unmap */
    }
    wl_surface_commit(s->wl_surf);
    if (!active) surf_egl_destroy(s);
}

/* A surface only takes part in the frame loop when it has something to show. */
static int surf_is_live(const LunaSurface* s) {
    return s->egl_surf != EGL_NO_SURFACE && (!s->is_overlay || s->was_shown);
}

static int wl_backend_start(void) {
    /* Designated initialisers leave root_idx as 0 (= element 0).  Until
     * bind_indices() resolves root_id, treat every surface as unbound so
     * overlays are not mistaken for "shown" and we never wl_region_destroy(NULL). */
    for (int i = 0; i < LUNA_SURF_COUNT; i++)
        g_surfs[i].root_idx = -1;

    g_wl.display = wl_display_connect(NULL);
    if (!g_wl.display) { fprintf(stderr, "[luna-shell/wl] wl_display_connect failed\n"); return 0; }

    g_wl.registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(g_wl.registry, &g_wl_registry_listener, NULL);
    wl_display_roundtrip(g_wl.display);
    if (!g_wl.compositor || !g_wl.layer_shell) {
        fprintf(stderr, "[luna-shell/wl] compositor is missing wl_compositor or zwlr_layer_shell_v1\n");
        if (!g_wl.layer_shell) fprintf(stderr, "[luna-shell/wl] hint: run under a wlroots compositor (sway, wayfire, etc.)\n");
        return 0;
    }

    g_wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    /* EGL display — explicitly request Wayland platform */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_wl.dpy = get_plat_dpy
        ? get_plat_dpy(EGL_PLATFORM_WAYLAND_KHR, g_wl.display, NULL)
        : eglGetDisplay((EGLNativeDisplayType)g_wl.display);
    EGLint major, minor;
    if (g_wl.dpy == EGL_NO_DISPLAY || !eglInitialize(g_wl.dpy, &major, &minor)) {
        fprintf(stderr, "[luna-shell/wl] eglInitialize failed\n"); return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[luna-shell/wl] eglBindAPI(EGL_OPENGL_API) failed\n"); return 0;
    }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLint n_cfg = 0;
    if (!eglChooseConfig(g_wl.dpy, cfg_attribs, &g_wl_egl_cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "[luna-shell/wl] eglChooseConfig failed\n"); return 0;
    }

    /* Single shared GL context for all surfaces */
    static const int versions[][2] = { {4,5}, {4,1}, {3,3} };
    g_wl.ctx = EGL_NO_CONTEXT;
    for (int i = 0; i < (int)(sizeof(versions)/sizeof(versions[0])); i++) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, versions[i][0],
            EGL_CONTEXT_MINOR_VERSION, versions[i][1],
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        g_wl.ctx = eglCreateContext(g_wl.dpy, g_wl_egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (g_wl.ctx != EGL_NO_CONTEXT) {
            fprintf(stderr, "[luna-shell/wl] OpenGL %d.%d (shared context)\n",
                    versions[i][0], versions[i][1]);
            break;
        }
    }
    if (g_wl.ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[luna-shell/wl] eglCreateContext failed\n"); return 0;
    }

    /* Create all layer-shell surfaces, commit to get configure events */
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        s->egl_surf = EGL_NO_SURFACE;
        if (!surf_wl_create(s)) {
            fprintf(stderr, "[luna-shell/wl] failed to create surface '%s'\n", s->name);
            return 0;
        }
    }
    wl_display_flush(g_wl.display);

    /* Wait for ALL static surfaces to configure; overlays may take longer */
    int timeout = 200;
    while (timeout-- > 0) {
        int all_ready = 1;
        for (int i = 0; i < LUNA_SURF_COUNT; i++)
            if (!g_surfs[i].is_overlay && !g_surfs[i].configured) { all_ready = 0; break; }
        if (all_ready) break;
        if (wl_display_dispatch(g_wl.display) < 0) break;
    }
    /* Wait for overlays too (best-effort) */
    wl_display_roundtrip(g_wl.display);

    /* bg surface now has the real output dimensions */
    if (g_surfs[LUNA_SURF_BG].surf_w > 0 && g_surfs[LUNA_SURF_BG].surf_h > 0) {
        luna_resize((float)g_surfs[LUNA_SURF_BG].surf_w,
                    (float)g_surfs[LUNA_SURF_BG].surf_h);
    }

    /* Assign default sizes for surfaces that haven't had configure yet */
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        if (s->surf_w <= 0) s->surf_w = s->fixed_w > 0 ? s->fixed_w : (int)luna_window_width;
        if (s->surf_h <= 0) s->surf_h = s->fixed_h > 0 ? s->fixed_h : (int)luna_window_height;
    }

    /* EGL windows for the surfaces that are always on screen.  Overlays get
     * theirs the first time they are opened — allocating buffer chains for
     * eight full-screen surfaces up front is both slow and, on a GPU that is
     * short on memory, a way to make the very first eglSwapBuffers() fail. */
    for (int i = 0; i < LUNA_SURF_FIRST_OL; i++) {
        LunaSurface* s = &g_surfs[i];
        if (!surf_egl_create(s)) {
            fprintf(stderr, "[luna-shell/wl] eglCreateWindowSurface failed for '%s' (EGL 0x%x)\n",
                    s->name, eglGetError());
            return 0;
        }
        if (i == LUNA_SURF_BG) {
            if (!eglMakeCurrent(g_wl.dpy, s->egl_surf, s->egl_surf, g_wl.ctx)) {
                fprintf(stderr, "[luna-shell/wl] eglMakeCurrent failed (bg, EGL 0x%x)\n", eglGetError());
                return 0;
            }
            /* Throttle on the background surface only.  With one swap interval
             * per surface every frame would block on several frame callbacks
             * in a row instead of one. */
            eglSwapInterval(g_wl.dpy, 1);
        } else {
            eglMakeCurrent(g_wl.dpy, s->egl_surf, s->egl_surf, g_wl.ctx);
            eglSwapInterval(g_wl.dpy, 0);
        }
    }
    eglMakeCurrent(g_wl.dpy, g_surfs[LUNA_SURF_BG].egl_surf,
                   g_surfs[LUNA_SURF_BG].egl_surf, g_wl.ctx);

    /* Overlays start hidden: no buffer, no input region */
    for (int i = LUNA_SURF_FIRST_OL; i < LUNA_SURF_COUNT; i++)
        surf_set_shown(&g_surfs[i], 0);

    wl_cursor_init();
    return 1;
}

/* ── Render one surface (skipped when clean — see g_surf_dirty) ── */
static void wl_surf_render(LunaSurface* s, int surf_idx) {
    if (!surf_is_live(s) || s->surf_w <= 0 || s->surf_h <= 0) return;
    if (surf_idx >= 0 && surf_idx < LUNA_SURF_COUNT &&
        (g_surf_dirty & (1u << surf_idx)) == 0)
        return;
    /* Drain any pending events before touching Mesa's EGL path: a protocol
     * error already queued here means the connection is dead, and calling
     * eglSwapBuffers() on a dead connection walks into create_wl_buffer()
     * with a NULL image — the "segfault in libgallium" crash. */
    if (wl_check_error("pre-swap")) return;
    if (!eglMakeCurrent(g_wl.dpy, s->egl_surf, s->egl_surf, g_wl.ctx)) {
        fprintf(stderr, "[luna-shell/wl] eglMakeCurrent failed for '%s' (EGL 0x%x)\n",
                s->name, eglGetError());
        g_should_close = 1;
        return;
    }
    /* fbw/fbh = surface pixel size (no HiDPI scaling here yet) */
    int fw = s->surf_w, fh = s->surf_h;
    glViewport(0, 0, fw, fh);
    /* Wallpaper is opaque — clear to night black so a missed paint never
     * flashes through as fully transparent. Chrome layers stay transparent. */
    if (surf_idx == LUNA_SURF_BG)
        glClearColor(0.02f, 0.02f, 0.06f, 1.0f);
    else
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s->root_idx >= 0) {
        luna_render_region(s->root_idx, fw, fh,
                           s->doc_x, s->doc_y,
                           (float)fw, (float)fh);
    } else {
        /* bg: no root filter, full-document render */
        luna_render(fw, fh);
    }
    if (!eglSwapBuffers(g_wl.dpy, s->egl_surf)) {
        EGLint err = eglGetError();
        fprintf(stderr, "[luna-shell/wl] eglSwapBuffers failed for '%s' (EGL 0x%x)\n",
                s->name, err);
        if (err == EGL_BAD_ALLOC)
            fprintf(stderr, "[luna-shell/wl] the driver could not allocate a buffer for this surface\n");
        g_should_close = 1;
        return;
    }
    /* A protocol error is fatal for the connection; carrying on would only
     * take us back into the driver's Wayland buffer path with a dead display. */
    wl_check_error("eglSwapBuffers");
    if (surf_idx >= 0 && surf_idx < LUNA_SURF_COUNT)
        g_surf_dirty &= ~(1u << surf_idx);
    if (surf_idx == LUNA_SURF_BG)
        g_last_bg_paint = g_now;
}

/* ── Update surface doc origins from layout and handle overlay show/hide ── */
static void wl_surfs_update(void) {
    /* Update doc_x/doc_y for positioned surfaces from luna layout */
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        if (s->root_idx < 0) continue;
        wl_surface_doc_origin(s, &s->doc_x, &s->doc_y);
    }

    /* Handle overlay visibility changes */
    for (int i = LUNA_SURF_FIRST_OL; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        int shown = (s->root_idx >= 0) ? is_shown(s->root_idx) : 0;
        if (shown != s->was_shown) {
            s->was_shown = shown;
            surf_set_shown(s, shown);
            if (shown) shell_request_repaint(i);
        }
    }
}

static void wl_backend_get_fb_size(int* w, int* h) {
    /* Main size = background surface (= output size) */
    *w = g_surfs[LUNA_SURF_BG].surf_w;
    *h = g_surfs[LUNA_SURF_BG].surf_h;
}

/* swap_buffers: used for single-surface backends; nop here (per-surface in frame loop) */
static void wl_backend_swap_buffers(void) {}

static void wl_backend_poll_events(void) {
    /* Read new messages from the socket, then dispatch.  dispatch_pending
     * alone only drains what Mesa already pulled in during eglSwapBuffers —
     * without a prepare_read/read_events cycle, input can stall after the
     * first few frames and leave the shell unable to see quit chords. */
    while (wl_display_prepare_read(g_wl.display) != 0) {
        if (wl_display_dispatch_pending(g_wl.display) < 0) {
            wl_check_error("event dispatch");
            return;
        }
    }
    wl_display_flush(g_wl.display);
    struct pollfd pfd = { .fd = wl_display_get_fd(g_wl.display), .events = POLLIN };
    int pr = poll(&pfd, 1, g_wl_poll_timeout_ms);
    if (pr < 0) {
        wl_display_cancel_read(g_wl.display);
        if (errno != EINTR) wl_check_error("event poll");
        return;
    }
    if (pr > 0) {
        if (wl_display_read_events(g_wl.display) < 0) {
            wl_check_error("event read");
            return;
        }
    } else {
        wl_display_cancel_read(g_wl.display);
    }
    if (wl_display_dispatch_pending(g_wl.display) < 0 ||
        wl_display_flush(g_wl.display) < 0)
        wl_check_error("event dispatch");
}

/* Paint ARGB8888 cursor glyphs into the shm buffer (same shapes as KMS). */
static void wl_cursor_paint(int cursor_type) {
    if (!g_wl.cursor_pixels) return;
    uint32_t* px = g_wl.cursor_pixels;
    memset(px, 0, g_wl.cursor_bytes);
    g_wl.cursor_type = cursor_type;
    g_wl.cursor_hot_x = 1;
    g_wl.cursor_hot_y = 1;

    if (cursor_blit_theme(px, WL_CURSOR_SIZE, WL_CURSOR_SIZE, WL_CURSOR_SIZE,
                          cursor_type, &g_wl.cursor_hot_x, &g_wl.cursor_hot_y)) {
        if (g_wl.cursor_surf && g_wl.cursor_buf) {
            wl_surface_attach(g_wl.cursor_surf, g_wl.cursor_buf, 0, 0);
            wl_surface_damage(g_wl.cursor_surf, 0, 0, WL_CURSOR_SIZE, WL_CURSOR_SIZE);
            wl_surface_commit(g_wl.cursor_surf);
        }
        wl_cursor_apply();
        return;
    }

    fprintf(stderr, "[luna-shell/wl] cursor theme missing pixels for type %d\n", cursor_type);
}

static void wl_cursor_apply(void) {
    if (!g_wl.pointer || !g_wl.cursor_surf || !g_wl.pointer_entered) return;
    wl_pointer_set_cursor(g_wl.pointer, g_wl.pointer_serial,
                          g_wl.cursor_surf, g_wl.cursor_hot_x, g_wl.cursor_hot_y);
}

static int wl_cursor_init(void) {
    if (!g_wl.compositor || !g_wl.shm) {
        fprintf(stderr, "[luna-shell/wl] wl_shm unavailable — compositor will draw its default cursor\n");
        return 0;
    }
    g_wl.cursor_bytes = (size_t)WL_CURSOR_SIZE * WL_CURSOR_SIZE * 4;
    g_wl.cursor_fd = -1;

    char path[] = "/tmp/luna-cursor-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "[luna-shell/wl] mkstemp(cursor) failed: %s\n", strerror(errno));
        return 0;
    }
    unlink(path);
    if (ftruncate(fd, (off_t)g_wl.cursor_bytes) != 0) {
        fprintf(stderr, "[luna-shell/wl] ftruncate(cursor) failed: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    void* map = mmap(NULL, g_wl.cursor_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "[luna-shell/wl] mmap(cursor) failed: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(g_wl.shm, fd, (int32_t)g_wl.cursor_bytes);
    if (!pool) {
        munmap(map, g_wl.cursor_bytes);
        close(fd);
        return 0;
    }
    g_wl.cursor_buf = wl_shm_pool_create_buffer(pool, 0,
        WL_CURSOR_SIZE, WL_CURSOR_SIZE, WL_CURSOR_SIZE * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!g_wl.cursor_buf) {
        munmap(map, g_wl.cursor_bytes);
        close(fd);
        return 0;
    }

    g_wl.cursor_surf = wl_compositor_create_surface(g_wl.compositor);
    if (!g_wl.cursor_surf) {
        wl_buffer_destroy(g_wl.cursor_buf); g_wl.cursor_buf = NULL;
        munmap(map, g_wl.cursor_bytes);
        close(fd);
        return 0;
    }

    g_wl.cursor_pixels = (uint32_t*)map;
    g_wl.cursor_fd = fd;
    g_wl.cursor_type = -1;
    wl_cursor_paint(0);
    fprintf(stderr, "[luna-shell/wl] software cursor ready\n");
    return 1;
}

static void wl_cursor_fini(void) {
    if (g_wl.cursor_surf) { wl_surface_destroy(g_wl.cursor_surf); g_wl.cursor_surf = NULL; }
    if (g_wl.cursor_buf)  { wl_buffer_destroy(g_wl.cursor_buf);  g_wl.cursor_buf  = NULL; }
    if (g_wl.cursor_pixels) {
        munmap(g_wl.cursor_pixels, g_wl.cursor_bytes);
        g_wl.cursor_pixels = NULL;
    }
    if (g_wl.cursor_fd >= 0) { close(g_wl.cursor_fd); g_wl.cursor_fd = -1; }
}

static void wl_backend_set_cursor(int cursor_type) {
    if (!g_wl.cursor_pixels) return;
    if (cursor_type != g_wl.cursor_type)
        wl_cursor_paint(cursor_type);
    wl_cursor_apply();
}

static void wl_backend_terminate(void) {
    wl_cursor_fini();
    for (int i = 0; i < LUNA_SURF_COUNT; i++) surf_destroy(&g_surfs[i]);
    if (g_wl.ctx != EGL_NO_CONTEXT)       eglDestroyContext(g_wl.dpy, g_wl.ctx);
    if (g_wl.dpy != EGL_NO_DISPLAY)       eglTerminate(g_wl.dpy);
    if (g_wl.xkb_state)  xkb_state_unref(g_wl.xkb_state);
    if (g_wl.xkb_keymap) xkb_keymap_unref(g_wl.xkb_keymap);
    if (g_wl.xkb_ctx)    xkb_context_unref(g_wl.xkb_ctx);
    if (g_wl.display)    wl_display_disconnect(g_wl.display);
}

static const LunaBackend g_wl_backend = {
    .start        = wl_backend_start,
    .get_fb_size  = wl_backend_get_fb_size,
    .swap_buffers = wl_backend_swap_buffers,
    .poll_events  = wl_backend_poll_events,
    .set_cursor   = wl_backend_set_cursor,
    .terminate    = wl_backend_terminate,
};

/*
  X11 / EGL backend ═══════════════════════════
 Enabled at compile time with -DLUNA_BACKEND_X11 -lX11.
 Selected at runtime when DISPLAY is set and WAYLAND_DISPLAY is not.
*/
#ifdef LUNA_BACKEND_X11

/* Traditional X11 panels use freedesktop's XEmbed tray protocol.  This is
 * distinct from Luna's native compositor-state tray above: it lets classic
 * tray clients (such as luna-wifi) embed when the shell runs on X11. */
#define X11_TRAY_ICON_SIZE 24
#define X11_TRAY_MAX_ICONS 12
#define X11_SYSTEM_TRAY_REQUEST_DOCK 0
#define X11_XEMBED_EMBEDDED_NOTIFY 0

static struct {
    Display* display;
    Window   window;
    Window   tray_host;
    Atom     wm_delete_window;
    Atom     tray_selection, tray_opcode, tray_manager, xembed;
    Window   tray_icons[X11_TRAY_MAX_ICONS];
    int      tray_icon_count;

    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;

    int width, height;
    int mods;
    double mouse_x, mouse_y;

    struct xkb_context* xkb_ctx;
    struct xkb_keymap*  xkb_keymap;
    struct xkb_state*   xkb_state;
} g_x11;

static void x11_tray_layout(void) {
    const int host_w = X11_TRAY_MAX_ICONS * (X11_TRAY_ICON_SIZE + 2) + 4;
    int x = g_x11.width - host_w - 4;
    if (x < 0) x = 0;
    /* This is a separate override-redirect top-level, never a child of the
     * full-screen EGL window.  Some legacy XEmbed clients inherit their
     * parent geometry while docking; keeping the tray host independent means
     * they can never see or occupy the desktop-sized shell surface. */
    if (g_x11.tray_host)
        XMoveResizeWindow(g_x11.display, g_x11.tray_host, x, 2,
                          host_w, X11_TRAY_ICON_SIZE + 2);
    for (int i = 0; i < g_x11.tray_icon_count; i++)
        XMoveResizeWindow(g_x11.display, g_x11.tray_icons[i],
                          2 + i * (X11_TRAY_ICON_SIZE + 2), 1,
                          X11_TRAY_ICON_SIZE, X11_TRAY_ICON_SIZE);
}

static void x11_tray_remove(Window icon) {
    for (int i = 0; i < g_x11.tray_icon_count; i++) {
        if (g_x11.tray_icons[i] != icon) continue;
        memmove(&g_x11.tray_icons[i], &g_x11.tray_icons[i + 1],
                (size_t)(g_x11.tray_icon_count - i - 1) * sizeof(Window));
        g_x11.tray_icon_count--;
        x11_tray_layout();
        return;
    }
}

static int x11_tray_contains(Window icon) {
    for (int i = 0; i < g_x11.tray_icon_count; i++)
        if (g_x11.tray_icons[i] == icon) return 1;
    return 0;
}

static void x11_tray_dock(Window icon) {
    if (!icon || g_x11.tray_icon_count >= X11_TRAY_MAX_ICONS) return;
    for (int i = 0; i < g_x11.tray_icon_count; i++)
        if (g_x11.tray_icons[i] == icon) return;
    g_x11.tray_icons[g_x11.tray_icon_count++] = icon;
    /* Intercept ConfigureRequest as well: several legacy tray clients keep
     * their pre-dock toplevel geometry and otherwise request it again after
     * receiving _XEMBED_EMBEDDED_NOTIFY. */
    XSelectInput(g_x11.display, icon, StructureNotifyMask);
    XReparentWindow(g_x11.display, icon, g_x11.tray_host, 0, 0);
    XMapRaised(g_x11.display, icon);
    x11_tray_layout();
    XEvent embedded;
    memset(&embedded, 0, sizeof(embedded));
    embedded.xclient.type = ClientMessage;
    embedded.xclient.window = icon;
    embedded.xclient.message_type = g_x11.xembed;
    embedded.xclient.format = 32;
    embedded.xclient.data.l[0] = CurrentTime;
    embedded.xclient.data.l[1] = X11_XEMBED_EMBEDDED_NOTIFY;
    embedded.xclient.data.l[3] = g_x11.tray_host;
    XSendEvent(g_x11.display, icon, False, NoEventMask, &embedded);
}

static void x11_tray_claim(void) {
    g_x11.tray_selection = XInternAtom(g_x11.display, "_NET_SYSTEM_TRAY_S0", False);
    g_x11.tray_opcode = XInternAtom(g_x11.display, "_NET_SYSTEM_TRAY_OPCODE", False);
    g_x11.tray_manager = XInternAtom(g_x11.display, "MANAGER", False);
    g_x11.xembed = XInternAtom(g_x11.display, "_XEMBED", False);
    XSetSelectionOwner(g_x11.display, g_x11.tray_selection, g_x11.tray_host, CurrentTime);
    if (XGetSelectionOwner(g_x11.display, g_x11.tray_selection) != g_x11.tray_host) {
        fprintf(stderr, "[luna-shell/x11] another system tray owns _NET_SYSTEM_TRAY_S0\n");
        return;
    }
    XEvent manager;
    memset(&manager, 0, sizeof(manager));
    manager.xclient.type = ClientMessage;
    manager.xclient.window = RootWindow(g_x11.display, DefaultScreen(g_x11.display));
    manager.xclient.message_type = g_x11.tray_manager;
    manager.xclient.format = 32;
    manager.xclient.data.l[0] = CurrentTime;
    manager.xclient.data.l[1] = g_x11.tray_selection;
    manager.xclient.data.l[2] = g_x11.tray_host;
    XSendEvent(g_x11.display, manager.xclient.window, False, StructureNotifyMask, &manager);
}

static int x11_mod_bits(unsigned int x11state) {
    int mods = 0;
    if (x11state & ShiftMask)   mods |= LUNA_MOD_SHIFT;
    if (x11state & ControlMask) mods |= LUNA_MOD_CONTROL;
    if (x11state & Mod1Mask)    mods |= LUNA_MOD_ALT;
    if (x11state & Mod4Mask)    mods |= LUNA_MOD_SUPER;
    return mods;
}

static int x11_backend_start(void) {
    g_x11.display = XOpenDisplay(NULL);
    if (!g_x11.display) { fprintf(stderr, "[luna-shell/x11] XOpenDisplay failed\n"); return 0; }

    int screen = DefaultScreen(g_x11.display);
    Window root = RootWindow(g_x11.display, screen);

    g_x11.width  = (int)luna_window_width;
    g_x11.height = (int)luna_window_height;

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_x11.dpy = get_plat_dpy
        ? get_plat_dpy(EGL_PLATFORM_X11_KHR, g_x11.display, NULL)
        : eglGetDisplay((EGLNativeDisplayType)g_x11.display);
    EGLint major, minor;
    if (g_x11.dpy == EGL_NO_DISPLAY || !eglInitialize(g_x11.dpy, &major, &minor)) {
        fprintf(stderr, "[luna-shell/x11] eglInitialize failed\n"); return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[luna-shell/x11] eglBindAPI(EGL_OPENGL_API) failed\n"); return 0;
    }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!eglChooseConfig(g_x11.dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "[luna-shell/x11] eglChooseConfig failed\n"); return 0;
    }

    EGLint visual_id = 0;
    eglGetConfigAttrib(g_x11.dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual_id);
    XVisualInfo vinfo_tpl; vinfo_tpl.visualid = (VisualID)visual_id;
    int n_vis = 0;
    XVisualInfo* vinfo = XGetVisualInfo(g_x11.display, VisualIDMask, &vinfo_tpl, &n_vis);

    XSetWindowAttributes swa;
    unsigned long swa_mask;
    if (vinfo && n_vis > 0) {
        swa.colormap  = XCreateColormap(g_x11.display, root, vinfo->visual, AllocNone);
        swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                         StructureNotifyMask;
        swa_mask = CWColormap | CWEventMask;
        g_x11.window = XCreateWindow(g_x11.display, root,
            0, 0, (unsigned)g_x11.width, (unsigned)g_x11.height, 0,
            vinfo->depth, InputOutput, vinfo->visual, swa_mask, &swa);
        XFree(vinfo);
    } else {
        if (vinfo) XFree(vinfo);
        swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                         StructureNotifyMask;
        swa_mask = CWEventMask;
        g_x11.window = XCreateSimpleWindow(g_x11.display, root,
            0, 0, (unsigned)g_x11.width, (unsigned)g_x11.height, 0,
            BlackPixel(g_x11.display, screen), BlackPixel(g_x11.display, screen));
        XSelectInput(g_x11.display, g_x11.window, swa.event_mask);
    }

    XStoreName(g_x11.display, g_x11.window, "Luna Desktop");
    g_x11.wm_delete_window = XInternAtom(g_x11.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11.display, g_x11.window, &g_x11.wm_delete_window, 1);

    if (g_fullscreen) {
        Atom wm_state   = XInternAtom(g_x11.display, "_NET_WM_STATE", False);
        Atom fullscreen = XInternAtom(g_x11.display, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(g_x11.display, g_x11.window,
                        wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char*)&fullscreen, 1);
    }
    /* Separate, clipped XEmbed viewport.  It must be a root child instead
     * of a shell child: an embedded client must not be able to derive the
     * shell's desktop-sized allocation from its X parent. */
    XSetWindowAttributes tray_attrs;
    memset(&tray_attrs, 0, sizeof(tray_attrs));
    tray_attrs.override_redirect = True;
    tray_attrs.background_pixel = BlackPixel(g_x11.display, screen);
    g_x11.tray_host = XCreateWindow(g_x11.display, root,
                                    0, 2, 1, X11_TRAY_ICON_SIZE + 2, 0,
                                    CopyFromParent, InputOutput, CopyFromParent,
                                    CWOverrideRedirect | CWBackPixel, &tray_attrs);
    XSelectInput(g_x11.display, g_x11.tray_host,
                 StructureNotifyMask | SubstructureNotifyMask | SubstructureRedirectMask);
    x11_tray_layout();
    XMapWindow(g_x11.display, g_x11.window);
    /* Map after the desktop so this small root-level tray stays above the
     * fullscreen EGL surface instead of being obscured by it. */
    XMapRaised(g_x11.display, g_x11.tray_host);
    x11_tray_claim();
    XFlush(g_x11.display);

    static const int versions[][2] = { {4,5}, {4,1}, {3,3} };
    g_x11.ctx = EGL_NO_CONTEXT;
    for (int i = 0; i < (int)(sizeof(versions)/sizeof(versions[0])); i++) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, versions[i][0],
            EGL_CONTEXT_MINOR_VERSION, versions[i][1],
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        g_x11.ctx = eglCreateContext(g_x11.dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (g_x11.ctx != EGL_NO_CONTEXT) {
            fprintf(stderr, "[luna-shell/x11] OpenGL %d.%d context\n", versions[i][0], versions[i][1]);
            break;
        }
    }
    if (g_x11.ctx == EGL_NO_CONTEXT) { fprintf(stderr, "[luna-shell/x11] eglCreateContext failed\n"); return 0; }

    g_x11.surf = eglCreateWindowSurface(g_x11.dpy, cfg, (EGLNativeWindowType)g_x11.window, NULL);
    if (g_x11.surf == EGL_NO_SURFACE) { fprintf(stderr, "[luna-shell/x11] eglCreateWindowSurface failed\n"); return 0; }
    if (!eglMakeCurrent(g_x11.dpy, g_x11.surf, g_x11.surf, g_x11.ctx)) {
        fprintf(stderr, "[luna-shell/x11] eglMakeCurrent failed\n"); return 0;
    }
    eglSwapInterval(g_x11.dpy, 1);

    g_x11.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rules = {
        .rules   = getenv("XKB_DEFAULT_RULES"),
        .model   = getenv("XKB_DEFAULT_MODEL"),
        .layout  = getenv("XKB_DEFAULT_LAYOUT"),
        .variant = getenv("XKB_DEFAULT_VARIANT"),
        .options = getenv("XKB_DEFAULT_OPTIONS"),
    };
    g_x11.xkb_keymap = xkb_keymap_new_from_names(g_x11.xkb_ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_x11.xkb_keymap)
        g_x11.xkb_keymap = xkb_keymap_new_from_names(g_x11.xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (g_x11.xkb_keymap)
        g_x11.xkb_state = xkb_state_new(g_x11.xkb_keymap);

    luna_window_width  = (float)g_x11.width;
    luna_window_height = (float)g_x11.height;
    fprintf(stderr, "[luna-shell/x11] %dx%d\n", g_x11.width, g_x11.height);
    return 1;
}

static void x11_backend_get_fb_size(int* w, int* h) { *w = g_x11.width; *h = g_x11.height; }
static void x11_backend_swap_buffers(void) { eglSwapBuffers(g_x11.dpy, g_x11.surf); }

static void x11_process_events(void) {
    while (XPending(g_x11.display)) {
        XEvent ev;
        XNextEvent(g_x11.display, &ev);
        switch (ev.type) {
        case ConfigureNotify:
            /* The XEmbed tray host is a second window on this Display.
             * Its fixed 316x26 geometry must never become the desktop
             * viewport (doing so shrinks the entire Luna render target to
             * the tray's size). */
            if (ev.xconfigure.window != g_x11.window)
                break;
            if (ev.xconfigure.width  != g_x11.width ||
                ev.xconfigure.height != g_x11.height) {
                g_x11.width  = ev.xconfigure.width;
                g_x11.height = ev.xconfigure.height;
                luna_window_width  = (float)g_x11.width;
                luna_window_height = (float)g_x11.height;
                x11_tray_layout();
            }
            break;
        case ClientMessage:
            if (ev.xclient.message_type == g_x11.tray_opcode &&
                ev.xclient.data.l[1] == X11_SYSTEM_TRAY_REQUEST_DOCK)
                x11_tray_dock((Window)ev.xclient.data.l[2]);
            else if ((Atom)ev.xclient.data.l[0] == g_x11.wm_delete_window)
                g_should_close = 1;
            break;
        case ConfigureRequest:
            if (x11_tray_contains(ev.xconfigurerequest.window)) {
                /* A tray icon is always a 24px surface; never let it turn the
                 * tray viewport into the geometry of its old top-level window. */
                XMoveResizeWindow(g_x11.display, ev.xconfigurerequest.window,
                                  0, 0, X11_TRAY_ICON_SIZE, X11_TRAY_ICON_SIZE);
                x11_tray_layout();
            }
            break;
        case DestroyNotify:
            x11_tray_remove(ev.xany.window);
            break;
        case MotionNotify:
            g_x11.mouse_x = ev.xmotion.x;
            g_x11.mouse_y = ev.xmotion.y;
            luna_mouse_move(g_x11.mouse_x, g_x11.mouse_y);
            break;
        case ButtonPress:
        case ButtonRelease: {
            int action = ev.type == ButtonPress ? LUNA_PRESS : LUNA_RELEASE;
            /* X button events carry their own coordinates.  Reusing the
             * previous MotionNotify position can dispatch a click to a stale,
             * visibly shifted location when those events are coalesced. */
            g_x11.mouse_x = ev.xbutton.x;
            g_x11.mouse_y = ev.xbutton.y;
            g_x11.mods = x11_mod_bits(ev.xbutton.state);
            /* Buttons 4/5 are scroll wheel */
            if (ev.xbutton.button == Button4) {
                if (action == LUNA_PRESS) luna_scroll(0,  1.0);
            } else if (ev.xbutton.button == Button5) {
                if (action == LUNA_PRESS) luna_scroll(0, -1.0);
            } else {
                int btn;
                switch (ev.xbutton.button) {
                    case Button1: btn = 0; break;
                    case Button3: btn = 1; break;
                    case Button2: btn = 2; break;
                    default:      btn = 3; break;
                }
                luna_mouse_button(btn, action, g_x11.mods, g_x11.mouse_x, g_x11.mouse_y);
            }
            break;
        }
        case KeyPress:
        case KeyRelease: {
            int pressed = ev.type == KeyPress;
            g_x11.mods = x11_mod_bits(ev.xkey.state);
            /* XKB_KEY_* and X11 KeySym share the same namespace for standard keys */
            KeySym ksym = XLookupKeysym(&ev.xkey, 0);
            int lkey = xkb_keysym_to_luna_key((xkb_keysym_t)ksym);
            if (lkey != -1)
                dispatch_key(lkey, (int)ev.xkey.keycode,
                             pressed ? LUNA_PRESS : LUNA_RELEASE, g_x11.mods);
            if (pressed) {
                char buf[8] = "";
                XLookupString(&ev.xkey, buf, sizeof(buf)-1, NULL, NULL);
                unsigned char c = (unsigned char)buf[0];
                if (c >= 32 && c != 127) luna_char(c);
            }
            break;
        }
        default: break;
        }
    }
}

static void x11_backend_poll_events(void) {
    /* XPending is non-blocking.  Sleep on the X connection when its queue is
     * empty so a clean desktop becomes event-driven. */
    if (!XPending(g_x11.display) && g_single_poll_timeout_ms > 0) {
        struct pollfd pfd = {
            .fd = ConnectionNumber(g_x11.display),
            .events = POLLIN,
        };
        int pr;
        do {
            pr = poll(&pfd, 1, g_single_poll_timeout_ms);
        } while (pr < 0 && errno == EINTR);
    }
    x11_process_events();
}
static void x11_backend_set_cursor(int cursor_type) { (void)cursor_type; }

static void x11_backend_terminate(void) {
    if (g_x11.surf != EGL_NO_SURFACE) eglDestroySurface(g_x11.dpy, g_x11.surf);
    if (g_x11.ctx  != EGL_NO_CONTEXT) eglDestroyContext(g_x11.dpy, g_x11.ctx);
    if (g_x11.dpy  != EGL_NO_DISPLAY) eglTerminate(g_x11.dpy);
    if (g_x11.xkb_state)  xkb_state_unref(g_x11.xkb_state);
    if (g_x11.xkb_keymap) xkb_keymap_unref(g_x11.xkb_keymap);
    if (g_x11.xkb_ctx)    xkb_context_unref(g_x11.xkb_ctx);
    if (g_x11.tray_host) XDestroyWindow(g_x11.display, g_x11.tray_host);
    if (g_x11.window)  XDestroyWindow(g_x11.display, g_x11.window);
    if (g_x11.display) XCloseDisplay(g_x11.display);
}

static const LunaBackend g_x11_backend = {
    .start        = x11_backend_start,
    .get_fb_size  = x11_backend_get_fb_size,
    .swap_buffers = x11_backend_swap_buffers,
    .poll_events  = x11_backend_poll_events,
    .set_cursor   = x11_backend_set_cursor,
    .terminate    = x11_backend_terminate,
};

#endif /* LUNA_BACKEND_X11 */

/*
  CLI + main
*/
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--desktop") || !strcmp(argv[i], "-d"))
            { g_desktop_mode = 1; g_fullscreen = 1; }
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f"))
            g_fullscreen = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%fx%f", &luna_window_width, &luna_window_height);
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc)
            g_layout_path = argv[++i];
        else if (!strcmp(argv[i], "--css") && i + 1 < argc)
            g_css_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            luna_request_screenshot(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "luna-shell " LUNA_SHELL_VERSION " — Luna Desktop shell\n"
                "usage: luna-shell [--desktop] [--fullscreen] [--size WxH]\n"
                "                  [--layout PATH] [--css PATH] [--screenshot PATH]\n"
                "  backend: auto-selected — Wayland/EGL if WAYLAND_DISPLAY is set,\n"
                "           X11/EGL if DISPLAY is set (requires -DLUNA_BACKEND_X11 build),\n"
                "           otherwise KMS/DRM bare console\n"
                "  env: LUNA_DESKTOP_LAYOUT / LUNA_DESKTOP_CSS — external layout override\n"
                "       LUNA_APP_<NAME>=<cmd> — override dock/launchpad app commands\n"
                "       LUNA_SEAT — seat name for the KMS/libinput backend (default seat0)\n"
                "       LUNA_CURSOR_THEME — one-shot cursor theme override (GUI/settings normally win)\n"
                "       LUNA_CURSOR_PATH — directory of theme folders, or a theme dir itself\n"
                "       LUNA_IM_WAYLAND=0 — keep GTK_IM_MODULE=gim (default: wayland → whiz-im-wayland)\n"
                "       LUNA_INPUT_METHOD / LUNA_CLIPBOARD — helper cmds (or 'none' to skip)\n"
                "       LUNA_NO_HELPERS=1 — do not auto-start IME / clipboard manager\n"
                "       XKB_DEFAULT_LAYOUT / VARIANT / OPTIONS — keyboard layout (e.g. jp,us)\n"
                "       --size is ignored by the KMS backend, which always uses the\n"
                "       display's native mode\n"
                "  keys: Super/F4 — Launchpad, Esc — close overlay, Alt+Tab — switch apps\n"
                "        Super+Arrows — tile/max/min, Super+D — show desktop, Alt+F4 — close window\n"
                "        Cmd+, — Settings, F12 — screenshot\n"
                "  settings: ~/.config/luna-shell/settings.conf\n");
            exit(0);
        }
    }
    if (!g_layout_path) g_layout_path = getenv("LUNA_DESKTOP_LAYOUT");
    if (!g_css_path)    g_css_path    = getenv("LUNA_DESKTOP_CSS");
}

static int env_is_true(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0;
}

/* Belt-and-suspenders env setup that existed in the original GLFW-based
 * code and is still worth keeping even though we now call
 * eglGetPlatformDisplayEXT() explicitly for both backends: some Mesa
 * subsystems outside of that one call still consult EGL_PLATFORM, and
 * LUNA_EGL_SOFTWARE is documented in luna-session's own error message,
 * so honoring it here too (in case a future wrapper stops setting the
 * Mesa vars itself) costs nothing. */
static void setup_wayland_egl_env(void) {
    if (getenv("WAYLAND_DISPLAY") && !getenv("EGL_PLATFORM"))
        setenv("EGL_PLATFORM", "wayland", 0);
#ifdef LUNA_BACKEND_X11
    else if (getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY") && !getenv("EGL_PLATFORM"))
        setenv("EGL_PLATFORM", "x11", 0);
#endif
    if (env_is_true("LUNA_EGL_SOFTWARE")) {
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
        setenv("GALLIUM_DRIVER", "llvmpipe", 0);
    }
}

int main(int argc, char** argv) {
    luna_window_width  = 1440.0f;
    luna_window_height = 900.0f;
    parse_args(argc, argv);
    settings_load();
    apply_xkb_session_env();
    shell_paths_init();
    setup_wayland_egl_env();

    /* Backend selection:
     *   WAYLAND_DISPLAY set             → Wayland/EGL (any compositor, incl. Wayback)
     *   DISPLAY set, no WAYLAND_DISPLAY → X11/EGL  (Xorg, only if built with -DLUNA_BACKEND_X11)
     *   Neither                         → KMS/DRM bare console */
    if (getenv("WAYLAND_DISPLAY")) {
        /* Force toolkit/IME env even when launched outside luna-session
         * (console often inherits GTK_IM_MODULE=gim from Berry X). */
        apply_toolkit_session_env();
        ensure_wayland_helpers();
        g_backend = &g_wl_backend;
#ifdef LUNA_BACKEND_X11
    } else if (getenv("DISPLAY")) {
        g_backend = &g_x11_backend;
#endif
    } else {
        g_backend = &g_kms_backend;
    }

    if (!g_backend->start()) {
        fprintf(stderr, "[luna-shell] failed to start the display backend\n");
        return 1;
    }

    LunaPlatform plat = {
        .get_time       = plat_time,
        .get_proc       = plat_proc,
        .set_cursor     = plat_cursor,
        .request_close  = plat_close,
        .iconify        = plat_iconify,
        .maximize_toggle= plat_maximize,
    };
    luna_set_platform(&plat);

    LunaInitConfig cfg = { luna_window_width, luna_window_height, plat_proc };
    if (!luna_init(&cfg)) {
        fprintf(stderr, "[luna-shell] luna_init failed — check GL context version\n");
        g_backend->terminate();
        return 1;
    }

    int css_loaded = g_css_path && luna_load_css_file(g_css_path);
    int loaded = 0;
    if (g_layout_path) {
        luna_set_html_base_dir(g_layout_path);
        loaded = luna_load_html_file(g_layout_path);
    }
    if (!loaded) {
        luna_set_html_base_dir("ui");
        if (!css_loaded) luna_parse_css(default_css);
        luna_parse_html(default_html);
    }
    luna_inject_body_background();
    register_handlers();
    luna_wire_onclick_handlers();
    bind_indices();
    /* Resolve root_id → root_idx for each Wayland surface now that the layout
     * is loaded.  The struct is zero-initialised, so root_idx defaults to 0
     * (element 0) rather than -1, which causes every overlay to appear "shown"
     * on the first frame and triggers wl_region_destroy(NULL) → SIGSEGV. */
    if (g_backend == &g_wl_backend) {
        for (int i = 0; i < LUNA_SURF_COUNT; i++)
            g_surfs[i].root_idx = g_surfs[i].root_id
                ? luna_get_element_by_id(g_surfs[i].root_id)
                : -1;
    }
    luna_set_mouse_release_hook(on_mouse_release_hook);
    fill_about_info();
    apply_wallpaper(g_settings.wallpaper);
    cursor_theme_reload(g_settings.cursor_theme);
    apply_keyboard_layout(g_settings.kb_layout);
    apply_wm_settings();
    read_cpu_percent(); /* prime /proc/stat delta */
    g_cached_bat = read_battery_percent();
    snprintf(g_cached_net, sizeof(g_cached_net), "%s", read_net_status());

    g_now = plat_time();
    toast_show("Welcome to Luna", "Your desktop is ready.", 8.0);
    session_restore_schedule();

    double last    = plat_time();
    int    prev_ww = 0, prev_wh = 0;

    while (!g_should_close) {
        g_now = plat_time();
        double dt = g_now - last;
        last = g_now;

        int fbw, fbh;
        g_backend->get_fb_size(&fbw, &fbh);
        /* A layer-shell output has no usable size until its first configure.
         * Never turn that transient 0x0 into the CSS viewport: doing so moves
         * every centered/absolute dialog to the left/top before the real size
         * arrives on the next event dispatch. */
        if (fbw > 0 && fbh > 0 && (fbw != prev_ww || fbh != prev_wh)) {
            luna_resize((float)fbw, (float)fbh);
            prev_ww = fbw; prev_wh = fbh;
        }
        reap_children();
        update_clock();
        update_stats();
        update_launchpad_filter();
        poll_shell_state();
        session_restore_tick();
        slider_tick("bright_thumb", "bright_fill", "bright_track");
        slider_tick("vol_thumb", "vol_fill", "vol_track");
        dock_magnify_tick();
        cursor_theme_tick_and_refresh();
        if (g_toast_deadline > 0.0 && g_now > g_toast_deadline) {
            set_hidden(g_toast_idx, 1);
            g_toast_deadline = 0.0;
        }

        luna_update(g_now, dt);
        if (g_backend == &g_wl_backend) {
            wl_surfs_update();
            /* Wallpaper aurora/stars: only damage the bg layer when the
             * desktop is empty. Continuous full-screen commits under open
             * windows force a whole-desktop re-composite every tick → flicker
             * and starve client frame callbacks. Idle desktop: ~4 fps. */
            int desktop_busy = 0;
            for (int wi = 0; wi < g_win_count; wi++) {
                if (!g_wins[wi].minimized) { desktop_busy = 1; break; }
            }
            if (!desktop_busy && g_now - g_last_bg_paint >= 0.25)
                shell_request_repaint(LUNA_SURF_BG);
            /* Per-surface settling — dock mag must not redraw the menubar. */
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                if (!surf_is_live(&g_surfs[i])) continue;
                if (luna_visuals_settling_under(g_surfs[i].root_idx))
                    shell_request_repaint(i);
            }
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                wl_surf_render(&g_surfs[i], i);
            }
            /* Interactive easing is capped at roughly 60 fps.  When idle,
             * sleep until input or the next real shell/background deadline. */
            int settling = luna_visuals_settling();
            g_wl_poll_timeout_ms = settling
                ? shell_wait_timeout_ms(17, g_now + 1.0 / 60.0)
                : shell_wait_timeout_ms(250,
                      desktop_busy ? 0.0 : g_last_bg_paint + 0.25);
        } else {
            /* Compute this once: it scans the element array.  The result is
             * also the complete redraw decision for the single KMS/X11
             * framebuffer. */
            int settling = luna_visuals_settling();
            /* Pointer/hover easing runs at vblank cadence, but CSS wallpaper
             * animation is intentionally 15 fps when nothing is changing.
             * The KMS hardware cursor remains full-rate without repainting the
             * primary plane, saving GPU work and memory bandwidth. */
            double idle_frame_deadline = g_last_bg_paint + 1.0 / 15.0;
            if (g_now >= idle_frame_deadline)
                g_frame_dirty = 1;
            if (g_frame_dirty || settling) {
                glViewport(0, 0, fbw, fbh);
                glClearColor(0.04f, 0.05f, 0.12f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                luna_render(fbw, fbh);
                /* The default framebuffer contents are undefined after EGL swap
                 * on the X11 backend.  Capture while the completed Luna frame is
                 * still current, otherwise --screenshot can return a black image
                 * even though the window itself was rendered correctly. */
                luna_flush_pending_screenshot();
                g_backend->swap_buffers();
                g_frame_dirty = 0;
                g_last_bg_paint = g_now;
                g_single_poll_timeout_ms = settling ? 0
                    : shell_wait_timeout_ms(67, g_last_bg_paint + 1.0 / 15.0);
            } else {
                g_single_poll_timeout_ms = shell_wait_timeout_ms(67, idle_frame_deadline);
            }
        }
        /* Wayland surfaces perform their own swap in wl_surf_render(); keep
         * the existing post-render capture point for that backend. */
        if (g_backend == &g_wl_backend)
            luna_flush_pending_screenshot();
        g_backend->poll_events();
    }
    session_save();
    luna_shutdown();
    g_backend->terminate();
    return 0;
}
