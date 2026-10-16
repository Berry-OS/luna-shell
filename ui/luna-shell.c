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
#define LUNA_UI_MAX_ELEMENTS 3000
/* The shell loads the base sheet, a complete skin sheet, and the shared
 * widget sheet into one cascade.  The luna-ui default (600 expanded
 * selectors) silently clipped the tail of that cascade; every non-default
 * skin consequently lost the widget rules that are intentionally appended
 * last.  Size this host for the full shipped cascade, including selector-list
 * expansion and enough headroom for external skins. */
#define LUNA_UI_MAX_RULES 3400
/* Enable NanoSVG for <img> / background-image .svg (nanosvg.h next to luna-ui.h). */
#define LUNA_UI_NANOSVG
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
#if defined(__GLIBC__)
#include <malloc.h>
#endif

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
#define LUNA_KEY_LEFT_SHIFT  340
#define LUNA_KEY_RIGHT_SHIFT 344
#define LUNA_KEY_LEFT_CONTROL 341
#define LUNA_KEY_RIGHT_CONTROL 345
#define LUNA_KEY_LEFT_ALT    342
#define LUNA_KEY_RIGHT_ALT   346
#define LUNA_KEY_COMMA       44
#define LUNA_KEY_SPACE       32
#define LUNA_KEY_F1          290
#define LUNA_KEY_F2          291
#define LUNA_KEY_F3          292
#define LUNA_KEY_F4          293
#define LUNA_KEY_F5          294
#define LUNA_KEY_F6          295
#define LUNA_KEY_F7          296
#define LUNA_KEY_F8          297
#define LUNA_KEY_F9          298
#define LUNA_KEY_F10         299
#define LUNA_KEY_F11         300
#define LUNA_KEY_F12         301
#define LUNA_KEY_PRINT_SCREEN 283
#define LUNA_KEY_PAGE_UP      266
#define LUNA_KEY_PAGE_DOWN    267
#define LUNA_MOD_SHIFT       0x0001
#define LUNA_MOD_CONTROL     0x0002
#define LUNA_MOD_ALT         0x0004
#define LUNA_MOD_SUPER       0x0008

#define LUNA_SHELL_VERSION "1.2"
#define MAX_WINDOWS    128
#define MAX_WIN_SLOTS  12
#define MAX_TRAY_SLOTS 32
#define MAX_TRAY_ENTRIES 128  /* compositor task/status entries kept from one snapshot */
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

#define LUNA_SNI_MAX_ITEMS 16
#define LUNA_SNI_IMPLEMENTATION
#include "luna-sni.h"

/* AirPlay receiver (widget/airplay.h).  -DLUNA_AIRPLAY_PLAYFAIR additionally
 * pulls in airplay_playfair.h, which is GPL-3.0: a shell built that way is a
 * combined work under the GPL-3.0. */
/* AirDrop (widget/airdrop.h): OpenSSL is loaded at run time, nothing to link. */
#ifdef LUNA_WITH_AIRDROP
#  define AIRDROP_IMPLEMENTATION
#  include "../widget/airdrop.h"
#endif

#ifdef LUNA_WITH_AIRPLAY
#  ifdef LUNA_AIRPLAY_PLAYFAIR
#    define AIRPLAY_PLAYFAIR
#  endif
#  define AIRPLAY_IMPLEMENTATION
#  include "../widget/airplay.h"
#endif

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

/* CSS parsing and skin replacement create sizeable temporary allocation
 * arenas.  Current luna-ui keeps its final DOM/CSS tables compact, but glibc
 * may retain freed arena pages indefinitely and make RSS look like a leak.
 * Return completely free pages after the infrequent stylesheet rebuilds. */
static void shell_trim_heap(void) {
#if defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
}

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

/* Shell-owned desktop widgets (injected into #bg_layer; skins must not replace). */
static const char g_shell_widgets_html[] =
#include "luna-shell-widgets.html.h"
;

static const char g_shell_widgets_css[] =
#include "luna-shell-widgets.css.h"
;

static unsigned long long g_shell_widgets_css_gen;
static int g_dock_width_dirty = 1;
static int g_shell_pending_right;
static double g_shell_right_x;
static double g_shell_right_y;
/* Compositor IPC and shell keyboard both route Launchpad shortcuts when the
 * overlay owns keyboard focus.  Suppress the duplicate shell dispatch. */
static int g_shell_ipc_launchpad_toggle = 0;

static const char* shell_widgets_div_close(const char* open) {
    int depth = 0;
    const char* p = open;
    if (!p) return NULL;
    while (*p) {
        if (!strncmp(p, "<!--", 4)) {
            const char* end = strstr(p + 4, "-->");
            if (!end) return NULL;
            p = end + 3;
            continue;
        }
        if (!strncmp(p, "<div", 4) &&
            (p[4] == '>' || p[4] == ' ' || p[4] == '\t' || p[4] == '\n')) {
            depth++;
            p += 4;
            continue;
        }
        if (!strncmp(p, "</div>", 6)) {
            depth--;
            if (depth == 0) return p;
            p += 6;
            continue;
        }
        p++;
    }
    return NULL;
}

static int shell_widgets_legacy_open(const char* p) {
    return !strncmp(p, "<div id=\"widget_clock\"", 22) ||
           !strncmp(p, "<div id=\"widget_stats\"", 22) ||
           !strncmp(p, "<div id=\"widget_weather\"", 24) ||
           !strncmp(p, "<div id=\"widget_airplay\"", 24) ||
           !strncmp(p, "<div id=\"widget_airdrop\"", 24) ||
           !strncmp(p, "<div id=\"luna_shell_widgets\"", 28);
}

/* The HTML wallpaper container (#wp_custom) has to be the last element of the
 * document: luna_dom_set_inner_html() renumbers every element behind its
 * target, and the shell keeps element indices in dozens of globals.  So the
 * whole #bg_layer block (wallpaper, widgets) moves to the end of <body> and
 * ends with the empty container.  Surfaces render per root, so the paint
 * order between #bg_layer and the other roots is unaffected. */
static char* shell_move_bg_last(char* html) {
    char* bg = strstr(html, "<div id=\"bg_layer\"");
    const char* close = bg ? shell_widgets_div_close(bg) : NULL;
    char* body_end = strstr(html, "</body>");
    if (!bg || !close || (body_end && body_end < bg)) return html;
    size_t n = strlen(html);
    static const char saver_div[] = "<div id=\"screensaver\" class=\"hidden\"></div>";
    char* out = malloc(n + 128);
    if (!out) return html;
    const char* tail = close + 6;
    char* end = body_end ? body_end : html + n;
    char* q = out;
    memcpy(q, html, (size_t)(bg - html)); q += bg - html;
    memcpy(q, saver_div, sizeof(saver_div) - 1); q += sizeof(saver_div) - 1;
    memcpy(q, tail, (size_t)(end - tail)); q += end - tail;
    memcpy(q, bg, (size_t)(close - bg)); q += close - bg;
    q += sprintf(q, "<div id=\"wp_custom\"></div></div>");
    strcpy(q, end);
    free(html);
    return out;
}

static char* shell_merge_widgets_html(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char* clean = (char*)malloc(n + 1);
    if (!clean) return NULL;
    const char* p = src;
    char* q = clean;
    while (*p) {
        if (shell_widgets_legacy_open(p)) {
            const char* close = shell_widgets_div_close(p);
            if (close) {
                p = close + 6;
                continue;
            }
        }
        *q++ = *p++;
    }
    *q = '\0';

    const char* insert = NULL;
    const char* bg = strstr(clean, "<div id=\"bg_layer\"");
    if (bg) insert = shell_widgets_div_close(bg);
    if (!insert) insert = strstr(clean, "</body>");
    if (!insert) insert = clean + strlen(clean);

    size_t clean_n = strlen(clean);
    size_t head_n = (size_t)(insert - clean);
    size_t widgets_n = strlen(g_shell_widgets_html);
    char* out = (char*)malloc(clean_n + widgets_n + 1);
    if (!out) { free(clean); return NULL; }
    memcpy(out, clean, head_n);
    memcpy(out + head_n, g_shell_widgets_html, widgets_n);
    memcpy(out + head_n + widgets_n, insert, clean_n - head_n + 1);
    free(clean);
    return shell_move_bg_last(out);
}

static void shell_parse_layout_html(const char* src) {
    char* html = shell_merge_widgets_html(src);
    luna_parse_html(html ? html : src);
    free(html);
}

static int shell_load_layout_file(const char* path) {
    char* src = read_file(path);
    if (!src) return 0;
    shell_parse_layout_html(src);
    free(src);
    return 1;
}

static char* g_wp_css;   /* HTML wallpaper stylesheet; see "Custom wallpapers" */

static void shell_ensure_widget_css(void) {
    unsigned long long gen = (unsigned long long)luna_css_generation();
    if (g_shell_widgets_css_gen == gen) return;
    luna_parse_css(g_shell_widgets_css);
    if (g_wp_css) luna_parse_css(g_wp_css);
    /* luna_parse_css() advances the generation.  Cache that post-parse value;
     * caching `gen` makes maintenance parse this sheet again on every tick,
     * continuously restyling the document before :hover can expose a tooltip. */
    g_shell_widgets_css_gen = (unsigned long long)luna_css_generation();
}

static void shell_dispatch_pending_right_click(void);
static void shell_sync_bg_input_region(void);
static void desk_menu_open(float x, float y);
#define WG_DEFAULT (-100000)
static const struct { const char* id; const char* name; } g_wg_ids[5] = {
    { "widget_clock", "Clock" }, { "widget_stats", "System" }, { "widget_weather", "Weather" },
    { "widget_airplay", "AirPlay" }, { "widget_airdrop", "AirDrop" }
};
static int g_wg_moving = -1, g_wg_menu = -1, g_wg_arm;
static void widgets_apply(void);
static int  widget_of_hit(int hit);
static void widget_menu_open(int w, int anchor);
static void widget_menu_pick(const char* id);

static void shell_mouse_button(int button, int action, int mods, double x, double y) {
    luna_mouse_button(button, action, mods, x, y);
    if (button == LUNA_MOUSE_BUTTON_RIGHT && action == LUNA_PRESS) {
        g_shell_pending_right = 1;
        g_shell_right_x = x;
        g_shell_right_y = y;
        /* Open window menus on press so release-hook dismiss does not race a
         * deferred dispatch from the frame loop. */
        shell_dispatch_pending_right_click();
    }
}

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
    { .key = "email",    .name = "Email",     .env = "LUNA_APP_EMAIL",    .default_cmd = "thunderbird",          .dock_visible = 1 },
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
    char wallpaper_file[PATH_MAX]; /* image path; overrides the built-in wallpaper when set */
    int  temp_colors;      /* tint the menubar temperature by severity */
    int  temp_warm;        /* degrees C where the temperature turns yellow */
    int  temp_hot;         /*                                  ... orange */
    int  temp_crit;        /*                                  ... red */
    int  weather_colors;   /* tint weather temperatures by severity */
    int  weather_cold;     /* degrees C at/below which weather turns blue */
    int  weather_hot;      /* at/above: orange */
    int  weather_crit;     /* at/above: red */
    char skin[64];      /* "default" | discovered skin directory name */
    char hostname[64];
    char cursor_theme[64]; /* "aero" | "miku" | custom theme dir name */
    char kb_layout[64];    /* XKB layout, e.g. "jp,us" | "us" | "de,us" */
    char ui_language[32];  /* locale inherited by newly launched applications */
    int  numlock_on;       /* desired NumLock state now and at sign-in */
    int  window_gap;       /* tiled window inset in pixels: 0 | 8 | 16 */
    int  edge_snap;        /* magnet to screen + other windows while dragging */
    int  dock_snap;        /* magnet stops above the dock instead of the screen bottom */
    int  edge_tile;        /* Aero Snap: tile left/right when released at an edge */
    int  corner_snap;      /* drop at a screen corner to fill that quarter */
    int  tray_group;       /* one task icon per application instead of per window */
    int  top_edge_maximize;
    int  titlebar_double_click;
    int  classic_titlebar;
    int  super_shortcuts;
    int  active_window_outline; /* draw the focused-window frame */
    int  window_list_visible;   /* top-center text window list */
    int  window_tray_icons;     /* compact running-app icons in status area */
    int  cascade_new_windows;   /* offset newly mapped windows */
    int  dock_enabled;       /* show the floating dock bar */
    int  widgets_enabled;    /* show desktop clock/stats/weather cards */
    int  dock_magnification;
    int  wallpaper_animation; /* animate aurora/stars only while enabled */
    int  wallpaper_fps;       /* GLSL wallpaper redraw rate; 0 = still */
    int  wallpaper_speed;     /* GLSL wallpaper time scale, percent */
    int  saver_minutes;       /* idle minutes before the GLSL screensaver; 0 = off */
    char saver_file[PATH_MAX]; /* screensaver shader; empty = GLSL wallpaper / built-in */
    int  session_restore;
    int  stats_style;      /* desktop statistics widget: 0 bars, 1 line graph */
    int  mb_graph;         /* menubar CPU/temperature graph: 0 both, 1 CPU, 2 temp, 3 hidden */
    int  widget_hidden;    /* bit per desktop widget (see g_wg_ids) hidden by the user */
    int  widget_x[5], widget_y[5]; /* moved position of each widget, WG_DEFAULT = stock place */
    int  airplay_latency;  /* ms the AirPlay A/V scheduler holds frames (0 = no sync) */
    int  airplay_av_offset; /* ms: > 0 delays the video window relative to the audio */
    int  airdrop_enabled;  /* AirDrop receiver + discovery (off by default) */
    int  airplay_enabled;  /* AirPlay receiver listening (off by default: it opens a network service) */
    char airplay_name[64]; /* name shown on iPhone / Mac; empty = "Luna Desktop" */
    char order_mb[160];    /* menubar item order, '|' separated ids (empty = default) */
    char order_tray[640];  /* tray icon order, '|' separated keys (empty = default) */
    int  stats_interval;   /* seconds between widget refreshes; 0 = never (static) */
    int  state_interval;   /* min seconds between compositor state.json writes that only carry title/position changes */
    int  wifi_enabled;     /* desired Wi-Fi radio state, persisted across boots */
    int  bluetooth_enabled; /* desired Bluetooth radio state, persisted across boots */
    char weather_city[64]; /* Open-Meteo city name, default Tokyo */
    char audio_backend[16]; /* "auto" | "wpctl" | "pactl" | "alsa" */
    char alsa_card[32];     /* "" / "default" / card index ("0") */
    char alsa_control[64];  /* simple mixer control, usually "Master" */
    char brightness_backend[16]; /* "auto" | "sysfs" | "brightnessctl" | "xrandr" */
    /* Toolkit scale for apps launched from Luna (see apply_toolkit_session_env).
     * LUNA_GDK_* / LUNA_QT_* / LUNA_XCURSOR_SIZE still override when set. */
    char gdk_scale[8];       /* integer toolkit scale: "1" | "2" | "3" */
    char gdk_dpi_scale[8];   /* fractional text scale: "0.75" | "1" | "1.25" | … */
    char qt_scale_factor[8]; /* Qt scale, same values as gdk_dpi_scale */
    char xcursor_size[8];    /* "24" | "32" | "48" */
    /* Shell chords. "Super", "Super+,", "F3", "Print", or "none". */
    char shortcut_launchpad[32];
    char shortcut_settings[32];
    char shortcut_screenshot[32];
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

static void cursor_theme_tick_and_refresh(void);
static void on_win_click(LunaElement* e);
static void on_tray_click(LunaElement* e);
static void shell_runtime_maintenance(void);

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
    g_settings.dock_snap = 0;
    g_settings.edge_tile = 0;
    g_settings.corner_snap = 1;
    g_settings.tray_group = 0;
    g_settings.top_edge_maximize = 0;
    g_settings.titlebar_double_click = 1;
    g_settings.classic_titlebar = 0;
    g_settings.super_shortcuts = 1;
    g_settings.active_window_outline = 0;
    g_settings.window_list_visible = 0;
    g_settings.window_tray_icons = 1;
    g_settings.wallpaper_file[0] = 0;
    g_settings.temp_colors = 1;
    g_settings.temp_warm = 60;
    g_settings.temp_hot = 75;
    g_settings.temp_crit = 90;
    g_settings.weather_colors = 1;
    g_settings.weather_cold = 5;
    g_settings.weather_hot = 30;
    g_settings.weather_crit = 35;
    g_settings.cascade_new_windows = 1;
    g_settings.dock_enabled = 1;
    g_settings.widgets_enabled = 1;
    g_settings.dock_magnification = 1;
    g_settings.wallpaper_animation = 1;
    g_settings.wallpaper_fps = 30;
    g_settings.wallpaper_speed = 100;
    g_settings.saver_minutes = 0;
    g_settings.saver_file[0] = 0;
    g_settings.session_restore = 1;
    g_settings.stats_style = 1;
    g_settings.mb_graph = 0;
    g_settings.airplay_enabled = 0;
    g_settings.airdrop_enabled = 0;
    g_settings.airplay_latency = 200;
    g_settings.widget_hidden = 0;
    for (int i = 0; i < 5; i++) g_settings.widget_x[i] = g_settings.widget_y[i] = -100000;
    g_settings.airplay_av_offset = 0;
    g_settings.airplay_name[0] = 0;
    g_settings.order_mb[0] = g_settings.order_tray[0] = 0;
    g_settings.stats_interval = 5;
    g_settings.state_interval = 10;
    g_settings.wifi_enabled = 1;
    g_settings.bluetooth_enabled = 1;
    g_settings.numlock_on = 1;
    snprintf(g_settings.weather_city, sizeof(g_settings.weather_city), "Tokyo");
    snprintf(g_settings.audio_backend, sizeof(g_settings.audio_backend), "auto");
    snprintf(g_settings.alsa_card, sizeof(g_settings.alsa_card), "default");
    snprintf(g_settings.alsa_control, sizeof(g_settings.alsa_control), "Master");
    snprintf(g_settings.brightness_backend, sizeof(g_settings.brightness_backend), "auto");
    /* Xorg parity: GDK_SCALE is integer-only (GTK ignores "0.8"), and a
     * GDK_DPI_SCALE below 1 shrinks every explicitly sized font (gedit, sakura,
     * Firefox).  UI font size comes from gtk-font-name, mirrored to GSettings
     * in gsettings_mirror_gtk_font(). */
    snprintf(g_settings.gdk_scale, sizeof(g_settings.gdk_scale), "1");
    snprintf(g_settings.gdk_dpi_scale, sizeof(g_settings.gdk_dpi_scale), "1");
    snprintf(g_settings.qt_scale_factor, sizeof(g_settings.qt_scale_factor), "1");
    snprintf(g_settings.xcursor_size, sizeof(g_settings.xcursor_size), "24");
    snprintf(g_settings.shortcut_launchpad, sizeof(g_settings.shortcut_launchpad), "Super");
    snprintf(g_settings.shortcut_settings, sizeof(g_settings.shortcut_settings), "Super+,");
    snprintf(g_settings.shortcut_screenshot, sizeof(g_settings.shortcut_screenshot), "Print");
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
            else if (!strcmp(key, "dock_snap"))
                g_settings.dock_snap = atoi(val) != 0;
            else if (!strcmp(key, "edge_snap"))
                g_settings.edge_snap = atoi(val) != 0;
            else if (!strcmp(key, "edge_tile"))
                g_settings.edge_tile = atoi(val) != 0;
            else if (!strcmp(key, "corner_snap"))
                g_settings.corner_snap = atoi(val) != 0;
            else if (!strcmp(key, "tray_group"))
                g_settings.tray_group = atoi(val) != 0;
            else if (!strcmp(key, "top_edge_maximize"))
                g_settings.top_edge_maximize = atoi(val) != 0;
            else if (!strcmp(key, "titlebar_double_click"))
                g_settings.titlebar_double_click = atoi(val) != 0;
            else if (!strcmp(key, "classic_titlebar"))
                g_settings.classic_titlebar = atoi(val) != 0;
            else if (!strcmp(key, "super_shortcuts"))
                g_settings.super_shortcuts = atoi(val) != 0;
            else if (!strcmp(key, "active_window_outline"))
                g_settings.active_window_outline = atoi(val) != 0;
            else if (!strcmp(key, "window_list_visible"))
                g_settings.window_list_visible = atoi(val) != 0;
            else if (!strcmp(key, "window_tray_icons"))
                g_settings.window_tray_icons = atoi(val) != 0;
            else if (!strcmp(key, "wallpaper_file"))
                snprintf(g_settings.wallpaper_file, sizeof(g_settings.wallpaper_file), "%s", val);
            else if (!strcmp(key, "temp_colors"))
                g_settings.temp_colors = atoi(val) != 0;
            else if (!strcmp(key, "temp_warm"))
                g_settings.temp_warm = atoi(val);
            else if (!strcmp(key, "temp_hot"))
                g_settings.temp_hot = atoi(val);
            else if (!strcmp(key, "temp_crit"))
                g_settings.temp_crit = atoi(val);
            else if (!strcmp(key, "weather_colors"))
                g_settings.weather_colors = atoi(val) != 0;
            else if (!strcmp(key, "weather_cold"))
                g_settings.weather_cold = atoi(val);
            else if (!strcmp(key, "weather_hot"))
                g_settings.weather_hot = atoi(val);
            else if (!strcmp(key, "weather_crit"))
                g_settings.weather_crit = atoi(val);
            else if (!strcmp(key, "cascade_new_windows"))
                g_settings.cascade_new_windows = atoi(val) != 0;
            else if (!strcmp(key, "dock_enabled"))
                g_settings.dock_enabled = atoi(val) != 0;
            else if (!strcmp(key, "widgets_enabled"))
                g_settings.widgets_enabled = atoi(val) != 0;
            else if (!strcmp(key, "dock_magnification"))
                g_settings.dock_magnification = atoi(val) != 0;
            else if (!strcmp(key, "wallpaper_fps")) {
                int n = atoi(val);
                g_settings.wallpaper_fps = n < 0 ? 0 : n > 60 ? 60 : n;
            } else if (!strcmp(key, "wallpaper_speed")) {
                int n = atoi(val);
                g_settings.wallpaper_speed = n < 0 ? 0 : n > 400 ? 400 : n;
            } else if (!strcmp(key, "saver_minutes")) {
                int n = atoi(val);
                g_settings.saver_minutes = n < 0 ? 0 : n > 720 ? 720 : n;
            } else if (!strcmp(key, "saver_file"))
                snprintf(g_settings.saver_file, sizeof(g_settings.saver_file), "%s", val);
            else if (!strcmp(key, "wallpaper_animation"))
                g_settings.wallpaper_animation = atoi(val) != 0;
            else if (!strcmp(key, "session_restore"))
                g_settings.session_restore = atoi(val) != 0;
            else if (!strcmp(key, "mb_graph")) {
                int n = atoi(val);
                g_settings.mb_graph = n < 0 || n > 3 ? 0 : n;
            }
            else if (!strcmp(key, "widget_hidden"))
                g_settings.widget_hidden = atoi(val) & 31;
            else if (!strcmp(key, "widget_pos")) {
                const char* q = val;
                for (int i = 0; i < 5 && q && *q; i++) {
                    int x, y;
                    if (sscanf(q, "%d,%d", &x, &y) == 2) { g_settings.widget_x[i] = x; g_settings.widget_y[i] = y; }
                    q = strchr(q, ';');
                    if (q) q++;
                }
            }
            else if (!strcmp(key, "airplay_latency")) {
                int n = atoi(val);
                g_settings.airplay_latency = n < 0 ? 0 : n > 2000 ? 2000 : n;
            }
            else if (!strcmp(key, "airplay_av_offset")) {
                int n = atoi(val);
                g_settings.airplay_av_offset = n < -1000 ? -1000 : n > 1000 ? 1000 : n;
            }
            else if (!strcmp(key, "airdrop_enabled"))
                g_settings.airdrop_enabled = atoi(val) != 0;
            else if (!strcmp(key, "airplay_enabled"))
                g_settings.airplay_enabled = atoi(val) != 0;
            else if (!strcmp(key, "airplay_name"))
                snprintf(g_settings.airplay_name, sizeof(g_settings.airplay_name), "%s", val);
            else if (!strcmp(key, "order_mb"))
                snprintf(g_settings.order_mb, sizeof(g_settings.order_mb), "%s", val);
            else if (!strcmp(key, "order_tray"))
                snprintf(g_settings.order_tray, sizeof(g_settings.order_tray), "%s", val);
            else if (!strcmp(key, "stats_style"))
                g_settings.stats_style = !strcmp(val, "bar") || !strcmp(val, "0") ? 0 : 1;
            else if (!strcmp(key, "stats_interval")) {
                int n = atoi(val);
                g_settings.stats_interval = n < 0 ? 0 : n > 3600 ? 3600 : n;
            }
            else if (!strcmp(key, "state_interval")) {
                int n = atoi(val);
                g_settings.state_interval = n < 0 ? 0 : n > 3600 ? 3600 : n;
            }
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
            else if (!strcmp(key, "shortcut_launchpad") && val[0])
                snprintf(g_settings.shortcut_launchpad, sizeof(g_settings.shortcut_launchpad), "%s", val);
            else if (!strcmp(key, "shortcut_settings") && val[0])
                snprintf(g_settings.shortcut_settings, sizeof(g_settings.shortcut_settings), "%s", val);
            else if (!strcmp(key, "shortcut_screenshot") && val[0])
                snprintf(g_settings.shortcut_screenshot, sizeof(g_settings.shortcut_screenshot), "%s", val);
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
    fprintf(f, "dock_snap=%d\n", g_settings.dock_snap);
    fprintf(f, "edge_tile=%d\n", g_settings.edge_tile);
    fprintf(f, "corner_snap=%d\n", g_settings.corner_snap);
    fprintf(f, "tray_group=%d\n", g_settings.tray_group);
    fprintf(f, "top_edge_maximize=%d\n", g_settings.top_edge_maximize);
    fprintf(f, "titlebar_double_click=%d\n", g_settings.titlebar_double_click);
    fprintf(f, "classic_titlebar=%d\n", g_settings.classic_titlebar);
    fprintf(f, "super_shortcuts=%d\n", g_settings.super_shortcuts);
    fprintf(f, "active_window_outline=%d\n", g_settings.active_window_outline);
    fprintf(f, "window_list_visible=%d\n", g_settings.window_list_visible);
    fprintf(f, "window_tray_icons=%d\n", g_settings.window_tray_icons);
    fprintf(f, "wallpaper_file=%s\n", g_settings.wallpaper_file);
    fprintf(f, "temp_colors=%d\n", g_settings.temp_colors);
    fprintf(f, "temp_warm=%d\n", g_settings.temp_warm);
    fprintf(f, "temp_hot=%d\n", g_settings.temp_hot);
    fprintf(f, "temp_crit=%d\n", g_settings.temp_crit);
    fprintf(f, "weather_colors=%d\n", g_settings.weather_colors);
    fprintf(f, "weather_cold=%d\n", g_settings.weather_cold);
    fprintf(f, "weather_hot=%d\n", g_settings.weather_hot);
    fprintf(f, "weather_crit=%d\n", g_settings.weather_crit);
    fprintf(f, "cascade_new_windows=%d\n", g_settings.cascade_new_windows);
    fprintf(f, "dock_enabled=%d\n", g_settings.dock_enabled);
    fprintf(f, "widgets_enabled=%d\n", g_settings.widgets_enabled);
    fprintf(f, "dock_magnification=%d\n", g_settings.dock_magnification);
    fprintf(f, "wallpaper_animation=%d\n", g_settings.wallpaper_animation);
    fprintf(f, "wallpaper_fps=%d\n", g_settings.wallpaper_fps);
    fprintf(f, "wallpaper_speed=%d\n", g_settings.wallpaper_speed);
    fprintf(f, "saver_minutes=%d\n", g_settings.saver_minutes);
    fprintf(f, "saver_file=%s\n", g_settings.saver_file);
    fprintf(f, "session_restore=%d\n", g_settings.session_restore);
    fprintf(f, "state_interval=%d\n", g_settings.state_interval);
    fprintf(f, "stats_style=%s\n", g_settings.stats_style ? "graph" : "bar");
    fprintf(f, "mb_graph=%d\n", g_settings.mb_graph);
    fprintf(f, "airplay_enabled=%d\n", g_settings.airplay_enabled);
    fprintf(f, "widget_hidden=%d\n", g_settings.widget_hidden);
    fprintf(f, "widget_pos=%d,%d;%d,%d;%d,%d;%d,%d;%d,%d\n", g_settings.widget_x[0], g_settings.widget_y[0], g_settings.widget_x[1], g_settings.widget_y[1],
            g_settings.widget_x[2], g_settings.widget_y[2], g_settings.widget_x[3], g_settings.widget_y[3], g_settings.widget_x[4], g_settings.widget_y[4]);
    fprintf(f, "airplay_latency=%d\n", g_settings.airplay_latency);
    fprintf(f, "airplay_av_offset=%d\n", g_settings.airplay_av_offset);
    fprintf(f, "airdrop_enabled=%d\n", g_settings.airdrop_enabled);
    fprintf(f, "airplay_name=%s\n", g_settings.airplay_name);
    fprintf(f, "order_mb=%s\n", g_settings.order_mb);
    fprintf(f, "order_tray=%s\n", g_settings.order_tray);
    fprintf(f, "stats_interval=%d\n", g_settings.stats_interval);
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
    fprintf(f, "shortcut_launchpad=%s\n", g_settings.shortcut_launchpad);
    fprintf(f, "shortcut_settings=%s\n", g_settings.shortcut_settings);
    fprintf(f, "shortcut_screenshot=%s\n", g_settings.shortcut_screenshot);
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
static void apply_dock_visibility(void);
static void skin_apply_chrome(int skin_idx);
static void skin_apply_toolkit(int skin_idx);
static int  skin_apply_wm_decoration(int skin_idx);
static void skin_export_window_theme(int skin_idx);
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
    /* Clients which do not bind xdg-decoration retain their native CSD
     * (GTK commonly draws a HeaderBar in that case). */
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
            /* Kept as a recognised legacy key.  Window chrome is now always
             * compositor-owned so SDL, GLFW/libdecor and native clients all
             * use the selected skin's titlebar renderer. */
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
    shell_ensure_widget_css();
    luna_mark_layout_dirty();
    shell_trim_heap();
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
static void session_autosave_tick(void);

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
    uint64_t win_id;
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
static int g_lp_grid_idx   = -1;
static int g_settings_idx          = -1;
static int g_settings_sheet_idx    = -1;
static int g_settings_panel_apps   = -1;
static int g_settings_panel_disp   = -1;
static int g_settings_panel_lang   = -1;
static int g_settings_panel_kb     = -1;
static int g_settings_panel_sound  = -1;
static int g_settings_panel_display = -1;
static int g_settings_panel_wm     = -1;
static int g_stab_apps_idx         = -1;
static int g_stab_disp_idx         = -1;
static int g_stab_lang_idx         = -1;
static int g_stab_kb_idx           = -1;
static int g_stab_sound_idx        = -1;
static int g_stab_display_idx      = -1;
static int g_stab_wm_idx           = -1;
static int g_win_menu_idx  = -1;
static int g_clip_menu_idx = -1;
static int g_mb_clip_idx   = -1;
static uint64_t g_win_menu_target = 0;
/* >=0 → win_menu is targeting a shell modeless dialog (not a compositor window). */
static int g_win_menu_dialog = -1;
/* When set, #win_menu is reparented under a dialog box so it paints on that
 * dialog's Wayland surface (and above its chrome) instead of a separate
 * overlay that can end up behind luna.dialog.* layers. */
static int g_win_menu_embedded = 0;
static int g_win_menu_saved_parent = -1;

/* Shared chrome for shell modeless dialogs (settings / about / confirm / …).
 * Titlebar drag, traffic-lights, and the right-click window menu all key off
 * this table so a new dialog only needs one descriptor. */
typedef enum {
    SHELL_DLG_SETTINGS = 0,
    SHELL_DLG_ABOUT,
    SHELL_DLG_CONFIRM,
    SHELL_DLG_NET_DETAIL,
    SHELL_DLG_COUNT
} ShellDialogKind;

enum {
    SHELL_DLG_F_MAXIMIZE  = 1 << 0, /* green traffic light / menu Maximize */
    SHELL_DLG_F_DIALOG_NS = 1 << 1  /* luna.dialog.* layer-shell namespace */
};

typedef struct {
    const char* name;       /* LunaSurface.name */
    const char* root_id;    /* overlay root (hidden when closed) */
    const char* box_id;     /* sheet / dialog box */
    const char* drag_id;    /* titlebar drag handle */
    const char* title_id;   /* optional; used as win_menu caption */
    const char* close_id;
    const char* min_id;
    const char* max_id;
    unsigned    flags;
} ShellDialogChrome;

static const ShellDialogChrome g_shell_dialogs[SHELL_DLG_COUNT] = {
    { "settings",   "settings_win",     "settings_sheet",  "settings_drag",
      "settings_title",   "stl_close", "stl_min", "stl_max",
      SHELL_DLG_F_MAXIMIZE | SHELL_DLG_F_DIALOG_NS },
    { "about",      "about_win",        "about_box",       "about_drag",
      "about_title",      "tl_close",  "tl_min",  "tl_max",
      SHELL_DLG_F_MAXIMIZE | SHELL_DLG_F_DIALOG_NS },
    { "confirm",    "confirm_overlay",  "confirm_box",     "confirm_drag",
      "confirm_title",    "ctl_close", "ctl_min", "ctl_max",
      0 },
    { "net_detail", "net_detail_win",   "net_detail_box",  "net_detail_drag",
      "net_detail_title", "ntl_close", "ntl_min", "ntl_max",
      SHELL_DLG_F_DIALOG_NS },
};

static int  shell_dialog_kind_from_hit(int hit);
static int  shell_dialog_hit_is_titlebar(int kind, int hit, double x, double y);
static void shell_dialog_menu_open(int kind, float x, float y);
static void win_menu_detach_embed(void) {
    if (!g_win_menu_embedded || g_win_menu_idx < 0) {
        g_win_menu_embedded = 0;
        g_win_menu_saved_parent = -1;
        return;
    }
    LunaElement* menu = luna_element_at(g_win_menu_idx);
    if (menu) {
        menu->parent_idx = g_win_menu_saved_parent;
        menu->z_override_valid = 0;
    }
    g_win_menu_embedded = 0;
    g_win_menu_saved_parent = -1;
    luna_mark_layout_dirty();
}

/* Cached menubar hit-test indices (avoid repeated ID lookups in mouse hook) */
static int g_mb_logo_idx  = -1;
static int g_mb_cc_idx    = -1;
static int g_mb_wifi_idx  = -1;
static int g_mb_bat_idx   = -1;
static int g_desk_menu_idx = -1;
static int g_tray_menu_idx = -1;
#define TRAY_MENU_SLOTS 24
static uint64_t g_tray_menu_ids[TRAY_MENU_SLOTS];
static double g_desk_shot_at;   /* deferred screenshot (after the menu is gone) */
static int g_power_menu_idx = -1;
static int g_wifi_menu_idx = -1;
static int g_wifi_selected = -1;
static int g_bt_menu_idx = -1;
static int g_mb_weather_idx = -1;
static int g_weather_menu_idx = -1;
static int g_calendar_menu_idx = -1;
static int g_alarm_ring_idx = -1;
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
static int g_win_glyph_idx[MAX_WIN_SLOTS];          /* span.win_glyph child of win_N */
static int g_tray_glyph_idx[MAX_TRAY_SLOTS];        /* span.tray_glyph child of tray_N */
static int g_tray_tip_idx[MAX_TRAY_SLOTS];          /* span.tray_tip child of tray_N */
static int g_tray_area_idx          = -1;           /* #tray_area container */
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
    UI_WG_CLOCK_VIEW,
    UI_WG_STOPWATCH_OPEN,
    UI_WG_STOPWATCH_VIEW,
    UI_WG_STOPWATCH_TIME,
    UI_WG_STOPWATCH_TOGGLE,
    UI_WG_STOPWATCH_STATE,
    UI_WIDGET_CLOCK,
    UI_WIDGET_STATS,
    UI_WIDGET_WEATHER,
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
    "wg_time", "wg_date", "wg_clock_view", "wg_stopwatch_open",
    "wg_stopwatch_view",
    "wg_stopwatch_time", "wg_stopwatch_toggle", "wg_stopwatch_state",
    "widget_clock", "widget_stats", "widget_weather",
    "st_cpu_val", "st_cpu_fill",
    "st_mem_val", "st_mem_fill", "st_disk_val", "st_disk_fill",
    "win_menu_title", "switcher"
};
static int g_ui_idx[UI_CACHE_COUNT];
static int g_lp_app_idx[APP_COUNT];
static int g_sw_slot_idx[MAX_SWITCHER_SLOTS];

/* Extra Launchpad/Dock applications filled from XDG .desktop entries. */
#define MAX_LP_XDG 48
#define MAX_DOCK_XDG 8
#define MAX_DOCK_PINNED 32

typedef struct {
    char id[NAME_MAX + 1];
    char name[256];
    char path[PATH_MAX];
    char icon_name[256];
    char icon_path[PATH_MAX];
    char startup_wm_class[256];
} LunaLpXdgApp;

typedef struct {
    int app_index;
    int item_idx;
    int icon_idx;
    int dot_idx;
    int label_idx;
    int pinned;
} LunaDockXdgSlot;

static LunaLpXdgApp g_lp_xdg[MAX_LP_XDG];
static int g_lp_xdg_count = 0;
static int g_lp_xdg_idx[MAX_LP_XDG];
static int g_lp_xdg_icon_idx[MAX_LP_XDG];
static int g_lp_xdg_label_idx[MAX_LP_XDG];
static int g_lp_xdg_ready = 0;

static LunaDockXdgSlot g_dock_xdg[MAX_DOCK_XDG];
static int g_dock_xdg_count = 0;
static int g_dock_xdg_sep_idx = -1;
static char g_dock_pinned[MAX_DOCK_PINNED][NAME_MAX + 1];
static int g_dock_pinned_count = 0;

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
static LunaTrayEntry g_tray[MAX_TRAY_ENTRIES];
static int           g_tray_count = 0;
static char          g_shell_state_path[512];
static char          g_shell_sock_path[512];
static char          g_shell_action_sock_path[512];
static int           g_shell_action_fd = -1;
static int           g_win_slot_idx[MAX_WIN_SLOTS];  /* element index for win_0.. */
static int           g_tray_slot_idx[MAX_TRAY_SLOTS];  /* element index for tray_0.. */
static uint64_t      g_win_slot_id[MAX_WIN_SLOTS];    /* compositor window handle shown in slot */
static char          g_tray_slot_key[MAX_TRAY_SLOTS][64];
static char          g_tray_sni_service[MAX_TRAY_SLOTS][96];
static char          g_tray_sni_path[MAX_TRAY_SLOTS][96];
static int           g_tray_sni_kind[MAX_TRAY_SLOTS];
static LunaSniSnapshot g_sni;
static unsigned long long g_sni_generation;

/* Dock magnification is CSS-only (#dock.dock_animated).  The shell never
 * interpolates icon transforms per frame — that work belongs in the stylesheet. */

static int g_about_sheet_max = 0;
static int g_settings_sheet_max = 0;

static void app_set_dot(LunaApp* app, int running) {
    char dot_id[64];
    snprintf(dot_id, sizeof(dot_id), "dot_%s", app->key);
    int idx = luna_get_element_by_id(dot_id);
    if (idx != -1)
        luna_update_classes(idx, "running", running ? "running" : NULL);
}

static void set_hidden(int idx, int hidden);
static int elem_idx_of(LunaElement* e) {
    for (int i = 0; i < luna_element_count(); i++)
        if (luna_element_at(i) == e) return i;
    return -1;
}

static void toast_show(const char* title, const char* msg, double secs);
static void on_power_menu(LunaElement* e);
static void stopwatch_set_mode(int enabled);
static void on_control_center(LunaElement* e);

static void shell_paths_init(void) {
    char dir[PATH_MAX];
    if (!g_xdg.runtime_dir[0] ||
        !xdg_app_dir(dir, sizeof(dir), g_xdg.runtime_dir, "luna-shell", 1)) {
        fprintf(stderr, "[luna-shell/xdg] runtime directory unavailable\n");
        g_shell_state_path[0] = 0;
        g_shell_sock_path[0] = 0;
        g_shell_action_sock_path[0] = 0;
        return;
    }
    path_join2(g_shell_state_path, sizeof(g_shell_state_path), dir, "state.json");
    path_join2(g_shell_sock_path, sizeof(g_shell_sock_path), dir, "luna-shell.sock");
    path_join2(g_shell_action_sock_path, sizeof(g_shell_action_sock_path), dir, "action.sock");
}

static void shell_action_close(void) {
    if (g_shell_action_fd >= 0) {
        close(g_shell_action_fd);
        g_shell_action_fd = -1;
    }
    if (g_shell_action_sock_path[0])
        unlink(g_shell_action_sock_path);
}

static void shell_action_init(void) {
    shell_action_close();
    if (!g_shell_action_sock_path[0]) return;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t splen = strlen(g_shell_action_sock_path);
    if (splen >= sizeof(addr.sun_path)) {
        close(fd);
        return;
    }
    unlink(g_shell_action_sock_path);
    memcpy(addr.sun_path, g_shell_action_sock_path, splen + 1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    g_shell_action_fd = fd;
}

static int shell_action_poll_fd(void) {
    return g_shell_action_fd;
}

static void launchpad_close(void);
static void on_launchpad_open(LunaElement* e);
static void on_settings_open(LunaElement* e);
static void take_timestamped_screenshot(void);
static void settings_sync_scale_sliders(void);
static void settings_mark_shortcuts(void);
static void settings_commit_scale_sliders(void);
static void shortcut_capture_end(void);
static int shortcut_handle_key(int key, int mods, int action);
static int shortcut_try_dispatch(int key, int mods);
static void shell_note_user_activity(void);
static int is_shown(int idx) {
    if (idx < 0) return 0;
    LunaElement* e = luna_element_at(idx);
    return e && strstr(e->class_name, "hidden") == NULL;
}

static void shell_request_repaint(int surf_idx);

static void saver_start(void);
static void saver_stop(void);

static void shell_action_dispatch(const char* action) {
    if (!action || !*action) return;
    shell_note_user_activity();
    if (!strcmp(action, "idle_start")) { saver_start(); return; }
    if (!strcmp(action, "idle_stop"))  { saver_stop();  return; }
    if (!strcmp(action, "launchpad_toggle")) {
        g_shell_ipc_launchpad_toggle = 1;
        if (is_shown(g_launchpad_idx)) launchpad_close();
        else on_launchpad_open(NULL);
    } else if (!strcmp(action, "settings_open")) {
        on_settings_open(NULL);
    } else if (!strcmp(action, "screenshot")) {
        take_timestamped_screenshot();
    } else if (!strcmp(action, "power_menu_toggle")) {
        on_power_menu(NULL);
    } else if (!strcmp(action, "stopwatch_open")) {
        stopwatch_set_mode(1);
    }
    shell_request_repaint(-1);
}

static int shell_action_drain(void) {
    if (g_shell_action_fd < 0) return 0;
    char buf[64];
    int any = 0;
    for (;;) {
        ssize_t n = recv(g_shell_action_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            buf[strcspn(buf, "\r\n")] = 0;
            shell_action_dispatch(buf);
            any = 1;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return any;
}

/* A settings apply can exceed the Unix listener backlog.  Never wait for
 * the compositor on the UI thread; retain commands in FIFO order instead. */
#define SHELL_CMD_QUEUE_CAP 256
static char g_cmd_queue[SHELL_CMD_QUEUE_CAP][128];
static unsigned g_cmd_head, g_cmd_count;
static int g_cmd_fd = -1;
static size_t g_cmd_offset;

static void shell_cmd_flush(void) {
    while (g_cmd_count) {
        if (g_cmd_fd < 0) {
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (fd < 0) return;
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            size_t n = strlen(g_shell_sock_path);
            if (n >= sizeof(addr.sun_path)) { close(fd); return; }
            memcpy(addr.sun_path, g_shell_sock_path, n + 1);
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                close(fd);
                return; /* Backlog full or compositor restarting: retry next tick. */
            }
            g_cmd_fd = fd;
            g_cmd_offset = 0;
        }
        const char* wire = g_cmd_queue[g_cmd_head];
        size_t len = strlen(wire);
        ssize_t n = send(g_cmd_fd, wire + g_cmd_offset, len - g_cmd_offset,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) return;
        if (n <= 0) {
            close(g_cmd_fd);
            g_cmd_fd = -1;
            return;
        }
        g_cmd_offset += (size_t)n;
        if (g_cmd_offset < len) return;
        close(g_cmd_fd);
        g_cmd_fd = -1;
        g_cmd_head = (g_cmd_head + 1) % SHELL_CMD_QUEUE_CAP;
        g_cmd_count--;
    }
}

static int shell_send_cmd(const char* cmd) {
    if (!cmd || !*cmd || !g_shell_sock_path[0] ||
        strlen(cmd) + 1 >= sizeof(g_cmd_queue[0])) return 0;
    /* Preserve startup retry behavior when no compositor is running (KMS). */
    if (access(g_shell_sock_path, F_OK) != 0) return 0;
    shell_cmd_flush();
    if (g_cmd_count == SHELL_CMD_QUEUE_CAP) return 0;
    unsigned tail = (g_cmd_head + g_cmd_count) % SHELL_CMD_QUEUE_CAP;
    snprintf(g_cmd_queue[tail], sizeof(g_cmd_queue[tail]), "%s\n", cmd);
    g_cmd_count++;
    shell_cmd_flush();
    return 1; /* Accepted for delivery; a full backlog does not lose settings. */
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

static const char* shell_icon_name_for_app(const char* app_id) {
    if (!app_id || !*app_id) return "app";
    char a[96];
    snprintf(a, sizeof(a), "%s", app_id);
    for (char* p = a; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strstr(a, "terminal") || strstr(a, "foot") || strstr(a, "sakura") ||
        strstr(a, "kitty") || strstr(a, "alacritty"))
        return "terminal";
    if (strstr(a, "firefox") || strstr(a, "browser") || strstr(a, "chrome") ||
        strstr(a, "chromium") || strstr(a, "epiphany"))
        return "browser";
    if (strstr(a, "nautilus") || strstr(a, "files") || strstr(a, "thunar") ||
        strstr(a, "pcmanfm") || strstr(a, "luna-fm"))
        return "files";
    if (strstr(a, "gedit") || strstr(a, "editor") || strstr(a, "code") ||
        strstr(a, "luna-editor"))
        return "editor";
    if (strstr(a, "music") || strstr(a, "rhythmbox") || strstr(a, "vlc"))
        return "music";
    if (strstr(a, "settings") || strstr(a, "control") || strstr(a, "gnome-control"))
        return "settings";
    if (strstr(a, "gtk") || strstr(a, "demo"))
        return "gtk";
    return "app";
}

static const char* tray_glyph(const char* icon) {
    /* LunaSymbols (Font Awesome) codepoints as UTF-8.
     * The compositor appends "_active" to the focused item's icon name. */
    char name[32];
    if (!icon || !*icon) return "\uf111";                 /* circle */
    snprintf(name, sizeof(name), "%s", icon);
    char* suf = strstr(name, "_active");
    if (suf) *suf = 0;
    if (!strcmp(name, "terminal")) return "\uf120";       /* terminal */
    if (!strcmp(name, "browser"))  return "\uf0ac";       /* globe */
    if (!strcmp(name, "files"))    return "\uf07b";       /* folder */
    if (!strcmp(name, "editor"))   return "\uf1c9";       /* file-code */
    if (!strcmp(name, "music"))    return "\uf001";       /* music */
    if (!strcmp(name, "settings")) return "\uf013";       /* gear */
    if (!strcmp(name, "gtk"))      return "\uf1b2";       /* cube */
    if (!strcmp(name, "wifi"))     return "\uf1eb";       /* wifi */
    if (!strcmp(name, "bat"))      return "\uf240";       /* battery-full */
    if (!strcmp(name, "clip"))     return "\uf0ea";       /* copy / clipboard */
    if (!strcmp(name, "app"))      return "\uf2d0";       /* generic window: no icon known */
    return "\uf111";
}

static uint64_t tray_lookup_surface(const char* id) {
    if (!id || !*id) return 0;
    if (!strncmp(id, "win:", 4))
        return (uint64_t)strtoull(id + 4, NULL, 10);
    if (strncmp(id, "app:", 4) != 0) return 0;
    const char* app = id + 4;
    uint64_t fallback = 0;
    for (int i = 0; i < g_win_count; i++) {
        if (strcmp(g_wins[i].app_id, app) != 0) continue;
        if (g_wins[i].focused) return g_wins[i].id;
        if (!g_wins[i].minimized && !fallback) fallback = g_wins[i].id;
        else if (!fallback) fallback = g_wins[i].id;
    }
    return fallback;
}

static void parse_tray_surface_id(LunaTrayEntry* t) {
    t->surface_id = tray_lookup_surface(t->id);
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
/* Stopwatch state uses the shell's monotonic clock.  The elapsed value is
 * accumulated only when paused; while running the displayed value is derived
 * from g_now so hiding the stopwatch never loses time or needs background
 * repaint work. */
static int g_stopwatch_mode = 0;
static int g_stopwatch_running = 0;
static double g_stopwatch_started_at = 0.0;
static double g_stopwatch_elapsed = 0.0;
static double g_stopwatch_next_deadline = 0.0;
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
static double plat_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static struct timespec t0;
    static int started = 0;
    if (!started) { t0 = ts; started = 1; }
    return (double)(ts.tv_sec - t0.tv_sec) + (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;
}


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
/* Animation cadence. Idle aurora/stars only (no open windows). 30 Hz keeps
 * slow wallpaper motion visually smooth while the existing desktop-busy gate
 * prevents full-screen background commits underneath application windows. */
#define LUNA_WL_BG_FRAME_SEC      (1.0 / 30.0)
#define LUNA_SINGLE_BG_FRAME_SEC  (1.0 / 30.0)
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
    LunaTrayEntry tray[MAX_TRAY_ENTRIES];
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
            if (tc >= MAX_TRAY_ENTRIES) continue;
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
     * compositor updates repaint shell chrome.  Match by window id so a
     * snapshot that is not already in registration order still counts. */
    int geometry_changed = 0;
    if (!state_changed && wc == g_win_count) {
        for (int i = 0; i < wc; i++) {
            LunaWinEntry* prev = NULL;
            for (int j = 0; j < g_win_count; j++) {
                if (g_wins[j].id == wins[i].id) { prev = &g_wins[j]; break; }
            }
            if (!prev) break;
            if (wins[i].x != prev->x || wins[i].y != prev->y) {
                geometry_changed = 1;
                break;
            }
        }
    }
    if (geometry_changed) g_window_motion_busy_until = g_now + 0.20;
    /* Keep chips in first-seen order: update existing entries in place, drop
     * closed windows, and append newly registered ones at the end. */
    {
        LunaWinEntry merged[MAX_WINDOWS];
        char seen[MAX_WINDOWS];
        int n = 0;
        memset(seen, 0, (size_t)wc);
        for (int i = 0; i < g_win_count && n < MAX_WINDOWS; i++) {
            for (int j = 0; j < wc; j++) {
                if (!seen[j] && wins[j].id == g_wins[i].id) {
                    merged[n++] = wins[j];
                    seen[j] = 1;
                    break;
                }
            }
        }
        for (int j = 0; j < wc && n < MAX_WINDOWS; j++) {
            if (!seen[j]) merged[n++] = wins[j];
        }
        g_win_count = n;
        memcpy(g_wins, merged, (size_t)n * sizeof(LunaWinEntry));
    }
    g_visible_window_count = 0;
    for (int i = 0; i < g_win_count; i++)
        g_visible_window_count += !g_wins[i].minimized;

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

/* Resolved application icons, keyed by app_id.  Filled by app_icons_resolve()
 * (which needs the .desktop/icon-theme code further down) whenever the window
 * list changes; the UI code here only reads it.  "" means "looked up, none
 * found" so unknown apps are probed once, not on every update. */
#define APP_ICON_CACHE 128
static struct { char app_id[96]; char path[PATH_MAX]; int fails; } g_app_icon_cache[APP_ICON_CACHE];
static int g_app_icon_cache_n = 0;

static const char* app_icon_cached(const char* app_id) {
    if (!app_id || !*app_id) return "";
    for (int i = 0; i < g_app_icon_cache_n; i++)
        if (!strcmp(g_app_icon_cache[i].app_id, app_id)) return g_app_icon_cache[i].path;
    return "";
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
    int list_idx = luna_get_element_by_id("win_list");
    if (list_idx >= 0)
        set_hidden(list_idx, !g_settings.window_list_visible);

    /* Registration order: focus and minimize only restyle the chip so the
     * list does not jump when the user switches windows. */
    int n = g_win_count < MAX_WIN_SLOTS ? g_win_count : MAX_WIN_SLOTS;

    for (int s = 0; s < MAX_WIN_SLOTS; s++) {
        int idx = g_win_slot_idx[s];
        if (idx < 0) continue;
        if (s >= n) {
            set_hidden(idx, 1);
            g_win_slot_id[s] = 0;
            continue;
        }
        LunaWinEntry* w = &g_wins[s];
        g_win_slot_id[s] = w->id;
        set_hidden(idx, 0);
        if (g_win_label_idx[s] >= 0)
            luna_set_text(g_win_label_idx[s], w->title);
        if (g_win_glyph_idx[s] >= 0) {
            const char* icon_file = app_icon_cached(w->app_id);
            if (*icon_file) {
                luna_set_text(g_win_glyph_idx[s], "");
                luna_set_background_image(g_win_glyph_idx[s], icon_file);
            } else {
                luna_set_background_image(g_win_glyph_idx[s], NULL);
                luna_set_text(g_win_glyph_idx[s],
                              tray_glyph(shell_icon_name_for_app(w->app_id)));
            }
        }
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
    return cmd && (str_contains_ci(cmd, "firefox") || str_contains_ci(cmd, "mozilla") ||
                   str_contains_ci(cmd, "chrom") || str_contains_ci(cmd, "brave"));
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
    if (!strcmp(key, "email"))
        return str_contains_ci(app_id, "thunderbird") || str_contains_ci(app_id, "betterbird") ||
               str_contains_ci(app_id, "geary") || str_contains_ci(app_id, "evolution");
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
    /* Tall popups: keep the bottom above the taskbar / screen edge. */
    {
        float avail = bot_max - y;
        if (avail > 48.0f && mh > avail) {
            m->has_max_height = 1;
            m->css_max_height = floorf(avail);
            m->max_height_pct = 0;
            m->raw_max_height = m->css_max_height;
            m->raw_max_height_off = 0.0f;
            mh = avail;
        }
    }
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

/* app_id of the window behind a "win:N" / "app:ID" task key. */
static const char* tray_key_app_id(const char* key) {
    uint64_t sid = tray_lookup_surface(key);
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].id == sid) return g_wins[i].app_id;
    return "";
}

static void tray_apply_image(int slot, int glyph_idx, const char* path) {
    if (glyph_idx >= 0) luna_set_text(glyph_idx, "");
    luna_set_background_image(slot, path);
}

static void tray_apply_glyph(int slot, int glyph_idx, const char* icon) {
    luna_set_background_image(slot, NULL);
    if (glyph_idx >= 0) luna_set_text(glyph_idx, tray_glyph(icon));
}

static int tray_write_sni_png(int slot, const LunaSniItem* it, char* path, size_t n) {
    if (!it || !it->has_pixmap || it->icon_w <= 0 || it->icon_h <= 0) return 0;
    if (!g_xdg.runtime_dir[0]) return 0;
    snprintf(path, n, "%s/luna-shell/sni-%d.png", g_xdg.runtime_dir, slot);
    luna_invalidate_texture(path);
    return stbi_write_png(path, it->icon_w, it->icon_h, 4, it->rgba, it->icon_w * 4) != 0;
}

static int tray_compositor_is_status(const LunaTrayEntry* t) {
    if (!t || !t->id[0]) return 0;
    if (!strncmp(t->id, "win:", 4) || !strncmp(t->id, "app:", 4)) return 0;
    return 1;
}


static int tray_key_is_window(const char* key) {
    return key && (!strncmp(key, "win:", 4) || !strncmp(key, "app:", 4));
}

static LunaTrayEntry* tray_entry_for_key(const char* key) {
    if (!key || !*key) return NULL;
    for (int i = 0; i < g_tray_count; i++)
        if (!strcmp(g_tray[i].id, key)) return &g_tray[i];
    return NULL;
}

/* Copy `src` keeping at most `maxw` display columns (a multibyte character
 * counts double), ending in an ellipsis when cut.  The tooltip box is a fixed
 * two lines, so anything longer used to be clipped mid-word. */
static void tip_fit(char* dst, size_t n, const char* src, int maxw) {
    size_t o = 0;
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && o + 5 < n; ) {
        int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        int cw = len == 1 ? 1 : 2;
        if (w + cw > maxw) { memcpy(dst + o, "…", 3); o += 3; break; }
        for (int i = 0; i < len && p[i]; i++) dst[o++] = (char)p[i];
        p += len;
        w += cw;
    }
    dst[o] = 0;
}

static void tray_window_tooltip(char* out, size_t n, const char* key,
                                const LunaTrayEntry* tray) {
    const char* app_name = tray && tray->label[0] ? tray->label : NULL;
    const char* title = NULL;
    uint64_t sid = tray_lookup_surface(key);
    int same_app = 0;
    for (int w = 0; w < g_win_count; w++) {
        if (g_wins[w].id != sid) continue;
        title = g_wins[w].title[0] ? g_wins[w].title : NULL;
        if (!app_name) {
            for (int a = 0; a < APP_COUNT; a++) {
                if (app_id_matches_key(g_wins[w].app_id, g_apps[a].key)) {
                    app_name = g_apps[a].name;
                    break;
                }
            }
            if (!app_name && g_wins[w].app_id[0]) app_name = g_wins[w].app_id;
        }
        if (!strncmp(key, "app:", 4))
            for (int k = 0; k < g_win_count; k++)
                same_app += !strcmp(g_wins[k].app_id, g_wins[w].app_id);
        break;
    }
    if (!app_name) app_name = "Application";
    /* Line 1: application, line 2: window title (the engine breaks on '\n'). */
    char a[96], t[96];
    tip_fit(a, sizeof(a), app_name, 40);
    if (same_app > 1) {
        snprintf(out, n, "%s \u00d7%d\nClick to choose a window", a, same_app);
    } else if (title && strcmp(title, app_name)) {
        tip_fit(t, sizeof(t), title, 64);
        snprintf(out, n, "%s\n%s", a, t);
    } else {
        snprintf(out, n, "%s", a);
    }
}

/* Tray tooltip text, two lines ("App" / "title").  The bubble is sized to the
 * widest line and kept on screen by inline width/left; showing it is plain CSS
 * :hover. */
static void tray_tip_set(int slot, const char* tip) {
    int ti = g_tray_tip_idx[slot];
    if (ti < 0) return;
    LunaElement* te = luna_element_at(ti);
    if (!te || !strcmp(te->text, tip)) return;
    luna_set_text(ti, tip);
    if (!tip[0]) return;
    float w = 0.0f;
    int lines = 1;
    for (const char* p = tip; ; ) {
        const char* nl = strchr(p, '\n');
        char line[192];
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = 0;
        float lw = luna_measure_text(line, 12.0f, 0);
        if (lw > w) w = lw;
        if (!nl) break;
        lines++;
        p = nl + 1;
    }
    w += 44.0f;   // padding, border and a little slack so the last word never wraps
    float maxw = luna_window_width > 40.0f ? luna_window_width - 16.0f : 520.0f;
    if (w > maxw) w = maxw;
    if (w < 64.0f) w = 64.0f;
    int si = g_tray_slot_idx[slot];
    LunaElement* se = si >= 0 ? luna_element_at(si) : NULL;
    float sw = se && se->w > 1.0f ? se->w : 24.0f;
    float left = (sw - w) * 0.5f;
    if (se && se->w > 1.0f && luna_window_width > 40.0f) {
        float abs_left = se->x + left;
        if (abs_left + w > luna_window_width - 8.0f) abs_left = luna_window_width - 8.0f - w;
        if (abs_left < 8.0f) abs_left = 8.0f;
        left = abs_left - se->x;
    }
    char style[96];
    snprintf(style, sizeof(style), "width:%dpx;height:%dpx;left:%dpx;transform:none;",
             (int)w, 12 + lines * 17, (int)left);
    luna_dom_set_attr(ti, "style", style);
}

static void shell_sync_tray_window_tips(void) {
    for (int s = 0; s < MAX_TRAY_SLOTS; s++) {
        if (!tray_key_is_window(g_tray_slot_key[s])) continue;
        char tip[192];
        tray_window_tooltip(tip, sizeof(tip), g_tray_slot_key[s],
                            tray_entry_for_key(g_tray_slot_key[s]));
        if (tip[0]) tray_tip_set(s, tip);
    }
}

/* Tooltip text of a status slot: SNI title/tooltip, or the compositor entry. */
static void tray_status_tooltip(char* out, size_t n, int slot, const LunaTrayEntry* t) {
    const char* raw = "";
    if (g_tray_sni_kind[slot]) {
        for (int i = 0; i < g_sni.count; i++) {
            const LunaSniItem* it = &g_sni.items[i];
            if (strcmp(it->path[0] ? it->path : it->id, g_tray_slot_key[slot])) continue;
            raw = it->tooltip[0] ? it->tooltip : it->title[0] ? it->title : it->id;
            break;
        }
    } else if (t) {
        raw = t->tooltip[0] ? t->tooltip : t->label;
    }
    tip_fit(out, n, raw, 64);
}

static void tray_sync_window_tips_and_visibility(void) {
    int visible = 0;
    for (int s = 0; s < MAX_TRAY_SLOTS; s++) {
        const char* key = g_tray_slot_key[s];
        int is_window = tray_key_is_window(key);
        int hide_window = !g_settings.window_tray_icons && is_window;
        LunaTrayEntry* t = tray_entry_for_key(key);
        if (g_tray_tip_idx[s] >= 0) {
            char tip[192];
            if (is_window) tray_window_tooltip(tip, sizeof(tip), key, t);
            else if (key && *key) tray_status_tooltip(tip, sizeof(tip), s, t);
            else tip[0] = 0;
            tray_tip_set(s, tip);
            /* No text, no bubble: an empty dark box looked like a glitch. */
            set_hidden(g_tray_tip_idx[s], tip[0] == 0);
        }
        if (hide_window && g_tray_slot_idx[s] >= 0) {
            set_hidden(g_tray_slot_idx[s], 1);
        } else if (key && *key && g_tray_slot_idx[s] >= 0) {
            visible++;
        }
    }
    if (g_tray_area_idx >= 0)
        set_hidden(g_tray_area_idx, visible == 0);
}


/* ── Tray / menubar icon order ──
 * Orders are '|'-separated key lists saved in settings.  A key that is in the
 * list ranks by its position; anything else keeps its default place after. */
typedef struct { int kind, ref, rank; char key[64]; } TrayCand;
/* More icons than fit in ~40% of the bar: group windows by application. */
static int tray_soft_limit(void) {
    int n = (int)(luna_window_width * 0.4f / 27.0f);
    return n < 6 ? 6 : n > 28 ? 28 : n;
}

static int order_rank(const char* list, const char* key, int dflt) {
    size_t kn = strlen(key);
    int pos = 0;
    for (const char* p = list; p && *p; pos++) {
        const char* e = strchr(p, '|');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == kn && !strncmp(p, key, n)) return pos;
        if (!e) break;
        p = e + 1;
    }
    return dflt;
}

static void order_store(char* list, size_t cap, char keys[][64], int n) {
    size_t used = 0;
    list[0] = 0;
    for (int i = 0; i < n; i++) {
        if (strchr(keys[i], '|')) continue;
        int w = snprintf(list + used, cap - used, "%s%s", used ? "|" : "", keys[i]);
        if (w < 0 || (size_t)w >= cap - used) { list[used] = 0; break; }
        used += (size_t)w;
    }
}

/* Stable insertion sort by rank. */
static void tray_cand_sort(TrayCand* c, int n) {
    for (int i = 1; i < n; i++) {
        TrayCand t = c[i];
        int j = i - 1;
        while (j >= 0 && c[j].rank > t.rank) { c[j + 1] = c[j]; j--; }
        c[j + 1] = t;
    }
}

/* Running-app icons follow the order the windows first appeared in. */
static int tray_window_rank(const char* key) {
    uint64_t sid = tray_lookup_surface(key);
    const char* app = !strncmp(key, "app:", 4) ? key + 4 : NULL;
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].id == sid || (app && !strcmp(g_wins[i].app_id, app))) return i;
    return MAX_WINDOWS;
}

static void update_tray_ui(void) {
    /* Windowed GLFW/X11 preview is not a session host: leave the tray
     * collapsed so it neither paints empty chrome nor steals clicks.
     * luna-session always passes --desktop. */
    if (!g_desktop_mode) {
        if (g_tray_area_idx >= 0) set_hidden(g_tray_area_idx, 1);
        for (int s = 0; s < MAX_TRAY_SLOTS; s++) {
            if (g_tray_slot_idx[s] >= 0) {
                set_hidden(g_tray_slot_idx[s], 1);
                luna_set_background_image(g_tray_slot_idx[s], NULL);
            }
            g_tray_slot_key[s][0] = 0;
            g_tray_sni_kind[s] = 0;
        }
        luna_mark_layout_dirty();
        return;
    }

    /* Candidates: SNI + compositor status icons ordered by the user's list,
     * then running-app entries in launch order.  The old code filled slots
     * first-come-first-served from a table of eight, so a few status icons
     * silently pushed running apps out of the tray. */
    TrayCand cs[MAX_TRAY_SLOTS + MAX_TRAY_ENTRIES], cw[MAX_TRAY_ENTRIES];
    int ns = 0, nw = 0;
    for (int i = 0; i < g_sni.count && ns < MAX_TRAY_SLOTS; i++) {
        const LunaSniItem* it = &g_sni.items[i];
        cs[ns].kind = 0; cs[ns].ref = i;
        snprintf(cs[ns].key, sizeof(cs[ns].key), "%.63s", it->path[0] ? it->path : it->id);
        cs[ns].rank = order_rank(g_settings.order_tray, cs[ns].key, 1000 + ns);
        ns++;
    }
    for (int i = 0; i < g_tray_count; i++) {
        LunaTrayEntry* t = &g_tray[i];
        if (tray_compositor_is_status(t) && ns < MAX_TRAY_SLOTS + MAX_TRAY_ENTRIES) {
            cs[ns].kind = 1; cs[ns].ref = i;
            snprintf(cs[ns].key, sizeof(cs[ns].key), "%.63s", t->id);
            cs[ns].rank = order_rank(g_settings.order_tray, cs[ns].key, 1000 + ns);
            ns++;
        } else if (g_settings.window_tray_icons && tray_key_is_window(t->id) && nw < MAX_TRAY_ENTRIES) {
            cw[nw].kind = 2; cw[nw].ref = i;
            snprintf(cw[nw].key, sizeof(cw[nw].key), "%.63s", t->id);
            cw[nw].rank = tray_window_rank(t->id);
            nw++;
        }
    }
    tray_cand_sort(cs, ns);
    tray_cand_sort(cw, nw);
    /* Never drop an icon.  When the tray would get crowded, windows of one
     * application collapse into a single "app:" icon (click opens a window
     * chooser) instead; the slot table is large enough for every icon. */
    if (ns + nw > tray_soft_limit()) {
        int m = 0;
        for (int i = 0; i < nw; i++) {
            const char* app = tray_key_app_id(cw[i].key);
            int joined = 0;
            if (app[0] && strlen(app) < 58) {
                for (int j = 0; j < m && !joined; j++)
                    if (!strcmp(tray_key_app_id(cw[j].key), app) && !strncmp(cw[j].key, "app:", 4)) joined = 1;
            }
            if (joined) continue;
            cw[m] = cw[i];
            if (app[0] && strlen(app) < 58) snprintf(cw[m].key, sizeof(cw[m].key), "app:%s", app);
            m++;
        }
        nw = m;
    }
    int win_keep = nw, stat_keep = ns;
    if (stat_keep + win_keep > MAX_TRAY_SLOTS) win_keep = MAX_TRAY_SLOTS - stat_keep > 0 ? MAX_TRAY_SLOTS - stat_keep : 0;

    int used = 0;
    for (int k = 0; k < stat_keep + win_keep; k++, used++) {
        const TrayCand* c = k < stat_keep ? &cs[k] : &cw[k - stat_keep];
        int idx = g_tray_slot_idx[used];
        if (idx < 0) continue;
        set_hidden(idx, 0);
        g_tray_sni_service[used][0] = 0;
        g_tray_sni_path[used][0] = 0;
        snprintf(g_tray_slot_key[used], sizeof(g_tray_slot_key[used]), "%s", c->key);
        if (c->kind == 0) {
            const LunaSniItem* it = &g_sni.items[c->ref];
            g_tray_sni_kind[used] = 1;
            snprintf(g_tray_sni_service[used], sizeof(g_tray_sni_service[used]), "%s", it->service);
            snprintf(g_tray_sni_path[used], sizeof(g_tray_sni_path[used]), "%s", it->path);
            char png[PATH_MAX];
            if (tray_write_sni_png(used, it, png, sizeof(png)))
                tray_apply_image(idx, g_tray_glyph_idx[used], png);
            else if (it->icon_file[0])
                tray_apply_image(idx, g_tray_glyph_idx[used], it->icon_file);
            else
                tray_apply_glyph(idx, g_tray_glyph_idx[used],
                                 it->icon_name[0] ? it->icon_name : "app");
            continue;
        }
        LunaTrayEntry* t = &g_tray[c->ref];
        g_tray_sni_kind[used] = 0;
        if (c->kind == 1) {
            tray_apply_glyph(idx, g_tray_glyph_idx[used], t->icon);
            tray_slot_style(idx, t);
            /* An X11 (XEmbed) icon is drawn by the compositor over this slot. */
            if (!strncmp(t->id, "x11:", 4) && g_tray_glyph_idx[used] >= 0)
                luna_set_text(g_tray_glyph_idx[used], "");
        } else {
            const char* icon_file = app_icon_cached(tray_key_app_id(c->key));
            if (*icon_file)
                tray_apply_image(idx, g_tray_glyph_idx[used], icon_file);
            else
                tray_apply_glyph(idx, g_tray_glyph_idx[used], t->icon);
            if (!strncmp(c->key, "app:", 4)) {
                /* A grouped icon is active when any of its windows has focus. */
                LunaTrayEntry grp = *t;
                char* suf = strstr(grp.icon, "_active");
                if (suf) *suf = 0;
                for (int w = 0; w < g_win_count; w++)
                    if (g_wins[w].focused && !strcmp(g_wins[w].app_id, c->key + 4)) {
                        snprintf(grp.icon + strlen(grp.icon), sizeof(grp.icon) - strlen(grp.icon), "_active");
                        break;
                    }
                tray_slot_style(idx, &grp);
            } else {
                tray_slot_style(idx, t);
            }
        }
    }

    for (int s = used; s < MAX_TRAY_SLOTS; s++) {
        int idx = g_tray_slot_idx[s];
        if (idx >= 0) {
            set_hidden(idx, 1);
            luna_set_background_image(idx, NULL);
        }
        g_tray_slot_key[s][0] = 0;
        g_tray_sni_kind[s] = 0;
    }
    if (g_tray_area_idx >= 0) set_hidden(g_tray_area_idx, used == 0);
    tray_sync_window_tips_and_visibility();
    luna_mark_layout_dirty();
    shell_request_repaint(1);
}

static double g_toast_deadline = 0.0;
static char   g_lp_query[160] = "";

/* Pending confirmation action */
enum { ACT_NONE = 0, ACT_SHUTDOWN, ACT_RESTART, ACT_LOGOUT, ACT_AIRDROP };
static int g_pending_action = ACT_NONE;

/* ── Small helpers ── */


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

static void dismiss_popovers(void);
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
    win_menu_detach_embed();
    g_win_menu_target = 0;
    g_win_menu_dialog = -1;
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

static void dismiss_power_menu(void) {
    if (is_shown(g_power_menu_idx)) set_hidden(g_power_menu_idx, 1);
}

/* ── Desktop (wallpaper) right-click menu ──────────────────────────────── */
static void dismiss_desk_menu(void) {
    if (is_shown(g_desk_menu_idx)) set_hidden(g_desk_menu_idx, 1);
}

/* ── Window chooser for a grouped task icon, and the active-app title menu ── */
static void dismiss_tray_menu(void) {
    if (is_shown(g_tray_menu_idx)) set_hidden(g_tray_menu_idx, 1);
}

static void dismiss_weather_menu(void) {
    if (is_shown(g_weather_menu_idx)) set_hidden(g_weather_menu_idx, 1);
}

static void dismiss_calendar_menu(void) {
    if (is_shown(g_calendar_menu_idx)) set_hidden(g_calendar_menu_idx, 1);
}

static void on_launch_app(LunaElement* e);
static int wifi_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (str_has_prefix(id, "wifi_") && isdigit((unsigned char)id[5])) return atoi(id + 5);
    }
    return -1;
}

static int eth_row_number(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (str_has_prefix(id, "eth_") && isdigit((unsigned char)id[4])) return atoi(id + 4);
    }
    return -1;
}

static void net_tip_update(void);
static void net_detail_open(int is_eth, int index);
static void net_detail_close(LunaElement* e) {
    (void)e;
    g_net_detail_kind = -1;
    g_net_detail_index = -1;
    if (g_net_detail_idx >= 0) set_hidden(g_net_detail_idx, 1);
}

static void position_menu_near(int menu_idx, int anchor_idx, float fallback_x) {
    if (menu_idx < 0) return;
    /* Opening handlers expose the popup before calling us.  Resolve its
     * skin-specific auto/content height now; measuring the display:none box
     * used a stale base-skin height and put tall bottom-edge menus mostly
     * below the output. */
    luna_update(g_now, 0.0);
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
        /* After the top clamp, cap height so the menu does not grow through
         * the taskbar (common on windows-* skins with tall Start / Wi-Fi). */
        {
            float avail = max_bottom - y;
            if (avail > 48.0f && mh > avail) {
                m->has_max_height = 1;
                m->css_max_height = floorf(avail);
                m->max_height_pct = 0;
                m->raw_max_height = m->css_max_height;
                m->raw_max_height_off = 0.0f;
            }
        }
    } else {
        if (y + mh > luna_window_height - 8.0f)
            y = luna_window_height - mh - 8.0f;
        if (y < (float)(g_chrome_menubar_height + 2))
            y = (float)(g_chrome_menubar_height + 2);
        if (y < 6.0f) y = 6.0f;
        {
            float avail = luna_window_height - 8.0f - y;
            if (avail > 48.0f && mh > avail) {
                m->has_max_height = 1;
                m->css_max_height = floorf(avail);
                m->max_height_pct = 0;
                m->raw_max_height = m->css_max_height;
                m->raw_max_height_off = 0.0f;
            }
        }
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
    if (!snapshot.busy && snapshot.error[0])
        toast_show("Wi-Fi error", snapshot.error, 6.0);
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

static void thermal_menus_tick(void);
static int text_would_change(int idx, const char* buf) {
    if (idx < 0 || !buf) return 0;
    LunaElement* e = luna_element_at(idx);
    return e && strcmp(e->text, buf) != 0;
}


static void wifi_tick(void) {
    wifi_consume_snapshot();
    eth_consume_snapshot();
    bt_consume_snapshot();
    if (luna_sni_consume(&g_sni, &g_sni_generation))
        update_tray_ui();
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
    if (is_shown(g_power_menu_idx))
        thermal_menus_tick();
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
    if (stat(path, &st) == 0) return 1;
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", name);
    return lstat(path, &st) == 0;
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

/* ── Calendar popover + alarms ── */

#define MAX_ALARMS 24
#define MAX_CAL_ALARM_ROWS 4
#define ALARM_REPEAT_ONCE 0
#define ALARM_REPEAT_DAILY 1
#define ALARM_REPEAT_WEEKDAYS 2

typedef struct {
    int year, month, day; /* month 0-11; date used by once-alarms */
    int hour, min;
    int repeat;
    int enabled;
    char label[48];
    time_t last_fired_slot; /* wall-clock minute of last ring */
    time_t snooze_until;    /* 0 = none */
} LunaAlarm;

static LunaAlarm g_alarms[MAX_ALARMS];
static int g_alarm_count = 0;
static int g_alarm_repeat_mode = ALARM_REPEAT_ONCE;
static int g_alarm_ringing = -1;
static int g_cal_alarm_row_id[MAX_CAL_ALARM_ROWS];
static double g_alarm_next_deadline = 0.0;

static void alarms_path(char* buf, size_t n) {
    char dir[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-shell", 0) ||
        !path_join2(buf, n, dir, "alarms.conf"))
        buf[0] = 0;
}

static void alarms_save(void) {
    ensure_config_dir();
    char path[512];
    alarms_path(path, sizeof(path));
    if (!path[0]) return;
    char tmp[576];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return;
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# luna-shell alarms — year month day hour min repeat enabled label\n");
    for (int i = 0; i < g_alarm_count; i++) {
        const LunaAlarm* a = &g_alarms[i];
        fprintf(f, "%d %d %d %d %d %d %d %s\n",
                a->year, a->month + 1, a->day, a->hour, a->min,
                a->repeat, a->enabled, a->label[0] ? a->label : "Alarm");
    }
    if (fclose(f) != 0) { unlink(tmp); return; }
    if (rename(tmp, path) != 0) unlink(tmp);
}

static void alarms_load(void) {
    g_alarm_count = 0;
    g_alarm_ringing = -1;
    g_alarm_repeat_mode = ALARM_REPEAT_ONCE;
    char path[512];
    alarms_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f) && g_alarm_count < MAX_ALARMS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0) continue;
        LunaAlarm a;
        memset(&a, 0, sizeof(a));
        int month1 = 0, enabled = 1;
        char label[80] = "";
        if (sscanf(line, "%d %d %d %d %d %d %d %79[^\n]",
                   &a.year, &month1, &a.day, &a.hour, &a.min,
                   &a.repeat, &enabled, label) < 7)
            continue;
        if (month1 < 1 || month1 > 12) continue;
        if (a.hour < 0 || a.hour > 23 || a.min < 0 || a.min > 59) continue;
        if (a.repeat < 0 || a.repeat > 2) a.repeat = 0;
        a.month = month1 - 1;
        a.enabled = enabled != 0;
        trim_line(label);
        if (!label[0]) snprintf(label, sizeof(label), "Alarm");
        snprintf(a.label, sizeof(a.label), "%s", label);
        g_alarms[g_alarm_count++] = a;
    }
    fclose(f);
}

static const char* alarm_repeat_label(int repeat) {
    if (repeat == ALARM_REPEAT_DAILY) return "Daily";
    if (repeat == ALARM_REPEAT_WEEKDAYS) return "Weekdays";
    return "Once";
}

static int alarm_applies_to_day(const LunaAlarm* a, int year, int month, int day) {
    if (!a || !a->enabled) return 0;
    if (a->repeat == ALARM_REPEAT_DAILY) return 1;
    if (a->repeat == ALARM_REPEAT_WEEKDAYS) {
        struct tm first = {0};
        first.tm_year = year - 1900;
        first.tm_mon = month;
        first.tm_mday = day;
        first.tm_isdst = -1;
        if (mktime(&first) == (time_t)-1) return 0;
        return first.tm_wday >= 1 && first.tm_wday <= 5;
    }
    return a->year == year && a->month == month && a->day == day;
}

static int calendar_day_has_alarm(int year, int month, int day) {
    for (int i = 0; i < g_alarm_count; i++)
        if (alarm_applies_to_day(&g_alarms[i], year, month, day)) return 1;
    return 0;
}

static int alarm_parse_time(const char* s, int* hour, int* min) {
    char buf[16];
    size_t n = 0;
    if (!s) s = "";
    while (*s && isspace((unsigned char)*s)) s++;
    while (s[n] && n + 1 < sizeof(buf)) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = 0;
    for (char* p = buf; *p; p++)
        if (*p == '.' || *p == ' ') *p = ':';
    int h = -1, m = 0;
    if (strchr(buf, ':')) {
        if (sscanf(buf, "%d:%d", &h, &m) != 2) return 0;
    } else {
        if (n == 0 || n > 4) return 0;
        for (size_t i = 0; i < n; i++)
            if (!isdigit((unsigned char)buf[i])) return 0;
        int v = atoi(buf);
        if (n <= 2) { h = v; m = 0; }
        else { h = v / 100; m = v % 100; }
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    *hour = h;
    *min = m;
    return 1;
}

static void calendar_alarm_preset_time(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    char buf[8];
    int idx;
    if (!localtime_r(&now, &tm_now)) return;
    int h = tm_now.tm_hour;
    int m = ((tm_now.tm_min + 9) / 5) * 5;
    if (m >= 60) { m = 0; h = (h + 1) % 24; }
    if (h < 0 || h > 23) h = 0;
    if (m < 0 || m > 59) m = 0;
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    idx = luna_get_element_by_id("cal_alarm_time");
    if (idx >= 0) luna_set_value(idx, buf);
}

static void calendar_alarm_refresh(void) {
    static const char* months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char date[48], count[24];
    int shown = 0, matching = 0;
    int date_idx = luna_get_element_by_id("cal_alarm_date");
    int count_idx = luna_get_element_by_id("cal_alarm_count");
    int empty_idx = luna_get_element_by_id("cal_alarm_empty");
    int repeat_idx = luna_get_element_by_id("cal_alarm_repeat");

    for (int i = 0; i < MAX_CAL_ALARM_ROWS; i++)
        g_cal_alarm_row_id[i] = -1;

    if (g_cal_selected_day > 0)
        snprintf(date, sizeof(date), "%s %d",
                 months[g_cal_month], g_cal_selected_day);
    else
        snprintf(date, sizeof(date), "Pick a day");
    if (date_idx >= 0) luna_set_text(date_idx, date);

    if (repeat_idx >= 0)
        luna_set_text(repeat_idx, alarm_repeat_label(g_alarm_repeat_mode));

    for (int i = 0; i < g_alarm_count; i++) {
        if (g_cal_selected_day > 0 &&
            alarm_applies_to_day(&g_alarms[i], g_cal_year, g_cal_month,
                                 g_cal_selected_day))
            matching++;
    }
    if (matching == 0) snprintf(count, sizeof(count), "No alarms");
    else if (matching == 1) snprintf(count, sizeof(count), "1 alarm");
    else snprintf(count, sizeof(count), "%d alarms", matching);
    if (count_idx >= 0) luna_set_text(count_idx, count);

    for (int i = 0; i < g_alarm_count && shown < MAX_CAL_ALARM_ROWS; i++) {
        char id[24], timebuf[8];
        int row, t, l, r;
        if (g_cal_selected_day < 1 ||
            !alarm_applies_to_day(&g_alarms[i], g_cal_year, g_cal_month,
                                  g_cal_selected_day))
            continue;
        snprintf(id, sizeof(id), "cal_al%d", shown);
        row = luna_get_element_by_id(id);
        if (row < 0) continue;
        set_hidden(row, 0);
        snprintf(timebuf, sizeof(timebuf), "%02d:%02d",
                 g_alarms[i].hour, g_alarms[i].min);
        t = -1; l = -1; r = -1;
        for (int c = 0; c < luna_element_count(); c++) {
            LunaElement* e = luna_element_at(c);
            if (!e || e->parent_idx != row) continue;
            if (strstr(e->class_name, "cal_al_time")) t = c;
            else if (strstr(e->class_name, "cal_al_label")) l = c;
            else if (strstr(e->class_name, "cal_al_repeat")) r = c;
        }
        if (t >= 0) luna_set_text(t, timebuf);
        if (l >= 0) luna_set_text(l, g_alarms[i].label[0] ? g_alarms[i].label : "Alarm");
        if (r >= 0) luna_set_text(r, alarm_repeat_label(g_alarms[i].repeat));
        g_cal_alarm_row_id[shown] = i;
        shown++;
    }
    for (int i = shown; i < MAX_CAL_ALARM_ROWS; i++) {
        char id[24];
        snprintf(id, sizeof(id), "cal_al%d", i);
        set_hidden(luna_get_element_by_id(id), 1);
    }
    if (empty_idx >= 0) set_hidden(empty_idx, shown ? 1 : 0);
}

static void alarm_hide_ring(void) {
    g_alarm_ringing = -1;
    if (is_shown(g_alarm_ring_idx)) set_hidden(g_alarm_ring_idx, 1);
    if (g_mb_clock_idx >= 0)
        luna_update_classes(g_mb_clock_idx, "ringing", NULL);
    shell_request_repaint(-1);
}

static void alarm_show_ring(int idx) {
    static const char* wdays[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };
    static const char* months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    LunaAlarm* a;
    time_t now = time(NULL);
    struct tm tm_now;
    char timebuf[8], when[64];
    if (idx < 0 || idx >= g_alarm_count) return;
    a = &g_alarms[idx];
    g_alarm_ringing = idx;
    a->snooze_until = 0;
    a->last_fired_slot = now / 60;
    if (!localtime_r(&now, &tm_now)) memset(&tm_now, 0, sizeof(tm_now));
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d", a->hour, a->min);
    snprintf(when, sizeof(when), "%s · %s %d",
             wdays[tm_now.tm_wday], months[tm_now.tm_mon], tm_now.tm_mday);
    {
        int t = luna_get_element_by_id("alarm_ring_time");
        int l = luna_get_element_by_id("alarm_ring_label");
        int w = luna_get_element_by_id("alarm_ring_when");
        if (t >= 0) luna_set_text(t, timebuf);
        if (l >= 0) luna_set_text(l, a->label[0] ? a->label : "Alarm");
        if (w >= 0) luna_set_text(w, when);
    }
    set_hidden(g_alarm_ring_idx, 0);
    if (g_mb_clock_idx >= 0)
        luna_update_classes(g_mb_clock_idx, NULL, "ringing");
    toast_show("Alarm", a->label[0] ? a->label : "It's time", 8.0);
    (void)write(STDERR_FILENO, "\a", 1);
    shell_request_repaint(-1);
}

static int alarm_due_now(const LunaAlarm* a, const struct tm* now, time_t now_t) {
    if (!a || !a->enabled) return 0;
    if (a->snooze_until > 0)
        return now_t >= a->snooze_until;
    if (a->last_fired_slot == now_t / 60) return 0;
    if (a->hour != now->tm_hour || a->min != now->tm_min) return 0;
    if (a->repeat == ALARM_REPEAT_DAILY) return 1;
    if (a->repeat == ALARM_REPEAT_WEEKDAYS)
        return now->tm_wday >= 1 && now->tm_wday <= 5;
    return a->year == now->tm_year + 1900 &&
           a->month == now->tm_mon &&
           a->day == now->tm_mday;
}

static double alarm_poll_deadline(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    time_t soonest;
    if (!localtime_r(&now, &tm_now)) return 0.0;
    soonest = now + (tm_now.tm_sec < 59 ? (60 - tm_now.tm_sec) : 1);
    for (int i = 0; i < g_alarm_count; i++) {
        if (!g_alarms[i].enabled) continue;
        if (g_alarms[i].snooze_until > now && g_alarms[i].snooze_until < soonest)
            soonest = g_alarms[i].snooze_until;
    }
    g_alarm_next_deadline = g_now + (double)(soonest - now);
    if (g_alarm_next_deadline <= g_now) g_alarm_next_deadline = g_now + 0.25;
    return g_alarm_next_deadline;
}

static void alarm_tick(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    if (g_alarm_ringing >= 0) return;
    if (!localtime_r(&now, &tm_now)) return;
    for (int i = 0; i < g_alarm_count; i++) {
        if (!alarm_due_now(&g_alarms[i], &tm_now, now)) continue;
        alarm_show_ring(i);
        alarm_poll_deadline();
        return;
    }
    alarm_poll_deadline();
}

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
        {
            char cls[48] = "";
            int has_alarm = 0;
            if (!other && calendar_day_has_alarm(g_cal_year, g_cal_month, day))
                has_alarm = 1;
            if (other) snprintf(cls, sizeof(cls), "other");
            else {
                if (is_today) strcat(cls, "today");
                if (is_sel) {
                    if (cls[0]) strcat(cls, " ");
                    strcat(cls, "selected");
                }
                if (has_alarm) {
                    if (cls[0]) strcat(cls, " ");
                    strcat(cls, "has_alarm");
                }
            }
            luna_update_classes(idx, "other today selected has_alarm",
                                cls[0] ? cls : NULL);
        }
    }
    calendar_alarm_refresh();
}


static double stopwatch_elapsed_now(void) {
    double elapsed = g_stopwatch_elapsed;
    if (g_stopwatch_running) {
        double delta = g_now - g_stopwatch_started_at;
        if (delta > 0.0) elapsed += delta;
    }
    return elapsed > 0.0 ? elapsed : 0.0;
}

static void stopwatch_format_time(char* out, size_t n, double elapsed) {
    unsigned long long tenths = (unsigned long long)floor(elapsed * 10.0 + 0.000001);
    unsigned long long hours = tenths / 36000ULL;
    unsigned long long minutes = (tenths / 600ULL) % 60ULL;
    unsigned long long seconds = (tenths / 10ULL) % 60ULL;
    unsigned long long tenth = tenths % 10ULL;
    if (hours > 0ULL)
        snprintf(out, n, "%llu:%02llu:%02llu.%llu", hours, minutes, seconds, tenth);
    else
        snprintf(out, n, "%02llu:%02llu.%llu", minutes, seconds, tenth);
}

static void stopwatch_set_text_if_changed(int idx, const char* text, int paint_only) {
    if (idx < 0 || !text) return;
    LunaElement* el = luna_element_at(idx);
    if (!el || strcmp(el->text, text) == 0) return;
    if (paint_only) luna_set_text_paint_only(idx, text);
    else luna_set_text(idx, text);
}

static void stopwatch_render(void) {
    char buf[32];
    double elapsed = stopwatch_elapsed_now();
    stopwatch_format_time(buf, sizeof(buf), elapsed);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_TIME], buf, 1);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_TOGGLE],
                                  g_stopwatch_running ? "Pause" :
                                  (elapsed > 0.0 ? "Resume" : "Start"), 0);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_STATE],
                                  g_stopwatch_running ? "Running" :
                                  (elapsed > 0.0 ? "Paused" : "Ready"), 0);
    if (g_stopwatch_mode) shell_request_repaint(0);
}

static void widgets_sync_layout_classes(void) {
    int g = g_settings.stats_style == 1, sw = g_stopwatch_mode;
    luna_update_classes(g_ui_idx[UI_WIDGET_STATS], g ? NULL : "stats_graph", g ? "stats_graph" : NULL);
    /* The weather card sits below the statistics card, whose height depends on
     * the style, and below the stopwatch when that is open. */
    luna_update_classes(g_ui_idx[UI_WIDGET_WEATHER], "clock_below_stopwatch wx_g wx_g_sw",
                        g && sw ? "wx_g_sw" : sw ? "clock_below_stopwatch" : g ? "wx_g" : NULL);
}

static void settings_mark_stats(void) {
    static const struct { const char* id; int v; } styles[] = { { "st_style_bar", 0 }, { "st_style_graph", 1 } };
    for (int i = 0; i < 2; i++) {
        int idx = luna_get_element_by_id(styles[i].id);
        if (idx >= 0) luna_update_classes(idx, "selected", g_settings.stats_style == styles[i].v ? "selected" : NULL);
    }
    static const struct { const char* id; int v; } ints[] = {
        { "st_int_2", 2 }, { "st_int_5", 5 }, { "st_int_10", 10 }, { "st_int_30", 30 }, { "st_int_0", 0 } };
    for (int i = 0; i < 5; i++) {
        int idx = luna_get_element_by_id(ints[i].id);
        if (idx >= 0) luna_update_classes(idx, "selected", g_settings.stats_interval == ints[i].v ? "selected" : NULL);
    }
}

static void settings_mark_toggle(const char* id, int enabled) {
    int idx = luna_get_element_by_id(id);
    if (idx < 0) return;
    luna_update_classes(idx, "on", enabled ? "on" : NULL);
    if (!strcmp(id, "wm_wallpaper_motion"))
        luna_set_text(idx, enabled ? "Wallpaper motion · On" : "Wallpaper motion · Off");
}

static void on_settings_tab(LunaElement* e);

static void stopwatch_set_mode(int enabled) {
    enabled = enabled != 0;
    g_stopwatch_mode = enabled;
    set_hidden(g_ui_idx[UI_WG_CLOCK_VIEW], enabled);
    set_hidden(g_ui_idx[UI_WG_STOPWATCH_VIEW], !enabled);
    luna_update_classes(g_ui_idx[UI_WIDGET_CLOCK],
                        enabled ? NULL : "stopwatch_mode",
                        enabled ? "stopwatch_mode" : NULL);
    luna_update_classes(g_ui_idx[UI_WIDGET_STATS],
                        enabled ? NULL : "clock_below_stopwatch",
                        enabled ? "clock_below_stopwatch" : NULL);
    widgets_sync_layout_classes();
    g_stopwatch_next_deadline = (enabled && g_stopwatch_running) ? g_now : 0.0;
    if (enabled) stopwatch_render();
    luna_mark_layout_dirty();
    shell_sync_bg_input_region();
    shell_request_repaint(0);
}

static void on_stopwatch_open(LunaElement* e) {
    (void)e;
    dismiss_calendar_menu();
    stopwatch_set_mode(1);
}

static void on_stopwatch_back(LunaElement* e) {
    (void)e;
    stopwatch_set_mode(0);
}

static void on_stopwatch_toggle(LunaElement* e) {
    (void)e;
    if (g_stopwatch_running) {
        double delta = g_now - g_stopwatch_started_at;
        if (delta > 0.0) g_stopwatch_elapsed += delta;
        g_stopwatch_running = 0;
        g_stopwatch_next_deadline = 0.0;
    } else {
        g_stopwatch_started_at = g_now;
        g_stopwatch_running = 1;
        g_stopwatch_next_deadline = g_now;
    }
    stopwatch_render();
}

static void on_stopwatch_reset(LunaElement* e) {
    (void)e;
    g_stopwatch_elapsed = 0.0;
    if (g_stopwatch_running) {
        g_stopwatch_started_at = g_now;
        g_stopwatch_next_deadline = g_now;
    }
    stopwatch_render();
}

static void stopwatch_tick(void) {
    if (!g_stopwatch_running || !g_stopwatch_mode) return;
    if (g_stopwatch_next_deadline > 0.0 && g_now < g_stopwatch_next_deadline) return;
    stopwatch_render();
    g_stopwatch_next_deadline = g_now + 0.1;
}

static void on_calendar_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
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
    calendar_alarm_preset_time();
    set_hidden(g_calendar_menu_idx, 0);
    position_menu_near(g_calendar_menu_idx,
                       menu_anchor_from(e, g_mb_clock_idx >= 0 ? g_mb_clock_idx
                                        : luna_get_element_by_id("mb_clock")),
                       luna_window_width - 330.0f);
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

static void on_calendar_alarm_repeat(LunaElement* e) {
    (void)e;
    g_alarm_repeat_mode = (g_alarm_repeat_mode + 1) % 3;
    calendar_alarm_refresh();
}

static void on_calendar_alarm_add(LunaElement* e) {
    int ti = luna_get_element_by_id("cal_alarm_time");
    int ni = luna_get_element_by_id("cal_alarm_note");
    const char* time_s = ti >= 0 ? luna_get_value(ti) : "";
    const char* note_s = ni >= 0 ? luna_get_value(ni) : "";
    LunaAlarm a;
    (void)e;
    memset(&a, 0, sizeof(a));
    if (g_cal_selected_day < 1) {
        toast_show("Alarm", "Pick a day on the calendar first", 2.5);
        return;
    }
    if (g_alarm_count >= MAX_ALARMS) {
        toast_show("Alarm", "Alarm list is full", 2.5);
        return;
    }
    if (!alarm_parse_time(time_s, &a.hour, &a.min)) {
        toast_show("Alarm", "Use a time like 07:30", 2.5);
        return;
    }
    a.year = g_cal_year;
    a.month = g_cal_month;
    a.day = g_cal_selected_day;
    a.repeat = g_alarm_repeat_mode;
    a.enabled = 1;
    {
        const char* src = note_s ? note_s : "";
        size_t i = 0;
        while (*src && isspace((unsigned char)*src)) src++;
        while (src[i] && i + 1 < sizeof(a.label)) {
            a.label[i] = src[i];
            i++;
        }
        a.label[i] = 0;
        trim_line(a.label);
    }
    if (!a.label[0]) snprintf(a.label, sizeof(a.label), "Alarm");
    g_alarms[g_alarm_count++] = a;
    alarms_save();
    calendar_fill();
    alarm_poll_deadline();
    toast_show("Alarm", a.label, 2.0);
}

static void on_calendar_alarm_delete(LunaElement* e) {
    int idx = elem_idx_of(e);
    const char* id;
    int row = -1, alarm_i;
    if (idx < 0) return;
    id = luna_element_at(idx)->id;
    if (!str_has_prefix(id, "cal_al")) return;
    row = atoi(id + 6);
    if (row < 0 || row >= MAX_CAL_ALARM_ROWS) {
        /* clicks on the glyph inherit the parent id; walk up */
        for (int p = luna_element_at(idx)->parent_idx; p != -1;
             p = luna_element_at(p)->parent_idx) {
            const char* pid = luna_element_at(p)->id;
            if (str_has_prefix(pid, "cal_al") && strstr(pid, "_del")) {
                row = atoi(pid + 6);
                break;
            }
            if (str_has_prefix(pid, "cal_al")) {
                row = atoi(pid + 6);
                break;
            }
        }
    }
    if (row < 0 || row >= MAX_CAL_ALARM_ROWS) return;
    alarm_i = g_cal_alarm_row_id[row];
    if (alarm_i < 0 || alarm_i >= g_alarm_count) return;
    if (g_alarm_ringing == alarm_i) alarm_hide_ring();
    else if (g_alarm_ringing > alarm_i) g_alarm_ringing--;
    memmove(&g_alarms[alarm_i], &g_alarms[alarm_i + 1],
            (size_t)(g_alarm_count - alarm_i - 1) * sizeof(g_alarms[0]));
    g_alarm_count--;
    alarms_save();
    calendar_fill();
    alarm_poll_deadline();
}

static void on_alarm_snooze(LunaElement* e) {
    (void)e;
    if (g_alarm_ringing < 0 || g_alarm_ringing >= g_alarm_count) {
        alarm_hide_ring();
        return;
    }
    g_alarms[g_alarm_ringing].snooze_until = time(NULL) + 5 * 60;
    g_alarms[g_alarm_ringing].last_fired_slot = time(NULL) / 60;
    alarm_hide_ring();
    alarm_poll_deadline();
    toast_show("Alarm", "Snoozed for 5 minutes", 2.5);
}

static void on_alarm_dismiss(LunaElement* e) {
    (void)e;
    if (g_alarm_ringing >= 0 && g_alarm_ringing < g_alarm_count) {
        LunaAlarm* a = &g_alarms[g_alarm_ringing];
        a->snooze_until = 0;
        if (a->repeat == ALARM_REPEAT_ONCE) {
            a->enabled = 0;
            memmove(a, a + 1,
                    (size_t)(g_alarm_count - g_alarm_ringing - 1) * sizeof(*a));
            g_alarm_count--;
            alarms_save();
        }
    }
    alarm_hide_ring();
    calendar_fill();
    alarm_poll_deadline();
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

#define NOTIFICATION_HISTORY_MAX 8
typedef struct {
    char title[96];
    char message[192];
    char when[16];
} LunaNotificationHistoryEntry;

static LunaNotificationHistoryEntry g_notification_history[NOTIFICATION_HISTORY_MAX];
static int g_notification_history_count = 0;

static void notification_history_copy(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    if (!src) src = "";
    snprintf(dst, n, "%s", src);
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t') dst[i] = ' ';
}

static void notification_history_refresh(void) {
    int empty_idx = luna_get_element_by_id("notify_empty");
    if (empty_idx >= 0) set_hidden(empty_idx, g_notification_history_count > 0);
    for (int i = 0; i < NOTIFICATION_HISTORY_MAX; i++) {
        char row_id[24], time_id[32], title_id[32], message_id[32], legacy_id[32];
        snprintf(row_id, sizeof(row_id), "notify_%d", i);
        snprintf(time_id, sizeof(time_id), "notify_%d_time", i);
        snprintf(title_id, sizeof(title_id), "notify_%d_title", i);
        snprintf(message_id, sizeof(message_id), "notify_%d_message", i);
        snprintf(legacy_id, sizeof(legacy_id), "notify_%d_text", i);
        int row = luna_get_element_by_id(row_id);
        if (row < 0) continue;
        if (i >= g_notification_history_count) {
            set_hidden(row, 1);
            continue;
        }
        const LunaNotificationHistoryEntry* h = &g_notification_history[i];
        int time_idx = luna_get_element_by_id(time_id);
        int title_idx = luna_get_element_by_id(title_id);
        int message_idx = luna_get_element_by_id(message_id);
        if (time_idx >= 0 && title_idx >= 0 && message_idx >= 0) {
            luna_set_text(time_idx, h->when[0] ? h->when : "Now");
            luna_set_text(title_idx, h->title[0] ? h->title : "Notification");
            luna_set_text(message_idx, h->message);
            set_hidden(message_idx, h->message[0] ? 0 : 1);
        } else {
            /* Third-party skins may still expose only notify_N_text. */
            int legacy_idx = luna_get_element_by_id(legacy_id);
            if (legacy_idx >= 0) {
                char line[320];
                if (h->message[0])
                    snprintf(line, sizeof(line), "%s  %s — %s", h->when, h->title, h->message);
                else
                    snprintf(line, sizeof(line), "%s  %s", h->when, h->title);
                luna_set_text(legacy_idx, line);
            }
        }
        set_hidden(row, 0);
    }
}

static void notification_history_push(const char* title, const char* msg) {
    if (g_notification_history_count > 0) {
        int move = g_notification_history_count;
        if (move >= NOTIFICATION_HISTORY_MAX) move = NOTIFICATION_HISTORY_MAX - 1;
        memmove(&g_notification_history[1], &g_notification_history[0],
      (size_t)move * sizeof(g_notification_history[0]));
    }
    LunaNotificationHistoryEntry* h = &g_notification_history[0];
    memset(h, 0, sizeof(*h));
    notification_history_copy(h->title, sizeof(h->title), title ? title : "Luna");
    notification_history_copy(h->message, sizeof(h->message), msg ? msg : "");
    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info))
        (void)strftime(h->when, sizeof(h->when), "%H:%M", &tm_info);
    if (g_notification_history_count < NOTIFICATION_HISTORY_MAX)
        g_notification_history_count++;
    notification_history_refresh();
}

static void toast_show(const char* title, const char* msg, double secs) {
    notification_history_push(title, msg);
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

/* Rewrite (or create) gtk settings.ini so gtk-theme-name matches the skin.
 * Other Settings keys are preserved when present. */
static void gtk_ini_set_theme(const char* path, const char* theme_name,
                              const char* layout, const char* font_name) {
    if (!path || !layout || !*layout) return;
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return;
    FILE* in = fopen(path, "r");
    FILE* out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return;
    }
    const char* keys[] = { "gtk-theme-name", "gtk-decoration-layout",
                           "gtk-font-name", "gtk-xft-dpi" };
    const char* values[] = { theme_name, layout, font_name, "98304" };
    int seen[4] = { 0 };
    int in_settings = 0;
    int saw_settings = 0;
    int failed = 0;
    char line[512];
    for (;;) {
        int have_line = in && fgets(line, sizeof(line), in);
        char* p = line;
        if (have_line) while (*p == ' ' || *p == '\t') p++;
        if (!have_line || *p == '[') {
            if (in_settings) {
                fputc('\n', out);
                for (int i = 0; i < 4; i++) {
                    if (!seen[i] && values[i] && *values[i]) {
                        fprintf(out, "%s=%s\n", keys[i], values[i]);
                        seen[i] = 1;
                    }
                }
            }
            if (!have_line) break;
            in_settings = !strncmp(p, "[Settings]", 10);
            saw_settings |= in_settings;
        } else if (in_settings) {
            int skip = 0;
            for (int i = 0; i < 4; i++) {
                size_t len = strlen(keys[i]);
                if (strncmp(p, keys[i], len)) continue;
                char* eq = p + len;
                while (*eq == ' ' || *eq == '\t') eq++;
                if (*eq != '=') continue;
                if (i < 2 && values[i] && *values[i]) {
                    if (!seen[i]) fprintf(out, "%s=%s\n", keys[i], values[i]);
                    skip = 1;
                }
                seen[i] = 1;
                break;
            }
            if (skip) continue;
        }
        fputs(line, out);
    }
    if (in) {
        failed = ferror(in);
        fclose(in);
    }
    if (!saw_settings) {
        fprintf(out, "\n[Settings]\n");
        for (int i = 0; i < 4; i++)
            if (values[i] && *values[i]) fprintf(out, "%s=%s\n", keys[i], values[i]);
    }
    failed |= ferror(out);
    if (fclose(out) != 0) failed = 1;
    if (failed || rename(tmp, path) != 0) unlink(tmp);
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
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_controls %d", skin->controls_on_left);
    ok &= shell_send_cmd(cmd);
    /* Use one decoration implementation for every xdg-decoration client.
     * GTK-style clients which do not expose that protocol retain their CSD,
     * while SDL, GLFW/libdecor and Luna clients negotiate the compositor SSD.
     * This removes toolkit-specific bars and also keeps hit testing, moving,
     * resizing and window controls on the compositor's existing fast path. */
    ok &= shell_send_cmd("wm_config prefer_ssd 1");
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

/* GTK 3/4 on Wayland take fonts from GSettings (org.gnome.desktop.interface)
 * whenever no XSettings manager or settings portal exists, and ignore
 * settings.ini.  Xorg reads settings.ini instead, so the same apps came up in
 * "Adwaita Sans 11" here and in the LXAppearance font there.  Mirror the ini
 * font into GSettings so both sessions size text identically. */
static int gtk_ini_get(const char* path, const char* key, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t key_len = strlen(key);
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, key_len)) continue;
        p += key_len;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t' || *p == '"') p++;
        char* e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                         e[-1] == '\t' || e[-1] == '"')) e--;
        *e = 0;
        if (*p) { snprintf(out, n, "%s", p); found = 1; }
    }
    fclose(f);
    return found;
}

static void gsettings_mirror_gtk_font(void) {
    char ini[PATH_MAX], font[128];
    font[0] = 0;
    const char* env_font = getenv("LUNA_GTK_FONT_NAME");
    if (env_font && *env_font)
        snprintf(font, sizeof(font), "%s", env_font);
    else if (!path_join2(ini, sizeof(ini), g_xdg.config_home, "gtk-3.0/settings.ini") ||
             !gtk_ini_get(ini, "gtk-font-name", font, sizeof(font)))
        snprintf(font, sizeof(font), "Sans 9");

    const char* layout = getenv("LUNA_GTK_BUTTON_LAYOUT");
    if (!layout || !*layout) layout = "icon:minimize,maximize,close";
    static char applied[128], applied_layout[256];
    if (!strcmp(applied, font) && !strcmp(applied_layout, layout)) return;

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) {
        snprintf(applied, sizeof(applied), "%s", font);
        snprintf(applied_layout, sizeof(applied_layout), "%s", layout);
        return;
    }
    const char* sets[][3] = {
        { "org.gnome.desktop.interface", "font-name", font },
        { "org.gnome.desktop.interface", "document-font-name", font },
        { "org.gnome.desktop.interface", "text-scaling-factor", "1.0" },
        { "org.gnome.desktop.wm.preferences", "button-layout", layout },
    };
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        pid_t c = fork();
        if (c == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
            execlp("gsettings", "gsettings", "set", sets[i][0], sets[i][1], sets[i][2],
                   (char*)NULL);
            _exit(127);
        }
        if (c < 0) break;
        int st = 0;
        pid_t waited;
        do { waited = waitpid(c, &st, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 || (WIFEXITED(st) && WEXITSTATUS(st) == 127)) break;
    }
    _exit(0);
}

static int env_is_true(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0;
}


/* Whether child applications render on the GPU (dma-buf, zero-copy) rather
 * than through llvmpipe + wl_shm.  Clients use a render node, which needs no
 * DRM master, so the only real requirements are a usable node and a compositor
 * that scans out GPU buffers.  LUNA_CLIENT_RENDERER=gpu|software (or the older
 * LUNA_CLIENT_GPU) is honoured; "auto"/unset decides from the environment, the
 * same way luna-session does, so a shell started by hand behaves identically. */
static int client_gpu_enabled(void) {
    const char* mode = getenv("LUNA_CLIENT_RENDERER");
    if (mode && !strcasecmp(mode, "gpu")) return 1;
    if (mode && !strcasecmp(mode, "software")) return 0;
    if (env_is_true("LUNA_CLIENT_GPU")) return 1;
    const char* backend = getenv("LUNA_BACKEND");
    if (backend && *backend && strcasecmp(backend, "dri")) return 0;
    for (int n = 128; n < 192; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", n);
        if (access(path, R_OK | W_OK) == 0) return 1;
    }
    return 0;
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
    /* DOSBox 0.74 is SDL 1.2 via sdl12-compat.  Without these hints it forces
     * X11 (no XWayland here) or draws a borderless SDL2/libdecor window whose
     * titlebar subsurface never receives clicks, so move/min/max/close die. */
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "wayland", 0);
    if (!getenv("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR"))
        setenv("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0", 0);
    if (!getenv("SDL_VIDEO_WAYLAND_PREFER_LIBDECOR"))
        setenv("SDL_VIDEO_WAYLAND_PREFER_LIBDECOR", "0", 0);
    if (!getenv("SDL12COMPAT_ALLOW_SYSWM"))
        setenv("SDL12COMPAT_ALLOW_SYSWM", "0", 0);
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
        if (!gdk_scale || !*gdk_scale) gdk_scale = "1";
        if (!gdk_dpi || !*gdk_dpi) gdk_dpi = "1";
        if (!qt_scale || !*qt_scale) qt_scale = "1";
        if (!cursor_sz || !*cursor_sz) cursor_sz = "24";
        /* GTK parses GDK_SCALE as an integer; old configs saved "0.8". */
        char gdk_scale_int[8];
        int gs = (int)lroundf(strtof(gdk_scale, NULL));
        snprintf(gdk_scale_int, sizeof(gdk_scale_int), "%d", gs < 1 ? 1 : gs > 3 ? 3 : gs);
        setenv("GDK_SCALE", gdk_scale_int, 1);
        setenv("GDK_DPI_SCALE", gdk_dpi, 1);
        setenv("QT_SCALE_FACTOR", qt_scale, 1);
        setenv("XCURSOR_SIZE", cursor_sz, 1);
    }
    gsettings_mirror_gtk_font();
    /* luna-session may set LUNA_CLIENT_RENDERER / GSK_RENDERER.  Still reject
     * vulkan: Luna has no Vulkan WSI / dmabuf path for clients, and GTK4 then
     * stalls (Firefox dialogs never paint). */
    {
        const char* gsk = getenv("GSK_RENDERER");
        if (!client_gpu_enabled())
            setenv("GSK_RENDERER", "cairo", 1);
        else if (!gsk || !*gsk || !strcasecmp(gsk, "vulkan"))
            setenv("GSK_RENDERER", getenv("LUNA_GSK_RENDERER") ?: "gl", 1);
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
            gtk_ini_set_theme(ini, NULL, layout, font_name);
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
     * children, but let Mesa choose its matching loader/Gallium driver.
     * luna-session exports LUNA_CLIENT_RENDERER=gpu when a render node is
     * usable; honour that (and the older LUNA_CLIENT_GPU) so non-browser
     * clients can keep dmabuf zero-copy instead of SHM uploads. */
    if (!getenv("LIBGL_ALWAYS_SOFTWARE") &&
        !(client_gpu_enabled() && !env_is_true("LUNA_EGL_SOFTWARE")))
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
    char startup_wm_class[256];
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
        else if (!strcmp(key, "StartupWMClass"))
            snprintf(entry->startup_wm_class, sizeof(entry->startup_wm_class), "%s", value);
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

/* GTK3 apps whose tray icon is a GtkStatusIcon (XEmbed) only show up when they
 * run on X11, where Xwayland and the compositor's XEmbed tray take over.  The
 * session defaults every other client to Wayland.  LUNA_X11_APPS (comma list)
 * replaces this default. */
static void child_x11_hint(const char* exec_base) {
    static const char dflt[] = ",clipit,parcellite,connman-ui-gtk,pasystray,volumeicon,"
                               "xfce4-clipman,redshift-gtk,gtkperf,";
    if (!exec_base || !*exec_base || strchr(exec_base, ',')) return;
    const char* env = getenv("LUNA_X11_APPS");
    char list[512];
    snprintf(list, sizeof(list), ",%s,", env && *env ? env : dflt + 1);
    char key[NAME_MAX + 3];
    snprintf(key, sizeof(key), ",%s,", exec_base);
    if (!getenv("DISPLAY")) return;           /* no Xwayland: nothing to fall back to */
    if (strstr(list, key)) setenv("GDK_BACKEND", "x11", 1);
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
        if (have_direct) child_x11_hint(exec_basename(args.argv[0]));
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

/* Return 1 if a process whose /proc/pid/comm matches `name` is alive.
 * TASK_COMM_LEN is 16 (15 chars + NUL), so "whiz-im-wayland" is truncated. */
static int helper_comm_running(const char* name) {
    char want[16];
    if (!name || !*name) return 0;
    snprintf(want, sizeof(want), "%s", name);
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
            if (!strcmp(comm, want)) found = 1;
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
    /* LUNA_NO_HELPERS means luna-session owns the first launch.  If that
     * helper exited (EGL init used to abort whiz-im-wayland), still restart
     * it — helper_comm_running() avoids a duplicate while it is alive. */

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
        if (have_direct) child_x11_hint(exec_basename(parsed_args.argv[0]));
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


typedef struct LunaSliderIds LunaSliderIds;
static LunaSliderIds* g_bright_slider_ptr;
static LunaSliderIds* g_vol_slider_ptr;
static void cc_sliders_pull_from_system(LunaSliderIds* bright, LunaSliderIds* vol);
static void cc_sliders_flush(LunaSliderIds* bright, LunaSliderIds* vol);
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

/* Severity class for an outdoor temperature (°C); NULL = normal colour or
 * colouring disabled.  Colours live in skins/widgets.css (.wx_*). */
static const char* weather_temp_class(float t) {
    if (!g_settings.weather_colors) return NULL;
    if (t >= g_settings.weather_crit) return "wx_crit";
    if (t >= g_settings.weather_hot)  return "wx_hot";
    if (t <= g_settings.weather_cold) return "wx_cold";
    return NULL;
}

static void weather_tint(int idx, const char* cls) {
    if (idx >= 0) luna_update_classes(idx, "wx_cold wx_hot wx_crit", cls);
}

static void weather_tint_id(const char* id, const char* cls) {
    weather_tint(luna_get_element_by_id(id), cls);
}

/* Icon colour class for a WMO weather code; NULL when colouring is disabled. */
static const char* weather_icon_class(int code) {
    if (!g_settings.weather_colors) return NULL;
    if (code == 0) return "wxi_clear";
    if (code == 1 || code == 2) return "wxi_pcloud";
    if (code == 3) return "wxi_cloud";
    if (code == 45 || code == 48) return "wxi_fog";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "wxi_rain";
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "wxi_snow";
    if (code >= 95) return "wxi_storm";
    return NULL;
}

static void weather_icon_set(int idx, int code) {
    if (idx < 0) return;
    luna_set_text(idx, weather_icon_glyph(code));
    luna_update_classes(idx, "wxi_clear wxi_pcloud wxi_cloud wxi_fog wxi_rain wxi_snow wxi_storm",
                        weather_icon_class(code));
}

static void weather_icon_set_id(const char* id, int code) {
    weather_icon_set(luna_get_element_by_id(id), code);
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
            weather_icon_set(c, code);
        else if (strstr(e->class_name, "wf_temp")) {
            if (show_min)
                snprintf(buf, sizeof(buf), "%.0f° %.0f°", tmax, tmin);
            else
                snprintf(buf, sizeof(buf), "%.0f°", tmax);
            luna_set_text(c, buf);
            weather_tint(c, weather_temp_class(tmax));
        }
    }
}

static void weather_apply_ui(const WeatherData* d) {
    char buf[160];

    if (!d->ok) {
        weather_set_text_id("mb_weather_temp", "—");
        weather_icon_set_id("mb_weather_icon", -1);
        set_hidden(luna_get_element_by_id("weather_hint"), 1);
        set_hidden(luna_get_element_by_id("weather_content"), 1);
        set_hidden(luna_get_element_by_id("weather_error"), 0);
        weather_set_text_id("weather_error", d->err[0] ? d->err : "Weather unavailable");
        weather_set_text_id("wg_wx_city", "Weather");
        weather_set_text_id("wg_wx_temp", "—");
        weather_set_text_id("wg_wx_desc", d->err[0] ? d->err : "Unavailable");
        weather_tint_id("mb_weather_temp", NULL);
        weather_tint_id("weather_temp", NULL);
        weather_tint_id("wg_wx_temp", NULL);
    } else {
        snprintf(buf, sizeof(buf), "%.0f°", d->temp);
        weather_set_text_id("mb_weather_temp", buf);
        weather_icon_set_id("mb_weather_icon", d->code);

        set_hidden(luna_get_element_by_id("weather_hint"), 1);
        set_hidden(luna_get_element_by_id("weather_error"), 1);
        set_hidden(luna_get_element_by_id("weather_content"), 0);

        snprintf(buf, sizeof(buf), "%s%s%s", d->city,
                 d->country[0] ? ", " : "", d->country);
        weather_set_text_id("weather_city_label", buf);
        weather_icon_set_id("weather_icon", d->code);
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
        weather_icon_set_id("wg_wx_icon", d->code);
        weather_set_text_id("wg_wx_desc", weather_code_text(d->code));
        {
            const char* cls = weather_temp_class(d->temp);
            weather_tint_id("mb_weather_temp", cls);
            weather_tint_id("weather_temp", cls);
            weather_tint_id("wg_wx_temp", cls);
        }

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

    if (!g_weather.ok) {
        set_hidden(luna_get_element_by_id("weather_content"), 1);
        set_hidden(luna_get_element_by_id("weather_error"), 1);
        set_hidden(luna_get_element_by_id("weather_hint"), 0);
        weather_set_text_id("weather_hint", "Fetching…");
        weather_set_text_id("mb_weather_temp", "…");
        weather_set_text_id("wg_wx_desc", "Fetching…");
    }
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


static void on_weather_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
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







static void dismiss_popovers(void) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
}

static void on_luna_menu(LunaElement* e) {
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
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
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
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
    dismiss_power_menu();
    dismiss_desk_menu();
    dismiss_tray_menu();
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


/* ── CPU fan / power mode menus ──────────────────────────────────────────
 * Fan: applesmc-style fanN_{input,min,max,output,manual} (MacBooks) with
 * mbpfan as the automatic controller.  Power: cpufreq governor and turbo, or
 * power-profiles-daemon when present.  Sensor reads are a handful of tiny sysfs
 * files, done only while a menu is open (every 2 s), never for a closed menu.
 * Writes go through one shell command (privileged via pkexec when the shell is
 * not root). */
static char   g_fan_dir[96];
static char   g_cpu_temp_path[128];
static int    g_fan_mode = -1;          /* 0 auto, 1 low, 2 mid, 3 high, 4 max */
static double g_thermal_tick_at;

static int sysfs_long(const char* path, long* out) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int ok = fscanf(f, "%ld", out) == 1;
    fclose(f);
    return ok;
}

static int sysfs_str(const char* path, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    out[0] = 0;
    if (!fgets(out, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}

static int process_running(const char* comm) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    int found = 0;
    struct dirent* de;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[64], name[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        if (sysfs_str(path, name, sizeof(name)) && !strcmp(name, comm)) found = 1;
    }
    closedir(d);
    return found;
}

static void thermal_discover(void) {
    if (g_fan_dir[0] && g_cpu_temp_path[0]) return;
    char path[160];
    snprintf(path, sizeof(path), "/sys/devices/platform/applesmc.768/fan1_input");
    if (!g_fan_dir[0] && access(path, R_OK) == 0)
        snprintf(g_fan_dir, sizeof(g_fan_dir), "/sys/devices/platform/applesmc.768");
    DIR* d = opendir("/sys/class/hwmon");
    struct dirent* de;
    while (d && (de = readdir(d))) {
        if (strncmp(de->d_name, "hwmon", 5)) continue;
        char dir[96], name[48];
        snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", de->d_name);
        snprintf(path, sizeof(path), "%s/name", dir);
        int named = sysfs_str(path, name, sizeof(name));
        snprintf(path, sizeof(path), "%s/fan1_input", dir);
        if (!g_fan_dir[0] && access(path, R_OK) == 0) snprintf(g_fan_dir, sizeof(g_fan_dir), "%s", dir);
        snprintf(path, sizeof(path), "%s/temp1_input", dir);
        if (!g_cpu_temp_path[0] && named && access(path, R_OK) == 0 &&
            (!strcmp(name, "coretemp") || !strcmp(name, "k10temp") || !strcmp(name, "zenpower") ||
             !strcmp(name, "cpu_thermal") || !strcmp(name, "acpitz")))
            snprintf(g_cpu_temp_path, sizeof(g_cpu_temp_path), "%s", path);
    }
    if (d) closedir(d);
}

/* Run a script privileged: directly as root, otherwise through pkexec. */
static void run_privileged(const char* script) {
    char cmd[1024];
    if (geteuid() == 0)
        snprintf(cmd, sizeof(cmd), "%s", script);
    else if (command_available("pkexec"))
        snprintf(cmd, sizeof(cmd), "pkexec sh -c '%s'", script);
    else {
        toast_show("Power", "Root permission is required", 3.0);
        return;
    }
    spawn_command(cmd);
}

static void fan_update_ui(void) {
    thermal_discover();
    char text[200];
    long temp = 0, rpm = 0, rmin = 0, rmax = 0, manual = 1;
    char path[160];
    int have_t = g_cpu_temp_path[0] && sysfs_long(g_cpu_temp_path, &temp);
    int have_f = 0;
    if (g_fan_dir[0]) {
        snprintf(path, sizeof(path), "%s/fan1_input", g_fan_dir);  have_f = sysfs_long(path, &rpm);
        snprintf(path, sizeof(path), "%s/fan1_min", g_fan_dir);    sysfs_long(path, &rmin);
        snprintf(path, sizeof(path), "%s/fan1_max", g_fan_dir);    sysfs_long(path, &rmax);
        snprintf(path, sizeof(path), "%s/fan1_manual", g_fan_dir); sysfs_long(path, &manual);
    }
    if (!have_f && !have_t)
        snprintf(text, sizeof(text), "No fan or temperature sensor found");
    else if (have_t && have_f)
        snprintf(text, sizeof(text), "CPU %ld °C  ·  Fan %ld rpm (%ld–%ld)", temp / 1000, rpm, rmin, rmax);
    else if (have_t)
        snprintf(text, sizeof(text), "CPU %ld °C", temp / 1000);
    else
        snprintf(text, sizeof(text), "Fan %ld rpm (%ld–%ld)", rpm, rmin, rmax);
    int st = luna_get_element_by_id("fan_status");
    if (st >= 0 && text_would_change(st, text)) luna_set_text(st, text);

    int mode = g_fan_mode;
    if (mode < 0) mode = (!manual || process_running("mbpfan")) ? 0 : 2;
    static const char* const ids[] = { "fan_auto", "fan_low", "fan_mid", "fan_high", "fan_max" };
    for (int i = 0; i < 5; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx < 0) continue;
        luna_update_classes(idx, "connected", i == mode ? "connected" : NULL);
        set_hidden(idx, !have_f && i > 0);       /* manual levels need a fan */
    }
}

static void power_update_ui(void) {
    char text[200] = "No battery detected  ·  AC power";
    DIR* d = opendir("/sys/class/power_supply");
    struct dirent* de;
    int ac = -1, have_bat = 0;
    long cap = 0;
    char status[32] = "";
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[160], type[24];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
        if (!sysfs_str(path, type, sizeof(type))) continue;
        if (!strcmp(type, "Mains") || !strcmp(type, "USB")) {
            long on = 0;
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", de->d_name);
            if (sysfs_long(path, &on) && (ac < 0 || on)) ac = (int)on;
        } else if (!strcmp(type, "Battery") && !have_bat) {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
            have_bat = sysfs_long(path, &cap);
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
            sysfs_str(path, status, sizeof(status));
        }
    }
    if (d) closedir(d);
    if (have_bat)
        snprintf(text, sizeof(text), "%s  ·  Battery %ld%%%s%s", ac > 0 ? "AC adapter" : "On battery",
                 cap, status[0] ? "  ·  " : "", status);
    else if (ac == 0)
        snprintf(text, sizeof(text), "No battery detected  ·  AC unplugged");
    fan_update_ui();
    int st = luna_get_element_by_id("pw_status");
    if (st >= 0 && text_would_change(st, text)) luna_set_text(st, text);

    char gov[32] = "", avail[160] = "";
    sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof(gov));
    sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", avail, sizeof(avail));
    int level = !strcmp(gov, "performance") ? 2 : (!strcmp(gov, "powersave") || !strcmp(gov, "conservative")) ? 0 : 1;
    static const char* const ids[] = { "pw_save", "pw_bal", "pw_perf" };
    for (int i = 0; i < 3; i++) {
        int idx = luna_get_element_by_id(ids[i]);
        if (idx >= 0) {
            luna_update_classes(idx, "connected", i == level ? "connected" : NULL);
            set_hidden(idx, !avail[0] && !command_available("powerprofilesctl"));
        }
    }
    long no_turbo = 0;
    int have_turbo = sysfs_long("/sys/devices/system/cpu/intel_pstate/no_turbo", &no_turbo);
    int tb = luna_get_element_by_id("pw_turbo");
    if (tb >= 0) {
        set_hidden(tb, !have_turbo);
        luna_update_classes(tb, "connected", have_turbo && !no_turbo ? "connected" : NULL);
    }
}

static void thermal_menus_tick(void) {
    if (g_now - g_thermal_tick_at < 2.0) return;
    g_thermal_tick_at = g_now;
    if (is_shown(g_power_menu_idx)) power_update_ui();
    luna_mark_layout_dirty();
}




static void desk_menu_open(float x, float y) {
    if (g_desk_menu_idx < 0) return;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_power_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    int w = luna_get_element_by_id("dm_widgets");
    if (w >= 0) {
        int lbl = -1;
        for (int i = 0; i < luna_element_count(); i++)
            if (luna_element_at(i)->parent_idx == w && strstr(luna_element_at(i)->class_name, "mi_label")) { lbl = i; break; }
        if (lbl >= 0) luna_set_text(lbl, g_settings.widgets_enabled ? "Hide Desktop Widgets" : "Show Desktop Widgets");
    }
    luna_mark_layout_dirty();
    set_hidden(g_desk_menu_idx, 0);
    position_menu_at(g_desk_menu_idx, x, y);
}

/* ── Wallpaper image picker ──
 * The desktop's file dialog is whatever the system provides (zenity, kdialog
 * or yad).  It blocks, so it runs on its own thread; the chosen path is handed
 * back through the async eventfd and applied from the main loop, which is the
 * only place allowed to touch Luna UI state. */
/* ── Custom wallpapers ──
 * One setting (wallpaper_file) names the wallpaper; its extension picks the
 * renderer:
 *   image  .png .jpg .jpeg .webp .bmp .tga   background-image, cover
 *   html   .html .htm   markup mounted in #wp_custom, styled by the file's
 *                       <style> blocks and a sibling NAME.css
 *   glsl   .frag .glsl .fs   fragment shader drawn under the UI
 *                       (Shadertoy mainImage() or a plain main())
 * Pick one and every other kind is dropped, so a built-in theme can always be
 * chosen again. */
enum { WP_IMAGE = 0, WP_HTML, WP_GLSL };

static int wp_kind_of(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return WP_IMAGE;
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return WP_HTML;
    if (!strcasecmp(dot, ".frag") || !strcasecmp(dot, ".glsl") || !strcasecmp(dot, ".fs"))
        return WP_GLSL;
    return WP_IMAGE;
}

static char* wp_read_file(const char* path, size_t max) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    return buf;
}

/* g_wp_css (declared with shell_ensure_widget_css) is appended after the
 * widget sheet so every skin reload keeps the HTML wallpaper's styles. */
static const char* wp_ci_find(const char* h, const char* needle) {
    return strcasestr(h, needle);
}

/* Moves every <style>…</style> block of `html` into *css and returns the
 * markup that remains (the <body> contents when there is a <body>). */
static char* wp_split_html(char* html, char** css) {
    /* Comments first: a "<style>" mentioned inside one is not a stylesheet. */
    for (char* c = strstr(html, "<!--"); c; c = strstr(c, "<!--")) {
        char* e = strstr(c + 4, "-->");
        if (!e) { *c = 0; break; }
        memmove(c, e + 3, strlen(e + 3) + 1);
    }
    size_t cap = strlen(html) + 1;
    char* out = malloc(cap);
    char* sheet = calloc(1, cap);
    if (!out || !sheet) { free(out); free(sheet); return NULL; }
    size_t o = 0, c = 0;
    for (const char* p = html; *p;) {
        const char* st = wp_ci_find(p, "<style");
        if (!st) { size_t n = strlen(p); memcpy(out + o, p, n); o += n; break; }
        memcpy(out + o, p, (size_t)(st - p)); o += (size_t)(st - p);
        const char* open_end = strchr(st, '>');
        const char* en = open_end ? wp_ci_find(open_end, "</style") : NULL;
        if (!open_end || !en) break;
        memcpy(sheet + c, open_end + 1, (size_t)(en - open_end - 1));
        c += (size_t)(en - open_end - 1);
        sheet[c++] = '\n';
        const char* close = strchr(en, '>');
        p = close ? close + 1 : en + 7;
    }
    out[o] = 0; sheet[c] = 0;
    char* res = out;
    const char* b = wp_ci_find(out, "<body");
    if (b && (b = strchr(b, '>'))) {
        char* e = (char*)wp_ci_find(b, "</body");
        if (e) *e = 0;
        res = strdup(b + 1);
        free(out);
    }
    *css = sheet;
    return res;
}

static void wp_set_user_css(char* css) {
    free(g_wp_css);
    g_wp_css = css && *css ? css : NULL;
    if (!g_wp_css) free(css);
}

/* #wp_custom is appended to #bg_layer when the layout is parsed. */
static int wp_custom_element(int wall) {
    (void)wall;
    return luna_get_element_by_id("wp_custom");
}

/* Loads FILE into #wp_custom; returns 0 on failure. */
static int wp_html_load(int wall, const char* file) {
    char* raw = wp_read_file(file, 1u << 20);
    if (!raw) return 0;
    char* css = NULL;
    char* markup = wp_split_html(raw, &css);
    free(raw);
    if (!markup) { free(css); return 0; }
    /* NAME.css beside NAME.html is the usual place for the stylesheet. */
    char side[PATH_MAX];
    snprintf(side, sizeof(side), "%s", file);
    char* dot = strrchr(side, '.');
    char* sidecar = NULL;
    if (dot && (size_t)(dot - side) + 5 < sizeof(side)) {
        strcpy(dot, ".css");
        sidecar = wp_read_file(side, 1u << 20);
    }
    size_t n = strlen(css) + (sidecar ? strlen(sidecar) : 0) + 2;
    char* all = malloc(n);
    if (all) snprintf(all, n, "%s\n%s", css, sidecar ? sidecar : "");
    free(css); free(sidecar);
    wp_set_user_css(all);
    int idx = wp_custom_element(wall);
    if (idx >= 0) {
        luna_dom_set_inner_html(idx, markup);
        luna_update_classes(idx, NULL, "active");
    }
    free(markup);
    return idx >= 0;
}

/* GLSL shader surface, used for the wallpaper and for the screensaver.
 * Compilation happens inside the render pass because that is the only place a
 * GL context is current. */
typedef struct {
    char     path[PATH_MAX];
    const char* builtin;  /* source used when `path` is empty */
    int      pending;     /* source changed; (re)compile before the next draw */
    int      failed;      /* last compile failed */
    int      animated;    /* source reads iTime/iFrame */
    GLuint   prog, vao;
    GLint    loc;
    double   time;        /* shader seconds, advanced by wall time * speed */
    double   last;        /* plat_time() of the previous draw, 0 = none */
    unsigned frame;
} WpGlsl;
static WpGlsl g_glsl;        /* wallpaper */
static WpGlsl g_saver_glsl;  /* screensaver */

static const char* const WP_GLSL_VS =
    "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char* const WP_GLSL_FS_HEAD =
    "uniform vec4 _lw;\n"                    /* xy resolution, z seconds, w frame */
    "#define iResolution vec3(_lw.xy, 1.0)\n"
    "#define iTime _lw.z\n"
    "#define iFrame int(_lw.w)\n"
    "#define iMouse vec4(0.0)\n";
static const char* const WP_GLSL_OUT = "out vec4 _fc;\n";
static const char* const WP_GLSL_LEGACY = "#define gl_FragColor _fc\n";

/* Shader used by the screensaver when neither a file nor a GLSL wallpaper is set. */
static const char* const WP_SAVER_DEFAULT =
    "void mainImage(out vec4 o, in vec2 p) {\n"
    "  vec2 uv = (p - 0.5 * iResolution.xy) / iResolution.y;\n"
    "  float t = iTime * 0.3, v = 0.0;\n"
    "  for (int i = 1; i < 5; i++) {\n"
    "    float k = float(i);\n"
    "    uv += 0.35 / k * vec2(sin(uv.y * k * 2.1 + t), cos(uv.x * k * 2.3 - t));\n"
    "    v += length(uv) * 0.5;\n"
    "  }\n"
    "  o = vec4(0.5 + 0.5 * cos(vec3(0.0, 2.0, 4.0) + v * 2.0 + t), 1.0) * 0.8;\n"
    "}\n";

static void wp_glsl_set(WpGlsl* g, const char* path) {
    g->path[0] = 0;
    if (path) snprintf(g->path, sizeof(g->path), "%s", path);
    g->pending = 1;
    g->failed = 0;
    g->animated = 0;
    char* src = g->path[0] ? wp_read_file(g->path, 1u << 18) : NULL;
    const char* text = src ? src : g->builtin;
    g->animated = text && (strstr(text, "iTime") || strstr(text, "iFrame"));
    free(src);
}

static int wp_glsl_build(WpGlsl* g) {
    if (g->prog) { glDeleteProgram_(g->prog); g->prog = 0; }
    g->pending = 0;
    char* user = g->path[0] ? wp_read_file(g->path, 1u << 18)
                            : (g->builtin ? strdup(g->builtin) : NULL);
    if (!user) { g->failed = 1; return 0; }
    int toy = strstr(user, "mainImage") != NULL;
    int legacy = strstr(user, "gl_FragColor") != NULL;
    /* The wrapper owns #version; neutralise the user's own line. */
    if (!strncmp(user, "#version", 8)) { user[0] = '/'; user[1] = '/'; }
    size_t n = strlen(user) + 640;
    char* fs = malloc(n);
    if (!fs) { free(user); g->failed = 1; return 0; }
    snprintf(fs, n, "%s%s%s%s\n%s", WP_GLSL_FS_HEAD,
             toy || legacy ? WP_GLSL_OUT : "", legacy ? WP_GLSL_LEGACY : "", user,
             toy ? "void main(){ vec4 c = vec4(0.0, 0.0, 0.0, 1.0);"
                   " mainImage(c, gl_FragCoord.xy); _fc = vec4(c.rgb, 1.0); }\n"
                 : "");
    free(user);
    g->prog = create_shader_program(WP_GLSL_VS, fs, "wallpaper");
    free(fs);
    if (!g->prog) { g->failed = 1; return 0; }
    g->loc = luna_glGetUniformLocation(g->prog, "_lw");
    if (!g->vao) luna_glGenVertexArrays(1, &g->vao);
    g->time = 0.0;
    g->last = 0.0;
    g->frame = 0;
    return 1;
}

static int wp_glsl_usable(const WpGlsl* g) { return g->path[0] || g->builtin; }

/* Draws the shader over the freshly cleared target.  Returns 1 when pixels
 * were produced, in which case the caller renders the UI with the backdrop
 * preserved.  `speed` scales shader time (0 freezes it). */
static int wp_glsl_draw(WpGlsl* g, int fw, int fh, double speed) {
    if (!wp_glsl_usable(g)) return 0;
    if (g->pending) wp_glsl_build(g);
    if (!g->prog) return 0;
    double now = plat_time();
    if (g->last > 0.0 && now > g->last) {
        double dt = now - g->last;
        g->time += (dt > 0.5 ? 0.5 : dt) * speed;   /* no jump after a long pause */
    }
    g->last = now;
    GLint prev_prog = 0, prev_vao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    GLboolean blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    luna_glUseProgram(g->prog);
    luna_glUniform4f(g->loc, (float)fw, (float)fh, (float)g->time, (float)g->frame++);
    luna_glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    luna_glBindVertexArray((GLuint)prev_vao);
    luna_glUseProgram((GLuint)prev_prog);
    if (blend) glEnable(GL_BLEND);
    return 1;
}

/* Wallpaper motion: GLSL_FPS 0 keeps the first frame, low rates and a low
 * speed give a barely-moving backdrop that costs almost nothing. */
static double wp_motion_speed(void) {
    return g_settings.wallpaper_animation ? g_settings.wallpaper_speed / 100.0 : 0.0;
}

static int wp_glsl_animating(void) {
    return g_glsl.path[0] && !g_glsl.failed && g_glsl.animated &&
           g_settings.wallpaper_animation && g_settings.wallpaper_fps > 0;
}

static double bg_frame_sec(void) {
    return wp_glsl_animating() ? 1.0 / g_settings.wallpaper_fps : LUNA_WL_BG_FRAME_SEC;
}

/* Screensaver state. */
static int g_saver_active;
static int g_saver_surf = -1;
static int g_saver_idx = -1;

static void saver_start(void) {
    if (g_saver_active || g_saver_surf < 0 || g_saver_idx < 0 || g_settings.saver_minutes <= 0)
        return;
    const char* f = NULL;
    if (g_settings.saver_file[0] && access(g_settings.saver_file, R_OK) == 0)
        f = g_settings.saver_file;
    else if (g_glsl.path[0])
        f = g_glsl.path;
    g_saver_glsl.builtin = WP_SAVER_DEFAULT;
    wp_glsl_set(&g_saver_glsl, f);
    g_saver_active = 1;
    set_hidden(g_saver_idx, 0);
    g_surf_dirty |= 1u << g_saver_surf;
}

static void saver_stop(void) {
    if (!g_saver_active) return;
    g_saver_active = 0;
    set_hidden(g_saver_idx, 1);
    g_surf_dirty = 0xffffffffu;   /* everything underneath repaints once */
}

/* ── Wallpaper chooser ──
 * A dialog built from Luna UI elements (no external zenity/kdialog/yad): theme
 * tiles, the shipped samples, the user's wallpaper folder and a small folder
 * browser, 12 tiles a page.  A click applies the wallpaper at once.  Image
 * thumbnails are decoded and downscaled on a worker thread into a cache and
 * handed back through the async eventfd, so a folder of 4K photos never
 * stalls the shell or fills memory with full-size textures. */
static const char* g_wallpaper_override;   /* --wallpaper PATH for this run */
static int         g_wp_user_chose;        /* a UI choice outranks --wallpaper / LUNA_WALLPAPER */
static void apply_wallpaper(const char* theme);
static void wp_motion_apply(void);
static void settings_mark_wallpaper(const char* theme);
static const char* wallpaper_file_effective(void);

/* ~/.config/luna-shell/wallpapers: where user-made HTML/GLSL/image wallpapers
 * live; created on first use. */
static void wallpaper_dir(char* out, size_t cap) {
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    out[0] = 0;
    if (xdg && *xdg) snprintf(out, cap, "%s/luna-shell/wallpapers", xdg);
    else if (home) snprintf(out, cap, "%s/.config/luna-shell/wallpapers", home);
    if (!out[0]) return;
    char mk[PATH_MAX];
    snprintf(mk, sizeof(mk), "%s", out);
    for (char* q = mk + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(mk, 0755); *q = '/'; }
    mkdir(mk, 0755);
}

static int shell_spawn_argv(const char* const argv[]);
static void wallpaper_open_folder(void) {
    char dir[PATH_MAX];
    wallpaper_dir(dir, sizeof(dir));
    if (!dir[0]) return;
    const char* const argv[] = { "xdg-open", dir, NULL };
    if (!shell_spawn_argv(argv))
        toast_show("Wallpaper", dir, 4.0);
}


#define WPD_TILES   12
#define WPD_MAX     400
#define WPD_DIRS    10
#define WPD_THUMB_W 280
#define WPD_THUMB_H 164
enum { WPD_THEME = 0, WPD_DIR, WPD_IMAGE, WPD_HTML, WPD_GLSL, WPD_NONE, WPD_FILE };
typedef struct { char name[128]; int kind, dir; } WpdEntry;
typedef struct { char src[PATH_MAX], dst[PATH_MAX]; int tile; } WpdJob;

static WpdEntry g_wpd_ent[WPD_MAX];
static int      g_wpd_count, g_wpd_page, g_wpd_tab, g_wpd_saver, g_wpd_live, g_wpd_live_from;
static int      g_wpd_send;                  /* 1: the dialog picks any file for AirDrop */
static void   (*g_wpd_send_cb)(const char* path);
static char     g_wpd_dirs[WPD_DIRS][PATH_MAX];
static int      g_wpd_ndirs;
static char     g_wpd_browse[PATH_MAX];      /* folder shown by the Pictures / Browse tabs */
static int      g_wpd_win = -1, g_wpd_box = -1, g_wpd_surf = -1;
static int      g_wpd_tile[WPD_TILES], g_wpd_thumb[WPD_TILES], g_wpd_glyph[WPD_TILES], g_wpd_label[WPD_TILES];
static unsigned g_wpd_gen;                   /* bumped per page; stale thumbnails are dropped */
static pthread_mutex_t g_wpd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int ready; unsigned gen; char path[PATH_MAX]; } g_wpd_done[WPD_TILES];
static const struct { const char* id; const char* name; } g_wpd_themes[4] = {
    { "night", "Night Sky" }, { "ocean", "Deep Ocean" }, { "forest", "Pine Forest" }, { "sunset", "Dusk Sunset" }
};

static const char* wpd_base(const char* path) {
    const char* b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static int wpd_kind_of(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return -1;
    if (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
        !strcasecmp(dot, ".webp") || !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".tga")) return WPD_IMAGE;
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return WPD_HTML;
    if (!strcasecmp(dot, ".frag") || !strcasecmp(dot, ".glsl") || !strcasecmp(dot, ".fs")) return WPD_GLSL;
    return -1;
}

static void wpd_path_of(const WpdEntry* e, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_wpd_dirs[e->dir], e->name);
}

static void wpd_add_dir(const char* dir) {
    if (!dir || !*dir || g_wpd_ndirs >= WPD_DIRS || access(dir, R_OK) != 0) return;
    for (int i = 0; i < g_wpd_ndirs; i++)
        if (!strcmp(g_wpd_dirs[i], dir)) return;
    snprintf(g_wpd_dirs[g_wpd_ndirs++], PATH_MAX, "%s", dir);
}

static int wpd_cmp(const void* a, const void* b) {
    const WpdEntry* x = a; const WpdEntry* y = b;
    if ((x->kind == WPD_DIR) != (y->kind == WPD_DIR)) return x->kind == WPD_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Entries of one folder; folders only when browsing, only shaders for a saver. */
static void wpd_scan(int dir_idx, int with_dirs) {
    DIR* d = opendir(g_wpd_dirs[dir_idx]);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) && g_wpd_count < WPD_MAX) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(g_wpd_ent[0].name)) continue;
        char full[PATH_MAX + 300];
        snprintf(full, sizeof(full), "%s/%s", g_wpd_dirs[dir_idx], de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        int kind;
        if (S_ISDIR(st.st_mode)) {
            if (!with_dirs) continue;
            kind = WPD_DIR;
        } else {
            kind = wpd_kind_of(de->d_name);
            if (g_wpd_send) { if (kind != WPD_IMAGE) kind = WPD_FILE; }
            else if (kind < 0 || (g_wpd_saver && kind != WPD_GLSL)) continue;
            if (g_wpd_live) {                      /* shipped live wallpapers only, no duplicates */
                if (kind == WPD_IMAGE) continue;
                int dup = 0;
                for (int k = g_wpd_live_from; k < g_wpd_count; k++)
                    if (!strcmp(g_wpd_ent[k].name, de->d_name)) { dup = 1; break; }
                if (dup) continue;
            }
        }
        WpdEntry* e = &g_wpd_ent[g_wpd_count++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->kind = kind;
        e->dir = dir_idx;
    }
    closedir(d);
}

static void wpd_sample_dirs(void) {
    char p[PATH_MAX];
    if (g_xdg.data_home[0] && snprintf(p, sizeof(p), "%s/luna-desktop/skins/wallpapers", g_xdg.data_home) < (int)sizeof(p))
        wpd_add_dir(p);
    const char* xd = getenv("XDG_DATA_DIRS");
    char list[PATH_MAX];
    snprintf(list, sizeof(list), "%s", xd && *xd ? xd : "/usr/local/share:/usr/share");
    char* save = NULL;
    for (char* b = strtok_r(list, ":", &save); b; b = strtok_r(NULL, ":", &save))
        if (*b == '/' && snprintf(p, sizeof(p), "%s/luna-desktop/skins/wallpapers", b) < (int)sizeof(p))
            wpd_add_dir(p);
    wpd_add_dir("skins/wallpapers");
    wpd_add_dir("apps/luna-shell/skins/wallpapers");
    ssize_t len = readlink("/proc/self/exe", p, sizeof(p) - 40);
    if (len > 0) {
        p[len] = 0;
        char* sl = strrchr(p, '/');
        if (sl) {
            snprintf(sl, 40, "/../share/luna-desktop/skins/wallpapers");
            wpd_add_dir(p);
        }
    }
    wpd_add_dir("/usr/share/wallpapers");
    wpd_add_dir("/usr/share/backgrounds");
}

static void wpd_set_text(const char* id, const char* text) {
    int i = luna_get_element_by_id(id);
    if (i >= 0) luna_set_text(i, text);
}

static const char* wpd_current_file(void) {
    const char* f = g_wpd_saver ? g_settings.saver_file : wallpaper_file_effective();
    return f && *f ? f : NULL;
}

/* ---- thumbnails (worker thread) ---- */
static unsigned long long wpd_hash(const char* s, long long extra) {
    unsigned long long h = 1469598103934665603ULL ^ (unsigned long long)extra;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static int wpd_make_thumb(const char* src, const char* dst) {
    int w, h, n;
    if (!stbi_info(src, &w, &h, &n) || w <= 0 || h <= 0 || (long long)w * h > 80000000LL) return 0;
    unsigned char* px = stbi_load(src, &w, &h, &n, 4);
    if (!px) return 0;
    /* Cover-crop to the tile's aspect, then box-average down. */
    unsigned char* out = malloc((size_t)WPD_THUMB_W * WPD_THUMB_H * 4);
    if (!out) { stbi_image_free(px); return 0; }
    float sc = fmaxf((float)WPD_THUMB_W / w, (float)WPD_THUMB_H / h);
    float cw = WPD_THUMB_W / sc, ch = WPD_THUMB_H / sc;
    float x0 = (w - cw) * 0.5f, y0 = (h - ch) * 0.5f;
    for (int y = 0; y < WPD_THUMB_H; y++) {
        int sy0 = (int)(y0 + y / sc), sy1 = (int)(y0 + (y + 1) / sc);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int x = 0; x < WPD_THUMB_W; x++) {
            int sx0 = (int)(x0 + x / sc), sx1 = (int)(x0 + (x + 1) / sc);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char* q = px + ((size_t)yy * w + xx) * 4;
                    acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; acc[3] += q[3]; cnt++;
                }
            unsigned char* o = out + ((size_t)y * WPD_THUMB_W + x) * 4;
            if (!cnt) cnt = 1;
            o[0] = acc[0] / cnt; o[1] = acc[1] / cnt; o[2] = acc[2] / cnt; o[3] = acc[3] / cnt;
        }
    }
    stbi_image_free(px);
    int ok = stbi_write_png(dst, WPD_THUMB_W, WPD_THUMB_H, 4, out, WPD_THUMB_W * 4);
    free(out);
    return ok != 0;
}

typedef struct { unsigned gen; int n; WpdJob job[WPD_TILES]; } WpdBatch;

static void* wpd_thumb_thread(void* arg) {
    WpdBatch* b = arg;
    /* luna-ui turns on stb's global flip-on-load for its textures; thumbnails must stay upright. */
    stbi_set_flip_vertically_on_load_thread(0);
    for (int i = 0; i < b->n; i++) {
        if (b->gen != g_wpd_gen) break;     /* page changed: abandon the rest */
        WpdJob* j = &b->job[i];
        if (access(j->dst, R_OK) != 0 && !wpd_make_thumb(j->src, j->dst)) continue;
        pthread_mutex_lock(&g_wpd_lock);
        snprintf(g_wpd_done[j->tile].path, PATH_MAX, "%s", j->dst);
        g_wpd_done[j->tile].gen = b->gen;
        g_wpd_done[j->tile].ready = 1;
        pthread_mutex_unlock(&g_wpd_lock);
        shell_async_notify(NULL);
    }
    free(b);
    return NULL;
}

static void wpd_apply_thumbs(void) {
    if (g_wpd_win < 0) return;
    pthread_mutex_lock(&g_wpd_lock);
    int any = 0;
    for (int t = 0; t < WPD_TILES; t++) {
        if (!g_wpd_done[t].ready) continue;
        g_wpd_done[t].ready = 0;
        if (g_wpd_done[t].gen != g_wpd_gen || g_wpd_thumb[t] < 0) continue;
        luna_invalidate_texture(g_wpd_done[t].path);
        luna_set_background_image(g_wpd_thumb[t], g_wpd_done[t].path);
        if (g_wpd_glyph[t] >= 0) luna_set_text(g_wpd_glyph[t], "");
        any = 1;
    }
    pthread_mutex_unlock(&g_wpd_lock);
    if (any && g_wpd_surf >= 0) g_surf_dirty |= 1u << g_wpd_surf;
}

/* ---- tab content ---- */
static void wpd_load_tab(void) {
    g_wpd_count = 0;
    g_wpd_ndirs = 0;
    char dir[PATH_MAX];
    const char* home = g_xdg.home[0] ? g_xdg.home : "/";
    if (g_wpd_tab == 0) {
        if (g_wpd_saver) {
            snprintf(g_wpd_ent[0].name, sizeof(g_wpd_ent[0].name), "Wallpaper / built-in");
            g_wpd_ent[0].kind = WPD_NONE;
            g_wpd_count = 1;
        } else {
            for (int i = 0; i < 4; i++) {
                snprintf(g_wpd_ent[i].name, sizeof(g_wpd_ent[i].name), "%s", g_wpd_themes[i].name);
                g_wpd_ent[i].kind = WPD_THEME;
                g_wpd_ent[i].dir = i;
            }
            g_wpd_count = 4;
        }
        /* The shipped shader / HTML wallpapers sit right next to the themes. */
        wpd_sample_dirs();
        g_wpd_live = 1;
        g_wpd_live_from = g_wpd_count;
        for (int i = 0; i < g_wpd_ndirs; i++)
            if (strstr(g_wpd_dirs[i], "luna-desktop/skins/wallpapers") || strstr(g_wpd_dirs[i], "skins/wallpapers"))
                wpd_scan(i, 0);
        g_wpd_live = 0;
        qsort(g_wpd_ent + g_wpd_live_from, (size_t)(g_wpd_count - g_wpd_live_from), sizeof(g_wpd_ent[0]), wpd_cmp);
    } else if (g_wpd_tab == 1) {
        wpd_sample_dirs();
        for (int i = 0; i < g_wpd_ndirs; i++) wpd_scan(i, 0);
    } else if (g_wpd_tab == 2) {
        wallpaper_dir(dir, sizeof(dir));
        wpd_add_dir(dir);
        for (int i = 0; i < g_wpd_ndirs; i++) wpd_scan(i, 0);
    } else {
        if (g_wpd_tab == 3 && !g_wpd_browse[0]) snprintf(g_wpd_browse, sizeof(g_wpd_browse), "%s/%s", home, g_wpd_send ? "Downloads" : "Pictures");
        if (!g_wpd_browse[0] || access(g_wpd_browse, R_OK) != 0) snprintf(g_wpd_browse, sizeof(g_wpd_browse), "%s", home);
        wpd_add_dir(g_wpd_browse);
        wpd_scan(0, 1);
    }
    /* NAME.png next to NAME.frag / .html is that wallpaper's preview, not a wallpaper. */
    for (int i = 0; i < g_wpd_count; i++) {
        if (g_wpd_ent[i].kind != WPD_IMAGE) continue;
        char base[128];
        snprintf(base, sizeof(base), "%s", g_wpd_ent[i].name);
        char* dot = strrchr(base, '.');
        if (dot) *dot = 0;
        for (int k = 0; k < g_wpd_count; k++) {
            if ((g_wpd_ent[k].kind != WPD_GLSL && g_wpd_ent[k].kind != WPD_HTML) || g_wpd_ent[k].dir != g_wpd_ent[i].dir) continue;
            char b2[128];
            snprintf(b2, sizeof(b2), "%s", g_wpd_ent[k].name);
            char* d2 = strrchr(b2, '.');
            if (d2) *d2 = 0;
            if (!strcmp(base, b2)) { g_wpd_ent[i] = g_wpd_ent[--g_wpd_count]; i--; break; }
        }
    }
    if (g_wpd_count > 1 && g_wpd_tab != 0) qsort(g_wpd_ent, (size_t)g_wpd_count, sizeof(g_wpd_ent[0]), wpd_cmp);
}

static void wpd_render(void) {
    if (g_wpd_win < 0) return;
    int pages = (g_wpd_count + WPD_TILES - 1) / WPD_TILES;
    if (pages < 1) pages = 1;
    if (g_wpd_page >= pages) g_wpd_page = pages - 1;
    if (g_wpd_page < 0) g_wpd_page = 0;
    g_wpd_gen++;
    const char* cur = wpd_current_file();
    WpdBatch* batch = calloc(1, sizeof(*batch));
    if (batch) batch->gen = g_wpd_gen;
    char cache[PATH_MAX] = "";
    if (g_xdg.cache_home[0]) {
        snprintf(cache, sizeof(cache), "%s/luna-shell/wp-thumbs", g_xdg.cache_home);
        char mk[PATH_MAX];
        snprintf(mk, sizeof(mk), "%s/luna-shell", g_xdg.cache_home);
        mkdir(mk, 0700);
        mkdir(cache, 0700);
    }
    for (int t = 0; t < WPD_TILES; t++) {
        int i = g_wpd_page * WPD_TILES + t;
        int tile = g_wpd_tile[t];
        if (tile < 0) continue;
        if (i >= g_wpd_count) { set_hidden(tile, 1); continue; }
        const WpdEntry* e = &g_wpd_ent[i];
        set_hidden(tile, 0);
        char path[PATH_MAX + 300] = "";
        if (e->kind != WPD_THEME && e->kind != WPD_NONE) wpd_path_of(e, path, sizeof(path));
        int sel = 0;
        const char* glyph = "";
        const char* tcls = "wpd_th_plain";
        if (e->kind == WPD_THEME) {
            sel = !cur && !strcmp(g_settings.wallpaper, g_wpd_themes[e->dir].id);
            tcls = !strcmp(g_wpd_themes[e->dir].id, "night") ? "wpd_th_night" :
                   !strcmp(g_wpd_themes[e->dir].id, "ocean") ? "wpd_th_ocean" :
                   !strcmp(g_wpd_themes[e->dir].id, "forest") ? "wpd_th_forest" : "wpd_th_sunset";
        } else if (e->kind == WPD_NONE) {
            sel = !cur;
            glyph = "";
        } else {
            sel = cur && e->kind != WPD_DIR && !strcmp(cur, path);
            glyph = e->kind == WPD_FILE ? "" : e->kind == WPD_DIR ? "" : e->kind == WPD_HTML ? "" :
                    e->kind == WPD_GLSL ? "" : "";
            tcls = e->kind == WPD_DIR ? "wpd_th_dir" : e->kind == WPD_HTML ? "wpd_th_html" :
                   e->kind == WPD_GLSL ? "wpd_th_glsl" : "wpd_th_plain";
        }
        luna_update_classes(tile, "selected", sel ? "selected" : NULL);
        luna_update_classes(g_wpd_thumb[t], "wpd_th_night wpd_th_ocean wpd_th_forest wpd_th_sunset wpd_th_dir wpd_th_html wpd_th_glsl wpd_th_plain", tcls);
        luna_set_background_image(g_wpd_thumb[t], NULL);
        if (g_wpd_glyph[t] >= 0) luna_set_text(g_wpd_glyph[t], glyph);
        if (g_wpd_label[t] >= 0) {
            char base[128];
            snprintf(base, sizeof(base), "%s", e->name);
            char* dot = e->kind == WPD_DIR || e->kind == WPD_THEME || e->kind == WPD_NONE || e->kind == WPD_FILE ? NULL : strrchr(base, '.');
            if (dot) *dot = 0;
            luna_set_text(g_wpd_label[t], base);
        }
        char thumb_src[PATH_MAX + 300] = "";
        if (e->kind == WPD_IMAGE) snprintf(thumb_src, sizeof(thumb_src), "%s", path);
        else if (e->kind == WPD_GLSL || e->kind == WPD_HTML) {
            static const char* const ext[] = { ".png", ".jpg" };
            for (int x = 0; x < 2 && !thumb_src[0]; x++) {
                char cand[PATH_MAX + 300];
                snprintf(cand, sizeof(cand), "%s", path);
                char* dot = strrchr(cand, '.');
                if (dot) { *dot = 0; strcat(cand, ext[x]); }
                if (access(cand, R_OK) == 0) snprintf(thumb_src, sizeof(thumb_src), "%s", cand);
            }
        }
        if (thumb_src[0] && batch && cache[0]) {
            struct stat st;
            if (stat(thumb_src, &st) == 0) {
                WpdJob* j = &batch->job[batch->n++];
                snprintf(j->src, sizeof(j->src), "%s", thumb_src);
                snprintf(j->dst, sizeof(j->dst), "%s/%016llx.png", cache, wpd_hash(thumb_src, (long long)st.st_mtime ^ ((long long)st.st_size << 20)));
                j->tile = t;
            }
        }
    }
    int empty = g_wpd_count == 0;
    set_hidden(luna_get_element_by_id("wpd_empty"), !empty);
    for (int i = 0; i < 5; i++) {
        char id[16];
        snprintf(id, sizeof(id), "wpd_tab_%d", i);
        luna_update_classes(luna_get_element_by_id(id), "selected", i == g_wpd_tab ? "selected" : NULL);
    }
    int browse = g_wpd_tab >= 3;
    set_hidden(luna_get_element_by_id("wpd_pathbar"), !browse);
    if (browse) wpd_set_text("wpd_path", g_wpd_browse);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d / %d", g_wpd_page + 1, pages);
    wpd_set_text("wpd_page", buf);
    snprintf(buf, sizeof(buf), "%d item%s", g_wpd_count, g_wpd_count == 1 ? "" : "s");
    wpd_set_text("wpd_status", buf);
    const char* c = wpd_current_file();
    wpd_set_text("wpd_cur", c ? wpd_base(c) : g_wpd_saver ? "Screensaver: default" : g_settings.wallpaper);
    luna_mark_layout_dirty();
    g_surf_dirty |= g_wpd_surf >= 0 ? 1u << g_wpd_surf : 0;
    if (batch && batch->n > 0) {
        pthread_t th;
        if (pthread_create(&th, NULL, wpd_thumb_thread, batch) == 0) pthread_detach(th);
        else free(batch);
    } else {
        free(batch);
    }
}

/* Delete every cached thumbnail and rebuild the current page. */
static void wpd_clear_cache(void) {
    char dir[PATH_MAX];
    int removed = 0;
    if (g_xdg.cache_home[0] &&
        snprintf(dir, sizeof(dir), "%s/luna-shell/wp-thumbs", g_xdg.cache_home) < (int)sizeof(dir)) {
        DIR* d = opendir(dir);
        struct dirent* de;
        while (d && (de = readdir(d))) {
            size_t n = strlen(de->d_name);
            if (n < 5 || strcmp(de->d_name + n - 4, ".png") != 0) continue;
            char f[PATH_MAX + 300];
            snprintf(f, sizeof(f), "%s/%s", dir, de->d_name);
            luna_invalidate_texture(f);
            if (unlink(f) == 0) removed++;
        }
        if (d) closedir(d);
    }
    wpd_render();
    char msg[48];
    snprintf(msg, sizeof(msg), "Cleared %d thumbnail%s", removed, removed == 1 ? "" : "s");
    wpd_set_text("wpd_status", msg);
}

static void wpd_open(int tab, int saver) {
    if (g_wpd_win < 0) return;
    g_wpd_send = 0;
    for (int i = 0; i < 3; i++) { char id[16]; snprintf(id, sizeof(id), "wpd_tab_%d", i); set_hidden(luna_get_element_by_id(id), 0); }
    wpd_set_text("wpd_tab_3", "Pictures");
    g_wpd_saver = saver;
    g_wpd_tab = tab;
    g_wpd_page = 0;
    wpd_set_text("wpd_title", saver ? "Screensaver Shader" : "Wallpaper");
    wpd_set_text("wpd_tab_0", saver ? "Default" : "Themes");
    wpd_load_tab();
    set_hidden(g_wpd_win, 0);
    wpd_render();
    center_element(g_wpd_box >= 0 ? g_wpd_box : g_wpd_win);
    shell_request_repaint(-1);
}

/* The same dialog, as a file picker for AirDrop: Downloads + Browse, any file. */
static void wpd_open_send(const char* title, void (*cb)(const char* path)) {
    if (g_wpd_win < 0) return;
    g_wpd_send = 1; g_wpd_send_cb = cb;
    g_wpd_saver = 0; g_wpd_tab = 3; g_wpd_page = 0;
    g_wpd_browse[0] = 0;
    for (int i = 0; i < 3; i++) { char id[16]; snprintf(id, sizeof(id), "wpd_tab_%d", i); set_hidden(luna_get_element_by_id(id), 1); }
    wpd_set_text("wpd_title", title);
    wpd_set_text("wpd_tab_3", "Downloads");
    wpd_load_tab();
    set_hidden(g_wpd_win, 0);
    wpd_render();
    wpd_set_text("wpd_cur", "Click a file to send it");
    center_element(g_wpd_box >= 0 ? g_wpd_box : g_wpd_win);
    shell_request_repaint(-1);
}

static void wpd_close(void) {
    if (g_wpd_win < 0 || !is_shown(g_wpd_win)) return;
    set_hidden(g_wpd_win, 1);
    g_wpd_gen++;                      /* drop thumbnails still in flight */
    g_wpd_count = 0;
    for (int t = 0; t < WPD_TILES; t++)
        if (g_wpd_thumb[t] >= 0) luna_set_background_image(g_wpd_thumb[t], NULL);
    shell_request_repaint(-1);
}

static void wallpaper_apply_file(const char* path) {
    snprintf(g_settings.wallpaper_file, sizeof(g_settings.wallpaper_file), "%s", path);
    g_wp_user_chose = 1;
    apply_wallpaper(g_settings.wallpaper);
    settings_save();
    shell_request_repaint(-1);
}

static void wpd_pick(int slot) {
    int i = g_wpd_page * WPD_TILES + slot;
    if (i < 0 || i >= g_wpd_count) return;
    const WpdEntry* e = &g_wpd_ent[i];
    char path[PATH_MAX + 300];
    if (e->kind == WPD_DIR) {
        wpd_path_of(e, path, sizeof(path));
        snprintf(g_wpd_browse, sizeof(g_wpd_browse), "%s", path);
        g_wpd_page = 0;
        wpd_load_tab();
    } else if (g_wpd_send) {
        wpd_path_of(e, path, sizeof(path));
        void (*cb)(const char*) = g_wpd_send_cb;
        wpd_close();
        if (cb) cb(path);
        return;
    } else if (g_wpd_saver) {
        if (e->kind == WPD_NONE) g_settings.saver_file[0] = 0;
        else { wpd_path_of(e, path, sizeof(path)); snprintf(g_settings.saver_file, sizeof(g_settings.saver_file), "%s", path); }
        if (!g_settings.saver_minutes) g_settings.saver_minutes = 5;
        wp_motion_apply();
    } else if (e->kind == WPD_THEME) {
        const char* id = g_wpd_themes[e->dir].id;
        snprintf(g_settings.wallpaper, sizeof(g_settings.wallpaper), "%s", id);
        g_settings.wallpaper_file[0] = 0;
        g_wp_user_chose = 1;
        settings_mark_wallpaper(id);
        apply_wallpaper(id);
        settings_save();
    } else {
        wpd_path_of(e, path, sizeof(path));
        wallpaper_apply_file(path);
    }
    wpd_render();
}

static void wpd_up(int home) {
    char p[PATH_MAX];
    if (home) snprintf(p, sizeof(p), "%s", g_xdg.home[0] ? g_xdg.home : "/");
    else {
        snprintf(p, sizeof(p), "%s", g_wpd_browse);
        char* sl = strrchr(p, '/');
        if (sl && sl != p) *sl = 0; else snprintf(p, sizeof(p), "/");
    }
    snprintf(g_wpd_browse, sizeof(g_wpd_browse), "%s", p);
    g_wpd_page = 0;
    wpd_load_tab();
    wpd_render();
}

static void on_wpd_click(LunaElement* e) {
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!strncmp(id, "wpd_t", 5) && isdigit((unsigned char)id[5])) { wpd_pick(atoi(id + 5)); return; }
        if (!strncmp(id, "wpd_tab_", 8)) {
            g_wpd_tab = atoi(id + 8);
            g_wpd_page = 0;
            wpd_load_tab();
            wpd_render();
            return;
        }
        if (!strcmp(id, "wpd_close") || !strcmp(id, "wpd_done")) { wpd_close(); return; }
        if (!strcmp(id, "wpd_up")) { wpd_up(0); return; }
        if (!strcmp(id, "wpd_home")) { wpd_up(1); return; }
        if (!strcmp(id, "wpd_prev")) { g_wpd_page--; wpd_render(); return; }
        if (!strcmp(id, "wpd_next")) { g_wpd_page++; wpd_render(); return; }
        if (!strcmp(id, "wpd_open_dir")) { wallpaper_open_folder(); return; }
        if (!strcmp(id, "wpd_clear")) { wpd_clear_cache(); return; }
    }
}

static void wpd_init_ids(void) {
    g_wpd_win = luna_get_element_by_id("wpd_win");
    g_wpd_box = luna_get_element_by_id("wpd_box");
    for (int t = 0; t < WPD_TILES; t++) {
        char id[16];
        snprintf(id, sizeof(id), "wpd_t%d", t);
        g_wpd_tile[t] = luna_get_element_by_id(id);
        g_wpd_thumb[t] = g_wpd_glyph[t] = g_wpd_label[t] = -1;
        if (g_wpd_tile[t] < 0) continue;
        int kids[4];
        int n = luna_dom_children(g_wpd_tile[t], kids, 4);
        if (n > 0) g_wpd_thumb[t] = kids[0];
        if (n > 1) g_wpd_label[t] = kids[1];
        if (n > 0) {
            int gk[2];
            if (luna_dom_children(kids[0], gk, 2) > 0) g_wpd_glyph[t] = gk[0];
        }
        wire_subtree(g_wpd_tile[t], on_wpd_click);
    }
    static const char* const ids[] = { "wpd_close", "wpd_done", "wpd_up", "wpd_home", "wpd_prev", "wpd_next",
        "wpd_open_dir", "wpd_clear", "wpd_tab_0", "wpd_tab_1", "wpd_tab_2", "wpd_tab_3", "wpd_tab_4" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        wire_subtree(luna_get_element_by_id(ids[i]), on_wpd_click);
}

static void wallpaper_pick_tick(void) {
    if (g_saver_glsl.failed && g_saver_active) saver_stop();
    if (g_glsl.failed && g_glsl.path[0]) {
        /* The shader did not compile (details on stderr): show the built-in
         * wallpaper instead of an empty screen. */
        g_glsl.path[0] = 0;
        int w = luna_get_element_by_id("wallpaper");
        if (w >= 0) luna_update_classes(w, "wp_glsl", NULL);
        toast_show("Wallpaper", "The GLSL wallpaper failed to compile.", 4.0);
    }
    wpd_apply_thumbs();
}

/* Back to a built-in animated theme: forget the custom file entirely. */
static void wallpaper_use_theme(void) {
    g_settings.wallpaper_file[0] = 0;
    g_wp_user_chose = 1;
}

/* ── Wallpaper motion / screensaver controls (settings → Desktop) ── */
static void wp_set_label(const char* id, const char* text) {
    int idx = luna_get_element_by_id(id);
    if (idx >= 0) luna_set_text(idx, text);
}

static void wp_ui_refresh(void) {
    char buf[96];
    int fps = g_settings.wallpaper_fps;
    snprintf(buf, sizeof(buf), "Shader motion: %s",
             fps == 0 ? "Still" : fps <= 2 ? "Slow · 2 fps" :
             fps <= 10 ? "Gentle · 10 fps" : fps <= 30 ? "Smooth · 30 fps" : "Full · 60 fps");
    wp_set_label("wp_motion_fps", buf);
    snprintf(buf, sizeof(buf), "Shader speed: %d%%", g_settings.wallpaper_speed);
    wp_set_label("wp_motion_speed", buf);
    if (g_settings.saver_minutes > 0)
        snprintf(buf, sizeof(buf), "Screensaver: after %d min", g_settings.saver_minutes);
    else
        snprintf(buf, sizeof(buf), "Screensaver: Off");
    wp_set_label("wp_saver_time", buf);
    const char* base = strrchr(g_settings.saver_file, '/');
    snprintf(buf, sizeof(buf), "Saver shader: %s",
             g_settings.saver_file[0] ? (base ? base + 1 : g_settings.saver_file) : "wallpaper / built-in");
    wp_set_label("wp_saver_file", buf);
}

/* Persist and push the new values to the compositor and the renderer. */
static void wp_motion_apply(void) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "idle_timeout %d", g_settings.saver_minutes * 60);
    shell_send_cmd(cmd);
    wp_ui_refresh();
    settings_save();
    g_surf_dirty = 0xffffffffu;
    shell_request_repaint(-1);
}

static int wp_next_level(const int* levels, int n, int cur) {
    for (int i = 0; i < n; i++)
        if (levels[i] > cur) return levels[i];
    return levels[0];
}

static void on_wp_button(LunaElement* e) {
    static const int fps_levels[] = { 0, 2, 10, 30, 60 };
    static const int speed_levels[] = { 10, 25, 50, 100, 200 };
    static const int saver_levels[] = { 0, 1, 5, 10, 15, 30, 60 };
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!strcmp(id, "wp_pick_file")) { wpd_open(1, 0); return; }
        if (!strcmp(id, "wp_open_dir"))  { wallpaper_open_folder(); return; }
        if (!strcmp(id, "wp_motion_fps")) {
            g_settings.wallpaper_fps = wp_next_level(fps_levels, 5, g_settings.wallpaper_fps);
            wp_motion_apply(); return;
        }
        if (!strcmp(id, "wp_motion_speed")) {
            g_settings.wallpaper_speed = wp_next_level(speed_levels, 5, g_settings.wallpaper_speed);
            wp_motion_apply(); return;
        }
        if (!strcmp(id, "wp_saver_time")) {
            g_settings.saver_minutes = wp_next_level(saver_levels, 7, g_settings.saver_minutes);
            wp_motion_apply(); return;
        }
        if (!strcmp(id, "wp_saver_file")) { wpd_open(1, 1); return; }
    }
}

static void on_desk_menu_pick(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "dm_")) { id = cand; break; }
    }
    if (!id) return;
    dismiss_desk_menu();
    dismiss_tray_menu();
    if (!strcmp(id, "dm_shot")) {
        /* Let the menu leave the screen first, then capture. */
        g_desk_shot_at = g_now + 0.35;
    } else if (!strcmp(id, "dm_term")) {
        for (int a = 0; a < APP_COUNT; a++)
            if (!strcmp(g_apps[a].key, "terminal")) { app_launch(&g_apps[a]); break; }
    } else if (!strcmp(id, "dm_wgshow")) {
        g_settings.widget_hidden = 0;
        widgets_apply();
        settings_save();
    } else if (!strcmp(id, "dm_widgets")) {
        g_settings.widgets_enabled = !g_settings.widgets_enabled;
        apply_dock_visibility();
        settings_mark_toggle("wm_widgets", g_settings.widgets_enabled);
        settings_save();
    } else if (!strcmp(id, "dm_wall")) {
        wpd_open(0, 0);
    } else if (!strcmp(id, "dm_settings")) {
        on_settings_open(NULL);
        int tab = luna_get_element_by_id("stab_apps");
        if (tab >= 0) on_settings_tab(luna_element_at(tab));
    } else if (!strcmp(id, "dm_wallfile")) {
        wpd_open(4, 0);
    } else if (!strcmp(id, "dm_wallreset")) {
        wallpaper_use_theme();
        apply_wallpaper(g_settings.wallpaper);
        settings_save();
    } else if (!strcmp(id, "dm_wallfolder")) {
        wallpaper_open_folder();
    } else if (!strcmp(id, "dm_launch")) {
        on_launchpad_open(NULL);
    }
    shell_request_repaint(-1);
}

static void on_power_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    dismiss_wifi_menu();
    dismiss_bt_menu();
    dismiss_weather_menu();
    dismiss_calendar_menu();
    if (is_shown(g_power_menu_idx)) { dismiss_power_menu(); return; }
    g_thermal_tick_at = g_now;
    power_update_ui();
    luna_mark_layout_dirty();
    set_hidden(g_power_menu_idx, 0);
    position_menu_near(g_power_menu_idx, menu_anchor_from(e, g_mb_bat_idx), luna_window_width - 330.0f);
}

static void on_fan_pick(LunaElement* e) {
    static const struct { const char* id; int mode; int pct; } levels[] = {
        { "fan_auto", 0, -1 }, { "fan_low", 1, 0 }, { "fan_mid", 2, 50 },
        { "fan_high", 3, 75 }, { "fan_max", 4, 100 },
    };
    int mode = -1, pct = -1;
    for (int idx = elem_idx_of(e); idx != -1 && mode < 0; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++)
            if (!strcmp(id, levels[i].id)) { mode = levels[i].mode; pct = levels[i].pct; break; }
    }
    if (mode < 0) return;
    thermal_discover();
    char script[640];
    int has_unit = access("/usr/lib/systemd/system/mbpfan.service", F_OK) == 0 ||
                   access("/etc/systemd/system/mbpfan.service", F_OK) == 0 ||
                   access("/lib/systemd/system/mbpfan.service", F_OK) == 0;
    if (mode == 0) {
        /* Hand the fan back to mbpfan, or to the firmware when there is none. */
        if (has_unit)
            snprintf(script, sizeof(script), "systemctl start mbpfan");
        else if (g_fan_dir[0])
            snprintf(script, sizeof(script), "echo 0 > %s/fan1_manual", g_fan_dir);
        else
            return;
    } else {
        long rmin = 0, rmax = 0;
        char path[160];
        if (!g_fan_dir[0]) { toast_show("Cooling", "No controllable fan found", 3.0); return; }
        snprintf(path, sizeof(path), "%s/fan1_min", g_fan_dir); sysfs_long(path, &rmin);
        snprintf(path, sizeof(path), "%s/fan1_max", g_fan_dir); sysfs_long(path, &rmax);
        if (rmax <= rmin) { toast_show("Cooling", "Fan range unavailable", 3.0); return; }
        long target = rmin + (rmax - rmin) * pct / 100;
        /* mbpfan would immediately overwrite a manual target: stop it first. */
        snprintf(script, sizeof(script), "%secho 1 > %s/fan1_manual; echo %ld > %s/fan1_output",
                 has_unit ? "systemctl stop mbpfan; " : "", g_fan_dir, target, g_fan_dir);
    }
    run_privileged(script);
    g_fan_mode = mode;
    g_thermal_tick_at = g_now + 1.0;   /* let the fan spin before re-reading */
    fan_update_ui();
    luna_mark_layout_dirty();
}

static const char* governor_pick(const char* avail, int level) {
    static const char* const save[] = { "powersave", "conservative", "ondemand", "schedutil" };
    static const char* const bal[]  = { "schedutil", "ondemand", "powersave" };
    static const char* const perf[] = { "performance" };
    const char* const* list = level == 0 ? save : level == 1 ? bal : perf;
    size_t n = level == 0 ? 4 : level == 1 ? 3 : 1;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(list[i]);
        for (const char* p = avail; (p = strstr(p, list[i])); p += l)
            if ((p == avail || p[-1] == ' ') && (p[l] == ' ' || p[l] == 0)) return list[i];
    }
    return NULL;
}

static void on_power_pick(LunaElement* e) {
    int level = -1, turbo = 0;
    for (int idx = elem_idx_of(e); idx != -1 && level < 0 && !turbo; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!strcmp(id, "pw_save")) level = 0;
        else if (!strcmp(id, "pw_bal")) level = 1;
        else if (!strcmp(id, "pw_perf")) level = 2;
        else if (!strcmp(id, "pw_turbo")) turbo = 1;
    }
    char script[512];
    if (turbo) {
        long no_turbo = 0;
        if (!sysfs_long("/sys/devices/system/cpu/intel_pstate/no_turbo", &no_turbo)) return;
        snprintf(script, sizeof(script), "echo %d > /sys/devices/system/cpu/intel_pstate/no_turbo", no_turbo ? 0 : 1);
        run_privileged(script);
    } else if (level >= 0) {
        if (command_available("powerprofilesctl")) {
            static const char* const prof[] = { "power-saver", "balanced", "performance" };
            snprintf(script, sizeof(script), "powerprofilesctl set %s", prof[level]);
            spawn_command(script);
        } else {
            char avail[160] = "";
            sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", avail, sizeof(avail));
            const char* gov = governor_pick(avail, level);
            if (!gov) { toast_show("Power", "No matching CPU governor", 3.0); return; }
            snprintf(script, sizeof(script),
                     "for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo %s > $f; done", gov);
            run_privileged(script);
        }
    } else {
        return;
    }
    g_thermal_tick_at = g_now - 1.2;   /* re-read shortly, once the write has landed */
    luna_mark_layout_dirty();
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
    if (pass >= 0) {
        luna_set_value(pass, "");
        luna_focus_element(pass);
    }
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
    luna_focus_element(-1);
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


static int luna_str_ends_ci(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && !strcasecmp(s + sl - xl, suffix);
}

/* Formats luna-ui can paint via background-image (stb_image + NanoSVG). */
static int luna_icon_path_supported(const char* path) {
    if (!path || !*path) return 0;
    return luna_str_ends_ci(path, ".png") || luna_str_ends_ci(path, ".jpg") ||
           luna_str_ends_ci(path, ".jpeg") || luna_str_ends_ci(path, ".bmp") ||
           luna_str_ends_ci(path, ".tga") || luna_str_ends_ci(path, ".svg");
}

static int luna_icon_accept_path(const char* path, char* out, size_t out_n) {
    if (!path || !*path || access(path, R_OK) != 0) return 0;
    if (!luna_icon_path_supported(path)) return 0;
    snprintf(out, out_n, "%s", path);
    return 1;
}

static void luna_icon_normalize_name(const char* src, char* dst, size_t dst_n) {
    if (!dst || dst_n == 0) return;
    dst[0] = 0;
    if (!src || !*src) return;
    snprintf(dst, dst_n, "%s", src);
    static const char* exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".svg", NULL };
    for (int i = 0; exts[i]; i++) {
        size_t dl = strlen(dst), el = strlen(exts[i]);
        if (dl > el && !strcasecmp(dst + dl - el, exts[i])) {
            dst[dl - el] = 0;
            return;
        }
    }
}

static int luna_icon_try_pixmap_dir(const char* dir, const char* name,
                                    char* out, size_t out_n) {
    static const char* exts[] = { "", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".svg", NULL };
    for (int i = 0; exts[i]; i++) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s%s", dir, name, exts[i]);
        if (n > 0 && (size_t)n < sizeof(path) &&
            luna_icon_accept_path(path, out, out_n)) return 1;
    }
    return 0;
}

static int luna_icon_try_theme(const char* root, const char* theme, const char* name,
                               char* out, size_t out_n) {
    static const int sizes[] = { 512, 256, 192, 128, 96, 72, 64, 48, 32, 24, 22, 16 };
    static const char* contexts[] = { "apps", "applications", NULL };
    static const char* raster_exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", NULL };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (int c = 0; contexts[c]; c++) {
            for (int e = 0; raster_exts[e]; e++) {
                char path[PATH_MAX];
                int n = snprintf(path, sizeof(path), "%s/%s/%dx%d/%s/%s%s",
                                 root, theme, sizes[s], sizes[s], contexts[c], name,
                                 raster_exts[e]);
                if (n > 0 && (size_t)n < sizeof(path) &&
                    luna_icon_accept_path(path, out, out_n)) return 1;
            }
        }
    }
    for (int c = 0; contexts[c]; c++) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s/scalable/%s/%s.svg",
                         root, theme, contexts[c], name);
        if (n > 0 && (size_t)n < sizeof(path) &&
            luna_icon_accept_path(path, out, out_n)) return 1;
    }
    return 0;
}

static int luna_app_icon_resolve(const char* icon, char* out, size_t out_n) {
    if (!out || out_n == 0) return 0;
    out[0] = 0;
    if (!icon || !*icon) return 0;
    if (path_is_absolute(icon))
        return luna_icon_accept_path(icon, out, out_n);

    char name[256];
    luna_icon_normalize_name(icon, name, sizeof(name));
    if (!name[0]) return 0;

    char xdg_icons[PATH_MAX] = "";
    char home_icons[PATH_MAX] = "";
    (void)path_join2(xdg_icons, sizeof(xdg_icons), g_xdg.data_home, "icons");
    (void)path_join2(home_icons, sizeof(home_icons), g_xdg.home, ".icons");
    const char* roots[] = { xdg_icons, home_icons, "/usr/local/share/icons", "/usr/share/icons", NULL };

    const char* requested_theme = getenv("LUNA_ICON_THEME");
    const char* themes[4];
    int tc = 0;
    if (requested_theme && *requested_theme) themes[tc++] = requested_theme;
    themes[tc++] = "Adwaita";
    themes[tc++] = "hicolor";
    themes[tc] = NULL;

    for (int t = 0; themes[t]; t++)
        for (int r = 0; roots[r]; r++)
            if (roots[r][0] && luna_icon_try_theme(roots[r], themes[t], name, out, out_n))
                return 1;

    char user_pixmaps[PATH_MAX] = "";
    (void)path_join2(user_pixmaps, sizeof(user_pixmaps), g_xdg.data_home, "pixmaps");
    const char* pixmaps[] = { user_pixmaps, "/usr/local/share/pixmaps", "/usr/share/pixmaps", NULL };
    for (int i = 0; pixmaps[i]; i++) {
        if (!pixmaps[i][0]) continue;
        if (luna_icon_try_pixmap_dir(pixmaps[i], icon, out, out_n)) return 1;
        if (strcmp(icon, name) && luna_icon_try_pixmap_dir(pixmaps[i], name, out, out_n)) return 1;
    }
    return 0;
}

static void xdg_app_icon_apply(int element_idx, const LunaLpXdgApp* app) {
    if (element_idx < 0) return;
    luna_set_background_image(element_idx, NULL);
    luna_set_text(element_idx, "");
    if (!app) return;
    if (app->icon_path[0]) {
        luna_set_background_image(element_idx, app->icon_path);
        return;
    }
    luna_set_text(element_idx, "\uf2d0");
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
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->id, sizeof(slot->id), "%s", desktop_id);
    snprintf(slot->name, sizeof(slot->name), "%s", entry.name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    snprintf(slot->icon_name, sizeof(slot->icon_name), "%s", entry.icon);
    snprintf(slot->startup_wm_class, sizeof(slot->startup_wm_class), "%s",
             entry.startup_wm_class);
    (void)luna_app_icon_resolve(entry.icon, slot->icon_path, sizeof(slot->icon_path));
    fprintf(stderr, "[luna-shell/icon] %s: Icon=%s -> %s\n",
            slot->name, slot->icon_name[0] ? slot->icon_name : "(none)",
            slot->icon_path[0] ? slot->icon_path : "(not found)");
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
        g_lp_xdg_icon_idx[i] = -1;
        g_lp_xdg_label_idx[i] = -1;
        memset(&g_lp_xdg[i], 0, sizeof(g_lp_xdg[i]));
    }
    g_lp_xdg_count = 0;

    char seen[MAX_LP_XDG * 4][NAME_MAX + 1];
    int seen_count = 0;
    char dir[PATH_MAX];
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

        for (int child = 0; child < luna_element_count(); child++) {
            LunaElement* ch = luna_element_at(child);
            if (!ch || ch->parent_idx != g_lp_xdg_idx[i]) continue;
            if (strstr(ch->class_name, "lp_icon")) g_lp_xdg_icon_idx[i] = child;
            if (strstr(ch->class_name, "lp_label")) g_lp_xdg_label_idx[i] = child;
        }

        if (i < g_lp_xdg_count) {
            set_hidden(g_lp_xdg_idx[i], 0);
            if (g_lp_xdg_label_idx[i] >= 0)
                luna_set_text(g_lp_xdg_label_idx[i], g_lp_xdg[i].name);
            else
                luna_set_text(g_lp_xdg_idx[i], g_lp_xdg[i].name);
            xdg_app_icon_apply(g_lp_xdg_icon_idx[i], &g_lp_xdg[i]);
            wire_subtree(g_lp_xdg_idx[i], on_launch_app);
        } else {
            set_hidden(g_lp_xdg_idx[i], 1);
            if (g_lp_xdg_icon_idx[i] >= 0)
                luna_set_background_image(g_lp_xdg_icon_idx[i], NULL);
        }
    }
    g_lp_xdg_ready = 1;
    fprintf(stderr, "[luna-shell] launchpad: %d XDG applications\n", g_lp_xdg_count);
}

static int lp_xdg_find_by_id(const char* desktop_id) {
    if (!desktop_id || !*desktop_id) return -1;
    for (int i = 0; i < g_lp_xdg_count; i++)
        if (!strcasecmp(g_lp_xdg[i].id, desktop_id)) return i;
    return -1;
}

static void dock_pinned_load(void) {
    g_dock_pinned_count = 0;
    char dir[PATH_MAX], path[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-desktop", 1) ||
        !path_join2(path, sizeof(path), dir, "dock-pinned")) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[PATH_MAX];
    while (g_dock_pinned_count < MAX_DOCK_PINNED && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char* tail = p + strlen(p);
        while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!*p) continue;
        snprintf(g_dock_pinned[g_dock_pinned_count++], NAME_MAX + 1, "%s", p);
    }
    fclose(f);
}

static int dock_pinned_save(void) {
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    if (!xdg_app_dir(dir, sizeof(dir), g_xdg.config_home, "luna-desktop", 1) ||
        !path_join2(path, sizeof(path), dir, "dock-pinned")) return 0;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return 0;
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# Luna Desktop Dock\n");
    for (int i = 0; i < g_dock_pinned_count; i++) fprintf(f, "%s\n", g_dock_pinned[i]);
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

static void dock_xdg_find_children(LunaDockXdgSlot* slot) {
    if (!slot || slot->item_idx < 0) return;
    slot->icon_idx = slot->dot_idx = slot->label_idx = -1;
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* ch = luna_element_at(i);
        if (!ch || ch->parent_idx != slot->item_idx) continue;
        if (strstr(ch->class_name, "dock_icon")) slot->icon_idx = i;
        else if (strstr(ch->class_name, "dock_dot")) slot->dot_idx = i;
        else if (strstr(ch->class_name, "dock_label")) slot->label_idx = i;
    }
}

static int dock_xdg_slot_from_element(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx >= 0; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!id || strncmp(id, "dock_xdg_", 9)) continue;
        int slot = atoi(id + 9);
        return slot >= 0 && slot < MAX_DOCK_XDG ? slot : -1;
    }
    return -1;
}

static LunaWinEntry* find_win_for_xdg_app(const LunaLpXdgApp* app) {
    if (!app) return NULL;
    char desktop_key[NAME_MAX + 1];
    snprintf(desktop_key, sizeof(desktop_key), "%s", app->id);
    size_t n = strlen(desktop_key);
    if (n > 8 && !strcasecmp(desktop_key + n - 8, ".desktop")) desktop_key[n - 8] = 0;
    for (int i = 0; i < g_win_count; i++) {
        const char* app_id = g_wins[i].app_id;
        if (!app_id[0]) continue;
        if (desktop_key[0] && !strcasecmp(desktop_key, app_id)) return &g_wins[i];
        if (app->startup_wm_class[0] && !strcasecmp(app->startup_wm_class, app_id))
            return &g_wins[i];
    }
    return NULL;
}

/* Resolve the real application icon for every open window's app_id: the
 * matching .desktop entry's Icon= first (by desktop id, StartupWMClass or the
 * last reverse-DNS component), then an icon-theme/pixmap file named after the
 * app_id itself.  Results land in g_app_icon_cache for the task list/tray. */
/* The launchpad list is capped (MAX_LP_XDG), so an installed app such as
 * pcmanfm can be missing from it.  Look the .desktop file up directly: by
 * desktop id (also lower-cased / last reverse-DNS part), then by
 * StartupWMClass, and take its Icon=. */
/* Icon name with distro suffixes dropped: firefox-esr -> firefox. */
static int icon_resolve_loose(const char* name, char* out, size_t n) {
    if (luna_app_icon_resolve(name, out, n)) return 1;
    static const char* const suf[] = { "-esr", "-bin", "-stable", "-beta", "-nightly", "-browser" };
    char base[256];
    for (size_t i = 0; i < sizeof(suf) / sizeof(suf[0]); i++) {
        size_t nl = strlen(name), sl = strlen(suf[i]);
        if (nl <= sl || strcasecmp(name + nl - sl, suf[i]) != 0) continue;
        snprintf(base, sizeof(base), "%.*s", (int)(nl - sl), name);
        if (luna_app_icon_resolve(base, out, n)) return 1;
    }
    return 0;
}

/* Every word of the desktop id (weston-terminal) occurs among the words of
 * the app_id (org.freedesktop.weston.wayland-terminal). */
static int app_id_words_cover(const char* key, const char* app_id) {
    char k[NAME_MAX + 1];
    snprintf(k, sizeof(k), "%s", key);
    int words = 0;
    char* save = NULL;
    for (char* w = strtok_r(k, ".-_", &save); w; w = strtok_r(NULL, ".-_", &save)) {
        if (strlen(w) < 3) continue;
        words++;
        int found = 0;
        char a[256];
        snprintf(a, sizeof(a), "%s", app_id);
        char* s2 = NULL;
        for (char* t = strtok_r(a, ".-_", &s2); t && !found; t = strtok_r(NULL, ".-_", &s2))
            found = !strcasecmp(t, w);
        if (!found) return 0;
    }
    return words >= 2;
}

static int app_icon_from_desktop_files(const char* app_id, const char* last, char* out, size_t n) {
    char dirs[PATH_MAX * 2];
    const char* xd = getenv("XDG_DATA_DIRS");
    snprintf(dirs, sizeof(dirs), "%s/applications:%s", g_xdg.data_home[0] ? g_xdg.data_home : "",
             xd && *xd ? xd : "/usr/local/share:/usr/share");
    char* save = NULL;
    for (char* base = strtok_r(dirs, ":", &save); base; base = strtok_r(NULL, ":", &save)) {
        if (*base != '/') continue;
        char dir[PATH_MAX];
        /* the first element already ends in /applications */
        if (strstr(base, "/applications")) snprintf(dir, sizeof(dir), "%s", base);
        else snprintf(dir, sizeof(dir), "%s/applications", base);
        DIR* d = opendir(dir);
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d))) {
            size_t len = strlen(de->d_name);
            if (len < 9 || strcmp(de->d_name + len - 8, ".desktop") != 0) continue;
            char key[NAME_MAX + 1];
            snprintf(key, sizeof(key), "%.*s", (int)(len - 8), de->d_name);
            int hit = !strcasecmp(key, app_id) || !strcasecmp(key, last) || app_id_words_cover(key, app_id);
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            LunaDesktopEntry* e = malloc(sizeof(*e));
            if (!e) continue;
            int ok = desktop_entry_load(path, e);
            if (ok && !hit && e->startup_wm_class[0] && !strcasecmp(e->startup_wm_class, app_id)) hit = 1;
            if (ok && hit && e->icon[0] && icon_resolve_loose(e->icon, out, n)) {
                free(e);
                closedir(d);
                return 1;
            }
            free(e);
        }
        closedir(d);
    }
    return 0;
}

static void app_icon_resolve_one(const char* app_id) {
    if (!app_id || !*app_id) return;
    int slot = -1;
    for (int i = 0; i < g_app_icon_cache_n; i++)
        if (!strcmp(g_app_icon_cache[i].app_id, app_id)) { slot = i; break; }
    /* An app that could not be resolved is retried a few times: the window's
     * app_id and the icon theme can both settle after the first state update
     * (notably while a session is being restored). */
    if (slot >= 0 && (g_app_icon_cache[slot].path[0] || g_app_icon_cache[slot].fails >= 3)) return;
    if (slot < 0 && g_app_icon_cache_n >= APP_ICON_CACHE) return;
    if (!g_lp_xdg_ready) launchpad_populate_xdg();

    const char* last = strrchr(app_id, '.');
    last = last && last[1] ? last + 1 : app_id;
    char path[PATH_MAX] = "";
    for (int i = 0; i < g_lp_xdg_count && !path[0]; i++) {
        const LunaLpXdgApp* a = &g_lp_xdg[i];
        char key[NAME_MAX + 1];
        snprintf(key, sizeof(key), "%s", a->id);
        size_t n = strlen(key);
        if (n > 8 && !strcasecmp(key + n - 8, ".desktop")) key[n - 8] = 0;
        if (!strcasecmp(key, app_id) || !strcasecmp(key, last) ||
            (a->startup_wm_class[0] && !strcasecmp(a->startup_wm_class, app_id)))
            snprintf(path, sizeof(path), "%s", a->icon_path);
    }
    if (!path[0] && !icon_resolve_loose(app_id, path, sizeof(path)) &&
        !icon_resolve_loose(last, path, sizeof(path)))
        (void)app_icon_from_desktop_files(app_id, last, path, sizeof(path));

    if (slot < 0) {
        slot = g_app_icon_cache_n++;
        snprintf(g_app_icon_cache[slot].app_id, sizeof(g_app_icon_cache[slot].app_id), "%s", app_id);
        g_app_icon_cache[slot].fails = 0;
    }
    snprintf(g_app_icon_cache[slot].path, sizeof(g_app_icon_cache[slot].path), "%s", path);
    if (!path[0]) g_app_icon_cache[slot].fails++;
}

static void app_icons_resolve(void) {
    for (int i = 0; i < g_win_count; i++) app_icon_resolve_one(g_wins[i].app_id);
}

static void launch_xdg_app(int app_index) {
    if (app_index < 0 || app_index >= g_lp_xdg_count) return;
    LunaApp tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.key = "xdg";
    tmp.name = g_lp_xdg[app_index].name;
    snprintf(tmp.cmd, sizeof(tmp.cmd), "%s", g_lp_xdg[app_index].path);
    app_launch(&tmp);
}

static void on_dock_xdg_click(LunaElement* e) {
    int s = dock_xdg_slot_from_element(e);
    if (s < 0 || s >= g_dock_xdg_count) return;
    LunaDockXdgSlot* slot = &g_dock_xdg[s];
    if (slot->app_index < 0 || slot->app_index >= g_lp_xdg_count) return;
    LunaLpXdgApp* app = &g_lp_xdg[slot->app_index];
    int force_launch = (luna_last_click_mods() & LUNA_MOD_ALT) != 0;
    LunaWinEntry* w = force_launch ? NULL : find_win_for_xdg_app(app);
    if (!w) { launch_xdg_app(slot->app_index); return; }
    char cmd[64];
    if (w->focused && !w->minimized)
        snprintf(cmd, sizeof(cmd), "minimize %" PRIu64, w->id);
    else
        snprintf(cmd, sizeof(cmd), "activate %" PRIu64, w->id);
    shell_send_cmd(cmd);
}

static void dock_xdg_init(void) {
    g_dock_xdg_count = 0;
    g_dock_xdg_sep_idx = luna_get_element_by_id("dock_xdg_sep");
    if (g_dock_xdg_sep_idx >= 0) set_hidden(g_dock_xdg_sep_idx, 1);
    for (int i = 0; i < MAX_DOCK_XDG; i++) {
        LunaDockXdgSlot* slot = &g_dock_xdg[i];
        memset(slot, 0, sizeof(*slot));
        slot->app_index = slot->item_idx = slot->icon_idx = slot->dot_idx = slot->label_idx = -1;
        char id[32];
        snprintf(id, sizeof(id), "dock_xdg_%02d", i);
        slot->item_idx = luna_get_element_by_id(id);
        if (slot->item_idx < 0) continue;
        dock_xdg_find_children(slot);
        set_hidden(slot->item_idx, 1);
    }
}

static void dock_populate_xdg(void) {
    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    for (int i = 0; i < MAX_DOCK_XDG; i++) {
        LunaDockXdgSlot* slot = &g_dock_xdg[i];
        slot->app_index = -1;
        slot->pinned = 0;
        if (slot->item_idx >= 0) set_hidden(slot->item_idx, 1);
        if (slot->icon_idx >= 0) luna_set_background_image(slot->icon_idx, NULL);
        if (slot->dot_idx >= 0) set_hidden(slot->dot_idx, 1);
    }
    g_dock_xdg_count = 0;

    for (int p = 0; p < g_dock_pinned_count && g_dock_xdg_count < MAX_DOCK_XDG; p++) {
        int app_index = lp_xdg_find_by_id(g_dock_pinned[p]);
        if (app_index < 0) continue;
        LunaDockXdgSlot* slot = &g_dock_xdg[g_dock_xdg_count++];
        LunaLpXdgApp* app = &g_lp_xdg[app_index];
        slot->app_index = app_index;
        slot->pinned = 1;
        if (slot->item_idx < 0) continue;
        set_hidden(slot->item_idx, 0);
        if (slot->label_idx >= 0) luna_set_text(slot->label_idx, app->name);
        xdg_app_icon_apply(slot->icon_idx, app);
        if (slot->dot_idx >= 0) set_hidden(slot->dot_idx, find_win_for_xdg_app(app) == NULL);
        wire_subtree(slot->item_idx, on_dock_xdg_click);
    }
    if (g_dock_xdg_sep_idx >= 0) set_hidden(g_dock_xdg_sep_idx, g_dock_xdg_count == 0);
    luna_mark_layout_dirty();
    fprintf(stderr, "[luna-shell] dock: %d pinned XDG applications\n", g_dock_xdg_count);
}

static void on_launchpad_open(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    dock_populate_xdg();
    set_hidden(g_launchpad_idx, 0);
    if (g_lp_search_idx >= 0) {
        luna_set_value(g_lp_search_idx, "");
        luna_focus_element(g_lp_search_idx);
    }
    g_lp_query[0] = 0;
    if (g_lp_grid_idx >= 0) {
        LunaElement* grid = luna_element_at(g_lp_grid_idx);
        if (grid) {
            grid->scroll_top = 0.0f;
            grid->scroll_dest_top = 0.0f;
        }
    }
    luna_mark_layout_dirty();
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
    if (xdg >= 0) launch_xdg_app(xdg);
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

static int mb_order_known(const char* id);

/* Menubar controls that open a popup from their own click / right-click: the
 * release that follows must not count as an "outside click" and close it. */
static int mb_popup_anchor_hit(int hit) {
    if (widget_of_hit(hit) >= 0) return 1;
    for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx) {
        const char* id = luna_element_at(p)->id;
        if (!id || !id[0]) continue;
        if (!strcmp(id, "mb_app")) return 1;
        if (!strncmp(id, "tray_", 5) && isdigit((unsigned char)id[5])) return 1;
        if (id[0] == 'm' && id[1] == 'b' && id[2] == '_' && mb_order_known(id)) return 1;
    }
    return 0;
}

static int widget_move_click(void);
static void on_mouse_release_hook(int hit, int drag_moved) {
    if (widget_move_click()) return;
    if (drag_moved) {
        /* Slider thumbs use drag_mode=2; flush the final level on release so a
         * fast fling is not lost to the apply throttle. */
        cc_sliders_flush(g_bright_slider_ptr, g_vol_slider_ptr);
    }
    settings_commit_scale_sliders();
    if (drag_moved) return;
    if (hit < 0) return;
    if (is_shown(g_launchpad_idx) &&
        (hit == g_launchpad_idx || hit == luna_get_element_by_id("lp_hint"))) {
        launchpad_close();
        return;
    }
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
    if (is_shown(g_power_menu_idx) &&
        !hit_inside(hit, g_power_menu_idx) &&
        !hit_inside(hit, g_mb_bat_idx))
        dismiss_power_menu();
    if (is_shown(g_desk_menu_idx) && !hit_inside(hit, g_desk_menu_idx) && !mb_popup_anchor_hit(hit))
        dismiss_desk_menu();
    if (is_shown(g_tray_menu_idx) && !hit_inside(hit, g_tray_menu_idx) && !mb_popup_anchor_hit(hit))
        dismiss_tray_menu();
    if (is_shown(g_weather_menu_idx) &&
        !hit_inside(hit, g_weather_menu_idx) &&
        !hit_inside(hit, g_mb_weather_idx) &&
        !hit_inside(hit, luna_get_element_by_id("widget_weather")))
        dismiss_weather_menu();
    if (is_shown(g_calendar_menu_idx) &&
        !hit_inside(hit, g_calendar_menu_idx) &&
        !hit_inside(hit, g_mb_clock_idx) &&
        !hit_inside(hit, luna_get_element_by_id("wg_date")) &&
        !hit_inside(hit, luna_get_element_by_id("widget_clock")) &&
        !hit_inside(hit, g_alarm_ring_idx))
        dismiss_calendar_menu();
    if (is_shown(g_win_menu_idx) && !hit_inside(hit, g_win_menu_idx)) {
        /* Keep open when the click was on a win_item, tray window icon, or
         * modeless-dialog titlebar (handler opens on press; release must not
         * race-dismiss the menu). */
        int on_anchor = 0;
        for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx) {
            const char* id = luna_element_at(p)->id;
            if (!id || !id[0]) continue;
            if (id[0]=='w' && id[1]=='i' && id[2]=='n' && id[3]=='_') { on_anchor = 1; break; }
            if (!strcmp(id, "mb_app")) { on_anchor = 1; break; }
            if (id[0]=='t' && id[1]=='r' && id[2]=='a' && id[3]=='y' && id[4]=='_') {
                on_anchor = 1; break;
            }
        }
        if (!on_anchor) {
            double mx = 0.0, my = 0.0;
            luna_get_pointer(&mx, &my);
            int dlg = shell_dialog_kind_from_hit(hit);
            if (dlg >= 0 && shell_dialog_hit_is_titlebar(dlg, hit, mx, my))
                on_anchor = 1;
        }
        if (!on_anchor) dismiss_win_menu();
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

/* `--wallpaper PATH` / LUNA_WALLPAPER win over the saved setting for this run
 * without rewriting it (g_wallpaper_override, declared with the picker). */

static const char* wallpaper_file_effective(void) {
    const char* f = NULL;
    if (!g_wp_user_chose) {
        f = g_wallpaper_override;
        if (!f || !*f) f = getenv("LUNA_WALLPAPER");
    }
    if (!f || !*f) f = g_settings.wallpaper_file;
    return f && *f && access(f, R_OK) == 0 ? f : NULL;
}

static void apply_wallpaper(const char* theme) {
    int idx = luna_get_element_by_id("wallpaper");
    if (idx < 0) return;
    const char* file = wallpaper_file_effective();
    int kind = file ? wp_kind_of(file) : -1;
    int had_css = g_wp_css != NULL;

    if (kind == WP_HTML && !wp_html_load(idx, file)) {
        toast_show("Wallpaper", "The HTML wallpaper could not be loaded.", 3.0);
        kind = -1;
    }
    if (kind != WP_HTML) {
        wp_set_user_css(NULL);
        int c = luna_get_element_by_id("wp_custom");
        if (c >= 0) {
            luna_dom_set_inner_html(c, "");
            luna_update_classes(c, "active", NULL);
        }
    }
    wp_glsl_set(&g_glsl, kind == WP_GLSL ? file : NULL);
    /* An image replaces the whole built-in scene (gradient, aurora, stars,
     * moon): the CSS `.photo` class hides those layers and sizes the picture
     * to cover the output.  HTML and GLSL wallpapers hide it the same way. */
    if (kind == WP_IMAGE) {
        luna_invalidate_texture(file);
        luna_set_background_image(idx, file);
    } else {
        luna_set_background_image(idx, NULL);
    }
    luna_update_classes(idx, "photo wp_html wp_glsl",
                        kind == WP_IMAGE ? "photo" :
                        kind == WP_HTML  ? "wp_html" :
                        kind == WP_GLSL  ? "wp_glsl" : NULL);
    /* Remove all theme classes; night is the default gradient (no extra class). */
    luna_update_classes(idx, "ocean forest sunset",
                        kind < 0 && strcmp(theme, "night") != 0 ? theme : NULL);
    luna_update_classes(idx, "wallpaper_static",
                        g_settings.wallpaper_animation ? NULL : "wallpaper_static");
    /* The HTML wallpaper's stylesheet rides on the skin reload. */
    if ((had_css || g_wp_css) && skin_apply_styles(g_settings.skin)) {
        luna_dom_restyle(idx);
    }
    g_surf_dirty = 0xffffffffu;
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



static void settings_mark_display_scale(void) {
    int dpi = luna_get_element_by_id("scale_dpi_val");
    int gdk = luna_get_element_by_id("scale_gdk_val");
    int qt  = luna_get_element_by_id("scale_qt_val");
    int cur = luna_get_element_by_id("scale_cur_val");
    if (dpi >= 0) luna_set_text(dpi, g_settings.gdk_dpi_scale);
    if (gdk >= 0) luna_set_text(gdk, g_settings.gdk_scale);
    if (qt  >= 0) luna_set_text(qt,  g_settings.qt_scale_factor);
    if (cur >= 0) luna_set_text(cur, g_settings.xcursor_size);
    settings_sync_scale_sliders();
}


/* Apply the persisted Dock membership after the layout has been bound and
 * again immediately when the user changes it in Settings.  Launchpad entries
 * remain available, so removing an app from the Dock never makes it
 * inaccessible. */
static void apply_dock_app_settings(void) {
    for (int i = 0; i < APP_COUNT; i++) {
        char id[64];
        snprintf(id, sizeof(id), "dock_%s", g_apps[i].key);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) {
            set_hidden(idx, !g_apps[i].dock_visible);
        }
    }
    /* #dock uses fit-content, so hidden launchers collapse automatically. */
    g_dock_width_dirty = 1;
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
    snprintf(cmd, sizeof(cmd), "wm_config dock_snap %d", g_settings.dock_snap);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config edge_tile %d", g_settings.edge_tile);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config corner_snap %d", g_settings.corner_snap);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config tray_group %d", g_settings.tray_group);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config top_edge_maximize %d", g_settings.top_edge_maximize);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config titlebar_double_click %d", g_settings.titlebar_double_click);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config super_shortcuts %d", g_settings.super_shortcuts);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "state_interval %d", g_settings.state_interval);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "idle_timeout %d", g_settings.saver_minutes * 60);
    ok &= shell_send_cmd(cmd);
    /* The active skin supplies the titlebar style/colors; decoration ownership
     * is kept compositor-side by skin_apply_wm_decoration(). */
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

    snprintf(cmd, sizeof(cmd), "wm_config focus_outline %d", g_settings.active_window_outline);
    ok &= shell_send_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "wm_config cascade_windows %d", g_settings.cascade_new_windows);
    ok &= shell_send_cmd(cmd);

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
    settings_mark_toggle("wm_dock_snap", g_settings.dock_snap);
    settings_mark_toggle("wm_tile", g_settings.edge_tile);
    settings_mark_toggle("wm_corner", g_settings.corner_snap);
    settings_mark_toggle("wm_tray_group", g_settings.tray_group);
    settings_mark_toggle("wm_top_maximize", g_settings.top_edge_maximize);
    settings_mark_toggle("wm_double_click", g_settings.titlebar_double_click);
    settings_mark_toggle("wm_classic_titlebar", g_settings.classic_titlebar);
    settings_mark_toggle("wm_shortcuts", g_settings.super_shortcuts);
    settings_mark_toggle("wm_focus_outline", g_settings.active_window_outline);
    settings_mark_toggle("wm_window_list", g_settings.window_list_visible);
    settings_mark_toggle("wm_window_tray", g_settings.window_tray_icons);
    settings_mark_toggle("wm_cascade", g_settings.cascade_new_windows);
    settings_mark_toggle("wm_dock", g_settings.dock_enabled);
    settings_mark_toggle("wm_widgets", g_settings.widgets_enabled);
    settings_mark_toggle("wm_dock_mag", g_settings.dock_magnification);
    settings_mark_toggle("wm_wallpaper_motion", g_settings.wallpaper_animation);
    settings_mark_toggle("wm_restore", g_settings.session_restore);
    settings_mark_gap();
    settings_mark_stats();
    settings_mark_audio_backend(g_settings.audio_backend);
    settings_mark_brightness_backend(g_settings.brightness_backend);
    settings_mark_display_scale();
    settings_mark_shortcuts();
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
    set_hidden(g_settings_panel_display, 1);
    set_hidden(g_settings_panel_wm, 1);
    if (g_stab_apps_idx  >= 0) luna_update_classes(g_stab_apps_idx,  "active", "active");
    if (g_stab_disp_idx  >= 0) luna_update_classes(g_stab_disp_idx,  "active", NULL);
    if (g_stab_lang_idx  >= 0) luna_update_classes(g_stab_lang_idx,  "active", NULL);
    if (g_stab_kb_idx    >= 0) luna_update_classes(g_stab_kb_idx,    "active", NULL);
    if (g_stab_sound_idx >= 0) luna_update_classes(g_stab_sound_idx, "active", NULL);
    if (g_stab_display_idx >= 0) luna_update_classes(g_stab_display_idx, "active", NULL);
    if (g_stab_wm_idx    >= 0) luna_update_classes(g_stab_wm_idx,    "active", NULL);
}

static void on_settings_open(LunaElement* e) {
    (void)e;
    dismiss_popovers();
    settings_populate_ui();
    set_hidden(g_settings_idx, 0);
    /* Resolve the subtree now that it is visible, but keep the viewport-
     * relative position authored by the active skin.  center_element() used
     * to provide this eager layout as a side effect while also overwriting
     * CSS, making the native sheet sit lower than the browser preview. */
    luna_update(g_now, 0.0);
}

static void on_settings_close(LunaElement* e) {
    (void)e;
    shortcut_capture_end();
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
    int is_display = !strcmp(id, "stab_display");
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
    if (g_stab_display_idx >= 0)
        luna_update_classes(g_stab_display_idx, "active", is_display ? "active" : NULL);
    if (g_stab_wm_idx >= 0)
        luna_update_classes(g_stab_wm_idx, "active", is_wm ? "active" : NULL);
    set_hidden(g_settings_panel_apps, !is_apps);
    set_hidden(g_settings_panel_disp, !is_disp);
    set_hidden(g_settings_panel_lang, !is_lang);
    set_hidden(g_settings_panel_kb, !is_kb);
    set_hidden(g_settings_panel_sound, !is_sound);
    set_hidden(g_settings_panel_display, !is_display);
    set_hidden(g_settings_panel_wm, !is_wm);
    if (is_sound || is_display) settings_update_sound_status();
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
    else if (!strcmp(id, "wm_dock_snap")) value = &g_settings.dock_snap;
    else if (!strcmp(id, "wm_tile")) value = &g_settings.edge_tile;
    else if (!strcmp(id, "wm_corner")) value = &g_settings.corner_snap;
    else if (!strcmp(id, "wm_tray_group")) value = &g_settings.tray_group;
    else if (!strcmp(id, "wm_top_maximize")) value = &g_settings.top_edge_maximize;
    else if (!strcmp(id, "wm_double_click")) value = &g_settings.titlebar_double_click;
    else if (!strcmp(id, "wm_classic_titlebar")) value = &g_settings.classic_titlebar;
    else if (!strcmp(id, "wm_shortcuts")) value = &g_settings.super_shortcuts;
    else if (!strcmp(id, "wm_dock")) value = &g_settings.dock_enabled;
    else if (!strcmp(id, "wm_widgets")) value = &g_settings.widgets_enabled;
    else if (!strcmp(id, "wm_dock_mag")) value = &g_settings.dock_magnification;
    else if (!strcmp(id, "wm_wallpaper_motion")) value = &g_settings.wallpaper_animation;
    else if (!strcmp(id, "wm_restore")) value = &g_settings.session_restore;
    else if (!strcmp(id, "wm_focus_outline")) value = &g_settings.active_window_outline;
    else if (!strcmp(id, "wm_window_list")) value = &g_settings.window_list_visible;
    else if (!strcmp(id, "wm_window_tray")) value = &g_settings.window_tray_icons;
    else if (!strcmp(id, "wm_cascade")) value = &g_settings.cascade_new_windows;
    if (!value) return;
    *value = !*value;
    settings_mark_toggle(id, *value);
    if (!strcmp(id, "wm_wallpaper_motion"))
        apply_wallpaper(g_settings.wallpaper);
    else if (!strcmp(id, "wm_dock") || !strcmp(id, "wm_widgets"))
        apply_dock_visibility();
    else
        apply_wm_settings();
    if (!strcmp(id, "wm_window_list")) update_window_list_ui();
    if (!strcmp(id, "wm_window_tray")) update_tray_ui();
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
    wallpaper_use_theme();
    settings_mark_wallpaper(theme);
    apply_wallpaper(theme);
    settings_save();
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

static void airdrop_ask_decide(int accept);
static char g_ad_ask_text[260];

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
    case ACT_AIRDROP:
        if (t >= 0)  luna_set_text(t,  "Accept AirDrop?");
        if (m >= 0)  luna_set_text(m,  g_ad_ask_text);
        if (ok >= 0) luna_set_text(ok, "Accept");
        if (icon >= 0) {
            luna_set_text(icon, "\xef\x87\x98"); /* paper-plane, U+F1D8 */
            luna_update_classes(icon, "restart logout", NULL);
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
    /* The Wayland session presents this subtree through a compact layer
     * surface.  Resolve the newly-visible box before centring it so
     * wl_surfs_update() sees the real dialog geometry on the same frame.
     * Without this pass it can reuse the display:none layout (0/stale size),
     * leaving the confirmation surface unmapped or outside its input box. */
    luna_update(g_now, 0.0);
    center_element(g_confirm_box_idx >= 0 ? g_confirm_box_idx : g_confirm_idx);
}

static void on_shutdown(LunaElement* e) { (void)e; confirm_open(ACT_SHUTDOWN); }
static void on_restart(LunaElement* e)  { (void)e; confirm_open(ACT_RESTART); }
static void on_logout(LunaElement* e)   { (void)e; confirm_open(ACT_LOGOUT); }

static void on_confirm_cancel(LunaElement* e) {
    (void)e;
    if (g_pending_action == ACT_AIRDROP) airdrop_ask_decide(0);
    g_pending_action = ACT_NONE;
    set_hidden(g_confirm_idx, 1);
}

static void request_session_logout(void) {
    const char* text = getenv("LUNA_SESSION_PID");
    if (!text || !*text) return;

    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    /* Only signal our actual parent.  This both rejects stale/injected values
     * and keeps a standalone luna-shell from affecting an unrelated process. */
    if (errno || !end || *end || value <= 1 || value > INT_MAX ||
        (pid_t)value != getppid()) return;
    (void)kill((pid_t)value, SIGUSR1);
}

static void on_confirm_ok(LunaElement* e) {
    (void)e;
    int action = g_pending_action;
    g_pending_action = ACT_NONE;
    set_hidden(g_confirm_idx, 1);
    if (action == ACT_AIRDROP) { airdrop_ask_decide(1); return; }
    /* Persist open dock apps before leaving / powering off. */
    session_save();
    switch (action) {
    case ACT_SHUTDOWN: spawn_command("systemctl poweroff"); break;
    case ACT_RESTART:  spawn_command("systemctl reboot");  break;
    /* Ask luna-session to stop the compositor and every session helper.  It
     * exits successfully so the display manager regains control without a
     * systemd Restart=on-failure loop.  Direct exit remains the standalone
     * fallback when no supervisor is present. */
    case ACT_LOGOUT:
        request_session_logout();
        exit(EXIT_SUCCESS);
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

static float g_scale_dpi_level, g_scale_gdk_level, g_scale_qt_level, g_scale_cur_level;
static float g_scale_dpi_written = -1.0f, g_scale_gdk_written = -1.0f;
static float g_scale_qt_written = -1.0f, g_scale_cur_written = -1.0f;
static double g_scale_dpi_apply, g_scale_gdk_apply, g_scale_qt_apply, g_scale_cur_apply;
static int g_scale_dummy_available;
static int g_scale_dirty = 0;
static LunaSliderIds g_scale_dpi_slider, g_scale_gdk_slider, g_scale_qt_slider, g_scale_cur_slider;

enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_LAUNCHPAD,
    SHORTCUT_SETTINGS,
    SHORTCUT_SCREENSHOT
};
static int g_shortcut_capture = SHORTCUT_NONE;
static int g_shortcut_super_held = 0;
static int g_shortcut_got_combo = 0;

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
    if (!is_shown(g_cc_idx) && !is_shown(g_settings_idx)) {
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

static LunaSliderIds* slider_from_track_id(const char* id) {
    if (!id || !*id) return NULL;
    if (!strcmp(id, "bright_track")) return g_bright_slider_ptr;
    if (!strcmp(id, "vol_track")) return g_vol_slider_ptr;
    if (!strcmp(id, "scale_dpi_track")) return &g_scale_dpi_slider;
    if (!strcmp(id, "scale_gdk_track")) return &g_scale_gdk_slider;
    if (!strcmp(id, "scale_qt_track")) return &g_scale_qt_slider;
    if (!strcmp(id, "scale_cur_track")) return &g_scale_cur_slider;
    return NULL;
}

static void on_cc_track(LunaElement* e) {
    int track = -1;
    LunaSliderIds* ids = NULL;
    for (int i = elem_idx_of(e); i != -1; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        ids = slider_from_track_id(id);
        if (ids) { track = i; break; }
    }
    if (track < 0 || !ids || !ids->resolved || ids->thumb < 0) return;

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

static void format_scale_value(char* buf, size_t n, float v, int integer) {
    if (integer) {
        snprintf(buf, n, "%d", (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
        return;
    }
    snprintf(buf, n, "%.2f", v);
    char* dot = strchr(buf, '.');
    if (!dot) return;
    char* e = buf + strlen(buf);
    while (e > dot + 1 && *(e - 1) == '0') { e--; *e = 0; }
    if (e > buf && *(e - 1) == '.') *(e - 1) = 0;
}

static float scale_parse_value(const char* s, float fallback) {
    if (!s || !*s) return fallback;
    char* end = NULL;
    float v = strtof(s, &end);
    return (end && end != s) ? v : fallback;
}

static float scale_clamp_step(float v, float vmin, float vmax, float vstep) {
    if (vstep > 0.0f) v = roundf(v / vstep) * vstep;
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return v;
}

static float scale_to_ratio(float v, float vmin, float vmax) {
    if (vmax <= vmin) return 0.0f;
    float r = (v - vmin) / (vmax - vmin);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

static int scale_write_generic(float level01, float vmin, float vmax, float vstep,
                               int integer, char* dst, size_t dst_n, const char* label_id) {
    float v = scale_clamp_step(vmin + level01 * (vmax - vmin), vmin, vmax, vstep);
    format_scale_value(dst, dst_n, v, integer);
    int li = luna_get_element_by_id(label_id);
    if (li >= 0) luna_set_text(li, dst);
    g_scale_dirty = 1;
    return 1;
}

static int scale_write_dpi(float l) {
    return scale_write_generic(l, 0.50f, 2.00f, 0.05f, 0,
                               g_settings.gdk_dpi_scale, sizeof(g_settings.gdk_dpi_scale),
                               "scale_dpi_val");
}
static int scale_write_gdk(float l) {
    return scale_write_generic(l, 1.0f, 3.0f, 1.0f, 1,
                               g_settings.gdk_scale, sizeof(g_settings.gdk_scale),
                               "scale_gdk_val");
}
static int scale_write_qt(float l) {
    return scale_write_generic(l, 0.50f, 2.00f, 0.05f, 0,
                               g_settings.qt_scale_factor, sizeof(g_settings.qt_scale_factor),
                               "scale_qt_val");
}
static int scale_write_cur(float l) {
    return scale_write_generic(l, 16.0f, 64.0f, 4.0f, 1,
                               g_settings.xcursor_size, sizeof(g_settings.xcursor_size),
                               "scale_cur_val");
}

static void scale_slider_init(LunaSliderIds* ids, float* level, float* last_written,
                              double* last_apply, int (*write_fn)(float),
                              const char* thumb, const char* fill, const char* track) {
    memset(ids, 0, sizeof(*ids));
    ids->thumb = ids->fill = ids->track = -1;
    ids->level = level;
    ids->available = &g_scale_dummy_available;
    ids->last_apply = last_apply;
    ids->last_written = last_written;
    ids->write_fn = write_fn;
    ids->kind = "scale";
    g_scale_dummy_available = 1;
    *last_written = -1.0f;
    *last_apply = 0.0;
    slider_resolve(ids, thumb, fill, track);
}

static void settings_sync_scale_sliders(void) {
    g_scale_dpi_level = scale_to_ratio(
        scale_parse_value(g_settings.gdk_dpi_scale, 1.0f), 0.50f, 2.00f);
    g_scale_gdk_level = scale_to_ratio(
        scale_parse_value(g_settings.gdk_scale, 1.0f), 1.0f, 3.0f);
    g_scale_qt_level = scale_to_ratio(
        scale_parse_value(g_settings.qt_scale_factor, 1.0f), 0.50f, 2.00f);
    g_scale_cur_level = scale_to_ratio(
        scale_parse_value(g_settings.xcursor_size, 24.0f), 16.0f, 64.0f);
    if (g_scale_dpi_slider.resolved)
        slider_set_ratio(g_scale_dpi_slider.thumb, g_scale_dpi_slider.fill,
                         g_scale_dpi_slider.track, g_scale_dpi_level);
    if (g_scale_gdk_slider.resolved)
        slider_set_ratio(g_scale_gdk_slider.thumb, g_scale_gdk_slider.fill,
                         g_scale_gdk_slider.track, g_scale_gdk_level);
    if (g_scale_qt_slider.resolved)
        slider_set_ratio(g_scale_qt_slider.thumb, g_scale_qt_slider.fill,
                         g_scale_qt_slider.track, g_scale_qt_level);
    if (g_scale_cur_slider.resolved)
        slider_set_ratio(g_scale_cur_slider.thumb, g_scale_cur_slider.fill,
                         g_scale_cur_slider.track, g_scale_cur_level);
    if (g_scale_dpi_slider.resolved) {
        g_scale_dpi_slider.last_thumb_x = NAN;
        g_scale_gdk_slider.last_thumb_x = NAN;
        g_scale_qt_slider.last_thumb_x = NAN;
        g_scale_cur_slider.last_thumb_x = NAN;
    }
    g_scale_dirty = 0;
}

static void settings_commit_scale_sliders(void) {
    if (!g_scale_dirty) return;
    g_scale_dirty = 0;
    apply_toolkit_session_env();
    settings_save();
}

static int shortcut_key_is_mod(int key) {
    return key == LUNA_KEY_LEFT_SHIFT || key == LUNA_KEY_RIGHT_SHIFT ||
           key == LUNA_KEY_LEFT_CONTROL || key == LUNA_KEY_RIGHT_CONTROL ||
           key == LUNA_KEY_LEFT_ALT || key == LUNA_KEY_RIGHT_ALT ||
           key == LUNA_KEY_LEFT_SUPER || key == LUNA_KEY_RIGHT_SUPER;
}

static int shortcut_parse_token(const char* tok, int* key, int* is_mod) {
    *is_mod = 0;
    if (!tok || !*tok) return 0;
    if (!strcasecmp(tok, "super") || !strcasecmp(tok, "win") || !strcasecmp(tok, "meta")) {
        *key = LUNA_KEY_LEFT_SUPER; *is_mod = 1; return 1;
    }
    if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) {
        *key = LUNA_KEY_LEFT_CONTROL; *is_mod = 1; return 1;
    }
    if (!strcasecmp(tok, "alt")) {
        *key = LUNA_KEY_LEFT_ALT; *is_mod = 1; return 1;
    }
    if (!strcasecmp(tok, "shift")) {
        *key = LUNA_KEY_LEFT_SHIFT; *is_mod = 1; return 1;
    }
    if (!strcasecmp(tok, "print") || !strcasecmp(tok, "prtsc") ||
        !strcasecmp(tok, "printscreen") || !strcasecmp(tok, "print_screen")) {
        *key = LUNA_KEY_PRINT_SCREEN; return 1;
    }
    if (!strcasecmp(tok, "space")) { *key = LUNA_KEY_SPACE; return 1; }
    if (!strcasecmp(tok, "comma") || !strcmp(tok, ",")) { *key = LUNA_KEY_COMMA; return 1; }
    if ((tok[0] == 'F' || tok[0] == 'f') && tok[1] >= '1' && tok[1] <= '9') {
        int n = atoi(tok + 1);
        if (n >= 1 && n <= 12) { *key = LUNA_KEY_F1 + (n - 1); return 1; }
    }
    if (tok[0] && !tok[1]) {
        unsigned char c = (unsigned char)tok[0];
        if (c >= 'a' && c <= 'z') { *key = 'A' + (c - 'a'); return 1; }
        if (c >= 'A' && c <= 'Z') { *key = (int)c; return 1; }
        if (c >= '0' && c <= '9') { *key = (int)c; return 1; }
        if (c == ',') { *key = LUNA_KEY_COMMA; return 1; }
    }
    return 0;
}

static int shortcut_parse(const char* spec, int* key, int* mods) {
    *key = 0;
    *mods = 0;
    if (!spec || !*spec || !strcasecmp(spec, "none") || !strcasecmp(spec, "off"))
        return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    int have_key = 0;
    char* save = NULL;
    for (char* tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char* e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\t')) { e--; *e = 0; }
        int k = 0, is_mod = 0;
        if (!shortcut_parse_token(tok, &k, &is_mod)) continue;
        if (is_mod) {
            if (k == LUNA_KEY_LEFT_SUPER) *mods |= LUNA_MOD_SUPER;
            else if (k == LUNA_KEY_LEFT_CONTROL) *mods |= LUNA_MOD_CONTROL;
            else if (k == LUNA_KEY_LEFT_ALT) *mods |= LUNA_MOD_ALT;
            else if (k == LUNA_KEY_LEFT_SHIFT) *mods |= LUNA_MOD_SHIFT;
            if (!have_key) { *key = k; have_key = 1; }
        } else {
            *key = k;
            have_key = 1;
        }
    }
    return have_key && *key;
}

static void shortcut_format(char* buf, size_t n, int key, int mods) {
    if (!key) { snprintf(buf, n, "none"); return; }
    if (key == LUNA_KEY_LEFT_SUPER || key == LUNA_KEY_RIGHT_SUPER) {
        snprintf(buf, n, "Super");
        return;
    }
    char kname[16];
    if (key == LUNA_KEY_PRINT_SCREEN) snprintf(kname, sizeof(kname), "Print");
    else if (key == LUNA_KEY_SPACE) snprintf(kname, sizeof(kname), "Space");
    else if (key == LUNA_KEY_COMMA) snprintf(kname, sizeof(kname), ",");
    else if (key >= LUNA_KEY_F1 && key <= LUNA_KEY_F12)
        snprintf(kname, sizeof(kname), "F%d", key - LUNA_KEY_F1 + 1);
    else if (key >= 'A' && key <= 'Z') snprintf(kname, sizeof(kname), "%c", key);
    else if (key >= '0' && key <= '9') snprintf(kname, sizeof(kname), "%c", key);
    else snprintf(kname, sizeof(kname), "?");
    char pre[24] = "";
    if (mods & LUNA_MOD_CONTROL) strncat(pre, "Ctrl+", sizeof(pre) - strlen(pre) - 1);
    if (mods & LUNA_MOD_ALT)     strncat(pre, "Alt+", sizeof(pre) - strlen(pre) - 1);
    if (mods & LUNA_MOD_SHIFT)   strncat(pre, "Shift+", sizeof(pre) - strlen(pre) - 1);
    if (mods & LUNA_MOD_SUPER)   strncat(pre, "Super+", sizeof(pre) - strlen(pre) - 1);
    snprintf(buf, n, "%s%s", pre, kname);
}

static int shortcut_match(const char* spec, int key, int mods) {
    int want_key = 0, want_mods = 0;
    if (!shortcut_parse(spec, &want_key, &want_mods)) return 0;
    if (want_key == LUNA_KEY_LEFT_SUPER || want_key == LUNA_KEY_RIGHT_SUPER) {
        return (key == LUNA_KEY_LEFT_SUPER || key == LUNA_KEY_RIGHT_SUPER) &&
               !(mods & (LUNA_MOD_CONTROL | LUNA_MOD_ALT | LUNA_MOD_SHIFT));
    }
    int mask = LUNA_MOD_SHIFT | LUNA_MOD_CONTROL | LUNA_MOD_ALT | LUNA_MOD_SUPER;
    return key == want_key && (mods & mask) == (want_mods & mask);
}

static const char* shortcut_slot_spec(int slot) {
    if (slot == SHORTCUT_LAUNCHPAD) return g_settings.shortcut_launchpad;
    if (slot == SHORTCUT_SETTINGS) return g_settings.shortcut_settings;
    if (slot == SHORTCUT_SCREENSHOT) return g_settings.shortcut_screenshot;
    return "none";
}

static char* shortcut_slot_dst(int slot) {
    if (slot == SHORTCUT_LAUNCHPAD) return g_settings.shortcut_launchpad;
    if (slot == SHORTCUT_SETTINGS) return g_settings.shortcut_settings;
    if (slot == SHORTCUT_SCREENSHOT) return g_settings.shortcut_screenshot;
    return NULL;
}

static const char* shortcut_slot_label_id(int slot) {
    if (slot == SHORTCUT_LAUNCHPAD) return "shortcut_launchpad_key";
    if (slot == SHORTCUT_SETTINGS) return "shortcut_settings_key";
    if (slot == SHORTCUT_SCREENSHOT) return "shortcut_screenshot_key";
    return NULL;
}

static const char* shortcut_slot_row_id(int slot) {
    if (slot == SHORTCUT_LAUNCHPAD) return "shortcut_launchpad";
    if (slot == SHORTCUT_SETTINGS) return "shortcut_settings";
    if (slot == SHORTCUT_SCREENSHOT) return "shortcut_screenshot";
    return NULL;
}

static void settings_mark_shortcuts(void) {
    for (int slot = SHORTCUT_LAUNCHPAD; slot <= SHORTCUT_SCREENSHOT; slot++) {
        const char* lid = shortcut_slot_label_id(slot);
        const char* rid = shortcut_slot_row_id(slot);
        int li = lid ? luna_get_element_by_id(lid) : -1;
        int ri = rid ? luna_get_element_by_id(rid) : -1;
        if (li >= 0) luna_set_text(li, shortcut_slot_spec(slot));
        if (ri >= 0)
            luna_update_classes(ri, "capturing",
                                g_shortcut_capture == slot ? "capturing" : NULL);
    }
}

static void shortcut_capture_begin(int slot) {
    g_shortcut_capture = slot;
    g_shortcut_super_held = 0;
    g_shortcut_got_combo = 0;
    settings_mark_shortcuts();
    int li = luna_get_element_by_id(shortcut_slot_label_id(slot));
    if (li >= 0) luna_set_text(li, "Press a key…");
}

static void shortcut_capture_end(void) {
    g_shortcut_capture = SHORTCUT_NONE;
    g_shortcut_super_held = 0;
    g_shortcut_got_combo = 0;
    settings_mark_shortcuts();
}

static void shortcut_assign(int slot, int key, int mods) {
    char* dst = shortcut_slot_dst(slot);
    if (!dst) return;
    shortcut_format(dst, 32, key, mods);
    settings_save();
    shortcut_capture_end();
    toast_show("Keyboard", "Shortcut updated", 2.0);
}

static void on_shortcut_row(LunaElement* e) {
    const char* id = NULL;
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* cand = luna_element_at(i)->id;
        if (str_has_prefix(cand, "shortcut_")) { id = cand; break; }
    }
    if (!id) return;
    int slot = SHORTCUT_NONE;
    if (!strcmp(id, "shortcut_launchpad") || !strcmp(id, "shortcut_launchpad_key"))
        slot = SHORTCUT_LAUNCHPAD;
    else if (!strcmp(id, "shortcut_settings") || !strcmp(id, "shortcut_settings_key"))
        slot = SHORTCUT_SETTINGS;
    else if (!strcmp(id, "shortcut_screenshot") || !strcmp(id, "shortcut_screenshot_key"))
        slot = SHORTCUT_SCREENSHOT;
    if (!slot) return;
    if (g_shortcut_capture == slot) shortcut_capture_end();
    else shortcut_capture_begin(slot);
}

static int shortcut_handle_key(int key, int mods, int action) {
    if (!g_shortcut_capture) return 0;
    int super = (key == LUNA_KEY_LEFT_SUPER || key == LUNA_KEY_RIGHT_SUPER);
    if (action == LUNA_RELEASE) {
        if (super && g_shortcut_super_held && !g_shortcut_got_combo) {
            shortcut_assign(g_shortcut_capture, LUNA_KEY_LEFT_SUPER, 0);
            return 1;
        }
        if (super) g_shortcut_super_held = 0;
        return 1;
    }
    if (action != LUNA_PRESS) return 1;
    if (key == LUNA_KEY_ESCAPE) {
        shortcut_capture_end();
        return 1;
    }
    if (key == LUNA_KEY_BACKSPACE || key == LUNA_KEY_DELETE) {
        char* dst = shortcut_slot_dst(g_shortcut_capture);
        if (dst) snprintf(dst, 32, "none");
        settings_save();
        shortcut_capture_end();
        toast_show("Keyboard", "Shortcut cleared", 2.0);
        return 1;
    }
    if (super) {
        g_shortcut_super_held = 1;
        return 1;
    }
    if (shortcut_key_is_mod(key)) return 1;
    g_shortcut_got_combo = 1;
    shortcut_assign(g_shortcut_capture, key, mods);
    return 1;
}

static int shortcut_try_dispatch(int key, int mods) {
    if (g_shell_ipc_launchpad_toggle) {
        g_shell_ipc_launchpad_toggle = 0;
        return 1;
    }
    if (shortcut_match(g_settings.shortcut_launchpad, key, mods)) {
        if (is_shown(g_launchpad_idx)) launchpad_close();
        else on_launchpad_open(NULL);
        return 1;
    }
    if (shortcut_match(g_settings.shortcut_settings, key, mods)) {
        on_settings_open(NULL);
        return 1;
    }
    if (shortcut_match(g_settings.shortcut_screenshot, key, mods)) {
        take_timestamped_screenshot();
        return 1;
    }
    return 0;
}

static void launchpad_scroll_page(int dir) {
    if (g_lp_grid_idx < 0) return;
    LunaElement* grid = luna_element_at(g_lp_grid_idx);
    if (!grid) return;
    float step = grid->h > 80.0f ? grid->h * 0.8f : 240.0f;
    float next = grid->scroll_top + dir * step;
    if (next < 0.0f) next = 0.0f;
    float max_s = grid->scroll_content_h - grid->h;
    if (max_s < 0.0f) max_s = 0.0f;
    if (next > max_s) next = max_s;
    grid->scroll_top = next;
    grid->scroll_dest_top = next;
    luna_mark_layout_dirty();
}

/* Launchpad is rendered as its own full-output layer surface.  Wheel events
 * received on that surface should scroll the application viewport even when
 * the pointer is over the search field, a gap between tiles, or the footer.
 * Routing solely through luna_scroll() makes the result depend on the current
 * document hit-test and can therefore miss #lp_grid altogether. */
static void shell_scroll(double xoff, double yoff) {
    /* A normal wheel also scrolls the compact horizontal task strip. */
    LunaElement* tray = luna_element_at(g_tray_area_idx);
    if (tray && is_shown(g_tray_area_idx) &&
        g_luna_mx >= tray->x && g_luna_mx < tray->x + tray->w &&
        g_luna_my >= tray->y && g_luna_my < tray->y + tray->h) {
        float inner_w = tray->w - tray->pad_l - tray->pad_r - tray->border_width * 2.0f;
        if (tray->scroll_content_w > inner_w + 1.0f) {
            double delta = fabs(xoff) > 0.001 ? xoff : yoff;
            add_scroll_left(g_tray_area_idx, -(float)delta * 24.0f, 1);
            shell_request_repaint(1);
            return;
        }
    }
    if (is_shown(g_launchpad_idx) && g_lp_grid_idx >= 0 && fabs(yoff) > 0.001) {
        LunaElement* grid = luna_element_at(g_lp_grid_idx);
        if (grid) {
            float inner_h = grid->h - grid->pad_t - grid->pad_b -
                            grid->border_width * 2.0f;
            if (inner_h < 0.0f) inner_h = 0.0f;
            float max_s = grid->scroll_content_h - inner_h;
            if (max_s < 0.0f) max_s = 0.0f;
            float next = grid->scroll_dest_top - (float)yoff * 18.0f;
            if (next < 0.0f) next = 0.0f;
            if (next > max_s) next = max_s;
            grid->scroll_top = next;
            grid->scroll_dest_top = next;
            luna_mark_layout_dirty();
        }
        if (fabs(xoff) <= 0.001) return;
    }
    luna_scroll(xoff, yoff);
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
            app_icons_resolve();
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
        g_win_menu_dialog = -1;
        win_menu_detach_embed();
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

static unsigned g_session_sig;
static double g_session_autosave_at;

/* Cheap fingerprint of everything session_write() persists. */
static unsigned session_signature(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < g_win_count; i++) {
        const LunaWinEntry* w = &g_wins[i];
        for (const char* c = w->app_id; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
        unsigned v[5] = { (unsigned)w->x, (unsigned)w->y, (unsigned)w->minimized,
                          (unsigned)w->maximized, (unsigned)w->fullscreen };
        for (int k = 0; k < 5; k++) h = (h ^ v[k]) * 16777619u;
    }
    return h;
}

static void session_write(void) {
    /* An empty window list means the clients are already gone (logout) or the
     * compositor is not up yet; keep the last good session instead of
     * clobbering it with nothing. */
    if (g_win_count <= 0) return;
    ensure_state_dir();
    g_session_sig = session_signature();

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

static void session_save(void) {
    if (!g_settings.session_restore) return;
    ensure_state_dir();
    /* Shutdown is outside the frame loop, so take one synchronous compositor
     * snapshot here rather than waiting for the background poller. */
    (void)shell_state_watch_drain();
    g_shell_state_changed = 0;
    load_shell_state();
    session_write();
}

/* Keep the file current while the desktop runs: the logout path often sees an
 * already-empty window list, which used to lose every position. */
static void session_autosave_tick(void) {
    if (!g_settings.session_restore || g_session_restore_active || !g_session_restore_done) return;
    if (g_now < g_session_autosave_at) return;
    g_session_autosave_at = g_now + 5.0;
    if (g_win_count <= 0 || session_signature() == g_session_sig) return;
    session_write();
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
                    rw->win_id = 0;
                    /* Map at the saved spot instead of jumping after the first
                     * frame; the later `move` stays as a fallback. */
                    if (!rw->maximized && !rw->fullscreen && !rw->minimized && app_id[0]) {
                        char hint[128];
                        snprintf(hint, sizeof(hint), "place_next %s %d %d", app_id, x, y);
                        shell_send_cmd(hint);
                    }
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
            /* Browsers restore their windows slowly (software GL, many tabs). */
            g_session_restore_deadline = g_now + 45.0;
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
                /* Each live window serves one saved record, otherwise every
                 * window of an app (all Firefox/Chrome windows) is dragged to
                 * the last saved position. */
                int claimed = 0;
                for (int q = 0; q < g_restore_window_count; q++)
                    if (q != r && g_restore_windows[q].applied && g_restore_windows[q].win_id == g_wins[i].id)
                        { claimed = 1; break; }
                if (claimed) continue;
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
                rw->win_id = wid;
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

static int* shell_dialog_max_flag(int kind) {
    if (kind == SHELL_DLG_SETTINGS) return &g_settings_sheet_max;
    if (kind == SHELL_DLG_ABOUT) return &g_about_sheet_max;
    return NULL;
}

static int shell_dialog_box_idx(int kind) {
    if (kind < 0 || kind >= SHELL_DLG_COUNT) return -1;
    return luna_get_element_by_id(g_shell_dialogs[kind].box_id);
}

static int shell_dialog_root_idx(int kind) {
    if (kind < 0 || kind >= SHELL_DLG_COUNT) return -1;
    return luna_get_element_by_id(g_shell_dialogs[kind].root_id);
}

static int shell_dialog_kind_by_name(const char* name) {
    if (!name || !name[0]) return -1;
    for (int k = 0; k < SHELL_DLG_COUNT; k++)
        if (!strcmp(g_shell_dialogs[k].name, name)) return k;
    return -1;
}

static int shell_dialog_kind_from_hit(int hit) {
    for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx) {
        const char* id = luna_element_at(p)->id;
        if (!id || !id[0]) continue;
        for (int k = 0; k < SHELL_DLG_COUNT; k++) {
            int root = shell_dialog_root_idx(k);
            if (root < 0 || !is_shown(root)) continue;
            const ShellDialogChrome* d = &g_shell_dialogs[k];
            if (!strcmp(id, d->root_id) || !strcmp(id, d->box_id) ||
                !strcmp(id, d->drag_id))
                return k;
        }
    }
    return -1;
}

static int shell_dialog_hit_is_control(int kind, int hit) {
    const ShellDialogChrome* d = &g_shell_dialogs[kind];
    const char* local[3] = { d->close_id, d->min_id, d->max_id };
    for (int i = 0; i < 3; i++) {
        if (!local[i] || !local[i][0]) continue;
        int idx = luna_get_element_by_id(local[i]);
        if (idx >= 0 && hit_inside(hit, idx)) return 1;
    }
    return 0;
}

static int shell_dialog_hit_is_titlebar(int kind, int hit, double x, double y) {
    if (kind < 0 || kind >= SHELL_DLG_COUNT || hit < 0) return 0;
    if (shell_dialog_hit_is_control(kind, hit)) return 0;

    const ShellDialogChrome* d = &g_shell_dialogs[kind];
    int drag = luna_get_element_by_id(d->drag_id);
    if (drag >= 0 && hit_inside(hit, drag)) return 1;

    /* Any hit inside the titleband of a shown dialog counts as chrome —
     * including a title label layered above/below the drag handle — as long
     * as it is not a traffic-light control. */
    int box = shell_dialog_box_idx(kind);
    LunaElement* be = box >= 0 ? luna_element_at(box) : NULL;
    if (!be || be->w <= 0.0f || be->h <= 0.0f) return 0;
    if (!hit_inside(hit, box)) return 0;
    float th = 52.0f;
    if (drag >= 0) {
        LunaElement* de = luna_element_at(drag);
        if (de && de->h > 1.0f) th = de->h;
    }
    return x >= be->x && x <= be->x + be->w &&
           y >= be->y && y <= be->y + th;
}

static void shell_dialog_close(int kind) {
    switch (kind) {
        case SHELL_DLG_SETTINGS:   on_settings_close(NULL); break;
        case SHELL_DLG_ABOUT:      on_about_close(NULL); break;
        case SHELL_DLG_CONFIRM:    on_confirm_cancel(NULL); break;
        case SHELL_DLG_NET_DETAIL: net_detail_close(NULL); break;
        default: break;
    }
}

static void shell_dialog_minimize(int kind) {
    /* Yellow traffic light: hide the sheet (same as each dialog's min handler). */
    shell_dialog_close(kind);
}

static void shell_dialog_maximize(int kind) {
    int box = shell_dialog_box_idx(kind);
    int* flag = shell_dialog_max_flag(kind);
    if (box < 0 || !flag) return;
    if (!(g_shell_dialogs[kind].flags & SHELL_DLG_F_MAXIMIZE)) return;
    sheet_toggle_maximize(box, flag);
}

static void shell_dialog_center(int kind) {
    int box = shell_dialog_box_idx(kind);
    if (box < 0) box = shell_dialog_root_idx(kind);
    if (box < 0) return;
    int* flag = shell_dialog_max_flag(kind);
    if (flag && *flag) {
        *flag = 0;
        LunaElement* e = luna_element_at(box);
        if (e) {
            e->has_css_width = 0;
            e->has_css_height = 0;
        }
    }
    center_element(box);
}


static void win_menu_embed_in(int host_idx) {
    if (g_win_menu_idx < 0 || host_idx < 0) return;
    LunaElement* menu = luna_element_at(g_win_menu_idx);
    if (!menu) return;
    if (g_win_menu_embedded && menu->parent_idx == host_idx) {
        menu->z_override_valid = 1;
        menu->z_override = 9000;
        menu->z_index = 9000;
        return;
    }
    win_menu_detach_embed();
    g_win_menu_saved_parent = menu->parent_idx;
    menu->parent_idx = host_idx;
    menu->z_override_valid = 1;
    menu->z_override = 9000;
    menu->z_index = 9000;
    g_win_menu_embedded = 1;
    luna_mark_layout_dirty();
}

/* Like position_menu_at, but when the menu is embedded under a dialog box the
 * click is in document space and must be converted to box-local coordinates. */
static void position_menu_at_for_host(int menu_idx, int host_idx, float x, float y) {
    if (menu_idx < 0) return;
    float lx = x, ly = y;
    float host_w = luna_window_width;
    float host_h = luna_window_height;
    LunaElement* host = host_idx >= 0 ? luna_element_at(host_idx) : NULL;
    if (host && host->w > 1.0f && host->h > 1.0f) {
        lx = x - host->x;
        ly = y - host->y;
        host_w = host->w;
        host_h = host->h;
    }
    LunaElement* m = luna_element_at(menu_idx);
    float mw = m->w > 1.0f ? m->w : (m->css_width > 1.0f ? m->css_width : 200.0f);
    float mh = m->h > 1.0f ? m->h : (m->css_height > 1.0f ? m->css_height : 160.0f);
    if (lx + mw > host_w - 8.0f) lx = host_w - mw - 8.0f;
    if (ly + mh > host_h - 8.0f) ly = host_h - mh - 8.0f;
    if (lx < 6.0f) lx = 6.0f;
    if (ly < 6.0f) ly = 6.0f;
    {
        float avail = host_h - ly - 8.0f;
        if (avail > 48.0f && mh > avail) {
            m->has_max_height = 1;
            m->css_max_height = floorf(avail);
            m->max_height_pct = 0;
            m->raw_max_height = m->css_max_height;
            m->raw_max_height_off = 0.0f;
        }
    }
    m->rel_x = floorf(lx);
    m->rel_y = floorf(ly);
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

static void win_menu_set_dialog_maximize_label(int kind) {
    int* flag = shell_dialog_max_flag(kind);
    const char* label = (flag && *flag) ? "Restore" : "Maximize";
    if (g_wm_maximize_label_idx >= 0) luna_set_text(g_wm_maximize_label_idx, label);
    if (g_wm_fullscreen_label_idx >= 0) luna_set_text(g_wm_fullscreen_label_idx, "Fullscreen");
}

static void shell_dialog_menu_open(int kind, float x, float y) {
    if (kind < 0 || kind >= SHELL_DLG_COUNT || g_win_menu_idx < 0) return;
    const ShellDialogChrome* d = &g_shell_dialogs[kind];
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_clip_menu();
    g_win_menu_target = 0;
    g_win_menu_dialog = kind;
    win_menu_set_dialog_maximize_label(kind);
    const char* title = "Window";
    if (d->title_id) {
        int t = luna_get_element_by_id(d->title_id);
        LunaElement* te = t >= 0 ? luna_element_at(t) : NULL;
        if (te && te->text[0]) title = te->text;
    }
    int mt = luna_get_element_by_id("win_menu_title");
    if (mt >= 0) luna_set_text(mt, title);
    /* Embed under the dialog box so the menu shares that surface and cannot
     * fall behind a luna.dialog.* / modal sheet layer. */
    int box = shell_dialog_box_idx(kind);
    win_menu_embed_in(box >= 0 ? box : shell_dialog_root_idx(kind));
    set_hidden(g_win_menu_idx, 0);
    position_menu_at_for_host(g_win_menu_idx, box, x, y);
}

static int on_win_menu_dialog_action(const char* id) {
    int kind = g_win_menu_dialog;
    if (kind < 0 || !id) return 0;
    dismiss_win_menu();
    if (!strcmp(id, "wm_activate"))
        return 1; /* already front-most among shell chrome */
    if (!strcmp(id, "wm_minimize")) {
        shell_dialog_minimize(kind);
        return 1;
    }
    if (!strcmp(id, "wm_maximize") || !strcmp(id, "wm_fullscreen")) {
        shell_dialog_maximize(kind);
        return 1;
    }
    if (!strcmp(id, "wm_tile_left") || !strcmp(id, "wm_tile_right"))
        return 1; /* no tiling for modeless sheets */
    if (!strcmp(id, "wm_center")) {
        shell_dialog_center(kind);
        return 1;
    }
    if (!strcmp(id, "wm_close")) {
        shell_dialog_close(kind);
        return 1;
    }
    return 0;
}

static void win_menu_open(uint64_t wid, int anchor_idx, const char* title) {
    if (!wid || g_win_menu_idx < 0) return;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_clip_menu();
    win_menu_detach_embed();
    g_win_menu_dialog = -1;
    g_win_menu_target = wid;
    LunaWinEntry* w = NULL;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].id == wid) { w = &g_wins[i]; break; }
    }
    win_menu_set_maximize_label(w);
    int t = luna_get_element_by_id("win_menu_title");
    if (t >= 0) luna_set_text(t, title && title[0] ? title : "Window");
    set_hidden(g_win_menu_idx, 0);
    position_menu_near(g_win_menu_idx, anchor_idx, 200.0f);
}

static void on_win_menu_action(LunaElement* e) {
    const char* id = NULL;
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* cand = luna_element_at(idx)->id;
        if (cand[0] == 'w' && cand[1] == 'm' && cand[2] == '_') { id = cand; break; }
    }
    if (g_win_menu_dialog >= 0) {
        on_win_menu_dialog_action(id);
        return;
    }
    uint64_t wid = g_win_menu_target;
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
        luna_consume_pointer_event();
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

#define CLIP_MENU_SLOTS 10
static int g_clip_page;   /* 0 = newest 10; each page below holds the next 10 */
static int g_clip_has_more;

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

/* Live list from luna-clipboard's in-memory history.  File writes can fail;
 * the manager process still holds the entries. */
static FILE* clip_open_live_list(void) {
    char path[512];
    clip_cmd_sock_path(path, sizeof(path));
    if (!path[0]) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t splen = strlen(path);
    if (splen >= sizeof(addr.sun_path)) { close(fd); return NULL; }
    memcpy(addr.sun_path, path, splen + 1);
    int cr = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (cr != 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    if (poll(&pfd, 1, 200) <= 0) { close(fd); return NULL; }
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
        close(fd);
        return NULL;
    }
    if (fl >= 0) fcntl(fd, F_SETFL, fl);
    if (send(fd, "list\n", 5, 0) != 5) { close(fd); return NULL; }
    shutdown(fd, SHUT_WR);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 200) <= 0) { close(fd); return NULL; }
    FILE* f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}

static int clip_fill_slots_from_file(FILE* f) {
    int count = 0, k = 0;
    const int first = g_clip_page * CLIP_MENU_SLOTS;
    g_clip_has_more = 0;
    for (;;) {
        char lenbuf[64], mime[256];
        if (!fgets(lenbuf, sizeof(lenbuf), f)) break;
        size_t len = (size_t)strtoul(lenbuf, NULL, 10);
        if (!fgets(mime, sizeof(mime), f)) break;
        if (len > 2 * 1024 * 1024) break;
        static char data[2 * 1024 * 1024 + 1];
        if (fread(data, 1, len, f) != len) break;
        data[len] = 0;
        int c = fgetc(f);
        (void)c;
        int entry = k++;
        if (entry < first) continue;               /* an earlier page */
        if (count >= CLIP_MENU_SLOTS) { g_clip_has_more = 1; break; }

        char id[32];
        snprintf(id, sizeof(id), "clip_%d", count);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) {
            set_hidden(idx, 0);
            char preview[80], num[8];
            clip_preview_text(data, len, preview, sizeof(preview));
            snprintf(num, sizeof(num), "%d", entry + 1);
            for (int i = 0; i < luna_element_count(); i++) {
                LunaElement* el = luna_element_at(i);
                if (el->parent_idx != idx) continue;
                if (strstr(el->class_name, "clip_preview")) luna_set_text(i, preview);
                else if (strstr(el->class_name, "mi_sym")) luna_set_text(i, num);
            }
        }
        count++;
    }
    return count;
}

static int clip_populate_menu(void) {
    int empty_idx = luna_get_element_by_id("clip_empty");
    for (int s = 0; s < CLIP_MENU_SLOTS; s++) {
        char id[32];
        snprintf(id, sizeof(id), "clip_%d", s);
        int idx = luna_get_element_by_id(id);
        if (idx >= 0) set_hidden(idx, 1);
    }

    int count = 0;
    FILE* f = clip_open_live_list();
    if (f) {
        count = clip_fill_slots_from_file(f);
        fclose(f);
    }
    if (count == 0) {
        char path[576];
        clip_history_path(path, sizeof(path));
        f = path[0] ? fopen(path, "rb") : NULL;
        if (f) {
            count = clip_fill_slots_from_file(f);
            fclose(f);
        }
    }
    if (empty_idx >= 0) set_hidden(empty_idx, count == 0 && g_clip_page == 0 ? 0 : 1);
    /* Older entries live one level down: "Older" pages forward, "Newer" back. */
    int more = luna_get_element_by_id("clip_more"), back = luna_get_element_by_id("clip_back");
    if (more >= 0) set_hidden(more, g_clip_has_more ? 0 : 1);
    if (back >= 0) set_hidden(back, g_clip_page > 0 ? 0 : 1);
    return count;
}

static void on_clipboard_menu(LunaElement* e) {
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    if (is_shown(g_clip_menu_idx)) { dismiss_clip_menu(); return; }
    g_clip_page = 0;
    clip_populate_menu();
    set_hidden(g_clip_menu_idx, 0);
    position_menu_near(g_clip_menu_idx,
                       menu_anchor_from(e, g_mb_clip_idx),
                       luna_window_width - 300.0f);
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
    snprintf(cmd, sizeof(cmd), "select %d", g_clip_page * CLIP_MENU_SLOTS + slot);
    clip_send_cmd(cmd);
    toast_show("Clipboard", "Selection restored", 2.0);
}

static void on_clip_page(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!strcmp(id, "clip_more")) { g_clip_page++; break; }
        if (!strcmp(id, "clip_back")) { if (g_clip_page > 0) g_clip_page--; break; }
    }
    clip_populate_menu();
    luna_mark_layout_dirty();
}

static void on_clip_clear(LunaElement* e) {
    (void)e;
    dismiss_clip_menu();
    clip_send_cmd("clear");
    toast_show("Clipboard", "History cleared", 2.0);
}



/* Windows of one app (same app_id), newest-first order is the compositor's. */
static int tray_collect_windows(const char* app_id, uint64_t* ids, int cap) {
    int n = 0;
    for (int i = 0; i < g_win_count && n < cap; i++)
        if (!strcmp(g_wins[i].app_id, app_id)) ids[n++] = g_wins[i].id;
    return n;
}

static int tray_menu_open_for_app(const char* app_id, int anchor_idx) {
    if (g_tray_menu_idx < 0 || !app_id || !*app_id) return 0;
    uint64_t ids[TRAY_MENU_SLOTS];
    int n = tray_collect_windows(app_id, ids, TRAY_MENU_SLOTS);
    if (n < 2) return 0;                 /* one window: the plain window menu is enough */
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    for (int i = 0; i < TRAY_MENU_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "tw_%d", i);
        int idx = luna_get_element_by_id(id);
        if (idx < 0) continue;
        if (i >= n) { set_hidden(idx, 1); g_tray_menu_ids[i] = 0; continue; }
        g_tray_menu_ids[i] = ids[i];
        const char* title = "";
        int minimized = 0;
        for (int w = 0; w < g_win_count; w++)
            if (g_wins[w].id == ids[i]) { title = g_wins[w].title; minimized = g_wins[w].minimized; break; }
        char label[128];
        snprintf(label, sizeof(label), "%s%.100s", minimized ? "– " : "", title[0] ? title : app_id);
        for (int c = 0; c < luna_element_count(); c++) {
            LunaElement* el = luna_element_at(c);
            if (el->parent_idx == idx && strstr(el->class_name, "mi_label")) { luna_set_text(c, label); break; }
        }
        set_hidden(idx, 0);
    }
    static const char* const mv_ids[] = { "tw_mv_l", "tw_mv_r", "tw_mv_x" };
    for (int i = 0; i < 3; i++) set_hidden(luna_get_element_by_id(mv_ids[i]), 1);
    int head = luna_get_element_by_id("tray_menu_title");
    if (head >= 0) luna_set_text(head, app_id);
    luna_mark_layout_dirty();
    set_hidden(g_tray_menu_idx, 0);
    position_menu_near(g_tray_menu_idx, anchor_idx, luna_window_width - 320.0f);
    return 1;
}

/* ── Icon order menu ──
 * Right-click a tray icon or a status item on the right of the menubar to
 * move it left / right.  Tray icons are ranked by key (settings order_tray),
 * the fixed status items through flex `order` (settings order_mb). */
#define MB_ORDER_N 6
static const char* const g_mb_order_ids[MB_ORDER_N] = {
    "mb_graph", "mb_clip", "mb_wifi", "mb_bat", "mb_cc", "mb_weather"
};
static char g_order_key[64];
static int  g_order_is_tray;

static int mb_order_sorted(int* ids) {
    TrayCand c[MB_ORDER_N];
    for (int i = 0; i < MB_ORDER_N; i++) {
        c[i].ref = i; c[i].kind = 0;
        c[i].rank = order_rank(g_settings.order_mb, g_mb_order_ids[i], 1000 + i);
    }
    tray_cand_sort(c, MB_ORDER_N);
    for (int i = 0; i < MB_ORDER_N; i++) ids[i] = c[i].ref;
    return MB_ORDER_N;
}

static void mb_order_apply(void) {
    int ids[MB_ORDER_N];
    mb_order_sorted(ids);
    for (int p = 0; p < MB_ORDER_N; p++)
        luna_set_flex_order(luna_get_element_by_id(g_mb_order_ids[ids[p]]), p + 1);
    luna_set_flex_order(luna_get_element_by_id("mb_clock"), 100);
    luna_mark_layout_dirty();
    shell_request_repaint(1);
}

static int mb_order_known(const char* id) {
    for (int i = 0; i < MB_ORDER_N; i++) if (!strcmp(id, g_mb_order_ids[i])) return 1;
    return 0;
}

static int mb_item_visible(const char* id) {
    int i = luna_get_element_by_id(id);
    return i >= 0 && !strstr(luna_element_at(i)->class_name, "hidden");
}

/* Swap `key` with its visible neighbour in `keys` (dir -1 left, +1 right). */
static int order_swap(char keys[][64], int n, const char* key, int dir, int (*visible)(const char*)) {
    int at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(keys[i], key)) { at = i; break; }
    if (at < 0) return 0;
    for (int j = at + dir; j >= 0 && j < n; j += dir) {
        if (visible && !visible(keys[j])) continue;
        char t[64];
        memcpy(t, keys[at], 64); memcpy(keys[at], keys[j], 64); memcpy(keys[j], t, 64);
        return 1;
    }
    return 0;
}

static void icon_move(int is_tray, const char* key, int dir) {
    char keys[MAX_TRAY_SLOTS + MB_ORDER_N + 24][64];
    int n = 0;
    if (is_tray) {
        for (int s = 0; s < MAX_TRAY_SLOTS; s++)
            if (g_tray_slot_key[s][0] && !tray_key_is_window(g_tray_slot_key[s]))
                snprintf(keys[n++], 64, "%s", g_tray_slot_key[s]);
        if (!order_swap(keys, n, key, dir, NULL)) return;
        /* Keep remembered keys of icons that are not shown right now. */
        for (const char* p = g_settings.order_tray; *p; ) {
            const char* e = strchr(p, '|');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            int known = 0;
            for (int i = 0; i < n; i++) if (strlen(keys[i]) == len && !strncmp(keys[i], p, len)) known = 1;
            if (!known && len < 64 && n < (int)(sizeof(keys) / sizeof(keys[0]))) {
                memcpy(keys[n], p, len); keys[n][len] = 0; n++;
            }
            if (!e) break;
            p = e + 1;
        }
        order_store(g_settings.order_tray, sizeof(g_settings.order_tray), keys, n);
        update_tray_ui();
    } else {
        int ids[MB_ORDER_N];
        mb_order_sorted(ids);
        for (int i = 0; i < MB_ORDER_N; i++) snprintf(keys[i], 64, "%s", g_mb_order_ids[ids[i]]);
        n = MB_ORDER_N;
        if (!order_swap(keys, n, key, dir, mb_item_visible)) return;
        order_store(g_settings.order_mb, sizeof(g_settings.order_mb), keys, n);
        mb_order_apply();
    }
    settings_save();
}

static void order_menu_open(int is_tray, const char* key, int anchor_idx) {
    if (g_tray_menu_idx < 0 || !key || !*key) return;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    g_order_is_tray = is_tray;
    snprintf(g_order_key, sizeof(g_order_key), "%s", key);
    for (int i = 0; i < TRAY_MENU_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "tw_%d", i);
        set_hidden(luna_get_element_by_id(id), 1);
    }
    static const char* const mv[] = { "tw_mv_l", "tw_mv_r", "tw_mv_x" };
    for (int i = 0; i < 3; i++) set_hidden(luna_get_element_by_id(mv[i]), 0);
    int head = luna_get_element_by_id("tray_menu_title");
    if (head >= 0) luna_set_text(head, "Icon position");
    luna_mark_layout_dirty();
    set_hidden(g_tray_menu_idx, 0);
    position_menu_near(g_tray_menu_idx, anchor_idx, luna_window_width - 240.0f);
}

static void on_tray_menu_pick(LunaElement* e) {
    int slot = -1;
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (!strncmp(id, "tw_wg_", 6)) { char k[24]; snprintf(k, sizeof(k), "%s", id); dismiss_tray_menu(); widget_menu_pick(k); return; }
        if (!strncmp(id, "tw_mv_", 6)) {
            char key[64];
            snprintf(key, sizeof(key), "%s", g_order_key);
            dismiss_tray_menu();
            if (id[6] == 'x') {
                if (g_order_is_tray) g_settings.order_tray[0] = 0;
                else g_settings.order_mb[0] = 0;
                if (g_order_is_tray) update_tray_ui(); else mb_order_apply();
                settings_save();
            } else {
                icon_move(g_order_is_tray, key, id[6] == 'l' ? -1 : 1);
            }
            return;
        }
        if (id[0] == 't' && id[1] == 'w' && id[2] == '_') { slot = atoi(id + 3); break; }
    }
    uint64_t wid = slot >= 0 && slot < TRAY_MENU_SLOTS ? g_tray_menu_ids[slot] : 0;
    dismiss_tray_menu();
    if (!wid) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "activate %" PRIu64, wid);
    shell_send_cmd(cmd);
}

/* Click on the application name at the top left: window actions for the
 * focused window (minimize, maximize, tile, close, ...).  Without a focused
 * window it falls back to the newest visible one, then to the desktop menu,
 * so the label is never a dead click. */
static void on_mb_app_click(LunaElement* e) {
    (void)e;
    const LunaWinEntry* pick = NULL;
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].focused && !g_wins[i].minimized) { pick = &g_wins[i]; break; }
    for (int i = g_win_count - 1; !pick && i >= 0; i--)
        if (!g_wins[i].minimized) pick = &g_wins[i];
    if (pick) {
        win_menu_open(pick->id, g_mb_app_idx, pick->title);
    } else if (g_mb_app_idx >= 0) {
        LunaElement* a = luna_element_at(g_mb_app_idx);
        desk_menu_open(a->x, a->y + a->h + 4.0f);
    }
    luna_consume_pointer_event();
    shell_request_repaint(-1);
}

static void on_tray_click(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0] == 't' && id[1] == 'r' && id[2] == 'a' && id[3] == 'y' && id[4] == '_') {
            int slot = atoi(id + 5);
            if (slot < 0 || slot >= MAX_TRAY_SLOTS) return;
            LunaElement* slot_el = luna_element_at(idx);
            int ax = slot_el ? (int)slot_el->x : 0;
            int ay = slot_el ? (int)slot_el->y : 0;
            if (g_tray_sni_kind[slot] &&
                !tray_key_is_window(g_tray_slot_key[slot])) {
                luna_sni_request_activate(g_tray_sni_service[slot],
                                          g_tray_sni_path[slot], ax, ay);
                return;
            }

            const char* key = g_tray_slot_key[slot];
            LunaTrayEntry* t = tray_entry_for_key(key);
            uint64_t sid = tray_lookup_surface(key);
            if (sid) {
                LunaWinEntry* w = NULL;
                for (int i = 0; i < g_win_count; i++) {
                    if (g_wins[i].id == sid) { w = &g_wins[i]; break; }
                }
                if (luna_last_click_button() == LUNA_MOUSE_BUTTON_RIGHT) {
                    /* Several windows of one app share this icon: choose which. */
                    if (!strncmp(key, "app:", 4) && tray_menu_open_for_app(key + 4, idx)) {
                        luna_consume_pointer_event();
                        return;
                    }
                    win_menu_open(sid, idx,
                                  w ? w->title :
                                  (t ? (t->tooltip[0] ? t->tooltip : t->label) : NULL));
                    luna_consume_pointer_event();
                    return;
                }
                if (!strncmp(key, "app:", 4) && tray_menu_open_for_app(key + 4, idx)) {
                    luna_consume_pointer_event();
                    return;
                }
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "activate %" PRIu64, sid);
                shell_send_cmd(cmd);
                return;
            }
            if (t && !strncmp(t->id, "service:", 8)) {
                tray_send_action(t->id, "activate");
                if (!strcmp(t->id, "service:luna-wifi"))
                    on_wifi_menu(e);
                return;
            }
            if (t && t->tooltip[0])
                toast_show(t->label, t->tooltip, 3.0);
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


/* ── Desktop statistics widget: bars or a line graph, on its own timer ───────
 * The monitor thread samples /proc every couple of seconds but deliberately
 * never wakes the UI, and the old path folded CPU/memory into whatever
 * background frame happened to occur, so on a still wallpaper the meters froze.
 * Now a timer (Settings: refresh 2/5/10/30 s or off) pulls the latest sample,
 * keeps a history, and — only while the widget is actually visible — paints one
 * background frame.  The graph is a row of columns whose heights are set in
 * place (no DOM churn): a translucent area under a 2 px top edge. */
#define STATS_HIST 44
#define STATS_GRAPH_H 24.0f
static float  g_hist_cpu[STATS_HIST], g_hist_mem[STATS_HIST], g_hist_temp[STATS_HIST];
static int    g_hist_n;
static int    g_cols_cpu[STATS_HIST], g_cols_mem[STATS_HIST], g_cols_temp[STATS_HIST];
static int    g_cols_n;
static double g_stats_next_at;
static int    g_st_temp_val = -1, g_st_temp_fill = -1, g_st_disk_sub = -1;

static void stats_cols_init(void) {
    g_cols_n = 0;
    g_st_temp_val = luna_get_element_by_id("st_temp_val");
    g_st_temp_fill = luna_get_element_by_id("st_temp_fill");
    g_st_disk_sub = luna_get_element_by_id("st_disk_sub");
    int cpu = luna_get_element_by_id("st_cpu_graph");
    int mem = luna_get_element_by_id("st_mem_graph");
    int tmp = luna_get_element_by_id("st_temp_graph");
    if (cpu < 0 || mem < 0 || tmp < 0) return;
    int a[STATS_HIST], b[STATS_HIST], c[STATS_HIST];
    int na = luna_dom_children(cpu, a, STATS_HIST), nb = luna_dom_children(mem, b, STATS_HIST);
    int nc = luna_dom_children(tmp, c, STATS_HIST);
    int n = na < nb ? na : nb;
    if (nc < n) n = nc;
    for (int i = 0; i < n; i++) { g_cols_cpu[i] = a[i]; g_cols_mem[i] = b[i]; g_cols_temp[i] = c[i]; }
    g_cols_n = n;
}

/* Column heights are set in place against the graph's pixel height. */
static void set_col_pct(int idx, float pct, float full_h) {
    if (idx < 0) return;
    LunaElement* e = luna_element_at(idx);
    float min_r = 2.0f / full_h;
    float r = pct / 100.0f;
    if (r < min_r) r = min_r;
    if (r > 1.0f) r = 1.0f;
    e->pct_h = 1;
    e->raw_h = r;
    e->raw_h_off = 0.0f;
    e->has_css_height = 0;
    e->h = full_h * r;
}

/* CPU temperature as a graph/bar percentage: 30 C is empty, 90 C is full. */
static float temp_pct(int t) {
    float p = ((float)t - 30.0f) * (100.0f / 60.0f);
    return t <= 0 ? 0.0f : p < 0.0f ? 0.0f : p > 100.0f ? 100.0f : p;
}

static void stats_draw_graphs(void) {
    for (int i = 0; i < g_cols_n; i++) {
        /* newest sample on the right; columns before the first sample stay flat */
        int h = i - (g_cols_n - g_hist_n);
        set_col_pct(g_cols_cpu[i], h >= 0 ? g_hist_cpu[h] : 0.0f, STATS_GRAPH_H);
        set_col_pct(g_cols_mem[i], h >= 0 ? g_hist_mem[h] : 0.0f, STATS_GRAPH_H);
        set_col_pct(g_cols_temp[i], h >= 0 ? g_hist_temp[h] : 0.0f, STATS_GRAPH_H);
    }
}

/* Storage barely changes, so it is shown as numbers only: "412 GB" over
 * "of 954 GB · 57% used". */
static void stats_set_storage(const LunaMonitorSnapshot* snap) {
    if (!snap->disk_valid) return;
    double used = snap->disk_used > 99.9f ? 99.9 : snap->disk_used;
    double total = used < 99.0 ? snap->disk_free_gb / (1.0 - used / 100.0) : snap->disk_free_gb;
    char a[32], sub[64];
    if (snap->disk_free_gb >= 1000.0) snprintf(a, sizeof(a), "%.1f TB", snap->disk_free_gb / 1000.0);
    else snprintf(a, sizeof(a), "%.0f GB", snap->disk_free_gb);
    char t[24];
    if (total >= 1000.0) snprintf(t, sizeof(t), "%.1f TB", total / 1000.0);
    else snprintf(t, sizeof(t), "%.0f GB", total);
    snprintf(sub, sizeof(sub), "free / %s", t);
    int idx = g_ui_idx[UI_ST_DISK_VAL];
    if (idx >= 0 && text_would_change(idx, a)) luna_set_text_paint_only(idx, a);
    int sidx = g_st_disk_sub;
    if (sidx >= 0 && text_would_change(sidx, sub)) luna_set_text_paint_only(sidx, sub);
}

static void stats_set_temp(int t) {
    char buf[16];
    if (t > 0) snprintf(buf, sizeof(buf), "%d°C", t); else snprintf(buf, sizeof(buf), "—");
    int idx = g_st_temp_val;
    if (idx >= 0 && text_would_change(idx, buf)) luna_set_text_paint_only(idx, buf);
    set_bar_fill(g_st_temp_fill, temp_pct(t));
}


static void stats_apply_style(void) {
    widgets_sync_layout_classes();
    luna_mark_layout_dirty();
    g_stats_next_at = g_now;          /* refresh immediately in the new style */
    shell_request_repaint(0);
}

static void stats_tick(void) {
    if (!g_settings.widgets_enabled || g_settings.stats_interval <= 0) return;
    if (g_now < g_stats_next_at) return;
    g_stats_next_at = g_now + (double)g_settings.stats_interval;
    LunaMonitorSnapshot snap;
    if (!luna_monitor_peek_status(&snap)) return;
    if (g_hist_n < STATS_HIST) g_hist_n++;
    else {
        memmove(g_hist_cpu, g_hist_cpu + 1, sizeof(float) * (STATS_HIST - 1));
        memmove(g_hist_mem, g_hist_mem + 1, sizeof(float) * (STATS_HIST - 1));
        memmove(g_hist_temp, g_hist_temp + 1, sizeof(float) * (STATS_HIST - 1));
    }
    g_hist_cpu[g_hist_n - 1] = snap.cpu;
    g_hist_mem[g_hist_n - 1] = (float)snap.memory;
    g_hist_temp[g_hist_n - 1] = temp_pct(snap.cpu_temp);
    /* Hidden or covered: keep the history, skip the UI work and the frame. */
    if (!repaint_matters(g_ui_idx[UI_ST_CPU_VAL])) return;
    char buf[32];
    int idx = g_ui_idx[UI_ST_CPU_VAL];
    snprintf(buf, sizeof(buf), "%d%%", (int)snap.cpu);
    if (text_would_change(idx, buf)) luna_set_text_paint_only(idx, buf);
    idx = g_ui_idx[UI_ST_MEM_VAL];
    snprintf(buf, sizeof(buf), "%d%%", snap.memory);
    if (text_would_change(idx, buf)) luna_set_text_paint_only(idx, buf);
    if (g_settings.stats_style == 1) {
        stats_draw_graphs();
        luna_mark_layout_dirty();
    } else {
        set_bar_fill(g_ui_idx[UI_ST_CPU_FILL], snap.cpu);
        set_bar_fill(g_ui_idx[UI_ST_MEM_FILL], (float)snap.memory);
    }
    stats_set_temp(snap.cpu_temp);
    stats_set_storage(&snap);
    shell_request_repaint(0);
}


/* ── Menubar CPU / temperature graph (LXDE-panel style) ────────────────────
 * 14 columns, two translucent gradient layers (temperature behind, CPU in
 * front).  Click cycles CPU+temp → CPU → temp → collapsed icon.  It has its
 * own 2 s timer and only repaints the menubar surface while visible. */
#define MG_COLS 14
#define MG_H    18.0f
#define MG_INTERVAL 2.0
static int    g_mg_idx = -1, g_mg_tip = -1, g_mg_cpu[MG_COLS], g_mg_temp[MG_COLS], g_mg_cols;
static float  g_mg_hc[MG_COLS], g_mg_ht[MG_COLS];
static int    g_mg_n;
static double g_mg_next_at;

static void on_mbgraph_click(LunaElement* e);

static void mbgraph_apply_mode(void) {
    if (g_mg_idx < 0) return;
    int m = g_settings.mb_graph;
    luna_update_classes(g_mg_idx, "mg_only_cpu mg_only_temp mg_off",
                        m == 1 ? "mg_only_cpu" : m == 2 ? "mg_only_temp" : m == 3 ? "mg_off" : NULL);
    g_mg_next_at = g_now;
    luna_mark_layout_dirty();
    shell_request_repaint(1);
}

static void mbgraph_init(void) {
    g_mg_idx = luna_get_element_by_id("mb_graph");
    g_mg_tip = luna_get_element_by_id("mg_tip");
    int c = luna_get_element_by_id("mg_cpu"), t = luna_get_element_by_id("mg_temp");
    g_mg_cols = 0;
    if (g_mg_idx < 0 || c < 0 || t < 0) return;
    int a[MG_COLS], b[MG_COLS];
    int na = luna_dom_children(c, a, MG_COLS), nb = luna_dom_children(t, b, MG_COLS);
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) { g_mg_cpu[i] = a[i]; g_mg_temp[i] = b[i]; }
    g_mg_cols = n;
    wire_subtree(g_mg_idx, on_mbgraph_click);
    mbgraph_apply_mode();
}

static void mbgraph_tick(void) {
    if (g_mg_cols == 0 || g_settings.mb_graph == 3) return;
    if (g_now < g_mg_next_at) return;
    g_mg_next_at = g_now + MG_INTERVAL;
    LunaMonitorSnapshot snap;
    if (!luna_monitor_peek_status(&snap)) return;
    if (g_mg_n < MG_COLS) g_mg_n++;
    else {
        memmove(g_mg_hc, g_mg_hc + 1, sizeof(float) * (MG_COLS - 1));
        memmove(g_mg_ht, g_mg_ht + 1, sizeof(float) * (MG_COLS - 1));
    }
    g_mg_hc[g_mg_n - 1] = snap.cpu;
    g_mg_ht[g_mg_n - 1] = temp_pct(snap.cpu_temp);
    for (int i = 0; i < g_mg_cols; i++) {
        int h = i - (g_mg_cols - g_mg_n);
        set_col_pct(g_mg_cpu[i], h >= 0 ? g_mg_hc[h] : 0.0f, MG_H);
        set_col_pct(g_mg_temp[i], h >= 0 ? g_mg_ht[h] : 0.0f, MG_H);
    }
    if (g_mg_tip >= 0) {
        char tip[64];
        if (snap.cpu_temp > 0) snprintf(tip, sizeof(tip), "CPU %d%%  ·  %d°C", (int)snap.cpu, snap.cpu_temp);
        else snprintf(tip, sizeof(tip), "CPU %d%%", (int)snap.cpu);
        if (text_would_change(g_mg_tip, tip)) luna_set_text_paint_only(g_mg_tip, tip);
    }
    luna_mark_layout_dirty();
    shell_request_repaint(1);
}

static void on_mbgraph_click(LunaElement* e) {
    (void)e;
    g_settings.mb_graph = (g_settings.mb_graph + 1) % 4;
    mbgraph_apply_mode();
    settings_save();
}

/* ── AirPlay receiver widget ─────────────────────────────────────────────────
 * airplay.h runs on its own threads; they only fill g_ap and wake the main loop
 * (shell_async_notify).  airplay_tick() — main thread — turns that into DOM
 * changes.  Mirrored video is piped, as Annex-B H.264, into an ffplay window. */
#ifdef LUNA_WITH_AIRPLAY
static airplay_t*      g_airplay;
static pthread_mutex_t g_ap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int dirty, clients, mirroring, audio;
    char client[64], note[96];
} g_ap;
static pid_t g_ap_player = -1;
static int   g_ap_player_fd = -1;
static int   g_ap_ui_ready, g_ap_toggle = -1, g_ap_status = -1, g_ap_hint = -1, g_ap_card = -1;

static int ap_have_ffplay(void) {
    const char* path = getenv("PATH");
    if (!path) return 0;
    char* copy = strdup(path);
    char* save = NULL;
    int found = 0;
    for (char* d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char f[PATH_MAX];
        snprintf(f, sizeof(f), "%s/ffplay", d);
        found = access(f, X_OK) == 0;
    }
    free(copy);
    return found;
}

static void ap_set_note(const char* note) {
    pthread_mutex_lock(&g_ap_lock);
    snprintf(g_ap.note, sizeof(g_ap.note), "%s", note);
    g_ap.dirty = 1;
    pthread_mutex_unlock(&g_ap_lock);
    shell_async_notify(NULL);
}

static void ap_player_stop(void) {
    pthread_mutex_lock(&g_ap_lock);
    if (g_ap_player_fd >= 0) { close(g_ap_player_fd); g_ap_player_fd = -1; }
    pid_t pid = g_ap_player;
    g_ap_player = -1;
    pthread_mutex_unlock(&g_ap_lock);
    if (pid > 0) {
        kill(pid, SIGTERM);
        int st;
        waitpid(pid, &st, 0);
    }
}

/* Called with the lock held; returns the pipe fd or -1. */
static int ap_player_start_locked(void) {
    if (g_ap_player_fd >= 0) {
        int st;
        if (g_ap_player > 0 && waitpid(g_ap_player, &st, WNOHANG) == g_ap_player) {   /* the user closed the window */
            close(g_ap_player_fd); g_ap_player_fd = -1; g_ap_player = -1;
        } else return g_ap_player_fd;
    }
    int p[2];
    if (pipe(p) != 0) return -1;
    char title[128];
    snprintf(title, sizeof(title), "AirPlay – %s", g_ap.client[0] ? g_ap.client : "Screen Mirroring");
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[0], 0);
        close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("ffplay", "ffplay", "-loglevel", "quiet", "-fflags", "nobuffer", "-flags", "low_delay",
               "-framedrop", "-probesize", "32", "-analyzeduration", "0", "-window_title", title,
               "-f", "h264", "-i", "-", (char*)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) { close(p[1]); return -1; }
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    g_ap_player = pid; g_ap_player_fd = p[1];
    return p[1];
}

/* ── audio sink ──────────────────────────────────────────────────────────────
 * ALAC arrives decoded (PCM) and goes to the first available player that takes raw
 * s16le; AAC-LC/ELD arrive as raw frames, are wrapped as LATM/LOAS and played by ffplay
 * (AAC-ELD needs ffmpeg >= 4).  LUNA_AIRPLAY_AUDIO_CMD overrides the player ("sh -c"). */
static pid_t g_ap_aud = -1;
static int   g_ap_aud_fd = -1, g_ap_aud_kind;           /* kind: 1 = PCM, 2 = LATM */
static int ap_which(const char* prog) {
    const char* path = getenv("PATH");
    if (!path) return 0;
    char* copy = strdup(path);
    char* save = NULL;
    int found = 0;
    for (char* d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char f[PATH_MAX];
        snprintf(f, sizeof(f), "%s/%s", d, prog);
        found = access(f, X_OK) == 0;
    }
    free(copy);
    return found;
}
static void ap_audio_stop(void) {
    pthread_mutex_lock(&g_ap_lock);
    if (g_ap_aud_fd >= 0) { close(g_ap_aud_fd); g_ap_aud_fd = -1; }
    pid_t pid = g_ap_aud;
    g_ap_aud = -1; g_ap_aud_kind = 0;
    pthread_mutex_unlock(&g_ap_lock);
    if (pid > 0) { kill(pid, SIGTERM); int st; waitpid(pid, &st, 0); }
}
/* Called with the lock held. */
static int ap_audio_start_locked(int kind) {
    if (g_ap_aud_fd >= 0 && g_ap_aud_kind == kind) {
        int st;
        if (g_ap_aud > 0 && waitpid(g_ap_aud, &st, WNOHANG) == g_ap_aud) { close(g_ap_aud_fd); g_ap_aud_fd = -1; g_ap_aud = -1; }
        else return g_ap_aud_fd;
    }
    if (g_ap_aud_fd >= 0) return g_ap_aud_fd;              /* another kind is playing: keep it */
    int p[2];
    if (pipe(p) != 0) return -1;
    const char* custom = getenv("LUNA_AIRPLAY_AUDIO_CMD");
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[0], 0);
        close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        if (custom && *custom) execl("/bin/sh", "sh", "-c", custom, (char*)NULL);
        if (kind == 1) {
            if (ap_which("paplay")) execlp("paplay", "paplay", "--raw", "--rate=44100", "--format=s16le", "--channels=2", "--latency-msec=60", (char*)NULL);
            if (ap_which("pw-cat")) execlp("pw-cat", "pw-cat", "-p", "--format", "s16", "--rate", "44100", "--channels", "2", "-", (char*)NULL);
            if (ap_which("aplay")) execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "2", "--buffer-time=80000", "-", (char*)NULL);
            execlp("ffplay", "ffplay", "-nodisp", "-loglevel", "quiet", "-fflags", "nobuffer", "-f", "s16le",
                   "-sample_rate", "44100", "-channels", "2", "-i", "-", (char*)NULL);
        } else {
            execlp("ffplay", "ffplay", "-nodisp", "-loglevel", "quiet", "-fflags", "nobuffer", "-probesize", "32",
                   "-analyzeduration", "0", "-f", "latm", "-i", "-", (char*)NULL);
        }
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) { close(p[1]); return -1; }
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    g_ap_aud = pid; g_ap_aud_fd = p[1]; g_ap_aud_kind = kind;
    return p[1];
}
static void ap_write_all(int fd, const uint8_t* d, size_t n) {
    size_t off = 0;
    while (off < n) { ssize_t w = write(fd, d + off, n - off); if (w <= 0) break; off += (size_t)w; }
}
static void ap_on_pcm(void* u, const int16_t* samples, size_t frames, int channels, int rate, uint64_t ts) {
    (void)u; (void)rate; (void)ts;
    pthread_mutex_lock(&g_ap_lock);
    int fd = ap_audio_start_locked(1);
    int first = !g_ap.audio;
    g_ap.audio = 1;
    if (first) g_ap.dirty = 1;
    pthread_mutex_unlock(&g_ap_lock);
    if (first) shell_async_notify(NULL);
    if (fd >= 0) ap_write_all(fd, (const uint8_t*)samples, frames * (size_t)channels * 2);
}
static void ap_on_audio_ex(void* u, const uint8_t* frame, size_t len, int codec, uint32_t rtp, uint64_t ts) {
    (void)u; (void)rtp; (void)ts;
    static const uint8_t asc_lc[2] = { 0x12, 0x10 }, asc_eld[4] = { 0xf8, 0xe8, 0x50, 0x00 };
    uint8_t out[1400];
    size_t w = codec == 8 ? airplay_loas_wrap(asc_eld, 28, frame, len, out, sizeof(out))
             : codec == 4 ? airplay_loas_wrap(asc_lc, 16, frame, len, out, sizeof(out)) : 0;
    if (!w) return;
    pthread_mutex_lock(&g_ap_lock);
    int fd = ap_audio_start_locked(2);
    int first = !g_ap.audio;
    g_ap.audio = 1;
    if (first) g_ap.dirty = 1;
    pthread_mutex_unlock(&g_ap_lock);
    if (first) shell_async_notify(NULL);
    if (fd >= 0) ap_write_all(fd, out, w);
}

static void ap_on_video(void* u, const uint8_t* d, size_t n, uint64_t ts, int idr) {
    (void)u; (void)ts; (void)idr;
    pthread_mutex_lock(&g_ap_lock);
    int fd = ap_player_start_locked();
    int first = !g_ap.mirroring;
    g_ap.mirroring = 1;
    if (first) g_ap.dirty = 1;
    pthread_mutex_unlock(&g_ap_lock);
    if (first) shell_async_notify(NULL);
    if (fd >= 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, d + off, n - off);
            if (w <= 0) break;                                   /* window closed: next frame restarts it */
            off += (size_t)w;
        }
    }
}
static void ap_on_client(void* u, int connected, const char* name, const char* addr) {
    (void)u; (void)addr;
    pthread_mutex_lock(&g_ap_lock);
    g_ap.clients += connected ? 1 : -1;
    if (g_ap.clients < 0) g_ap.clients = 0;
    if (connected) snprintf(g_ap.client, sizeof(g_ap.client), "%s", name);
    else if (!g_ap.clients) { g_ap.client[0] = 0; g_ap.mirroring = 0; g_ap.audio = 0; }
    g_ap.dirty = 1;
    pthread_mutex_unlock(&g_ap_lock);
    if (!connected) { ap_player_stop(); ap_audio_stop(); }
    shell_async_notify(NULL);
}
static void ap_on_log(void* u, int level, const char* msg) {
    (void)u;
    fprintf(stderr, "[luna-shell/airplay] %s%s\n", level ? "warning: " : "", msg);
    if (level && strstr(msg, "FairPlay")) ap_set_note("FairPlay unavailable");
}

static void ap_ui_refresh(void) {
    if (g_ap_card < 0) return;
    char status[96], hint[96];
    pthread_mutex_lock(&g_ap_lock);
    int clients = g_ap.clients, mirroring = g_ap.mirroring, audio = g_ap.audio;
    char client[64], note[96];
    snprintf(client, sizeof(client), "%s", g_ap.client);
    snprintf(note, sizeof(note), "%s", g_ap.note);
    g_ap.dirty = 0;
    pthread_mutex_unlock(&g_ap_lock);
    const char* name = g_settings.airplay_name[0] ? g_settings.airplay_name : "Luna Desktop";
    if (!g_airplay) {
        snprintf(status, sizeof(status), "Off");
        snprintf(hint, sizeof(hint), "%s", note[0] ? note : "Receive from iPhone, iPad, Mac.");
    } else if (mirroring) {
        snprintf(status, sizeof(status), "%s · %s", audio ? "Mirroring + sound" : "Mirroring", client);
        snprintf(hint, sizeof(hint), "Close the window: link stays");
    } else if (clients) {
        snprintf(status, sizeof(status), "%s · %s", audio ? "Playing" : "Connected", client);
        snprintf(hint, sizeof(hint), "%s", note[0] ? note : "Waiting for a stream…");
    } else {
        snprintf(status, sizeof(status), "Ready · %s", name);
        snprintf(hint, sizeof(hint), "%s", note[0] ? note : "Screen Mirroring → me");
    }
    if (g_ap_status >= 0 && text_would_change(g_ap_status, status)) luna_set_text(g_ap_status, status);
    if (g_ap_hint >= 0 && text_would_change(g_ap_hint, hint)) luna_set_text(g_ap_hint, hint);
    luna_update_classes(g_ap_toggle, "on", g_airplay ? "on" : NULL);
    luna_update_classes(g_ap_card, "on live", mirroring ? "live" : g_airplay ? "on" : NULL);
    shell_request_repaint(0);
}

static void ap_stop(void) {
    if (g_airplay) { airplay_destroy(g_airplay); g_airplay = NULL; }
    ap_player_stop();
    ap_audio_stop();
    pthread_mutex_lock(&g_ap_lock);
    g_ap.clients = g_ap.mirroring = g_ap.audio = 0;
    g_ap.client[0] = 0;
    pthread_mutex_unlock(&g_ap_lock);
}

static void ap_start(void) {
    if (g_airplay) return;
    signal(SIGPIPE, SIG_IGN);                                    /* a closed player window must not kill the shell */
    airplay_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_log = ap_on_log; cb.on_client = ap_on_client; cb.on_video = ap_on_video;
    cb.on_pcm = ap_on_pcm; cb.on_audio_ex = ap_on_audio_ex;
    airplay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = g_settings.airplay_name[0] ? g_settings.airplay_name : "Luna Desktop";
    cfg.width = (int)luna_window_width; cfg.height = (int)luna_window_height; cfg.fps = 30;
    cfg.latency_ms = g_settings.airplay_latency; cfg.av_offset_ms = g_settings.airplay_av_offset;
    /* A persistent identity makes senders remember the pairing across restarts. */
    uint8_t seed[32];
    char path[PATH_MAX];
    int have = 0;
    if (g_xdg.config_home[0] && snprintf(path, sizeof(path), "%s/luna-shell/airplay.key", g_xdg.config_home) < (int)sizeof(path)) {
        FILE* f = fopen(path, "rb");
        if (f) { have = fread(seed, 1, 32, f) == 32; fclose(f); }
        if (!have) {
            FILE* r = fopen("/dev/urandom", "rb");
            if (r && fread(seed, 1, 32, r) == 32) {
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd >= 0) { have = write(fd, seed, 32) == 32; close(fd); }
            }
            if (r) fclose(r);
        }
    }
    if (have) cfg.ed_seed = seed;
#ifndef LUNA_AIRPLAY_PLAYFAIR
    /* FairPlay runs in a separate GPL helper process (airplay-fairplayd), nothing of it is linked here. */
    airplay_fairplay_ops fpops;
    char ferr[160];
    int fp_ok = airplay_fairplay_spawn(&fpops, NULL, ferr, sizeof(ferr)) == 0;
    if (fp_ok) cfg.fairplay = &fpops;
#else
    int fp_ok = 1;
#endif
    g_airplay = airplay_create(&cfg, &cb);
    if (!g_airplay || airplay_start(g_airplay) != 0) {
        if (g_airplay) { airplay_destroy(g_airplay); g_airplay = NULL; }
        ap_set_note("Could not start the receiver.");
        return;
    }
    if (!fp_ok) ap_set_note("FairPlay helper not found");
    else if (!ap_have_ffplay()) ap_set_note("Install ffmpeg (ffplay) to view the screen.");
    else ap_set_note("");
}

static void on_airplay_toggle(LunaElement* e) {
    (void)e;
    if (g_airplay) { ap_stop(); g_settings.airplay_enabled = 0; }
    else { ap_start(); g_settings.airplay_enabled = g_airplay != NULL; }
    settings_save();
    ap_ui_refresh();
}

static void airplay_init(void) {
    g_ap_card = luna_get_element_by_id("widget_airplay");
    g_ap_toggle = luna_get_element_by_id("ap_toggle");
    g_ap_status = luna_get_element_by_id("ap_status");
    g_ap_hint = luna_get_element_by_id("ap_hint");
    if (g_ap_card < 0) return;
    wire_subtree(g_ap_toggle, on_airplay_toggle);
    g_ap_ui_ready = 1;
    if (g_settings.airplay_enabled) ap_start();
    ap_ui_refresh();
}

static void airplay_tick(void) {
    if (!g_ap_ui_ready) return;
    pthread_mutex_lock(&g_ap_lock);
    int dirty = g_ap.dirty;
    pthread_mutex_unlock(&g_ap_lock);
    if (dirty) ap_ui_refresh();
}
static void airplay_shutdown(void) { ap_stop(); }
#else
static void airplay_init(void) {}
static void airplay_tick(void) {}
static void airplay_shutdown(void) {}
#endif

/* ── AirDrop widget ─────────────────────────────────────────────────────────
 * Same threading rule as the AirPlay card: library threads fill g_ad and wake
 * the main loop, which owns every DOM change.  An incoming request blocks its
 * connection thread until the user answers the confirmation dialog. */
#ifdef LUNA_WITH_AIRDROP
#define AD_UI_PEERS 3
#define AD_KEEP 12
static airdrop_t*      g_airdrop;
static pthread_mutex_t g_ad_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ad_cv = PTHREAD_COND_INITIALIZER;
static struct {
    int dirty, busy, ask_pending, ask_decision, workers;
    char status[96], hint[120], toast_title[48], toast_msg[200], ask_text[260];
    struct { char id[32], name[72], host[80]; int port; } peers[AD_KEEP]; int npeers;
    double next_progress;
} g_ad;
static int g_ad_card = -1, g_ad_toggle = -1, g_ad_status = -1, g_ad_hint = -1, g_ad_peer_el[AD_UI_PEERS] = { -1, -1, -1 };
static int g_ad_ui_ready, g_ad_target = -1, g_ad_ask_shown;

static void ad_note(const char* status, const char* hint) {
    pthread_mutex_lock(&g_ad_lock);
    if (status) snprintf(g_ad.status, sizeof(g_ad.status), "%s", status);
    if (hint) snprintf(g_ad.hint, sizeof(g_ad.hint), "%s", hint);
    g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
}
static void ad_toast(const char* title, const char* msg) {
    pthread_mutex_lock(&g_ad_lock);
    snprintf(g_ad.toast_title, sizeof(g_ad.toast_title), "%s", title);
    snprintf(g_ad.toast_msg, sizeof(g_ad.toast_msg), "%s", msg);
    g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
}
static void ad_worker_begin(void) { pthread_mutex_lock(&g_ad_lock); g_ad.workers++; pthread_mutex_unlock(&g_ad_lock); }
static void ad_worker_end(void) { pthread_mutex_lock(&g_ad_lock); g_ad.workers--; pthread_cond_broadcast(&g_ad_cv); pthread_mutex_unlock(&g_ad_lock); }

static void airdrop_ask_decide(int accept) {
    pthread_mutex_lock(&g_ad_lock);
    if (g_ad.ask_pending) { g_ad.ask_decision = accept ? 1 : 0; pthread_cond_broadcast(&g_ad_cv); }
    pthread_mutex_unlock(&g_ad_lock);
}

/* Connection thread: blocks until the dialog is answered (or 30 s pass). */
static int ad_on_ask(void* u, const airdrop_ask* a) {
    (void)u;
    char what[180] = "";
    if (a->nfiles == 1) snprintf(what, sizeof(what), "“%.100s”", a->files[0].name);
    else if (a->nfiles > 1) snprintf(what, sizeof(what), "%d files, e.g. “%.80s”", a->nfiles, a->files[0].name);
    else if (a->nurls) snprintf(what, sizeof(what), "a link");
    pthread_mutex_lock(&g_ad_lock);
    snprintf(g_ad.ask_text, sizeof(g_ad.ask_text), "%.80s wants to send you %s.", a->sender_name[0] ? a->sender_name : "A device", what);
    g_ad.ask_pending = 1; g_ad.ask_decision = -1; g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 30;
    pthread_mutex_lock(&g_ad_lock);
    while (g_ad.ask_decision == -1) if (pthread_cond_timedwait(&g_ad_cv, &g_ad_lock, &until) != 0) break;
    int ok = g_ad.ask_decision == 1;
    g_ad.ask_pending = 0; g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
    if (ok && a->nurls) { ad_toast("AirDrop", a->urls[0]); }
    return ok;
}
static void ad_on_progress(void* u, const char* name, uint64_t done, int sending) {
    (void)u;
    pthread_mutex_lock(&g_ad_lock);
    int due = g_now >= g_ad.next_progress;
    if (due) {
        g_ad.next_progress = g_now + 0.25;
        snprintf(g_ad.status, sizeof(g_ad.status), "%s · %.60s", sending ? "Sending" : "Receiving", name);
        snprintf(g_ad.hint, sizeof(g_ad.hint), "%.1f MB", (double)done / 1048576.0);
        g_ad.busy = 1; g_ad.dirty = 1;
    }
    pthread_mutex_unlock(&g_ad_lock);
    if (due) shell_async_notify(NULL);
}
static void ad_on_received(void* u, const char* path) {
    (void)u;
    const char* base = strrchr(path, '/');
    ad_toast("AirDrop · received", base ? base + 1 : path);
}
static void ad_on_done(void* u, int ok, int sending) {
    (void)u; (void)ok; (void)sending;
    pthread_mutex_lock(&g_ad_lock);
    g_ad.busy = 0; g_ad.status[0] = 0; g_ad.hint[0] = 0; g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
}
static void ad_on_log(void* u, int level, const char* msg) {
    (void)u;
    fprintf(stderr, "[luna-shell/airdrop] %s%s\n", level ? "warning: " : "", msg);
}

/* A new peer: fetch its display name through /Discover off the mDNS thread. */
typedef struct { char id[32], host[80]; int port; airdrop_t* ad; } AdNameJob;
static void* ad_name_thread(void* arg) {
    AdNameJob* j = (AdNameJob*)arg;
    char name[72] = "";
    if (airdrop_discover(j->ad, j->host, j->port, name, sizeof(name)) != 0) snprintf(name, sizeof(name), "Device %.4s", j->id);
    pthread_mutex_lock(&g_ad_lock);
    for (int i = 0; i < g_ad.npeers; i++)
        if (!strcmp(g_ad.peers[i].id, j->id)) { snprintf(g_ad.peers[i].name, sizeof(g_ad.peers[i].name), "%s", name); g_ad.dirty = 1; }
    pthread_mutex_unlock(&g_ad_lock);
    shell_async_notify(NULL);
    free(j);
    ad_worker_end();
    return NULL;
}
static void ad_on_peer(void* u, int added, const airdrop_peer* p) {
    (void)u;
    pthread_mutex_lock(&g_ad_lock);
    int idx = -1;
    for (int i = 0; i < g_ad.npeers; i++) if (!strcmp(g_ad.peers[i].id, p->id)) idx = i;
    if (!added) {
        if (idx >= 0) { g_ad.peers[idx] = g_ad.peers[--g_ad.npeers]; g_ad.dirty = 1; }
        pthread_mutex_unlock(&g_ad_lock);
        shell_async_notify(NULL);
        return;
    }
    int fresh = idx < 0;
    if (fresh && g_ad.npeers < AD_KEEP) {
        idx = g_ad.npeers++;
        snprintf(g_ad.peers[idx].id, sizeof(g_ad.peers[idx].id), "%s", p->id);
        snprintf(g_ad.peers[idx].name, sizeof(g_ad.peers[idx].name), "Device %.4s", p->id);
    }
    if (idx >= 0) { snprintf(g_ad.peers[idx].host, sizeof(g_ad.peers[idx].host), "%s", p->host); g_ad.peers[idx].port = p->port; g_ad.dirty = 1; }
    g_ad.workers += fresh && idx >= 0 && g_airdrop;
    pthread_mutex_unlock(&g_ad_lock);
    if (fresh && idx >= 0 && g_airdrop) {
        AdNameJob* j = (AdNameJob*)calloc(1, sizeof(*j));
        pthread_t th;
        if (j) {
            snprintf(j->id, sizeof(j->id), "%s", p->id); snprintf(j->host, sizeof(j->host), "%s", p->host); j->port = p->port; j->ad = g_airdrop;
            if (pthread_create(&th, NULL, ad_name_thread, j) == 0) pthread_detach(th);
            else { free(j); ad_worker_end(); }
        } else ad_worker_end();
    }
    shell_async_notify(NULL);
}

static void ad_ui_refresh(void) {
    if (g_ad_card < 0) return;
    char status[96], hint[120];
    pthread_mutex_lock(&g_ad_lock);
    int busy = g_ad.busy, np = g_ad.npeers;
    char names[AD_UI_PEERS][72];
    for (int i = 0; i < AD_UI_PEERS; i++) snprintf(names[i], sizeof(names[i]), "%s", i < np ? g_ad.peers[i].name : "");
    snprintf(status, sizeof(status), "%s", g_ad.status);
    snprintf(hint, sizeof(hint), "%s", g_ad.hint);
    char tt[48], tm[200];
    snprintf(tt, sizeof(tt), "%s", g_ad.toast_title); snprintf(tm, sizeof(tm), "%s", g_ad.toast_msg);
    g_ad.toast_title[0] = g_ad.toast_msg[0] = 0;
    int ask = g_ad.ask_pending;
    if (ask) snprintf(g_ad_ask_text, sizeof(g_ad_ask_text), "%s", g_ad.ask_text);
    g_ad.dirty = 0;
    pthread_mutex_unlock(&g_ad_lock);
    if (!status[0]) {
        if (!g_airdrop) { snprintf(status, sizeof(status), "Off"); snprintf(hint, sizeof(hint), "Share files with nearby devices."); }
        else {
            snprintf(status, sizeof(status), "Ready · %s", g_settings.airplay_name[0] ? g_settings.airplay_name : "Luna Desktop");
            if (np) snprintf(hint, sizeof(hint), "%d nearby · click to send", np);
            else snprintf(hint, sizeof(hint), access("/sys/class/net/awdl0", F_OK) == 0 ? "No devices nearby." : "LAN only (no AWDL)");
        }
    }
    if (g_ad_status >= 0 && text_would_change(g_ad_status, status)) luna_set_text(g_ad_status, status);
    if (g_ad_hint >= 0 && text_would_change(g_ad_hint, hint)) luna_set_text(g_ad_hint, hint);
    luna_update_classes(g_ad_toggle, "on", g_airdrop ? "on" : NULL);
    luna_update_classes(g_ad_card, "on busy", busy ? "busy" : g_airdrop ? "on" : NULL);
    for (int i = 0; i < AD_UI_PEERS; i++) {
        int row = g_ad_peer_el[i];
        if (row < 0) continue;
        set_hidden(row, !(g_airdrop && i < np));
        if (i < np) {
            int kids[2];
            if (luna_dom_children(row, kids, 2) > 0 && text_would_change(kids[0], names[i])) luna_set_text(kids[0], names[i]);
        }
    }
    if (tt[0]) toast_show(tt, tm, 4.0);
    if (ask && !g_ad_ask_shown) { g_ad_ask_shown = 1; confirm_open(ACT_AIRDROP); }
    if (!ask && g_ad_ask_shown) { g_ad_ask_shown = 0; if (g_pending_action == ACT_AIRDROP) { g_pending_action = ACT_NONE; set_hidden(g_confirm_idx, 1); } }
    shell_request_repaint(0);
}

static void ad_stop(void) {
    airdrop_ask_decide(0);
    pthread_mutex_lock(&g_ad_lock);
    airdrop_t* a = g_airdrop;
    g_airdrop = NULL;
    pthread_mutex_unlock(&g_ad_lock);
    if (a) {
        airdrop_stop(a);
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 5;
        pthread_mutex_lock(&g_ad_lock);                                /* let name lookups / sends finish before the library goes away */
        while (g_ad.workers > 0) if (pthread_cond_timedwait(&g_ad_cv, &g_ad_lock, &until) != 0) break;
        g_ad.npeers = 0; g_ad.busy = 0; g_ad.status[0] = g_ad.hint[0] = 0;
        pthread_mutex_unlock(&g_ad_lock);
        airdrop_destroy(a);
    }
}
static void ad_download_dir(char* out, size_t n) {
    const char* x = getenv("XDG_DOWNLOAD_DIR");
    if (x && *x) snprintf(out, n, "%s", x);
    else snprintf(out, n, "%s/Downloads", g_xdg.home[0] ? g_xdg.home : ".");
}
static void ad_start(void) {
    if (g_airdrop) return;
    airdrop_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_log = ad_on_log; cb.on_peer = ad_on_peer; cb.on_ask = ad_on_ask; cb.on_progress = ad_on_progress;
    cb.on_received = ad_on_received; cb.on_done = ad_on_done;
    airdrop_config cfg;
    char dir[PATH_MAX];
    memset(&cfg, 0, sizeof(cfg));
    ad_download_dir(dir, sizeof(dir));
    cfg.name = g_settings.airplay_name[0] ? g_settings.airplay_name : "Luna Desktop";
    cfg.download_dir = dir;
    cfg.discoverable = 1;
    cfg.interface_name = access("/sys/class/net/awdl0", F_OK) == 0 ? "awdl0" : NULL;
    airdrop_t* a = airdrop_create(&cfg, &cb);
    if (!a) return;
    if (airdrop_start(a) != 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%.150s", airdrop_last_error(a));
        airdrop_destroy(a);
        ad_note("Could not start", msg);
        return;
    }
    pthread_mutex_lock(&g_ad_lock); g_airdrop = a; g_ad.status[0] = g_ad.hint[0] = 0; pthread_mutex_unlock(&g_ad_lock);
}

typedef struct { airdrop_t* ad; char host[80], name[72], path[PATH_MAX]; int port; } AdSendJob;
static void* ad_send_thread(void* arg) {
    AdSendJob* j = (AdSendJob*)arg;
    char err[160] = "";
    const char* files[1] = { j->path };
    int rc = airdrop_send(j->ad, j->host, j->port, files, 1, err, sizeof(err));
    const char* base = strrchr(j->path, '/');
    char msg[220];
    if (rc == 0) snprintf(msg, sizeof(msg), "%.100s → %.80s", base ? base + 1 : j->path, j->name);
    else snprintf(msg, sizeof(msg), "%.150s", err);
    ad_toast(rc == 0 ? "AirDrop · sent" : "AirDrop · failed", msg);
    free(j);
    ad_worker_end();
    return NULL;
}
static void ad_send_picked(const char* path) {
    pthread_mutex_lock(&g_ad_lock);
    airdrop_t* a = g_airdrop;
    int t = g_ad_target;
    if (!a || t < 0 || t >= g_ad.npeers) { pthread_mutex_unlock(&g_ad_lock); return; }
    AdSendJob* j = (AdSendJob*)calloc(1, sizeof(*j));
    if (!j) { pthread_mutex_unlock(&g_ad_lock); return; }
    j->ad = a; j->port = g_ad.peers[t].port;
    snprintf(j->host, sizeof(j->host), "%s", g_ad.peers[t].host);
    snprintf(j->name, sizeof(j->name), "%s", g_ad.peers[t].name);
    snprintf(j->path, sizeof(j->path), "%s", path);
    g_ad.workers++;
    g_ad.busy = 1;
    snprintf(g_ad.status, sizeof(g_ad.status), "Waiting for %.60s…", j->name);
    snprintf(g_ad.hint, sizeof(g_ad.hint), "The receiver has to accept.");
    g_ad.dirty = 1;
    pthread_mutex_unlock(&g_ad_lock);
    pthread_t th;
    if (pthread_create(&th, NULL, ad_send_thread, j) == 0) pthread_detach(th);
    else { free(j); ad_worker_end(); }
    shell_async_notify(NULL);
}
static void on_airdrop_peer(LunaElement* e) {
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!strncmp(id, "ad_peer_", 8) && isdigit((unsigned char)id[8])) {
            g_ad_target = atoi(id + 8);
            char title[120];
            pthread_mutex_lock(&g_ad_lock);
            snprintf(title, sizeof(title), "Send to %.80s", g_ad_target < g_ad.npeers ? g_ad.peers[g_ad_target].name : "device");
            pthread_mutex_unlock(&g_ad_lock);
            wpd_open_send(title, ad_send_picked);
            return;
        }
    }
}
static void on_airdrop_toggle(LunaElement* e) {
    (void)e;
    if (g_airdrop) { ad_stop(); g_settings.airdrop_enabled = 0; }
    else { ad_start(); g_settings.airdrop_enabled = g_airdrop != NULL; }
    settings_save();
    ad_ui_refresh();
}
static void airdrop_init(void) {
    g_ad_card = luna_get_element_by_id("widget_airdrop");
    g_ad_toggle = luna_get_element_by_id("ad_toggle");
    g_ad_status = luna_get_element_by_id("ad_status");
    g_ad_hint = luna_get_element_by_id("ad_hint");
    if (g_ad_card < 0) return;
    wire_subtree(g_ad_toggle, on_airdrop_toggle);
    for (int i = 0; i < AD_UI_PEERS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "ad_peer_%d", i);
        g_ad_peer_el[i] = luna_get_element_by_id(id);
        wire_subtree(g_ad_peer_el[i], on_airdrop_peer);
    }
    g_ad_ui_ready = 1;
    if (g_settings.airdrop_enabled) ad_start();
    ad_ui_refresh();
}
static void airdrop_tick(void) {
    if (!g_ad_ui_ready) return;
    pthread_mutex_lock(&g_ad_lock);
    int dirty = g_ad.dirty;
    pthread_mutex_unlock(&g_ad_lock);
    if (dirty) ad_ui_refresh();
}
static void airdrop_shutdown(void) { ad_stop(); }
#else
static void airdrop_ask_decide(int accept) { (void)accept; }
static void airdrop_init(void) {}
static void airdrop_tick(void) {}
static void airdrop_shutdown(void) {}
#endif

static void on_stats_style(LunaElement* e) {
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (!strcmp(id, "st_style_bar")) { g_settings.stats_style = 0; break; }
        if (!strcmp(id, "st_style_graph")) { g_settings.stats_style = 1; break; }
    }
    settings_mark_stats();
    stats_apply_style();
    settings_save();
}

static void on_stats_interval(LunaElement* e) {
    for (int i = elem_idx_of(e); i >= 0; i = luna_element_at(i)->parent_idx) {
        const char* id = luna_element_at(i)->id;
        if (str_has_prefix(id, "st_int_")) { g_settings.stats_interval = atoi(id + 7); break; }
    }
    settings_mark_stats();
    g_stats_next_at = g_now;
    settings_save();
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
        else buf[0] = '\0';
        /* CPU temperature rides on the AC/battery item (its menu holds the
         * fan and power controls). */
        if (fresh.cpu_temp > 0) {
            size_t n = strlen(buf);
            snprintf(buf + n, sizeof(buf) - n, "%s%d\u00b0", n ? " " : " ", fresh.cpu_temp);
        }
        /* Tint by severity: the thresholds are user settings, and a sensor-less
         * machine (temp <= 0) always shows the normal colour. */
        {
            int t = fresh.cpu_temp;
            const char* sev = !g_settings.temp_colors || t <= 0 ? NULL
                : t >= g_settings.temp_crit ? "temp_crit"
                : t >= g_settings.temp_hot  ? "temp_hot"
                : t >= g_settings.temp_warm ? "temp_warm" : NULL;
            if (luna_update_classes(idx, "temp_warm temp_hot temp_crit", sev))
                dirty_mb = 1;
        }
        /* Keep AC-only power status as compact as the other icon-only items. */
        luna_update_classes(idx, "icon_only", buf[0] ? NULL : "icon_only");
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
        const char* net_icon;
        if (!strcmp(fresh.network, "Wi-Fi"))
            net_icon = "\uf1eb"; /* Wi-Fi */
        else if (!strcmp(fresh.network, "Ethernet"))
            net_icon = "\uf6ff"; /* wired network */
        else
            net_icon = "\uf127"; /* disconnected / offline */
        if (g_mb_wifi_icon_idx >= 0 &&
            text_would_change(g_mb_wifi_icon_idx, net_icon)) {
            luna_set_text_paint_only(g_mb_wifi_icon_idx, net_icon);
            dirty_mb |= repaint_matters(g_mb_wifi_icon_idx);
        }
        // The network glyph already distinguishes Wi-Fi/Ethernet.  Keep the
        // compact menubar icon-only instead of repeating the connection name.
        if (text_would_change(idx, "")) {
            luna_set_text_paint_only(idx, "");
            dirty_mb |= repaint_matters(idx);
        }
    }

    if (dirty_mb) shell_request_repaint(1);
    if (dirty_cc && is_shown(g_cc_idx)) shell_request_repaint(6);

    /* The widget clock is applied on its own, not folded into whatever
     * background frame happens to occur: with a still wallpaper that frame
     * never came, so the clock froze until something else repainted the
     * desktop.  The text is stored even while a window covers the widget, and
     * a visible change costs one background frame per minute. */
    {
        static int clock_repaint_pending;
        int t_idx = g_ui_idx[UI_WG_TIME], d_idx = g_ui_idx[UI_WG_DATE];
        if (pending_bg_valid) {
            if (pending_bg.widget_time[0] && text_would_change(t_idx, pending_bg.widget_time)) {
                luna_set_text_paint_only(t_idx, pending_bg.widget_time);
                clock_repaint_pending = 1;
            }
            if (pending_bg.widget_date[0] && text_would_change(d_idx, pending_bg.widget_date)) {
                luna_set_text_paint_only(d_idx, pending_bg.widget_date);
                clock_repaint_pending = 1;
            }
        }
        if (clock_repaint_pending) {
            if (!(repaint_matters(t_idx) || repaint_matters(d_idx)))
                clock_repaint_pending = 0;      /* hidden: the stored text shows on next paint */
            else if (!g_switcher_visible && g_now - g_last_user_activity >= STATUS_BG_IDLE_GRACE_SEC) {
                shell_request_repaint(0);
                clock_repaint_pending = 0;
            }
        }
    }

    if (!pending_bg_valid) return;

    int wg_time_idx = g_ui_idx[UI_WG_TIME];
    int wg_date_idx = g_ui_idx[UI_WG_DATE];
    int stats_live = 0;   /* the statistics widget has its own timer: stats_tick() */
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
    int bg_status_safe = (!shell_desktop_busy() || g_settings.widgets_enabled) &&
                         !g_switcher_visible &&
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

        if (repaint_matters(g_ui_idx[UI_ST_DISK_VAL])) {
            stats_set_storage(&snap);
            stats_set_temp(snap.cpu_temp);
            dirty_bg = 1;
        }
    }

    if (dirty_bg) shell_request_repaint(0);
    pending_bg_valid = 0;
}

/* Bound event sleep by the next scheduled shell job.  Input descriptors wake
 * poll immediately, so longer idle sleeps do not add input latency. */
static int shell_wait_timeout_ms(int max_ms, double repaint_deadline) {
    if (g_cmd_count && max_ms > 10) max_ms = 10;
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
        SOONER(g_alarm_next_deadline);
        if (g_stopwatch_running && g_stopwatch_mode)
            SOONER(g_stopwatch_next_deadline);
        SOONER(g_session_restore_at);
        if (g_desk_shot_at > 0.0) SOONER(g_desk_shot_at);
        if (g_settings.widgets_enabled && g_settings.stats_interval > 0)
            SOONER(g_stats_next_at);
        if (g_mg_cols && g_settings.mb_graph != 3) SOONER(g_mg_next_at);
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
    g_lp_grid_idx       = luna_get_element_by_id("lp_grid");
    g_settings_idx      = luna_get_element_by_id("settings_win");
    g_settings_sheet_idx = luna_get_element_by_id("settings_sheet");
    g_settings_panel_apps = luna_get_element_by_id("settings_panel_apps");
    g_settings_panel_disp = luna_get_element_by_id("settings_panel_disp");
    g_settings_panel_lang = luna_get_element_by_id("settings_panel_language");
    g_settings_panel_kb   = luna_get_element_by_id("settings_panel_keyboard");
    g_settings_panel_sound = luna_get_element_by_id("settings_panel_sound");
    g_settings_panel_display = luna_get_element_by_id("settings_panel_display");
    g_settings_panel_wm   = luna_get_element_by_id("settings_panel_wm");
    g_stab_apps_idx     = luna_get_element_by_id("stab_apps");
    g_stab_disp_idx     = luna_get_element_by_id("stab_disp");
    g_stab_lang_idx     = luna_get_element_by_id("stab_language");
    g_stab_kb_idx       = luna_get_element_by_id("stab_keyboard");
    g_stab_sound_idx    = luna_get_element_by_id("stab_sound");
    g_stab_display_idx  = luna_get_element_by_id("stab_display");
    g_stab_wm_idx       = luna_get_element_by_id("stab_wm");
    g_win_menu_idx      = luna_get_element_by_id("win_menu");
    g_clip_menu_idx     = luna_get_element_by_id("clip_menu");
    g_mb_logo_idx       = luna_get_element_by_id("mb_logo");
    g_mb_cc_idx         = luna_get_element_by_id("mb_cc");
    g_mb_wifi_idx       = luna_get_element_by_id("mb_wifi");
    g_wifi_menu_idx     = luna_get_element_by_id("wifi_menu");
    g_bt_menu_idx       = luna_get_element_by_id("bt_menu");
    g_power_menu_idx    = luna_get_element_by_id("power_menu");
    g_desk_menu_idx     = luna_get_element_by_id("desk_menu");
    g_tray_menu_idx     = luna_get_element_by_id("tray_menu");
    g_mb_bat_idx        = luna_get_element_by_id("mb_bat");
    g_mb_weather_idx    = luna_get_element_by_id("mb_weather");
    g_weather_menu_idx  = luna_get_element_by_id("weather_menu");
    g_calendar_menu_idx = luna_get_element_by_id("calendar_menu");
    g_alarm_ring_idx    = luna_get_element_by_id("alarm_ring");
    g_mb_clock_idx      = luna_get_element_by_id("mb_clock");
    g_net_detail_idx    = luna_get_element_by_id("net_detail_win");
    wpd_init_ids();
    g_net_detail_box_idx = luna_get_element_by_id("net_detail_box");

    for (int i = 0; i < UI_CACHE_COUNT; i++)
        g_ui_idx[i] = luna_get_element_by_id(g_ui_cache_ids[i]);
    if (g_ui_idx[UI_WIDGET_CLOCK] < 0)
        fprintf(stderr, "[luna-shell] widget_clock missing — desktop widgets were not injected\n");

    /* Older skins predate explicit Storage IDs. Resolve those two cached
     * elements from the stats widget so live updates remain skin-compatible. */
    if (g_ui_idx[UI_ST_DISK_VAL] < 0) {
        int stats = g_ui_idx[UI_WIDGET_STATS];
        if (stats >= 0) {
            for (int i = 0; i < luna_element_count(); i++) {
                LunaElement* el = luna_element_at(i);
                if (!el) continue;
                int inside = i == stats;
                for (int p = el->parent_idx; !inside && p != -1; ) {
                    LunaElement* parent = luna_element_at(p);
                    if (!parent) break;
                    if (p == stats) inside = 1;
                    p = parent->parent_idx;
                }
                if (!inside) continue;
                if (g_ui_idx[UI_ST_DISK_FILL] < 0 &&
                    strstr(el->class_name, "g_disk"))
                    g_ui_idx[UI_ST_DISK_FILL] = i;
                if (g_ui_idx[UI_ST_DISK_VAL] < 0 &&
                    strstr(el->class_name, "st_val") &&
                    i != g_ui_idx[UI_ST_CPU_VAL] &&
                    i != g_ui_idx[UI_ST_MEM_VAL])
                    g_ui_idx[UI_ST_DISK_VAL] = i;
            }
        }
    }
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
        for (int k = 0; k < SHELL_DLG_COUNT; k++) {
            int idx = luna_get_element_by_id(g_shell_dialogs[k].drag_id);
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
    wire_subtree(luna_get_element_by_id("wg_time"),       on_calendar_menu);
    wire_subtree(g_ui_idx[UI_WG_STOPWATCH_OPEN],          on_stopwatch_open);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_back"), on_stopwatch_back);
    wire_subtree(g_ui_idx[UI_WG_STOPWATCH_TOGGLE],      on_stopwatch_toggle);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_reset"), on_stopwatch_reset);
    stopwatch_set_mode(g_stopwatch_mode);
    stats_cols_init();
    mbgraph_init();
    mb_order_apply();
    airplay_init();
    airdrop_init();
    stats_apply_style();
    stopwatch_render();
    wire_subtree(luna_get_element_by_id("cal_prev"),      on_calendar_prev);
    wire_subtree(luna_get_element_by_id("cal_next"),      on_calendar_next);
    wire_subtree(luna_get_element_by_id("cal_today_btn"), on_calendar_today);
    for (int i = 0; i < 42; i++) {
        char id[16]; snprintf(id, sizeof(id), "cal_d%d", i);
        wire_subtree(luna_get_element_by_id(id), on_calendar_day);
    }
    wire_subtree(luna_get_element_by_id("cal_alarm_add"),    on_calendar_alarm_add);
    wire_subtree(luna_get_element_by_id("cal_alarm_repeat"), on_calendar_alarm_repeat);
    for (int i = 0; i < MAX_CAL_ALARM_ROWS; i++) {
        char id[24]; snprintf(id, sizeof(id), "cal_al%d_del", i);
        wire_subtree(luna_get_element_by_id(id), on_calendar_alarm_delete);
    }
    wire_subtree(luna_get_element_by_id("alarm_snooze"),  on_alarm_snooze);
    wire_subtree(luna_get_element_by_id("alarm_dismiss"), on_alarm_dismiss);
    wire_subtree(luna_get_element_by_id("alarm_ring_backdrop"), on_alarm_dismiss);
    wire_subtree(luna_get_element_by_id("mb_weather"),    on_weather_menu);
    wire_subtree(luna_get_element_by_id("weather_go"),    on_weather_go);
    wire_subtree(luna_get_element_by_id("widget_weather"),on_widget_weather);
    wire_subtree(luna_get_element_by_id("mb_cc"),         on_control_center);
    wire_subtree(luna_get_element_by_id("mb_bat"),        on_power_menu);
    for (int i = 0; i < TRAY_MENU_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "tw_%d", i);
        wire_subtree(luna_get_element_by_id(id), on_tray_menu_pick);
    }
    wire_subtree(luna_get_element_by_id("tw_mv_l"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("tw_mv_r"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("tw_mv_x"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("tw_wg_hide"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("tw_wg_move"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("tw_wg_reset"), on_tray_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_wgshow"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("mb_app"), on_mb_app_click);
    wire_subtree(luna_get_element_by_id("dm_shot"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_term"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_widgets"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_wallfile"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_wallreset"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_wallfolder"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("wp_pick_file"), on_wp_button);
    wire_subtree(luna_get_element_by_id("wp_open_dir"), on_wp_button);
    wire_subtree(luna_get_element_by_id("wp_motion_fps"), on_wp_button);
    wire_subtree(luna_get_element_by_id("wp_motion_speed"), on_wp_button);
    wire_subtree(luna_get_element_by_id("wp_saver_time"), on_wp_button);
    wire_subtree(luna_get_element_by_id("wp_saver_file"), on_wp_button);
    wp_ui_refresh();
    wire_subtree(luna_get_element_by_id("dm_wall"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_settings"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("dm_launch"), on_desk_menu_pick);
    wire_subtree(luna_get_element_by_id("fan_auto"),      on_fan_pick);
    wire_subtree(luna_get_element_by_id("fan_low"),       on_fan_pick);
    wire_subtree(luna_get_element_by_id("fan_mid"),       on_fan_pick);
    wire_subtree(luna_get_element_by_id("fan_high"),      on_fan_pick);
    wire_subtree(luna_get_element_by_id("fan_max"),       on_fan_pick);
    wire_subtree(luna_get_element_by_id("pw_save"),       on_power_pick);
    wire_subtree(luna_get_element_by_id("pw_bal"),        on_power_pick);
    wire_subtree(luna_get_element_by_id("pw_perf"),       on_power_pick);
    wire_subtree(luna_get_element_by_id("pw_turbo"),      on_power_pick);
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
    g_tray_area_idx = luna_get_element_by_id("tray_area");
    for (int i = 0; i < MAX_TRAY_SLOTS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "tray_%d", i);
        g_tray_slot_idx[i] = luna_get_element_by_id(id);
        wire_subtree(g_tray_slot_idx[i], on_tray_click);
    }

    /* Wire settings tab buttons */
    wire_subtree(g_stab_apps_idx, on_settings_tab);
    wire_subtree(g_stab_disp_idx, on_settings_tab);
    wire_subtree(g_stab_lang_idx, on_settings_tab);
    wire_subtree(g_stab_kb_idx, on_settings_tab);
    wire_subtree(g_stab_sound_idx, on_settings_tab);
    wire_subtree(g_stab_display_idx, on_settings_tab);
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
        const char* scale_tracks[] = {
            "scale_dpi_track", "scale_gdk_track", "scale_qt_track", "scale_cur_track"
        };
        for (size_t i = 0; i < sizeof(scale_tracks) / sizeof(scale_tracks[0]); i++)
            wire_subtree(luna_get_element_by_id(scale_tracks[i]), on_cc_track);
        const char* shortcut_ids[] = {
            "shortcut_launchpad", "shortcut_settings", "shortcut_screenshot"
        };
        for (size_t i = 0; i < sizeof(shortcut_ids) / sizeof(shortcut_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(shortcut_ids[i]), on_shortcut_row);
    }
    {
        const char* toggle_ids[] = {
            "wm_snap", "wm_dock_snap", "wm_tile", "wm_corner", "wm_tray_group", "wm_top_maximize", "wm_double_click", "wm_classic_titlebar", "wm_shortcuts",
            "wm_dock", "wm_widgets", "wm_dock_mag", "wm_wallpaper_motion", "wm_restore",
            "wm_focus_outline", "wm_window_list", "wm_window_tray", "wm_cascade"
        };
        for (size_t i = 0; i < sizeof(toggle_ids) / sizeof(toggle_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(toggle_ids[i]), on_wm_toggle);
        static const char* const st_style_ids[] = { "st_style_bar", "st_style_graph" };
        for (size_t i = 0; i < 2; i++) wire_subtree(luna_get_element_by_id(st_style_ids[i]), on_stats_style);
        static const char* const st_int_ids[] = { "st_int_2", "st_int_5", "st_int_10", "st_int_30", "st_int_0" };
        for (size_t i = 0; i < 5; i++) wire_subtree(luna_get_element_by_id(st_int_ids[i]), on_stats_interval);
        const char* gap_ids[] = { "wm_gap_0", "wm_gap_8", "wm_gap_16" };
        for (size_t i = 0; i < sizeof(gap_ids) / sizeof(gap_ids[0]); i++)
            wire_subtree(luna_get_element_by_id(gap_ids[i]), on_wm_gap);
    }

    wire_subtree(g_mb_clip_idx, on_clipboard_menu);
    wire_subtree(luna_get_element_by_id("clip_clear"), on_clip_clear);
    wire_subtree(luna_get_element_by_id("clip_more"), on_clip_page);
    wire_subtree(luna_get_element_by_id("clip_back"), on_clip_page);
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
    memset(g_win_glyph_idx,  -1, sizeof(g_win_glyph_idx));
    memset(g_tray_glyph_idx, -1, sizeof(g_tray_glyph_idx));
    memset(g_tray_tip_idx,   -1, sizeof(g_tray_tip_idx));
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

        /* win_label / win_glyph children */
        if (strstr(e->class_name, "win_label")) {
            for (int s = 0; s < MAX_WIN_SLOTS; s++)
                if (g_win_slot_idx[s] == p) { g_win_label_idx[s] = i; break; }
            continue;
        }
        if (strstr(e->class_name, "win_glyph")) {
            for (int s = 0; s < MAX_WIN_SLOTS; s++)
                if (g_win_slot_idx[s] == p) { g_win_glyph_idx[s] = i; break; }
            continue;
        }
        /* tray_glyph children */
        if (strstr(e->class_name, "tray_glyph")) {
            for (int s = 0; s < MAX_TRAY_SLOTS; s++)
                if (g_tray_slot_idx[s] == p) { g_tray_glyph_idx[s] = i; break; }
            continue;
        }
        if (strstr(e->class_name, "tray_tip")) {
            for (int s = 0; s < MAX_TRAY_SLOTS; s++)
                if (g_tray_slot_idx[s] == p) {
                    g_tray_tip_idx[s] = i;
                    luna_update_classes(i, "hidden", NULL);
                    break;
                }
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

    /* Hide HTML preview chips until the compositor snapshot arrives.  Glyph
     * indices must already be cached so the first real update can paint icons. */
    update_window_list_ui();
    update_tray_ui();

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

    /* XDG application metadata is needed by both Launchpad and the Dock, so
     * populate it at bind time instead of waiting for the first Launchpad open. */
    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    dock_pinned_load();
    dock_xdg_init();
    dock_populate_xdg();
    (void)dock_pinned_save; /* kept for future Keep/Remove in Dock UI */
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
    int  (*present_busy)(void);
    void (*set_cursor)(int cursor_type);
    void (*terminate)(void);
} LunaBackend;

/* Mesa's threaded software front-end can return before its wl_shm pixels are
 * complete, so keep the old finish workaround only when that front-end is
 * explicitly active.  LUNA_EGL_SOFTWARE disables mesa_glthread during setup;
 * in that normal path (and on hardware), eglSwapBuffers provides the required
 * flush and an extra glFinish/glFlush only serializes every layer. */
static void shell_gl_sync_before_swap(void) {
    static int needs_finish = -1;
    if (needs_finish < 0) {
        const char* r = (const char*)glGetString(GL_RENDERER);
        int software = r && (strstr(r, "llvmpipe") || strstr(r, "softpipe") ||
                             strstr(r, "SwiftShader"));
        needs_finish = software && env_is_true("mesa_glthread");
    }
    if (needs_finish)
        glFinish();
}

static const LunaBackend* g_backend = NULL;
static const LunaBackend g_wl_backend; /* forward declaration */

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
    if (shortcut_handle_key(key, mods, action))
        return;
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
            if (is_shown(g_alarm_ring_idx))  { on_alarm_dismiss(NULL);   return; }
            if (is_shown(g_settings_idx))    { on_settings_close(NULL); return; }
            if (is_shown(g_launchpad_idx))   { launchpad_close();       return; }
            if (is_shown(g_confirm_idx))     { on_confirm_cancel(NULL); return; }
            if (is_shown(g_about_idx))       { on_about_close(NULL);    return; }
            if (is_shown(g_win_menu_idx))    { dismiss_win_menu();      return; }
            if (is_shown(g_clip_menu_idx))   { dismiss_clip_menu();     return; }
            if (is_shown(g_wifi_menu_idx))   { dismiss_wifi_menu();     return; }
            if (is_shown(g_bt_menu_idx))     { dismiss_bt_menu();       return; }
            if (is_shown(g_power_menu_idx))  { dismiss_power_menu();    return; }
            if (is_shown(g_desk_menu_idx))   { dismiss_desk_menu();     return; }
            if (is_shown(g_tray_menu_idx))   { dismiss_tray_menu();     return; }
            if (is_shown(g_weather_menu_idx)){ dismiss_weather_menu();  return; }
            if (is_shown(g_calendar_menu_idx)){ dismiss_calendar_menu(); return; }
            if (g_wg_moving >= 0)            { g_wg_moving = -1;        return; }
            if (is_shown(g_wpd_win))         { wpd_close();             return; }
            if (is_shown(g_net_detail_idx))  { net_detail_close(NULL);  return; }
            if (is_shown(g_luna_menu_idx))   { dismiss_luna_menu(g_luna_menu_idx); return; }
            if (is_shown(g_cc_idx))          { dismiss_cc(g_cc_idx);    return; }
        }
        if (is_shown(g_wpd_win) &&
            (key == LUNA_KEY_PAGE_DOWN || key == LUNA_KEY_PAGE_UP)) {
            g_wpd_page += key == LUNA_KEY_PAGE_DOWN ? 1 : -1;
            wpd_render();
            return;
        }
        if (is_shown(g_launchpad_idx) &&
            (key == LUNA_KEY_PAGE_DOWN || key == LUNA_KEY_PAGE_UP)) {
            launchpad_scroll_page(key == LUNA_KEY_PAGE_DOWN ? 1 : -1);
            return;
        }
        if (is_shown(g_alarm_ring_idx) &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            on_alarm_dismiss(NULL);
            return;
        }
        if (is_shown(g_calendar_menu_idx) &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            on_calendar_alarm_add(NULL);
            return;
        }
        if (is_shown(g_weather_menu_idx) &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            weather_do_search();
            return;
        }
        if (is_shown(g_wifi_menu_idx) &&
            luna_focused_element() == luna_get_element_by_id("wifi_password") &&
            (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER)) {
            on_wifi_connect(NULL);
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
        if (shortcut_try_dispatch(key, mods))
            return;
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
        case XKB_KEY_F1:        return LUNA_KEY_F1;
        case XKB_KEY_F2:        return LUNA_KEY_F2;
        case XKB_KEY_F3:        return LUNA_KEY_F3;
        case XKB_KEY_F4:        return LUNA_KEY_F4;
        case XKB_KEY_F5:        return LUNA_KEY_F5;
        case XKB_KEY_F6:        return LUNA_KEY_F6;
        case XKB_KEY_F7:        return LUNA_KEY_F7;
        case XKB_KEY_F8:        return LUNA_KEY_F8;
        case XKB_KEY_F9:        return LUNA_KEY_F9;
        case XKB_KEY_F10:       return LUNA_KEY_F10;
        case XKB_KEY_F11:       return LUNA_KEY_F11;
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
    struct gbm_bo* flip_bo; /* BO queued for the outstanding page flip */

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
static void kms_cursor_move(void) {
    if (!g_kms.cursor_shown) return;
    drmModeMoveCursor(g_kms.fd, g_kms.crtc_id,
                      (int)g_kms.mouse_x - g_kms.cursor_hot_x,
                      (int)g_kms.mouse_y - g_kms.cursor_hot_y);
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


static void cursor_theme_tick_and_refresh(void) {
    if (g_cursor_reload_pending) {
        g_cursor_reload_pending = 0;
        g_cursor_frame_changed = 1;
        if (g_backend && g_backend->set_cursor)
            g_backend->set_cursor(g_cur_theme.active_role);
        g_cursor_frame_changed = 0;
    }
    if (!g_cursor_present) {
        shell_runtime_maintenance();
        return;
    }
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
    } else if (luna_cur_theme_tick(&g_cur_theme, g_now)) {
        /* Re-push the current role so animated .ani frames advance. */
        g_cursor_frame_changed = 1;
        if (g_backend && g_backend->set_cursor)
            g_backend->set_cursor(g_cur_theme.active_role);
        g_cursor_frame_changed = 0;
    }
    shell_runtime_maintenance();
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





static int kms_open_drm_device(void) {
    static const char* nodes[] = { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL };
    for (int i = 0; nodes[i]; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        if (res) {
            drmModeFreeResources(res);
            /* Page-flip completions are drained from poll(); a blocking read
             * there would stall the whole shell on real KMS drivers. */
            int flags = fcntl(fd, F_GETFL);
            if (flags >= 0)
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            return fd;
        }
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
        case LIBINPUT_EVENT_DEVICE_ADDED: {
            struct libinput_device* device = libinput_event_get_device(ev);
            fprintf(stderr, "[luna-shell/kms] input added: %s%s%s%s%s\n",
                    libinput_device_get_name(device),
                    libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD) ? " keyboard" : "",
                    libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)  ? " pointer" : "",
                    libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)    ? " touch" : "",
                    libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TABLET_TOOL) ? " tablet" : "");
            break;
        }
        case LIBINPUT_EVENT_DEVICE_REMOVED: {
            struct libinput_device* device = libinput_event_get_device(ev);
            fprintf(stderr, "[luna-shell/kms] input removed: %s\n",
                    libinput_device_get_name(device));
            break;
        }
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
            if (luna_mouse_move_changed(g_kms.mouse_x, g_kms.mouse_y))
                g_frame_dirty = 1;
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer* p = libinput_event_get_pointer_event(ev);
            g_kms.mouse_x = libinput_event_pointer_get_absolute_x_transformed(p, g_kms.width);
            g_kms.mouse_y = libinput_event_pointer_get_absolute_y_transformed(p, g_kms.height);
            kms_cursor_move();
            shell_note_user_activity();
            if (luna_mouse_move_changed(g_kms.mouse_x, g_kms.mouse_y))
                g_frame_dirty = 1;
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
            shell_mouse_button(btn, action, xkb_mod_bits(g_kms.xkb_state), g_kms.mouse_x, g_kms.mouse_y);
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
            shell_scroll(-xv / 10.0, -yv / 10.0);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_UP:
        case LIBINPUT_EVENT_TOUCH_CANCEL: {
            struct libinput_event_touch* t = libinput_event_get_touch_event(ev);
            struct libinput_device* device = libinput_event_get_device(ev);
            int type = libinput_event_get_type(ev);
            int slot = libinput_event_touch_get_seat_slot(t);
            LunaTouchEvent touch;
            memset(&touch, 0, sizeof(touch));
            /* A seat slot can be reused after UP.  Pairing it with the device
             * keeps simultaneous contacts from different touch devices apart. */
            touch.id = (int64_t)(((uint64_t)(uintptr_t)device << 8) ^
                                 (uint32_t)(slot < 0 ? 0 : slot));
            touch.phase = type == LIBINPUT_EVENT_TOUCH_DOWN   ? LUNA_TOUCH_DOWN :
                          type == LIBINPUT_EVENT_TOUCH_MOTION ? LUNA_TOUCH_MOVE :
                          type == LIBINPUT_EVENT_TOUCH_UP     ? LUNA_TOUCH_UP : LUNA_TOUCH_CANCEL;
            touch.tool = LUNA_TOUCH_TOOL_FINGER;
            touch.x = libinput_event_touch_get_x_transformed(t, g_kms.width);
            touch.y = libinput_event_touch_get_y_transformed(t, g_kms.height);
            touch.pressure = (touch.phase == LUNA_TOUCH_UP ||
                              touch.phase == LUNA_TOUCH_CANCEL) ? 0.0f : 1.0f;
            /* Keep the pointer position coherent with the primary-touch mouse
             * emulation in luna_touch(), but do not expose the hardware cursor. */
            g_kms.mouse_x = touch.x;
            g_kms.mouse_y = touch.y;
            shell_note_user_activity();
            luna_touch(&touch);
            g_frame_dirty = 1;
            break;
        }
        case LIBINPUT_EVENT_TABLET_TOOL_TIP:
        case LIBINPUT_EVENT_TABLET_TOOL_AXIS: {
            struct libinput_event_tablet_tool* t = libinput_event_get_tablet_tool_event(ev);
            struct libinput_tablet_tool* tool = libinput_event_tablet_tool_get_tool(t);
            enum libinput_tablet_tool_type tool_type = libinput_tablet_tool_get_type(tool);
            int type = libinput_event_get_type(ev);
            LunaTouchEvent touch;
            memset(&touch, 0, sizeof(touch));
            touch.id = (int64_t)(uintptr_t)tool;
            touch.phase = type == LIBINPUT_EVENT_TABLET_TOOL_AXIS ? LUNA_TOUCH_MOVE :
                (libinput_event_tablet_tool_get_tip_state(t) == LIBINPUT_TABLET_TOOL_TIP_DOWN
                     ? LUNA_TOUCH_DOWN : LUNA_TOUCH_UP);
            touch.tool = tool_type == LIBINPUT_TABLET_TOOL_TYPE_ERASER
                             ? LUNA_TOUCH_TOOL_ERASER : LUNA_TOUCH_TOOL_STYLUS;
            touch.x = libinput_event_tablet_tool_get_x_transformed(t, g_kms.width);
            touch.y = libinput_event_tablet_tool_get_y_transformed(t, g_kms.height);
            touch.pressure = touch.phase == LUNA_TOUCH_UP ? 0.0f :
                             (float)libinput_event_tablet_tool_get_pressure(t);
            touch.tilt_x = (float)libinput_event_tablet_tool_get_tilt_x(t);
            touch.tilt_y = (float)libinput_event_tablet_tool_get_tilt_y(t);
            g_kms.mouse_x = touch.x;
            g_kms.mouse_y = touch.y;
            shell_note_user_activity();
            luna_touch(&touch);
            g_frame_dirty = 1;
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
            if (pressed && !g_shortcut_capture) {
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
    if (g_kms.prev_bo) {
        gbm_surface_release_buffer(g_kms.gbm_surf, g_kms.prev_bo);
        g_kms.prev_bo = NULL;
    }
    g_kms.prev_bo = g_kms.flip_bo;
    g_kms.flip_bo = NULL;
    g_kms.flip_pending = 0;
}

static void kms_drain_page_flips(void) {
    if (!g_kms.flip_pending) return;
    drmEventContext evctx = { .version = 2, .page_flip_handler = kms_page_flip_handler };
    for (;;) {
        if (drmHandleEvent(g_kms.fd, &evctx) != 0)
            break;
        if (!g_kms.flip_pending)
            return;
    }
}

static int kms_present_busy(void) { return g_kms.flip_pending; }

static void kms_swap_buffers(void) {
    if (g_kms.flip_pending)
        return;
    shell_gl_sync_before_swap();
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
        g_kms.prev_bo = bo;
        return;
    }

    g_kms.flip_pending = 1;
    g_kms.flip_bo = bo;
    if (drmModePageFlip(g_kms.fd, g_kms.crtc_id, fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0) {
        fprintf(stderr, "[luna-shell/kms] page flip failed: %s\n",
                strerror(errno));
        g_kms.flip_pending = 0;
        g_kms.flip_bo = NULL;
        gbm_surface_release_buffer(g_kms.gbm_surf, bo);
        return;
    }
    /* prev_bo stays locked until the flip event retires it.  Blocking in
     * swap_buffers for that event was the regular console hitch on real KMS
     * hardware; QEMU often delivers the event immediately so the bug hid there. */
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
    int action_fd = shell_action_poll_fd();
    double outer_deadline = plat_time() +
        (double)g_single_poll_timeout_ms / 1000.0;

    for (;;) {
        struct pollfd pfds[5];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = g_kms.fd, .events = POLLIN };
        if (g_kms.li)
            pfds[nfds++] = (struct pollfd){ .fd = libinput_get_fd(g_kms.li), .events = POLLIN };
        if (state_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = state_fd, .events = POLLIN };
        if (async_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = async_fd, .events = POLLIN };
        if (action_fd >= 0)
            pfds[nfds++] = (struct pollfd){ .fd = action_fd, .events = POLLIN };
        int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
        int pr;
        do { pr = poll(pfds, nfds, timeout_ms); }
        while (pr < 0 && errno == EINTR);

        int flip_ready = pr > 0 && (pfds[0].revents & POLLIN);
        if (flip_ready)
            kms_drain_page_flips();

        int input_ready = 0, state_ready = 0, async_ready = 0, action_ready = 0;
        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            if (pfds[i].fd == g_kms.fd) continue;
            if (g_kms.li && pfds[i].fd == libinput_get_fd(g_kms.li))
                input_ready = 1;
            else if (state_fd >= 0 && pfds[i].fd == state_fd)
                state_ready = 1;
            else if (async_fd >= 0 && pfds[i].fd == async_fd)
                async_ready = 1;
            else if (action_fd >= 0 && pfds[i].fd == action_fd)
                action_ready = 1;
        }
        if (action_ready)
            (void)shell_action_drain();
        if (state_ready) {
            (void)shell_state_watch_drain();
            if (g_interaction_busy)
                outer_deadline = plat_time() + 1.0;
        }
        if (async_ready) shell_async_drain();

        int async_deferred = async_ready && g_interaction_busy;
        if (flip_ready || action_ready || pr <= 0 || input_ready ||
            (async_ready && !async_deferred))
            break;
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
    if (g_kms.flip_bo)    gbm_surface_release_buffer(g_kms.gbm_surf, g_kms.flip_bo);
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
    .present_busy = kms_present_busy,
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
    int                            kbd_exclusive;      /* grab keys while mapped */
    /* Modeless dialog whose Wayland buffer should cover input_root only,
     * rather than allocating a transparent full-output buffer chain. */
    int                            compact_to_input;
    /* runtime */
    int                            root_idx;
    int                            input_root_idx;
    int                            input_x, input_y, input_w, input_h;
    struct wl_surface*             wl_surf;
    struct zwlr_layer_surface_v1*  layer_surf;
    struct wl_egl_window*          egl_win;
    EGLSurface                     egl_surf;
    /* Presentation pacing.  A fixed 17 ms timer runs at 58.8 Hz and beats
     * against a physical 60 Hz scanout, producing a very visible periodic
     * hitch.  Keep at most one committed frame in flight and let the
     * compositor's wl_surface.frame event open the next slot. */
    struct wl_callback*            frame_cb;
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
    /* bg: wallpaper + desktop widgets. The client publishes only widget
     * rectangles as input; blank wallpaper remains click-through. */
    { .name="bg",           .root_id="bg_layer",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, .anchor=ZWLR_ANCHOR_ALL,
      .exclusive_zone=0, .is_kbd=0 },
    /* The top-edge buffer includes 56 transparent overflow pixels for the
     * CSS tooltip.  Allocate them before EGL creation: resizing this surface
     * later is unreliable on software/QEMU Wayland and can leave the old 32px
     * buffer attached.  Its exclusive zone and input region remain the 32px
     * #menubar box, so the transparent overflow never intercepts clicks (in
     * particular clicks used to dismiss an IME candidate window). */
    { .name="menubar",      .root_id="menubar", .input_root_id="menubar",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT|ZWLR_ANCHOR_RIGHT,
      .exclusive_zone=32, .fixed_h=88 },
    /* dock: floating bar at the bottom */
    { .name="dock",         .root_id="dock",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_BOTTOM,
      .exclusive_zone=92, .margin_bottom=12, .fixed_w=960, .fixed_h=84 },
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
      .is_overlay=1, .is_kbd=1, .kbd_exclusive=1 },
    /* Modeless utility dialogs are real small layer-surfaces.  Their exact
     * geometry is synchronized from input_root after Luna layout, so skins
     * remain free to change these bootstrap sizes. */
    /* screensaver: covers the whole output including the bars */
    { .name="saver",        .root_id="screensaver",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .exclusive_zone=-1, .is_overlay=1, .is_kbd=1, .kbd_exclusive=1 },
    { .name="settings",     .root_id="settings_win", .input_root_id="settings_sheet",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT,
      .fixed_w=748, .fixed_h=688,
      .is_overlay=1, .is_kbd=1, .compact_to_input=1 },
    { .name="about",        .root_id="about_win", .input_root_id="about_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT,
      .fixed_w=520, .fixed_h=628,
      .is_overlay=1, .compact_to_input=1 },
    { .name="confirm",      .root_id="confirm_overlay", .input_root_id="confirm_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT,
      .fixed_w=548, .fixed_h=368,
      .is_overlay=1, .is_kbd=1, .compact_to_input=1 },
    /* toast: small fixed surface, TOP|RIGHT anchored */
    { .name="toast",        .root_id="toast",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_RIGHT,
      .margin_top=38, .margin_right=14, .fixed_w=340, .fixed_h=76,
      .is_overlay=1 },
    /* wifi_menu / bt_menu: full-output overlays so position_menu_near works on Wayland */
    { .name="wifi_menu",    .root_id="wifi_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="bt_menu",      .root_id="bt_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="tray_menu",    .root_id="tray_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="desk_menu",    .root_id="desk_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="power_menu",   .root_id="power_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="weather_menu", .root_id="weather_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="calendar_menu", .root_id="calendar_menu",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1, .is_kbd=1 },
    { .name="alarm_ring", .root_id="alarm_ring", .input_root_id="alarm_ring_card",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, .anchor=ZWLR_ANCHOR_ALL,
      .is_overlay=1 },
    { .name="net_detail", .root_id="net_detail_win", .input_root_id="net_detail_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT,
      .fixed_w=520, .fixed_h=648,
      .is_overlay=1, .compact_to_input=1 },
    { .name="wpd", .root_id="wpd_win", .input_root_id="wpd_box",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor=ZWLR_ANCHOR_TOP|ZWLR_ANCHOR_LEFT,
      .fixed_w=680, .fixed_h=560,
      .is_overlay=1, .is_kbd=1, .compact_to_input=1 },
};
#define LUNA_SURF_COUNT (int)(sizeof(g_surfs)/sizeof(g_surfs[0]))

/* X11 tray icons live in compositor-drawn windows; tell it where their menubar
 * slots are.  Sent only when a rectangle changes. */
static void shell_x11_tray_report(void) {
    static int last[MAX_TRAY_SLOTS][5];
    static unsigned last_bg = 0xffffffffu;
    LunaSurface* mb = &g_surfs[LUNA_SURF_MENUBAR];
    /* X icons are opaque: paint their container in the menubar's colour. */
    int mbi = mb->root_idx;
    if (mbi >= 0) {
        LunaElement* m = luna_element_at(mbi);
        unsigned bg = ((unsigned)lroundf(m->cur_r * 255.0f) & 255u) << 16 |
                      ((unsigned)lroundf(m->cur_g * 255.0f) & 255u) << 8 |
                      ((unsigned)lroundf(m->cur_b * 255.0f) & 255u);
        if (bg != last_bg) {
            last_bg = bg;
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "x11_tray_bg %06x", bg);
            shell_send_cmd(cmd);
        }
    }
    for (int s = 0; s < MAX_TRAY_SLOTS; s++) {
        const char* key = g_tray_slot_key[s];
        if (strncmp(key, "x11:", 4) != 0) { last[s][4] = 0; continue; }
        int idx = g_tray_slot_idx[s];
        LunaElement* e = idx >= 0 ? luna_element_at(idx) : NULL;
        int shown = e && e->w > 0.0f && e->h > 0.0f && is_shown(idx);
        int r[5] = { -64, -64, 24, 24, 1 };
        if (shown) {
            r[0] = (int)lroundf(e->x - mb->doc_x);
            r[1] = (int)lroundf(e->y - mb->doc_y);
            r[2] = (int)lroundf(e->w);
            r[3] = (int)lroundf(e->h);
        }
        if (!memcmp(last[s], r, sizeof(r))) continue;
        memcpy(last[s], r, sizeof(r));
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "x11_tray_pos %s %d %d %d %d", key + 4, r[0], r[1], r[2], r[3]);
        shell_send_cmd(cmd);
    }
}

static void saver_bind(void) {
    g_saver_idx = luna_get_element_by_id("screensaver");
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        if (!strcmp(g_surfs[i].name, "saver")) g_saver_surf = i;
        if (!strcmp(g_surfs[i].name, "wpd")) g_wpd_surf = i;
    }
}

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
    /* The background has no root filter, so its render path is luna_render()
     * over the complete output even when only one widget changed.  Damage
     * limits the compositor copy, but it cannot recover the GL/layout time
     * already spent here.  Fold status changes into a frame that was going to
     * happen anyway instead of manufacturing a periodic full-screen frame. */
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

static void wl_cursor_apply(void) {
    if (!g_wl.pointer || !g_wl.cursor_surf || !g_wl.pointer_entered) return;
    wl_pointer_set_cursor(g_wl.pointer, g_wl.pointer_serial,
                          g_wl.cursor_surf, g_wl.cursor_hot_x, g_wl.cursor_hot_y);
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


static EGLConfig         g_wl_egl_cfg;
static LunaSurface*      g_pointer_surface = NULL;   /* surface under the cursor */
static int surf_buffer_w(const LunaSurface* s) {
    int scale = s->buffer_scale > 0 ? s->buffer_scale : 1;
    return s->surf_w * scale;
}

static int surf_buffer_h(const LunaSurface* s) {
    int scale = s->buffer_scale > 0 ? s->buffer_scale : 1;
    return s->surf_h * scale;
}


#define LUNA_COMPACT_PAD_MIN 28
#define LUNA_COMPACT_PAD_MAX 96

static int wl_compact_pad(const LunaSurface* s) {
    int pad = LUNA_COMPACT_PAD_MIN;
    if (!s || s->input_root_idx < 0) return 48;
    LunaElement* e = luna_element_at(s->input_root_idx);
    if (!e || !e->has_shadow) return pad;
    for (int i = 0; i < e->shadow_count; i++) {
        const LunaShadow* sh = &e->shadows[i];
        if (sh->inset) continue;
        float ext = sh->blur * 1.55f + fabsf(sh->dx) + fabsf(sh->dy) +
                    (sh->spread > 0.0f ? sh->spread : 0.0f) + 4.0f;
        int p = (int)ceilf(ext);
        if (p > pad) pad = p;
    }
    if (pad > LUNA_COMPACT_PAD_MAX) pad = LUNA_COMPACT_PAD_MAX;
    return pad;
}

typedef struct {
    LunaSurface* surf;
    int active;
    int moved;
    float grab_x, grab_y; /* pointer offset inside the visible dialog */
    float box_x, box_y;   /* desired visible dialog origin */
    /* Surface origin frozen at press.  luna-compositor keeps the implicit
     * pointer grab relative to that origin even after set_margin moves the
     * layer, so converting with the live doc_x/doc_y double-counts motion and
     * makes the dialog jump ahead of the cursor. */
    float origin_x, origin_y;
} LunaWlDialogDrag;

static LunaWlDialogDrag g_wl_dialog_drag;

static int wl_dialog_dragging(void) {
    return g_wl_dialog_drag.active;
}

static int wl_point_in_element(int idx, double x, double y) {
    if (idx < 0 || !luna_element_visible(idx)) return 0;
    LunaElement* e = luna_element_at(idx);
    if (!e || e->w <= 0.0f || e->h <= 0.0f) return 0;
    return x >= e->x && x <= e->x + e->w &&
           y >= e->y && y <= e->y + e->h;
}

static int wl_point_in_id(const char* id, double x, double y) {
    return id && wl_point_in_element(luna_get_element_by_id(id), x, y);
}

static int wl_point_over_dialog_control(double x, double y) {
    for (int k = 0; k < SHELL_DLG_COUNT; k++) {
        const ShellDialogChrome* d = &g_shell_dialogs[k];
        const char* ids[3] = { d->close_id, d->min_id, d->max_id };
        for (int i = 0; i < 3; i++)
            if (ids[i] && wl_point_in_id(ids[i], x, y)) return 1;
    }
    return 0;
}

static const char* wl_compact_drag_id(const LunaSurface* s) {
    int kind = s ? shell_dialog_kind_by_name(s->name) : -1;
    return kind >= 0 ? g_shell_dialogs[kind].drag_id : NULL;
}

/* Synchronize a compact modeless Wayland layer with its actual Luna box.
 * Position-only changes do not invalidate the EGL buffer: layer-shell moves
 * the already-rendered pixels for us. */
static int surf_sync_compact_geometry(LunaSurface* s) {
    if (!s || !s->compact_to_input || s->input_root_idx < 0) return 0;
    LunaElement* box = luna_element_at(s->input_root_idx);
    if (!box || box->w <= 0.0f || box->h <= 0.0f) return 0;

    float left = box->x;
    float top = box->y;
    float right = box->x + box->w;
    float bottom = box->y + box->h;

    /* An embedded window menu is painted on this surface; grow the compact
     * buffer so the popup is not clipped when it hangs past the dialog edge. */
    if (g_win_menu_embedded && g_win_menu_idx >= 0 && is_shown(g_win_menu_idx) &&
        hit_inside(g_win_menu_idx, s->input_root_idx)) {
        LunaElement* menu = luna_element_at(g_win_menu_idx);
        if (menu && menu->w > 0.0f && menu->h > 0.0f) {
            if (menu->x < left) left = menu->x;
            if (menu->y < top) top = menu->y;
            if (menu->x + menu->w > right) right = menu->x + menu->w;
            if (menu->y + menu->h > bottom) bottom = menu->y + menu->h;
        }
    }

    int box_w = (int)ceilf(right - left);
    int box_h = (int)ceilf(bottom - top);
    if (box_w < 1) box_w = 1;
    if (box_h < 1) box_h = 1;

    /* Keep transparent breathing room around the real dialog so its
     * CSS box-shadow is not clipped by the compact Wayland buffer. */
    int pad = wl_compact_pad(s);
    int x = (int)floorf(left) - pad;
    int y = (int)floorf(top) - pad;
    int w = box_w + pad * 2;
    int h = box_h + pad * 2;

    int size_changed = s->fixed_w != w || s->fixed_h != h ||
                       s->surf_w != w || s->surf_h != h;
    int state_changed =
        s->anchor != (ZWLR_ANCHOR_TOP | ZWLR_ANCHOR_LEFT) ||
        s->margin_left != x || s->margin_top != y ||
        s->margin_right != 0 || s->margin_bottom != 0 ||
        s->fixed_w != w || s->fixed_h != h;

    s->anchor = ZWLR_ANCHOR_TOP | ZWLR_ANCHOR_LEFT;
    s->margin_left = x;
    s->margin_top = y;
    s->margin_right = 0;
    s->margin_bottom = 0;
    s->fixed_w = w;
    s->fixed_h = h;
    s->doc_x = (float)x;
    s->doc_y = (float)y;

    if (size_changed) {
        s->surf_w = w;
        s->surf_h = h;
        if (s->egl_win)
            wl_egl_window_resize(s->egl_win, surf_buffer_w(s), surf_buffer_h(s), 0, 0);
        s->full_damage = 1;
        if (s->was_shown)
            shell_request_repaint((int)(s - g_surfs));
    }

    if (state_changed && s->layer_surf && s->wl_surf) {
        zwlr_layer_surface_v1_set_anchor(s->layer_surf, s->anchor);
        zwlr_layer_surface_v1_set_size(s->layer_surf, (uint32_t)w, (uint32_t)h);
        zwlr_layer_surface_v1_set_margin(s->layer_surf, y, 0, 0, x);
        wl_surface_commit(s->wl_surf);
        if (g_wl.display) (void)wl_display_flush(g_wl.display);
    }
    return state_changed;
}

static int wl_native_dialog_drag_begin(void) {
    LunaSurface* s = g_pointer_surface;
    if (!s || !s->compact_to_input || !s->was_shown ||
        s->input_root_idx < 0 || g_wl_dialog_drag.active)
        return 0;

    LunaElement* box = luna_element_at(s->input_root_idx);
    if (!box) return 0;
    const char* drag_id = wl_compact_drag_id(s);
    int drag_idx = drag_id ? luna_get_element_by_id(drag_id) : -1;
    int hit = hit_test_at(g_wl.mouse_x, g_wl.mouse_y);
    /* Use the painted hit target, as the other dialog controls do.  Testing
     * only the drag div's geometry lets an oversized/late-layout drag box
     * steal presses from buttons layered above it (confirmation alerts were
     * especially prone to this while their compact surface was settling). */
    int hit_drag = hit_inside(hit, drag_idx);
    if (!hit_drag) {
        /* Some skins do not size the dedicated drag div.  Keep the top 52px
         * of the dialog draggable, but only when the actual hit is not a
         * descendant control layered over that fallback titlebar. */
        hit_drag =
            (hit == s->input_root_idx || hit == drag_idx) &&
            g_wl.mouse_x >= box->x && g_wl.mouse_x <= box->x + box->w &&
            g_wl.mouse_y >= box->y && g_wl.mouse_y <= box->y + 52.0f;
    }
    if (!hit_drag || wl_point_over_dialog_control(g_wl.mouse_x, g_wl.mouse_y))
        return 0;

    g_wl_dialog_drag.surf = s;
    g_wl_dialog_drag.active = 1;
    g_wl_dialog_drag.moved = 0;
    g_wl_dialog_drag.origin_x = s->doc_x;
    g_wl_dialog_drag.origin_y = s->doc_y;
    g_wl_dialog_drag.grab_x = (float)(g_wl.mouse_x - box->x);
    g_wl_dialog_drag.grab_y = (float)(g_wl.mouse_y - box->y);
    g_wl_dialog_drag.box_x = box->x;
    g_wl_dialog_drag.box_y = box->y;
    return 1;
}

/* Called from wl_pointer.frame so high-polling mice collapse to one layer
 * commit per Wayland input frame.  No Luna layout or OpenGL work happens. */
static void wl_native_dialog_drag_motion(void) {
    LunaSurface* s = g_wl_dialog_drag.surf;
    if (!g_wl_dialog_drag.active || !s || !s->layer_surf || !s->wl_surf) return;

    /* Pointer events stay relative to the grab-time surface origin (see
     * origin_x/y).  Do not add the live margin/doc origin here. */
    float global_x = g_wl_dialog_drag.origin_x + (float)g_wl.pointer_x;
    float global_y = g_wl_dialog_drag.origin_y + (float)g_wl.pointer_y;
    float box_x = global_x - g_wl_dialog_drag.grab_x;
    float box_y = global_y - g_wl_dialog_drag.grab_y;

    LunaElement* box = s->input_root_idx >= 0
        ? luna_element_at(s->input_root_idx) : NULL;
    int pad = wl_compact_pad(s);
    float box_w = box && box->w > 0.0f ? box->w : (float)(s->fixed_w - pad * 2);
    float box_h = box && box->h > 0.0f ? box->h : (float)(s->fixed_h - pad * 2);

    int out_w = g_surfs[LUNA_SURF_BG].surf_w;
    int out_h = g_surfs[LUNA_SURF_BG].surf_h;
    if (out_w > 0) {
        float max_x = (float)out_w - box_w;
        if (max_x < 0.0f) max_x = 0.0f;
        if (box_x < 0.0f) box_x = 0.0f;
        if (box_x > max_x) box_x = max_x;
    }
    if (out_h > 0) {
        float max_y = (float)out_h - box_h;
        if (max_y < 0.0f) max_y = 0.0f;
        if (box_y < 0.0f) box_y = 0.0f;
        if (box_y > max_y) box_y = max_y;
    }

    int x = (int)lroundf(box_x) - pad;
    int y = (int)lroundf(box_y) - pad;
    if (x == s->margin_left && y == s->margin_top) return;

    s->margin_left = x;
    s->margin_top = y;
    s->margin_right = 0;
    s->margin_bottom = 0;
    s->doc_x = (float)x;
    s->doc_y = (float)y;
    g_wl_dialog_drag.box_x = box_x;
    g_wl_dialog_drag.box_y = box_y;

    zwlr_layer_surface_v1_set_margin(s->layer_surf, y, 0, 0, x);
    wl_surface_commit(s->wl_surf);
    if (g_wl.display) (void)wl_display_flush(g_wl.display);
    g_wl_dialog_drag.moved = 1;
}

static void wl_native_dialog_drag_end(void) {
    if (!g_wl_dialog_drag.active) return;
    wl_native_dialog_drag_motion();

    LunaSurface* s = g_wl_dialog_drag.surf;
    int moved = g_wl_dialog_drag.moved;
    float box_x = g_wl_dialog_drag.box_x;
    float box_y = g_wl_dialog_drag.box_y;
    g_wl_dialog_drag.active = 0;
    g_wl_dialog_drag.moved = 0;
    g_wl_dialog_drag.surf = NULL;

    if (s && moved && s->input_root_idx >= 0) {
        LunaElement* box = luna_element_at(s->input_root_idx);
        if (box) {
            /* Commit the final DOM position once.  During the drag only the
             * native layer moved, so no flex/grid relayout ran per sample. */
            drag_set_screen_origin(box, box_x, box_y);
            luna_mark_layout_dirty();
            /* Re-align the buffer to the updated DOM.  Mid-drag paints were
             * suppressed because doc origin and sheet position diverge. */
            s->full_damage = 1;
            s->input_w = s->input_h = -1;
            shell_request_repaint((int)(s - g_surfs));
        }
    }
}

static void wl_forward_pointer_motion(void) {
    if (!g_pointer_surface) return;
    int changed = luna_mouse_move_changed(g_wl.mouse_x, g_wl.mouse_y);
    if (changed) shell_request_repaint((int)(g_pointer_surface - g_surfs));
}

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
    if (s->compact_to_input) {
        *x = s->doc_x;
        *y = s->doc_y;
        return;
    }

    LunaElement* e = luna_element_at(s->root_idx);
    if (!e) return;
    /* Layer surfaces are cropped from document space.  Include the root's
     * own CSS transform (notably dock left:50% + translateX(-50%)); otherwise
     * the native surface starts at the untransformed anchor and clips the
     * first launcher while pointer coordinates miss by half the dock width. */
    *x = e->x + e->cur_tx;
    *y = e->y + e->cur_ty;
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
    wl_forward_pointer_motion();
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
    shell_note_user_activity();
    if (g_wl_dialog_drag.active)
        return; /* wlp_frame coalesces the native surface move */
    wl_refresh_pointer_doc_pos();
    /* set_cursor is needed on enter and when the glyph changes, not for every
     * motion event.  Avoiding a Wayland request per sample removes pointer
     * latency on high-polling-rate mice. */
    wl_forward_pointer_motion();
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

    if (btn == LUNA_MOUSE_BUTTON_LEFT && action == LUNA_PRESS &&
        wl_native_dialog_drag_begin())
        return;
    if (btn == LUNA_MOUSE_BUTTON_LEFT && action == LUNA_RELEASE &&
        g_wl_dialog_drag.active) {
        wl_native_dialog_drag_end();
        wl_refresh_pointer_doc_pos();
        wl_forward_pointer_motion();
        return;
    }

    shell_mouse_button(btn, action, g_wl.mods, g_wl.mouse_x, g_wl.mouse_y);
}
static void wlp_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)t;
    double v = wl_fixed_to_double(value) / 10.0;
    shell_note_user_activity();
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) shell_scroll(0, -v);
    else shell_scroll(-v, 0);
}
/* wl_pointer v5+ events.  luna-compositor (and every modern compositor) sends
 * a frame after every motion/button/axis group.  We bind wl_seat at version 5,
 * so leaving these as NULL makes libwayland jump to a null fnptr the moment
 * the mouse moves — the classic "displays fine until the pointer moves" crash. */
static void wlp_frame(void* d, struct wl_pointer* p) {
    (void)d; (void)p;
    if (g_wl_dialog_drag.active)
        wl_native_dialog_drag_motion();
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
    if (pressed && !g_shortcut_capture) {
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
    /* Modeless dialogs use luna.dialog.* so the compositor can stack them with
     * xdg_toplevels (GTK apps) and list them in Alt+Tab.  Menus / launchpad /
     * toast stay on the always-on-top overlay path via "luna-shell". */
    const char* ns = "luna-shell";
    char dialog_ns[64];
    {
        int kind = shell_dialog_kind_by_name(s->name);
        if (s->compact_to_input && kind >= 0 &&
            (g_shell_dialogs[kind].flags & SHELL_DLG_F_DIALOG_NS)) {
            snprintf(dialog_ns, sizeof(dialog_ns), "luna.dialog.%s",
                     g_shell_dialogs[kind].name);
            ns = dialog_ns;
        }
    }
    s->layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_wl.layer_shell, s->wl_surf, NULL, s->layer, ns);
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
         * protocol error.  Launchpad uses EXCLUSIVE so Esc / typing work
         * without first clicking the overlay. */
        if (s->kbd_exclusive || g_wl.layer_shell_version < 4)
            keyboard_mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
        else
            keyboard_mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
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

    /* The interval is set once when each EGL surface is created.  Re-applying
     * it here used to enter the driver on every layer of every frame. */

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

static void surf_frame_done(void* data, struct wl_callback* cb,
                            uint32_t callback_data) {
    (void)callback_data;
    LunaSurface* s = (LunaSurface*)data;
    if (s->frame_cb == cb) s->frame_cb = NULL;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener g_surf_frame_listener = {
    .done = surf_frame_done,
};

static void surf_frame_cancel(LunaSurface* s) {
    if (!s->frame_cb) return;
    wl_callback_destroy(s->frame_cb);
    s->frame_cb = NULL;
}


/* ── Drop the EGL window of a hidden overlay ──
 * Each full-screen overlay costs a whole buffer chain (1920x1200x4 x N) on the
 * GPU, and the driver keeps them alive for as long as the EGLSurface exists.
 * Seven overlays that are hidden almost all of the time are not worth a
 * permanent ~200 MB, so their buffers are released on hide and reallocated on
 * the next open.  The layer surface itself stays around, so no re-negotiation
 * with the compositor is needed. */
static void surf_egl_destroy(LunaSurface* s) {
    surf_frame_cancel(s);
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
                    "  Set LUNA_DISABLE_DMABUF=1 to force the compositor to use wl_shm.\n");
    } else {
        fprintf(stderr, "[luna-shell/wl] Wayland connection lost during %s: %s\n",
                where, strerror(err));
    }
    g_should_close = 1;
    return 1;
}

/* ── Free a layer surface (used when unmap is permanent, e.g. on close) ── */
static void surf_destroy(LunaSurface* s) {
    surf_frame_cancel(s);
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

static void shell_dispatch_pending_right_click(void) {
    if (!g_shell_pending_right) return;
    g_shell_pending_right = 0;
    int hit = hit_test_at(g_shell_right_x, g_shell_right_y);

    /* Modeless dialog titlebars share the same window menu as compositor
     * windows (minimize / maximize / center / close). */
    {
        int dlg = shell_dialog_kind_from_hit(hit);
        if (dlg >= 0 &&
            shell_dialog_hit_is_titlebar(dlg, hit, g_shell_right_x, g_shell_right_y)) {
            shell_dialog_menu_open(dlg, (float)g_shell_right_x, (float)g_shell_right_y);
            luna_consume_pointer_event();
            shell_request_repaint(-1);
            return;
        }
    }

    for (int i = hit; i != -1; ) {
        LunaElement* e = luna_element_at(i);
        if (!e) break;
        const char* id = e->id;
        int handled = 0;
        if (id && !strncmp(id, "tray_", 5) && id[5] >= '0' && id[5] <= '9') {
            int slot = atoi(id + 5);
            if (slot >= 0 && slot < MAX_TRAY_SLOTS && g_tray_slot_key[slot][0]) {
                if (tray_key_is_window(g_tray_slot_key[slot])) on_tray_click(e);
                else order_menu_open(1, g_tray_slot_key[slot], i);
                handled = 1;
            }
        } else if (id && id[0] == 'm' && id[1] == 'b' && id[2] == '_' && mb_order_known(id)) {
            order_menu_open(0, id, i);
            handled = 1;
        } else if (id && !strncmp(id, "win_", 4) && id[4] >= '0' && id[4] <= '9') {
            on_win_click(e);
            handled = 1;
        }
        if (handled) {
            luna_consume_pointer_event();
            shell_request_repaint(-1);
            break;
        }
        i = e->parent_idx;
    }

    {   /* a widget: its own menu */
        int w = widget_of_hit(hit);
        if (w >= 0) {
            widget_menu_open(w, luna_get_element_by_id(g_wg_ids[w].id));
            luna_consume_pointer_event();
            shell_request_repaint(-1);
            return;
        }
    }

    /* Empty wallpaper: the desktop menu.  Anything inside a widget, the
     * menubar, the dock or another layer keeps its own behaviour. */
    {
        int on_wallpaper = 0, blocked = 0;
        for (int i = hit; i != -1; i = luna_element_at(i)->parent_idx) {
            const char* id = luna_element_at(i)->id;
            if (!strncmp(id, "widget_", 7) || !strcmp(id, "menubar") || !strcmp(id, "dock")) blocked = 1;
            if (!strcmp(id, "bg_layer") || !strcmp(id, "wallpaper")) on_wallpaper = 1;
        }
        if (on_wallpaper && !blocked) {
            desk_menu_open((float)g_shell_right_x, (float)g_shell_right_y);
            luna_consume_pointer_event();
            shell_request_repaint(-1);
        }
    }
}

static void shell_sync_dock_width(void) {
    if (g_backend != &g_wl_backend) return;
    LunaSurface* dock = &g_surfs[LUNA_SURF_DOCK];
    if (!dock->layer_surf) return;
    if (!g_dock_width_dirty && dock->fixed_w > 0) return;
    int di = luna_get_element_by_id("dock");
    LunaElement* de = di >= 0 ? luna_element_at(di) : NULL;
    if (!de || !is_shown(di)) return;
    /* luna-ui resolves width:fit-content.  The native layer only mirrors that
     * CSS border-box width; it must not maintain a second layout algorithm. */
    int dw = (int)ceilf(de->w);
    int max_w = (int)luna_window_width - 24;
    if (dw < 96) dw = 96;
    if (dw > max_w) dw = max_w;
    if (dw != dock->fixed_w) {
        dock->fixed_w = dw;
        surf_reconfigure_chrome(dock);
        shell_request_repaint(2);
        if (g_wl.display) wl_display_flush(g_wl.display);
    }
    g_dock_width_dirty = 0;
}

static void shell_runtime_maintenance(void) {
    shell_ensure_widget_css();
    shell_sync_tray_window_tips();
    shell_dispatch_pending_right_click();
}

#ifdef LUNA_BACKEND_X11
static void x11_tray_layout(void);
#endif

/* Apply menubar edge / dock visibility from the active skin.  Works for the
 * Wayland multi-surface backend (layer anchors) and for single-framebuffer
 * backends (DOM classes + CSS). */
/* ── Desktop widgets: hide / move individually ─────────────────────────────
 * Right-click a widget: Hide, Move… (then click where it should go), Reset Position.
 * Hidden widgets come back from the desktop menu.  Positions override the stylesheet
 * with an inline left/top. */

static void widgets_apply(void) {
    for (int i = 0; i < 5; i++) {
        int idx = luna_get_element_by_id(g_wg_ids[i].id);
        if (idx < 0) continue;
        set_hidden(idx, !g_settings.widgets_enabled || (g_settings.widget_hidden >> i) & 1);
        if (g_settings.widget_x[i] != WG_DEFAULT) {
            char st[96];
            snprintf(st, sizeof(st), "left:%dpx;top:%dpx;right:auto;", g_settings.widget_x[i], g_settings.widget_y[i]);
            luna_dom_set_attr(idx, "style", st);
        } else {
            luna_dom_set_attr(idx, "style", "");
        }
    }
    int show = luna_get_element_by_id("dm_wgshow");
    if (show >= 0) set_hidden(show, g_settings.widget_hidden == 0);
    luna_mark_layout_dirty();
    shell_request_repaint(-1);
}
static int widget_of_hit(int hit) {
    for (int i = hit; i != -1; i = luna_element_at(i)->parent_idx)
        for (int w = 0; w < 5; w++) if (!strcmp(luna_element_at(i)->id, g_wg_ids[w].id)) return w;
    return -1;
}
static void widget_menu_open(int w, int anchor) {
    if (g_tray_menu_idx < 0) return;
    dismiss_luna_menu(g_luna_menu_idx);
    dismiss_cc(g_cc_idx);
    dismiss_win_menu();
    dismiss_clip_menu();
    g_wg_menu = w;
    for (int i = 0; i < TRAY_MENU_SLOTS; i++) { char id[16]; snprintf(id, sizeof(id), "tw_%d", i); set_hidden(luna_get_element_by_id(id), 1); }
    static const char* const mv[] = { "tw_mv_l", "tw_mv_r", "tw_mv_x" };
    for (int i = 0; i < 3; i++) set_hidden(luna_get_element_by_id(mv[i]), 1);
    static const char* const wg[] = { "tw_wg_hide", "tw_wg_move", "tw_wg_reset" };
    for (int i = 0; i < 3; i++) set_hidden(luna_get_element_by_id(wg[i]), 0);
    int head = luna_get_element_by_id("tray_menu_title");
    if (head >= 0) luna_set_text(head, g_wg_ids[w].name);
    luna_mark_layout_dirty();
    set_hidden(g_tray_menu_idx, 0);
    position_menu_near(g_tray_menu_idx, anchor, luna_window_width - 240.0f);
}
static void widget_menu_pick(const char* id) {
    int w = g_wg_menu;
    if (w < 0 || w > 4) return;
    if (!strcmp(id, "tw_wg_hide")) { g_settings.widget_hidden |= 1 << w; widgets_apply(); settings_save(); }
    else if (!strcmp(id, "tw_wg_reset")) { g_settings.widget_x[w] = g_settings.widget_y[w] = WG_DEFAULT; widgets_apply(); settings_save(); }
    else if (!strcmp(id, "tw_wg_move")) { g_wg_moving = w; g_wg_arm = 1; toast_show("Move widget", "Click where it should go (Esc cancels).", 4.0); }
}
/* Left click while moving: put the widget's centre under the pointer. */
static int widget_move_click(void) {
    if (g_wg_moving < 0) return 0;
    if (g_wg_arm) { g_wg_arm = 0; return 0; }          /* the release of the "Move…" click itself */
    int w = g_wg_moving;
    g_wg_moving = -1;
    int idx = luna_get_element_by_id(g_wg_ids[w].id);
    LunaElement* e = idx >= 0 ? luna_element_at(idx) : NULL;
    float ew = e && e->w > 1 ? e->w : 220.0f, eh = e && e->h > 1 ? e->h : 110.0f;
    int x = (int)(g_luna_mx - ew * 0.5f), y = (int)(g_luna_my - eh * 0.5f);
    int maxx = (int)(luna_window_width - ew - 8.0f), maxy = (int)(luna_window_height - eh - 8.0f);
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    if (x < 8) x = 8;
    if (y < 40) y = 40;
    g_settings.widget_x[w] = x; g_settings.widget_y[w] = y;
    widgets_apply();
    settings_save();
    luna_consume_pointer_event();
    return 1;
}

static void apply_dock_visibility(void) {
    int dock_hidden = !g_settings.dock_enabled || g_chrome_dock_hidden;
    int dock_idx = luna_get_element_by_id("dock");
    if (dock_idx >= 0) {
        set_hidden(dock_idx, dock_hidden);
        /* A layer-shell surface is already centred by its BOTTOM-only anchor.
         * Lay the CSS root out at the origin of that compact surface; keeping
         * left:50% here applies the desktop centring a second time and makes
         * the first dock item start at the screen centre. */
        if (g_backend == &g_wl_backend)
            luna_update_classes(dock_idx, "dock_layer", "dock_layer");
        if (!dock_hidden)
            g_dock_width_dirty = 1;
    }
    widgets_apply();
    shell_sync_bg_input_region();
    luna_mark_layout_dirty();

    if (g_surfs[LUNA_SURF_DOCK].layer_surf) {
        LunaSurface* dock = &g_surfs[LUNA_SURF_DOCK];
        if (dock_hidden) {
            dock->exclusive_zone = 0;
            dock->margin_top = dock->margin_right = dock->margin_bottom = dock->margin_left = 0;
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
            dock->fixed_h = 84;
            g_dock_width_dirty = 1;
            surf_reconfigure_chrome(dock);
            if (dock->egl_surf == EGL_NO_SURFACE) {
                dock->was_shown = 1;
                surf_set_shown(dock, 1);
            }
        }
        shell_request_repaint(2);
        if (g_wl.display) wl_display_flush(g_wl.display);
    } else {
        shell_request_repaint(2);
    }
}

static void skin_apply_chrome(int skin_idx) {
    if (skin_idx < 0 || skin_idx >= g_skin_count) skin_idx = 0;
    const LunaSkin* skin = &g_skins[skin_idx];
    int edge = skin->menubar_edge;
    int height = skin->menubar_height > 0 ? skin->menubar_height : 32;

    g_chrome_menubar_edge = edge;
    g_chrome_menubar_height = height;
    g_chrome_dock_hidden = (skin->dock_mode == SKIN_DOCK_HIDDEN);

    int mb_idx = luna_get_element_by_id("menubar");
    if (mb_idx >= 0) {
        luna_update_classes(mb_idx, "edge_bottom",
                            edge == SKIN_EDGE_BOTTOM ? "edge_bottom" : NULL);
    }
    apply_dock_visibility();

    /* Wayland layer-shell surfaces are independent of CSS absolute insets. */
    if (g_surfs[LUNA_SURF_MENUBAR].layer_surf) {
        LunaSurface* mb = &g_surfs[LUNA_SURF_MENUBAR];
        LunaSurface* toast = NULL;
        for (int i = 0; i < LUNA_SURF_COUNT; i++)
            if (g_surfs[i].root_id && !strcmp(g_surfs[i].root_id, "toast")) {
                toast = &g_surfs[i];
                break;
            }

        /* The two-line top-bar tooltip is nested below its icon. Keep the exclusive
         * zone at the bar height, but give the transparent surface enough
         * pixels to render the popup without clipping it. */
        mb->fixed_h = edge == SKIN_EDGE_TOP ? height + 56 : height;
        mb->exclusive_zone = height;
        mb->fixed_w = 0;
        mb->margin_top = mb->margin_right = mb->margin_bottom = mb->margin_left = 0;
        if (edge == SKIN_EDGE_BOTTOM)
            mb->anchor = ZWLR_ANCHOR_BOTTOM | ZWLR_ANCHOR_LEFT | ZWLR_ANCHOR_RIGHT;
        else
            mb->anchor = ZWLR_ANCHOR_TOP | ZWLR_ANCHOR_LEFT | ZWLR_ANCHOR_RIGHT;
        surf_reconfigure_chrome(mb);

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
#ifdef LUNA_BACKEND_X11
    /* Keep the XEmbed host on the live menubar/taskbar edge.  GLFW / windowed
     * X11 never creates the host, so this is a no-op there. */
    x11_tray_layout();
#endif
}

static void surf_update_input_region(LunaSurface* s) {
    if (!s || !s->wl_surf || (s->is_overlay && !s->was_shown)) return;

    if (s == &g_surfs[LUNA_SURF_BG]) {
        static uint64_t last_sig = 0;
        static struct wl_surface* last_surface = NULL;
        static int have_sig = 0;
        uint64_t sig = 1469598103934665603ULL;
        int rects[4][4];
        int rect_count = 0;
        /* The whole wallpaper takes part so a right click on it can open the
         * desktop menu; windows and layers above still get their own input. */
        if (s->surf_w > 0 && s->surf_h > 0) {
            rects[0][0] = 0; rects[0][1] = 0; rects[0][2] = s->surf_w; rects[0][3] = s->surf_h;
            rect_count = 1;
            sig ^= ((uint64_t)s->surf_w << 20) ^ (uint64_t)s->surf_h;
            sig *= 1099511628211ULL;
        }
        const char* ids[] = { "widget_clock", "widget_stats", "widget_weather" };
        if (g_settings.widgets_enabled) {
            for (int i = 0; i < 3; i++) {
                int idx = luna_get_element_by_id(ids[i]);
                LunaElement* e = idx >= 0 ? luna_element_at(idx) : NULL;
                if (!e || e->w <= 0.0f || e->h <= 0.0f) continue;
                int x = (int)floorf(e->x - s->doc_x);
                int y = (int)floorf(e->y - s->doc_y);
                int w = (int)ceilf(e->w);
                int h = (int)ceilf(e->h);
                if (w <= 0 || h <= 0) continue;
                rects[rect_count][0] = x;
                rects[rect_count][1] = y;
                rects[rect_count][2] = w;
                rects[rect_count][3] = h;
                rect_count++;
                uint64_t vals[] = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
                for (int v = 0; v < 4; v++) {
                    sig ^= vals[v];
                    sig *= 1099511628211ULL;
                }
            }
        }
        sig ^= (uint64_t)rect_count;
        sig *= 1099511628211ULL;
        if (have_sig && s->wl_surf == last_surface && sig == last_sig) return;

        struct wl_region* region = wl_compositor_create_region(g_wl.compositor);
        if (!region) return;
        for (int i = 0; i < rect_count; i++)
            wl_region_add(region, rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        wl_surface_set_input_region(s->wl_surf, region);
        wl_region_destroy(region);
        wl_surface_commit(s->wl_surf);
        last_sig = sig;
        last_surface = s->wl_surf;
        have_sig = 1;
        return;
    }

    if (s->input_root_idx < 0) return;
    /* While the native layer is sliding under a frozen DOM box, box-doc
     * deltas are meaningless and would walk the input region off the chrome. */
    if (g_wl_dialog_drag.active && g_wl_dialog_drag.surf == s) return;
    LunaElement* box = luna_element_at(s->input_root_idx);
    if (!box) return;
    float left = box->x;
    float top = box->y;
    float right = box->x + box->w;
    float bottom = box->y + box->h;
    if (g_win_menu_embedded && g_win_menu_idx >= 0 && is_shown(g_win_menu_idx) &&
        hit_inside(g_win_menu_idx, s->input_root_idx)) {
        LunaElement* menu = luna_element_at(g_win_menu_idx);
        if (menu && menu->w > 0.0f && menu->h > 0.0f) {
            if (menu->x < left) left = menu->x;
            if (menu->y < top) top = menu->y;
            if (menu->x + menu->w > right) right = menu->x + menu->w;
            if (menu->y + menu->h > bottom) bottom = menu->y + menu->h;
        }
    }
    int x = (int)floorf(left - s->doc_x);
    int y = (int)floorf(top - s->doc_y);
    int w = (int)ceilf(right - left);
    int h = (int)ceilf(bottom - top);
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

static void shell_sync_bg_input_region(void) {
    if (g_backend != &g_wl_backend) return;
    surf_update_input_region(&g_surfs[LUNA_SURF_BG]);
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
            /* Throttle via poll() in the main loop, not EGL swap interval.
             * The compositor already paces scanout with asynchronous KMS page
             * flips; a non-zero interval here double-waits for vblank. */
            eglSwapInterval(g_wl.dpy, 0);
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
    /* Do not queue frames faster than the compositor can present them.  The
     * callback wakes wl_backend_poll_events(), after which the still-dirty
     * surface is rendered with the newest animation/input state. */
    if (s->frame_cb) return;
    /* Native dialog drag moves the layer via set_margin while the Luna sheet
     * stays put.  Re-rendering with the live doc origin crops the old sheet
     * out of the compact buffer and leaves a blank (or torn) dialog. */
    if (g_wl_dialog_drag.active && g_wl_dialog_drag.surf == s) {
        if (surf_idx >= 0 && surf_idx < LUNA_SURF_COUNT)
            g_surf_dirty &= ~(1u << surf_idx);
        return;
    }
    if (surf_idx >= 0 && surf_idx < LUNA_SURF_COUNT &&
        (g_surf_dirty & (1u << surf_idx)) == 0)
        return;
    /* Probe before touching GL: a clock/stats tick often marks the wallpaper
     * dirty even though every element's paint hash is unchanged.  Skipping the
     * clear/draw/swap removes the periodic hitch those timers produced. */
    if (!s->full_damage &&
        !(surf_idx == LUNA_SURF_BG && wp_glsl_animating()) &&
        !(surf_idx == g_saver_surf && g_saver_active) &&
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
    s->frame_cb = wl_surface_frame(s->wl_surf);
    if (!s->frame_cb) {
        fprintf(stderr, "[luna-shell/wl] cannot allocate frame callback for '%s'\n",
                s->name);
        g_should_close = 1;
        return;
    }
    wl_callback_add_listener(s->frame_cb, &g_surf_frame_listener, s);
    if (!eglMakeCurrent(g_wl.dpy, s->egl_surf, s->egl_surf, g_wl.ctx)) {
        fprintf(stderr, "[luna-shell/wl] eglMakeCurrent failed for '%s' (EGL 0x%x)\n",
                s->name, eglGetError());
        surf_frame_cancel(s);
        g_should_close = 1;
        return;
    }
    /* The layer-shell configure size is logical.  Render into a buffer scaled
     * to the output so text and vector edges land on native display pixels. */
    int fw = surf_buffer_w(s), fh = surf_buffer_h(s);
    float logical_w = (float)s->surf_w;
    float logical_h = (float)s->surf_h;
    glViewport(0, 0, fw, fh);

    /* The paint clip only culls elements; it does not constrain all their
     * GL writes. Repainting an intersecting background/blur outside the
     * cleared rectangle corrupts retained pixels. Recycled EGL buffers also
     * cannot be repaired with a truncated buffer-age history. Keep the clean
     * frame probe above, but repaint and advertise the entire changed surface. */
    luna_redraw_set_paint_clip(0, 0, 0, 0);
    glDisable(GL_SCISSOR_TEST);
    if (surf_idx == LUNA_SURF_BG || surf_idx == g_saver_surf)
        glClearColor(0.02f, 0.02f, 0.06f, 1.0f);
    else
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    s->full_damage = 1;
    int glsl_drawn = 0;
    if (surf_idx == LUNA_SURF_BG && g_glsl.path[0])
        glsl_drawn = wp_glsl_draw(&g_glsl, fw, fh,
                                  g_settings.wallpaper_fps > 0 ? wp_motion_speed() : 0.0);
    else if (surf_idx == g_saver_surf && g_saver_active)
        glsl_drawn = wp_glsl_draw(&g_saver_glsl, fw, fh, 1.0);
    if (glsl_drawn) luna_set_preserve_backdrop(1);

    if (s->root_idx >= 0) {
        luna_render_region(s->root_idx, fw, fh,
                           s->doc_x, s->doc_y,
                           logical_w, logical_h);
    } else {
        /* bg: no root filter, full-document render */
        luna_render(fw, fh);
    }
    if (glsl_drawn) luna_set_preserve_backdrop(0);
    luna_redraw_set_paint_clip(0, 0, 0, 0);

    /* llvmpipe / mesa_glthread can still be writing the color buffer when
     * eglSwapBuffers returns on software GL.  Hardware GL can pipeline. */
    shell_gl_sync_before_swap();
    if (!surf_swap(s)) {
        EGLint err = eglGetError();
        surf_frame_cancel(s);
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
    /* Update geometry from Luna layout.  Compact modeless dialogs move their
     * real layer-surface instead of carrying a full-output transparent buffer. */
    for (int i = 0; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        if (s->root_idx < 0) continue;
        if (s->compact_to_input && s->input_root_idx >= 0) {
            if (!(g_wl_dialog_drag.active && g_wl_dialog_drag.surf == s))
                (void)surf_sync_compact_geometry(s);
        } else {
            wl_surface_doc_origin(s, &s->doc_x, &s->doc_y);
        }
        /* Chrome may paint outside its interactive box (the menubar tooltip
         * is the important case).  Leave that transparent overflow click-
         * through so an IME candidate window can still be dismissed. */
        if (!s->is_overlay && s->input_root_idx >= 0)
            surf_update_input_region(s);
    }

    /* Handle overlay visibility changes */
    for (int i = LUNA_SURF_FIRST_OL; i < LUNA_SURF_COUNT; i++) {
        LunaSurface* s = &g_surfs[i];
        int shown = (s->root_idx >= 0) ? is_shown(s->root_idx) : 0;
        /* Dialog titlebar menus are reparented onto the dialog surface; keep
         * the dedicated win_menu overlay unmapped so it cannot stack behind. */
        if (shown && g_win_menu_embedded && s->root_id &&
            !strcmp(s->root_id, "win_menu"))
            shown = 0;
        if (shown != s->was_shown) {
            s->was_shown = shown;
            if (shown) s->input_w = s->input_h = -1;
            surf_set_shown(s, shown);
            if (shown) shell_request_repaint(i);
        }
        if (shown) surf_update_input_region(s);
    }
    shell_sync_bg_input_region();
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
    int action_fd = shell_action_poll_fd();
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

        struct pollfd pfds[4] = {
            { .fd = wl_display_get_fd(g_wl.display), .events = POLLIN },
            { .fd = state_fd, .events = POLLIN },
            { .fd = async_fd, .events = POLLIN },
            { .fd = action_fd, .events = POLLIN },
        };
        int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
        int pr;
        do { pr = poll(pfds, 4, timeout_ms); }
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
        int action_ready = pr > 0 && action_fd >= 0 &&
                           (pfds[3].revents & POLLIN);

        if (action_ready)
            (void)shell_action_drain();
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

        if (action_ready) {
            if (wl_display_dispatch_pending(g_wl.display) < 0 ||
                wl_display_flush(g_wl.display) < 0)
                wl_check_error("event dispatch");
            return;
        }
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

static int wl_backend_present_busy(void) { return 0; }

static const LunaBackend g_wl_backend = {
    .start        = wl_backend_start,
    .get_fb_size  = wl_backend_get_fb_size,
    .swap_buffers = wl_backend_swap_buffers,
    .poll_events  = wl_backend_poll_events,
    .present_busy = wl_backend_present_busy,
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
    if (!g_x11.display || !g_x11.tray_host) return;
    const int host_w = X11_TRAY_MAX_ICONS * (X11_TRAY_ICON_SIZE + 2) + 4;
    const int host_h = X11_TRAY_ICON_SIZE + 2;
    int x = g_x11.width - host_w - 8;
    if (x < 0) x = 0;
    int bar = g_chrome_menubar_height > 0 ? g_chrome_menubar_height : 32;
    int y;
    if (g_chrome_menubar_edge == SKIN_EDGE_BOTTOM) {
        y = g_x11.height - bar + (bar - host_h) / 2;
        if (y < 0) y = 0;
    } else {
        y = (bar - host_h) / 2;
        if (y < 2) y = 2;
    }
    /* This is a separate override-redirect top-level, never a child of the
     * full-screen EGL window.  Some legacy XEmbed clients inherit their
     * parent geometry while docking; keeping the tray host independent means
     * they can never see or occupy the desktop-sized shell surface. */
    XMoveResizeWindow(g_x11.display, g_x11.tray_host, x, y,
                      host_w, host_h);
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
    /* XEmbed system tray is only for a real desktop session (--desktop /
     * luna-session on X11).  A GLFW or windowed X11 preview must not claim
     * _NET_SYSTEM_TRAY_S0 from the host panel.  luna-session itself is
     * Wayland, so the native #tray_area path is what paints icons there. */
    if (g_desktop_mode) {
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
    }
    XMapWindow(g_x11.display, g_x11.window);
    /* Map after the desktop so this small root-level tray stays above the
     * fullscreen EGL surface instead of being obscured by it. */
    if (g_x11.tray_host) {
        XMapRaised(g_x11.display, g_x11.tray_host);
        x11_tray_claim();
    }
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
            if (luna_mouse_move_changed(g_x11.mouse_x, g_x11.mouse_y))
                g_frame_dirty = 1;
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
                if (action == LUNA_PRESS) shell_scroll(0,  1.0);
            } else if (ev.xbutton.button == Button5) {
                if (action == LUNA_PRESS) shell_scroll(0, -1.0);
            } else {
                int btn;
                switch (ev.xbutton.button) {
                    case Button1: btn = 0; break;
                    case Button3: btn = 1; break;
                    case Button2: btn = 2; break;
                    default:      btn = 3; break;
                }
                shell_mouse_button(btn, action, g_x11.mods, g_x11.mouse_x, g_x11.mouse_y);
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
            if (pressed && !g_shortcut_capture) {
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
        int action_fd = shell_action_poll_fd();
        double outer_deadline = plat_time() +
            (double)g_single_poll_timeout_ms / 1000.0;
        for (;;) {
            struct pollfd pfds[4] = {
                { .fd = ConnectionNumber(g_x11.display), .events = POLLIN },
                { .fd = state_fd, .events = POLLIN },
                { .fd = async_fd, .events = POLLIN },
                { .fd = action_fd, .events = POLLIN },
            };
            int timeout_ms = shell_poll_coalesced_timeout_ms(outer_deadline);
            int pr;
            do { pr = poll(pfds, 4, timeout_ms); }
            while (pr < 0 && errno == EINTR);

            int x_ready = pr > 0 && (pfds[0].revents & POLLIN);
            int state_ready = pr > 0 && state_fd >= 0 &&
                              (pfds[1].revents & POLLIN);
            int async_ready = pr > 0 && async_fd >= 0 &&
                              (pfds[2].revents & POLLIN);
            int action_ready = pr > 0 && action_fd >= 0 &&
                               (pfds[3].revents & POLLIN);
            if (action_ready)
                (void)shell_action_drain();
            if (state_ready) {
                (void)shell_state_watch_drain();
                if (g_interaction_busy)
                    outer_deadline = plat_time() + 1.0;
            }
            if (async_ready) shell_async_drain();

            int async_deferred = async_ready && g_interaction_busy;
            if (action_ready || pr <= 0 || x_ready ||
                (async_ready && !async_deferred))
                break;
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

static int x11_backend_present_busy(void) { return 0; }

static const LunaBackend g_x11_backend = {
    .start        = x11_backend_start,
    .get_fb_size  = x11_backend_get_fb_size,
    .swap_buffers = x11_backend_swap_buffers,
    .poll_events  = x11_backend_poll_events,
    .present_busy = x11_backend_present_busy,
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
        else if (!strcmp(argv[i], "--wallpaper") && i + 1 < argc)
            g_wallpaper_override = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            luna_request_screenshot(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "luna-shell " LUNA_SHELL_VERSION " — Luna Desktop shell\n"
                "usage: luna-shell [--desktop] [--fullscreen] [--size WxH]\n"
                "                  [--skin NAME] [--layout PATH] [--css PATH]\n"
                "                  [--wallpaper IMAGE]\n"
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
                "  keys: Super — Launchpad (configurable), Esc — close overlay, Alt+Tab — switch apps\n"
                "        Super+Arrows — tile/max/min, Super+D — show desktop, Alt+F4 — close window\n"
                "        Super+, — Settings, Print Screen — screenshot (both configurable)\n"
                "  settings: $XDG_CONFIG_HOME/luna-shell/settings.conf\n"
                "  session:  $XDG_STATE_HOME/luna-shell/session\n");
            exit(0);
        }
    }
    if (!g_layout_path) g_layout_path = getenv("LUNA_DESKTOP_LAYOUT");
    if (!g_css_path)    g_css_path    = getenv("LUNA_DESKTOP_CSS");
    if (!g_skin_override) g_skin_override = getenv("LUNA_DESKTOP_SKIN");
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
    alarms_load();
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
    shell_action_init();
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
        .request_redraw  = luna_app_request_redraw,
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
        loaded = shell_load_layout_file(g_layout_path);
    }
    if (!loaded) {
        fprintf(stderr, "[luna-shell] using embedded layout (no luna-desktop/shell/luna-shell.html)\n");
        luna_set_html_base_dir("skins/default");
        shell_parse_layout_html(default_html);
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
    shell_ensure_widget_css();
    /* The shell-owned widget sheet is deliberately appended after the
     * document cascade so its runtime bindings remain available.  Re-apply
     * the selected skin afterwards: otherwise equal-specificity widget rules
     * silently win over skin overrides (notably XP's paper widgets, bars and
     * colours), while a browser preview—where the skin link is last—looks
     * correct. */
    if (startup_skin > 0 && g_skins[startup_skin].css[0])
        luna_load_css_file(g_skins[startup_skin].css);
    shell_trim_heap();
    luna_inject_body_background();
    register_handlers();
    luna_wire_onclick_handlers();
    bind_indices();
    saver_bind();
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
    char weather_cache[512] = "";
    {
        char dir[PATH_MAX];
        if (xdg_app_dir(dir, sizeof(dir), g_xdg.cache_home, "luna-shell", 1))
            path_join2(weather_cache, sizeof(weather_cache), dir, "weather.cache");
    }
    snprintf(weather_agent, sizeof(weather_agent), "luna-shell/%s", LUNA_SHELL_VERSION);
    LunaWeatherConfig weather_cfg = {
        .initial_city = NULL,
        .user_agent = weather_agent,
        .refresh_interval_sec = WEATHER_REFRESH_SEC,
        .cache_path = weather_cache[0] ? weather_cache : NULL,
        .notify = shell_async_notify,
        .notify_user = NULL,
        .child_lock = shell_child_reaper_lock,
        .child_unlock = shell_child_reaper_unlock,
        .child_user = NULL,
    };
    g_weather_worker_ready = luna_weather_init(&weather_cfg);
    if (!g_weather_worker_ready)
        fprintf(stderr, "[luna-shell] warning: weather worker unavailable\n");
    else
        weather_tick();

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

    if (g_desktop_mode) {
        LunaSniConfig sni_cfg = {
            .notify = shell_async_notify,
            .notify_user = NULL,
        };
        if (!luna_sni_init(&sni_cfg))
            fprintf(stderr, "[luna-shell] warning: StatusNotifier host unavailable\n");
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
    scale_slider_init(&g_scale_dpi_slider, &g_scale_dpi_level, &g_scale_dpi_written,
                      &g_scale_dpi_apply, scale_write_dpi,
                      "scale_dpi_thumb", "scale_dpi_fill", "scale_dpi_track");
    scale_slider_init(&g_scale_gdk_slider, &g_scale_gdk_level, &g_scale_gdk_written,
                      &g_scale_gdk_apply, scale_write_gdk,
                      "scale_gdk_thumb", "scale_gdk_fill", "scale_gdk_track");
    scale_slider_init(&g_scale_qt_slider, &g_scale_qt_level, &g_scale_qt_written,
                      &g_scale_qt_apply, scale_write_qt,
                      "scale_qt_thumb", "scale_qt_fill", "scale_qt_track");
    scale_slider_init(&g_scale_cur_slider, &g_scale_cur_level, &g_scale_cur_written,
                      &g_scale_cur_apply, scale_write_cur,
                      "scale_cur_thumb", "scale_cur_fill", "scale_cur_track");
    settings_sync_scale_sliders();

    /* Test aid: LUNA_TEST_SHOT_AFTER=SECONDS takes one desktop screenshot
     * (into LUNA_SCREENSHOT_DIR) after that delay. */
    if (getenv("LUNA_TEST_SHOT_AFTER"))
        g_desk_shot_at = plat_time() + atof(getenv("LUNA_TEST_SHOT_AFTER"));

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
        shell_cmd_flush();
        int ui_dragging = luna_pointer_dragging() ||
            (g_backend == &g_wl_backend && wl_dialog_dragging());
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
            stats_tick();
            mbgraph_tick();
            airplay_tick();
            airdrop_tick();
            if (g_desk_shot_at > 0.0 && g_now >= g_desk_shot_at) {
                g_desk_shot_at = 0.0;
                static int test_bg_armed;
                if (getenv("LUNA_TEST_SHOT_AFTER") && !test_bg_armed) {
                    /* test aid: the capture reads the surface rendered last,
                     * so repaint only the background, then shoot. */
                    test_bg_armed = 1;
                    const char* sv = getenv("LUNA_TEST_SHOT_SURF");   /* surface index, default bg */
                    int ts = sv ? atoi(sv) : LUNA_SURF_BG;
                    if (ts < 0 || ts >= LUNA_SURF_COUNT) ts = LUNA_SURF_BG;
                    g_surfs[ts].full_damage = 1;
                    g_surf_dirty = 1u << ts;
                    g_desk_shot_at = g_now + 0.4;
                } else {
                    take_timestamped_screenshot();
                }
            }
            wallpaper_pick_tick();
            wifi_tick();
            weather_tick();
            alarm_tick();
            stopwatch_tick();
            update_launchpad_filter();
            wm_settings_retry_tick();
            session_restore_tick();
            session_autosave_tick();
        }
        slider_tick_when_needed(&bright_slider);
        slider_tick_when_needed(&vol_slider);
        slider_tick_when_needed(&g_scale_dpi_slider);
        slider_tick_when_needed(&g_scale_gdk_slider);
        slider_tick_when_needed(&g_scale_qt_slider);
        slider_tick_when_needed(&g_scale_cur_slider);
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
            /* Class/visibility changes above mark layout dirty.  Mirror the
             * CSS fit-content width only after luna_needs_redraw_mask() has
             * completed that layout pass; reading it from maintenance used
             * the old zero width and collapsed the native dock to 96px. */
            shell_sync_dock_width();
            int settling = 0;
            for (int i = 0; i < LUNA_SURF_COUNT; i++)
                if ((redraw_flags[i] & (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) ==
                    (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) settling = 1;
            g_bg_animated =
                (redraw_flags[LUNA_SURF_BG] & LUNA_REDRAW_ANIM) != 0 ||
                wp_glsl_animating();
            if (g_saver_active) {   /* full-rate shader; the bars sleep behind it */
                g_bg_animated = 0;
                settling = 1;
                g_surf_dirty |= 1u << g_saver_surf;
            }
            wl_surfs_update();
            shell_x11_tray_report();
            /* Wallpaper aurora/stars: only damage the bg layer when the
             * desktop is empty. Continuous full-screen commits under open
             * windows force a whole-desktop re-composite every tick → flicker
             * and starve client frame callbacks.  Twelve hertz keeps the idle
             * animation smooth enough without repainting beneath
             * application windows. */
            int desktop_busy = shell_desktop_busy();
            int bg_ticking = g_bg_animated && !desktop_busy && !interaction_busy;
            if (bg_ticking &&
                g_now - g_last_bg_paint >= bg_frame_sec())
                shell_request_repaint(LUNA_SURF_BG);
            /* Per-surface settling — dock mag must not redraw the menubar.
             * One pass answers for every surface at once; asking per surface
             * meant a full element scan (with a parent-chain walk per element)
             * a dozen times on every single frame, idle or not. */
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                if (redraw_flags[i] & LUNA_REDRAW_PAINT)
                    shell_request_repaint(i);
            }
            /* Wait for the next frame slot (or input) before touching GL.
             * Rendering first and sleeping afterward let real GPUs finish an
             * entire frame and then idle, which reads as a regular hitch. */
            g_wl_poll_timeout_ms = settling
                ? shell_wait_timeout_ms(17, g_now + 1.0 / 60.0)
                : shell_wait_timeout_ms(1000,
                      bg_ticking
                          ? g_last_bg_paint + bg_frame_sec()
                          : 0.0);
            /* Keep `last` at the top-of-loop stamp: the next iteration's dt
             * must span the poll wait too, or animations advance only by the
             * render time and run ~10-20x slower than wall clock. */
            g_backend->poll_events();
            g_now = plat_time();
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                wl_surf_render(&g_surfs[i], i);
            }
        } else {
            int redraw_flags = luna_needs_redraw(g_now, dt);
            int settling =
                (redraw_flags & (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT)) ==
                (LUNA_REDRAW_ANIM | LUNA_REDRAW_PAINT);
            g_bg_animated = (redraw_flags & LUNA_REDRAW_ANIM) != 0 ||
                            wp_glsl_animating();
            double idle_frame_deadline = g_bg_animated && !interaction_busy
                ? g_last_bg_paint + LUNA_SINGLE_BG_FRAME_SEC
                : 0.0;
            if (idle_frame_deadline > 0.0 && g_now >= idle_frame_deadline)
                g_frame_dirty = 1;
            int want_frame = g_frame_dirty || (redraw_flags & LUNA_REDRAW_PAINT);
            if (g_backend->present_busy()) {
                g_single_poll_timeout_ms = shell_wait_timeout_ms(17, g_now + 1.0 / 60.0);
            } else {
                g_single_poll_timeout_ms = want_frame
                    ? (settling ? shell_wait_timeout_ms(17, g_now + 1.0 / 60.0) : 0)
                    : shell_wait_timeout_ms(1000, idle_frame_deadline);
            }
            /* Keep `last` at the top-of-loop stamp: the next iteration's dt
             * must span the poll wait too, or animations advance only by the
             * render time and run ~10-20x slower than wall clock. */
            g_backend->poll_events();
            g_now = plat_time();
            /* Input callbacks above can invalidate the frame during poll. */
            want_frame |= g_frame_dirty;
            if (want_frame && !g_backend->present_busy()) {
                glViewport(0, 0, fbw, fbh);
                glClearColor(0.04f, 0.05f, 0.12f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                int glsl_drawn = g_glsl.path[0] &&
                    wp_glsl_draw(&g_glsl, fbw, fbh,
                                 g_settings.wallpaper_fps > 0 ? wp_motion_speed() : 0.0);
                if (glsl_drawn) luna_set_preserve_backdrop(1);
                luna_render(fbw, fbh);
                if (glsl_drawn) luna_set_preserve_backdrop(0);
                luna_flush_pending_screenshot();
                g_backend->swap_buffers();
                g_frame_dirty = 0;
                g_last_bg_paint = g_now;
                finish_screenshot_notification();
            }
        }
        /* Wayland surfaces perform their own swap in wl_surf_render(); keep
         * the existing post-render capture point for that backend. */
        if (g_backend == &g_wl_backend) {
            luna_flush_pending_screenshot();
            finish_screenshot_notification();
        }
    }
    session_save();
    luna_sni_shutdown();
    luna_weather_shutdown();
    luna_bluetooth_shutdown();
    luna_ethernet_shutdown();
    luna_wifi_shutdown();
    airplay_shutdown();
    airdrop_shutdown();
    luna_monitor_shutdown();
    shell_state_watch_close();
    shell_action_close();
    if (g_cmd_fd >= 0) close(g_cmd_fd);
    shell_async_close();
    luna_shutdown();
    g_backend->terminate();
    return 0;
}
