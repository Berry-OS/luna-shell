#define _GNU_SOURCE
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

/* Berry rpm %{optflags} is `-Os -ffast-math`.  Fast-math makes luna-ui's
 * opacity/cull compares (eff_op <= 0.004, dw <= 0) collapse so luna_render
 * draws nothing — glClear only, black wallpaper, invisible dock/menubar,
 * but hit-testing and set_cursor still work.  The Makefile appends
 * -fno-fast-math after optflags; this guard catches a raw gcc/rpmbuild. */
#ifdef __FAST_MATH__
#error "luna-shell requires IEEE floats; rebuild without -ffast-math (pass -fno-fast-math after %{optflags})"
#endif

/* Skins grew with Network/Bluetooth/Sound panels; toast sits at the end of
 * layout.html and was the first thing dropped when this cap was 800.
 * Launchpad XDG slots add ~150 nodes on top of the chrome DOM. */
#define LUNA_UI_MAX_ELEMENTS 2000
#define LUNA_UI_IMPLEMENTATION
/* Custom KMS / Wayland / X11 host — do not pull in luna_linux.h (GLFW). */
#define LUNA_UI_NO_PLATFORM
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <spawn.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <pwd.h>

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
#include "luna-ui/luna-ui.h"
#define LUNA_WINDOW_IMPLEMENTATION
#include "luna-ui/luna-window.h"

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
#define LUNA_KEY_PRINT_SCREEN 283
#define LUNA_MOD_CONTROL     0x0002
#define LUNA_MOD_ALT         0x0004
#define LUNA_MOD_SUPER       0x0008

#define LUNA_SHELL_VERSION "1.2"
#define MAX_WINDOWS    128
#define MAX_WIN_SLOTS  12
#define MAX_TRAY_SLOTS 8
#define MAX_SWITCHER_SLOTS 12
#define MAX_WIFI_NETWORKS 8
#define MAX_ETHERNET_LINKS 4
#define MAX_BT_DEVICES 8

#define LUNA_WIFI_MAX_NETWORKS MAX_WIFI_NETWORKS
#define LUNA_WIFI_IMPLEMENTATION
#include "luna-wifi.h"

#define LUNA_ETHERNET_MAX_LINKS MAX_ETHERNET_LINKS
#define LUNA_ETHERNET_IMPLEMENTATION
#include "luna-ethernet.h"

#define LUNA_BLUETOOTH_MAX_DEVICES MAX_BT_DEVICES
#define LUNA_BLUETOOTH_IMPLEMENTATION
#include "luna-bluetooth.h"

#define LUNA_MONITOR_IMPLEMENTATION
#include "luna-monitor.h"

#define LUNA_WEATHER_IMPLEMENTATION
#include "luna-weather.h"

static int g_should_close = 0;
static int g_desktop_mode = 0;
static int g_fullscreen = 0;
static volatile sig_atomic_t g_sigchld_pending = 0;

/* ── XDG base directories ────────────────────────────────────────────────
 * Keep every persistent/runtime path in one place.  XDG paths must be
 * absolute; relative environment values are invalid and therefore ignored.
 * Directories created for user-owned configuration/state/cache use 0700 as
 * required by the Base Directory specification. */
typedef struct {
    char home[PATH_MAX];
    char config_home[PATH_MAX];
    char data_home[PATH_MAX];
    char state_home[PATH_MAX];
    char cache_home[PATH_MAX];
    char runtime_dir[PATH_MAX];
} LunaXdgPaths;

static LunaXdgPaths g_xdg;

static int path_is_absolute(const char* path) {
    return path && path[0] == '/';
}

static int str_has_prefix(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (*text == 0 || *text != *prefix) return 0;
        text++;
        prefix++;
    }
    return 1;
}

static int path_join2(char* out, size_t n, const char* base, const char* leaf) {
    if (!out || n == 0 || !base || !*base) return 0;
    if (!leaf || !*leaf) {
        int written = snprintf(out, n, "%s", base);
        return written >= 0 && (size_t)written < n;
    }
    int written = snprintf(out, n, "%s%s%s", base,
                           base[strlen(base) - 1] == '/' ? "" : "/", leaf);
    return written >= 0 && (size_t)written < n;
}

static int mkdir_p_mode(const char* path, mode_t mode) {
    char tmp[PATH_MAX];
    size_t len;

    if (!path_is_absolute(path)) return 0;
    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return 0;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;

    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return 0;
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return 0;
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

static void xdg_home_path(char* out, size_t n, const char* env_name,
                          const char* fallback_suffix) {
    const char* value = getenv(env_name);
    if (value && *value && path_is_absolute(value)) {
        snprintf(out, n, "%s", value);
        return;
    }
    if (value && *value)
        fprintf(stderr, "[luna-shell/xdg] ignoring relative %s=%s\n",
                env_name, value);
    if (!path_join2(out, n, g_xdg.home, fallback_suffix)) out[0] = 0;
}

static int xdg_runtime_valid(const char* path) {
    struct stat st;
    if (!path_is_absolute(path) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return st.st_uid == getuid() && (st.st_mode & 0777) == 0700;
}

static void xdg_runtime_init(void) {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime && path_is_absolute(runtime)) {
        struct stat st;
        if (stat(runtime, &st) != 0 && errno == ENOENT)
            (void)mkdir_p_mode(runtime, 0700);
        if (xdg_runtime_valid(runtime)) {
            snprintf(g_xdg.runtime_dir, sizeof(g_xdg.runtime_dir), "%s", runtime);
            return;
        }
        fprintf(stderr,
                "[luna-shell/xdg] invalid XDG_RUNTIME_DIR=%s; using a private replacement\n",
                runtime);
    } else if (runtime && *runtime) {
        fprintf(stderr,
                "[luna-shell/xdg] ignoring relative XDG_RUNTIME_DIR=%s\n", runtime);
    } else {
        fprintf(stderr,
                "[luna-shell/xdg] XDG_RUNTIME_DIR is unset; using a private replacement\n");
    }

    char templ[PATH_MAX];
    int written = snprintf(templ, sizeof(templ), "/tmp/luna-runtime-%lu-XXXXXX",
                           (unsigned long)getuid());
    if (written > 0 && (size_t)written < sizeof(templ) && mkdtemp(templ)) {
        chmod(templ, 0700);
        snprintf(g_xdg.runtime_dir, sizeof(g_xdg.runtime_dir), "%s", templ);
        setenv("XDG_RUNTIME_DIR", g_xdg.runtime_dir, 1);
        return;
    }

    /* Last-resort replacement.  Refuse a pre-existing object owned by another
     * user; otherwise lock permissions down before exporting it to children. */
    snprintf(g_xdg.runtime_dir, sizeof(g_xdg.runtime_dir),
             "/tmp/luna-runtime-%lu", (unsigned long)getuid());
    struct stat st;
    if (lstat(g_xdg.runtime_dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
            fprintf(stderr, "[luna-shell/xdg] no safe runtime directory available\n");
            g_xdg.runtime_dir[0] = 0;
            return;
        }
        chmod(g_xdg.runtime_dir, 0700);
    } else if (mkdir(g_xdg.runtime_dir, 0700) != 0) {
        fprintf(stderr, "[luna-shell/xdg] cannot create runtime directory: %s\n",
                strerror(errno));
        g_xdg.runtime_dir[0] = 0;
        return;
    }
    setenv("XDG_RUNTIME_DIR", g_xdg.runtime_dir, 1);
}

static void xdg_paths_init(void) {
    memset(&g_xdg, 0, sizeof(g_xdg));
    const char* home = getenv("HOME");
    if (!home || !*home || !path_is_absolute(home)) {
        struct passwd* pw = getpwuid(getuid());
        home = (pw && pw->pw_dir && path_is_absolute(pw->pw_dir)) ? pw->pw_dir : "/";
    }
    snprintf(g_xdg.home, sizeof(g_xdg.home), "%s", home);
    xdg_home_path(g_xdg.config_home, sizeof(g_xdg.config_home),
                  "XDG_CONFIG_HOME", ".config");
    xdg_home_path(g_xdg.data_home, sizeof(g_xdg.data_home),
                  "XDG_DATA_HOME", ".local/share");
    xdg_home_path(g_xdg.state_home, sizeof(g_xdg.state_home),
                  "XDG_STATE_HOME", ".local/state");
    xdg_home_path(g_xdg.cache_home, sizeof(g_xdg.cache_home),
                  "XDG_CACHE_HOME", ".cache");
    xdg_runtime_init();
}

static int xdg_app_dir(char* out, size_t n, const char* base,
                       const char* app, int create) {
    if (!path_join2(out, n, base, app)) return 0;
    return !create || mkdir_p_mode(out, 0700);
}

static int xdg_find_data_file(char* out, size_t n, const char* relative) {
    if (path_join2(out, n, g_xdg.data_home, relative) && access(out, R_OK) == 0)
        return 1;

    const char* dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char list[PATH_MAX * 2];
    if (snprintf(list, sizeof(list), "%s", dirs) >= (int)sizeof(list)) return 0;
    char* save = NULL;
    for (char* base = strtok_r(list, ":", &save); base;
         base = strtok_r(NULL, ":", &save)) {
        if (!path_is_absolute(base)) continue;
        if (path_join2(out, n, base, relative) && access(out, R_OK) == 0)
            return 1;
    }
    out[0] = 0;
    return 0;
}

/* Load the shell's bundled fonts through the LunaPlatform role API.
 *
 * Relying only on luna-ui.h's process-working-directory scan made the selected
 * face depend on where luna-shell was launched from.  It also meant an update
 * that added new candidate directories could silently select a different
 * regular font.  Resolve environment overrides first, then the known bundled,
 * executable-relative and XDG data locations.  Returning NULL deliberately
 * leaves CJK/mono system discovery to luna-ui.h when no explicit file exists. */
static unsigned char* shell_read_binary_file(const char* path, size_t* out_size) {
    FILE* f;
    long end;
    unsigned char* data;
    size_t got;
    if (out_size) *out_size = 0;
    if (!path || !*path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (unsigned char*)malloc((size_t)end);
    if (!data) {
        fclose(f);
        return NULL;
    }
    got = fread(data, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) {
        free(data);
        return NULL;
    }
    if (out_size) *out_size = got;
    return data;
}

static unsigned char* shell_try_font_path(const char* path, size_t* out_size) {
    unsigned char* data = shell_read_binary_file(path, out_size);
    if (data || !path || path_is_absolute(path)) return data;

#if defined(__linux__)
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = 0;
        char* slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char resolved[PATH_MAX];
            if (path_join2(resolved, sizeof(resolved), exe, path)) {
                data = shell_read_binary_file(resolved, out_size);
                if (data) return data;
            }
        }
    }
#endif
    return NULL;
}

static unsigned char* shell_try_font_candidates(const char* const* paths,
                                                  size_t count,
                                                  size_t* out_size) {
    for (size_t i = 0; i < count; i++) {
        unsigned char* data = shell_try_font_path(paths[i], out_size);
        if (data) {
            fprintf(stderr, "[luna-shell/font] loaded %s\n", paths[i]);
            return data;
        }
    }
    return NULL;
}

static unsigned char* shell_try_xdg_font_candidates(const char* const* paths,
                                                      size_t count,
                                                      size_t* out_size) {
    char resolved[PATH_MAX];
    for (size_t i = 0; i < count; i++) {
        if (xdg_find_data_file(resolved, sizeof(resolved), paths[i])) {
            unsigned char* data = shell_read_binary_file(resolved, out_size);
            if (data) {
                fprintf(stderr, "[luna-shell/font] loaded %s\n", resolved);
                return data;
            }
        }
    }
    return NULL;
}

static unsigned char* plat_load_font(int role, size_t* out_size) {
    const char* env_name = NULL;
    const char* env_path;
    if (out_size) *out_size = 0;

    switch (role) {
        case LUNA_FONT_REGULAR: env_name = "LUNA_FONT_REGULAR"; break;
        case LUNA_FONT_BOLD:    env_name = "LUNA_FONT_BOLD";    break;
        case LUNA_FONT_CJK:     env_name = "LUNA_FONT_CJK";     break;
        case LUNA_FONT_MONO:    env_name = "LUNA_FONT_MONO";    break;
        case LUNA_FONT_SYMBOLS: env_name = "LUNA_FONT_ICONS";   break;
        case LUNA_FONT_BRANDS:  env_name = "LUNA_FONT_BRANDS";  break;
        default: return NULL;
    }
    env_path = getenv(env_name);
    if (env_path && *env_path) {
        unsigned char* data = shell_try_font_path(env_path, out_size);
        if (data) return data;
        fprintf(stderr, "[luna-shell/font] cannot read %s=%s\n",
                env_name, env_path);
    }

    if (role == LUNA_FONT_REGULAR) {
        static const char* const direct[] = {
            "skins/fonts/web/Inter-Regular.ttf",
            "apps/luna-shell/skins/fonts/web/Inter-Regular.ttf",
            "ui/fonts/Inter-Regular.ttf",
            "fonts/Inter-Regular.ttf",
            "../skins/fonts/web/Inter-Regular.ttf"
        };
        static const char* const xdg[] = {
            "luna-shell/skins/fonts/web/Inter-Regular.ttf",
            "luna-shell/fonts/Inter-Regular.ttf",
            "vespera/fonts/Inter-Regular.ttf"
        };
        unsigned char* data = shell_try_font_candidates(
            direct, sizeof(direct) / sizeof(direct[0]), out_size);
        return data ? data : shell_try_xdg_font_candidates(
            xdg, sizeof(xdg) / sizeof(xdg[0]), out_size);
    }
    if (role == LUNA_FONT_BOLD) {
        static const char* const direct[] = {
            "skins/fonts/web/Inter-Bold.ttf",
            "apps/luna-shell/skins/fonts/web/Inter-Bold.ttf",
            "ui/fonts/Inter-Bold.ttf",
            "fonts/Inter-Bold.ttf",
            "../skins/fonts/web/Inter-Bold.ttf"
        };
        static const char* const xdg[] = {
            "luna-shell/skins/fonts/web/Inter-Bold.ttf",
            "luna-shell/fonts/Inter-Bold.ttf",
            "vespera/fonts/Inter-Bold.ttf"
        };
        unsigned char* data = shell_try_font_candidates(
            direct, sizeof(direct) / sizeof(direct[0]), out_size);
        return data ? data : shell_try_xdg_font_candidates(
            xdg, sizeof(xdg) / sizeof(xdg[0]), out_size);
    }
    if (role == LUNA_FONT_SYMBOLS || role == LUNA_FONT_BRANDS) {
        static const char* const solid_direct[] = {
            "skins/fonts/LunaSymbols-Solid.otf",
            "apps/luna-shell/skins/fonts/LunaSymbols-Solid.otf",
            "ui/fonts/LunaSymbols-Solid.otf",
            "fonts/LunaSymbols-Solid.otf",
            "../skins/fonts/LunaSymbols-Solid.otf",
            "/usr/share/fonts/luna/LunaSymbols-Solid.otf",
            "/usr/share/fonts/LunaSymbols-Solid.otf",
            "/usr/local/share/fonts/luna/LunaSymbols-Solid.otf"
        };
        static const char* const brands_direct[] = {
            "skins/fonts/LunaSymbols-Brands.otf",
            "apps/luna-shell/skins/fonts/LunaSymbols-Brands.otf",
            "ui/fonts/LunaSymbols-Brands.otf",
            "fonts/LunaSymbols-Brands.otf",
            "../skins/fonts/LunaSymbols-Brands.otf",
            "/usr/share/fonts/luna/LunaSymbols-Brands.otf",
            "/usr/share/fonts/LunaSymbols-Brands.otf",
            "/usr/local/share/fonts/luna/LunaSymbols-Brands.otf"
        };
        static const char* const solid_xdg[] = {
            "luna-shell/skins/fonts/LunaSymbols-Solid.otf",
            "luna-shell/fonts/LunaSymbols-Solid.otf",
            "vespera/fonts/LunaSymbols-Solid.otf"
        };
        static const char* const brands_xdg[] = {
            "luna-shell/skins/fonts/LunaSymbols-Brands.otf",
            "luna-shell/fonts/LunaSymbols-Brands.otf",
            "vespera/fonts/LunaSymbols-Brands.otf"
        };
        const char* const* direct = role == LUNA_FONT_SYMBOLS
            ? solid_direct : brands_direct;
        size_t direct_count = role == LUNA_FONT_SYMBOLS
            ? sizeof(solid_direct) / sizeof(solid_direct[0])
            : sizeof(brands_direct) / sizeof(brands_direct[0]);
        const char* const* xdg = role == LUNA_FONT_SYMBOLS
            ? solid_xdg : brands_xdg;
        size_t xdg_count = role == LUNA_FONT_SYMBOLS
            ? sizeof(solid_xdg) / sizeof(solid_xdg[0])
            : sizeof(brands_xdg) / sizeof(brands_xdg[0]);
        unsigned char* data =
            shell_try_font_candidates(direct, direct_count, out_size);
        return data ? data :
            shell_try_xdg_font_candidates(xdg, xdg_count, out_size);
    }

    return NULL;
}

static void on_sigchld(int signo) {
    (void)signo;
    g_sigchld_pending = 1;
}

static void install_sigchld_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    /* Do not use SA_RESTART: SIGCHLD should wake an idle poll immediately. */
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

static const char* g_layout_path = NULL;
static const char* g_css_path = NULL;
static const char* g_skin_override = NULL;
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
    int         dock_visible;
    pid_t       pid;
} LunaApp;

static LunaApp g_apps[] = {
    { .key = "files",    .name = "Files",     .env = "LUNA_APP_FILES",    .default_cmd = "pcmanfm",              .dock_visible = 1 },
    { .key = "terminal", .name = "Terminal",  .env = "LUNA_APP_TERMINAL", .default_cmd = "sakura",               .dock_visible = 1 },
    { .key = "browser",  .name = "Browser",   .env = "LUNA_APP_BROWSER",  .default_cmd = "firefox",              .dock_visible = 1 },
    { .key = "editor",   .name = "Editor",    .env = "LUNA_APP_EDITOR",   .default_cmd = "gedit",                .dock_visible = 1 },
    { .key = "music",    .name = "Music",     .env = "LUNA_APP_MUSIC",    .default_cmd = "aplay+ui",             .dock_visible = 1 },
    { .key = "settings", .name = "Settings",  .env = "LUNA_APP_SETTINGS", .default_cmd = "gnome-control-center", .dock_visible = 1 },
    { .key = "demo",     .name = "GTK Demo",  .env = "LUNA_APP_DEMO",     .default_cmd = "gtk4-demo"            },
    { .key = "hello",    .name = "Hello GTK", .env = "LUNA_APP_HELLO",    .default_cmd = "hello-gtk"            },
};
#define APP_COUNT ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

/* ── Persistent settings ── */

typedef struct {
    char wallpaper[32]; /* "night" | "ocean" | "forest" | "sunset" */
    char skin[64];      /* "default" | discovered skin directory name */
    char hostname[64];
    char cursor_theme[64]; /* "aero" | "miku" | custom theme dir name */
    char kb_layout[64];    /* XKB layout, e.g. "jp,us" | "us" | "de,us" */
    char ui_language[32];  /* locale inherited by newly launched applications */
    int  numlock_on;       /* desired NumLock state now and at sign-in */
    int  window_gap;       /* tiled window inset in pixels: 0 | 8 | 16 */
    int  edge_snap;
    int  top_edge_maximize;
    int  titlebar_double_click;
    int  classic_titlebar;
    int  super_shortcuts;
    int  dock_magnification;
    int  wallpaper_animation; /* animate aurora/stars only while enabled */
    int  session_restore;
    int  wifi_enabled;     /* desired Wi-Fi radio state, persisted across boots */
    int  bluetooth_enabled; /* desired Bluetooth radio state, persisted across boots */
    char weather_city[64]; /* Open-Meteo city name, default Tokyo */
    char audio_backend[16]; /* "auto" | "wpctl" | "pactl" | "alsa" */
    char alsa_card[32];     /* "" / "default" / card index ("0") */
    char alsa_control[64];  /* simple mixer control, usually "Master" */
    char brightness_backend[16]; /* "auto" | "sysfs" | "brightnessctl" | "xrandr" */
    /* Toolkit scale for apps launched from Luna (see apply_toolkit_session_env).
     * LUNA_GDK_* / LUNA_QT_* / LUNA_XCURSOR_SIZE still override when set. */
    char gdk_scale[8];       /* integer buffer scale: "1" | "2" */
    char gdk_dpi_scale[8];   /* fractional text scale: "1" | "1.25" | … */
    char qt_scale_factor[8]; /* Qt scale, same values as gdk_dpi_scale */
    char xcursor_size[8];    /* "24" | "32" | "48" */
} LunaSettings;

static LunaSettings g_settings;
static LunaCurTheme g_cur_theme;
static int g_cursor_reload_pending = 0;
/* luna_cur_theme_tick() advances a frame without changing the cursor role.
 * Backends normally skip an identical role, so carry the frame change
 * explicitly through set_cursor(). */
static int g_cursor_frame_changed = 0;
/* Wayland cursor animation is useful only while one of Luna's layer surfaces
 * owns pointer focus.  Enter/leave callbacks maintain this flag; KMS starts
 * with its hardware cursor present. */
static int g_cursor_present = 1;

static void cursor_theme_reload(const char* name);
static void cursor_theme_tick_and_refresh(void);

static void settings_path(char* buf, size_t n) {
    char dir[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-shell", 0) ||
        !path_join2(buf, n, dir, "settings.conf"))
        buf[0] = 0;
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
    snprintf(g_settings.skin, sizeof(g_settings.skin), "default");
    snprintf(g_settings.hostname, sizeof(g_settings.hostname), "Luna Desktop");
    snprintf(g_settings.cursor_theme, sizeof(g_settings.cursor_theme), "aero");
    g_settings.window_gap = 8;
    g_settings.edge_snap = 1;
    g_settings.top_edge_maximize = 0;
    g_settings.titlebar_double_click = 1;
    g_settings.classic_titlebar = 0;
    g_settings.super_shortcuts = 1;
    g_settings.dock_magnification = 1;
    g_settings.wallpaper_animation = 1;
    g_settings.session_restore = 1;
    g_settings.wifi_enabled = 1;
    g_settings.bluetooth_enabled = 1;
    g_settings.numlock_on = 1;
    snprintf(g_settings.weather_city, sizeof(g_settings.weather_city), "Tokyo");
    snprintf(g_settings.audio_backend, sizeof(g_settings.audio_backend), "auto");
    snprintf(g_settings.alsa_card, sizeof(g_settings.alsa_card), "default");
    snprintf(g_settings.alsa_control, sizeof(g_settings.alsa_control), "Master");
    snprintf(g_settings.brightness_backend, sizeof(g_settings.brightness_backend), "auto");
    snprintf(g_settings.gdk_scale, sizeof(g_settings.gdk_scale), "1");
    snprintf(g_settings.gdk_dpi_scale, sizeof(g_settings.gdk_dpi_scale), "1");
    snprintf(g_settings.qt_scale_factor, sizeof(g_settings.qt_scale_factor), "1");
    snprintf(g_settings.xcursor_size, sizeof(g_settings.xcursor_size), "24");
    {
        const char* lang = getenv("LC_ALL");
        if (!lang || !*lang) lang = getenv("LANG");
        if (lang && !strncasecmp(lang, "ja", 2))
            snprintf(g_settings.ui_language, sizeof(g_settings.ui_language), "ja_JP.UTF-8");
        else if (lang && (!strncasecmp(lang, "C", 1) || !strncasecmp(lang, "POSIX", 5)))
            snprintf(g_settings.ui_language, sizeof(g_settings.ui_language), "C.UTF-8");
        else
            snprintf(g_settings.ui_language, sizeof(g_settings.ui_language), "en_US.UTF-8");
    }
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
        } else if (!strcmp(section, "dock")) {
            for (int i = 0; i < APP_COUNT; i++)
                if (!strcmp(g_apps[i].key, key))
                    g_apps[i].dock_visible = atoi(val) != 0;
        } else if (!strcmp(section, "shell")) {
            if (!strcmp(key, "wallpaper"))
                snprintf(g_settings.wallpaper, sizeof(g_settings.wallpaper), "%s", val);
            else if (!strcmp(key, "skin"))
                snprintf(g_settings.skin, sizeof(g_settings.skin), "%s", val);
            else if (!strcmp(key, "hostname"))
                snprintf(g_settings.hostname, sizeof(g_settings.hostname), "%s", val);
            else if (!strcmp(key, "cursor_theme"))
                snprintf(g_settings.cursor_theme, sizeof(g_settings.cursor_theme), "%s", val);
            else if (!strcmp(key, "kb_layout") && val[0])
                /* Older settings files could contain an empty value.  Keep the
                 * environment/locale-derived default instead of silently
                 * reverting the entire Wayland session to a US keymap. */
                snprintf(g_settings.kb_layout, sizeof(g_settings.kb_layout), "%s", val);
            else if (!strcmp(key, "ui_language") && val[0])
                snprintf(g_settings.ui_language, sizeof(g_settings.ui_language), "%s", val);
            else if (!strcmp(key, "numlock_on"))
                g_settings.numlock_on = atoi(val) != 0;
            else if (!strcmp(key, "window_gap"))
                g_settings.window_gap = atoi(val);
            else if (!strcmp(key, "edge_snap"))
                g_settings.edge_snap = atoi(val) != 0;
            else if (!strcmp(key, "top_edge_maximize"))
                g_settings.top_edge_maximize = atoi(val) != 0;
            else if (!strcmp(key, "titlebar_double_click"))
                g_settings.titlebar_double_click = atoi(val) != 0;
            else if (!strcmp(key, "classic_titlebar"))
                g_settings.classic_titlebar = atoi(val) != 0;
            else if (!strcmp(key, "super_shortcuts"))
                g_settings.super_shortcuts = atoi(val) != 0;
            else if (!strcmp(key, "dock_magnification"))
                g_settings.dock_magnification = atoi(val) != 0;
            else if (!strcmp(key, "wallpaper_animation"))
                g_settings.wallpaper_animation = atoi(val) != 0;
            else if (!strcmp(key, "session_restore"))
                g_settings.session_restore = atoi(val) != 0;
            else if (!strcmp(key, "wifi_enabled"))
                g_settings.wifi_enabled = atoi(val) != 0;
            else if (!strcmp(key, "bluetooth_enabled"))
                g_settings.bluetooth_enabled = atoi(val) != 0;
            else if (!strcmp(key, "weather_city"))
                snprintf(g_settings.weather_city, sizeof(g_settings.weather_city), "%s", val);
            else if (!strcmp(key, "audio_backend") && val[0])
                snprintf(g_settings.audio_backend, sizeof(g_settings.audio_backend), "%s", val);
            else if (!strcmp(key, "alsa_card") && val[0])
                snprintf(g_settings.alsa_card, sizeof(g_settings.alsa_card), "%s", val);
            else if (!strcmp(key, "alsa_control") && val[0])
                snprintf(g_settings.alsa_control, sizeof(g_settings.alsa_control), "%s", val);
            else if (!strcmp(key, "brightness_backend") && val[0])
                snprintf(g_settings.brightness_backend, sizeof(g_settings.brightness_backend), "%s", val);
            else if (!strcmp(key, "gdk_scale") && val[0])
                snprintf(g_settings.gdk_scale, sizeof(g_settings.gdk_scale), "%s", val);
            else if (!strcmp(key, "gdk_dpi_scale") && val[0])
                snprintf(g_settings.gdk_dpi_scale, sizeof(g_settings.gdk_dpi_scale), "%s", val);
            else if (!strcmp(key, "qt_scale_factor") && val[0])
                snprintf(g_settings.qt_scale_factor, sizeof(g_settings.qt_scale_factor), "%s", val);
            else if (!strcmp(key, "xcursor_size") && val[0])
                snprintf(g_settings.xcursor_size, sizeof(g_settings.xcursor_size), "%s", val);
        }
    }
    fclose(f);
}

static void ensure_config_dir(void) {
    char dir[PATH_MAX];
    (void)mkdir_p_mode(g_xdg.config_home, 0700);
    (void)xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-shell", 1);
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
    fprintf(f, "\n[dock]\n");
    for (int i = 0; i < APP_COUNT; i++)
        fprintf(f, "%s=%d\n", g_apps[i].key, g_apps[i].dock_visible);
    fprintf(f, "\n[shell]\n");
    fprintf(f, "wallpaper=%s\n", g_settings.wallpaper);
    fprintf(f, "skin=%s\n", g_settings.skin);
    fprintf(f, "hostname=%s\n", g_settings.hostname);
    fprintf(f, "cursor_theme=%s\n", g_settings.cursor_theme);
    fprintf(f, "kb_layout=%s\n", g_settings.kb_layout);
    fprintf(f, "ui_language=%s\n", g_settings.ui_language);
    fprintf(f, "numlock_on=%d\n", g_settings.numlock_on);
    fprintf(f, "window_gap=%d\n", g_settings.window_gap);
    fprintf(f, "edge_snap=%d\n", g_settings.edge_snap);
    fprintf(f, "top_edge_maximize=%d\n", g_settings.top_edge_maximize);
    fprintf(f, "titlebar_double_click=%d\n", g_settings.titlebar_double_click);
    fprintf(f, "classic_titlebar=%d\n", g_settings.classic_titlebar);
    fprintf(f, "super_shortcuts=%d\n", g_settings.super_shortcuts);
    fprintf(f, "dock_magnification=%d\n", g_settings.dock_magnification);
    fprintf(f, "wallpaper_animation=%d\n", g_settings.wallpaper_animation);
    fprintf(f, "session_restore=%d\n", g_settings.session_restore);
    fprintf(f, "wifi_enabled=%d\n", g_settings.wifi_enabled);
    fprintf(f, "bluetooth_enabled=%d\n", g_settings.bluetooth_enabled);
    fprintf(f, "weather_city=%s\n", g_settings.weather_city);
    fprintf(f, "audio_backend=%s\n", g_settings.audio_backend);
    fprintf(f, "alsa_card=%s\n", g_settings.alsa_card);
    fprintf(f, "alsa_control=%s\n", g_settings.alsa_control);
    fprintf(f, "brightness_backend=%s\n", g_settings.brightness_backend);
    fprintf(f, "gdk_scale=%s\n", g_settings.gdk_scale);
    fprintf(f, "gdk_dpi_scale=%s\n", g_settings.gdk_dpi_scale);
    fprintf(f, "qt_scale_factor=%s\n", g_settings.qt_scale_factor);
    fprintf(f, "xcursor_size=%s\n", g_settings.xcursor_size);
    fclose(f);
}

/* ── Desktop skins ────────────────────────────────────────────────────────
 * A skin is a directory containing skin.conf + style.css and, optionally,
 * layout.html.  layout.html is the authoring surface: open it in a browser
 * (with <link> to ../default/style.css and style.css) to preview the look,
 * then run the same file as the shell layout — no export step.
 *
 * CSS is replaceable without rebuilding the shared DOM; a custom layout is
 * selected at startup (and when switching skins, if the HTML file is present)
 * and must retain the shell's documented element IDs so the native handlers
 * can bind to it.  The default skin is also the shared visual base; directories
 * whose names start with '_' remain reserved for non-skin assets.
 *
 * Chrome placement (menubar edge, dock visibility) is also declared in
 * skin.conf and applied live to the Wayland layer-shell surfaces — CSS alone
 * cannot move the menubar, because the layer is anchored independently of
 * the document box. */
#define MAX_SKINS 12
#define SKIN_EDGE_TOP    0
#define SKIN_EDGE_BOTTOM 1
#define SKIN_DOCK_FLOAT  0
#define SKIN_DOCK_HIDDEN 1
typedef struct {
    char id[64];
    char name[80];
    char description[112];
    char dir[PATH_MAX];
    char css[PATH_MAX];
    char layout[PATH_MAX];
    int  menubar_edge;    /* SKIN_EDGE_TOP | SKIN_EDGE_BOTTOM */
    int  menubar_height;  /* exclusive-zone / layer height in px */
    int  dock_mode;       /* SKIN_DOCK_FLOAT | SKIN_DOCK_HIDDEN */
    /* Toolkit themes shipped under gtk/<name>/ and qt/ inside the skin dir. */
    char gtk_theme[80];   /* empty = leave session GTK theme alone */
    char qt_style[64];    /* e.g. Fusion; empty = unset override */
    char qt_qss[PATH_MAX];/* absolute path to style.qss, or empty */
    /* Compositor SSD chrome. titlebar_style < 0 → use Settings toggle. */
    int  titlebar_style;  /* -1 unset | 0 modern | 1 classic dots | 2 flat retro */
    /* Single source of truth for titlebar height: pushed to the compositor as
     * `wm_config titlebar_height` (SSD bar) *and* written into
     * window-theme.conf (Luna UI client chrome), so both agree. 0 = unset. */
    int  titlebar_height;
    unsigned int titlebar_active;   /* 0 = compositor default palette */
    unsigned int titlebar_inactive;
    unsigned int titlebar_frame;
    int  prefer_ssd;      /* recommend server decorations when client unset */
    int  window_theme;    /* -1 auto | 0 light | 1 dark for Luna UI clients */
    int  controls_on_left;/* client-side control placement */
} LunaSkin;

static LunaSkin g_skins[MAX_SKINS];
static int g_skin_count = 0;
/* Live chrome geometry used by popup positioning (menus open upward when
 * the menubar is a bottom taskbar). */
static int g_chrome_menubar_edge = SKIN_EDGE_TOP;
static int g_chrome_menubar_height = 32;
static int g_chrome_dock_hidden = 0;
static void skin_apply_chrome(int skin_idx);
static void skin_apply_toolkit(int skin_idx);
static int  skin_apply_wm_decoration(int skin_idx);
static void skin_export_window_theme(int skin_idx);
static void skin_chrome_defaults(LunaSkin* skin);
static unsigned int skin_parse_color(const char* val);

static int path_is_regular(const char* path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int skin_join(char* out, size_t n, const char* dir, const char* leaf) {
    if (!out || n == 0) return 0;
    if (!leaf || !*leaf) { out[0] = 0; return 1; }
    if (path_is_absolute(leaf)) {
        int written = snprintf(out, n, "%s", leaf);
        return written >= 0 && (size_t)written < n;
    }
    return path_join2(out, n, dir, leaf);
}

static int skin_find(const char* id) {
    if (!id || !*id) return 0;
    /* Compatibility with settings written before the built-in skin moved to
     * skins/default. */
    if (!strcmp(id, "luna")) return 0;
    for (int i = 0; i < g_skin_count; i++)
        if (!strcmp(g_skins[i].id, id)) return i;
    return 0;
}

static void skin_chrome_defaults(LunaSkin* skin) {
    if (!skin) return;
    skin->menubar_edge = SKIN_EDGE_TOP;
    skin->menubar_height = 32;
    skin->dock_mode = SKIN_DOCK_FLOAT;
    skin->gtk_theme[0] = 0;
    skin->qt_style[0] = 0;
    skin->qt_qss[0] = 0;
    skin->titlebar_style = -1;
    skin->titlebar_height = 0;
    skin->titlebar_active = 0;
    skin->titlebar_inactive = 0;
    skin->titlebar_frame = 0;
    /* CSD is the safe fallback for clients which do not bind
     * xdg-decoration (GTK commonly draws a HeaderBar in that case).  Retro
     * skins which want compositor chrome opt in through skin.conf. */
    skin->prefer_ssd = 0;
    skin->window_theme = -1;
    skin->controls_on_left = 1;
}

/* Accept #RRGGBB, #AARRGGBB, 0xRRGGBB, or 0xAARRGGBB. Missing alpha → opaque. */
static unsigned int skin_parse_color(const char* val) {
    if (!val || !*val) return 0;
    while (*val == ' ' || *val == '\t') val++;
    if (*val == '#') val++;
    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) val += 2;
    char* end = NULL;
    unsigned long v = strtoul(val, &end, 16);
    if (!end || end == val) return 0;
    size_t digits = (size_t)(end - val);
    if (digits <= 6) return 0xff000000u | (unsigned int)(v & 0xfffffful);
    return (unsigned int)(v & 0xfffffffful);
}

static void skin_apply_chrome_preset(LunaSkin* skin, const char* preset) {
    if (!skin || !preset) return;
    if (!strcmp(preset, "taskbar")) {
        /* Windows 9x/XP-style bottom bar; apps live in the menubar window list. */
        skin->menubar_edge = SKIN_EDGE_BOTTOM;
        skin->menubar_height = 34;
        skin->dock_mode = SKIN_DOCK_HIDDEN;
    } else if (!strcmp(preset, "deskbar")) {
        /* BeOS Deskbar: compact top strip, no floating dock. */
        skin->menubar_edge = SKIN_EDGE_TOP;
        skin->menubar_height = 28;
        skin->dock_mode = SKIN_DOCK_HIDDEN;
    } else if (!strcmp(preset, "menubar")) {
        /* Classic Mac OS: top menubar only. */
        skin->menubar_edge = SKIN_EDGE_TOP;
        skin->menubar_height = 22;
        skin->dock_mode = SKIN_DOCK_HIDDEN;
    } else if (!strcmp(preset, "luna") || !strcmp(preset, "default")) {
        skin_chrome_defaults(skin);
    }
}

static void skin_add_dir(const char* root, const char* id) {
    if (!root || !*root || !id || !*id || g_skin_count >= MAX_SKINS) return;
    for (int i = 0; i < g_skin_count; i++)
        if (!strcmp(g_skins[i].id, id)) return;

    char dir[PATH_MAX], conf[PATH_MAX];
    if (!skin_join(dir, sizeof(dir), root, id) ||
        !skin_join(conf, sizeof(conf), dir, "skin.conf")) return;
    if (!path_is_regular(conf)) return;

    LunaSkin skin;
    memset(&skin, 0, sizeof(skin));
    skin_chrome_defaults(&skin);
    snprintf(skin.id, sizeof(skin.id), "%s", id);
    snprintf(skin.name, sizeof(skin.name), "%s", id);
    snprintf(skin.description, sizeof(skin.description), "External desktop skin");
    snprintf(skin.dir, sizeof(skin.dir), "%s", dir);
    char css_leaf[128] = "style.css";
    /* Prefer layout.html when present so browser-previewable HTML is the
     * same file the shell loads — skin.conf may override the leaf name. */
    char layout_leaf[128] = "layout.html";
    char qt_qss_leaf[128] = "";

    FILE* f = fopen(conf, "r");
    if (!f) return;
    char line[384];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;
        if (!strcmp(key, "name")) snprintf(skin.name, sizeof(skin.name), "%s", val);
        else if (!strcmp(key, "description")) snprintf(skin.description, sizeof(skin.description), "%s", val);
        else if (!strcmp(key, "css")) snprintf(css_leaf, sizeof(css_leaf), "%s", val);
        else if (!strcmp(key, "layout")) snprintf(layout_leaf, sizeof(layout_leaf), "%s", val);
        else if (!strcmp(key, "chrome")) skin_apply_chrome_preset(&skin, val);
        else if (!strcmp(key, "menubar_edge")) {
            skin.menubar_edge = !strcmp(val, "bottom") ? SKIN_EDGE_BOTTOM : SKIN_EDGE_TOP;
        } else if (!strcmp(key, "menubar_height")) {
            int h = atoi(val);
            if (h >= 18 && h <= 96) skin.menubar_height = h;
        } else if (!strcmp(key, "dock_mode")) {
            if (!strcmp(val, "hidden")) skin.dock_mode = SKIN_DOCK_HIDDEN;
            else skin.dock_mode = SKIN_DOCK_FLOAT;
        } else if (!strcmp(key, "gtk_theme")) {
            snprintf(skin.gtk_theme, sizeof(skin.gtk_theme), "%s", val);
        } else if (!strcmp(key, "qt_style")) {
            snprintf(skin.qt_style, sizeof(skin.qt_style), "%s", val);
        } else if (!strcmp(key, "qt_qss")) {
            snprintf(qt_qss_leaf, sizeof(qt_qss_leaf), "%s", val);
        } else if (!strcmp(key, "titlebar_style")) {
            int s = atoi(val);
            if (s >= 0 && s <= 2) skin.titlebar_style = s;
        } else if (!strcmp(key, "titlebar_height")) {
            int h = atoi(val);
            if (h >= LUNA_TITLEBAR_H_MIN && h <= LUNA_TITLEBAR_H_MAX)
                skin.titlebar_height = h;
        } else if (!strcmp(key, "titlebar_active")) {
            skin.titlebar_active = skin_parse_color(val);
        } else if (!strcmp(key, "titlebar_inactive")) {
            skin.titlebar_inactive = skin_parse_color(val);
        } else if (!strcmp(key, "titlebar_frame")) {
            skin.titlebar_frame = skin_parse_color(val);
        } else if (!strcmp(key, "prefer_ssd")) {
            skin.prefer_ssd = atoi(val) != 0;
        } else if (!strcmp(key, "window_theme")) {
            if (!strcasecmp(val, "dark")) skin.window_theme = 1;
            else if (!strcasecmp(val, "light")) skin.window_theme = 0;
            else skin.window_theme = -1;
        } else if (!strcmp(key, "titlebar_controls")) {
            skin.controls_on_left = strcasecmp(val, "right") != 0;
        }
    }
    fclose(f);

    if (!skin_join(skin.css, sizeof(skin.css), dir, css_leaf) ||
        !skin_join(skin.layout, sizeof(skin.layout), dir, layout_leaf)) return;
    if (!path_is_regular(skin.css)) return;
    if (skin.layout[0] && !path_is_regular(skin.layout)) skin.layout[0] = 0;
    if (qt_qss_leaf[0]) {
        if (!skin_join(skin.qt_qss, sizeof(skin.qt_qss), dir, qt_qss_leaf) ||
            !path_is_regular(skin.qt_qss))
            skin.qt_qss[0] = 0;
    } else {
        char def_qss[PATH_MAX];
        if (skin_join(def_qss, sizeof(def_qss), dir, "qt/style.qss") &&
            path_is_regular(def_qss))
            snprintf(skin.qt_qss, sizeof(skin.qt_qss), "%s", def_qss);
    }
    /* Auto-detect gtk_theme from gtk/<Name>/index.theme when conf omits it. */
    if (!skin.gtk_theme[0]) {
        char gtk_root[PATH_MAX];
        if (skin_join(gtk_root, sizeof(gtk_root), dir, "gtk")) {
            DIR* gd = opendir(gtk_root);
            if (gd) {
                struct dirent* ge;
                while ((ge = readdir(gd)) != NULL) {
                    if (ge->d_name[0] == '.') continue;
                    char idx[PATH_MAX];
                    if (skin_join(idx, sizeof(idx), gtk_root, ge->d_name)) {
                        size_t n = strlen(idx);
                        if (n + 12 < sizeof(idx)) {
                            snprintf(idx + n, sizeof(idx) - n, "/index.theme");
                            if (path_is_regular(idx)) {
                                size_t theme_len = strlen(ge->d_name);
                                if (theme_len >= sizeof(skin.gtk_theme)) continue;
                                memcpy(skin.gtk_theme, ge->d_name, theme_len + 1);
                                break;
                            }
                        }
                    }
                }
                closedir(gd);
            }
        }
    }
    g_skins[g_skin_count++] = skin;
}

static void skin_scan_root(const char* root) {
    if (!root || !*root || g_skin_count >= MAX_SKINS) return;
    DIR* d = opendir(root);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && g_skin_count < MAX_SKINS) {
        if (ent->d_name[0] == '.' || ent->d_name[0] == '_') continue;
        skin_add_dir(root, ent->d_name);
    }
    closedir(d);
}

static void skin_scan_xdg_data_dirs(void) {
    char root[PATH_MAX];
    if (path_join2(root, sizeof(root), g_xdg.data_home, "luna-desktop/skins"))
        skin_scan_root(root);

    const char* dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char list[PATH_MAX * 2];
    if (snprintf(list, sizeof(list), "%s", dirs) >= (int)sizeof(list)) return;
    char* save = NULL;
    for (char* base = strtok_r(list, ":", &save); base;
         base = strtok_r(NULL, ":", &save)) {
        if (!path_is_absolute(base)) {
            if (*base)
                fprintf(stderr, "[luna-shell/xdg] ignoring relative XDG_DATA_DIRS entry: %s\n",
                        base);
            continue;
        }
        if (path_join2(root, sizeof(root), base, "luna-desktop/skins"))
            skin_scan_root(root);
    }
}

static void skin_discover(void) {
    memset(g_skins, 0, sizeof(g_skins));
    g_skin_count = 1;
    skin_chrome_defaults(&g_skins[0]);
    snprintf(g_skins[0].id, sizeof(g_skins[0].id), "default");
    snprintf(g_skins[0].name, sizeof(g_skins[0].name), "Luna Moonlight");
    snprintf(g_skins[0].description, sizeof(g_skins[0].description), "The built-in Luna desktop");

    const char* extra = getenv("LUNA_SKIN_PATH");
    if (extra && *extra) skin_scan_root(extra);
    skin_scan_xdg_data_dirs();
    skin_scan_root("skins");
    skin_scan_root("apps/luna-shell/skins");
    {
        char exe[PATH_MAX], exe_root[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len > 0) {
            exe[len] = 0;
            char* slash = strrchr(exe, '/');
            if (slash) {
                *slash = 0;
                if (skin_join(exe_root, sizeof(exe_root), exe,
                              "../share/luna-desktop/skins"))
                    skin_scan_root(exe_root);
            }
        }
    }
}

static int skin_apply_styles(const char* id) {
    int selected = skin_find(id);
    luna_reset_css();
    int base_ok = g_css_path && luna_load_css_file(g_css_path);
    if (!base_ok) luna_parse_css(default_css);
    if (selected > 0 && !luna_load_css_file(g_skins[selected].css)) {
        luna_reset_css();
        luna_parse_css(default_css);
        return 0;
    }
    luna_mark_layout_dirty();
    return 1;
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
static int g_settings_panel_lang   = -1;
static int g_settings_panel_kb     = -1;
static int g_settings_panel_sound  = -1;
static int g_settings_panel_wm     = -1;
static int g_stab_apps_idx         = -1;
static int g_stab_disp_idx         = -1;
static int g_stab_lang_idx         = -1;
static int g_stab_kb_idx           = -1;
static int g_stab_sound_idx        = -1;
static int g_stab_wm_idx           = -1;
static int g_win_menu_idx  = -1;
static int g_clip_menu_idx = -1;
static int g_mb_clip_idx   = -1;
static uint64_t g_win_menu_target = 0;
/* Cached menubar hit-test indices (avoid repeated ID lookups in mouse hook) */
static int g_mb_logo_idx  = -1;
static int g_mb_cc_idx    = -1;
static int g_mb_wifi_idx  = -1;
static int g_wifi_menu_idx = -1;
static int g_wifi_selected = -1;
static int g_bt_menu_idx = -1;
static int g_mb_weather_idx = -1;
static int g_weather_menu_idx = -1;
static int g_calendar_menu_idx = -1;
static int g_mb_clock_idx = -1;
static int g_net_detail_idx = -1;
static int g_net_detail_box_idx = -1;
static int g_net_detail_kind = -1;  /* 0=wifi 1=ethernet */
static int g_net_detail_index = -1;
static int g_cal_year = 0;
static int g_cal_month = 0; /* 0-11 */
static int g_cal_selected_day = 0;

/* ── Per-frame child element cache (populated once in bind_indices) ──
 * Eliminates O(n) element scans from update_window_list_ui,
 * update_tray_ui, update_switcher_ui, win_menu_set_maximize_label,
 * and the asynchronous status application path. */
static int g_win_label_idx[MAX_WIN_SLOTS];          /* span.win_label child of win_N */
static int g_tray_glyph_idx[MAX_TRAY_SLOTS];        /* span.tray_glyph child of tray_N */
static int g_mb_app_idx             = -1;           /* mb_app element */
static int g_mb_bat_icon_idx        = -1;           /* luna_icon child of mb_bat */
static int g_mb_wifi_icon_idx       = -1;           /* luna_icon child of mb_wifi text span */
static int g_sw_title_idx[MAX_SWITCHER_SLOTS];      /* span.sw_title child of sw_N */
static int g_sw_app_idx[MAX_SWITCHER_SLOTS];        /* span.sw_app child of sw_N */
static int g_wm_maximize_label_idx  = -1;           /* mi_label child of wm_maximize */
static int g_wm_fullscreen_label_idx = -1;          /* mi_label child of wm_fullscreen */

/* IDs touched by recurring status/switcher/filter updates.  Resolve once at
 * bind time instead of scanning the DOM hash/table on every worker sample. */
enum {
    UI_MB_CLOCK = 0,
    UI_MB_BAT,
    UI_CC_CPU_FILL,
    UI_CC_MEM_FILL,
    UI_WG_TIME,
    UI_WG_DATE,
    UI_ST_CPU_VAL,
    UI_ST_CPU_FILL,
    UI_ST_MEM_VAL,
    UI_ST_MEM_FILL,
    UI_ST_DISK_VAL,
    UI_ST_DISK_FILL,
    UI_WIN_MENU_TITLE,
    UI_SWITCHER,
    UI_CACHE_COUNT
};
static const char* const g_ui_cache_ids[UI_CACHE_COUNT] = {
    "mb_clock", "mb_bat", "cc_cpu_fill", "cc_mem_fill",
    "wg_time", "wg_date", "st_cpu_val", "st_cpu_fill",
    "st_mem_val", "st_mem_fill", "st_disk_val", "st_disk_fill",
    "win_menu_title", "switcher"
};
static int g_ui_idx[UI_CACHE_COUNT];
static int g_lp_app_idx[APP_COUNT];
static int g_sw_slot_idx[MAX_SWITCHER_SLOTS];

/* Extra Launchpad tiles filled from XDG .desktop application entries. */
#define MAX_LP_XDG 48
typedef struct {
    char id[NAME_MAX + 1];
    char name[256];
    char path[PATH_MAX];
} LunaLpXdgApp;
static LunaLpXdgApp g_lp_xdg[MAX_LP_XDG];
static int g_lp_xdg_count = 0;
static int g_lp_xdg_idx[MAX_LP_XDG];
static int g_lp_xdg_label_idx[MAX_LP_XDG];
static int g_lp_xdg_ready = 0;

typedef LunaWifiBackend WifiBackend;
typedef LunaWifiNetwork WifiNetwork;
#define WIFI_NONE    LUNA_WIFI_NONE
#define WIFI_CONNMAN LUNA_WIFI_CONNMAN
#define WIFI_NMCLI   LUNA_WIFI_NMCLI
static WifiBackend g_wifi_backend = WIFI_NONE;
static WifiNetwork g_wifi_networks[MAX_WIFI_NETWORKS];
static int g_wifi_count = 0;
static int g_wifi_powered = 0;
static int g_wifi_service_available = 0;
static int g_wifi_busy = 1;
static char g_wifi_error[96];
static unsigned long long g_wifi_generation = 0;
static double g_last_wifi_request = -1e9;

typedef LunaEthernetBackend EthernetBackend;
typedef LunaEthernetLink EthernetLink;
#define ETH_NONE    LUNA_ETHERNET_NONE
#define ETH_CONNMAN LUNA_ETHERNET_CONNMAN
#define ETH_NMCLI   LUNA_ETHERNET_NMCLI
static EthernetBackend g_eth_backend = ETH_NONE;
static EthernetLink g_eth_links[MAX_ETHERNET_LINKS];
static int g_eth_count = 0;
static int g_eth_powered = 1;
static int g_eth_service_available = 0;
static int g_eth_busy = 1;
static char g_eth_error[96];
static unsigned long long g_eth_generation = 0;
static double g_last_eth_request = -1e9;

typedef LunaBluetoothBackend BluetoothBackend;
typedef LunaBluetoothDevice BluetoothDevice;
#define BT_NONE    LUNA_BLUETOOTH_NONE
#define BT_BLUEZ   LUNA_BLUETOOTH_BLUEZ
#define BT_CONNMAN LUNA_BLUETOOTH_CONNMAN
static BluetoothBackend g_bt_backend = BT_NONE;
static BluetoothDevice g_bt_devices[MAX_BT_DEVICES];
static int g_bt_count = 0;
static int g_bt_powered = 0;
static int g_bt_service_available = 0;
static int g_bt_busy = 1;
static char g_bt_error[96];
static unsigned long long g_bt_generation = 0;
static double g_last_bt_request = -1e9;

/* ── Weather widget snapshot (network work lives in luna-weather.h) ── */
#define WEATHER_REFRESH_SEC 1800.0
typedef LunaWeatherData WeatherData;
static WeatherData g_weather;
static int g_weather_busy = 0;
static int g_weather_worker_ready = 0;

/* popen()/pclose() in worker modules must not race the shell's waitpid(-1)
 * reaper.  The UI thread only try-locks this mutex, so it never waits behind a
 * slow network request. */
static pthread_mutex_t g_child_reaper_mutex = PTHREAD_MUTEX_INITIALIZER;

static double g_last_shell_poll = -10.0;
static struct timespec g_shell_state_mtime = { .tv_sec = -1, .tv_nsec = 0 };
static off_t g_shell_state_size = -1;

/* Cached system readings — updated from background poller snapshots and read everywhere else
 * to avoid synchronous sysfs I/O on every window-list or tray update. */
static int  g_cached_bat = -1;
static char g_cached_net[16] = "Offline";

/* ── Compositor window list + system tray (via luna-shell/state.json) ── */

typedef struct {
    uint64_t id;
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
    uint64_t surface_id; /* collision-free shell window handle */
} LunaTrayEntry;

/* Keep the complete compositor snapshot, not only the handful of chips that
 * fit in the menu bar.  Window lookup, Dock grouping, session restore and the
 * Alt-Tab overlay must continue to work when more than MAX_WIN_SLOTS windows
 * are open.  A fixed array keeps this bounded and allocation-free. */
static LunaWinEntry  g_wins[MAX_WINDOWS];
static int           g_win_count = 0;
static int           g_visible_window_count = 0;
static LunaTrayEntry g_tray[MAX_TRAY_SLOTS];
static int           g_tray_count = 0;
static char          g_shell_state_path[512];
static char          g_shell_sock_path[512];
static int           g_win_slot_idx[MAX_WIN_SLOTS];  /* element index for win_0.. */
static int           g_tray_slot_idx[MAX_TRAY_SLOTS];  /* element index for tray_0.. */
static uint64_t      g_win_slot_id[MAX_WIN_SLOTS];    /* compositor window handle shown in slot */
static char          g_tray_slot_key[MAX_TRAY_SLOTS][64];

/* Dock magnification is CSS-only (#dock.dock_animated).  The shell never
 * interpolates icon transforms per frame — that work belongs in the stylesheet. */

static int g_about_sheet_max = 0;
static int g_settings_sheet_max = 0;

static void app_set_dot(LunaApp* app, int running);
static void set_hidden(int idx, int hidden);
static int elem_idx_of(LunaElement* e);
static void toast_show(const char* title, const char* msg, double secs);
static void on_control_center(LunaElement* e);

static void shell_paths_init(void) {
    char dir[PATH_MAX];
    if (!g_xdg.runtime_dir[0] ||
        !xdg_app_dir(dir, sizeof(dir), g_xdg.runtime_dir, "luna-shell", 1)) {
        fprintf(stderr, "[luna-shell/xdg] runtime directory unavailable\n");
        g_shell_state_path[0] = 0;
        g_shell_sock_path[0] = 0;
        return;
    }
    path_join2(g_shell_state_path, sizeof(g_shell_state_path), dir, "state.json");
    path_join2(g_shell_sock_path, sizeof(g_shell_sock_path), dir, "luna-shell.sock");
}

static int shell_send_cmd(const char* cmd) {
    if (!cmd || !*cmd || !g_shell_sock_path[0]) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t splen = strlen(g_shell_sock_path);
    if (splen >= sizeof(addr.sun_path)) {
        close(fd);
        return 0;
    }
    memcpy(addr.sun_path, g_shell_sock_path, splen + 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }
    /* Commands are small, but stream writes are still allowed to be partial.
     * MSG_NOSIGNAL also prevents a compositor restart from killing the shell
     * with SIGPIPE between connect() and send().  Return success so persistent
     * compositor settings can be retried when its IPC socket is not ready yet. */
    int ok = 0;
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
        ok = off == len;
    }
    close(fd);
    return ok;
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

    char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
    int n = snprintf(path, sizeof(path), "%s/luna-shell/tray-%s.sock",
                     g_xdg.runtime_dir, name);
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
        t->surface_id = (uint64_t)strtoull(t->id + 4, NULL, 10);
    } else if (!strncmp(t->id, "app:", 4)) {
        for (int i = 0; i < g_win_count; i++) {
            if (!strcmp(g_wins[i].app_id, t->id + 4)) {
                t->surface_id = g_wins[i].id;
                return;
            }
        }
    }
}

static uint64_t g_switcher_ids[MAX_SWITCHER_SLOTS];
static int g_switcher_count = 0;
static int g_switcher_index = 0;
static int g_switcher_visible = 0;
static uint64_t g_pending_menu_id = 0;
static int g_pending_menu_x = 0;
static int g_pending_menu_y = 0;
static uint64_t g_shell_state_hash = 0;
static int g_shell_state_changed = 0;
/* Short grace window after compositor window geometry changes.  It suppresses
 * unrelated clock/Wi-Fi/weather paints while the compositor is moving a client. */
static double g_window_motion_busy_until = 0.0;
static double g_now = 0.0;
/* The shell can start a fraction before the compositor IPC socket appears.
 * Keep WM preferences pending until every command has actually been written;
 * otherwise edge_snap looks enabled in Settings but remains disabled in the
 * compositor for the entire session. */
static int g_wm_settings_pending = 0;
static int g_wm_settings_retry_count = 0;
static double g_wm_settings_retry_at = 0.0;
/* Periodic status samples must not interrupt pointer/keyboard interaction with
 * a full wallpaper-layer repaint.  Input callbacks update this timestamp; the
 * latest status snapshot is coalesced and applied after a short idle grace. */
static double g_last_user_activity = -1e9;
#define STATUS_BG_IDLE_GRACE_SEC       0.75
#define INTERACTION_IDLE_GRACE_SEC     0.14

/* Cached once per main-loop iteration.  Deferred maintenance deadlines must
 * not remain expired while an interaction is active, otherwise poll() is fed
 * a zero timeout and the shell busy-spins exactly when the compositor needs
 * the CPU most. */
static int g_interaction_busy = 0;

/* Used by the inotify coalescer below, before the backend section provides the
 * implementation. */
static double plat_time(void);

static void shell_note_user_activity(void) {
    /* Event callbacks run after poll(), so the frame timestamp may be as much
     * as one idle timeout old.  Use the monotonic clock directly; otherwise a
     * freshly received drag event can look old and fail to defer a coincident
     * status/maintenance update. */
    g_last_user_activity = plat_time();
}

/* One eventfd wakes every backend poll when a background worker has published
 * a new snapshot.  Workers never touch Luna UI or OpenGL state. */
static int g_async_event_fd = -1;

static void shell_async_notify(void* user) {
    (void)user;
    if (g_async_event_fd < 0) return;
    uint64_t one = 1;
    ssize_t n;
    do { n = write(g_async_event_fd, &one, sizeof(one)); }
    while (n < 0 && errno == EINTR);
}

static int shell_async_init(void) {
    if (g_async_event_fd >= 0) return 1;
    g_async_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return g_async_event_fd >= 0;
}

static int shell_async_poll_fd(void) { return g_async_event_fd; }

static void shell_async_drain(void) {
    if (g_async_event_fd < 0) return;
    uint64_t value;
    for (;;) {
        ssize_t n = read(g_async_event_fd, &value, sizeof(value));
        if (n == (ssize_t)sizeof(value)) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

static void shell_async_close(void) {
    if (g_async_event_fd >= 0) close(g_async_event_fd);
    g_async_event_fd = -1;
}

static void shell_child_reaper_lock(void* user) {
    (void)user;
    pthread_mutex_lock(&g_child_reaper_mutex);
}

static void shell_child_reaper_unlock(void* user) {
    (void)user;
    pthread_mutex_unlock(&g_child_reaper_mutex);
}

/* Event-driven compositor state updates.  Watch the parent directory because
 * the compositor may publish state.json using atomic rename. */
static int    g_shell_watch_fd = -1;
static int    g_shell_watch_wd = -1;
static int    g_shell_state_pending = 1;
static double g_shell_watch_retry_at = 0.0;
static char   g_shell_state_dir[512];
static char   g_shell_state_name[512];

/* A compositor commonly rewrites state.json for every pointer sample while a
 * client is moved.  Read the first sample promptly, then debounce the rest
 * until the write burst goes quiet.  Merely limiting reads to 120 ms was not
 * enough: every inotify wake still ran the full Luna update pass. */
#define SHELL_STATE_NORMAL_INTERVAL_SEC 0.12
#define SHELL_STATE_SWITCHER_INTERVAL_SEC 0.05
#define SHELL_STATE_BURST_QUIET_SEC 0.075
#define SHELL_STATE_BURST_RESET_SEC 0.30
#define SHELL_EXTERNAL_ACTIVITY_GRACE_SEC 0.28
static double g_shell_state_last_event_at = -1e9;
static int    g_shell_state_burst_read = 0;

static void shell_state_watch_paths(void) {
    if (g_shell_state_dir[0]) return;
    snprintf(g_shell_state_dir, sizeof(g_shell_state_dir), "%s", g_shell_state_path);
    char* slash = strrchr(g_shell_state_dir, '/');
    if (!slash) {
        snprintf(g_shell_state_name, sizeof(g_shell_state_name), "%s", g_shell_state_dir);
        snprintf(g_shell_state_dir, sizeof(g_shell_state_dir), ".");
        return;
    }
    snprintf(g_shell_state_name, sizeof(g_shell_state_name), "%s", slash + 1);
    if (slash == g_shell_state_dir) slash[1] = 0;
    else *slash = 0;
}

static void shell_state_watch_ensure(void) {
    if (g_shell_watch_wd >= 0) return;
    if (g_now < g_shell_watch_retry_at) return;
    g_shell_watch_retry_at = g_now + 1.0;
    shell_state_watch_paths();
    if (g_shell_watch_fd < 0) {
        g_shell_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_shell_watch_fd < 0) return;
    }
    g_shell_watch_wd = inotify_add_watch(g_shell_watch_fd, g_shell_state_dir,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
        IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
}

static int shell_state_watch_poll_fd(void) {
    shell_state_watch_ensure();
    return g_shell_watch_wd >= 0 ? g_shell_watch_fd : -1;
}

static int shell_state_watch_drain(void) {
    if (g_shell_watch_fd < 0) return 0;
    int changed = 0;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(g_shell_watch_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close(g_shell_watch_fd);
            g_shell_watch_fd = -1; g_shell_watch_wd = -1;
            break;
        }
        if (n == 0) break;
        for (char* at = buf; at < buf + n; ) {
            struct inotify_event* ev = (struct inotify_event*)at;
            if (ev->mask & IN_IGNORED) g_shell_watch_wd = -1;
            if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
                g_shell_watch_wd = -1;
            if (ev->len > 0 && !strcmp(ev->name, g_shell_state_name)) changed = 1;
            at += sizeof(*ev) + ev->len;
        }
    }
    if (changed) {
        double event_now = plat_time();
        if (event_now - g_shell_state_last_event_at > SHELL_STATE_BURST_RESET_SEC)
            g_shell_state_burst_read = 0;
        g_shell_state_last_event_at = event_now;
        g_shell_state_pending = 1;

        /* State-file activity is also the earliest reliable signal that an
         * external client move/resize is in progress.  Mark it busy before
         * parsing the next snapshot so a coincident clock/status sample cannot
         * commit another shell surface in the middle of the compositor move. */
        if (g_window_motion_busy_until <
            event_now + SHELL_EXTERNAL_ACTIVITY_GRACE_SEC)
            g_window_motion_busy_until =
                event_now + SHELL_EXTERNAL_ACTIVITY_GRACE_SEC;
        if (g_last_user_activity < event_now)
            g_last_user_activity = event_now;
    }
    return changed;
}

/* Absolute deadline at which the pending state should wake the main loop.
 * The first event in a burst is read immediately (needed for focus/switcher
 * changes); subsequent geometry writes are folded into one final read after
 * the stream has been quiet briefly. */
static double shell_state_pending_deadline(double now) {
    if (!g_shell_state_pending) return 0.0;
    if (g_shell_watch_wd < 0)
        return g_last_shell_poll +
            (g_switcher_visible ? SHELL_STATE_SWITCHER_INTERVAL_SEC
                                : SHELL_STATE_NORMAL_INTERVAL_SEC);
    if (!g_shell_state_burst_read) return now;
    if (g_switcher_visible)
        return g_last_shell_poll + SHELL_STATE_SWITCHER_INTERVAL_SEC;
    if (g_shell_state_last_event_at > -1e8)
        return g_shell_state_last_event_at + SHELL_STATE_BURST_QUIET_SEC;
    return g_last_shell_poll + SHELL_STATE_NORMAL_INTERVAL_SEC;
}

/* Recompute a poll timeout after a state-only wake.  This lets the backend
 * drain high-rate inotify traffic without returning through the expensive UI
 * update/render loop for every compositor write. */
static int shell_poll_coalesced_timeout_ms(double outer_deadline) {
    double now = plat_time();
    double next = outer_deadline;
    double state_deadline = shell_state_pending_deadline(now);
    if (state_deadline > 0.0 && state_deadline < next) next = state_deadline;
    double remain = next - now;
    if (remain <= 0.0) return 0;
    int ms = (int)ceil(remain * 1000.0);
    return ms < 1 ? 1 : ms;
}

static void shell_state_watch_close(void) {
    if (g_shell_watch_fd >= 0) close(g_shell_watch_fd);
    g_shell_watch_fd = -1; g_shell_watch_wd = -1;
}

/* Wayland layer-surface dirty bits — skip eglSwapBuffers when unchanged.
 * Continuous full-screen swaps were flooding the compositor (client flicker)
 * and delaying Firefox's WaitFlushedEvent / frame callbacks. */
static uint32_t g_surf_dirty = 0xffffffffu;
static double   g_last_bg_paint = 0.0;
/* Whether the wallpaper has a live CSS @keyframes animation.  When it does not
 * — a plain colour or a photo — the timed background repaint is dropped
 * entirely instead of pushing an identical full-screen frame several times a
 * second.  Re-checked once a second because the answer scans the elements. */
static int      g_bg_animated = 1;
static int      g_wl_poll_timeout_ms = 0;
/* Single-surface backends used to repaint the whole desktop unconditionally.
 * Keep a separate bit for them: g_surf_dirty describes Wayland layers, while
 * this flag says that the KMS/X11 framebuffer needs another complete frame. */
static int      g_frame_dirty = 1;
/* Animation cadence.  Idle aurora/stars only (no open windows).  12 Hz is
 * enough for the empty-desktop wallpaper and avoids the old 30 Hz full-screen
 * commit storm that showed up as a regular hitch on the console session. */
#define LUNA_WL_BG_FRAME_SEC      (1.0 / 12.0)
#define LUNA_SINGLE_BG_FRAME_SEC  (1.0 / 12.0)
/* A delayed event or maintenance read must not turn one missed frame into a
 * large easing jump.  CSS keyframes use absolute time; this only bounds the
 * interactive interpolation path. */
#define LUNA_MAX_FRAME_DT         0.050
/* Event wait used by the single-surface backends.  KMS and X11 both need to
 * sleep when the framebuffer is clean; otherwise X11 busy-spins and a KMS
 * session without libinput does the same. */
static int      g_single_poll_timeout_ms = 0;
static void shell_request_repaint(int surf_idx); /* -1 = all; else LUNA_SURF_* */
static int shell_bg_passive_refresh_ready(void);

/* Hash only fields that can change shell chrome.  Window x/y are deliberately
 * excluded: the compositor rewrites them continuously while a client window is
 * dragged, but the menubar, dock and switcher do not visually depend on them.
 * Treating every geometry sample as a UI change made the shell repaint on top
 * of the compositor's move loop and produced a regular drag hitch. */
static uint64_t hash_shell_snapshot(const LunaWinEntry* wins, int wc,
                                    const LunaTrayEntry* tray, int tc,
                                    int sw_count, int sw_idx, const uint64_t* sw_ids) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < wc; i++) {
        h = h * 0x100000001b3ULL ^ wins[i].id;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].focused;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].minimized;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].maximized;
        h = h * 0x100000001b3ULL ^ (uint64_t)wins[i].fullscreen;
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

static void apply_shell_state_stream(FILE* f) {
    if (!f) return;
    LunaWinEntry wins[MAX_WINDOWS];
    LunaTrayEntry tray[MAX_TRAY_SLOTS];
    int wc = 0, tc = 0;
    int sw_count = 0, sw_idx = 0;
    uint64_t sw_ids[MAX_SWITCHER_SLOTS];
    int have_menu = 0;
    uint64_t menu_id = 0;
    int menu_x = 0, menu_y = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'W' && line[1] == '\t') {
            if (wc >= MAX_WINDOWS) continue;
            uint64_t id = 0;
            int focused = 0, minimized = 0, maximized = 0, fullscreen = 0;
            int wx = 0, wy = 0;
            char title[96]; title[0] = 0;
            char app_id[64]; app_id[0] = 0;

            char* p = line + 2;
            char* tab1 = strchr(p, '\t'); if (!tab1) continue;
            *tab1 = 0;
            id = (uint64_t)strtoull(p, NULL, 10);
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

            wins[wc].id = id;
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
                /* luna-wifi is now built into this shell.  Ignore a stale tray
                 * registration left by an older standalone service. */
                if (!strcmp(id, "service:luna-wifi")) continue;
                snprintf(tray[tc].id, sizeof(tray[tc].id), "%s", id);
                snprintf(tray[tc].label, sizeof(tray[tc].label), "%s", label);
                snprintf(tray[tc].icon, sizeof(tray[tc].icon), "%s", icon);
                snprintf(tray[tc].tooltip, sizeof(tray[tc].tooltip), "%s",
                         tooltip[0] ? tooltip : label);
                tray[tc].surface_id = 0;
                tc++;
            }
        } else if (line[0] == 'M' && line[1] == '\t') {
            if (sscanf(line + 2, "%" SCNu64 "\t%d\t%d", &menu_id, &menu_x, &menu_y) == 3) {
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
                sw_ids[sw_count++] = (uint64_t)strtoull(p, &p, 10);
                if (*p == ',') p++;
            }
        }
    }
    uint64_t h = hash_shell_snapshot(wins, wc, tray, tc, sw_count, sw_idx, sw_ids);
    int state_changed = (h != g_shell_state_hash);

    /* Preserve current geometry for session saving without making geometry-only
     * compositor updates repaint shell chrome.  Detect an active move before
     * replacing the cached snapshot. */
    int geometry_changed = 0;
    if (!state_changed && wc == g_win_count) {
        for (int i = 0; i < wc; i++) {
            if (wins[i].id != g_wins[i].id) break;
            if (wins[i].x != g_wins[i].x || wins[i].y != g_wins[i].y) {
                geometry_changed = 1;
                break;
            }
        }
    }
    if (geometry_changed) g_window_motion_busy_until = g_now + 0.20;
    g_win_count = wc;
    memcpy(g_wins, wins, (size_t)wc * sizeof(LunaWinEntry));
    g_visible_window_count = 0;
    for (int i = 0; i < wc; i++)
        g_visible_window_count += !wins[i].minimized;

    if (state_changed) {
        g_shell_state_hash = h;
        for (int i = 0; i < tc; i++) parse_tray_surface_id(&tray[i]);
        g_tray_count = tc;
        memcpy(g_tray, tray, (size_t)tc * sizeof(LunaTrayEntry));
        if (sw_count > 0) {
            g_switcher_count = sw_count;
            g_switcher_index = sw_idx;
            for (int i = 0; i < sw_count; i++)
                g_switcher_ids[i] = sw_ids[i];
            g_switcher_visible = 1;
        } else {
            g_switcher_visible = 0;
            g_switcher_count = 0;
        }
    }

    if (have_menu) {
        g_pending_menu_id = menu_id;
        g_pending_menu_x = menu_x;
        g_pending_menu_y = menu_y;
    }
    /* Stash whether UI widgets need a refresh (used by poll_shell_state). */
    g_shell_state_changed = state_changed;
}

static void apply_shell_state_buffer(char* text, size_t len) {
    if (!text || len == 0) return;
    FILE* f = fmemopen(text, len, "r");
    if (!f) return;
    apply_shell_state_stream(f);
    fclose(f);
}

/* Synchronous fallback used only during shutdown/session save.  Runtime state
 * reads are performed by the background poller and delivered as memory. */
static void load_shell_state(void) {
    struct stat st;
    if (stat(g_shell_state_path, &st) != 0) return;
    if (st.st_size == g_shell_state_size &&
        st.st_mtim.tv_sec == g_shell_state_mtime.tv_sec &&
        st.st_mtim.tv_nsec == g_shell_state_mtime.tv_nsec)
        return;
    FILE* f = fopen(g_shell_state_path, "r");
    if (!f) return;
    apply_shell_state_stream(f);
    fclose(f);
    g_shell_state_size = st.st_size;
    g_shell_state_mtime = st.st_mtim;
}

static void win_slot_style(int slot, LunaWinEntry* w) {
    if (slot < 0 || !w) return;
    const char* add = w->focused
        ? (w->minimized ? "active minimized" : "active")
        : (w->minimized ? "minimized" : NULL);
    luna_update_classes(slot, "active minimized", add);
}

static void tray_slot_style(int slot, LunaTrayEntry* t) {
    if (slot < 0) return;
    static const char* remove_icons =
        "icon_terminal icon_browser icon_files icon_editor icon_music "
        "icon_settings icon_gtk icon_wifi icon_bat icon_app active";
    char cls[48];
    snprintf(cls, sizeof(cls), "icon_%s", t->icon);
    /* strip _active suffix for CSS class */
    char* suf = strstr(cls, "_active");
    int active = suf != NULL;
    if (suf) *suf = 0;
    if (active)
        snprintf(cls + strlen(cls), sizeof(cls) - strlen(cls), " active");
    luna_update_classes(slot, remove_icons, cls);
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

static int str_contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle) return 0;
    if (!*needle) return 1;
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return 1;
    }
    return 0;
}

static int is_browser_cmd(const LunaApp* app, const char* cmd) {
    if (app && !strcmp(app->key, "browser")) return 1;
    return cmd && (str_contains_ci(cmd, "firefox") || str_contains_ci(cmd, "chrom"));
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
    if (str_contains_ci(app_id, key)) return 1;
    if (!strcmp(key, "files"))
        return str_contains_ci(app_id, "pcmanfm") || str_contains_ci(app_id, "thunar") ||
               str_contains_ci(app_id, "nautilus") || str_contains_ci(app_id, "nemo") ||
               str_contains_ci(app_id, "caja") || str_contains_ci(app_id, "org.gnome.Nautilus");
    if (!strcmp(key, "terminal"))
        return str_contains_ci(app_id, "sakura") || str_contains_ci(app_id, "terminal") ||
               str_contains_ci(app_id, "kitty") || str_contains_ci(app_id, "alacritty") ||
               str_contains_ci(app_id, "xterm") || str_contains_ci(app_id, "foot");
    if (!strcmp(key, "browser"))
        return str_contains_ci(app_id, "firefox") || str_contains_ci(app_id, "chrome") ||
               str_contains_ci(app_id, "chromium") || str_contains_ci(app_id, "brave") ||
               str_contains_ci(app_id, "epiphany") || str_contains_ci(app_id, "org.mozilla");
    if (!strcmp(key, "editor"))
        return str_contains_ci(app_id, "gedit") || str_contains_ci(app_id, "mousepad") ||
               str_contains_ci(app_id, "leafpad") || str_contains_ci(app_id, "TextEditor") ||
               str_contains_ci(app_id, "code") || str_contains_ci(app_id, "kate");
    if (!strcmp(key, "music"))
        return str_contains_ci(app_id, "music") || str_contains_ci(app_id, "rhythmbox") ||
               str_contains_ci(app_id, "spotify");
    if (!strcmp(key, "settings"))
        return str_contains_ci(app_id, "control-center") || str_contains_ci(app_id, "Settings") ||
               str_contains_ci(app_id, "gnome-control");
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
    int root = g_ui_idx[UI_SWITCHER];
    if (root < 0) return;
    if (!g_switcher_visible || g_switcher_count <= 0) {
        set_hidden(root, 1);
        return;
    }
    set_hidden(root, 0);
    for (int s = 0; s < MAX_SWITCHER_SLOTS; s++) {
        int idx = g_sw_slot_idx[s];
        if (idx < 0) continue;
        if (s >= g_switcher_count) {
            set_hidden(idx, 1);
            continue;
        }
        set_hidden(idx, 0);
        uint64_t sid = g_switcher_ids[s];
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
        luna_update_classes(idx, "active",
                            s == g_switcher_index ? "active" : NULL);
    }
    luna_mark_layout_dirty();
    shell_request_repaint(0); /* switcher lives in bg layer */
}

static void position_menu_at(int menu_idx, float x, float y) {
    if (menu_idx < 0) return;
    LunaElement* m = luna_element_at(menu_idx);
    float mw = m->w > 1.0f ? m->w : (m->css_width > 1.0f ? m->css_width : 200.0f);
    float mh = m->h > 1.0f ? m->h : (m->css_height > 1.0f ? m->css_height : 160.0f);
    float top_min = (g_chrome_menubar_edge == SKIN_EDGE_BOTTOM)
        ? 6.0f
        : (float)(g_chrome_menubar_height + 2);
    float bot_max = (g_chrome_menubar_edge == SKIN_EDGE_BOTTOM)
        ? (luna_window_height - (float)g_chrome_menubar_height - 4.0f)
        : (luna_window_height - 8.0f);
    if (x + mw > luna_window_width - 8.0f) x = luna_window_width - mw - 8.0f;
    if (y + mh > bot_max) y = bot_max - mh;
    if (x < 6.0f) x = 6.0f;
    if (y < top_min) y = top_min;
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
    int bat = g_cached_bat;
    int wifi_idx = g_tray_slot_idx[app_slots];
    int bat_idx  = g_tray_slot_idx[app_slots + 1];
    if (wifi_idx >= 0) {
        set_hidden(wifi_idx, 0);
        LunaTrayEntry tw = { .id = "builtin:wifi", .icon = "wifi" };
        snprintf(tw.tooltip, sizeof(tw.tooltip), "%s", g_cached_net);
        tray_slot_style(wifi_idx, &tw);
        if (g_tray_glyph_idx[app_slots] >= 0)
            luna_set_text(g_tray_glyph_idx[app_slots], tray_glyph("wifi"));
    }
    if (bat_idx >= 0) {
        set_hidden(bat_idx, 0);
        LunaTrayEntry tb = { .id = "builtin:bat", .icon = "bat" };
        if (bat >= 0) snprintf(tb.tooltip, sizeof(tb.tooltip), "Battery %d%%", bat);
        else snprintf(tb.tooltip, sizeof(tb.tooltip), "AC Power");
        tray_slot_style(bat_idx, &tb);
        if (g_tray_glyph_idx[app_slots + 1] >= 0)
            luna_set_text(g_tray_glyph_idx[app_slots + 1], tray_glyph("bat"));
    }
    luna_mark_layout_dirty();
    shell_request_repaint(1); /* menubar tray */
}

static double g_toast_deadline = 0.0;
static char   g_lp_query[160] = "";

/* Pending confirmation action */
enum { ACT_NONE = 0, ACT_SHUTDOWN, ACT_RESTART, ACT_LOGOUT };
static int g_pending_action = ACT_NONE;

/* ── Small helpers ── */

static int elem_idx_of(LunaElement* e) {
    for (int i = 0; i < luna_element_count(); i++)
        if (luna_element_at(i) == e) return i;
    return -1;
}

/* Toggle visibility via the "hidden" class so display_mode is recomputed. */
static void set_hidden(int idx, int hidden) {
    if (idx < 0) return;
    int changed = hidden
        ? luna_update_classes(idx, NULL, "hidden")
        : luna_update_classes(idx, "hidden", NULL);
    if (!changed) return;
    /* Restyling already marks layout dirty.  Mark only the single-buffer
     * backends here; Wayland callers request their own surface repaint. */
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

static void wifi_update_ui(void);
static void eth_update_ui(void);
static void bt_update_ui(void);
static void network_update_ui(void);
static void center_element(int idx);
static void dismiss_popovers(void);
static void dismiss_luna_menu(int trap_idx);
static void dismiss_cc(int trap_idx);
static void dismiss_win_menu(void);
static void dismiss_clip_menu(void);
static void dismiss_wifi_menu(void);
static void dismiss_bt_menu(void);
static void dismiss_weather_menu(void);
static void dismiss_calendar_menu(void);
static void on_launch_app(LunaElement* e);
static int  wifi_row_number(LunaElement* e);
static int  eth_row_number(LunaElement* e);
static void net_tip_update(void);
static void net_detail_open(int is_eth, int index);
static void net_detail_close(LunaElement* e);
static void position_menu_near(int menu_idx, int anchor_idx, float fallback_x);
static int  menu_anchor_from(LunaElement* e, int fallback_idx);
static void position_menu_near(int menu_idx, int anchor_idx, float fallback_x);
static int  menu_anchor_from(LunaElement* e, int fallback_idx);

/* Wi-Fi backend operations are compiled into luna-shell through luna-wifi.h.
 * The module owns one worker thread; this render/UI thread only queues work and
 * consumes immutable snapshots.  No socket round-trip or backend command can
 * block a frame. */
static void wifi_refresh(void) {
    if (luna_wifi_request_refresh()) {
        g_wifi_service_available = 1;
        g_wifi_busy = 1;
        g_last_wifi_request = g_now;
    }
}

static int wifi_request_connect(const char* id, const char* passphrase) {
    int ok = luna_wifi_request_connect(id, passphrase ? passphrase : "");
    if (ok) { g_wifi_busy = 1; g_last_wifi_request = g_now; }
    return ok;
}

static int wifi_request_disconnect(const char* id) {
    int ok = luna_wifi_request_disconnect(id);
    if (ok) { g_wifi_busy = 1; g_last_wifi_request = g_now; }
    return ok;
}

static int wifi_request_powered(int powered) {
    int ok = luna_wifi_request_set_powered(powered != 0);
    if (ok) { g_wifi_busy = 1; g_last_wifi_request = g_now; }
    return ok;
}

static void eth_refresh(void) {
    if (luna_ethernet_request_refresh()) {
        g_eth_service_available = 1;
        g_eth_busy = 1;
        g_last_eth_request = g_now;
    }
}

static int eth_request_connect(const char* id) {
    int ok = luna_ethernet_request_connect(id);
    if (ok) { g_eth_busy = 1; g_last_eth_request = g_now; }
    return ok;
}

static int eth_request_disconnect(const char* id) {
    int ok = luna_ethernet_request_disconnect(id);
    if (ok) { g_eth_busy = 1; g_last_eth_request = g_now; }
    return ok;
}

static void bt_refresh(void) {
    if (luna_bluetooth_request_refresh()) {
        g_bt_service_available = 1;
        g_bt_busy = 1;
        g_last_bt_request = g_now;
    }
}

static int bt_request_powered(int powered) {
    int ok = luna_bluetooth_request_set_powered(powered != 0);
    if (ok) { g_bt_busy = 1; g_last_bt_request = g_now; }
    return ok;
}

static int bt_request_connect(const char* id) {
    int ok = luna_bluetooth_request_connect(id);
    if (ok) { g_bt_busy = 1; g_last_bt_request = g_now; }
    return ok;
}

static int bt_request_disconnect(const char* id) {
    int ok = luna_bluetooth_request_disconnect(id);
    if (ok) { g_bt_busy = 1; g_last_bt_request = g_now; }
    return ok;
}

static void wifi_consume_snapshot(void) {
    LunaWifiSnapshot snapshot;
    if (!luna_wifi_consume(&snapshot, &g_wifi_generation)) return;
    g_wifi_backend = snapshot.backend;
    g_wifi_count = snapshot.count;
    if (g_wifi_count < 0) g_wifi_count = 0;
    if (g_wifi_count > MAX_WIFI_NETWORKS) g_wifi_count = MAX_WIFI_NETWORKS;
    memcpy(g_wifi_networks, snapshot.networks,
           (size_t)g_wifi_count * sizeof(g_wifi_networks[0]));
    if (g_wifi_count < MAX_WIFI_NETWORKS)
        memset(g_wifi_networks + g_wifi_count, 0,
               (size_t)(MAX_WIFI_NETWORKS - g_wifi_count) * sizeof(g_wifi_networks[0]));
    g_wifi_powered = snapshot.powered;
    g_wifi_service_available = snapshot.available;
    g_wifi_busy = snapshot.busy;
    snprintf(g_wifi_error, sizeof(g_wifi_error), "%s", snapshot.error);
    if (is_shown(g_wifi_menu_idx)) network_update_ui();
}

static void eth_consume_snapshot(void) {
    LunaEthernetSnapshot snapshot;
    if (!luna_ethernet_consume(&snapshot, &g_eth_generation)) return;
    g_eth_backend = snapshot.backend;
    g_eth_count = snapshot.count;
    if (g_eth_count < 0) g_eth_count = 0;
    if (g_eth_count > MAX_ETHERNET_LINKS) g_eth_count = MAX_ETHERNET_LINKS;
    memcpy(g_eth_links, snapshot.links,
           (size_t)g_eth_count * sizeof(g_eth_links[0]));
    if (g_eth_count < MAX_ETHERNET_LINKS)
        memset(g_eth_links + g_eth_count, 0,
               (size_t)(MAX_ETHERNET_LINKS - g_eth_count) * sizeof(g_eth_links[0]));
    g_eth_powered = snapshot.powered;
    g_eth_service_available = snapshot.available;
    g_eth_busy = snapshot.busy;
    snprintf(g_eth_error, sizeof(g_eth_error), "%s", snapshot.error);
    if (is_shown(g_wifi_menu_idx)) network_update_ui();
}

static void bt_consume_snapshot(void) {
    LunaBluetoothSnapshot snapshot;
    if (!luna_bluetooth_consume(&snapshot, &g_bt_generation)) return;
    g_bt_backend = snapshot.backend;
    g_bt_count = snapshot.count;
    if (g_bt_count < 0) g_bt_count = 0;
    if (g_bt_count > MAX_BT_DEVICES) g_bt_count = MAX_BT_DEVICES;
    memcpy(g_bt_devices, snapshot.devices,
           (size_t)g_bt_count * sizeof(g_bt_devices[0]));
    if (g_bt_count < MAX_BT_DEVICES)
        memset(g_bt_devices + g_bt_count, 0,
               (size_t)(MAX_BT_DEVICES - g_bt_count) * sizeof(g_bt_devices[0]));
    g_bt_powered = snapshot.powered;
    g_bt_service_available = snapshot.available;
    g_bt_busy = snapshot.busy;
    snprintf(g_bt_error, sizeof(g_bt_error), "%s", snapshot.error);
    int cc = luna_get_element_by_id("cc_bt");
    if (cc >= 0) luna_update_classes(cc, "on", g_bt_powered ? "on" : NULL);
    if (is_shown(g_bt_menu_idx)) {
        bt_update_ui();
        luna_mark_layout_dirty();
    }
}

static void wifi_tick(void) {
    wifi_consume_snapshot();
    eth_consume_snapshot();
    bt_consume_snapshot();
    /* Refresh a visible network list without ever performing the scan/read on
     * the render thread.  Coalescing in luna-wifi.h keeps this bounded. */
    if (is_shown(g_wifi_menu_idx) && !g_wifi_busy &&
        g_now - g_last_wifi_request >= 5.0)
        wifi_refresh();
    if (is_shown(g_wifi_menu_idx) && !g_eth_busy &&
        g_now - g_last_eth_request >= 5.0)
        eth_refresh();
    if (is_shown(g_bt_menu_idx) && !g_bt_busy &&
        g_now - g_last_bt_request >= 5.0)
        bt_refresh();
    if (is_shown(g_wifi_menu_idx))
        net_tip_update();
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

static void wifi_update_ui(void) {
    int p = luna_get_element_by_id("wifi_power");
    if (p >= 0) {
        luna_update_classes(p, "on", g_wifi_powered ? "on" : NULL);
    }
    int cc = luna_get_element_by_id("cc_wifi");
    if (cc >= 0) {
        luna_update_classes(cc, "on", g_wifi_powered ? "on" : NULL);
    }
    int st = luna_get_element_by_id("wifi_status");
    if (st >= 0) luna_set_text(st,
        !g_wifi_service_available ? "Wi-Fi backend unavailable" :
        (g_wifi_busy ? "Updating…" :
        (g_wifi_error[0] ? g_wifi_error :
        (g_wifi_backend == WIFI_NONE ? "ConnMan / NetworkManager not found" :
        (!g_wifi_powered ? "Wi-Fi is turned off" :
         (g_wifi_count ? "Available networks" : "No networks found"))))));
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        char id[24]; snprintf(id, sizeof(id), "wifi_%d", i);
        int row = luna_get_element_by_id(id);
        if (row < 0) continue;
        if (i >= g_wifi_count) { set_hidden(row, 1); continue; }
        set_hidden(row, 0);
        luna_update_classes(row, "connected",
                            g_wifi_networks[i].connected ? "connected" : NULL);
        for (int j = 0; j < luna_element_count(); j++) {
            LunaElement* child = luna_element_at(j);
            if (child->parent_idx != row) continue;
            if (strstr(child->class_name, "mi_label")) luna_set_text(j, g_wifi_networks[i].name);
            if (strstr(child->class_name, "wifi_signal")) {
                char sig[20];
                if (g_wifi_networks[i].connected) snprintf(sig, sizeof(sig), "Connected");
                else if (g_wifi_networks[i].signal >= 0) snprintf(sig, sizeof(sig), "%d%%", g_wifi_networks[i].signal);
                else if (g_wifi_networks[i].saved) snprintf(sig, sizeof(sig), "Saved");
                else sig[0] = 0;
                luna_set_text(j, sig);
            }
        }
    }
}

static void eth_update_ui(void) {
    int section = luna_get_element_by_id("eth_section");
    int sep = luna_get_element_by_id("eth_sep");
    int st = luna_get_element_by_id("eth_status");
    int has_backend = g_eth_service_available && g_eth_backend != ETH_NONE;
    int show_section = has_backend || g_eth_count > 0 || g_eth_busy || g_eth_error[0];
    if (section >= 0) set_hidden(section, !show_section);
    if (sep >= 0) set_hidden(sep, !show_section);
    if (st >= 0) {
        set_hidden(st, !show_section);
        luna_set_text(st,
            !g_eth_service_available ? "Ethernet backend unavailable" :
            (g_eth_busy ? "Updating…" :
            (g_eth_error[0] ? g_eth_error :
            (!g_eth_powered ? "Ethernet is turned off" :
             (g_eth_count ? "Wired connections" : "No Ethernet interfaces")))));
    }
    for (int i = 0; i < MAX_ETHERNET_LINKS; i++) {
        char id[24]; snprintf(id, sizeof(id), "eth_%d", i);
        int row = luna_get_element_by_id(id);
        if (row < 0) continue;
        if (!show_section || i >= g_eth_count) { set_hidden(row, 1); continue; }
        set_hidden(row, 0);
        luna_update_classes(row, "connected",
                            g_eth_links[i].connected ? "connected" : NULL);
        for (int j = 0; j < luna_element_count(); j++) {
            LunaElement* child = luna_element_at(j);
            if (child->parent_idx != row) continue;
            if (strstr(child->class_name, "mi_label")) luna_set_text(j, g_eth_links[i].name);
            if (strstr(child->class_name, "eth_state")) {
                char sig[32];
                if (g_eth_links[i].connected) snprintf(sig, sizeof(sig), "Connected");
                else if (g_eth_links[i].available) snprintf(sig, sizeof(sig), "Cable");
                else snprintf(sig, sizeof(sig), "Unplugged");
                luna_set_text(j, sig);
            }
        }
    }
}

/* ── Interface address helpers (hover tip + detail dialog) ── */

typedef struct {
    char iface[32];
    char ipv4[48];
    char ipv6[64];
    char gateway[48];
    char dns[96];
    char mac[24];
} NetIfaceInfo;

static int net_iface_is_wireless(const char* name) {
    char path[256];
    struct stat st;
    if (!name || !*name) return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    return stat(path, &st) == 0;
}

static int net_iface_is_up(const char* name) {
    char path[256], state[24] = {0};
    FILE* f;
    if (!name || !*name) return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
    f = fopen(path, "r");
    if (!f) return 0;
    (void)fgets(state, sizeof(state), f);
    fclose(f);
    return strncmp(state, "up", 2) == 0;
}

static void net_read_mac(const char* iface, char* out, size_t n) {
    char path[256];
    FILE* f;
    out[0] = 0;
    if (!iface || !*iface || n < 2) return;
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        size_t len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = 0;
    }
    fclose(f);
}

static void net_read_gateway(char* out, size_t n) {
    FILE* f = fopen("/proc/net/route", "r");
    char line[256];
    out[0] = 0;
    if (!f) return;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; } /* header */
    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned long dest = 0, gateway = 0;
        int flags = 0;
        if (sscanf(line, "%63s %lx %lx %X", iface, &dest, &gateway, &flags) < 4)
            continue;
        if (dest != 0 || !(flags & 0x2) || gateway == 0) continue;
        struct in_addr addr;
        addr.s_addr = (uint32_t)gateway;
        inet_ntop(AF_INET, &addr, out, (socklen_t)n);
        break;
    }
    fclose(f);
}

static void net_read_dns(char* out, size_t n) {
    FILE* f = fopen("/etc/resolv.conf", "r");
    char line[256];
    size_t used = 0;
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "nameserver", 10) != 0) continue;
        p += 10;
        while (*p && isspace((unsigned char)*p)) p++;
        char* end = p;
        while (*end && !isspace((unsigned char)*end)) end++;
        *end = 0;
        if (!*p) continue;
        size_t len = strlen(p);
        if (used && used + 2 < n) { out[used++] = ','; out[used++] = ' '; out[used] = 0; }
        if (used + len + 1 > n) break;
        memcpy(out + used, p, len + 1);
        used += len;
        if (used > 40) break; /* keep the tip/dialog short */
    }
    fclose(f);
}

static void net_pick_iface(int want_wifi, const char* prefer, char* out, size_t n) {
    DIR* d;
    struct dirent* de;
    out[0] = 0;
    if (prefer && *prefer && strcmp(prefer, "lo") &&
        net_iface_is_wireless(prefer) == want_wifi &&
        net_iface_is_up(prefer)) {
        snprintf(out, n, "%s", prefer);
        return;
    }
    d = opendir("/sys/class/net");
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo")) continue;
        if (net_iface_is_wireless(de->d_name) != want_wifi) continue;
        if (!net_iface_is_up(de->d_name)) continue;
        snprintf(out, n, "%s", de->d_name);
        break;
    }
    closedir(d);
}

static void net_collect_iface(int want_wifi, const char* prefer, NetIfaceInfo* info) {
    struct ifaddrs* ifa_list = NULL;
    memset(info, 0, sizeof(*info));
    snprintf(info->ipv4, sizeof(info->ipv4), "—");
    snprintf(info->ipv6, sizeof(info->ipv6), "—");
    snprintf(info->gateway, sizeof(info->gateway), "—");
    snprintf(info->dns, sizeof(info->dns), "—");
    snprintf(info->mac, sizeof(info->mac), "—");
    net_pick_iface(want_wifi, prefer, info->iface, sizeof(info->iface));
    if (!info->iface[0]) {
        snprintf(info->iface, sizeof(info->iface), "—");
        return;
    }
    net_read_mac(info->iface, info->mac, sizeof(info->mac));
    if (!info->mac[0]) snprintf(info->mac, sizeof(info->mac), "—");
    {
        char gw[48] = {0};
        net_read_gateway(gw, sizeof(gw));
        if (gw[0]) snprintf(info->gateway, sizeof(info->gateway), "%s", gw);
    }
    {
        char dns[96] = {0};
        net_read_dns(dns, sizeof(dns));
        if (dns[0]) snprintf(info->dns, sizeof(info->dns), "%s", dns);
    }
    if (getifaddrs(&ifa_list) != 0) return;
    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        char buf[INET6_ADDRSTRLEN];
        if (!ifa->ifa_addr || !ifa->ifa_name) continue;
        if (strcmp(ifa->ifa_name, info->iface) != 0) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
                snprintf(info->ipv4, sizeof(info->ipv4), "%s", buf);
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* sa = (struct sockaddr_in6*)ifa->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&sa->sin6_addr)) continue;
            if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf)))
                snprintf(info->ipv6, sizeof(info->ipv6), "%s", buf);
        }
    }
    freeifaddrs(ifa_list);
}

static const char* eth_prefer_iface(const EthernetLink* n) {
    if (!n) return NULL;
    if (!strncmp(n->id, "iface:", 6)) return n->id + 6;
    /* nmcli stores the device name in id */
    if (g_eth_backend == ETH_NMCLI && n->id[0] && strchr(n->id, '_') == NULL)
        return n->id;
    return NULL;
}

static void net_format_tip(int is_eth, int index, char* out, size_t n) {
    NetIfaceInfo info;
    out[0] = 0;
    if (is_eth) {
        if (index < 0 || index >= g_eth_count) return;
        EthernetLink* link = &g_eth_links[index];
        net_collect_iface(0, eth_prefer_iface(link), &info);
        if (link->connected)
            snprintf(out, n, "%s\nIPv4 %s · %s", link->name, info.ipv4, info.iface);
        else
            snprintf(out, n, "%s\n%s · %s", link->name,
                     link->available ? "Cable present" : "Unplugged",
                     info.iface[0] && strcmp(info.iface, "—") ? info.iface : "no address");
        return;
    }
    if (index < 0 || index >= g_wifi_count) return;
    WifiNetwork* w = &g_wifi_networks[index];
    net_collect_iface(1, NULL, &info);
    if (w->connected)
        snprintf(out, n, "%s\nIPv4 %s · %s · %s", w->name, info.ipv4, info.iface,
                 w->secure ? "Secured" : "Open");
    else if (w->signal >= 0)
        snprintf(out, n, "%s\nSignal %d%% · %s%s", w->name, w->signal,
                 w->secure ? "Secured" : "Open",
                 w->saved ? " · Saved" : "");
    else
        snprintf(out, n, "%s\n%s%s", w->name,
                 w->secure ? "Secured" : "Open",
                 w->saved ? " · Saved" : "");
}

static void net_tip_update(void) {
    int tip = luna_get_element_by_id("net_tip");
    int line = luna_get_element_by_id("net_tip_line");
    int found = -1, is_eth = 0;
    int was_shown;
    char text[192];
    if (tip < 0) return;
    was_shown = is_shown(tip);
    if (!is_shown(g_wifi_menu_idx)) {
        if (was_shown) {
            set_hidden(tip, 1);
            luna_mark_layout_dirty();
        }
        return;
    }
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* e = luna_element_at(i);
        int row;
        if (!e || !e->is_hovered) continue;
        row = wifi_row_number(e);
        if (row >= 0 && row < g_wifi_count) { found = row; is_eth = 0; break; }
        row = eth_row_number(e);
        if (row >= 0 && row < g_eth_count) { found = row; is_eth = 1; break; }
    }
    if (found < 0) {
        if (was_shown) {
            set_hidden(tip, 1);
            luna_mark_layout_dirty();
        }
        return;
    }
    net_format_tip(is_eth, found, text, sizeof(text));
    if (line >= 0) luna_set_text(line, text);
    set_hidden(tip, 0);
    if (!was_shown) luna_mark_layout_dirty();
}

static void net_detail_close(LunaElement* e) {
    (void)e;
    g_net_detail_kind = -1;
    g_net_detail_index = -1;
    if (g_net_detail_idx >= 0) set_hidden(g_net_detail_idx, 1);
}

static void net_detail_fill(int is_eth, int index) {
    NetIfaceInfo info;
    char title[96], status[96], extra[96];
    int icon = luna_get_element_by_id("net_detail_icon");
    int action = luna_get_element_by_id("net_detail_action");
    g_net_detail_kind = is_eth ? 1 : 0;
    g_net_detail_index = index;
    title[0] = status[0] = extra[0] = 0;

    if (!is_eth && index >= 0 && index < g_wifi_count) {
        WifiNetwork* w = &g_wifi_networks[index];
        snprintf(title, sizeof(title), "%s", w->name);
        if (w->connected) snprintf(status, sizeof(status), "Connected");
        else if (w->saved) snprintf(status, sizeof(status), "Saved network");
        else snprintf(status, sizeof(status), "Available");
        net_collect_iface(1, NULL, &info);
        if (w->signal >= 0)
            snprintf(extra, sizeof(extra), "%s · Signal %d%%",
                     info.mac[0] ? info.mac : "—", w->signal);
        else
            snprintf(extra, sizeof(extra), "%s · %s",
                     info.mac[0] ? info.mac : "—",
                     w->secure ? "Secured" : "Open");
        if (icon >= 0) {
            for (int j = 0; j < luna_element_count(); j++) {
                LunaElement* child = luna_element_at(j);
                if (child->parent_idx == icon && strstr(child->class_name, "luna_icon"))
                    luna_set_text(j, "\uf1eb");
            }
        }
        if (action >= 0) {
            set_hidden(action, 0);
            luna_set_text(action, w->connected ? "Disconnect" : "Connect");
            luna_update_classes(action, "danger primary",
                                w->connected ? "danger" : "primary");
        }
    } else if (is_eth && index >= 0 && index < g_eth_count) {
        EthernetLink* link = &g_eth_links[index];
        snprintf(title, sizeof(title), "%s", link->name);
        if (link->connected) snprintf(status, sizeof(status), "Connected");
        else if (link->available) snprintf(status, sizeof(status), "Cable present");
        else snprintf(status, sizeof(status), "Unplugged");
        net_collect_iface(0, eth_prefer_iface(link), &info);
        snprintf(extra, sizeof(extra), "%s", info.mac[0] ? info.mac : "—");
        if (icon >= 0) {
            for (int j = 0; j < luna_element_count(); j++) {
                LunaElement* child = luna_element_at(j);
                if (child->parent_idx == icon && strstr(child->class_name, "luna_icon"))
                    luna_set_text(j, "\uf6ff");
            }
        }
        if (action >= 0) {
            set_hidden(action, 0);
            luna_set_text(action, link->connected ? "Disconnect" : "Connect");
            luna_update_classes(action, "danger primary",
                                link->connected ? "danger" : "primary");
        }
    } else {
        snprintf(title, sizeof(title), "Network");
        snprintf(status, sizeof(status), "Unavailable");
        net_collect_iface(1, NULL, &info);
        snprintf(extra, sizeof(extra), "%s", info.mac);
        if (action >= 0) set_hidden(action, 1);
    }

    {
        int t = luna_get_element_by_id("net_detail_title");
        int s = luna_get_element_by_id("net_detail_status");
        int a = luna_get_element_by_id("nd_iface");
        int v4 = luna_get_element_by_id("nd_ipv4");
        int v6 = luna_get_element_by_id("nd_ipv6");
        int gw = luna_get_element_by_id("nd_gateway");
        int dns = luna_get_element_by_id("nd_dns");
        int ex = luna_get_element_by_id("nd_extra");
        if (t >= 0) luna_set_text(t, title);
        if (s >= 0) luna_set_text(s, status);
        if (a >= 0) luna_set_text(a, info.iface);
        if (v4 >= 0) luna_set_text(v4, info.ipv4);
        if (v6 >= 0) luna_set_text(v6, info.ipv6);
        if (gw >= 0) luna_set_text(gw, info.gateway);
        if (dns >= 0) luna_set_text(dns, info.dns);
        if (ex >= 0) luna_set_text(ex, extra);
    }
}

static void net_detail_open(int is_eth, int index) {
    dismiss_popovers();
    net_detail_fill(is_eth, index);
    if (g_net_detail_idx < 0) return;
    set_hidden(g_net_detail_idx, 0);
    center_element(g_net_detail_box_idx >= 0 ? g_net_detail_box_idx : g_net_detail_idx);
}

static void on_net_detail_action(LunaElement* e) {
    (void)e;
    if (g_net_detail_kind == 0 && g_net_detail_index >= 0 &&
        g_net_detail_index < g_wifi_count) {
        WifiNetwork* n = &g_wifi_networks[g_net_detail_index];
        if (n->connected) {
            if (!wifi_request_disconnect(n->id))
                toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
            else
                toast_show("Wi-Fi", "Disconnecting...", 2.0);
        } else if (n->saved || !n->secure) {
            if (!wifi_request_connect(n->id, ""))
                toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
            else
                toast_show("Wi-Fi", "Connecting...", 2.0);
        } else {
            /* Need passphrase — reopen Wi-Fi menu credentials. */
            int saved = g_net_detail_index;
            WifiNetwork* need = &g_wifi_networks[saved];
            net_detail_close(NULL);
            g_wifi_selected = saved;
            {
                int label = luna_get_element_by_id("wifi_selected");
                if (label >= 0) {
                    char text[150];
                    snprintf(text, sizeof(text), "Connect to %s", need->name);
                    luna_set_text(label, text);
                }
            }
            set_hidden(luna_get_element_by_id("wifi_credentials"), 0);
            set_hidden(g_wifi_menu_idx, 0);
            luna_mark_layout_dirty();
            return;
        }
        net_detail_close(NULL);
        return;
    }
    if (g_net_detail_kind == 1 && g_net_detail_index >= 0 &&
        g_net_detail_index < g_eth_count) {
        EthernetLink* n = &g_eth_links[g_net_detail_index];
        if (n->connected) {
            if (!eth_request_disconnect(n->id))
                toast_show("Ethernet", "Ethernet worker is unavailable", 2.5);
            else
                toast_show("Ethernet", "Disconnecting...", 2.0);
        } else {
            if (!eth_request_connect(n->id))
                toast_show("Ethernet", "Ethernet worker is unavailable", 2.5);
            else
                toast_show("Ethernet", "Connecting...", 2.0);
        }
        net_detail_close(NULL);
    }
}

/* ── Calendar popover ── */

static int calendar_days_in_month(int year, int month) {
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1) {
        int leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    if (month < 0 || month > 11) return 30;
    return mdays[month];
}

static void calendar_fill(void) {
    static const char* months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    time_t now = time(NULL);
    struct tm tm_now;
    char label[64];
    int first_wday, dim, prev_dim, today_y, today_m, today_d;
    if (!localtime_r(&now, &tm_now)) return;
    today_y = tm_now.tm_year + 1900;
    today_m = tm_now.tm_mon;
    today_d = tm_now.tm_mday;
    if (g_cal_year < 1970) {
        g_cal_year = today_y;
        g_cal_month = today_m;
        g_cal_selected_day = today_d;
    }
    {
        struct tm first = {0};
        first.tm_year = g_cal_year - 1900;
        first.tm_mon = g_cal_month;
        first.tm_mday = 1;
        first.tm_isdst = -1;
        if (mktime(&first) == (time_t)-1) first_wday = 0;
        else first_wday = first.tm_wday;
    }
    dim = calendar_days_in_month(g_cal_year, g_cal_month);
    prev_dim = calendar_days_in_month(
        g_cal_month == 0 ? g_cal_year - 1 : g_cal_year,
        g_cal_month == 0 ? 11 : g_cal_month - 1);
    snprintf(label, sizeof(label), "%s %d", months[g_cal_month], g_cal_year);
    {
        int lab = luna_get_element_by_id("cal_month_label");
        if (lab >= 0) luna_set_text(lab, label);
    }
    for (int i = 0; i < 42; i++) {
        char id[16], text[16];
        int day, other = 0, is_today = 0, is_sel = 0;
        int idx;
        snprintf(id, sizeof(id), "cal_d%d", i);
        idx = luna_get_element_by_id(id);
        if (idx < 0) continue;
        if (i < first_wday) {
            day = prev_dim - first_wday + i + 1;
            other = 1;
        } else if (i - first_wday + 1 > dim) {
            day = i - first_wday + 1 - dim;
            other = 1;
        } else {
            day = i - first_wday + 1;
            if (g_cal_year == today_y && g_cal_month == today_m && day == today_d)
                is_today = 1;
            if (day == g_cal_selected_day) is_sel = 1;
        }
        snprintf(text, sizeof(text), "%d", day);
        luna_set_text(idx, text);
        luna_update_classes(idx, "other today selected",
                            other ? "other" :
                            (is_today ? "today" : (is_sel ? "selected" : NULL)));
        if (!other && is_today && is_sel)
            luna_update_classes(idx, "other today selected", "today selected");
        else if (!other && is_today)
            luna_update_classes(idx, "other today selected", "today");
        else if (!other && is_sel)
            luna_update_classes(idx, "other today selected", "selected");
        else if (other)
            luna_update_classes(idx, "other today selected", "other");
        else
            luna_update_classes(idx, "other today selected", NULL);
    }
}

static void dismiss_calendar_menu(void) {
    if (is_shown(g_calendar_menu_idx)) set_hidden(g_calendar_menu_idx, 1);
}

static void on_calendar_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    if (is_shown(g_calendar_menu_idx)) { dismiss_calendar_menu(); return; }
    {
        time_t now = time(NULL);
        struct tm tm_now;
        if (localtime_r(&now, &tm_now)) {
            g_cal_year = tm_now.tm_year + 1900;
            g_cal_month = tm_now.tm_mon;
            g_cal_selected_day = tm_now.tm_mday;
        }
    }
    calendar_fill();
    set_hidden(g_calendar_menu_idx, 0);
    position_menu_near(g_calendar_menu_idx,
                       menu_anchor_from(e, g_mb_clock_idx >= 0 ? g_mb_clock_idx
                                        : luna_get_element_by_id("mb_clock")),
                       luna_window_width - 310.0f);
}

static void on_calendar_prev(LunaElement* e) {
    (void)e;
    if (--g_cal_month < 0) { g_cal_month = 11; g_cal_year--; }
    g_cal_selected_day = 0;
    calendar_fill();
}

static void on_calendar_next(LunaElement* e) {
    (void)e;
    if (++g_cal_month > 11) { g_cal_month = 0; g_cal_year++; }
    g_cal_selected_day = 0;
    calendar_fill();
}

static void on_calendar_today(LunaElement* e) {
    (void)e;
    time_t now = time(NULL);
    struct tm tm_now;
    if (!localtime_r(&now, &tm_now)) return;
    g_cal_year = tm_now.tm_year + 1900;
    g_cal_month = tm_now.tm_mon;
    g_cal_selected_day = tm_now.tm_mday;
    calendar_fill();
}

static void on_calendar_day(LunaElement* e) {
    int idx = elem_idx_of(e);
    const char* id;
    int cell, day;
    if (idx < 0) return;
    id = luna_element_at(idx)->id;
    if (!str_has_prefix(id, "cal_d")) return;
    cell = atoi(id + 5);
    if (cell < 0 || cell > 41) return;
    {
        struct tm first = {0};
        int first_wday, dim;
        first.tm_year = g_cal_year - 1900;
        first.tm_mon = g_cal_month;
        first.tm_mday = 1;
        first.tm_isdst = -1;
        if (mktime(&first) == (time_t)-1) first_wday = 0;
        else first_wday = first.tm_wday;
        dim = calendar_days_in_month(g_cal_year, g_cal_month);
        day = cell - first_wday + 1;
        if (day < 1 || day > dim) return;
        g_cal_selected_day = day;
        calendar_fill();
    }
}

static void bt_update_ui(void) {
    int p = luna_get_element_by_id("bt_power");
    if (p >= 0) luna_update_classes(p, "on", g_bt_powered ? "on" : NULL);
    int cc = luna_get_element_by_id("cc_bt");
    if (cc >= 0) luna_update_classes(cc, "on", g_bt_powered ? "on" : NULL);
    int st = luna_get_element_by_id("bt_status");
    if (st >= 0) luna_set_text(st,
        !g_bt_service_available ? "Bluetooth backend unavailable" :
        (g_bt_busy ? "Updating…" :
        (g_bt_error[0] ? g_bt_error :
        (g_bt_backend == BT_NONE ? "bluetoothctl / ConnMan not found" :
        (!g_bt_powered ? "Bluetooth is turned off" :
         (g_bt_count ? "Devices" : "No devices found"))))));
    for (int i = 0; i < MAX_BT_DEVICES; i++) {
        char id[24]; snprintf(id, sizeof(id), "bt_%d", i);
        int row = luna_get_element_by_id(id);
        if (row < 0) continue;
        if (i >= g_bt_count) { set_hidden(row, 1); continue; }
        set_hidden(row, 0);
        luna_update_classes(row, "connected",
                            g_bt_devices[i].connected ? "connected" : NULL);
        for (int j = 0; j < luna_element_count(); j++) {
            LunaElement* child = luna_element_at(j);
            if (child->parent_idx != row) continue;
            if (strstr(child->class_name, "mi_label")) luna_set_text(j, g_bt_devices[i].name);
            if (strstr(child->class_name, "bt_state")) {
                char sig[24];
                if (g_bt_devices[i].connected) snprintf(sig, sizeof(sig), "Connected");
                else if (g_bt_devices[i].paired) snprintf(sig, sizeof(sig), "Paired");
                else sig[0] = 0;
                luna_set_text(j, sig);
            }
        }
    }
}

static void network_update_ui(void) {
    wifi_update_ui();
    eth_update_ui();
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
    float mw = m->css_width > 1.0f ? m->css_width :
               (m->w > 1.0f ? m->w : 240.0f);
    float mh = m->css_height > 1.0f ? m->css_height :
               (m->h > 1.0f ? m->h : 220.0f);
    /* A previous top+bottom stretch (or unresolved auto→0) can leave h near
     * the output height.  Prefer content height so upward open stays above
     * the taskbar instead of clamping to the top edge. */
    if (mh > luna_window_height * 0.45f) {
        float content_h = 0.0f;
        for (int i = 0; i < luna_element_count(); i++) {
            LunaElement* c = luna_element_at(i);
            if (!c || c->parent_idx != menu_idx || c->display_none) continue;
            if (strstr(c->class_name, "hidden")) continue;
            float bottom = c->rel_y + c->h;
            if (bottom > content_h) content_h = bottom;
        }
        if (content_h > 24.0f && content_h < luna_window_height * 0.9f)
            mh = content_h + 12.0f;
        else if (m->css_height > 1.0f && m->css_height < luna_window_height * 0.45f)
            mh = m->css_height;
        else
            mh = 280.0f;
    }
    int open_up = (g_chrome_menubar_edge == SKIN_EDGE_BOTTOM);
    float y;
    if (open_up)
        y = luna_window_height - (float)g_chrome_menubar_height - mh - 4.0f;
    else
        y = (float)(g_chrome_menubar_height + 4);

    if (anchor_idx >= 0) {
        LunaElement* a = luna_element_at(anchor_idx);
        if (a) {
            x = a->x;
            if (open_up)
                y = a->y - mh - 4.0f;
            else
                y = a->y + a->h + 4.0f;
        }
    }
    if (x + mw > luna_window_width - 8.0f)
        x = luna_window_width - mw - 8.0f;
    if (x < 6.0f) x = 6.0f;
    /* Keep the menu near the chrome edge instead of slamming tall menus to
     * the opposite side of the output (the old y<6 clamp looked like every
     * tray menu dropped from the top of a bottom taskbar). */
    if (open_up) {
        float min_y = 6.0f;
        float max_bottom = luna_window_height - (float)g_chrome_menubar_height - 4.0f;
        if (y + mh > max_bottom) y = max_bottom - mh;
        if (y < min_y) y = min_y;
    } else {
        if (y + mh > luna_window_height - 8.0f)
            y = luna_window_height - mh - 8.0f;
        if (y < (float)(g_chrome_menubar_height + 2))
            y = (float)(g_chrome_menubar_height + 2);
        if (y < 6.0f) y = 6.0f;
    }
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

/* Prefer the clicked element as the menu anchor when it resolves to a real
 * widget; otherwise fall back to the menubar status control. */
static int menu_anchor_from(LunaElement* e, int fallback_idx) {
    int idx = elem_idx_of(e);
    if (idx >= 0) {
        LunaElement* a = luna_element_at(idx);
        if (a && a->w > 1.0f && a->h > 1.0f) return idx;
    }
    return fallback_idx;
}

/* The control center has a fixed visual width.  Position it from the right
 * edge explicitly instead of relying on the CSS right inset: this also keeps
 * Wi-Fi and Control Center clicks opening the same, correctly aligned panel. */
static void position_control_center(void) {
    if (g_cc_idx < 0) return;
    LunaElement* m = luna_element_at(g_cc_idx);
    float w = m->css_width > 1.0f ? m->css_width : (m->w > 1.0f ? m->w : 324.0f);
    float h = m->css_height > 1.0f ? m->css_height : (m->h > 1.0f ? m->h : 360.0f);
    m->rel_x = floorf(luna_window_width - w - 8.0f);
    if (g_chrome_menubar_edge == SKIN_EDGE_BOTTOM)
        m->rel_y = floorf(luna_window_height - (float)g_chrome_menubar_height - h - 8.0f);
    else
        m->rel_y = (float)(g_chrome_menubar_height + 4);
    if (m->rel_x < 6.0f) m->rel_x = 6.0f;
    if (m->rel_y < 6.0f) m->rel_y = 6.0f;
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

static void apply_ui_language(const char* locale_name) {
    if (!locale_name || !*locale_name) return;
    setenv("LANG", locale_name, 1);
    setenv("LC_ALL", locale_name, 1);
    setenv("LC_MESSAGES", locale_name, 1);
    setenv("LC_TIME", locale_name, 1);
    /* Refresh libc-backed dates immediately when the locale is installed.
     * A missing optional locale must not prevent saving the user's choice;
     * C.UTF-8 remains available as a portable fallback in Settings. */
    (void)setlocale(LC_ALL, "");
}

static int apply_numlock_setting(void) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "keyboard_lock numlock %d", g_settings.numlock_on);
    setenv("LUNA_NUMLOCK", g_settings.numlock_on ? "1" : "0", 1);
    return shell_send_cmd(cmd);
}

static int gtk_ini_has_key(const char* path, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t key_len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, key_len)) {
            p += key_len;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') { found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

static void ensure_gtk_ini_defaults(const char* path, const char* layout,
                                    const char* font_name) {
    int have_layout = gtk_ini_has_key(path, "gtk-decoration-layout");
    int have_font = gtk_ini_has_key(path, "gtk-font-name");
    int have_dpi = gtk_ini_has_key(path, "gtk-xft-dpi");
    if (have_layout && have_font && have_dpi) return;

    FILE* f = fopen(path, access(path, F_OK) == 0 ? "a" : "w");
    if (!f) return;
    /* Xorg/LXDE normally supplies these through XSettings.  A bare Wayland
     * session has no XSettings manager, so GTK otherwise falls back to an
     * 11-point font even when the Xorg desktop used 9 points.  Keep existing
     * user choices; only fill values which are absent. */
    fprintf(f, "\n[Settings]\n");
    if (!have_layout) fprintf(f, "gtk-decoration-layout=%s\n", layout);
    if (!have_font) fprintf(f, "gtk-font-name=%s\n", font_name);
    if (!have_dpi) fprintf(f, "gtk-xft-dpi=98304\n"); /* 96 * 1024 */
    fclose(f);
}

/* Rewrite (or create) gtk settings.ini so gtk-theme-name matches the skin.
 * Other Settings keys are preserved when present. */
static void gtk_ini_set_theme(const char* path, const char* theme_name,
                              const char* layout, const char* font_name) {
    if (!path || !theme_name || !*theme_name) return;
    char tmp[640];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* in = fopen(path, "r");
    FILE* out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return;
    }
    int saw_settings = 0;
    int wrote_theme = 0;
    int wrote_layout = 0;
    int wrote_font = 0;
    int wrote_dpi = 0;
    char line[512];
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '[') {
                if (!strncmp(p, "[Settings]", 10)) saw_settings = 1;
                else if (saw_settings && !wrote_theme) {
                    fprintf(out, "gtk-theme-name=%s\n", theme_name);
                    wrote_theme = 1;
                    if (layout && *layout && !wrote_layout) {
                        fprintf(out, "gtk-decoration-layout=%s\n", layout);
                        wrote_layout = 1;
                    }
                    if (font_name && *font_name && !wrote_font) {
                        fprintf(out, "gtk-font-name=%s\n", font_name);
                        wrote_font = 1;
                    }
                    if (!wrote_dpi) {
                        fprintf(out, "gtk-xft-dpi=98304\n");
                        wrote_dpi = 1;
                    }
                    saw_settings = 0;
                }
                fputs(line, out);
                continue;
            }
            if (!strncmp(p, "gtk-theme-name", 14)) {
                char* eq = strchr(p, '=');
                if (eq) {
                    fprintf(out, "gtk-theme-name=%s\n", theme_name);
                    wrote_theme = 1;
                    continue;
                }
            }
            if (!strncmp(p, "gtk-decoration-layout", 21)) wrote_layout = 1;
            if (!strncmp(p, "gtk-font-name", 13)) wrote_font = 1;
            if (!strncmp(p, "gtk-xft-dpi", 11)) wrote_dpi = 1;
            fputs(line, out);
        }
        fclose(in);
    }
    if (!wrote_theme) {
        fprintf(out, "[Settings]\n");
        fprintf(out, "gtk-theme-name=%s\n", theme_name);
        if (layout && *layout) fprintf(out, "gtk-decoration-layout=%s\n", layout);
        if (font_name && *font_name) fprintf(out, "gtk-font-name=%s\n", font_name);
        fprintf(out, "gtk-xft-dpi=98304\n");
    }
    fclose(out);
    rename(tmp, path);
}

static void skin_apply_toolkit(int skin_idx) {
    if (skin_idx < 0 || skin_idx >= g_skin_count) skin_idx = 0;
    const LunaSkin* skin = &g_skins[skin_idx];
    const char* layout = getenv("LUNA_GTK_BUTTON_LAYOUT");
    if (!layout || !*layout) layout = "icon:minimize,maximize,close";
    const char* font_name = getenv("LUNA_GTK_FONT_NAME");
    if (!font_name || !*font_name) font_name = "Sans 9";

    if (skin->gtk_theme[0] && skin->dir[0]) {
        /* GTK resolves $XDG_DATA_HOME/themes/<Name> before system data dirs.
         * Symlink the skin there so new clients pick it up. */
        char theme_src[PATH_MAX], idx[PATH_MAX], theme_dst[PATH_MAX];
        char themes[PATH_MAX];
        int ok = skin_join(theme_src, sizeof(theme_src), skin->dir, "gtk") &&
                 skin_join(theme_src, sizeof(theme_src), theme_src, skin->gtk_theme) &&
                 skin_join(idx, sizeof(idx), theme_src, "index.theme") &&
                 path_is_regular(idx);
        if (ok) {
            if (!path_join2(themes, sizeof(themes), g_xdg.data_home, "themes") ||
                !mkdir_p_mode(themes, 0700))
                return;
            if (!path_join2(theme_dst, sizeof(theme_dst), themes, skin->gtk_theme))
                return;
            unlink(theme_dst);
            symlink(theme_src, theme_dst);
            setenv("GTK_THEME", skin->gtk_theme, 1);
            static const char* vers[] = { "gtk-3.0", "gtk-4.0", NULL };
            for (int i = 0; vers[i]; i++) {
                char dir[512], ini[576];
                path_join2(dir, sizeof(dir), g_xdg.config_home, vers[i]);
                snprintf(ini, sizeof(ini), "%s/settings.ini", dir);
                mkdir_p_mode(dir, 0700);
                gtk_ini_set_theme(ini, skin->gtk_theme, layout, font_name);
            }
        }
    } else {
        unsetenv("GTK_THEME");
    }

    if (skin->qt_style[0])
        setenv("QT_STYLE_OVERRIDE", skin->qt_style, 1);
    else
        unsetenv("QT_STYLE_OVERRIDE");

    if (skin->qt_qss[0])
        setenv("LUNA_QT_QSS", skin->qt_qss, 1);
    else
        unsetenv("LUNA_QT_QSS");

    /* Prefer qt5ct/qt6ct when installed so Fusion + our QSS apply to new
     * Qt clients without each app loading LUNA_QT_QSS itself. */
    if (skin->qt_qss[0] || skin->qt_style[0]) {
        static const char* qtct[] = { "qt5ct", "qt6ct", NULL };
        for (int i = 0; qtct[i]; i++) {
            char dir[512], conf[576];
            path_join2(dir, sizeof(dir), g_xdg.config_home, qtct[i]);
            mkdir_p_mode(dir, 0700);
            snprintf(conf, sizeof(conf), "%s/%s.conf", dir, qtct[i]);
            FILE* qf = fopen(conf, "w");
            if (!qf) continue;
            fprintf(qf, "[Appearance]\n");
            fprintf(qf, "style=%s\n",
                    skin->qt_style[0] ? skin->qt_style : "Fusion");
            if (skin->qt_qss[0])
                fprintf(qf, "stylesheet=%s\n", skin->qt_qss);
            fprintf(qf, "standard_dialogs=default\n");
            fclose(qf);
        }
        if (!getenv("QT_QPA_PLATFORMTHEME") ||
            !strcmp(getenv("QT_QPA_PLATFORMTHEME"), "gtk2") ||
            !strcmp(getenv("QT_QPA_PLATFORMTHEME"), "gtk3")) {
            /* Only force qt5ct when the theme plugin is likely available;
             * otherwise leave the platform theme alone. */
            if (access("/usr/lib/x86_64-linux-gnu/qt5/plugins/platformthemes/libqt5ct.so", R_OK) == 0 ||
                access("/usr/lib/aarch64-linux-gnu/qt5/plugins/platformthemes/libqt5ct.so", R_OK) == 0 ||
                access("/usr/lib/qt/plugins/platformthemes/libqt5ct.so", R_OK) == 0)
                setenv("QT_QPA_PLATFORMTHEME", "qt5ct", 1);
            else if (access("/usr/lib/x86_64-linux-gnu/qt6/plugins/platformthemes/libqt6ct.so", R_OK) == 0 ||
                     access("/usr/lib/aarch64-linux-gnu/qt6/plugins/platformthemes/libqt6ct.so", R_OK) == 0)
                setenv("QT_QPA_PLATFORMTHEME", "qt6ct", 1);
        }
    }
}

/* Resolved titlebar height for a skin: the skin's own value when it declares
 * one, otherwise the shared default.  Used for both the compositor SSD bar and
 * the client chrome written to window-theme.conf so the two never diverge. */
static int skin_titlebar_height(const LunaSkin* skin) {
    int h = skin->titlebar_height > 0 ? skin->titlebar_height
                                      : LUNA_TITLEBAR_H_DEFAULT;
    if (h < LUNA_TITLEBAR_H_MIN) h = LUNA_TITLEBAR_H_MIN;
    if (h > LUNA_TITLEBAR_H_MAX) h = LUNA_TITLEBAR_H_MAX;
    return h;
}

static int skin_apply_wm_decoration(int skin_idx) {
    if (skin_idx < 0 || skin_idx >= g_skin_count) skin_idx = 0;
    const LunaSkin* skin = &g_skins[skin_idx];
    char cmd[160];
    int ok = 1;
    int style = skin->titlebar_style >= 0 ? skin->titlebar_style
                                          : g_settings.classic_titlebar;
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_style %d", style);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_colors %u %u %u",
             skin->titlebar_active, skin->titlebar_inactive, skin->titlebar_frame);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_height %d",
             skin_titlebar_height(skin));
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config prefer_ssd %d", skin->prefer_ssd ? 1 : 0);
    ok &= shell_send_cmd(cmd);
    return ok;
}


static int skin_color_is_dark(unsigned int argb) {
    if (!argb) return 1;
    unsigned r = (argb >> 16) & 255u;
    unsigned g = (argb >> 8) & 255u;
    unsigned b = argb & 255u;
    return (r * 299u + g * 587u + b * 114u) < 128000u;
}

static void skin_export_window_theme(int skin_idx) {
    if (skin_idx < 0 || skin_idx >= g_skin_count) skin_idx = 0;
    const LunaSkin* skin = &g_skins[skin_idx];
    int dark = skin->window_theme >= 0 ? skin->window_theme :
               skin_color_is_dark(skin->titlebar_active);
    LunaWindowTheme theme;
    luna_window_theme_default(&theme, dark ? LUNA_WINDOW_THEME_DARK : LUNA_WINDOW_THEME_LIGHT);
    int style = skin->titlebar_style >= 0 ? skin->titlebar_style : g_settings.classic_titlebar;
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    theme.titlebar_style = style;
    theme.titlebar_height = (float)skin_titlebar_height(skin);
    theme.controls_on_left = skin->controls_on_left;
    if (skin->titlebar_active) theme.titlebar_active = skin->titlebar_active;
    if (skin->titlebar_inactive) theme.titlebar_inactive = skin->titlebar_inactive;
    if (skin->titlebar_frame) theme.titlebar_frame = skin->titlebar_frame;
    if (dark && skin_idx == 0) {
        theme.accent = 0xff8d7bffu;
        theme.accent_hover = 0xff6a52e0u;
    }

    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (!g_xdg.runtime_dir[0] ||
        !xdg_app_dir(dir, sizeof(dir), g_xdg.runtime_dir, "luna-shell", 1) ||
        !path_join2(path, sizeof(path), dir, "window-theme.conf")) return;
    int z = snprintf(tmp, sizeof(tmp), "%s.tmp-%lu", path, (unsigned long)getpid());
    if (z < 0 || (size_t)z >= sizeof(tmp)) return;
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# Luna Shell runtime window theme — auto-generated\n");
    fprintf(f, "mode=%s\n", dark ? "dark" : "light");
    fprintf(f, "window_background=0x%08x\n", theme.window_background);
    fprintf(f, "surface=0x%08x\n", theme.surface);
    fprintf(f, "surface_secondary=0x%08x\n", theme.surface_secondary);
    fprintf(f, "border=0x%08x\n", theme.border);
    fprintf(f, "text=0x%08x\n", theme.text);
    fprintf(f, "text_secondary=0x%08x\n", theme.text_secondary);
    fprintf(f, "accent=0x%08x\n", theme.accent);
    fprintf(f, "accent_hover=0x%08x\n", theme.accent_hover);
    fprintf(f, "danger=0x%08x\n", theme.danger);
    fprintf(f, "titlebar_active=0x%08x\n", theme.titlebar_active);
    fprintf(f, "titlebar_inactive=0x%08x\n", theme.titlebar_inactive);
    fprintf(f, "titlebar_frame=0x%08x\n", theme.titlebar_frame);
    fprintf(f, "titlebar_height=%.1f\n", theme.titlebar_height);
    fprintf(f, "corner_radius=%.1f\n", theme.corner_radius);
    fprintf(f, "dialog_radius=%.1f\n", theme.dialog_radius);
    fprintf(f, "control_size=%.1f\n", theme.control_size);
    fprintf(f, "titlebar_style=%d\n", theme.titlebar_style);
    fprintf(f, "controls_on_left=%d\n", theme.controls_on_left ? 1 : 0);
    if (fclose(f) != 0) { unlink(tmp); return; }
    chmod(tmp, 0600);
    if (rename(tmp, path) != 0) { unlink(tmp); return; }
    setenv("LUNA_WINDOW_THEME_FILE", path, 1);
    setenv("LUNA_WINDOW_THEME", dark ? "dark" : "light", 1);
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
        luna_update_classes(idx, "selected",
            (vals[i] && layout && !strcmp(vals[i], layout)) ? "selected" : NULL);
    }
    int current = luna_get_element_by_id("kb_current");
    if (current >= 0) {
        const char* name = layout ? layout : "";
        if (!strcmp(name, "jp")) name = "日本語 (JIS)";
        else if (!strcmp(name, "jp,us")) name = "日本語 (JIS) + English (US)";
        else if (!strcmp(name, "us")) name = "English (US)";
        else if (!strcmp(name, "de,us")) name = "Deutsch + English (US)";
        else if (!strcmp(name, "fr,us")) name = "Français + English (US)";
        else if (!strcmp(name, "kr,us")) name = "한국어 + English (US)";
        char text[128];
        snprintf(text, sizeof(text), "Current: %s", *name ? name : "System default");
        luna_set_text(current, text);
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

/* GLFW uses libdecor for Wayland client-side decorations.  Its GTK plugin
 * requests symbolic desktop-theme icons that are often absent on a bare VT;
 * repeated NULL icon results have caused small GL clients (including
 * luna-editor) to terminate during their first mapped frame.  Prefer the
 * dependency-light Cairo plugin, keeping it isolated so libdecor cannot pick
 * the GTK plugin from the same system directory.  luna-session sets this too,
 * but doing it here also covers a shell launched directly from a console. */
static void prefer_libdecor_cairo(void) {
    if (getenv("LIBDECOR_PLUGIN_DIR")) return;
    const char* enabled = getenv("LUNA_LIBDECOR_CAIRO");
    if (enabled && (!strcmp(enabled, "0") || !strcasecmp(enabled, "no") ||
                    !strcasecmp(enabled, "false") || !strcasecmp(enabled, "off")))
        return;

    static const char* const candidates[] = {
        "/usr/lib64/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/lib/x86_64-linux-gnu/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/lib/aarch64-linux-gnu/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/lib/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/local/lib64/libdecor/plugins-1/libdecor-cairo.so",
        "/usr/local/lib/libdecor/plugins-1/libdecor-cairo.so",
        NULL
    };
    const char* plugin = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) { plugin = candidates[i]; break; }
    }
    if (!plugin) return;

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = g_xdg.runtime_dir;
    char dir[PATH_MAX], link_path[PATH_MAX];
    if (!runtime || !*runtime ||
        !path_join2(dir, sizeof(dir), runtime, "luna-libdecor-cairo") ||
        !mkdir_p_mode(dir, 0700) ||
        !path_join2(link_path, sizeof(link_path), dir, "libdecor-cairo.so"))
        return;

    if (access(link_path, R_OK) != 0) {
        struct stat st;
        /* Replace only our own stale symlink; never overwrite a regular file. */
        if (lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode))
            unlink(link_path);
        if (symlink(plugin, link_path) != 0 && errno != EEXIST) return;
    }
    if (access(link_path, R_OK) == 0)
        setenv("LIBDECOR_PLUGIN_DIR", dir, 0);
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
    /* Hint for clients that may prefer Luna-only extensions (luna_wm_v1). */
    setenv("LUNA_SESSION", "1", 0);
    if (!getenv("GDK_BACKEND"))
        setenv("GDK_BACKEND", "wayland", 0);
    if (!getenv("MOZ_ENABLE_WAYLAND"))
        setenv("MOZ_ENABLE_WAYLAND", "1", 0);
    if (!getenv("MOZ_LAYERS_ALLOW_SOFTWARE_GL"))
        setenv("MOZ_LAYERS_ALLOW_SOFTWARE_GL", "1", 0);
    if (!getenv("MOZ_GTK_TITLEBAR_DECORATION"))
        setenv("MOZ_GTK_TITLEBAR_DECORATION", "client", 0);
    if (!getenv("QT_QPA_PLATFORM"))
        setenv("QT_QPA_PLATFORM", "wayland", 0);
    prefer_libdecor_cairo();
    /* Do not inherit scale hints from the Xorg login which started Luna.
     * GDK_SCALE=2 submits a 2x buffer; prefer GDK_DPI_SCALE for text sizing.
     * Settings dialog values apply unless LUNA_* overrides are set. */
    {
        const char* gdk_scale = getenv("LUNA_GDK_SCALE");
        const char* gdk_dpi = getenv("LUNA_GDK_DPI_SCALE");
        const char* qt_scale = getenv("LUNA_QT_SCALE_FACTOR");
        const char* cursor_sz = getenv("LUNA_XCURSOR_SIZE");
        if (!gdk_scale || !*gdk_scale) gdk_scale = g_settings.gdk_scale;
        if (!gdk_dpi || !*gdk_dpi) gdk_dpi = g_settings.gdk_dpi_scale;
        if (!qt_scale || !*qt_scale) qt_scale = g_settings.qt_scale_factor;
        if (!cursor_sz || !*cursor_sz) cursor_sz = g_settings.xcursor_size;
        if (!gdk_scale || !*gdk_scale) gdk_scale = "0.75";
        if (!gdk_dpi || !*gdk_dpi) gdk_dpi = "0.75";
        if (!qt_scale || !*qt_scale) qt_scale = "1";
        if (!cursor_sz || !*cursor_sz) cursor_sz = "24";
        setenv("GDK_SCALE", gdk_scale, 1);
        setenv("GDK_DPI_SCALE", gdk_dpi, 1);
        setenv("QT_SCALE_FACTOR", qt_scale, 1);
        setenv("XCURSOR_SIZE", cursor_sz, 1);
    }
    /* luna-session may set LUNA_CLIENT_RENDERER / GSK_RENDERER.  Still reject
     * vulkan: Luna has no Vulkan WSI / dmabuf path for clients, and GTK4 then
     * stalls (Firefox dialogs never paint). */
    {
        const char* mode = getenv("LUNA_CLIENT_RENDERER");
        const char* backend = getenv("LUNA_BACKEND");
        const char* gsk = getenv("GSK_RENDERER");
        if (mode && !strcasecmp(mode, "software"))
            setenv("GSK_RENDERER", "cairo", 1);
        else if ((!mode || !*mode || !strcasecmp(mode, "auto")) &&
                 backend && !strcasecmp(backend, "software"))
            setenv("GSK_RENDERER", "cairo", 1);
        else if (!gsk || !*gsk || !strcasecmp(gsk, "vulkan"))
            setenv("GSK_RENDERER", getenv("LUNA_GSK_RENDERER") ?: "cairo", 1);
    }
    /* GTK CSD WindowControls (minimize/maximize/close) when portals absent. */
    {
        const char* layout = getenv("LUNA_GTK_BUTTON_LAYOUT");
        if (!layout || !*layout)
            layout = "icon:minimize,maximize,close";
        const char* font_name = getenv("LUNA_GTK_FONT_NAME");
        if (!font_name || !*font_name) font_name = "Sans 9";
        static const char* vers[] = { "gtk-3.0", "gtk-4.0", NULL };
        for (int i = 0; vers[i]; i++) {
            char dir[512], ini[576];
            path_join2(dir, sizeof(dir), g_xdg.config_home, vers[i]);
            snprintf(ini, sizeof(ini), "%s/settings.ini", dir);
            mkdir_p_mode(dir, 0700);
            ensure_gtk_ini_defaults(ini, layout, font_name);
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
        char cursor_path[PATH_MAX];
        if (xdg_find_data_file(cursor_path, sizeof(cursor_path),
                               "icons/aero/cursors/left_ptr"))
            setenv("XCURSOR_THEME", "aero", 0);
        else if (xdg_find_data_file(cursor_path, sizeof(cursor_path),
                                    "icons/Adwaita/cursors/left_ptr"))
            setenv("XCURSOR_THEME", "Adwaita", 0);
    }
    apply_xkb_session_env();
    apply_toolkit_session_env();

    /* Compositor holds DRM master — client GBM/EGL sees fd=-1 and Firefox
     * stalls in WaitFlushedEvent.  Select Mesa's software path for ordinary
     * children, but let Mesa choose its matching loader/Gallium driver. */
    if (!getenv("LIBGL_ALWAYS_SOFTWARE"))
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    /* llvmpipe otherwise creates close to one worker per online CPU for every
     * application.  Four workers keep 2D Luna apps responsive without starving
     * the compositor; LP_NUM_THREADS remains user-overridable. */
    if (!getenv("LP_NUM_THREADS"))
        setenv("LP_NUM_THREADS", "4", 0);
    if (!getenv("MOZ_WEBRENDER"))
        setenv("MOZ_WEBRENDER", "0", 0);
    if (!getenv("MOZ_ACCELERATED"))
        setenv("MOZ_ACCELERATED", "0", 0);
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

/* ── XDG Desktop Application Autostart ───────────────────────────────────
 * Merge $XDG_CONFIG_HOME/autostart with $XDG_CONFIG_DIRS/autostart by desktop
 * file id.  The first occurrence wins, so a user Hidden=true entry suppresses
 * a system entry with the same basename.  GIO is preferred for launching
 * because it implements the complete Desktop Entry Exec/DBusActivatable
 * rules; a small direct Exec parser keeps the shell functional without GIO. */
#define XDG_AUTOSTART_MAX_ENTRIES 128
#define XDG_EXEC_MAX_ARGS 64
#define XDG_EXEC_ARG_SIZE 1024

typedef struct {
    char path[PATH_MAX];
    char name[256];
    char icon[256];
    char exec[2048];
    char try_exec[PATH_MAX];
    char only_show_in[512];
    char not_show_in[512];
    int hidden;
    int no_display;
    int enabled;
    int application;
    int dbus_activatable;
} LunaDesktopEntry;

typedef struct {
    char storage[XDG_EXEC_MAX_ARGS][XDG_EXEC_ARG_SIZE];
    char* argv[XDG_EXEC_MAX_ARGS + 1];
    int argc;
} LunaExecArgs;

static int g_xdg_autostart_has_im = 0;
static int g_xdg_autostart_has_clipboard = 0;

static int desktop_bool(const char* value, int fallback) {
    if (!value || !*value) return fallback;
    if (!strcasecmp(value, "true") || !strcmp(value, "1") ||
        !strcasecmp(value, "yes")) return 1;
    if (!strcasecmp(value, "false") || !strcmp(value, "0") ||
        !strcasecmp(value, "no")) return 0;
    return fallback;
}

static void desktop_unescape_value(char* value) {
    char* src = value;
    char* dst = value;
    while (*src) {
        if (*src == '\\' && src[1]) {
            src++;
            switch (*src) {
            case 's': *dst++ = ' '; src++; continue;
            case 'n': *dst++ = '\n'; src++; continue;
            case 't': *dst++ = '\t'; src++; continue;
            case 'r': *dst++ = '\r'; src++; continue;
            case '\\': *dst++ = '\\'; src++; continue;
            default: *dst++ = *src++; continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = 0;
}

static int desktop_entry_load(const char* path, LunaDesktopEntry* entry) {
    memset(entry, 0, sizeof(*entry));
    entry->enabled = 1;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int in_desktop = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            in_desktop = !strcmp(p, "[Desktop Entry]");
            continue;
        }
        if (!in_desktop) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq++ = 0;
        char* key = p;
        char* value = eq;
        /* Exec has its own quoting/escaping grammar.  Preserve it verbatim;
         * the remaining string/list keys use the generic desktop-entry
         * backslash escapes. */
        if (strcmp(key, "Exec")) desktop_unescape_value(value);

        if (!strcmp(key, "Type")) entry->application = !strcmp(value, "Application");
        else if (!strcmp(key, "Name"))
            snprintf(entry->name, sizeof(entry->name), "%s", value);
        else if (!strcmp(key, "Icon"))
            snprintf(entry->icon, sizeof(entry->icon), "%s", value);
        else if (!strcmp(key, "Exec"))
            snprintf(entry->exec, sizeof(entry->exec), "%s", value);
        else if (!strcmp(key, "TryExec"))
            snprintf(entry->try_exec, sizeof(entry->try_exec), "%s", value);
        else if (!strcmp(key, "OnlyShowIn"))
            snprintf(entry->only_show_in, sizeof(entry->only_show_in), "%s", value);
        else if (!strcmp(key, "NotShowIn"))
            snprintf(entry->not_show_in, sizeof(entry->not_show_in), "%s", value);
        else if (!strcmp(key, "Hidden")) entry->hidden = desktop_bool(value, 0);
        else if (!strcmp(key, "NoDisplay")) entry->no_display = desktop_bool(value, 0);
        else if (!strcmp(key, "DBusActivatable"))
            entry->dbus_activatable = desktop_bool(value, 0);
        else if (!strcmp(key, "X-GNOME-Autostart-enabled"))
            entry->enabled = desktop_bool(value, 1);
    }
    fclose(f);
    return 1;
}

static int desktop_list_contains(const char* semicolon_list, const char* item) {
    if (!semicolon_list || !*semicolon_list || !item || !*item) return 0;
    size_t item_len = strlen(item);
    const char* p = semicolon_list;
    while (*p) {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == item_len && !strncmp(p, item, len)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int desktop_matches_current(const char* semicolon_list) {
    if (!semicolon_list || !*semicolon_list) return 0;
    const char* current = getenv("XDG_CURRENT_DESKTOP");
    if (!current || !*current) current = "Luna";
    char copy[512];
    if (snprintf(copy, sizeof(copy), "%s", current) >= (int)sizeof(copy)) return 0;
    char* save = NULL;
    for (char* desktop = strtok_r(copy, ":", &save); desktop;
         desktop = strtok_r(NULL, ":", &save))
        if (desktop_list_contains(semicolon_list, desktop)) return 1;
    return 0;
}

static int desktop_try_exec_available(const char* try_exec) {
    if (!try_exec || !*try_exec) return 1;
    if (strchr(try_exec, '/')) return path_is_absolute(try_exec) && access(try_exec, X_OK) == 0;
    return command_available(try_exec);
}

static int exec_args_push(LunaExecArgs* args, const char* value) {
    if (!value || !*value || args->argc >= XDG_EXEC_MAX_ARGS) return 0;
    snprintf(args->storage[args->argc], XDG_EXEC_ARG_SIZE, "%s", value);
    args->argv[args->argc] = args->storage[args->argc];
    args->argc++;
    args->argv[args->argc] = NULL;
    return 1;
}

static int desktop_expand_exec_token(const char* token,
                                     const LunaDesktopEntry* entry,
                                     char* out, size_t out_n) {
    size_t o = 0;
    for (size_t i = 0; token[i] && o + 1 < out_n; i++) {
        if (token[i] != '%') {
            out[o++] = token[i];
            continue;
        }
        char code = token[++i];
        if (!code) return 0;
        const char* replacement = NULL;
        switch (code) {
        case '%': replacement = "%"; break;
        case 'c': replacement = entry->name; break;
        case 'k': replacement = entry->path; break;
        case 'f': case 'F': case 'u': case 'U':
        case 'd': case 'D': case 'n': case 'N': case 'v': case 'm':
        case 'i':
            replacement = "";
            break;
        default:
            /* Unknown field codes make the Exec line invalid. */
            return 0;
        }
        size_t len = strlen(replacement);
        if (o + len >= out_n) return 0;
        memcpy(out + o, replacement, len);
        o += len;
    }
    out[o] = 0;
    return 1;
}

static int desktop_exec_parse(const LunaDesktopEntry* entry, LunaExecArgs* args) {
    memset(args, 0, sizeof(*args));
    const char* p = entry->exec;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char raw[XDG_EXEC_ARG_SIZE];
        size_t r = 0;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            if (*p == '\\' && p[1]) p++;
            if (r + 1 >= sizeof(raw)) return 0;
            raw[r++] = *p++;
        }
        if (quoted) return 0;
        raw[r] = 0;

        if (!strcmp(raw, "%i")) {
            if (entry->icon[0]) {
                if (!exec_args_push(args, "--icon") ||
                    !exec_args_push(args, entry->icon)) return 0;
            }
            continue;
        }

        char expanded[XDG_EXEC_ARG_SIZE];
        if (!desktop_expand_exec_token(raw, entry, expanded, sizeof(expanded))) return 0;
        if (expanded[0] && !exec_args_push(args, expanded)) return 0;
    }
    return args->argc > 0;
}

static const char* exec_basename(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static void xdg_autostart_launch(const LunaDesktopEntry* entry) {
    LunaExecArgs args;
    int have_direct = entry->exec[0] && desktop_exec_parse(entry, &args);
    int have_gio = command_available("gio");
    if (have_direct) {
        const char* base = exec_basename(args.argv[0]);
        if (base && !strcmp(base, "whiz-im-wayland")) g_xdg_autostart_has_im = 1;
        if (base && !strcmp(base, "luna-clipboard")) g_xdg_autostart_has_clipboard = 1;
    }

    if (!have_gio && !have_direct) {
        fprintf(stderr, "[luna-shell/xdg] cannot launch autostart entry: %s\n",
                entry->path);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        child_session_env();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            if (fd > 2) close(fd);
        }
        if (have_gio)
            execlp("gio", "gio", "launch", entry->path, (char*)NULL);
        if (have_direct) execvp(args.argv[0], args.argv);
        _exit(127);
    }
    fprintf(stderr, "[luna-shell/xdg] autostart: %s\n",
            entry->name[0] ? entry->name : entry->path);
}

static int desktop_id_seen(char seen[][NAME_MAX + 1], int count, const char* id) {
    for (int i = 0; i < count; i++)
        if (!strcmp(seen[i], id)) return 1;
    return 0;
}

static void xdg_autostart_scan_dir(const char* dir,
                                   char seen[][NAME_MAX + 1], int* seen_count) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && *seen_count < XDG_AUTOSTART_MAX_ENTRIES) {
        size_t len = strlen(de->d_name);
        if (de->d_name[0] == '.' || len <= 8 || strcmp(de->d_name + len - 8, ".desktop"))
            continue;
        if (desktop_id_seen(seen, *seen_count, de->d_name)) continue;
        snprintf(seen[*seen_count], NAME_MAX + 1, "%s", de->d_name);
        (*seen_count)++;

        char path[PATH_MAX];
        if (!path_join2(path, sizeof(path), dir, de->d_name)) continue;
        LunaDesktopEntry entry;
        if (!desktop_entry_load(path, &entry)) continue;
        if (!entry.application || entry.hidden || !entry.enabled) continue;
        if (entry.only_show_in[0] && !desktop_matches_current(entry.only_show_in)) continue;
        if (entry.not_show_in[0] && desktop_matches_current(entry.not_show_in)) continue;
        if (!desktop_try_exec_available(entry.try_exec)) continue;
        if (!entry.exec[0] && !entry.dbus_activatable) continue;
        xdg_autostart_launch(&entry);
    }
    closedir(d);
}

static void xdg_autostart_run(void) {
    const char* disabled = getenv("LUNA_NO_XDG_AUTOSTART");
    if (disabled && desktop_bool(disabled, 0)) return;
    if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY") && !g_desktop_mode) return;
    const char* forced = getenv("LUNA_XDG_AUTOSTART");
    if (!g_desktop_mode && !(forced && desktop_bool(forced, 0)) &&
        !desktop_matches_current("Luna;luna;"))
        return;

    char seen[XDG_AUTOSTART_MAX_ENTRIES][NAME_MAX + 1];
    int seen_count = 0;
    char dir[PATH_MAX];
    if (path_join2(dir, sizeof(dir), g_xdg.config_home, "autostart"))
        xdg_autostart_scan_dir(dir, seen, &seen_count);

    const char* dirs = getenv("XDG_CONFIG_DIRS");
    if (!dirs || !*dirs) dirs = "/etc/xdg";
    char list[PATH_MAX * 2];
    if (snprintf(list, sizeof(list), "%s", dirs) >= (int)sizeof(list)) return;
    char* save = NULL;
    for (char* base = strtok_r(list, ":", &save); base;
         base = strtok_r(NULL, ":", &save)) {
        if (!path_is_absolute(base)) {
            if (*base)
                fprintf(stderr, "[luna-shell/xdg] ignoring relative XDG_CONFIG_DIRS entry: %s\n",
                        base);
            continue;
        }
        if (path_join2(dir, sizeof(dir), base, "autostart"))
            xdg_autostart_scan_dir(dir, seen, &seen_count);
    }
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

    if (!luna_im_use_gim() && !g_xdg_autostart_has_im) {
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

    if (!g_xdg_autostart_has_clipboard) {
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
            if (!strncmp(key, "xdg_", 4)) return NULL; /* handled by resolve_lp_xdg */
            for (int i = 0; i < APP_COUNT; i++)
                if (!strcmp(g_apps[i].key, key)) return &g_apps[i];
        }
    }
    return NULL;
}

static int resolve_lp_xdg_slot(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!id || id[0] == '\0') continue;
        if (strncmp(id, "lp_xdg_", 7) != 0) continue;
        int slot = atoi(id + 7);
        if (slot >= 0 && slot < g_lp_xdg_count && g_lp_xdg[slot].path[0])
            return slot;
    }
    return -1;
}

static void app_set_dot(LunaApp* app, int running) {
    char dot_id[64];
    snprintf(dot_id, sizeof(dot_id), "dot_%s", app->key);
    int idx = luna_get_element_by_id(dot_id);
    if (idx != -1) luna_element_at(idx)->opacity = running ? 1.0f : 0.0f;
}

/* Settings may refer to an XDG desktop file instead of a shell command:
 *   desktop:org.gnome.Nautilus.desktop
 *   org.gnome.Nautilus.desktop
 *   /absolute/path/to/custom.desktop
 *   sakura / pcmanfm  (bare command → applications/<cmd>.desktop when present)
 * gtk-launch/GIO preserve startup notification and DBusActivatable semantics;
 * the local Exec parser is a no-dependency fallback. */
static int app_desktop_entry_resolve(const char* command,
                                     char* desktop_id, size_t id_n,
                                     char* desktop_path, size_t path_n) {
    if (!command || !*command) return 0;
    const char* value = command;
    if (!strncmp(value, "desktop:", 8)) value += 8;
    else {
        size_t len = strlen(value);
        if (len <= 8 || strcmp(value + len - 8, ".desktop")) return 0;
    }
    while (*value == ' ' || *value == '	') value++;
    size_t len = strlen(value);
    while (len && (value[len - 1] == ' ' || value[len - 1] == '	')) len--;
    if (!len || len >= PATH_MAX) return 0;

    char item[PATH_MAX];
    memcpy(item, value, len);
    item[len] = 0;
    /* A desktop-id is a single lookup key, not an arbitrary shell fragment. */
    for (size_t i = 0; item[i]; i++)
        if ((unsigned char)item[i] < 0x20 || item[i] == '/' || item[i] == '\\' ||
            item[i] == ' ' || item[i] == '	' || item[i] == ';' || item[i] == '|') {
            if (!path_is_absolute(item)) return 0;
            break;
        }

    desktop_id[0] = 0;
    desktop_path[0] = 0;
    if (path_is_absolute(item)) {
        size_t ilen = strlen(item);
        if (ilen <= 8 || strcmp(item + ilen - 8, ".desktop") || access(item, R_OK) != 0)
            return 0;
        snprintf(desktop_path, path_n, "%s", item);
        return 1;
    }

    snprintf(desktop_id, id_n, "%s", item);
    char rel[PATH_MAX];
    size_t ilen = strlen(item);
    int rn = ilen > 8 && !strcmp(item + ilen - 8, ".desktop")
        ? snprintf(rel, sizeof(rel), "applications/%s", item)
        : snprintf(rel, sizeof(rel), "applications/%s.desktop", item);
    if (rn >= 0 && (size_t)rn < sizeof(rel))
        (void)xdg_find_data_file(desktop_path, path_n, rel);
    return 1;
}

static void app_launch(LunaApp* app) {
    const char* cmd = app->cmd[0] ? app->cmd : app->default_cmd;
    char desktop_id[NAME_MAX + 1];
    char desktop_path[PATH_MAX];
    int desktop_entry = app_desktop_entry_resolve(cmd,
                                                   desktop_id, sizeof(desktop_id),
                                                   desktop_path, sizeof(desktop_path));
    int have_gtk_launch = desktop_entry && desktop_id[0] && command_available("gtk-launch");
    int have_gio = desktop_entry && desktop_path[0] && command_available("gio");
    LunaDesktopEntry parsed_entry;
    LunaExecArgs parsed_args;
    int have_direct = desktop_entry && desktop_path[0] &&
                      desktop_entry_load(desktop_path, &parsed_entry) &&
                      parsed_entry.application && !parsed_entry.hidden &&
                      parsed_entry.exec[0] && desktop_exec_parse(&parsed_entry, &parsed_args);

    if (desktop_entry && !have_gtk_launch && !have_gio && !have_direct) {
        char msg[320];
        snprintf(msg, sizeof(msg), "Cannot resolve desktop entry: %s", cmd);
        toast_show(app->name, msg, 5.0);
        fprintf(stderr, "[luna-shell/xdg] cannot launch desktop entry: %s\n", cmd);
        return;
    }

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
            /* Skins may request SSD for other clients.  Firefox combines its
             * tab strip with the titlebar, so force its supported CSD mode;
             * mismatching the negotiated frame also offsets pointer input. */
            setenv("MOZ_GTK_TITLEBAR_DECORATION", "client", 1);
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
        if (have_gtk_launch)
            execlp("gtk-launch", "gtk-launch", desktop_id, (char*)NULL);
        if (have_gio)
            execlp("gio", "gio", "launch", desktop_path, (char*)NULL);
        if (have_direct)
            execvp(parsed_args.argv[0], parsed_args.argv);
        if (!desktop_entry)
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

/* ── Weather (Open-Meteo; mirrors qberry-weather.js) ── */

static void dismiss_luna_menu(int trap_idx);
static void dismiss_cc(int trap_idx);
static void dismiss_win_menu(void);
static void dismiss_clip_menu(void);
static void dismiss_wifi_menu(void);
static void dismiss_bt_menu(void);

typedef struct LunaSliderIds LunaSliderIds;
static LunaSliderIds* g_bright_slider_ptr;
static LunaSliderIds* g_vol_slider_ptr;
static void cc_sliders_pull_from_system(LunaSliderIds* bright, LunaSliderIds* vol);
static void cc_sliders_flush(LunaSliderIds* bright, LunaSliderIds* vol);
static void settings_mark_audio_backend(const char* backend);
static void settings_mark_brightness_backend(const char* backend);
static void settings_update_sound_status(void);
static void settings_read_alsa_fields(void);

static const char* weather_icon_glyph(int code) {
    if (code == 0) return "\uf185";
    if (code == 1 || code == 2) return "\uf6c4";
    if (code == 3) return "\uf0c2";
    if (code == 45 || code == 48) return "\uf75f";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "\uf73d";
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "\uf2dc";
    if (code >= 95) return "\uf0e7";
    return "\uf2c9";
}

static const char* weather_code_text(int code) {
    switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 80: return "Rain showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "Unknown";
    }
}

static void weather_set_text_id(const char* id, const char* text) {
    int idx = luna_get_element_by_id(id);
    if (idx >= 0) luna_set_text(idx, text);
}

static void weather_set_day(const char* prefix, int i, const char* name,
                            int code, float tmax, float tmin, int show_min) {
    char id[32], buf[48];
    snprintf(id, sizeof(id), "%s%d", prefix, i);
    int root = luna_get_element_by_id(id);
    if (root < 0) return;
    for (int c = 0; c < elem_count; c++) {
        LunaElement* e = luna_element_at(c);
        if (!e || e->parent_idx != root) continue;
        if (strstr(e->class_name, "wf_name"))
            luna_set_text(c, name);
        else if (strstr(e->class_name, "wf_icon"))
            luna_set_text(c, weather_icon_glyph(code));
        else if (strstr(e->class_name, "wf_temp")) {
            if (show_min)
                snprintf(buf, sizeof(buf), "%.0f° %.0f°", tmax, tmin);
            else
                snprintf(buf, sizeof(buf), "%.0f°", tmax);
            luna_set_text(c, buf);
        }
    }
}

static void weather_apply_ui(const WeatherData* d) {
    char buf[160];

    if (!d->ok) {
        weather_set_text_id("mb_weather_temp", "—");
        weather_set_text_id("mb_weather_icon", "\uf2c9");
        set_hidden(luna_get_element_by_id("weather_hint"), 1);
        set_hidden(luna_get_element_by_id("weather_content"), 1);
        set_hidden(luna_get_element_by_id("weather_error"), 0);
        weather_set_text_id("weather_error", d->err[0] ? d->err : "Weather unavailable");
        weather_set_text_id("wg_wx_city", "Weather");
        weather_set_text_id("wg_wx_temp", "—");
        weather_set_text_id("wg_wx_desc", d->err[0] ? d->err : "Unavailable");
    } else {
        snprintf(buf, sizeof(buf), "%.0f°", d->temp);
        weather_set_text_id("mb_weather_temp", buf);
        weather_set_text_id("mb_weather_icon", weather_icon_glyph(d->code));

        set_hidden(luna_get_element_by_id("weather_hint"), 1);
        set_hidden(luna_get_element_by_id("weather_error"), 1);
        set_hidden(luna_get_element_by_id("weather_content"), 0);

        snprintf(buf, sizeof(buf), "%s%s%s", d->city,
                 d->country[0] ? ", " : "", d->country);
        weather_set_text_id("weather_city_label", buf);
        weather_set_text_id("weather_icon", weather_icon_glyph(d->code));
        snprintf(buf, sizeof(buf), "%.0f°C", d->temp);
        weather_set_text_id("weather_temp", buf);
        weather_set_text_id("weather_desc", weather_code_text(d->code));
        snprintf(buf, sizeof(buf), "Feels %.0f°C", d->feels);
        weather_set_text_id("weather_feels", buf);
        snprintf(buf, sizeof(buf), "%.0f%%", d->humidity);
        weather_set_text_id("weather_hum", buf);
        snprintf(buf, sizeof(buf), "%.0f km/h", d->wind);
        weather_set_text_id("weather_wind", buf);

        weather_set_text_id("wg_wx_city", d->city);
        snprintf(buf, sizeof(buf), "%.0f°", d->temp);
        weather_set_text_id("wg_wx_temp", buf);
        weather_set_text_id("wg_wx_icon", weather_icon_glyph(d->code));
        weather_set_text_id("wg_wx_desc", weather_code_text(d->code));

        for (int i = 0; i < LUNA_WEATHER_FORECAST_DAYS; i++) {
            char dayname[16] = "—";
            if (d->days[i].date[0] && strcmp(d->days[i].date, "-")) {
                struct tm tm = {0};
                if (sscanf(d->days[i].date, "%d-%d-%d",
                           &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
                    tm.tm_year -= 1900;
                    tm.tm_mon -= 1;
                    tm.tm_isdst = -1;
                    mktime(&tm);
                    if (i == 0) snprintf(dayname, sizeof(dayname), "Today");
                    else strftime(dayname, sizeof(dayname), "%a", &tm);
                }
            }
            weather_set_day("wf_", i, dayname, d->days[i].code,
                            d->days[i].tmax, d->days[i].tmin, 1);
            weather_set_day("wg_wf_", i, dayname, d->days[i].code,
                            d->days[i].tmax, d->days[i].tmin, 0);
        }
    }

    shell_request_repaint(1);
    shell_request_repaint(0);
    if (is_shown(g_weather_menu_idx)) shell_request_repaint(-1);
}

static void weather_request(const char* city) {
    if (!city || !*city) return;
    if (!g_weather_worker_ready) {
        WeatherData d;
        memset(&d, 0, sizeof(d));
        snprintf(d.query_city, sizeof(d.query_city), "%s", city);
        snprintf(d.err, sizeof(d.err), "Weather worker unavailable");
        g_weather = d;
        g_weather_busy = 0;
        weather_apply_ui(&g_weather);
        return;
    }

    set_hidden(luna_get_element_by_id("weather_content"), 1);
    set_hidden(luna_get_element_by_id("weather_error"), 1);
    set_hidden(luna_get_element_by_id("weather_hint"), 0);
    weather_set_text_id("weather_hint", "Fetching…");
    weather_set_text_id("mb_weather_temp", "…");
    weather_set_text_id("wg_wx_desc", "Fetching…");
    g_weather_busy = 1;
    luna_weather_request(city);
}

static void weather_tick(void) {
    if (!g_weather_worker_ready) return;
    WeatherData data;
    if (luna_weather_consume(&data)) {
        g_weather = data;
        if (data.ok && data.query_city[0] &&
            strcmp(g_settings.weather_city, data.query_city) != 0) {
            snprintf(g_settings.weather_city, sizeof(g_settings.weather_city),
                     "%s", data.query_city);
            settings_save();
        }
        weather_apply_ui(&g_weather);
    }
    g_weather_busy = luna_weather_busy();
}

static void weather_do_search(void) {
    int pi = luna_get_element_by_id("weather_city_input");
    const char* v = pi >= 0 ? luna_get_value(pi) : "";
    char city[64];
    snprintf(city, sizeof(city), "%s", v ? v : "");
    trim_line(city);
    if (!city[0]) return;
    weather_request(city);
}

static void dismiss_weather_menu(void) {
    if (is_shown(g_weather_menu_idx)) set_hidden(g_weather_menu_idx, 1);
}

static void on_weather_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_calendar_menu();
    if (is_shown(g_weather_menu_idx)) { dismiss_weather_menu(); return; }
    int pi = luna_get_element_by_id("weather_city_input");
    if (pi >= 0) luna_set_value(pi, g_settings.weather_city);
    set_hidden(g_weather_menu_idx, 0);
    position_menu_near(g_weather_menu_idx,
                       menu_anchor_from(e, g_mb_weather_idx),
                       luna_window_width - 300.0f);
    if (!g_weather.ok && !g_weather_busy)
        weather_request(g_settings.weather_city);
}

static void on_weather_go(LunaElement* e) {
    (void)e;
    weather_do_search();
}

static void on_widget_weather(LunaElement* e) {
    on_weather_menu(e);
}

static void reap_children(void) {
    /* Worker modules may own popen()/pclose() children.  Never let the global
     * waitpid(-1) loop steal those children; both guards are try-locks so the
     * render thread remains non-blocking. */
    if (pthread_mutex_trylock(&g_child_reaper_mutex) != 0) {
        g_sigchld_pending = 1;
        return;
    }
    if (!luna_wifi_reaper_try_lock()) {
        pthread_mutex_unlock(&g_child_reaper_mutex);
        g_sigchld_pending = 1;
        return;
    }
    if (!luna_ethernet_reaper_try_lock()) {
        luna_wifi_reaper_unlock();
        pthread_mutex_unlock(&g_child_reaper_mutex);
        g_sigchld_pending = 1;
        return;
    }
    if (!luna_bluetooth_reaper_try_lock()) {
        luna_ethernet_reaper_unlock();
        luna_wifi_reaper_unlock();
        pthread_mutex_unlock(&g_child_reaper_mutex);
        g_sigchld_pending = 1;
        return;
    }
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < APP_COUNT; i++) {
            if (g_apps[i].pid == pid) {
                g_apps[i].pid = 0;
                app_set_dot(&g_apps[i], 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    toast_show(g_apps[i].name, "App not installed (LUNA_APP_*)", 5.0);
                } else if (WIFSIGNALED(status)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Application stopped (signal %d)",
                             WTERMSIG(status));
                    toast_show(g_apps[i].name, msg, 5.0);
                    fprintf(stderr, "[luna-shell] %s stopped by signal %d\n",
                            g_apps[i].name, WTERMSIG(status));
                } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Application exited with status %d",
                             WEXITSTATUS(status));
                    toast_show(g_apps[i].name, msg, 5.0);
                }
            }
        }
    }
    luna_bluetooth_reaper_unlock();
    luna_ethernet_reaper_unlock();
    luna_wifi_reaper_unlock();
    pthread_mutex_unlock(&g_child_reaper_mutex);
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
    set_hidden(luna_get_element_by_id("net_tip"), 1);
}

static void dismiss_bt_menu(void) {
    if (is_shown(g_bt_menu_idx)) set_hidden(g_bt_menu_idx, 1);
}

static void dismiss_popovers(void) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
}

static void on_luna_menu(LunaElement* e) {
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    if (is_shown(g_luna_menu_idx)) { dismiss_luna_menu(g_luna_menu_idx); return; }
    set_hidden(g_luna_menu_idx, 0);
    int logo = luna_get_element_by_id("mb_logo");
    position_menu_near(g_luna_menu_idx, menu_anchor_from(e, logo), 8.0f);
}

static void on_control_center(LunaElement* e) {
    (void)e;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    if (is_shown(g_cc_idx)) { dismiss_cc(g_cc_idx); return; }
    set_hidden(g_cc_idx, 0);
    position_control_center();
    if (g_bright_slider_ptr && g_vol_slider_ptr)
        cc_sliders_pull_from_system(g_bright_slider_ptr, g_vol_slider_ptr);
}

static void on_wifi_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    if (is_shown(g_wifi_menu_idx)) { dismiss_wifi_menu(); return; }
    wifi_refresh();
    eth_refresh();
    network_update_ui();
    set_hidden(g_wifi_menu_idx, 0);
    position_menu_near(g_wifi_menu_idx,
                       menu_anchor_from(e, g_mb_wifi_idx),
                       luna_window_width - 330.0f);
}

static void on_bt_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    if (is_shown(g_bt_menu_idx)) { dismiss_bt_menu(); return; }
    /* Keep Control Center open behind the device list when launched from CC. */
    bt_refresh();
    bt_update_ui();
    luna_mark_layout_dirty();
    set_hidden(g_bt_menu_idx, 0);
    position_menu_near(g_bt_menu_idx,
                       menu_anchor_from(e, luna_get_element_by_id("cc_bt")),
                       luna_window_width - 330.0f);
}

static void on_wifi_power(LunaElement* e) {
    (void)e;
    int desired = !g_wifi_powered;
    if (!wifi_request_powered(desired)) {
        toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
        return;
    }
    g_settings.wifi_enabled = desired;
    g_wifi_powered = desired; /* optimistic UI; next worker snapshot verifies it */
    settings_save();
    network_update_ui();
}

static void on_bt_power(LunaElement* e) {
    (void)e;
    int desired = !g_bt_powered;
    if (!bt_request_powered(desired)) {
        toast_show("Bluetooth", "Bluetooth worker is unavailable", 2.5);
        return;
    }
    g_settings.bluetooth_enabled = desired;
    g_bt_powered = desired;
    settings_save();
    bt_update_ui();
    luna_mark_layout_dirty();
}

static int wifi_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (str_has_prefix(id, "wifi_") && isdigit((unsigned char)id[5])) return atoi(id + 5);
    }
    return -1;
}

static void on_wifi_network(LunaElement* e) {
    int i = wifi_row_number(e);
    if (i < 0 || i >= g_wifi_count) return;
    WifiNetwork* n = &g_wifi_networks[i];

    if (luna_last_click_button() == LUNA_MOUSE_BUTTON_RIGHT) {
        net_detail_open(0, i);
        return;
    }

    if (n->connected) {
        if (!wifi_request_disconnect(n->id)) {
            toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
            return;
        }
        toast_show("Wi-Fi", "Disconnecting...", 2.0);
        dismiss_wifi_menu();
        return;
    }

    /* Favorite/saved ConnMan services already have credentials, and open
     * networks need none.  In both cases an SSID click can connect directly.
     * Only a new secured service opens the passphrase editor. */
    if (n->saved || !n->secure) {
        if (!wifi_request_connect(n->id, "")) {
            toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
            return;
        }
        toast_show("Wi-Fi", "Connecting...", 2.0);
        dismiss_wifi_menu();
        return;
    }

    g_wifi_selected = i;
    int label = luna_get_element_by_id("wifi_selected");
    if (label >= 0) {
        char text[150];
        snprintf(text, sizeof(text), "Connect to %s", n->name);
        luna_set_text(label, text);
    }
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

    if (n->secure && (!password || !*password)) {
        toast_show("Wi-Fi", "Enter the network passphrase", 2.5);
        return;
    }
    if (!wifi_request_connect(n->id, password ? password : "")) {
        toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
        return;
    }

    if (pi >= 0) luna_set_value(pi, "");
    toast_show("Wi-Fi", "Connecting...", 2.0);
    dismiss_wifi_menu();
}

static void on_wifi_scan(LunaElement* e) {
    (void)e;
    if (!luna_wifi_request_scan()) {
        toast_show("Wi-Fi", "Wi-Fi worker is unavailable", 2.5);
        return;
    }
    g_wifi_busy = 1;
    g_last_wifi_request = g_now;
    network_update_ui();
    toast_show("Wi-Fi", "Scanning for networks...", 2.0);
}

static int eth_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (str_has_prefix(id, "eth_") && isdigit((unsigned char)id[4])) return atoi(id + 4);
    }
    return -1;
}

static void on_eth_link(LunaElement* e) {
    int i = eth_row_number(e);
    if (i < 0 || i >= g_eth_count) return;
    EthernetLink* n = &g_eth_links[i];
    if (luna_last_click_button() == LUNA_MOUSE_BUTTON_RIGHT) {
        net_detail_open(1, i);
        return;
    }
    if (n->connected) {
        if (!eth_request_disconnect(n->id)) {
            toast_show("Ethernet", "Ethernet worker is unavailable", 2.5);
            return;
        }
        toast_show("Ethernet", "Disconnecting...", 2.0);
        return;
    }
    if (!n->available && strncmp(n->id, "iface:", 6) == 0) {
        toast_show("Ethernet", "Cable is unplugged", 2.5);
        return;
    }
    if (!eth_request_connect(n->id)) {
        toast_show("Ethernet", "Ethernet worker is unavailable", 2.5);
        return;
    }
    toast_show("Ethernet", "Connecting...", 2.0);
}

static int bt_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (str_has_prefix(id, "bt_") && isdigit((unsigned char)id[3])) return atoi(id + 3);
    }
    return -1;
}

static void on_bt_device(LunaElement* e) {
    int i = bt_row_number(e);
    if (i < 0 || i >= g_bt_count) return;
    BluetoothDevice* d = &g_bt_devices[i];
    if (d->connected) {
        if (!bt_request_disconnect(d->id)) {
            toast_show("Bluetooth", "Bluetooth worker is unavailable", 2.5);
            return;
        }
        toast_show("Bluetooth", "Disconnecting...", 2.0);
        return;
    }
    if (!bt_request_connect(d->id)) {
        toast_show("Bluetooth", "Bluetooth worker is unavailable", 2.5);
        return;
    }
    toast_show("Bluetooth", "Connecting...", 2.0);
}

static void on_bt_scan(LunaElement* e) {
    (void)e;
    if (!g_bt_powered) {
        toast_show("Bluetooth", "Turn on Bluetooth first", 2.5);
        return;
    }
    if (!luna_bluetooth_request_scan()) {
        toast_show("Bluetooth", "Bluetooth worker is unavailable", 2.5);
        return;
    }
    g_bt_busy = 1;
    g_last_bt_request = g_now;
    bt_update_ui();
    luna_mark_layout_dirty();
    toast_show("Bluetooth", "Scanning for devices...", 2.5);
}

static void launchpad_close(void) {
    if (!is_shown(g_launchpad_idx)) return;
    set_hidden(g_launchpad_idx, 1);
}

static int lp_xdg_name_cmp(const void* a, const void* b) {
    const LunaLpXdgApp* aa = (const LunaLpXdgApp*)a;
    const LunaLpXdgApp* bb = (const LunaLpXdgApp*)b;
    return strcasecmp(aa->name, bb->name);
}

static int lp_xdg_is_builtin_dup(const LunaDesktopEntry* entry) {
    if (!entry) return 0;
    for (int i = 0; i < APP_COUNT; i++) {
        if (entry->name[0] && !strcasecmp(entry->name, g_apps[i].name))
            return 1;
        const char* cmd = g_apps[i].cmd[0] ? g_apps[i].cmd : g_apps[i].default_cmd;
        if (!cmd || !*cmd) continue;
        /* Compare the first Exec token basename to the configured command. */
        const char* p = entry->exec;
        while (*p == ' ' || *p == '\t') p++;
        char tok[128];
        size_t n = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && n + 1 < sizeof(tok)) tok[n++] = *p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof(tok)) tok[n++] = *p++;
        }
        tok[n] = 0;
        if (!tok[0]) continue;
        const char* base = strrchr(tok, '/');
        base = base ? base + 1 : tok;
        if (!strcmp(base, cmd)) return 1;
    }
    return 0;
}

static void lp_xdg_try_add(const char* path, const char* desktop_id,
                           char seen[][NAME_MAX + 1], int* seen_count) {
    if (!path || !desktop_id || !*desktop_id) return;
    if (*seen_count >= MAX_LP_XDG * 4) return;
    if (desktop_id_seen(seen, *seen_count, desktop_id)) return;
    snprintf(seen[*seen_count], NAME_MAX + 1, "%s", desktop_id);
    (*seen_count)++;

    LunaDesktopEntry entry;
    if (!desktop_entry_load(path, &entry)) return;
    if (!entry.application || entry.hidden || entry.no_display || !entry.enabled)
        return;
    if (!entry.name[0] || (!entry.exec[0] && !entry.dbus_activatable)) return;
    if (entry.only_show_in[0] && !desktop_matches_current(entry.only_show_in)) return;
    if (entry.not_show_in[0] && desktop_matches_current(entry.not_show_in)) return;
    if (!desktop_try_exec_available(entry.try_exec)) return;
    if (lp_xdg_is_builtin_dup(&entry)) return;
    if (g_lp_xdg_count >= MAX_LP_XDG) return;

    LunaLpXdgApp* slot = &g_lp_xdg[g_lp_xdg_count++];
    snprintf(slot->id, sizeof(slot->id), "%s", desktop_id);
    snprintf(slot->name, sizeof(slot->name), "%s", entry.name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
}

static void lp_xdg_scan_dir(const char* dir,
                            char seen[][NAME_MAX + 1], int* seen_count) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (de->d_name[0] == '.' || len <= 8 ||
            strcmp(de->d_name + len - 8, ".desktop"))
            continue;
        char path[PATH_MAX];
        if (!path_join2(path, sizeof(path), dir, de->d_name)) continue;
        lp_xdg_try_add(path, de->d_name, seen, seen_count);
    }
    closedir(d);
}

static void launchpad_populate_xdg(void) {
    for (int i = 0; i < MAX_LP_XDG; i++) {
        g_lp_xdg_idx[i] = -1;
        g_lp_xdg_label_idx[i] = -1;
        g_lp_xdg[i].id[0] = 0;
        g_lp_xdg[i].name[0] = 0;
        g_lp_xdg[i].path[0] = 0;
    }
    g_lp_xdg_count = 0;

    char seen[MAX_LP_XDG * 4][NAME_MAX + 1];
    int seen_count = 0;
    char dir[PATH_MAX];

    /* User applications first so they win the desktop-id race. */
    if (path_join2(dir, sizeof(dir), g_xdg.data_home, "applications"))
        lp_xdg_scan_dir(dir, seen, &seen_count);

    const char* dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char list[PATH_MAX * 2];
    if (snprintf(list, sizeof(list), "%s", dirs) < (int)sizeof(list)) {
        char* save = NULL;
        for (char* base = strtok_r(list, ":", &save); base;
             base = strtok_r(NULL, ":", &save)) {
            if (!path_is_absolute(base)) continue;
            if (path_join2(dir, sizeof(dir), base, "applications"))
                lp_xdg_scan_dir(dir, seen, &seen_count);
        }
    }

    if (g_lp_xdg_count > 1)
        qsort(g_lp_xdg, (size_t)g_lp_xdg_count, sizeof(g_lp_xdg[0]), lp_xdg_name_cmp);

    for (int i = 0; i < MAX_LP_XDG; i++) {
        char id[32];
        snprintf(id, sizeof(id), "lp_xdg_%02d", i);
        g_lp_xdg_idx[i] = luna_get_element_by_id(id);
        if (g_lp_xdg_idx[i] < 0) continue;

        /* Prefer a dedicated label child when present. */
        g_lp_xdg_label_idx[i] = -1;
        for (int c = 0; c < luna_element_count(); c++) {
            LunaElement* ch = luna_element_at(c);
            if (ch->parent_idx != g_lp_xdg_idx[i]) continue;
            if (strstr(ch->class_name, "lp_label")) {
                g_lp_xdg_label_idx[i] = c;
                break;
            }
        }

        if (i < g_lp_xdg_count) {
            set_hidden(g_lp_xdg_idx[i], 0);
            if (g_lp_xdg_label_idx[i] >= 0)
                luna_set_text(g_lp_xdg_label_idx[i], g_lp_xdg[i].name);
            else
                luna_set_text(g_lp_xdg_idx[i], g_lp_xdg[i].name);
            wire_subtree(g_lp_xdg_idx[i], on_launch_app);
        } else {
            set_hidden(g_lp_xdg_idx[i], 1);
        }
    }
    g_lp_xdg_ready = 1;
    fprintf(stderr, "[luna-shell] launchpad: %d XDG applications\n", g_lp_xdg_count);
}

static void on_launchpad_open(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    set_hidden(g_launchpad_idx, 0);
}

static void on_launchpad_close(LunaElement* e) {
    (void)e;
    launchpad_close();
}

static void on_launch_app(LunaElement* e) {
    LunaApp* app = resolve_app(e);
    int xdg = resolve_lp_xdg_slot(e);
    launchpad_close();
    if (app) {
        app_launch(app);
        return;
    }
    if (xdg >= 0) {
        LunaApp tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.key = "xdg";
        tmp.name = g_lp_xdg[xdg].name;
        snprintf(tmp.cmd, sizeof(tmp.cmd), "%s", g_lp_xdg[xdg].path);
        app_launch(&tmp);
    }
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
        snprintf(cmd, sizeof(cmd), "minimize %" PRIu64, w->id);
    else
        snprintf(cmd, sizeof(cmd), "activate %" PRIu64, w->id);
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
    if (drag_moved) {
        /* Slider thumbs use drag_mode=2; flush the final level on release so a
         * fast fling is not lost to the apply throttle. */
        cc_sliders_flush(g_bright_slider_ptr, g_vol_slider_ptr);
        return;
    }
    if (hit < 0) return;
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
    if (is_shown(g_bt_menu_idx) &&
        !hit_inside(hit, g_bt_menu_idx) &&
        !hit_inside(hit, luna_get_element_by_id("cc_bt")) &&
        !hit_inside(hit, luna_get_element_by_id("cc_bt_open")))
        dismiss_bt_menu();
    if (is_shown(g_weather_menu_idx) &&
        !hit_inside(hit, g_weather_menu_idx) &&
        !hit_inside(hit, g_mb_weather_idx) &&
        !hit_inside(hit, luna_get_element_by_id("widget_weather")))
        dismiss_weather_menu();
    if (is_shown(g_calendar_menu_idx) &&
        !hit_inside(hit, g_calendar_menu_idx) &&
        !hit_inside(hit, g_mb_clock_idx) &&
        !hit_inside(hit, luna_get_element_by_id("wg_date")) &&
        !hit_inside(hit, luna_get_element_by_id("widget_clock")))
        dismiss_calendar_menu();
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
    luna_update_classes(idx, "ocean forest sunset",
                        strcmp(theme, "night") != 0 ? theme : NULL);
    luna_update_classes(idx, "wallpaper_static",
                        g_settings.wallpaper_animation ? NULL : "wallpaper_static");
}

static void settings_mark_wallpaper(const char* theme) {
    const char* ids[] = { "wp_night", "wp_ocean", "wp_forest", "wp_sunset" };
    for (int i = 0; i < 4; i++) {
        int ti = luna_get_element_by_id(ids[i]);
        if (ti < 0) continue;
        luna_update_classes(ti, "selected",
            !strcmp(ids[i] + 3, theme) ? "selected" : NULL);
    }
    luna_mark_layout_dirty();
}

static void settings_populate_skins(void) {
    int selected = skin_find(g_settings.skin);
    for (int i = 0; i < MAX_SKINS; i++) {
        char id[32];
        snprintf(id, sizeof(id), "skin_%d", i);
        int card = luna_get_element_by_id(id);
        if (card < 0) continue;
        set_hidden(card, i >= g_skin_count);
        if (i >= g_skin_count) continue;
        snprintf(id, sizeof(id), "skin_%d_name", i);
        int name = luna_get_element_by_id(id);
        if (name >= 0) luna_set_text(name, g_skins[i].name);
        snprintf(id, sizeof(id), "skin_%d_desc", i);
        int desc = luna_get_element_by_id(id);
        if (desc >= 0) luna_set_text(desc, g_skins[i].description);
        luna_update_classes(card, "selected",
                            i == selected ? "selected" : NULL);
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
        luna_update_classes(ti, "selected",
                            !strcmp(names[i], sel) ? "selected" : NULL);
    }
    luna_mark_layout_dirty();
}

static void settings_mark_toggle(const char* id, int enabled) {
    int idx = luna_get_element_by_id(id);
    if (idx < 0) return;
    luna_update_classes(idx, "on", enabled ? "on" : NULL);
    if (!strcmp(id, "wm_wallpaper_motion"))
        luna_set_text(idx, enabled ? "Wallpaper motion · On" : "Wallpaper motion · Off");
}

static void settings_mark_locale(const char* locale_name) {
    const char* ids[] = { "locale_ja", "locale_en", "locale_c" };
    const char* values[] = { "ja_JP.UTF-8", "en_US.UTF-8", "C.UTF-8" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx < 0) continue;
        luna_update_classes(idx, "selected",
                            !strcmp(locale_name, values[i]) ? "selected" : NULL);
    }
    luna_mark_layout_dirty();
}

static void settings_mark_gap(void) {
    const int gaps[] = { 0, 8, 16 };
    for (int i = 0; i < 3; i++) {
        char id[24];
        snprintf(id, sizeof(id), "wm_gap_%d", gaps[i]);
        int idx = luna_get_element_by_id(id);
        if (idx < 0) continue;
        luna_update_classes(idx, "selected",
                            g_settings.window_gap == gaps[i] ? "selected" : NULL);
    }
}

static void settings_mark_audio_backend(const char* backend) {
    static const char* ids[] = {
        "audio_auto", "audio_wpctl", "audio_pactl", "audio_alsa"
    };
    static const char* vals[] = { "auto", "wpctl", "pactl", "alsa" };
    for (int i = 0; i < 4; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx >= 0)
            luna_update_classes(idx, "selected",
                backend && !strcmp(backend, vals[i]) ? "selected" : NULL);
    }
}

static void settings_mark_brightness_backend(const char* backend) {
    static const char* ids[] = {
        "bright_auto", "bright_sysfs", "bright_brightnessctl", "bright_xrandr"
    };
    static const char* vals[] = { "auto", "sysfs", "brightnessctl", "xrandr" };
    for (int i = 0; i < 4; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx >= 0)
            luna_update_classes(idx, "selected",
                backend && !strcmp(backend, vals[i]) ? "selected" : NULL);
    }
}

static void settings_mark_choice_cards(const char* const* ids, const char* const* vals,
                                      int n, const char* cur) {
    for (int i = 0; i < n; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx >= 0)
            luna_update_classes(idx, "selected",
                cur && !strcmp(cur, vals[i]) ? "selected" : NULL);
    }
}

static void settings_mark_display_scale(void) {
    static const char* gdk_ids[] = { "gdk_scale_1", "gdk_scale_2" };
    static const char* gdk_vals[] = { "1", "2" };
    static const char* dpi_ids[] = {
        "gdk_dpi_05", "gdk_dpi_075", "gdk_dpi_1", "gdk_dpi_125",
        "gdk_dpi_15", "gdk_dpi_175", "gdk_dpi_2"
    };
    static const char* dpi_vals[] = { "0.5", "0.75", "1", "1.25", "1.5", "1.75", "2" };
    static const char* qt_ids[] = {
        "qt_scale_05", "qt_scale_075", "qt_scale_1", "qt_scale_125",
        "qt_scale_15", "qt_scale_175", "qt_scale_2"
    };
    static const char* qt_vals[] = { "0.5", "0.75", "1", "1.25", "1.5", "1.75", "2" };
    static const char* cur_ids[] = { "xcursor_24", "xcursor_32", "xcursor_48" };
    static const char* cur_vals[] = { "24", "32", "48" };
    settings_mark_choice_cards(gdk_ids, gdk_vals, 2, g_settings.gdk_scale);
    settings_mark_choice_cards(dpi_ids, dpi_vals, 7, g_settings.gdk_dpi_scale);
    settings_mark_choice_cards(qt_ids, qt_vals, 7, g_settings.qt_scale_factor);
    settings_mark_choice_cards(cur_ids, cur_vals, 3, g_settings.xcursor_size);
}

static void settings_update_sound_status(void) {
    int a = luna_get_element_by_id("audio_backend_status");
    if (a >= 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Tools: wpctl %s · pactl %s · amixer %s · alsamixer %s",
                 command_available("wpctl") ? "ok" : "—",
                 command_available("pactl") ? "ok" : "—",
                 command_available("amixer") ? "ok" : "—",
                 command_available("alsamixer") ? "ok" : "—");
        luna_set_text(a, msg);
    }
    int b = luna_get_element_by_id("bright_backend_status");
    if (b >= 0) {
        int has_sysfs = 0;
        DIR* d = opendir("/sys/class/backlight");
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] != '.') { has_sysfs = 1; break; }
            }
            closedir(d);
        }
        char msg[192];
        snprintf(msg, sizeof(msg), "Tools: backlight %s · brightnessctl %s · xrandr %s",
                 has_sysfs ? "ok" : "—",
                 command_available("brightnessctl") ? "ok" : "—",
                 command_available("xrandr") ? "ok" : "—");
        luna_set_text(b, msg);
    }
}

/* Apply the persisted Dock membership after the layout has been bound and
 * again immediately when the user changes it in Settings.  Launchpad entries
 * remain available, so removing an app from the Dock never makes it
 * inaccessible. */
static void apply_dock_app_settings(void) {
    int visible = 0;
    int dock_idx = luna_get_element_by_id("dock");
    for (int i = 0; i < APP_COUNT; i++) {
        char id[64];
        snprintf(id, sizeof(id), "dock_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) {
            set_hidden(idx, !g_apps[i].dock_visible);
            if (g_apps[i].dock_visible) visible++;
        }
    }
    /* Keep the floating bar centred and remove the empty space left by hidden
     * launchers.  The two permanent items (Launchpad and Trash) and separator
     * account for the fixed 158 px; each visible app contributes 64 px. */
    if (dock_idx >= 0) {
        char klass[32];
        for (int i = 0; i <= 6; i++) {
            snprintf(klass, sizeof(klass), "dock_apps_%d", i);
            luna_update_classes(dock_idx, klass, NULL);
        }
        if (visible > 6) visible = 6;
        snprintf(klass, sizeof(klass), "dock_apps_%d", visible);
        luna_update_classes(dock_idx, NULL, klass);
    }
    luna_mark_layout_dirty();
    shell_request_repaint(2);
}

static void apply_wm_settings(void) {
    char cmd[80];
    int ok = 1;
    ok &= apply_numlock_setting();
    snprintf(cmd, sizeof(cmd), "wm_config gap %d", g_settings.window_gap);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config edge_snap %d", g_settings.edge_snap);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config top_edge_maximize %d", g_settings.top_edge_maximize);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_double_click %d", g_settings.titlebar_double_click);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config super_shortcuts %d", g_settings.super_shortcuts);
    ok &= shell_send_cmd(cmd);
    /* Skin titlebar style/colors/prefer_ssd win over the Settings toggle when
     * the active skin declares them. */
    ok &= skin_apply_wm_decoration(skin_find(g_settings.skin));

    /* Dock motion is intentionally CSS-only.  The persisted setting merely
     * exposes a class to the stylesheet; no per-frame icon interpolation is
     * performed by the shell. */
    {
        int dock = luna_get_element_by_id("dock");
        if (dock >= 0)
            luna_update_classes(dock, "dock_animated",
                                g_settings.dock_magnification ? "dock_animated" : NULL);
    }

    if (ok) {
        if (g_wm_settings_pending)
            fprintf(stderr, "[luna-shell] compositor WM settings applied after retry\n");
        g_wm_settings_pending = 0;
        g_wm_settings_retry_count = 0;
        g_wm_settings_retry_at = 0.0;
    } else {
        if (!g_wm_settings_pending)
            fprintf(stderr, "[luna-shell] compositor IPC not ready; WM settings will be retried\n");
        g_wm_settings_pending = 1;
        g_wm_settings_retry_count++;
        /* Retry quickly during startup, then back off to one attempt/second. */
        g_wm_settings_retry_at = g_now +
            (g_wm_settings_retry_count < 8 ? 0.25 : 1.0);
    }
}

static void wm_settings_retry_tick(void) {
    if (g_wm_settings_pending && g_now >= g_wm_settings_retry_at)
        apply_wm_settings();
}

static void settings_populate_ui(void) {
    /* Fill app command inputs */
    for (int i = 0; i < APP_COUNT; i++) {
        char input_id[64];
        snprintf(input_id, sizeof(input_id), "pref_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(input_id);
        if (idx >= 0) luna_set_value(idx, g_apps[i].cmd);

        snprintf(input_id, sizeof(input_id), "dock_pref_%s", g_apps[i].key);
        if (luna_get_element_by_id(input_id) >= 0)
            settings_mark_toggle(input_id, g_apps[i].dock_visible);
    }
    /* Hostname */
    int h = luna_get_element_by_id("pref_hostname");
    if (h >= 0) luna_set_value(h, g_settings.hostname);
    /* Wallpaper selection markers */
    settings_populate_skins();
    settings_mark_wallpaper(g_settings.wallpaper);
    settings_mark_cursor(g_settings.cursor_theme);
    settings_mark_kb(g_settings.kb_layout);
    settings_mark_locale(g_settings.ui_language);
    settings_mark_toggle("sys_numlock", g_settings.numlock_on);
    settings_mark_toggle("wm_snap", g_settings.edge_snap);
    settings_mark_toggle("wm_top_maximize", g_settings.top_edge_maximize);
    settings_mark_toggle("wm_double_click", g_settings.titlebar_double_click);
    settings_mark_toggle("wm_classic_titlebar", g_settings.classic_titlebar);
    settings_mark_toggle("wm_shortcuts", g_settings.super_shortcuts);
    settings_mark_toggle("wm_dock_mag", g_settings.dock_magnification);
    settings_mark_toggle("wm_wallpaper_motion", g_settings.wallpaper_animation);
    settings_mark_toggle("wm_restore", g_settings.session_restore);
    settings_mark_gap();
    settings_mark_audio_backend(g_settings.audio_backend);
    settings_mark_brightness_backend(g_settings.brightness_backend);
    settings_mark_display_scale();
    {
        int c = luna_get_element_by_id("pref_alsa_card");
        if (c >= 0) luna_set_value(c, g_settings.alsa_card);
        int k = luna_get_element_by_id("pref_alsa_control");
        if (k >= 0) luna_set_value(k, g_settings.alsa_control);
    }
    settings_update_sound_status();
    /* Show apps tab by default */
    set_hidden(g_settings_panel_apps, 0);
    set_hidden(g_settings_panel_disp, 1);
    set_hidden(g_settings_panel_lang, 1);
    set_hidden(g_settings_panel_kb, 1);
    set_hidden(g_settings_panel_sound, 1);
    set_hidden(g_settings_panel_wm, 1);
    if (g_stab_apps_idx  >= 0) luna_update_classes(g_stab_apps_idx,  "active", "active");
    if (g_stab_disp_idx  >= 0) luna_update_classes(g_stab_disp_idx,  "active", NULL);
    if (g_stab_lang_idx  >= 0) luna_update_classes(g_stab_lang_idx,  "active", NULL);
    if (g_stab_kb_idx    >= 0) luna_update_classes(g_stab_kb_idx,    "active", NULL);
    if (g_stab_sound_idx >= 0) luna_update_classes(g_stab_sound_idx, "active", NULL);
    if (g_stab_wm_idx    >= 0) luna_update_classes(g_stab_wm_idx,    "active", NULL);
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
    settings_read_alsa_fields();
    settings_save();
    apply_wallpaper(g_settings.wallpaper);
    cursor_theme_reload(g_settings.cursor_theme);
    apply_keyboard_layout(g_settings.kb_layout);
    apply_ui_language(g_settings.ui_language);
    apply_toolkit_session_env();
    apply_wm_settings();
    apply_dock_app_settings();
    g_cursor_reload_pending = 1;
    set_hidden(g_settings_idx, 1);
    toast_show("Settings", "Settings saved successfully.", 3.0);
}

static void on_dock_pref_toggle(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "dock_pref_")) { id = cand; break; }
    }
    if (!id) return;
    const char* key = id + strlen("dock_pref_");
    for (int i = 0; i < APP_COUNT; i++) {
        if (strcmp(g_apps[i].key, key)) continue;
        g_apps[i].dock_visible = !g_apps[i].dock_visible;
        settings_mark_toggle(id, g_apps[i].dock_visible);
        apply_dock_app_settings();
        settings_save();
        return;
    }
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
    int is_lang = !strcmp(id, "stab_language");
    int is_kb = !strcmp(id, "stab_keyboard");
    int is_sound = !strcmp(id, "stab_sound");
    int is_wm = !strcmp(id, "stab_wm");

    if (g_stab_apps_idx >= 0)
        luna_update_classes(g_stab_apps_idx, "active", is_apps ? "active" : NULL);
    if (g_stab_disp_idx >= 0)
        luna_update_classes(g_stab_disp_idx, "active", is_disp ? "active" : NULL);
    if (g_stab_lang_idx >= 0)
        luna_update_classes(g_stab_lang_idx, "active", is_lang ? "active" : NULL);
    if (g_stab_kb_idx >= 0)
        luna_update_classes(g_stab_kb_idx, "active", is_kb ? "active" : NULL);
    if (g_stab_sound_idx >= 0)
        luna_update_classes(g_stab_sound_idx, "active", is_sound ? "active" : NULL);
    if (g_stab_wm_idx >= 0)
        luna_update_classes(g_stab_wm_idx, "active", is_wm ? "active" : NULL);
    set_hidden(g_settings_panel_apps, !is_apps);
    set_hidden(g_settings_panel_disp, !is_disp);
    set_hidden(g_settings_panel_lang, !is_lang);
    set_hidden(g_settings_panel_kb, !is_kb);
    set_hidden(g_settings_panel_sound, !is_sound);
    set_hidden(g_settings_panel_wm, !is_wm);
    if (is_sound) settings_update_sound_status();
}

static void on_locale_select(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "locale_")) { id = cand; break; }
    }
    if (!id) return;
    const char* locale_name = NULL;
    if (!strcmp(id, "locale_ja")) locale_name = "ja_JP.UTF-8";
    else if (!strcmp(id, "locale_en")) locale_name = "en_US.UTF-8";
    else if (!strcmp(id, "locale_c")) locale_name = "C.UTF-8";
    if (!locale_name) return;
    snprintf(g_settings.ui_language, sizeof(g_settings.ui_language), "%s", locale_name);
    settings_mark_locale(locale_name);
    apply_ui_language(locale_name);
    settings_save();
    toast_show("Language & Region", "Language updated for newly launched applications.", 3.0);
}

static void settings_read_alsa_fields(void) {
    int ac = luna_get_element_by_id("pref_alsa_card");
    if (ac >= 0) {
        const char* v = luna_get_value(ac);
        if (v && *v) {
            int ok = 1;
            if (!strcmp(v, "default")) ok = 1;
            else if (!strncmp(v, "hw:", 3)) {
                for (const char* p = v + 3; *p; p++)
                    if (!isdigit((unsigned char)*p) && *p != ',') { ok = 0; break; }
            } else {
                for (const char* p = v; *p; p++)
                    if (!isdigit((unsigned char)*p)) { ok = 0; break; }
            }
            if (ok) {
                snprintf(g_settings.alsa_card, sizeof(g_settings.alsa_card), "%.31s", v);
            }
        }
    }
    int ak = luna_get_element_by_id("pref_alsa_control");
    if (ak >= 0) {
        const char* v = luna_get_value(ak);
        if (v && *v && strlen(v) < sizeof(g_settings.alsa_control)) {
            int ok = 1;
            for (const char* p = v; *p; p++) {
                if (!(isalnum((unsigned char)*p) || *p == ' ' || *p == '_' ||
                      *p == '-' || *p == '.' || *p == '+' || *p == '/')) {
                    ok = 0; break;
                }
            }
            if (ok) {
                snprintf(g_settings.alsa_control, sizeof(g_settings.alsa_control),
                         "%.63s", v);
            }
        }
    }
}

static void on_audio_backend_select(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "audio_")) { id = cand; break; }
    }
    if (!id) return;
    const char* backend = NULL;
    if (!strcmp(id, "audio_auto")) backend = "auto";
    else if (!strcmp(id, "audio_wpctl")) backend = "wpctl";
    else if (!strcmp(id, "audio_pactl")) backend = "pactl";
    else if (!strcmp(id, "audio_alsa")) backend = "alsa";
    if (!backend) return;
    settings_read_alsa_fields();
    snprintf(g_settings.audio_backend, sizeof(g_settings.audio_backend), "%s", backend);
    settings_mark_audio_backend(backend);
    settings_save();
    settings_update_sound_status();
    toast_show("Sound",
               !strcmp(backend, "alsa") ? "Volume slider uses ALSA (amixer)" :
               !strcmp(backend, "wpctl") ? "Volume slider uses PipeWire (wpctl)" :
               !strcmp(backend, "pactl") ? "Volume slider uses PulseAudio (pactl)" :
               "Volume slider picks the first available backend",
               2.5);
}

static void on_brightness_backend_select(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "bright_")) { id = cand; break; }
    }
    if (!id) return;
    const char* backend = NULL;
    if (!strcmp(id, "bright_auto")) backend = "auto";
    else if (!strcmp(id, "bright_sysfs")) backend = "sysfs";
    else if (!strcmp(id, "bright_brightnessctl")) backend = "brightnessctl";
    else if (!strcmp(id, "bright_xrandr")) backend = "xrandr";
    if (!backend) return;
    snprintf(g_settings.brightness_backend, sizeof(g_settings.brightness_backend), "%s", backend);
    settings_mark_brightness_backend(backend);
    settings_save();
    settings_update_sound_status();
    toast_show("Display", "Brightness backend updated", 2.0);
}

static int settings_pick_scale_value(LunaElement* e, const char* prefix,
                                     const char* const* ids, const char* const* vals,
                                     int n, char* out, size_t out_n) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, prefix)) { id = cand; break; }
    }
    if (!id) return 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(id, ids[i])) continue;
        snprintf(out, out_n, "%s", vals[i]);
        return 1;
    }
    return 0;
}

static void on_display_scale_select(LunaElement* e) {
    static const char* gdk_ids[] = { "gdk_scale_1", "gdk_scale_2" };
    static const char* gdk_vals[] = { "1", "2" };
    static const char* dpi_ids[] = {
        "gdk_dpi_05", "gdk_dpi_075", "gdk_dpi_1", "gdk_dpi_125",
        "gdk_dpi_15", "gdk_dpi_175", "gdk_dpi_2"
    };
    static const char* dpi_vals[] = { "0.5", "0.75", "1", "1.25", "1.5", "1.75", "2" };
    static const char* qt_ids[] = {
        "qt_scale_05", "qt_scale_075", "qt_scale_1", "qt_scale_125",
        "qt_scale_15", "qt_scale_175", "qt_scale_2"
    };
    static const char* qt_vals[] = { "0.5", "0.75", "1", "1.25", "1.5", "1.75", "2" };
    static const char* cur_ids[] = { "xcursor_24", "xcursor_32", "xcursor_48" };
    static const char* cur_vals[] = { "24", "32", "48" };
    char buf[8];
    const char* toast = NULL;
    if (settings_pick_scale_value(e, "gdk_scale_", gdk_ids, gdk_vals, 2,
                                  buf, sizeof(buf))) {
        snprintf(g_settings.gdk_scale, sizeof(g_settings.gdk_scale), "%s", buf);
        toast = "GDK_SCALE updated for new apps";
    } else if (settings_pick_scale_value(e, "gdk_dpi_", dpi_ids, dpi_vals, 7,
                                         buf, sizeof(buf))) {
        snprintf(g_settings.gdk_dpi_scale, sizeof(g_settings.gdk_dpi_scale), "%s", buf);
        toast = "GDK_DPI_SCALE updated for new apps";
    } else if (settings_pick_scale_value(e, "qt_scale_", qt_ids, qt_vals, 7,
                                         buf, sizeof(buf))) {
        snprintf(g_settings.qt_scale_factor, sizeof(g_settings.qt_scale_factor), "%s", buf);
        toast = "QT_SCALE_FACTOR updated for new apps";
    } else if (settings_pick_scale_value(e, "xcursor_", cur_ids, cur_vals, 3,
                                         buf, sizeof(buf))) {
        snprintf(g_settings.xcursor_size, sizeof(g_settings.xcursor_size), "%s", buf);
        toast = "XCURSOR_SIZE updated for new apps";
    } else {
        return;
    }
    settings_mark_display_scale();
    apply_toolkit_session_env();
    settings_save();
    toast_show("Display scale", toast, 2.5);
}

static void on_open_alsamixer(LunaElement* e) {
    (void)e;
    if (!command_available("alsamixer")) {
        toast_show("ALSA", "alsamixer is not installed", 2.5);
        return;
    }
    settings_read_alsa_fields();
    /* Prefer the configured terminal app; fall back to common emulators. */
    const char* term = NULL;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!strcmp(g_apps[i].key, "terminal") && g_apps[i].cmd[0]) {
            term = g_apps[i].cmd;
            break;
        }
    }
    char cmd[512];
    const char* card = g_settings.alsa_card;
    char mixer_args[96] = {0};
    if (card[0] && strcmp(card, "default") != 0) {
        if (!strncmp(card, "hw:", 3))
            snprintf(mixer_args, sizeof(mixer_args), " -D %s", card);
        else
            snprintf(mixer_args, sizeof(mixer_args), " -c %s", card);
    }
    if (term && *term)
        snprintf(cmd, sizeof(cmd), "%s -e \"alsamixer%s\"", term, mixer_args);
    else if (command_available("sakura"))
        snprintf(cmd, sizeof(cmd), "sakura -e \"alsamixer%s\"", mixer_args);
    else if (command_available("xterm"))
        snprintf(cmd, sizeof(cmd), "xterm -e alsamixer%s", mixer_args);
    else {
        toast_show("ALSA", "No terminal available to host alsamixer", 2.5);
        return;
    }
    spawn_command(cmd);
    toast_show("ALSA", "Opening alsamixer…", 2.0);
}

static void on_system_toggle(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "sys_")) { id = cand; break; }
    }
    if (!id || strcmp(id, "sys_numlock")) return;
    g_settings.numlock_on = !g_settings.numlock_on;
    settings_mark_toggle(id, g_settings.numlock_on);
    apply_wm_settings();
    settings_save();
}

static void on_wm_toggle(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "wm_")) { id = cand; break; }
    }
    if (!id) return;
    int* value = NULL;
    if (!strcmp(id, "wm_snap")) value = &g_settings.edge_snap;
    else if (!strcmp(id, "wm_top_maximize")) value = &g_settings.top_edge_maximize;
    else if (!strcmp(id, "wm_double_click")) value = &g_settings.titlebar_double_click;
    else if (!strcmp(id, "wm_classic_titlebar")) value = &g_settings.classic_titlebar;
    else if (!strcmp(id, "wm_shortcuts")) value = &g_settings.super_shortcuts;
    else if (!strcmp(id, "wm_dock_mag")) value = &g_settings.dock_magnification;
    else if (!strcmp(id, "wm_wallpaper_motion")) value = &g_settings.wallpaper_animation;
    else if (!strcmp(id, "wm_restore")) value = &g_settings.session_restore;
    if (!value) return;
    *value = !*value;
    settings_mark_toggle(id, *value);
    if (!strcmp(id, "wm_wallpaper_motion"))
        apply_wallpaper(g_settings.wallpaper);
    else
        apply_wm_settings();
    settings_save();
    shell_request_repaint(-1);
}

static void on_wm_gap(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "wm_gap_")) { id = cand; break; }
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

static void on_skin_select(LunaElement* e) {
    int slot = -1;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (id && id[0] == 's' && id[1] == 'k' && id[2] == 'i' &&
            id[3] == 'n' && id[4] == '_' && isdigit((unsigned char)id[5])) {
            slot = atoi(id + 5);
            break;
        }
    }
    if (slot < 0 || slot >= g_skin_count) return;
    if (!skin_apply_styles(g_skins[slot].id)) {
        toast_show("Appearance", "The skin stylesheet could not be loaded.", 3.0);
        return;
    }
    snprintf(g_settings.skin, sizeof(g_settings.skin), "%s", g_skins[slot].id);
    settings_save();
    skin_apply_chrome(slot);
    skin_apply_toolkit(slot);
    apply_wm_settings();
    apply_wallpaper(g_settings.wallpaper);
    settings_populate_skins();
    shell_request_repaint(-1);
    if (g_skins[slot].layout[0] &&
        (!g_layout_path || strcmp(g_layout_path, g_skins[slot].layout) != 0))
        toast_show("Appearance",
                   "Style & chrome applied. Custom HTML layout loads at next sign-in. Restart apps for GTK/Qt themes.",
                   5.0);
    else if (g_skins[slot].gtk_theme[0] || g_skins[slot].qt_style[0] || g_skins[slot].qt_qss[0])
        toast_show("Appearance",
                   "Desktop skin applied. Restart apps to pick up GTK/Qt themes.",
                   4.0);
    else
        toast_show("Appearance", "Desktop skin applied.", 2.0);
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
        luna_update_classes(ok, "danger primary", danger ? "danger" : "primary");
    }
    switch (action) {
    case ACT_SHUTDOWN:
        if (t >= 0)  luna_set_text(t,  "Shut Down?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close. Save your work before continuing.");
        if (ok >= 0) luna_set_text(ok, "Shut Down");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x80\x91"); /* power-off, U+F011 */
            luna_update_classes(icon, "restart logout", NULL);
        }
        break;
    case ACT_RESTART:
        if (t >= 0)  luna_set_text(t,  "Restart?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close, then the computer will restart.");
        if (ok >= 0) luna_set_text(ok, "Restart");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x80\xa1"); /* rotate, U+F021 */
            luna_update_classes(icon, "restart logout", "restart");
        }
        break;
    case ACT_LOGOUT:
        if (t >= 0)  luna_set_text(t,  "Log Out?");
        if (m >= 0)  luna_set_text(m,  "Open applications will close and the session will end.");
        if (ok >= 0) luna_set_text(ok, "Log Out");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x82\x8b"); /* sign-out, U+F08B */
            luna_update_classes(icon, "restart logout", "logout");
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
    if (!strcmp(t->id, "cc_wifi")) {
        on_wifi_power(e);
        return;
    }
    if (!strcmp(t->id, "cc_bt")) {
        on_bt_power(e);
        return;
    }
    luna_update_classes(idx, "on", now_on ? "on" : NULL);
    /* Keep the knob's layout position stable and let the .on CSS transform
     * provide the entire animation.  Overriding rel_x here used to add a
     * second 18 px movement on top of translateX(), and made the transformed
     * child jump out from under the pointer during release hit-testing. */
}

/* Brightness / volume: 0..1 cached levels mirrored by the Control Center
 * sliders.  System backends are optional; the UI still moves when unavailable. */
static float g_bright_level = 0.75f;
static float g_vol_level = 0.60f;
static int g_bright_available = 0;
static int g_vol_available = 0;
static double g_last_bright_apply = -1e9;
static double g_last_vol_apply = -1e9;
static float g_last_bright_written = -1.0f;
static float g_last_vol_written = -1.0f;

struct LunaSliderIds {
    int thumb, fill, track;
    int resolved;
    float last_thumb_x, last_track_w;
    float* level;
    int* available;
    double* last_apply;
    float* last_written;
    int (*write_fn)(float level01);
    const char* kind; /* "brightness" | "volume" */
};

static int shell_run_capture(const char* cmd, char* out, size_t out_n) {
    if (!cmd || !out || out_n == 0) return 0;
    out[0] = 0;
    /* popen children must not be stolen by the shell-wide waitpid(-1) reaper. */
    pthread_mutex_lock(&g_child_reaper_mutex);
    FILE* f = popen(cmd, "r");
    if (!f) {
        pthread_mutex_unlock(&g_child_reaper_mutex);
        return 0;
    }
    size_t used = 0;
    while (used + 1 < out_n) {
        size_t n = fread(out + used, 1, out_n - 1 - used, f);
        if (n == 0) break;
        used += n;
    }
    out[used] = 0;
    int rc = pclose(f);
    pthread_mutex_unlock(&g_child_reaper_mutex);
    return rc == 0 || (WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
}

static int shell_spawn_argv(const char* const argv[]) {
    if (!argv || !argv[0]) return 0;
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, (char* const*)argv, environ) != 0)
        return 0;
    /* Detached: reaper collects the child.  Do not block the UI thread. */
    return 1;
}

static int brightness_read_sysfs(float* out01) {
    DIR* d = opendir("/sys/class/backlight");
    if (!d) return 0;
    struct dirent* de;
    int ok = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char cur_path[256], max_path[256];
        if (snprintf(cur_path, sizeof(cur_path),
                     "/sys/class/backlight/%s/brightness", de->d_name) >= (int)sizeof(cur_path))
            continue;
        if (snprintf(max_path, sizeof(max_path),
                     "/sys/class/backlight/%s/max_brightness", de->d_name) >= (int)sizeof(max_path))
            continue;
        FILE* fc = fopen(cur_path, "r");
        FILE* fm = fopen(max_path, "r");
        long cur = 0, max = 0;
        if (fc && fm && fscanf(fc, "%ld", &cur) == 1 && fscanf(fm, "%ld", &max) == 1 && max > 0) {
            *out01 = (float)cur / (float)max;
            if (*out01 < 0.0f) *out01 = 0.0f;
            if (*out01 > 1.0f) *out01 = 1.0f;
            ok = 1;
        }
        if (fc) fclose(fc);
        if (fm) fclose(fm);
        if (ok) break;
    }
    closedir(d);
    return ok;
}

static int brightness_write_sysfs(float level01) {
    DIR* d = opendir("/sys/class/backlight");
    if (!d) return 0;
    struct dirent* de;
    int ok = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char max_path[256], cur_path[256];
        if (snprintf(max_path, sizeof(max_path),
                     "/sys/class/backlight/%s/max_brightness", de->d_name) >= (int)sizeof(max_path))
            continue;
        if (snprintf(cur_path, sizeof(cur_path),
                     "/sys/class/backlight/%s/brightness", de->d_name) >= (int)sizeof(cur_path))
            continue;
        FILE* fm = fopen(max_path, "r");
        long max = 0;
        if (fm && fscanf(fm, "%ld", &max) == 1 && max > 0) {
            long cur = (long)(level01 * (float)max + 0.5f);
            if (cur < 1) cur = 1;
            if (cur > max) cur = max;
            FILE* fc = fopen(cur_path, "w");
            if (fc) {
                if (fprintf(fc, "%ld\n", cur) > 0) ok = 1;
                fclose(fc);
            }
        }
        if (fm) fclose(fm);
        if (ok) break;
    }
    closedir(d);
    return ok;
}

static int brightness_read_brightnessctl(float* out01) {
    char buf[256];
    if (!shell_run_capture("brightnessctl -m 2>/dev/null", buf, sizeof(buf))) return 0;
    /* device,class,current,percent%,max */
    char* p = buf;
    for (int i = 0; i < 3; i++) {
        p = strchr(p, ',');
        if (!p) return 0;
        p++;
    }
    int pct = atoi(p);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *out01 = (float)pct / 100.0f;
    return 1;
}

static int brightness_write_brightnessctl(float level01) {
    char pct[16];
    int v = (int)(level01 * 100.0f + 0.5f);
    if (v < 1) v = 1;
    if (v > 100) v = 100;
    snprintf(pct, sizeof(pct), "%d%%", v);
    const char* const argv[] = { "brightnessctl", "-q", "set", pct, NULL };
    return shell_spawn_argv(argv);
}

static int brightness_read_xrandr(float* out01) {
    char buf[8192];
    if (!shell_run_capture("xrandr --verbose --current 2>/dev/null", buf, sizeof(buf)))
        return 0;
    const char* p = strstr(buf, "Brightness:");
    if (!p) return 0;
    float b = 0.0f;
    if (sscanf(p + 11, "%f", &b) != 1) return 0;
    if (b < 0.1f) b = 0.1f;
    if (b > 1.0f) b = 1.0f;
    *out01 = (b - 0.1f) / 0.9f;
    return 1;
}

static int brightness_write_xrandr(float level01) {
    char buf[512];
    if (!shell_run_capture("xrandr --query 2>/dev/null", buf, sizeof(buf))) return 0;
    char output[64] = {0};
    char* line = buf;
    while (*line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strstr(line, " connected")) {
            sscanf(line, "%63s", output);
            if (strstr(line, " connected primary") || output[0]) break;
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (!output[0]) return 0;
    float b = 0.1f + level01 * 0.9f;
    char val[32];
    snprintf(val, sizeof(val), "%.3f", b);
    const char* const argv[] = { "xrandr", "--output", output, "--brightness", val, NULL };
    return shell_spawn_argv(argv);
}

static int brightness_read(float* out01) {
    const char* be = g_settings.brightness_backend;
    if (!be[0] || !strcmp(be, "auto")) {
        if (brightness_read_sysfs(out01)) return 1;
        if (command_available("brightnessctl") && brightness_read_brightnessctl(out01)) return 1;
        if (command_available("xrandr") && brightness_read_xrandr(out01)) return 1;
        return 0;
    }
    if (!strcmp(be, "sysfs")) return brightness_read_sysfs(out01);
    if (!strcmp(be, "brightnessctl"))
        return command_available("brightnessctl") && brightness_read_brightnessctl(out01);
    if (!strcmp(be, "xrandr"))
        return command_available("xrandr") && brightness_read_xrandr(out01);
    return 0;
}

static int brightness_write(float level01) {
    if (level01 < 0.0f) level01 = 0.0f;
    if (level01 > 1.0f) level01 = 1.0f;
    const char* be = g_settings.brightness_backend;
    if (!be[0] || !strcmp(be, "auto")) {
        if (brightness_write_sysfs(level01)) return 1;
        if (command_available("brightnessctl") && brightness_write_brightnessctl(level01)) return 1;
        if (command_available("xrandr") && brightness_write_xrandr(level01)) return 1;
        return 0;
    }
    if (!strcmp(be, "sysfs")) return brightness_write_sysfs(level01);
    if (!strcmp(be, "brightnessctl"))
        return command_available("brightnessctl") && brightness_write_brightnessctl(level01);
    if (!strcmp(be, "xrandr"))
        return command_available("xrandr") && brightness_write_xrandr(level01);
    return 0;
}

static int volume_read_wpctl(float* out01) {
    char buf[128];
    if (!shell_run_capture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null",
                          buf, sizeof(buf)))
        return 0;
    const char* p = strstr(buf, "Volume:");
    if (!p) return 0;
    float v = 0.0f;
    if (sscanf(p + 7, "%f", &v) != 1) return 0;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    *out01 = v;
    return 1;
}

static int volume_write_wpctl(float level01) {
    char val[32];
    snprintf(val, sizeof(val), "%.3f", level01);
    const char* const argv[] = {
        "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", val, NULL
    };
    return shell_spawn_argv(argv);
}

static int volume_read_pactl(float* out01) {
    char buf[512];
    if (!shell_run_capture("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null",
                          buf, sizeof(buf)))
        return 0;
    const char* p = strchr(buf, '%');
    if (!p) return 0;
    /* Walk back to the number before the first %. */
    const char* s = p;
    while (s > buf && (isdigit((unsigned char)s[-1]) || s[-1] == ' ')) s--;
    int pct = atoi(s);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *out01 = (float)pct / 100.0f;
    return 1;
}

static int volume_write_pactl(float level01) {
    char pct[16];
    int v = (int)(level01 * 100.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    snprintf(pct, sizeof(pct), "%d%%", v);
    const char* const argv[] = {
        "pactl", "set-sink-volume", "@DEFAULT_SINK@", pct, NULL
    };
    return shell_spawn_argv(argv);
}

static int alsa_control_ok(const char* name) {
    if (!name || !*name || strlen(name) >= 64) return 0;
    for (const char* p = name; *p; p++) {
        if (isalnum((unsigned char)*p) || *p == ' ' || *p == '_' ||
            *p == '-' || *p == '.' || *p == '+' || *p == '/')
            continue;
        return 0;
    }
    return 1;
}

static int alsa_card_ok(const char* card) {
    if (!card || !*card) return 1;
    if (!strcmp(card, "default")) return 1;
    if (!strncmp(card, "hw:", 3)) {
        for (const char* p = card + 3; *p; p++)
            if (!isdigit((unsigned char)*p) && *p != ',') return 0;
        return 1;
    }
    for (const char* p = card; *p; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

/* Build amixer argv into out[0..].  Returns argc, or 0 on invalid settings.
 * out must hold at least 8 pointers; strings live in scratch buffers. */
static int amixer_build_argv(const char* action, const char* value,
                             char card_buf[32], char ctl_buf[64], char val_buf[32],
                             const char* out[8]) {
    if (!alsa_control_ok(g_settings.alsa_control) || !alsa_card_ok(g_settings.alsa_card))
        return 0;
    if (!command_available("amixer")) return 0;
    snprintf(ctl_buf, 64, "%s", g_settings.alsa_control);
    int n = 0;
    out[n++] = "amixer";
    out[n++] = "-M"; /* mapped volume percent, matches alsamixer */
    const char* card = g_settings.alsa_card;
    if (card[0] && strcmp(card, "default") != 0) {
        if (!strncmp(card, "hw:", 3)) {
            snprintf(card_buf, 32, "%s", card);
            out[n++] = "-D";
            out[n++] = card_buf;
        } else {
            snprintf(card_buf, 32, "%s", card);
            out[n++] = "-c";
            out[n++] = card_buf;
        }
    }
    out[n++] = action;
    out[n++] = ctl_buf;
    if (value && *value) {
        snprintf(val_buf, 32, "%s", value);
        out[n++] = val_buf;
    }
    out[n] = NULL;
    return n;
}

static int volume_read_amixer(float* out01) {
    char card_buf[32], ctl_buf[64], val_buf[32];
    const char* argv[8];
    if (!amixer_build_argv("sget", NULL, card_buf, ctl_buf, val_buf, argv))
        return 0;
    /* Rebuild as a shell-safe capture command.  argv is validated above. */
    char cmd[256];
    int off = 0;
    for (int i = 0; argv[i]; i++) {
        int wrote = snprintf(cmd + off, sizeof(cmd) - (size_t)off, "%s%s",
                             i ? " " : "", argv[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof(cmd) - (size_t)off) return 0;
        off += wrote;
    }
    if (snprintf(cmd + off, sizeof(cmd) - (size_t)off, " 2>/dev/null") < 0)
        return 0;
    char buf[2048];
    if (!shell_run_capture(cmd, buf, sizeof(buf))) return 0;
    /* Prefer Playback percentages: "Playback 50 [39%] [-20.25dB]" */
    int pct = -1;
    for (char* p = buf; (p = strstr(p, "Playback")) != NULL; p++) {
        char* br = strchr(p, '[');
        if (!br) continue;
        if (strchr(br, '%')) {
            pct = atoi(br + 1);
            break;
        }
    }
    if (pct < 0) {
        char* br = strchr(buf, '[');
        while (br) {
            if (strchr(br, '%')) { pct = atoi(br + 1); break; }
            br = strchr(br + 1, '[');
        }
    }
    if (pct < 0) return 0;
    if (pct > 100) pct = 100;
    *out01 = (float)pct / 100.0f;
    return 1;
}

static int volume_write_amixer(float level01) {
    char pct[16];
    int v = (int)(level01 * 100.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    snprintf(pct, sizeof(pct), "%d%%", v);
    char card_buf[32], ctl_buf[64], val_buf[32];
    const char* argv_const[8];
    if (!amixer_build_argv("sset", pct, card_buf, ctl_buf, val_buf, argv_const))
        return 0;
    /* shell_spawn_argv wants a mutable-looking argv array of non-const. */
    char* argv[8];
    for (int i = 0; i < 8; i++) {
        argv[i] = (char*)argv_const[i];
        if (!argv_const[i]) break;
    }
    return shell_spawn_argv((const char* const*)argv);
}

static int volume_try_backend(const char* be, float* out01, int write, float level01) {
    if (!strcmp(be, "wpctl")) {
        if (write) return command_available("wpctl") && volume_write_wpctl(level01);
        return command_available("wpctl") && volume_read_wpctl(out01);
    }
    if (!strcmp(be, "pactl")) {
        if (write) return command_available("pactl") && volume_write_pactl(level01);
        return command_available("pactl") && volume_read_pactl(out01);
    }
    if (!strcmp(be, "alsa")) {
        if (write) return volume_write_amixer(level01);
        return volume_read_amixer(out01);
    }
    return 0;
}

static int volume_read(float* out01) {
    const char* be = g_settings.audio_backend;
    if (be[0] && strcmp(be, "auto") != 0)
        return volume_try_backend(be, out01, 0, 0.0f);
    if (volume_try_backend("wpctl", out01, 0, 0.0f)) return 1;
    if (volume_try_backend("pactl", out01, 0, 0.0f)) return 1;
    if (volume_try_backend("alsa", out01, 0, 0.0f)) return 1;
    return 0;
}

static int volume_write(float level01) {
    if (level01 < 0.0f) level01 = 0.0f;
    if (level01 > 1.0f) level01 = 1.0f;
    const char* be = g_settings.audio_backend;
    if (be[0] && strcmp(be, "auto") != 0)
        return volume_try_backend(be, NULL, 1, level01);
    if (volume_try_backend("wpctl", NULL, 1, level01)) return 1;
    if (volume_try_backend("pactl", NULL, 1, level01)) return 1;
    if (volume_try_backend("alsa", NULL, 1, level01)) return 1;
    return 0;
}

static float slider_read_ratio(int thumb_idx, int track_idx) {
    if (thumb_idx < 0 || track_idx < 0) return 0.0f;
    LunaElement* th = luna_element_at(thumb_idx);
    LunaElement* tr = luna_element_at(track_idx);
    if (!th || !tr) return 0.0f;
    float usable = tr->w - th->w;
    if (usable < 1.0f) usable = 1.0f;
    float ratio = th->rel_x / usable;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio;
}

static void slider_set_ratio(int thumb_idx, int fill_idx, int track_idx, float ratio) {
    if (thumb_idx < 0 || track_idx < 0) return;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    LunaElement* th = luna_element_at(thumb_idx);
    LunaElement* tr = luna_element_at(track_idx);
    if (!th || !tr) return;
    float track_w = tr->w > 1.0f ? tr->w : 268.0f;
    float thumb_w = th->w > 1.0f ? th->w : 18.0f;
    float usable = track_w - thumb_w;
    if (usable < 1.0f) usable = 1.0f;
    float thumb_x = ratio * usable;

    /* Inline style="left: N%" / "width: N%" must not keep winning over the
     * absolute positions we write while the user drags. */
    th->pct_left = 0;
    th->has_left = 1;
    th->rel_x = thumb_x;
    th->rel_y = 1.0f;
    th->pos_overridden_x = 1;
    th->pos_overridden_y = 1;

    if (fill_idx >= 0) {
        LunaElement* fill = luna_element_at(fill_idx);
        fill->pct_w = 0;
        fill->has_css_width = 1;
        fill->css_width = thumb_x + thumb_w * 0.5f;
        if (fill->css_width < 0.0f) fill->css_width = 0.0f;
        if (fill->css_width > track_w) fill->css_width = track_w;
    }
}

static void slider_apply_level(LunaSliderIds* ids, int force) {
    if (!ids || !ids->level || !ids->write_fn) return;
    float level = *ids->level;
    if (!force && ids->last_written && fabsf(level - *ids->last_written) < 0.005f)
        return;
    if (!force && ids->last_apply && g_now - *ids->last_apply < 0.05)
        return;
    if (ids->write_fn(level)) {
        if (ids->available) *ids->available = 1;
        if (ids->last_written) *ids->last_written = level;
        if (ids->last_apply) *ids->last_apply = g_now;
    } else if (ids->available) {
        *ids->available = 0;
    }
}

static void slider_tick_cached(LunaSliderIds* ids) {
    if (!ids->resolved) return;
    int ti = ids->thumb, fi = ids->fill, ki = ids->track;
    if (ti == -1 || fi == -1 || ki == -1) return;
    LunaElement* th = luna_element_at(ti);
    if (!luna_element_visible(ti) && !th->is_active) {
        ids->last_thumb_x = NAN;
        ids->last_track_w = NAN;
        return;
    }
    LunaElement* tr = luna_element_at(ki);
    float track_w = tr->w > 0 ? tr->w : 268.0f;
    float thumb_w = th->w > 1.0f ? th->w : 18.0f;
    float usable = track_w - thumb_w;
    if (usable < 1.0f) usable = 1.0f;

    /* Prefer the live drag position; otherwise restore from the cached level. */
    float thumb_x = th->is_active ? th->rel_x : (*ids->level * usable);
    if (thumb_x < 0.0f) thumb_x = 0.0f;
    if (thumb_x > usable) thumb_x = usable;

    int moved = th->is_active &&
                (ids->last_thumb_x != thumb_x || ids->last_track_w != track_w);
    if (thumb_x == ids->last_thumb_x && track_w == ids->last_track_w &&
        th->rel_y == 1.0f && th->pos_overridden_x && th->pos_overridden_y &&
        !th->pct_left)
        return;

    slider_set_ratio(ti, fi, ki, thumb_x / usable);
    *ids->level = thumb_x / usable;
    ids->last_thumb_x = thumb_x;
    ids->last_track_w = track_w;
    if (moved || th->is_active)
        slider_apply_level(ids, 0);
}

static void slider_tick_when_needed(LunaSliderIds* ids) {
    if (!ids->resolved) return;
    if (!is_shown(g_cc_idx)) {
        if (ids->thumb < 0) return;
        LunaElement* th = luna_element_at(ids->thumb);
        if (!th || !th->is_active) return;
    }
    slider_tick_cached(ids);
}

static void slider_resolve(LunaSliderIds* ids, const char* thumb_id,
                           const char* fill_id, const char* track_id) {
    ids->thumb = luna_get_element_by_id(thumb_id);
    ids->fill  = luna_get_element_by_id(fill_id);
    ids->track = luna_get_element_by_id(track_id);
    ids->last_thumb_x = NAN;
    ids->last_track_w = NAN;
    ids->resolved = 1;
    if (ids->thumb >= 0) {
        LunaElement* th = luna_element_at(ids->thumb);
        th->is_draggable = 1;
        th->drag_mode = 2; /* drag self inside the track */
        th->cursor_pointer = 1;
    }
}

static void cc_sliders_pull_from_system(LunaSliderIds* bright, LunaSliderIds* vol) {
    float b = g_bright_level, v = g_vol_level;
    g_bright_available = brightness_read(&b);
    if (g_bright_available) g_bright_level = b;
    g_vol_available = volume_read(&v);
    if (g_vol_available) g_vol_level = v;
    if (bright && bright->resolved)
        slider_set_ratio(bright->thumb, bright->fill, bright->track, g_bright_level);
    if (vol && vol->resolved)
        slider_set_ratio(vol->thumb, vol->fill, vol->track, g_vol_level);
    if (bright) {
        bright->last_thumb_x = NAN;
        bright->last_track_w = NAN;
        if (bright->last_written) *bright->last_written = g_bright_level;
    }
    if (vol) {
        vol->last_thumb_x = NAN;
        vol->last_track_w = NAN;
        if (vol->last_written) *vol->last_written = g_vol_level;
    }
    luna_mark_layout_dirty();
}

static void cc_sliders_flush(LunaSliderIds* bright, LunaSliderIds* vol) {
    if (bright && bright->resolved) {
        *bright->level = slider_read_ratio(bright->thumb, bright->track);
        slider_apply_level(bright, 1);
    }
    if (vol && vol->resolved) {
        *vol->level = slider_read_ratio(vol->thumb, vol->track);
        slider_apply_level(vol, 1);
    }
}

/* Filled from main() so track-click handlers can reach the live slider state. */

static void on_cc_track(LunaElement* e) {
    int track = -1;
    for (int i = elem_idx_of(e); i != -1; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!strcmp(id, "bright_track") || !strcmp(id, "vol_track")) {
            track = i;
            break;
        }
    }
    if (track < 0) return;
    LunaSliderIds* ids = NULL;
    if (!strcmp(luna_element_at(track)->id, "bright_track")) ids = g_bright_slider_ptr;
    else ids = g_vol_slider_ptr;
    if (!ids || !ids->resolved || ids->thumb < 0) return;

    double mx = 0, my = 0;
    luna_get_pointer(&mx, &my);
    LunaElement* tr = luna_element_at(track);
    LunaElement* th = luna_element_at(ids->thumb);
    float usable = tr->w - (th->w > 1.0f ? th->w : 18.0f);
    if (usable < 1.0f) usable = 1.0f;
    float ratio = ((float)mx - tr->x - (th->w * 0.5f)) / usable;
    slider_set_ratio(ids->thumb, ids->fill, ids->track, ratio);
    *ids->level = slider_read_ratio(ids->thumb, ids->track);
    ids->last_thumb_x = NAN;
    slider_apply_level(ids, 1);
    luna_mark_layout_dirty();
    if (ids->available && !*ids->available)
        toast_show(ids->kind && !strcmp(ids->kind, "volume") ? "Sound" : "Display",
                   ids->kind && !strcmp(ids->kind, "volume")
                       ? "No audio backend (wpctl/pactl)"
                       : "No brightness backend",
                   2.5);
}

/* ── System status snapshots (sampling lives in luna-monitor.h) ── */
typedef LunaMonitorSnapshot ShellStatusSnapshot;
#define SHELL_ASYNC_STATE_MAX LUNA_MONITOR_STATE_MAX

static void poll_shell_state(void) {
    shell_state_watch_ensure();

    int should_load = 0;
    double interval = g_switcher_visible
        ? SHELL_STATE_SWITCHER_INTERVAL_SEC
        : SHELL_STATE_NORMAL_INTERVAL_SEC;
    if (g_shell_watch_wd >= 0) {
        if (g_shell_state_pending) {
            double deadline = shell_state_pending_deadline(g_now);
            if (deadline <= 0.0 || g_now + 0.0005 >= deadline) {
                g_shell_state_pending = 0;
                should_load = 1;

                if (g_shell_state_last_event_at > -1e8 &&
                    g_now - g_shell_state_last_event_at <
                        SHELL_STATE_BURST_QUIET_SEC) {
                    /* First read in a still-active write burst. */
                    g_shell_state_burst_read = 1;
                } else {
                    /* Final read after the move/resize stream went quiet. */
                    g_shell_state_burst_read = 0;
                }
            }
        }
    } else {
        /* Fallback for startup before the runtime directory exists, or kernels
         * without a usable watch. Keep bounded polling behavior. */
        if (g_now - g_last_shell_poll >= interval) should_load = 1;
    }

    if (should_load) {
        g_last_shell_poll = g_now;
        luna_monitor_request_state();
    }

    static char state_text[SHELL_ASYNC_STATE_MAX];
    size_t state_len = 0;
    if (luna_monitor_consume_state(state_text, sizeof(state_text), &state_len)) {
        g_shell_state_changed = 0;
        apply_shell_state_buffer(state_text, state_len);
        if (g_shell_state_changed) {
            update_window_list_ui();
            update_tray_ui();
            update_dock_dots();
            update_switcher_ui();
        }
    }

    if (g_pending_menu_id > 0) {
        uint64_t wid = g_pending_menu_id;
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
        int t = g_ui_idx[UI_WIN_MENU_TITLE];
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
    char dir[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.state_home, "luna-shell", 0) ||
        !path_join2(buf, n, dir, "session"))
        buf[0] = 0;
}

static void legacy_session_path(char* buf, size_t n) {
    char dir[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-shell", 0) ||
        !path_join2(buf, n, dir, "session"))
        buf[0] = 0;
}

static void ensure_state_dir(void) {
    char dir[PATH_MAX];
    (void)mkdir_p_mode(g_xdg.state_home, 0700);
    (void)xdg_app_dir(dir, sizeof(dir), g_xdg.state_home, "luna-shell", 1);
}

static void session_save(void) {
    if (!g_settings.session_restore) return;
    ensure_state_dir();
    /* Shutdown is outside the frame loop, so take one synchronous compositor
     * snapshot here rather than waiting for the background poller. */
    (void)shell_state_watch_drain();
    g_shell_state_changed = 0;
    load_shell_state();

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
        if (!f) {
            /* One-time compatibility with releases that incorrectly stored
             * restart state below XDG_CONFIG_HOME. */
            legacy_session_path(path, sizeof(path));
            f = fopen(path, "r");
        }
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
                uint64_t wid = g_wins[best].id;
                char cmd[96];
                if (rw->fullscreen) {
                    snprintf(cmd, sizeof(cmd), "fullscreen %" PRIu64, wid);
                    shell_send_cmd(cmd);
                } else if (rw->maximized) {
                    snprintf(cmd, sizeof(cmd), "maximize %" PRIu64, wid);
                    shell_send_cmd(cmd);
                } else {
                    snprintf(cmd, sizeof(cmd), "move %" PRIu64 " %d %d", wid, rw->x, rw->y);
                    shell_send_cmd(cmd);
                }
                if (rw->minimized) {
                    snprintf(cmd, sizeof(cmd), "minimize %" PRIu64, wid);
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

static uint64_t win_id_from_element(LunaElement* e) {
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

static void win_menu_open(uint64_t wid, int anchor_idx, const char* title) {
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
    uint64_t wid = g_win_menu_target;
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
        snprintf(cmd, sizeof(cmd), "activate %" PRIu64, wid);
    else if (!strcmp(id, "wm_minimize"))
        snprintf(cmd, sizeof(cmd), "minimize %" PRIu64, wid);
    else if (!strcmp(id, "wm_maximize")) {
        if (w && w->fullscreen)
            snprintf(cmd, sizeof(cmd), "unfullscreen %" PRIu64, wid);
        else if (w && w->maximized)
            snprintf(cmd, sizeof(cmd), "unmaximize %" PRIu64, wid);
        else
            snprintf(cmd, sizeof(cmd), "toggle_maximize %" PRIu64, wid);
    } else if (!strcmp(id, "wm_fullscreen")) {
        if (w && w->fullscreen)
            snprintf(cmd, sizeof(cmd), "unfullscreen %" PRIu64, wid);
        else
            snprintf(cmd, sizeof(cmd), "fullscreen %" PRIu64, wid);
    } else if (!strcmp(id, "wm_tile_left"))
        snprintf(cmd, sizeof(cmd), "tile_left %" PRIu64, wid);
    else if (!strcmp(id, "wm_tile_right"))
        snprintf(cmd, sizeof(cmd), "tile_right %" PRIu64, wid);
    else if (!strcmp(id, "wm_center"))
        snprintf(cmd, sizeof(cmd), "center %" PRIu64, wid);
    else if (!strcmp(id, "wm_close"))
        snprintf(cmd, sizeof(cmd), "close %" PRIu64, wid);
    else
        return;
    shell_send_cmd(cmd);
}

static void on_win_click(LunaElement* e) {
    uint64_t wid = win_id_from_element(e);
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
        snprintf(cmd, sizeof(cmd), "minimize %" PRIu64, wid);
    else
        snprintf(cmd, sizeof(cmd), "activate %" PRIu64, wid);
    shell_send_cmd(cmd);
}

/* ── Clipboard history menu ── */

#define CLIP_MENU_SLOTS 8

/* Persistent clipboard history lives under XDG_DATA_HOME (survives cache
 * cleanups and reboots).  Fall back to the legacy cache path so existing
 * histories keep showing until luna-clipboard migrates them. */
static void clip_history_path(char* buf, size_t n) {
    char dir[PATH_MAX];
    buf[0] = 0;
    if (xdg_app_dir(dir, sizeof(dir), g_xdg.data_home, "luna-clipboard", 0) &&
        path_join2(buf, n, dir, "history") && access(buf, R_OK) == 0)
        return;
    if (xdg_app_dir(dir, sizeof(dir), g_xdg.cache_home, "luna-clipboard", 0) &&
        path_join2(buf, n, dir, "history") && access(buf, R_OK) == 0)
        return;
    /* Prefer the durable location when the file does not exist yet. */
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.data_home, "luna-clipboard", 0) ||
        !path_join2(buf, n, dir, "history"))
        buf[0] = 0;
}

static void clip_cmd_sock_path(char* buf, size_t n) {
    if (!path_join2(buf, n, g_xdg.runtime_dir, "luna-clipboard.sock"))
        buf[0] = 0;
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
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    if (is_shown(g_clip_menu_idx)) { dismiss_clip_menu(); return; }
    clip_populate_menu();
    position_menu_near(g_clip_menu_idx,
                       menu_anchor_from(e, g_mb_clip_idx),
                       luna_window_width - 300.0f);
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
                toast_show("Power", g_cached_bat >= 0 ? "On battery" : "AC connected", 2.5);
                return;
            }
            if (slot >= 0 && slot < app_slots && g_tray[slot].surface_id) {
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "activate %" PRIu64, g_tray[slot].surface_id);
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
 * render thread and showed up as a regular hitch.  Keep both the percentage
 * source and its resolved width in sync directly.  Keeping pct_w/raw_w is
 * important: otherwise a later unrelated layout pass restores the stale
 * percentage from the HTML and makes the meter jump back. */
static int set_bar_fill(int fi, float pct) {
    if (fi < 0) return 0;
    LunaElement* fill = luna_element_at(fi);
    float bar_w = 176.0f;
    int p = fill->parent_idx;
    if (p != -1) {
        LunaElement* bar = luna_element_at(p);
        if (bar && bar->w > 0.0f) {
            /* Percent widths resolve against the parent's content box.  Using
             * the border-box width overfills the inset Win95/XP meters. */
            bar_w = bar->w - bar->pad_l - bar->pad_r
                         - bar->border_width * 2.0f;
            if (bar_w < 0.0f) bar_w = 0.0f;
        }
    }
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float ratio = pct / 100.0f;
    float nw = bar_w * ratio;
    if (fill->pct_w && fabsf(fill->raw_w - ratio) < 0.0001f &&
        fabsf(fill->w - nw) < 0.5f)
        return 0;
    fill->pct_w = 1;
    fill->raw_w = ratio;
    fill->raw_w_off = 0.0f;
    fill->css_width = ratio;
    fill->has_css_width = 1;
    fill->w = nw;
    return 1;
}

/* Would repainting for this element actually change a pixel?
 *
 * The CPU/memory/disk readouts live on the wallpaper layer, which is the
 * full-screen surface: marking it dirty costs a whole-document GL frame plus a
 * whole-screen recomposite.  Doing that for a widget panel the user has closed
 * is exactly the kind of periodic hitch this loop must not produce. */
static int repaint_matters(int idx) {
    return idx >= 0 && luna_element_visible(idx);
}

static int text_would_change(int idx, const char* buf) {
    if (idx < 0 || !buf) return 0;
    LunaElement* e = luna_element_at(idx);
    return e && strcmp(e->text, buf) != 0;
}

/* Apply a completed worker snapshot.  This function performs only DOM/style
 * mutations; all procfs, sysfs, filesystem and time-zone work happened on the
 * poller thread before the eventfd woke this loop. */
static int shell_desktop_busy(void) {
    return g_visible_window_count > 0;
}

static void update_async_status(void) {
    /* Foreground status (menubar / Control Center) is consumed once per worker
     * sample.  Wallpaper widgets are kept separately and applied only when the
     * background already has a legitimate reason to repaint.  This prevents a
     * static wallpaper from doing a full-screen GL render every second merely
     * because the CPU percentage changed. */
    static ShellStatusSnapshot pending_bg;
    static int pending_bg_valid = 0;
    ShellStatusSnapshot fresh;
    int have_fresh = luna_monitor_consume_status(&fresh);

    char buf[64];
    int dirty_mb = 0, dirty_cc = 0;
    if (have_fresh) {
        pending_bg = fresh;
        pending_bg_valid = 1;

        int idx = g_ui_idx[UI_MB_CLOCK];
        if (fresh.mb_clock[0] && text_would_change(idx, fresh.mb_clock)) {
            luna_set_text_paint_only(idx, fresh.mb_clock);
            dirty_mb |= repaint_matters(idx);
        }

        /* Control Center is a small independent surface, so its live meters do
         * not force a wallpaper-layer repaint. */
        if (is_shown(g_cc_idx)) {
            if (set_bar_fill(g_ui_idx[UI_CC_CPU_FILL], fresh.cpu)) dirty_cc = 1;
            if (set_bar_fill(g_ui_idx[UI_CC_MEM_FILL], (float)fresh.memory)) dirty_cc = 1;
        }

        g_cached_bat = fresh.battery;
        idx = g_ui_idx[UI_MB_BAT];
        const char* bat_icon = g_cached_bat >= 0 ? "\uf240" : "\uf1e6";
        if (g_cached_bat >= 0) snprintf(buf, sizeof(buf), "%d%%", g_cached_bat);
        else snprintf(buf, sizeof(buf), "AC");
        if (g_mb_bat_icon_idx >= 0 &&
            text_would_change(g_mb_bat_icon_idx, bat_icon)) {
            luna_set_text_paint_only(g_mb_bat_icon_idx, bat_icon);
            dirty_mb |= repaint_matters(g_mb_bat_icon_idx);
        }
        if (text_would_change(idx, buf)) {
            luna_set_text_paint_only(idx, buf);
            dirty_mb |= repaint_matters(idx);
        }

        snprintf(g_cached_net, sizeof(g_cached_net), "%s", fresh.network);
        idx = g_mb_wifi_idx;
        const char* net_icon = (!strcmp(fresh.network, "Ethernet")) ? "\uf6ff" : "\uf1eb";
        if (g_mb_wifi_icon_idx >= 0 &&
            text_would_change(g_mb_wifi_icon_idx, net_icon)) {
            luna_set_text_paint_only(g_mb_wifi_icon_idx, net_icon);
            dirty_mb |= repaint_matters(g_mb_wifi_icon_idx);
        }
        if (text_would_change(idx, g_cached_net)) {
            luna_set_text_paint_only(idx, g_cached_net);
            dirty_mb |= repaint_matters(idx);
        }
    }

    if (dirty_mb) shell_request_repaint(1);
    if (dirty_cc && is_shown(g_cc_idx)) shell_request_repaint(6);

    if (!pending_bg_valid) return;

    int wg_time_idx = g_ui_idx[UI_WG_TIME];
    int wg_date_idx = g_ui_idx[UI_WG_DATE];
    int stats_live = repaint_matters(g_ui_idx[UI_ST_CPU_VAL]);
    int clock_widget_live = repaint_matters(wg_time_idx) || repaint_matters(wg_date_idx);
    int bg_widgets_live = stats_live || clock_widget_live;
    if (!bg_widgets_live) {
        pending_bg_valid = 0;
        return;
    }

    /* Do not create a new full-screen frame for status data.  Animated
     * wallpapers already repaint on cadence; static wallpapers pick up the
     * newest values on the next real background repaint (wallpaper change,
     * layout change, explicit damage, etc.). */
    int bg_status_safe = !shell_desktop_busy() && !g_switcher_visible &&
                         g_now - g_last_user_activity >= STATUS_BG_IDLE_GRACE_SEC &&
                         shell_bg_passive_refresh_ready();
    if (!bg_status_safe) return;

    ShellStatusSnapshot snap = pending_bg;
    int dirty_bg = 0;
    int idx;

    if (snap.widget_time[0] && text_would_change(wg_time_idx, snap.widget_time)) {
        luna_set_text_paint_only(wg_time_idx, snap.widget_time);
        dirty_bg |= repaint_matters(wg_time_idx);
    }
    if (snap.widget_date[0] && text_would_change(wg_date_idx, snap.widget_date)) {
        luna_set_text_paint_only(wg_date_idx, snap.widget_date);
        dirty_bg |= repaint_matters(wg_date_idx);
    }

    if (stats_live) {
        idx = g_ui_idx[UI_ST_CPU_VAL];
        snprintf(buf, sizeof(buf), "%d%%", (int)snap.cpu);
        if (text_would_change(idx, buf)) {
            luna_set_text_paint_only(idx, buf);
            dirty_bg |= repaint_matters(idx);
        }
        if (set_bar_fill(g_ui_idx[UI_ST_CPU_FILL], snap.cpu))
            dirty_bg |= repaint_matters(g_ui_idx[UI_ST_CPU_FILL]);

        idx = g_ui_idx[UI_ST_MEM_VAL];
        snprintf(buf, sizeof(buf), "%d%%", snap.memory);
        if (text_would_change(idx, buf)) {
            luna_set_text_paint_only(idx, buf);
            dirty_bg |= repaint_matters(idx);
        }
        if (set_bar_fill(g_ui_idx[UI_ST_MEM_FILL], (float)snap.memory))
            dirty_bg |= repaint_matters(g_ui_idx[UI_ST_MEM_FILL]);

        if (snap.disk_valid &&
            repaint_matters(g_ui_idx[UI_ST_DISK_VAL])) {
            idx = g_ui_idx[UI_ST_DISK_VAL];
            snprintf(buf, sizeof(buf), "%.0fG free", snap.disk_free_gb);
            if (text_would_change(idx, buf)) {
                luna_set_text_paint_only(idx, buf);
                dirty_bg |= repaint_matters(idx);
            }
            if (set_bar_fill(g_ui_idx[UI_ST_DISK_FILL], snap.disk_used))
                dirty_bg |= repaint_matters(g_ui_idx[UI_ST_DISK_FILL]);
        }
    }

    if (dirty_bg) shell_request_repaint(0);
    pending_bg_valid = 0;
}

/* Bound event sleep by the next scheduled shell job.  Input descriptors wake
 * poll immediately, so longer idle sleeps do not add input latency. */
static int shell_wait_timeout_ms(int max_ms, double repaint_deadline) {
    double next = g_now + (double)max_ms / 1000.0;
#define SOONER(deadline) do { \
        double d_ = (deadline); \
        if (d_ > 0.0 && d_ < next) next = d_; \
    } while (0)
    /* When inotify is active, its fd wakes poll() immediately.  Keeping the
     * fallback 50/120 ms deadline after the watch was installed eventually
     * left that deadline permanently in the past, making every event wait
     * return 0 and turning the idle shell into a busy loop. */
    if (g_shell_watch_wd < 0) {
        SOONER(g_last_shell_poll +
               (g_switcher_visible ? SHELL_STATE_SWITCHER_INTERVAL_SEC
                                   : SHELL_STATE_NORMAL_INTERVAL_SEC));
        SOONER(g_shell_watch_retry_at);
    } else {
        SOONER(shell_state_pending_deadline(g_now));
    }

    /* These jobs are deliberately deferred by the main loop during pointer or
     * compositor interaction.  Do not leave an expired deadline in the poll
     * calculation while deferred: that would force a zero-timeout busy loop. */
    if (!g_interaction_busy) {
        SOONER(g_toast_deadline);
        SOONER(g_session_restore_at);
        if (g_wm_settings_pending)
            SOONER(g_wm_settings_retry_at);
    } else {
        if (g_window_motion_busy_until > g_now)
            SOONER(g_window_motion_busy_until);
        if (g_last_user_activity > -1e8 &&
            g_last_user_activity + INTERACTION_IDLE_GRACE_SEC > g_now)
            SOONER(g_last_user_activity + INTERACTION_IDLE_GRACE_SEC);
    }
    SOONER(repaint_deadline);
    if (g_cursor_present && !g_interaction_busy &&
        g_cur_theme.active_role >= 0 &&
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
        int idx = g_lp_app_idx[i];
        if (idx >= 0)
            luna_element_at(idx)->display_none = !str_contains_ci(g_apps[i].name, q);
    }
    for (int i = 0; i < g_lp_xdg_count; i++) {
        int idx = g_lp_xdg_idx[i];
        if (idx >= 0)
            luna_element_at(idx)->display_none = !str_contains_ci(g_lp_xdg[i].name, q);
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
    total_kb = luna_monitor_memory_total_kb();
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
    luna_register_js_handler("onLocaleSelect",  on_locale_select);
    luna_register_js_handler("onSystemToggle",  on_system_toggle);
    luna_register_js_handler("onWmToggle",      on_wm_toggle);
    luna_register_js_handler("onWmGap",         on_wm_gap);
    luna_register_js_handler("onSkinSelect",    on_skin_select);
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
    if (g_toast_idx < 0) {
        fprintf(stderr,
                "[luna-shell] layout truncated: #toast missing "
                "(need LUNA_UI_MAX_ELEMENTS >= DOM size; now %d, have %d elements)\n",
                LUNA_UI_MAX_ELEMENTS, elem_count);
    }
    g_lp_search_idx     = luna_get_element_by_id("lp_search");
    g_settings_idx      = luna_get_element_by_id("settings_win");
    g_settings_sheet_idx = luna_get_element_by_id("settings_sheet");
    g_settings_panel_apps = luna_get_element_by_id("settings_panel_apps");
    g_settings_panel_disp = luna_get_element_by_id("settings_panel_disp");
    g_settings_panel_lang = luna_get_element_by_id("settings_panel_language");
    g_settings_panel_kb   = luna_get_element_by_id("settings_panel_keyboard");
    g_settings_panel_sound = luna_get_element_by_id("settings_panel_sound");
    g_settings_panel_wm   = luna_get_element_by_id("settings_panel_wm");
    g_stab_apps_idx     = luna_get_element_by_id("stab_apps");
    g_stab_disp_idx     = luna_get_element_by_id("stab_disp");
    g_stab_lang_idx     = luna_get_element_by_id("stab_language");
    g_stab_kb_idx       = luna_get_element_by_id("stab_keyboard");
    g_stab_sound_idx    = luna_get_element_by_id("stab_sound");
    g_stab_wm_idx       = luna_get_element_by_id("stab_wm");
    g_win_menu_idx      = luna_get_element_by_id("win_menu");
    g_clip_menu_idx     = luna_get_element_by_id("clip_menu");
    g_mb_logo_idx       = luna_get_element_by_id("mb_logo");
    g_mb_cc_idx         = luna_get_element_by_id("mb_cc");
    g_mb_wifi_idx       = luna_get_element_by_id("mb_wifi");
    g_wifi_menu_idx     = luna_get_element_by_id("wifi_menu");
    g_bt_menu_idx       = luna_get_element_by_id("bt_menu");
    g_mb_weather_idx    = luna_get_element_by_id("mb_weather");
    g_weather_menu_idx  = luna_get_element_by_id("weather_menu");
    g_calendar_menu_idx = luna_get_element_by_id("calendar_menu");
    g_mb_clock_idx      = luna_get_element_by_id("mb_clock");
    g_net_detail_idx    = luna_get_element_by_id("net_detail_win");
    g_net_detail_box_idx = luna_get_element_by_id("net_detail_box");

    for (int i = 0; i < UI_CACHE_COUNT; i++)
        g_ui_idx[i] = luna_get_element_by_id(g_ui_cache_ids[i]);
    for (int i = 0; i < APP_COUNT; i++) g_lp_app_idx[i] = -1;
    for (int i = 0; i < MAX_SWITCHER_SLOTS; i++) g_sw_slot_idx[i] = -1;

    /* The redesigned markup keeps the about box's visual class but no longer
     * includes sheet_box.  The shared class supplies its absolute positioning,
     * stacking and clipping, all of which the C-side centering code relies on. */
    if (g_about_box_idx >= 0) {
        luna_update_classes(g_about_box_idx, NULL, "sheet_box");
    }
    if (g_net_detail_box_idx >= 0) {
        luna_update_classes(g_net_detail_box_idx, NULL, "sheet_box");
    }

    /* Drag handles used to advertise draggable="1" in the markup.  Keep this
     * behavior in the shell so presentation-only HTML changes cannot disable
     * moving either sheet. */
    {
        const char* drag_ids[] = { "about_drag", "settings_drag", "confirm_drag", "net_detail_drag" };
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
        g_lp_app_idx[i] = luna_get_element_by_id(id);
        wire_subtree(g_lp_app_idx[i], on_launch_app);
        app_set_dot(&g_apps[i], 0);
        snprintf(id, sizeof(id), "dock_pref_%s", g_apps[i].key);
        wire_subtree(luna_get_element_by_id(id), on_dock_pref_toggle);
    }
    apply_dock_app_settings();
    /* Settings is shell chrome, not an external application.  Override both
     * generic app launch bindings after the loop so the dock and Launchpad
     * open Luna's existing modeless settings sheet. */
    wire_subtree(luna_get_element_by_id("dock_settings"), on_settings_open);
    wire_subtree(luna_get_element_by_id("lp_settings"), on_settings_open);
    wire_subtree(luna_get_element_by_id("mb_logo"),       on_luna_menu);
    wire_subtree(luna_get_element_by_id("mb_wifi"),       on_wifi_menu);
    wire_subtree(luna_get_element_by_id("mb_clock"),      on_calendar_menu);
    wire_subtree(luna_get_element_by_id("wg_date"),       on_calendar_menu);
    wire_subtree(luna_get_element_by_id("widget_clock"),  on_calendar_menu);
    wire_subtree(luna_get_element_by_id("cal_prev"),      on_calendar_prev);
    wire_subtree(luna_get_element_by_id("cal_next"),      on_calendar_next);
    wire_subtree(luna_get_element_by_id("cal_today_btn"), on_calendar_today);
    for (int i = 0; i < 42; i++) {
        char id[16]; snprintf(id, sizeof(id), "cal_d%d", i);
        wire_subtree(luna_get_element_by_id(id), on_calendar_day);
    }
    wire_subtree(luna_get_element_by_id("mb_weather"),    on_weather_menu);
    wire_subtree(luna_get_element_by_id("weather_go"),    on_weather_go);
    wire_subtree(luna_get_element_by_id("widget_weather"),on_widget_weather);
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
    wire_subtree(luna_get_element_by_id("bright_track"),  on_cc_track);
    wire_subtree(luna_get_element_by_id("vol_track"),     on_cc_track);
    wire_subtree(luna_get_element_by_id("wifi_power"),    on_wifi_power);
    wire_subtree(luna_get_element_by_id("wifi_connect"),  on_wifi_connect);
    wire_subtree(luna_get_element_by_id("wifi_scan"),     on_wifi_scan);
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        char id[16]; snprintf(id, sizeof(id), "wifi_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_wifi_network);
    }
    for (int i = 0; i < MAX_ETHERNET_LINKS; i++) {
        char id[16]; snprintf(id, sizeof(id), "eth_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_eth_link);
    }
    wire_subtree(luna_get_element_by_id("bt_power"),      on_bt_power);
    wire_subtree(luna_get_element_by_id("bt_scan"),       on_bt_scan);
    wire_subtree(luna_get_element_by_id("cc_bt_open"),    on_bt_menu);
    for (int i = 0; i < MAX_BT_DEVICES; i++) {
        char id[16]; snprintf(id, sizeof(id), "bt_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_bt_device);
    }

    /* Wire wallpaper selection thumbs */
    for (int i = 0; i < MAX_SKINS; i++) {
        char id[24];
        snprintf(id, sizeof(id), "skin_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_skin_select);
    }
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
    wire_subtree(g_stab_lang_idx, on_settings_tab);
    wire_subtree(g_stab_kb_idx, on_settings_tab);
    wire_subtree(g_stab_sound_idx, on_settings_tab);
    wire_subtree(g_stab_wm_idx, on_settings_tab);
    {
        const char* locale_ids[] = { "locale_ja", "locale_en", "locale_c" };
        for (size_t i = 0; i < sizeof(locale_ids) / sizeof(locale_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(locale_ids[i]), on_locale_select);
        wire_subtree(luna_get_element_by_id("sys_numlock"), on_system_toggle);
    }
    {
        const char* audio_ids[] = {
            "audio_auto", "audio_wpctl", "audio_pactl", "audio_alsa"
        };
        for (size_t i = 0; i < sizeof(audio_ids) / sizeof(audio_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(audio_ids[i]), on_audio_backend_select);
        const char* bright_ids[] = {
            "bright_auto", "bright_sysfs", "bright_brightnessctl", "bright_xrandr"
        };
        for (size_t i = 0; i < sizeof(bright_ids) / sizeof(bright_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(bright_ids[i]), on_brightness_backend_select);
        wire_subtree(luna_get_element_by_id("btn_open_alsamixer"), on_open_alsamixer);
        const char* scale_ids[] = {
            "gdk_scale_1", "gdk_scale_2",
            "gdk_dpi_05", "gdk_dpi_075", "gdk_dpi_1", "gdk_dpi_125",
            "gdk_dpi_15", "gdk_dpi_175", "gdk_dpi_2",
            "qt_scale_05", "qt_scale_075", "qt_scale_1", "qt_scale_125",
            "qt_scale_15", "qt_scale_175", "qt_scale_2",
            "xcursor_24", "xcursor_32", "xcursor_48"
        };
        for (size_t i = 0; i < sizeof(scale_ids) / sizeof(scale_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(scale_ids[i]), on_display_scale_select);
    }
    {
        const char* toggle_ids[] = {
            "wm_snap", "wm_top_maximize", "wm_double_click", "wm_classic_titlebar", "wm_shortcuts",
            "wm_dock_mag", "wm_wallpaper_motion", "wm_restore"
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
    wire_subtree(luna_get_element_by_id("ntl_close"), net_detail_close);
    wire_subtree(luna_get_element_by_id("ntl_min"), net_detail_close);
    wire_subtree(luna_get_element_by_id("net_detail_close"), net_detail_close);
    wire_subtree(luna_get_element_by_id("net_detail_action"), on_net_detail_action);
    wire_subtree(luna_get_element_by_id("net_detail_backdrop"), net_detail_close);
    wire_subtree(luna_get_element_by_id("stl_close"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_backdrop"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_cancel"), on_settings_close);
    wire_subtree(luna_get_element_by_id("settings_ok"), on_settings_save);
    wire_subtree(luna_get_element_by_id("confirm_backdrop"), on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("ctl_close"),        on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("confirm_cancel"), on_confirm_cancel);
    wire_subtree(luna_get_element_by_id("confirm_ok"), on_confirm_ok);

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

    /* Collect sw_N parent indices once for both binding and runtime updates. */
    for (int s = 0; s < MAX_SWITCHER_SLOTS; s++) {
        char id[16];
        snprintf(id, sizeof(id), "sw_%d", s);
        g_sw_slot_idx[s] = luna_get_element_by_id(id);
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
                if (g_sw_slot_idx[s] == p) { g_sw_title_idx[s] = i; break; }
            continue;
        }
        if (strstr(e->class_name, "sw_app")) {
            for (int s = 0; s < MAX_SWITCHER_SLOTS; s++)
                if (g_sw_slot_idx[s] == p) { g_sw_app_idx[s] = i; break; }
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

    /* Initial Control Center state uses the persisted Wi-Fi / Bluetooth preference. */
    int cc_wifi = luna_get_element_by_id("cc_wifi");
    if (cc_wifi != -1)
        luna_update_classes(cc_wifi, "on", g_settings.wifi_enabled ? "on" : NULL);
    int k = luna_get_element_by_id("cc_wifi_knob");
    if (k != -1) {
        luna_element_at(k)->rel_x = g_settings.wifi_enabled ? 21.0f : 3.0f;
        luna_element_at(k)->pos_overridden_x = 1;
    }
    int cc_bt = luna_get_element_by_id("cc_bt");
    if (cc_bt != -1)
        luna_update_classes(cc_bt, "on", g_settings.bluetooth_enabled ? "on" : NULL);
    k = luna_get_element_by_id("cc_bt_knob");
    if (k != -1) {
        luna_element_at(k)->rel_x = g_settings.bluetooth_enabled ? 21.0f : 3.0f;
        luna_element_at(k)->pos_overridden_x = 1;
    }
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

typedef struct {
    int  (*start)(void);
    void (*get_fb_size)(int* w, int* h);
    void (*swap_buffers)(void);
    void (*poll_events)(void);
    void (*set_cursor)(int cursor_type);
    void (*terminate)(void);
} LunaBackend;

static const LunaBackend* g_backend = NULL;
static const LunaBackend g_wl_backend; /* forward declaration */

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
static void  plat_begin_move(void)      { /* shell root is not a movable toplevel */ }
static void  plat_begin_resize(int edge){ (void)edge; }
static void  plat_set_title(const char* title) { (void)title; }
static int   plat_system_notify(const char* app, int kind, const char* title, const char* message) {
    (void)app; (void)kind; toast_show(title ? title : "Luna", message ? message : "", 4.0); return 1;
}
static void  plat_cursor(int type) {
    if (g_backend && g_backend->set_cursor) g_backend->set_cursor(type);
}

/* Keep this in lockstep with luna-ui's screenshot request buffer so the path
 * we later verify is exactly the path the renderer writes. */
static char g_shell_screenshot_path[511] = {0};
static double g_shell_screenshot_requested_at = 0.0;

static int screenshot_mkdir_p(const char* path) {
    return mkdir_p_mode(path, 0700);
}

static int xdg_user_dir_lookup(char* out, size_t n, const char* key,
                               const char* fallback_leaf) {
    char path[PATH_MAX];
    if (!path_join2(path, sizeof(path), g_xdg.config_home, "user-dirs.dirs"))
        return 0;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[PATH_MAX + 128];
        size_t key_len = strlen(key);
        while (fgets(line, sizeof(line), f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, key, key_len) || p[key_len] != '=') continue;
            p += key_len + 1;
            while (*p == ' ' || *p == '\t') p++;
            char* end = p + strcspn(p, "\r\n");
            *end = 0;
            if (*p == '"' && end > p + 1 && end[-1] == '"') {
                p++;
                end[-1] = 0;
            }

            char expanded[PATH_MAX];
            size_t o = 0;
            const char* s = p;
            if (!strncmp(s, "$HOME", 5)) {
                int w = snprintf(expanded, sizeof(expanded), "%s", g_xdg.home);
                if (w < 0 || (size_t)w >= sizeof(expanded)) break;
                o = (size_t)w;
                s += 5;
            } else if (!strncmp(s, "${HOME}", 7)) {
                int w = snprintf(expanded, sizeof(expanded), "%s", g_xdg.home);
                if (w < 0 || (size_t)w >= sizeof(expanded)) break;
                o = (size_t)w;
                s += 7;
            }
            while (*s && o + 1 < sizeof(expanded)) {
                if (*s == '\\' && s[1]) s++;
                expanded[o++] = *s++;
            }
            expanded[o] = 0;
            if (path_is_absolute(expanded)) {
                snprintf(out, n, "%s", expanded);
                fclose(f);
                return 1;
            }
            break;
        }
        fclose(f);
    }
    return path_join2(out, n, g_xdg.home, fallback_leaf);
}

static void take_timestamped_screenshot(void) {
    char default_dir[PATH_MAX];
    char pictures_dir[PATH_MAX];
    char stamp[64];
    const char* dir = getenv("LUNA_SCREENSHOT_DIR");
    struct timespec ts;
    struct tm tm_info;

    if (!dir || !*dir) {
        if (!xdg_user_dir_lookup(pictures_dir, sizeof(pictures_dir),
                                 "XDG_PICTURES_DIR", "Pictures") ||
            !path_join2(default_dir, sizeof(default_dir), pictures_dir, "Screenshots")) {
            toast_show("Screenshot", "The screenshot path is too long.", 4.0);
            return;
        }
        dir = default_dir;
    }
    if (!screenshot_mkdir_p(dir)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot create %.220s", dir);
        toast_show("Screenshot failed", msg, 5.0);
        return;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);
    if (strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_info) == 0 ||
        snprintf(g_shell_screenshot_path, sizeof(g_shell_screenshot_path),
                 "%s/luna-%s-%03ld.png", dir, stamp,
                 ts.tv_nsec / 1000000L) >= (int)sizeof(g_shell_screenshot_path)) {
        g_shell_screenshot_path[0] = '\0';
        toast_show("Screenshot", "The screenshot path is too long.", 4.0);
        return;
    }

    /* Read pixels after the next completed render.  Reading here handles the
     * key event after the previous EGL swap, whose default buffer may already
     * be undefined (notably on X11). */
    luna_request_screenshot(g_shell_screenshot_path);
    g_shell_screenshot_requested_at = g_now;
    g_frame_dirty = 1;
}

static void finish_screenshot_notification(void) {
    if (!g_shell_screenshot_path[0]) return;
    if (access(g_shell_screenshot_path, F_OK) == 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "Saved to %.300s", g_shell_screenshot_path);
        fprintf(stderr, "[luna-shell] screenshot: %s\n", g_shell_screenshot_path);
        g_shell_screenshot_path[0] = '\0';
        g_shell_screenshot_requested_at = 0.0;
        toast_show("Screenshot saved", msg, 4.0);
        g_frame_dirty = 1;
    } else if (g_now - g_shell_screenshot_requested_at > 5.0) {
        fprintf(stderr, "[luna-shell] screenshot failed: %s\n",
                g_shell_screenshot_path);
        g_shell_screenshot_path[0] = '\0';
        g_shell_screenshot_requested_at = 0.0;
        toast_show("Screenshot failed", "Could not write the PNG file.", 5.0);
        g_frame_dirty = 1;
    }
}

/* Shared key-press routing (menu shortcuts), used by both backends —
 * this is the old on_key() body, minus the GLFWwindow* parameter. */
static void dispatch_key(int key, int scancode, int action, int mods) {
    shell_note_user_activity();
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
                    snprintf(cmd, sizeof(cmd), "close %" PRIu64, g_wins[i].id);
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
            if (is_shown(g_wifi_menu_idx))   { dismiss_wifi_menu();     return; }
            if (is_shown(g_bt_menu_idx))     { dismiss_bt_menu();       return; }
            if (is_shown(g_weather_menu_idx)){ dismiss_weather_menu();  return; }
            if (is_shown(g_calendar_menu_idx)){ dismiss_calendar_menu(); return; }
            if (is_shown(g_net_detail_idx))  { net_detail_close(NULL);  return; }
            if (is_shown(g_luna_menu_idx))   { dismiss_luna_menu(g_luna_menu_idx); return; }
            if (is_shown(g_cc_idx))          { dismiss_cc(g_cc_idx);    return; }
        }
        if (is_shown(g_weather_menu_idx) &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            weather_do_search();
            return;
        }
        /* Confirmation alerts expose a conventional default action.  This is
         * handled by the shell rather than relying on whichever button happened
         * to retain focus on the menubar surface that opened the dialog. */
        if (is_shown(g_confirm_idx) &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            on_confirm_ok(NULL);
            return;
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
        if (key == LUNA_KEY_PRINT_SCREEN || key == LUNA_KEY_F12) {
            take_timestamped_screenshot();
            return;
        }
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
        case XKB_KEY_Print:     return LUNA_KEY_PRINT_SCREEN;
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
    char rel[PATH_MAX], path[PATH_MAX];
    const char* xcursor = (!strcmp(theme, "builtin")) ? "aero" : theme;
    if (snprintf(rel, sizeof(rel), "icons/%s/cursors/left_ptr", xcursor) <
            (int)sizeof(rel) &&
        xdg_find_data_file(path, sizeof(path), rel))
        setenv("XCURSOR_THEME", xcursor, 1);
    luna_cur_theme_load(&g_cur_theme, theme);
}

static void cursor_theme_tick_and_refresh(void) {
    if (g_cursor_reload_pending) {
        g_cursor_reload_pending = 0;
        g_cursor_frame_changed = 1;
        if (g_backend && g_backend->set_cursor)
            g_backend->set_cursor(g_cur_theme.active_role);
        g_cursor_frame_changed = 0;
    }
    if (!g_cursor_present) return;
    /* Do not inject decorative cursor-surface commits into a drag/click
     * sequence.  Re-arm an expired deadline so poll() cannot busy-spin while
     * animation is deferred; resume from the current frame after the grace. */
    if (g_interaction_busy &&
        g_cur_theme.active_role >= 0 &&
        g_cur_theme.active_role < LUNA_CUR_MAX_ROLES) {
        LunaCurAnim* a = &g_cur_theme.roles[g_cur_theme.active_role];
        if (a->loaded && a->nframes > 1 && a->frame_until <= g_now) {
            int delay = a->frames[a->frame_i].delay_ms;
            if (delay < 1) delay = 1;
            a->frame_until = g_now + (double)delay / 1000.0;
        }
        return;
    }
    if (!luna_cur_theme_tick(&g_cur_theme, g_now)) return;
    /* Re-push the current role so animated .ani frames advance. */
    g_cursor_frame_changed = 1;
    if (g_backend && g_backend->set_cursor)
        g_backend->set_cursor(g_cur_theme.active_role);
    g_cursor_frame_changed = 0;
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
            shell_note_user_activity();
            luna_mouse_move(g_kms.mouse_x, g_kms.mouse_y);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            g_kms.mouse_x = libinput_event_pointer_get_absolute_x_transformed(p, g_kms.width);
            g_kms.mouse_y = libinput_event_pointer_get_absolute_y_transformed(p, g_kms.height);
            kms_cursor_move();
            shell_note_user_activity();
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
            shell_note_user_activity();
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
            shell_note_user_activity();
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
    glFinish();
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
    int state_fd = shell_state_watch_poll_fd();
    int async_fd = shell_async_poll_fd();
    double outer_deadline = plat_time() +
        (double)g_single_poll_timeout_ms / 1000.0;

    for (;;) {
        struct pollfd pfds[3] = {
            { .fd = g_kms.li ? libinput_get_fd(g_kms.li) : -1, .events = POLLIN },
            { .fd = state_fd, .events = POLLIN },
            { .fd = async_fd, .events = POLLIN },
        };
        int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
        int pr;
        do { pr = poll(pfds, 3, timeout_ms); }
        while (pr < 0 && errno == EINTR);

        int input_ready = pr > 0 && pfds[0].fd >= 0 &&
                          (pfds[0].revents & POLLIN);
        int state_ready = pr > 0 && state_fd >= 0 &&
                          (pfds[1].revents & POLLIN);
        int async_ready = pr > 0 && async_fd >= 0 &&
                          (pfds[2].revents & POLLIN);
        if (state_ready) {
            (void)shell_state_watch_drain();
            if (g_interaction_busy)
                outer_deadline = plat_time() + 1.0;
        }
        if (async_ready) shell_async_drain();

        int async_deferred = async_ready && g_interaction_busy;
        if (pr <= 0 || input_ready || (async_ready && !async_deferred)) break;
        if ((!state_ready && !async_deferred) ||
            shell_poll_coalesced_timeout_ms(outer_deadline) == 0)
            break;
        /* State/background-worker wake during interaction: keep sleeping. */
    }
    kms_process_input();
}
static void kms_backend_set_cursor(int cursor_type) {
    if (!g_kms.cursor_ok) return;
    if (!g_cursor_frame_changed &&
        cursor_type == g_kms.cursor_type && g_kms.cursor_shown) return;
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
    const char*                    input_root_id;     /* modeless input rectangle */
    uint32_t                       layer;
    uint32_t                       anchor;
    int32_t                        exclusive_zone;
    int32_t                        margin_top, margin_right, margin_bottom, margin_left;
    int                            fixed_w, fixed_h;   /* 0 = fill from anchor */
    int                            is_overlay;         /* map/unmap on demand */
    int                            is_kbd;             /* keyboard interactivity */
    /* runtime */
    int                            root_idx;
    int                            input_root_idx;
    int                            input_x, input_y, input_w, input_h;
    struct wl_surface*             wl_surf;
    struct zwlr_layer_surface_v1*  layer_surf;
    struct wl_egl_window*          egl_win;
    EGLSurface                     egl_surf;
    int                            configured;
    int                            surf_w, surf_h;      /* logical surface size */
    int                            buffer_scale;        /* integer Wayland output scale */
    struct wl_output*              output;
    float                          doc_x, doc_y;
    int                            was_shown;          /* previous is_shown() state */
    /* Force whole-surface damage on the next swap.  Set for the first frame
     * and after every resize, where the compositor's idea of this surface's
     * contents cannot be trusted to match our incremental record. */
    int                            full_damage;
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
    { .name="settings",     .root_id="settings_win", .input_root_id="settings_sheet",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="about",        .root_id="about_win", .input_root_id="about_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="confirm",      .root_id="confirm_overlay", .input_root_id="confirm_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    /* toast: small fixed surface, TOP|RIGHT anchored */
    { .name="toast",        .root_id="toast",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_RIGHT,
      .margin_top=38, .margin_right=14, .fixed_w=340, .fixed_h=76,
      .is_overlay=1 },
    /* wifi_menu / bt_menu: full-output overlays so position_menu_near works on Wayland */
    { .name="wifi_menu",    .root_id="wifi_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="bt_menu",      .root_id="bt_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="weather_menu", .root_id="weather_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="calendar_menu", .root_id="calendar_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="net_detail", .root_id="net_detail_win", .input_root_id="net_detail_box",
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

/* luna-window.h (and other LUNA_UI_NO_PLATFORM hosts) call this instead of a
 * GLFW/native platform backend. Mark every layer dirty so dialogs/toasts
 * appear on the next frame. */
void luna_app_request_redraw(void) {
    shell_request_repaint(-1);
}

/* True only when applying wallpaper-widget data will not introduce an extra
 * full-screen frame.  On Wayland, a dirty background layer or an already-live
 * wallpaper animation supplies that frame.  KMS/X11 use one framebuffer, so
 * any pending frame (or the animation cadence) can absorb the update. */
static int shell_bg_passive_refresh_ready(void) {
    if (g_backend == &g_wl_backend) {
        if (g_surf_dirty & (1u << LUNA_SURF_BG)) return 1;
        return g_bg_animated && !shell_desktop_busy();
    }
    return g_frame_dirty || g_bg_animated;
}

/* ── Global Wayland / EGL state ── */
#define LUNA_MAX_WL_OUTPUTS 8
typedef struct {
    uint32_t name;
    struct wl_output* obj;
    int scale;
} LunaWlOutput;

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
    uint32_t compositor_version;
    uint32_t seat_version;
    uint32_t layer_shell_version;
    LunaWlOutput outputs[LUNA_MAX_WL_OUTPUTS];
    int output_count;
    int default_output_scale;

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


static int wl_scale_override(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char* v = getenv("LUNA_OUTPUT_SCALE");
    int scale = (v && *v) ? atoi(v) : 0;
    if (scale < 1 || scale > 4) scale = 0;
    cached = scale;
    return cached;
}

static int wl_output_scale_for(struct wl_output* output) {
    int forced = wl_scale_override();
    if (forced) return forced;
    for (int i = 0; i < g_wl.output_count; i++)
        if (g_wl.outputs[i].obj == output)
            return g_wl.outputs[i].scale > 0 ? g_wl.outputs[i].scale : 1;
    return g_wl.default_output_scale > 0 ? g_wl.default_output_scale : 1;
}

static int surf_buffer_w(const LunaSurface* s) {
    int scale = s->buffer_scale > 0 ? s->buffer_scale : 1;
    return s->surf_w * scale;
}

static int surf_buffer_h(const LunaSurface* s) {
    int scale = s->buffer_scale > 0 ? s->buffer_scale : 1;
    return s->surf_h * scale;
}

static void surf_apply_buffer_scale(LunaSurface* s, int scale) {
    if (!s || !s->wl_surf) return;
    /* wl_surface.set_buffer_scale was added in wl_surface v3.  Binding an
     * older compositor is valid, but such a surface must stay scale 1. */
    uint32_t surface_version = wl_proxy_get_version((struct wl_proxy*)s->wl_surf);
    if (surface_version < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) scale = 1;
    int forced = wl_scale_override();
    if (forced && surface_version >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        scale = forced;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (s->buffer_scale == scale) return;
    s->buffer_scale = scale;
    if (surface_version >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(s->wl_surf, scale);
    if (s->egl_win && s->surf_w > 0 && s->surf_h > 0)
        wl_egl_window_resize(s->egl_win, surf_buffer_w(s), surf_buffer_h(s), 0, 0);
    s->full_damage = 1;
    shell_request_repaint((int)(s - g_surfs));
}

static void surface_enter_output(void* data, struct wl_surface* surface,
                                 struct wl_output* output) {
    (void)surface;
    LunaSurface* s = (LunaSurface*)data;
    s->output = output;
    surf_apply_buffer_scale(s, wl_output_scale_for(output));
}

static void surface_leave_output(void* data, struct wl_surface* surface,
                                 struct wl_output* output) {
    (void)surface;
    LunaSurface* s = (LunaSurface*)data;
    if (s->output == output) s->output = NULL;
}

static const struct wl_surface_listener g_wl_surface_listener = {
    .enter = surface_enter_output,
    .leave = surface_leave_output,
};

static void output_geometry(void* data, struct wl_output* output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char* make, const char* model,
                            int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)physical_width;
    (void)physical_height; (void)subpixel; (void)make; (void)model;
    (void)transform;
}

static void output_mode(void* data, struct wl_output* output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void* data, struct wl_output* output) {
    (void)data; (void)output;
}

static void output_scale(void* data, struct wl_output* output, int32_t factor) {
    LunaWlOutput* o = (LunaWlOutput*)data;
    int forced = wl_scale_override();
    if (forced > 0) factor = forced;
    if (factor < 1) factor = 1;
    if (factor > 4) factor = 4;
    o->scale = factor;
    if (o == &g_wl.outputs[0] || g_wl.default_output_scale < 1)
        g_wl.default_output_scale = factor;
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        if (s->output == output || (!s->output && g_wl.output_count == 1))
            surf_apply_buffer_scale(s, factor);
    }
}

static const struct wl_output_listener g_wl_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

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
    g_cursor_present = 1;
    g_pointer_surface = NULL;
    for (int i = 0; i < LUNA_SURF_COUNT; i++)
        if (g_surfs[i].wl_surf == surf) { g_pointer_surface = &g_surfs[i]; break; }
    g_wl.pointer_x = wl_fixed_to_double(x);
    g_wl.pointer_y = wl_fixed_to_double(y);
    wl_refresh_pointer_doc_pos();
    /* Commit the latest animation frame before assigning the cursor role.
     * While the pointer is outside Luna we deliberately keep animation
     * updates local, avoiding invisible cursor-surface commits. */
    wl_cursor_paint(g_cur_theme.active_role);
    shell_note_user_activity();
    luna_mouse_move(g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_leave(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* surf) {
    (void)d; (void)p; (void)s; (void)surf;
    g_pointer_surface = NULL;
    g_wl.pointer_entered = 0;
    g_cursor_present = 0;
    /* The compositor drops the old client's cursor role while changing
     * pointer focus.  Sending set_cursor(NULL) with the leave serial races the
     * following client's enter/set_cursor exchange and can replace GTK's
     * cursor (or the shell cursor on re-entry) with the fallback arrow. */
}
static void wlp_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    g_wl.pointer_x = wl_fixed_to_double(x);
    g_wl.pointer_y = wl_fixed_to_double(y);
    wl_refresh_pointer_doc_pos();
    /* set_cursor is needed on enter and when the glyph changes, not for every
     * motion event.  Avoiding a Wayland request per sample removes pointer
     * latency on high-polling-rate mice. */
    shell_note_user_activity();
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
    shell_note_user_activity();
    luna_mouse_button(btn, action, g_wl.mods, g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)t;
    double v = wl_fixed_to_double(value) / 10.0;
    shell_note_user_activity();
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

static void wl_pointer_drop(void) {
    if (!g_wl.pointer) return;
    uint32_t ver = wl_proxy_get_version((struct wl_proxy*)g_wl.pointer);
    if (ver >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(g_wl.pointer);
    else
        wl_proxy_destroy((struct wl_proxy*)g_wl.pointer);
    g_wl.pointer = NULL;
    g_pointer_surface = NULL;
    g_wl.pointer_entered = 0;
}

static void wl_keyboard_drop(void) {
    if (!g_wl.keyboard) return;
    uint32_t ver = wl_proxy_get_version((struct wl_proxy*)g_wl.keyboard);
    if (ver >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(g_wl.keyboard);
    else
        wl_proxy_destroy((struct wl_proxy*)g_wl.keyboard);
    g_wl.keyboard = NULL;
}

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
    if (!(caps & WL_SEAT_CAPABILITY_POINTER)) wl_pointer_drop();
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)) wl_keyboard_drop();
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
        wl_egl_window_resize(s->egl_win, surf_buffer_w(s), surf_buffer_h(s), 0, 0);
        s->full_damage = 1;
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
        uint32_t bver = ver < 4 ? ver : 4;
        g_wl.compositor_version = bver;
        g_wl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, bver);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        g_wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        uint32_t bver = ver < 5 ? ver : 5;
        g_wl.seat_version = bver;
        g_wl.seat = wl_registry_bind(reg, name, &wl_seat_interface, bver);
        wl_seat_add_listener(g_wl.seat, &g_wl_seat_listener, NULL);
    } else if (!strcmp(iface, wl_output_interface.name) &&
               g_wl.output_count < LUNA_MAX_WL_OUTPUTS) {
        LunaWlOutput* o = &g_wl.outputs[g_wl.output_count++];
        memset(o, 0, sizeof(*o));
        o->name = name;
        o->scale = 1;
        uint32_t bver = ver < 2 ? ver : 2;
        o->obj = wl_registry_bind(reg, name, &wl_output_interface, bver);
        wl_output_add_listener(o->obj, &g_wl_output_listener, o);
        if (g_wl.default_output_scale < 1) g_wl.default_output_scale = 1;
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        g_wl.wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wl.wm_base, &g_xdg_wm_base_listener, NULL);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t bver = ver < 4 ? ver : 4;
        g_wl.layer_shell_version = bver;
        g_wl.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, bver);
    }
}
static void wl_registry_global_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d; (void)reg;
    for (int i = 0; i < g_wl.output_count; i++) {
        if (g_wl.outputs[i].name != name) continue;
        struct wl_output* dead = g_wl.outputs[i].obj;
        for (int si = 0; si < LUNA_SURF_COUNT; si++)
            if (g_surfs[si].output == dead) g_surfs[si].output = NULL;
        if (dead) wl_output_destroy(dead);
        memmove(&g_wl.outputs[i], &g_wl.outputs[i + 1],
                (size_t)(g_wl.output_count - i - 1) * sizeof(g_wl.outputs[0]));
        g_wl.output_count--;
        break;
    }
}
static const struct wl_registry_listener g_wl_registry_listener = {
    .global = wl_registry_global, .global_remove = wl_registry_global_remove,
};

/* ── Create a single layer-shell surface ── */
static int surf_wl_create(LunaSurface* s) {
    s->wl_surf = wl_compositor_create_surface(g_wl.compositor);
    if (!s->wl_surf) return 0;
    wl_surface_add_listener(s->wl_surf, &g_wl_surface_listener, s);
    s->buffer_scale = 0;
    surf_apply_buffer_scale(s, g_wl.default_output_scale > 0
                                ? g_wl.default_output_scale : 1);
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
    uint32_t keyboard_mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    if (s->is_kbd) {
        /* ON_DEMAND was introduced in layer-shell v4.  On v1-v3 the closest
         * conforming behaviour is EXCLUSIVE; sending enum value 2 there is a
         * protocol error. */
        keyboard_mode = g_wl.layer_shell_version >= 4
            ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
            : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    }
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_surf, keyboard_mode);
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
    s->egl_win = wl_egl_window_create(s->wl_surf, surf_buffer_w(s), surf_buffer_h(s));
    if (!s->egl_win) return 0;
    s->egl_surf = eglCreateWindowSurface(g_wl.dpy, g_wl_egl_cfg,
                                          (EGLNativeWindowType)s->egl_win, NULL);
    if (s->egl_surf == EGL_NO_SURFACE) {
        wl_egl_window_destroy(s->egl_win); s->egl_win = NULL; return 0;
    }
    /* A brand new buffer chain shares nothing with what the compositor last
     * saw for this surface. */
    s->full_damage = 1;
    return 1;
}

/* EGL_EXT_swap_buffers_with_damage — resolved once, optional. */
typedef EGLBoolean (*PFN_eglSwapBuffersWithDamage)(EGLDisplay, EGLSurface, const EGLint*, EGLint);
static PFN_eglSwapBuffersWithDamage g_egl_swap_damage = NULL;

static void egl_swap_damage_init(void) {
    static int tried = 0;
    if (tried) return;
    tried = 1;
    g_egl_swap_damage =
        (PFN_eglSwapBuffersWithDamage)eglGetProcAddress("eglSwapBuffersWithDamageEXT");
    if (!g_egl_swap_damage)
        g_egl_swap_damage =
            (PFN_eglSwapBuffersWithDamage)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
    /* Always track per-element damage: even without the EGL extension it lets
     * us skip commits that would post an identical buffer (clock/CPU widgets
     * that did not actually change a pixel).  Partial swaps are a bonus. */
    luna_redraw_track_damage(1);
    fprintf(stderr, "[luna-shell/wl] partial-swap damage %s\n",
            g_egl_swap_damage ? "enabled" : "unavailable (identical-frame skip only)");
}

/* Post the frame, telling the compositor which part of it is actually new.
 * Returns what eglSwapBuffers would have returned. */
static EGLBoolean surf_swap(LunaSurface* s) {
    float dx, dy, dw, dh;
    int has_dmg = luna_redraw_damage_region(&dx, &dy, &dw, &dh);
    if (!s->full_damage && !has_dmg)
        return EGL_TRUE;   /* identical frame — skip the commit entirely */

    if (s->full_damage || !g_egl_swap_damage) {
        s->full_damage = 0;
        return eglSwapBuffers(g_wl.dpy, s->egl_surf);
    }
    s->full_damage = 0;

    int scale = s->buffer_scale > 0 ? s->buffer_scale : 1;
    int bw = surf_buffer_w(s), bh = surf_buffer_h(s);
    int x0 = (int)floorf(dx * scale), y0 = (int)floorf(dy * scale);
    int x1 = (int)ceilf((dx + dw) * scale);
    int y1 = (int)ceilf((dy + dh) * scale);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x1 <= x0 || y1 <= y0)
        return eglSwapBuffers(g_wl.dpy, s->egl_surf);
    /* EGL damage rects count y upward from the bottom of the pixel buffer. */
    EGLint rect[4] = { x0, bh - y1, x1 - x0, y1 - y0 };
    return g_egl_swap_damage(g_wl.dpy, s->egl_surf, rect, 1);
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
        /* Modeless utility windows accept pointer input only inside their
         * visible window.  The rest of the transparent layer surface remains
         * click-through, so applications below keep behaving normally. */
        if (s->input_root_idx < 0)
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

/* Push the current LunaSurface chrome fields to the live layer-shell object. */
static void surf_reconfigure_chrome(LunaSurface* s) {
    if (!s || !s->layer_surf || !s->wl_surf) return;
    zwlr_layer_surface_v1_set_anchor(s->layer_surf, s->anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surf, s->exclusive_zone);
    zwlr_layer_surface_v1_set_size(s->layer_surf,
                                   (uint32_t)(s->fixed_w > 0 ? s->fixed_w : 0),
                                   (uint32_t)(s->fixed_h > 0 ? s->fixed_h : 0));
    zwlr_layer_surface_v1_set_margin(s->layer_surf,
        s->margin_top, s->margin_right, s->margin_bottom, s->margin_left);
    wl_surface_commit(s->wl_surf);
    s->full_damage = 1;
}

/* Apply menubar edge / dock visibility from the active skin.  Works for the
 * Wayland multi-surface backend (layer anchors) and for single-framebuffer
 * backends (DOM classes + CSS). */
static void skin_apply_chrome(int skin_idx) {
    if (skin_idx < 0 || skin_idx >= g_skin_count) skin_idx = 0;
    const LunaSkin* skin = &g_skins[skin_idx];
    int edge = skin->menubar_edge;
    int height = skin->menubar_height > 0 ? skin->menubar_height : 32;
    int dock_hidden = (skin->dock_mode == SKIN_DOCK_HIDDEN);

    g_chrome_menubar_edge = edge;
    g_chrome_menubar_height = height;
    g_chrome_dock_hidden = dock_hidden;

    int mb_idx = luna_get_element_by_id("menubar");
    if (mb_idx >= 0) {
        luna_update_classes(mb_idx, "edge_bottom",
                            edge == SKIN_EDGE_BOTTOM ? "edge_bottom" : NULL);
    }
    int dock_idx = luna_get_element_by_id("dock");
    if (dock_idx >= 0) {
        set_hidden(dock_idx, dock_hidden);
    }
    luna_mark_layout_dirty();

    /* Wayland layer-shell surfaces are independent of CSS absolute insets. */
    if (g_surfs[LUNA_SURF_MENUBAR].layer_surf) {
        LunaSurface* mb = &g_surfs[LUNA_SURF_MENUBAR];
        LunaSurface* dock = &g_surfs[LUNA_SURF_DOCK];
        LunaSurface* toast = NULL;
        for (int i = 0; i < LUNA_SURF_COUNT; i++)
            if (g_surfs[i].root_id && !strcmp(g_surfs[i].root_id, "toast")) {
                toast = &g_surfs[i];
                break;
            }

        mb->fixed_h = height;
        mb->exclusive_zone = height;
        mb->fixed_w = 0;
        mb->margin_top = mb->margin_right = mb->margin_bottom = mb->margin_left = 0;
        if (edge == SKIN_EDGE_BOTTOM)
            mb->anchor = ZWLR_ANCHOR_BOTTOM | ZWLR_ANCHOR_LEFT | ZWLR_ANCHOR_RIGHT;
        else
            mb->anchor = ZWLR_ANCHOR_TOP | ZWLR_ANCHOR_LEFT | ZWLR_ANCHOR_RIGHT;
        surf_reconfigure_chrome(mb);

        if (dock_hidden) {
            dock->exclusive_zone = 0;
            dock->margin_top = dock->margin_right = dock->margin_bottom = dock->margin_left = 0;
            /* Keep a 1×1 mapped surface to avoid fighting configure races, but
             * unmap so it neither paints nor reserves an exclusive zone. */
            if (dock->egl_surf != EGL_NO_SURFACE || dock->was_shown) {
                dock->was_shown = 0;
                surf_set_shown(dock, 0);
            }
            surf_reconfigure_chrome(dock);
        } else {
            dock->anchor = ZWLR_ANCHOR_BOTTOM;
            dock->exclusive_zone = 92;
            dock->margin_top = 0;
            dock->margin_right = 0;
            dock->margin_bottom = 12;
            dock->margin_left = 0;
            dock->fixed_w = 542;
            dock->fixed_h = 80;
            surf_reconfigure_chrome(dock);
            if (dock->egl_surf == EGL_NO_SURFACE) {
                dock->was_shown = 1;
                surf_set_shown(dock, 1);
            }
        }

        if (toast) {
            toast->margin_top = (edge == SKIN_EDGE_BOTTOM) ? 14 : (height + 6);
            toast->margin_right = 14;
            surf_reconfigure_chrome(toast);
        }
        shell_request_repaint(-1);
        if (g_wl.display) wl_display_flush(g_wl.display);
    }
    skin_apply_wm_decoration(skin_idx);
    skin_export_window_theme(skin_idx);
}

static void surf_update_input_region(LunaSurface* s) {
    if (!s->wl_surf || !s->was_shown || s->input_root_idx < 0) return;
    LunaElement* box = luna_element_at(s->input_root_idx);
    if (!box) return;
    int x = (int)floorf(box->x - s->doc_x);
    int y = (int)floorf(box->y - s->doc_y);
    int w = (int)ceilf(box->w);
    int h = (int)ceilf(box->h);
    if (w <= 0 || h <= 0) return;
    if (x == s->input_x && y == s->input_y && w == s->input_w && h == s->input_h)
        return;
    struct wl_region* region = wl_compositor_create_region(g_wl.compositor);
    if (!region) return;
    wl_region_add(region, x, y, w, h);
    wl_surface_set_input_region(s->wl_surf, region);
    wl_region_destroy(region);
    wl_surface_commit(s->wl_surf);
    s->input_x = x; s->input_y = y; s->input_w = w; s->input_h = h;
}

/* A surface only takes part in the frame loop when it has something to show. */
static int surf_is_live(const LunaSurface* s) {
    return s->egl_surf != EGL_NO_SURFACE && (!s->is_overlay || s->was_shown);
}

static int wl_backend_start(void) {
    /* No Luna layer owns the pointer until wl_pointer.enter arrives. */
    g_cursor_present = 0;
    int forced_scale = wl_scale_override();
    g_wl.default_output_scale = forced_scale > 0 ? forced_scale : 1;
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
    egl_swap_damage_init();

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
    /* Probe before touching GL: a clock/stats tick often marks the wallpaper
     * dirty even though every element's paint hash is unchanged.  Skipping the
     * clear/draw/swap removes the periodic hitch those timers produced. */
    if (!s->full_damage &&
        !luna_redraw_probe_region(s->root_idx, surf_buffer_w(s), surf_buffer_h(s),
                                  s->doc_x, s->doc_y,
                                  (float)s->surf_w, (float)s->surf_h)) {
        if (surf_idx >= 0 && surf_idx < LUNA_SURF_COUNT)
            g_surf_dirty &= ~(1u << surf_idx);
        return;
    }
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
    /* The layer-shell configure size is logical.  Render into a buffer scaled
     * to the output so text and vector edges land on native display pixels. */
    int fw = surf_buffer_w(s), fh = surf_buffer_h(s);
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
                           (float)s->surf_w, (float)s->surf_h);
    } else {
        /* bg: no root filter, full-document render */
        luna_render(fw, fh);
    }
    /* llvmpipe / mesa_glthread can still be writing the color buffer when
     * eglSwapBuffers returns.  The compositor CPU-mmaps that wl_shm on
     * commit; without a finish the first frame is only glClear, and the
     * dirty-probe then skips every later swap — black wallpaper, cursor. */
    glFinish();
    if (!surf_swap(s)) {
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
            if (shown) s->input_w = s->input_h = -1;
            surf_set_shown(s, shown);
            if (shown) shell_request_repaint(i);
        }
        if (shown) surf_update_input_region(s);
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
    /* State-file writes can arrive at pointer-report frequency while another
     * client is moved.  Keep those state-only wakes inside this function so
     * they do not run Luna's full update/settling/surface scan each time. */
    int state_fd = shell_state_watch_poll_fd();
    int async_fd = shell_async_poll_fd();
    double outer_deadline = plat_time() +
        (double)g_wl_poll_timeout_ms / 1000.0;

    for (;;) {
        while (wl_display_prepare_read(g_wl.display) != 0) {
            int dispatched = wl_display_dispatch_pending(g_wl.display);
            if (dispatched < 0) {
                wl_check_error("event dispatch");
                return;
            }
            if (dispatched > 0) return;
        }
        wl_display_flush(g_wl.display);

        struct pollfd pfds[3] = {
            { .fd = wl_display_get_fd(g_wl.display), .events = POLLIN },
            { .fd = state_fd, .events = POLLIN },
            { .fd = async_fd, .events = POLLIN },
        };
        int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
        int pr;
        do { pr = poll(pfds, 3, timeout_ms); }
        while (pr < 0 && errno == EINTR);
        if (pr < 0) {
            wl_display_cancel_read(g_wl.display);
            wl_check_error("event poll");
            return;
        }

        int wl_ready = pr > 0 && (pfds[0].revents & POLLIN);
        int state_ready = pr > 0 && state_fd >= 0 &&
                          (pfds[1].revents & POLLIN);
        int async_ready = pr > 0 && async_fd >= 0 &&
                          (pfds[2].revents & POLLIN);

        if (wl_ready) {
            if (wl_display_read_events(g_wl.display) < 0) {
                wl_check_error("event read");
                return;
            }
        } else {
            wl_display_cancel_read(g_wl.display);
        }
        if (state_ready) {
            (void)shell_state_watch_drain();
            if (g_interaction_busy)
                outer_deadline = plat_time() + 1.0;
        }
        if (async_ready) shell_async_drain();

        if (wl_ready) {
            if (wl_display_dispatch_pending(g_wl.display) < 0 ||
                wl_display_flush(g_wl.display) < 0)
                wl_check_error("event dispatch");
            return;
        }
        int async_deferred = async_ready && g_interaction_busy;
        if ((async_ready && !async_deferred) || pr == 0) return;
        if ((!state_ready && !async_deferred) ||
            shell_poll_coalesced_timeout_ms(outer_deadline) == 0)
            return;
        /* State/background-worker wake before the debounce/idle deadline:
         * continue waiting instead of running a frame during interaction. */
    }
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
        if (g_wl.pointer_entered && g_wl.cursor_surf && g_wl.cursor_buf) {
            /* Assign first, then commit.  On first enter the compositor can
             * remember this as a pending cursor and classify the following
             * buffer commit as cursor-only damage. */
            wl_cursor_apply();
            wl_surface_attach(g_wl.cursor_surf, g_wl.cursor_buf, 0, 0);
            wl_surface_damage(g_wl.cursor_surf, 0, 0, WL_CURSOR_SIZE, WL_CURSOR_SIZE);
            wl_surface_commit(g_wl.cursor_surf);
        }
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

    char path[PATH_MAX];
    if (!path_join2(path, sizeof(path), g_xdg.runtime_dir,
                    "luna-cursor-XXXXXX")) {
        fprintf(stderr, "[luna-shell/wl] cursor runtime path is too long\n");
        return 0;
    }
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
    if (g_cursor_frame_changed || cursor_type != g_wl.cursor_type)
        wl_cursor_paint(cursor_type);
    else
        wl_cursor_apply();
}

static void wl_backend_terminate(void) {
    wl_cursor_fini();
    wl_pointer_drop();
    wl_keyboard_drop();
    for (int i = 0; i < LUNA_SURF_COUNT; i++) surf_destroy(&g_surfs[i]);
    if (g_wl.ctx != EGL_NO_CONTEXT)       eglDestroyContext(g_wl.dpy, g_wl.ctx);
    if (g_wl.dpy != EGL_NO_DISPLAY)       eglTerminate(g_wl.dpy);
    if (g_wl.xkb_state)  xkb_state_unref(g_wl.xkb_state);
    if (g_wl.xkb_keymap) xkb_keymap_unref(g_wl.xkb_keymap);
    if (g_wl.xkb_ctx)    xkb_context_unref(g_wl.xkb_ctx);
    for (int i = 0; i < g_wl.output_count; i++)
        if (g_wl.outputs[i].obj) wl_output_destroy(g_wl.outputs[i].obj);
    g_wl.output_count = 0;
    if (g_wl.seat) {
        uint32_t ver = wl_proxy_get_version((struct wl_proxy*)g_wl.seat);
        if (ver >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(g_wl.seat);
        else
            wl_proxy_destroy((struct wl_proxy*)g_wl.seat);
        g_wl.seat = NULL;
    }
    if (g_wl.wm_base) {
        xdg_wm_base_destroy(g_wl.wm_base);
        g_wl.wm_base = NULL;
    }
    if (g_wl.layer_shell) {
        zwlr_layer_shell_v1_destroy(g_wl.layer_shell);
        g_wl.layer_shell = NULL;
    }
    if (g_wl.shm) {
        wl_proxy_destroy((struct wl_proxy*)g_wl.shm);
        g_wl.shm = NULL;
    }
    if (g_wl.compositor) {
        wl_proxy_destroy((struct wl_proxy*)g_wl.compositor);
        g_wl.compositor = NULL;
    }
    if (g_wl.registry) {
        wl_proxy_destroy((struct wl_proxy*)g_wl.registry);
        g_wl.registry = NULL;
    }
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
            shell_note_user_activity();
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
            shell_note_user_activity();
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
    /* XPending is non-blocking.  State-only inotify wakes are coalesced here so
     * compositor geometry writes do not repeatedly enter the full frame loop. */
    if (!XPending(g_x11.display)) {
        int state_fd = shell_state_watch_poll_fd();
        int async_fd = shell_async_poll_fd();
        double outer_deadline = plat_time() +
            (double)g_single_poll_timeout_ms / 1000.0;
        for (;;) {
            struct pollfd pfds[3] = {
                { .fd = ConnectionNumber(g_x11.display), .events = POLLIN },
                { .fd = state_fd, .events = POLLIN },
                { .fd = async_fd, .events = POLLIN },
            };
            int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
            int pr;
            do { pr = poll(pfds, 3, timeout_ms); }
            while (pr < 0 && errno == EINTR);

            int x_ready = pr > 0 && (pfds[0].revents & POLLIN);
            int state_ready = pr > 0 && state_fd >= 0 &&
                              (pfds[1].revents & POLLIN);
            int async_ready = pr > 0 && async_fd >= 0 &&
                              (pfds[2].revents & POLLIN);
            if (state_ready) {
                (void)shell_state_watch_drain();
                if (g_interaction_busy)
                    outer_deadline = plat_time() + 1.0;
            }
            if (async_ready) shell_async_drain();

            int async_deferred = async_ready && g_interaction_busy;
            if (pr <= 0 || x_ready || (async_ready && !async_deferred)) break;
            if ((!state_ready && !async_deferred) ||
                shell_poll_coalesced_timeout_ms(outer_deadline) == 0)
                break;
        }
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
        else if (!strcmp(argv[i], "--skin") && i + 1 < argc)
            g_skin_override = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            luna_request_screenshot(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "luna-shell " LUNA_SHELL_VERSION " — Luna Desktop shell\n"
                "usage: luna-shell [--desktop] [--fullscreen] [--size WxH]\n"
                "                  [--skin NAME] [--layout PATH] [--css PATH]\n"
                "                  [--screenshot PATH]\n"
                "  backend: auto-selected — Wayland/EGL if WAYLAND_DISPLAY is set,\n"
                "           X11/EGL if DISPLAY is set (requires -DLUNA_BACKEND_X11 build),\n"
                "           otherwise KMS/DRM bare console\n"
                "  env: LUNA_DESKTOP_SKIN — named skin from a discovered skins directory\n"
                "       LUNA_SKIN_PATH — additional directory containing skin folders\n"
                "       LUNA_DESKTOP_LAYOUT / LUNA_DESKTOP_CSS — external layout override\n"
                "       LUNA_APP_<NAME>=<cmd> — override dock/launchpad app commands\n"
                "         <cmd> may be desktop:org.example.App.desktop for XDG launching\n"
                "       LUNA_SEAT — seat name for the KMS/libinput backend (default seat0)\n"
                "       LUNA_CURSOR_THEME — one-shot cursor theme override (GUI/settings normally win)\n"
                "       LUNA_CURSOR_PATH — directory of theme folders, or a theme dir itself\n"
                "       LUNA_SCREENSHOT_DIR — screenshot directory (default: XDG Pictures/Screenshots)\n"
                "       LUNA_IM_WAYLAND=0 — keep GTK_IM_MODULE=gim (default: wayland → whiz-im-wayland)\n"
                "       LUNA_INPUT_METHOD / LUNA_CLIPBOARD — helper cmds (or 'none' to skip)\n"
                "       LUNA_NO_HELPERS=1 — do not auto-start IME / clipboard manager\n"
                "       LUNA_NO_XDG_AUTOSTART=1 — skip XDG autostart .desktop entries\n"
                "       LUNA_XDG_AUTOSTART=1 — force autostart outside a Luna desktop session\n"
                "       LUNA_OUTPUT_SCALE=1..4 — override Wayland output buffer scale\n"
                "       XKB_DEFAULT_LAYOUT / VARIANT / OPTIONS — keyboard layout (e.g. jp,us)\n"
                "       --size is ignored by the KMS backend, which always uses the\n"
                "       display's native mode\n"
                "  keys: Super/F4 — Launchpad, Esc — close overlay, Alt+Tab — switch apps\n"
                "        Super+Arrows — tile/max/min, Super+D — show desktop, Alt+F4 — close window\n"
                "        Cmd+, — Settings, Print Screen/F12 — screenshot\n"
                "  settings: $XDG_CONFIG_HOME/luna-shell/settings.conf\n"
                "  session:  $XDG_STATE_HOME/luna-shell/session\n");
            exit(0);
        }
    }
    if (!g_layout_path) g_layout_path = getenv("LUNA_DESKTOP_LAYOUT");
    if (!g_css_path)    g_css_path    = getenv("LUNA_DESKTOP_CSS");
    if (!g_skin_override) g_skin_override = getenv("LUNA_DESKTOP_SKIN");
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
        setenv("LP_NUM_THREADS", "4", 0);
        /* mesa_glthread can return from eglSwapBuffers before llvmpipe has
         * written the wl_shm pixels.  The compositor then presents glClear. */
        setenv("mesa_glthread", "false", 0);
    }
}

int main(int argc, char** argv) {
    luna_window_width  = 1440.0f;
    luna_window_height = 900.0f;
    install_sigchld_handler();
    parse_args(argc, argv);
    xdg_paths_init();
    settings_load();
    skin_discover();
    if (g_skin_override && *g_skin_override)
        snprintf(g_settings.skin, sizeof(g_settings.skin), "%s", g_skin_override);
    int startup_skin = skin_find(g_settings.skin);
    if (startup_skin == 0 && strcmp(g_settings.skin, "default"))
        snprintf(g_settings.skin, sizeof(g_settings.skin), "default");
    if (!g_layout_path && startup_skin > 0 && g_skins[startup_skin].layout[0])
        g_layout_path = g_skins[startup_skin].layout;
    /* The built-in default skin is embedded.  An installed
     * session still has PREFIX/share/luna-desktop/shell/luna-shell.html;
     * falling back to the embedded copy used cwd-relative "ui/" for CSS
     * and painted only glClear — black wallpaper, invisible menubar/dock. */
    {
        static char layout_buf[PATH_MAX];
        static char css_buf[PATH_MAX];
        if (!g_layout_path &&
            xdg_find_data_file(layout_buf, sizeof(layout_buf),
                               "luna-desktop/shell/luna-shell.html"))
            g_layout_path = layout_buf;
        if (!g_css_path &&
            xdg_find_data_file(css_buf, sizeof(css_buf),
                               "luna-desktop/shell/luna-shell.css"))
            g_css_path = css_buf;
    }
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

    /* Start standard desktop-session entries only after the display backend
     * is known to be usable.  Luna's built-in IME/clipboard fallbacks are
     * suppressed when matching autostart entries were found. */
    xdg_autostart_run();
    ensure_wayland_helpers();

    LunaPlatform plat = {
        .get_time        = plat_time,
        .get_proc        = plat_proc,
        .set_cursor      = plat_cursor,
        .request_close   = plat_close,
        .iconify         = plat_iconify,
        .maximize_toggle = plat_maximize,
        .begin_move      = plat_begin_move,
        .begin_resize    = plat_begin_resize,
        .set_title       = plat_set_title,
        .system_notify   = plat_system_notify,
        .load_font       = plat_load_font,
        .struct_size     = sizeof(LunaPlatform),
        .api_version     = LUNA_UI_API_VERSION,
    };
    luna_set_platform(&plat);

    LunaInitConfig cfg = { luna_window_width, luna_window_height, plat_proc, 0 };
    if (!luna_init(&cfg)) {
        fprintf(stderr, "[luna-shell] luna_init failed — check GL context version\n");
        g_backend->terminate();
        return 1;
    }

    int loaded = 0;
    if (g_layout_path) {
        luna_set_html_base_dir(g_layout_path);
        loaded = luna_load_html_file(g_layout_path);
    }
    if (!loaded) {
        fprintf(stderr, "[luna-shell] using embedded layout (no luna-desktop/shell/luna-shell.html)\n");
        luna_set_html_base_dir("skins/default");
        luna_parse_html(default_html);
    } else {
        fprintf(stderr, "[luna-shell] layout %s\n", g_layout_path);
    }
    /* Authoring contract: layout.html may <link> ../default/style.css and
     * style.css so a browser preview matches the running shell.  When those
     * links load successfully, trust that cascade as-is.  --css still wins
     * and rebuilds the sheet; otherwise fall back to the embedded base and
     * append the named skin's style.css when the document did not link it. */
    if (g_css_path) {
        luna_reset_css();
        if (!luna_load_css_file(g_css_path)) luna_parse_css(default_css);
        if (startup_skin > 0 && !luna_load_css_file(g_skins[startup_skin].css)) {
            fprintf(stderr, "[luna-shell] skin '%s' could not load; using Luna Moonlight\n",
                    g_settings.skin);
            snprintf(g_settings.skin, sizeof(g_settings.skin), "default");
        }
    } else if (luna_css_from_document) {
        /* layout.html already linked base (+ usually its own style.css).
         * Only append the selected skin when the open layout is not that
         * skin's file (e.g. --layout pointing at another theme's HTML). */
        int layout_is_this_skin = startup_skin > 0 && g_layout_path &&
            g_skins[startup_skin].layout[0] &&
            !strcmp(g_layout_path, g_skins[startup_skin].layout);
        if (startup_skin > 0 && !layout_is_this_skin &&
            !luna_load_css_file(g_skins[startup_skin].css)) {
            fprintf(stderr, "[luna-shell] skin '%s' could not load; using Luna Moonlight\n",
                    g_settings.skin);
            snprintf(g_settings.skin, sizeof(g_settings.skin), "default");
        }
    } else {
        luna_parse_css(default_css);
        if (startup_skin > 0 && !luna_load_css_file(g_skins[startup_skin].css)) {
            fprintf(stderr, "[luna-shell] skin '%s' could not load; using Luna Moonlight\n",
                    g_settings.skin);
            snprintf(g_settings.skin, sizeof(g_settings.skin), "default");
        }
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
        for (int i = 0; i < LUNA_SURF_COUNT; i++) {
            g_surfs[i].root_idx = g_surfs[i].root_id
                ? luna_get_element_by_id(g_surfs[i].root_id)
                : -1;
            g_surfs[i].input_root_idx = g_surfs[i].input_root_id
                ? luna_get_element_by_id(g_surfs[i].input_root_id)
                : -1;
        }
    }
    /* Move the menubar / dock layer anchors to match the selected skin
     * (e.g. Windows XP taskbar at the bottom).  Must run after the layer
     * surfaces exist and the DOM ids have been bound. */
    skin_apply_chrome(startup_skin);
    skin_apply_toolkit(startup_skin);
    luna_set_mouse_release_hook(on_mouse_release_hook);
    fill_about_info();
    apply_wallpaper(g_settings.wallpaper);
    cursor_theme_reload(g_settings.cursor_theme);
    apply_keyboard_layout(g_settings.kb_layout);
    apply_ui_language(g_settings.ui_language);
    apply_wm_settings();

    if (!shell_async_init())
        fprintf(stderr, "[luna-shell] warning: cannot create async eventfd\n");
    LunaMonitorConfig monitor_cfg = {
        .state_path = g_shell_state_path,
        .status_interval_sec = 2.0,
        .notify = shell_async_notify,
        .notify_user = NULL,
    };
    if (!luna_monitor_init(&monitor_cfg))
        fprintf(stderr, "[luna-shell] warning: background monitor unavailable\n");

    char weather_agent[96];
    snprintf(weather_agent, sizeof(weather_agent), "luna-shell/%s", LUNA_SHELL_VERSION);
    LunaWeatherConfig weather_cfg = {
        .initial_city = NULL,
        .user_agent = weather_agent,
        .refresh_interval_sec = WEATHER_REFRESH_SEC,
        .notify = shell_async_notify,
        .notify_user = NULL,
        .child_lock = shell_child_reaper_lock,
        .child_unlock = shell_child_reaper_unlock,
        .child_user = NULL,
    };
    g_weather_worker_ready = luna_weather_init(&weather_cfg);
    if (!g_weather_worker_ready)
        fprintf(stderr, "[luna-shell] warning: weather worker unavailable\n");

    LunaWifiConfig wifi_cfg = {
        .notify = shell_async_notify,
        .notify_user = NULL,
    };
    if (!luna_wifi_init(&wifi_cfg)) {
        g_wifi_service_available = 0;
        g_wifi_busy = 0;
        snprintf(g_wifi_error, sizeof(g_wifi_error), "Wi-Fi worker unavailable");
        fprintf(stderr, "[luna-shell] warning: Wi-Fi worker unavailable\n");
    } else {
        g_wifi_service_available = 1;
        g_wifi_busy = 1;
        g_last_wifi_request = plat_time();
        if (!luna_wifi_request_set_powered(g_settings.wifi_enabled))
            fprintf(stderr, "[luna-shell] warning: cannot restore saved Wi-Fi state\n");
    }

    LunaEthernetConfig eth_cfg = {
        .notify = shell_async_notify,
        .notify_user = NULL,
    };
    if (!luna_ethernet_init(&eth_cfg)) {
        g_eth_service_available = 0;
        g_eth_busy = 0;
        snprintf(g_eth_error, sizeof(g_eth_error), "Ethernet worker unavailable");
        fprintf(stderr, "[luna-shell] warning: Ethernet worker unavailable\n");
    } else {
        g_eth_service_available = 1;
        g_eth_busy = 1;
        g_last_eth_request = plat_time();
    }

    LunaBluetoothConfig bt_cfg = {
        .notify = shell_async_notify,
        .notify_user = NULL,
    };
    if (!luna_bluetooth_init(&bt_cfg)) {
        g_bt_service_available = 0;
        g_bt_busy = 0;
        snprintf(g_bt_error, sizeof(g_bt_error), "Bluetooth worker unavailable");
        fprintf(stderr, "[luna-shell] warning: Bluetooth worker unavailable\n");
    } else {
        g_bt_service_available = 1;
        g_bt_busy = 1;
        g_last_bt_request = plat_time();
        if (!luna_bluetooth_request_set_powered(g_settings.bluetooth_enabled))
            fprintf(stderr, "[luna-shell] warning: cannot restore saved Bluetooth state\n");
    }

    g_now = plat_time();
    toast_show("Welcome to Luna", "Your desktop is ready.", 8.0);
    session_restore_schedule();

    /* Debug: LUNA_DEBUG_LAUNCH=sakura,pcmanfm forces dock launches once the
     * shell is up — used to capture child abort env without clicking the dock. */
    {
        const char* dbg = getenv("LUNA_DEBUG_LAUNCH");
        if (dbg && *dbg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", dbg);
            for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                while (*tok == ' ') tok++;
                for (int i = 0; i < APP_COUNT; i++) {
                    if (!strcmp(g_apps[i].key, tok) || !strcmp(g_apps[i].default_cmd, tok) ||
                        !strcmp(g_apps[i].cmd, tok)) {
                        fprintf(stderr, "[luna-shell] LUNA_DEBUG_LAUNCH → %s\n", g_apps[i].key);
                        app_launch(&g_apps[i]);
                        break;
                    }
                }
            }
        }
    }
    weather_request(g_settings.weather_city);

    double last    = g_now;
    int    prev_ww = 0, prev_wh = 0;
    LunaSliderIds bright_slider = {
        .thumb = -1, .fill = -1, .track = -1,
        .level = &g_bright_level,
        .available = &g_bright_available,
        .last_apply = &g_last_bright_apply,
        .last_written = &g_last_bright_written,
        .write_fn = brightness_write,
        .kind = "brightness",
    };
    LunaSliderIds vol_slider = {
        .thumb = -1, .fill = -1, .track = -1,
        .level = &g_vol_level,
        .available = &g_vol_available,
        .last_apply = &g_last_vol_apply,
        .last_written = &g_last_vol_written,
        .write_fn = volume_write,
        .kind = "volume",
    };
    slider_resolve(&bright_slider, "bright_thumb", "bright_fill", "bright_track");
    slider_resolve(&vol_slider, "vol_thumb", "vol_fill", "vol_track");
    g_bright_slider_ptr = &bright_slider;
    g_vol_slider_ptr = &vol_slider;

    while (!g_should_close) {
        g_now = plat_time();
        double dt = g_now - last;
        last = g_now;
        if (dt < 0.0) dt = 0.0;
        if (dt > LUNA_MAX_FRAME_DT) dt = LUNA_MAX_FRAME_DT;

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
        if (g_sigchld_pending) {
            g_sigchld_pending = 0;
            reap_children();
        }
        int ui_dragging = luna_pointer_dragging();
        /* The compositor state stream is needed to detect external window
         * motion, but an internal Luna drag should not be interrupted by file
         * parsing or worker-result application. */
        if (!ui_dragging) poll_shell_state();
        int recent_user_activity =
            g_now - g_last_user_activity < INTERACTION_IDLE_GRACE_SEC;
        int interaction_busy = ui_dragging ||
            g_now < g_window_motion_busy_until || recent_user_activity;
        g_interaction_busy = interaction_busy;

        /* Clock/status, Wi-Fi, weather and session maintenance are not latency
         * critical.  Applying them in the middle of a drag used to add a
         * repeatable long frame whenever their timers fired.  Workers continue
         * running and their latest snapshots are consumed immediately after the
         * interaction ends. */
        if (!interaction_busy) {
            update_async_status();
            wifi_tick();
            weather_tick();
            update_launchpad_filter();
            wm_settings_retry_tick();
            session_restore_tick();
        }
        slider_tick_when_needed(&bright_slider);
        slider_tick_when_needed(&vol_slider);
        cursor_theme_tick_and_refresh();
        if (!interaction_busy && g_toast_deadline > 0.0 && g_now > g_toast_deadline) {
            set_hidden(g_toast_idx, 1);
            g_toast_deadline = 0.0;
        }

        /* Does the wallpaper actually animate? A still wallpaper needs no
         * timed repaint. The update/settling pass below is selected per backend
         * so animation interpolation and settling classification share one scan. */
        if (g_backend == &g_wl_backend) {
            int surf_roots[LUNA_SURF_COUNT];
            for (int i = 0; i < LUNA_SURF_COUNT; i++)
                surf_roots[i] = surf_is_live(&g_surfs[i]) ? g_surfs[i].root_idx : INT_MAX;
            unsigned redraw_flags[LUNA_SURF_COUNT];
            (void)luna_needs_redraw_mask(g_now, dt, surf_roots,
                                         LUNA_SURF_COUNT, redraw_flags);
            int settling = 0;
            for (int i = 0; i < LUNA_SURF_COUNT; i++)
                if ((redraw_flags[i] & (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) ==
                    (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) settling = 1;
            g_bg_animated =
                (redraw_flags[LUNA_SURF_BG] & LUNA_REDRAW_ANIM) != 0;
            wl_surfs_update();
            /* Wallpaper aurora/stars: only damage the bg layer when the
             * desktop is empty. Continuous full-screen commits under open
             * windows force a whole-desktop re-composite every tick → flicker
             * and starve client frame callbacks.  Twelve hertz keeps the idle
             * animation smooth enough without repainting beneath
             * application windows. */
            int desktop_busy = shell_desktop_busy();
            int bg_ticking = g_bg_animated && !desktop_busy && !interaction_busy;
            if (bg_ticking &&
                g_now - g_last_bg_paint >= LUNA_WL_BG_FRAME_SEC)
                shell_request_repaint(LUNA_SURF_BG);
            /* Per-surface settling — dock mag must not redraw the menubar.
             * One pass answers for every surface at once; asking per surface
             * meant a full element scan (with a parent-chain walk per element)
             * a dozen times on every single frame, idle or not. */
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                if (redraw_flags[i] & LUNA_REDRAW_PAINT)
                    shell_request_repaint(i);
            }
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                wl_surf_render(&g_surfs[i], i);
            }
            /* Interactive easing is capped at roughly 60 fps.  When idle,
             * sleep until input or the next real shell/background deadline. */
            g_wl_poll_timeout_ms = settling
                ? shell_wait_timeout_ms(17, g_now + 1.0 / 60.0)
                : shell_wait_timeout_ms(1000,
                      bg_ticking
                          ? g_last_bg_paint + LUNA_WL_BG_FRAME_SEC
                          : 0.0);
        } else {
            int redraw_flags = luna_needs_redraw(g_now, dt);
            int settling =
                (redraw_flags & (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) ==
                (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT);
            g_bg_animated = (redraw_flags & LUNA_REDRAW_ANIM) != 0;
            /* The integrated update result is also the complete redraw decision
             * for the single KMS/X11 framebuffer. */
            /* Pointer/hover easing runs at vblank cadence, while an idle CSS
             * wallpaper runs at 12 Hz.  The KMS hardware cursor remains
             * full-rate without repainting the
             * primary plane, saving GPU work and memory bandwidth.  A still
             * wallpaper is entirely event-driven: the old two-second "safety"
             * repaint was a periodic full-document render + KMS page flip and
             * was visible as a regular hitch even though no pixel changed. */
            double idle_frame_deadline = g_bg_animated && !interaction_busy
                ? g_last_bg_paint + LUNA_SINGLE_BG_FRAME_SEC
                : 0.0;
            if (idle_frame_deadline > 0.0 && g_now >= idle_frame_deadline)
                g_frame_dirty = 1;
            if (g_frame_dirty || (redraw_flags & LUNA_REDRAW_PAINT)) {
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
                finish_screenshot_notification();
                g_single_poll_timeout_ms = settling ? 0
                    : shell_wait_timeout_ms(1000, g_bg_animated
                          ? g_last_bg_paint + LUNA_SINGLE_BG_FRAME_SEC
                          : 0.0);
            } else {
                g_single_poll_timeout_ms = shell_wait_timeout_ms(1000, idle_frame_deadline);
            }
        }
        /* Wayland surfaces perform their own swap in wl_surf_render(); keep
         * the existing post-render capture point for that backend. */
        if (g_backend == &g_wl_backend) {
            luna_flush_pending_screenshot();
            finish_screenshot_notification();
        }
        g_backend->poll_events();
    }
    session_save();
    luna_weather_shutdown();
    luna_bluetooth_shutdown();
    luna_ethernet_shutdown();
    luna_wifi_shutdown();
    luna_monitor_shutdown();
    shell_state_watch_close();
    shell_async_close();
    luna_shutdown();
    g_backend->terminate();
    return 0;
}
