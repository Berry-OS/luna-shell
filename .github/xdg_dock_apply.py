from pathlib import Path


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

root = Path('.')
c_path = root / 'ui/luna-shell.c'
layout_path = root / 'skins/default/layout.html'
css_path = root / 'skins/default/style.css'

c = c_path.read_text()
layout = layout_path.read_text()
css = css_path.read_text()

old_struct = '''/* Extra Launchpad tiles filled from XDG .desktop application entries. */
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
'''
new_struct = '''/* Extra Launchpad/Dock applications filled from XDG .desktop entries. */
#define MAX_LP_XDG 48
#define MAX_DOCK_XDG 8
#define MAX_DOCK_PINNED 32

typedef enum {
    LUNA_ICON_NONE = 0,
    LUNA_ICON_RASTER,
    LUNA_ICON_SVG
} LunaIconType;

typedef struct {
    char id[NAME_MAX + 1];
    char name[256];
    char path[PATH_MAX];
    char icon_name[256];
    char icon_path[PATH_MAX];
    char startup_wm_class[256];
    LunaIconType icon_type;
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
'''
c = replace_once(c, old_struct, new_struct, 'xdg structs')

old_desktop_struct = '''typedef struct {
    char path[PATH_MAX];
    char name[256];
    char icon[256];
    char exec[2048];
    char try_exec[PATH_MAX];
    char only_show_in[512];
    char not_show_in[512];
    int hidden;
'''
new_desktop_struct = '''typedef struct {
    char path[PATH_MAX];
    char name[256];
    char icon[256];
    char exec[2048];
    char try_exec[PATH_MAX];
    char startup_wm_class[256];
    char only_show_in[512];
    char not_show_in[512];
    int hidden;
'''
c = replace_once(c, old_desktop_struct, new_desktop_struct, 'desktop struct')

old_parser = '''        else if (!strcmp(key, "TryExec"))
            snprintf(entry->try_exec, sizeof(entry->try_exec), "%s", value);
        else if (!strcmp(key, "OnlyShowIn"))
'''
new_parser = '''        else if (!strcmp(key, "TryExec"))
            snprintf(entry->try_exec, sizeof(entry->try_exec), "%s", value);
        else if (!strcmp(key, "StartupWMClass"))
            snprintf(entry->startup_wm_class, sizeof(entry->startup_wm_class), "%s", value);
        else if (!strcmp(key, "OnlyShowIn"))
'''
c = replace_once(c, old_parser, new_parser, 'StartupWMClass parser')

icon_code = r'''
static int luna_str_ends_ci(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && !strcasecmp(s + sl - xl, suffix);
}

static LunaIconType luna_icon_type_from_path(const char* path) {
    if (!path || !*path) return LUNA_ICON_NONE;
    if (luna_str_ends_ci(path, ".png") || luna_str_ends_ci(path, ".jpg") ||
        luna_str_ends_ci(path, ".jpeg") || luna_str_ends_ci(path, ".bmp") ||
        luna_str_ends_ci(path, ".tga"))
        return LUNA_ICON_RASTER;
    if (luna_str_ends_ci(path, ".svg") || luna_str_ends_ci(path, ".svgz"))
        return LUNA_ICON_SVG;
    return LUNA_ICON_NONE;
}

static int luna_icon_accept_path(const char* path, char* out, size_t out_n,
                                 LunaIconType* type) {
    if (!path || !*path || access(path, R_OK) != 0) return 0;
    LunaIconType t = luna_icon_type_from_path(path);
    if (t == LUNA_ICON_NONE) return 0;
    snprintf(out, out_n, "%s", path);
    if (type) *type = t;
    return 1;
}

static void luna_icon_normalize_name(const char* src, char* dst, size_t dst_n) {
    if (!dst || dst_n == 0) return;
    dst[0] = 0;
    if (!src || !*src) return;
    snprintf(dst, dst_n, "%s", src);
    static const char* exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".svg", ".svgz", NULL };
    for (int i = 0; exts[i]; i++) {
        size_t dl = strlen(dst), el = strlen(exts[i]);
        if (dl > el && !strcasecmp(dst + dl - el, exts[i])) {
            dst[dl - el] = 0;
            return;
        }
    }
}

static int luna_icon_try_pixmap_dir(const char* dir, const char* name,
                                    char* out, size_t out_n, LunaIconType* type) {
    static const char* exts[] = { "", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".svg", ".svgz", NULL };
    for (int i = 0; exts[i]; i++) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s%s", dir, name, exts[i]);
        if (n > 0 && (size_t)n < sizeof(path) &&
            luna_icon_accept_path(path, out, out_n, type)) return 1;
    }
    return 0;
}

static int luna_icon_try_theme(const char* root, const char* theme, const char* name,
                               char* out, size_t out_n, LunaIconType* type) {
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
                    luna_icon_accept_path(path, out, out_n, type)) return 1;
            }
        }
    }
    static const char* svg_exts[] = { ".svg", ".svgz", NULL };
    for (int c = 0; contexts[c]; c++) {
        for (int e = 0; svg_exts[e]; e++) {
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s/scalable/%s/%s%s",
                             root, theme, contexts[c], name, svg_exts[e]);
            if (n > 0 && (size_t)n < sizeof(path) &&
                luna_icon_accept_path(path, out, out_n, type)) return 1;
        }
    }
    return 0;
}

static int luna_app_icon_resolve(const char* icon, char* out, size_t out_n,
                                 LunaIconType* type) {
    if (!out || out_n == 0) return 0;
    out[0] = 0;
    if (type) *type = LUNA_ICON_NONE;
    if (!icon || !*icon) return 0;
    if (path_is_absolute(icon))
        return luna_icon_accept_path(icon, out, out_n, type);

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
            if (roots[r][0] && luna_icon_try_theme(roots[r], themes[t], name, out, out_n, type))
                return 1;

    char user_pixmaps[PATH_MAX] = "";
    (void)path_join2(user_pixmaps, sizeof(user_pixmaps), g_xdg.data_home, "pixmaps");
    const char* pixmaps[] = { user_pixmaps, "/usr/local/share/pixmaps", "/usr/share/pixmaps", NULL };
    for (int i = 0; pixmaps[i]; i++) {
        if (!pixmaps[i][0]) continue;
        if (luna_icon_try_pixmap_dir(pixmaps[i], icon, out, out_n, type)) return 1;
        if (strcmp(icon, name) && luna_icon_try_pixmap_dir(pixmaps[i], name, out, out_n, type)) return 1;
    }
    return 0;
}

static void xdg_app_icon_apply(int element_idx, const LunaLpXdgApp* app) {
    if (element_idx < 0) return;
    luna_set_background_image(element_idx, NULL);
    luna_set_text(element_idx, "");
    if (!app) return;
    if (app->icon_type == LUNA_ICON_RASTER && app->icon_path[0]) {
        luna_set_background_image(element_idx, app->icon_path);
        return;
    }
    /* SVG paths are deliberately retained.  Future luna-svg integration only
     * needs to replace this branch; desktop parsing/theme lookup stays shared. */
    luna_set_text(element_idx, "\uf2d0");
}

'''
c = replace_once(c, 'static void lp_xdg_try_add(const char* path, const char* desktop_id,\n', icon_code + 'static void lp_xdg_try_add(const char* path, const char* desktop_id,\n', 'icon resolver insertion')

old_add = '''    LunaLpXdgApp* slot = &g_lp_xdg[g_lp_xdg_count++];
    snprintf(slot->id, sizeof(slot->id), "%s", desktop_id);
    snprintf(slot->name, sizeof(slot->name), "%s", entry.name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
'''
new_add = '''    LunaLpXdgApp* slot = &g_lp_xdg[g_lp_xdg_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->id, sizeof(slot->id), "%s", desktop_id);
    snprintf(slot->name, sizeof(slot->name), "%s", entry.name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    snprintf(slot->icon_name, sizeof(slot->icon_name), "%s", entry.icon);
    snprintf(slot->startup_wm_class, sizeof(slot->startup_wm_class), "%s",
             entry.startup_wm_class);
    (void)luna_app_icon_resolve(entry.icon, slot->icon_path,
                                sizeof(slot->icon_path), &slot->icon_type);
    fprintf(stderr, "[luna-shell/icon] %s: Icon=%s -> %s (%s)\\n",
            slot->name, slot->icon_name[0] ? slot->icon_name : "(none)",
            slot->icon_path[0] ? slot->icon_path : "(not found)",
            slot->icon_type == LUNA_ICON_RASTER ? "raster" :
            slot->icon_type == LUNA_ICON_SVG ? "svg" : "none");
'''
c = replace_once(c, old_add, new_add, 'xdg app add')

start = c.index('static void launchpad_populate_xdg(void) {')
end = c.index('static void on_launchpad_open(LunaElement* e) {', start)
new_launchpad_and_dock = r'''static void launchpad_populate_xdg(void) {
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

'''
c = c[:start] + new_launchpad_and_dock + c[end:]

old_on_launch = '''    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    set_hidden(g_launchpad_idx, 0);
'''
new_on_launch = '''    if (!g_lp_xdg_ready) launchpad_populate_xdg();
    dock_populate_xdg();
    set_hidden(g_launchpad_idx, 0);
'''
c = replace_once(c, old_on_launch, new_on_launch, 'launchpad dock refresh')

old_xdg_launch = '''    if (xdg >= 0) {
        LunaApp tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.key = "xdg";
        tmp.name = g_lp_xdg[xdg].name;
        snprintf(tmp.cmd, sizeof(tmp.cmd), "%s", g_lp_xdg[xdg].path);
        app_launch(&tmp);
    }
'''
new_xdg_launch = '''    if (xdg >= 0) launch_xdg_app(xdg);
'''
c = replace_once(c, old_xdg_launch, new_xdg_launch, 'shared xdg launcher')

old_bind_tail = '''    int cc_bt = luna_get_element_by_id("cc_bt");
    if (cc_bt != -1)
        luna_update_classes(cc_bt, "on", g_settings.bluetooth_enabled ? "on" : NULL);
    k = luna_get_element_by_id("cc_bt_knob");
    if (k != -1) {
        luna_element_at(k)->rel_x = g_settings.bluetooth_enabled ? 21.0f : 3.0f;
        luna_element_at(k)->pos_overridden_x = 1;
    }
}
'''
new_bind_tail = '''    int cc_bt = luna_get_element_by_id("cc_bt");
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
'''
c = replace_once(c, old_bind_tail, new_bind_tail, 'bind XDG init')

c = replace_once(c,
'''    { .name="dock",         .root_id="dock",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_BOTTOM,
      .exclusive_zone=92, .margin_bottom=12, .fixed_w=640, .fixed_h=84 },
''',
'''    { .name="dock",         .root_id="dock",
      .layer=ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      .anchor=ZWLR_ANCHOR_BOTTOM,
      .exclusive_zone=92, .margin_bottom=12, .fixed_w=960, .fixed_h=84 },
''', 'Wayland dock width')

# Insert dynamic Dock slots immediately before the existing Trash separator.
marker = '''    <div class="dock_sep"></div>\n    <div class="dock_item" id="dock_trash"'''
slots = ['    <div id="dock_xdg_sep" class="dock_sep hidden"></div>']
for i in range(8):
    slots += [
        f'    <div class="dock_item hidden" id="dock_xdg_{i:02d}" role="button" tabindex="0" aria-label="Application">',
        '      <div class="dock_icon dock_xdg_icon luna_icon">&#xf2d0;</div>',
        '      <div class="dock_dot"></div>',
        '      <span class="dock_label">App</span>',
        '    </div>'
    ]
insert = '\n'.join(slots) + '\n    <div class="dock_sep"></div>\n    <div class="dock_item" id="dock_trash"'
layout = replace_once(layout, marker, insert, 'dock slots')

css += r'''

/* XDG application icons --------------------------------------------------
 * Raster images are resolved from freedesktop icon themes by luna-shell.
 * SVG paths are retained for the planned luna-svg renderer. */
.lp_icon, .dock_xdg_icon {
  padding: 0;
  background-size: contain;
  background-position: center;
  background-repeat: no-repeat;
  display: flex;
  align-items: center;
  justify-content: center;
}
.dock_xdg_icon { background-color: transparent; }
#dock_xdg_sep.hidden { display: none !important; }
'''

c_path.write_text(c)
layout_path.write_text(layout)
css_path.write_text(css)
print('Applied XDG Launchpad/Dock icon integration')
