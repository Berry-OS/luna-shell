#define _GNU_SOURCE
#define LUNA_UI_GLFW
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME "Luna View"
#define APP_VERSION "1.0"
#define SLIDESHOW_SECONDS 3.5
#define ZOOM_MIN 0.03f
#define ZOOM_MAX 32.0f

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
    size_t index;
} ImageList;

typedef struct {
    GLuint texture;
    int width;
    int height;
    int source_channels;
    off_t file_size;
    char path[PATH_MAX];
} ViewerImage;

typedef struct {
    GLuint program;
    GLint resolution;
    GLint center;
    GLint size;
    GLint angle;
    GLint clip_rect;
    GLint mode;
    GLint image;
} ViewerShader;

static GLFWwindow *g_window = NULL;
static ImageList g_images = {0};
static ViewerImage g_image = {0};
static ViewerShader g_shader = {0};

static int g_stage_idx = -1;
static int g_empty_idx = -1;
static int g_title_idx = -1;
static int g_subtitle_idx = -1;
static int g_info_idx = -1;
static int g_zoom_idx = -1;
static int g_slide_button_idx = -1;
static int g_delete_overlay_idx = -1;
static int g_delete_message_idx = -1;
static int g_delete_cancel_idx = -1;
static int g_help_overlay_idx = -1;
static int g_help_close_idx = -1;
static int g_toast_idx = -1;

static int g_fit_mode = 1;
static float g_manual_scale = 1.0f;
static float g_pan_x = 0.0f;
static float g_pan_y = 0.0f;
static int g_rotation_quarters = 0;
static int g_panning = 0;
static double g_last_mouse_x = 0.0;
static double g_last_mouse_y = 0.0;

static int g_window_dragging = 0;
static int g_drag_press_client_x = 0;
static int g_drag_press_client_y = 0;
static double g_last_title_click = -10.0;

static int g_fullscreen = 0;
static int g_windowed_x = 100;
static int g_windowed_y = 100;
static int g_windowed_w = 1100;
static int g_windowed_h = 760;

static int g_slideshow = 0;
static double g_last_slide_time = 0.0;
static int g_delete_dialog_open = 0;
static int g_help_dialog_open = 0;
static double g_toast_until = 0.0;
static int g_dirty = 1;

static const char *g_html =
"<!doctype html>"
"<html><head><title>Luna View</title></head>"
"<body>"
"  <div id=\"titlebar\" class=\"titlebar\">"
"    <div class=\"traffic\">"
"      <button class=\"traffic-button close\" onclick=\"app_close\" aria-label=\"閉じる\"></button>"
"      <button class=\"traffic-button minimize\" onclick=\"app_minimize\" aria-label=\"最小化\"></button>"
"      <button class=\"traffic-button maximize\" onclick=\"app_maximize\" aria-label=\"最大化\"></button>"
"    </div>"
"    <button class=\"open-button\" onclick=\"open_image\">画像を開く</button>"
"    <div class=\"title-copy\">"
"      <div id=\"image-title\" class=\"image-title\">Luna View</div>"
"      <div id=\"image-subtitle\" class=\"image-subtitle\">画像をドロップするか、画像を開いてください</div>"
"    </div>"
"    <div class=\"title-actions\">"
"      <button class=\"top-icon\" onclick=\"show_help\" aria-label=\"ショートカット\">?</button>"
"      <button class=\"top-icon\" onclick=\"toggle_slideshow\" aria-label=\"スライドショー\">▶</button>"
"      <button class=\"top-icon\" onclick=\"toggle_fullscreen\" aria-label=\"フルスクリーン\">⛶</button>"
"    </div>"
"  </div>"
"  <div id=\"stage\" class=\"stage\">"
"    <div id=\"empty-state\" class=\"empty-state\">"
"      <div class=\"empty-icon\">▧</div>"
"      <div class=\"empty-title\">画像をここにドロップ</div>"
"      <div class=\"empty-copy\">PNG、JPEG、BMP、TGA、GIF、HDR、PNM に対応</div>"
"      <button class=\"empty-open\" onclick=\"open_image\">画像を選択</button>"
"    </div>"
"  </div>"
"  <div class=\"bottom-shell\">"
"    <div class=\"bottom-bar\">"
"      <div class=\"bottom-info\">"
"        <div id=\"image-info\" class=\"image-info\">画像がありません</div>"
"      </div>"
"      <div class=\"viewer-controls\">"
"        <button class=\"control-button nav\" onclick=\"previous_image\" aria-label=\"前の画像\">‹</button>"
"        <button class=\"control-button nav\" onclick=\"next_image\" aria-label=\"次の画像\">›</button>"
"        <div class=\"separator\"></div>"
"        <button class=\"control-button\" onclick=\"rotate_left\" aria-label=\"左回転\">↺</button>"
"        <button class=\"control-button\" onclick=\"rotate_right\" aria-label=\"右回転\">↻</button>"
"        <div class=\"separator\"></div>"
"        <button class=\"control-button\" onclick=\"zoom_out\" aria-label=\"縮小\">−</button>"
"        <button id=\"zoom-label\" class=\"zoom-label\" onclick=\"actual_size\">100%</button>"
"        <button class=\"control-button\" onclick=\"zoom_in\" aria-label=\"拡大\">＋</button>"
"        <button class=\"text-button\" onclick=\"fit_image\">ウインドウに合わせる</button>"
"        <div class=\"separator\"></div>"
"        <button class=\"control-button danger\" onclick=\"request_delete\" aria-label=\"ゴミ箱へ移動\">⌫</button>"
"      </div>"
"      <div class=\"bottom-spacer\"></div>"
"    </div>"
"  </div>"
"  <div id=\"delete-overlay\" class=\"dialog-overlay hidden\">"
"    <div class=\"dialog-card\">"
"      <div class=\"dialog-symbol danger-symbol\">⌫</div>"
"      <div class=\"dialog-title\">画像をゴミ箱へ移動しますか？</div>"
"      <div id=\"delete-message\" class=\"dialog-message\">この操作はファイルマネージャーから取り消せます。</div>"
"      <div class=\"dialog-actions\">"
"        <button id=\"delete-cancel\" class=\"dialog-button secondary\" onclick=\"cancel_delete\">キャンセル</button>"
"        <button class=\"dialog-button destructive\" onclick=\"confirm_delete\">ゴミ箱へ移動</button>"
"      </div>"
"    </div>"
"  </div>"
"  <div id=\"help-overlay\" class=\"dialog-overlay hidden\">"
"    <div class=\"dialog-card help-card\">"
"      <div class=\"dialog-symbol\">⌨</div>"
"      <div class=\"dialog-title\">キーボードショートカット</div>"
"      <div class=\"shortcut-grid\">"
"        <div class=\"shortcut-key\">← / →</div><div class=\"shortcut-copy\">前 / 次の画像</div>"
"        <div class=\"shortcut-key\">＋ / −</div><div class=\"shortcut-copy\">拡大 / 縮小</div>"
"        <div class=\"shortcut-key\">0 / 1</div><div class=\"shortcut-copy\">フィット / 100%</div>"
"        <div class=\"shortcut-key\">R / Shift+R</div><div class=\"shortcut-copy\">右 / 左へ回転</div>"
"        <div class=\"shortcut-key\">Space</div><div class=\"shortcut-copy\">スライドショー</div>"
"        <div class=\"shortcut-key\">F / F11</div><div class=\"shortcut-copy\">フルスクリーン</div>"
"        <div class=\"shortcut-key\">Delete</div><div class=\"shortcut-copy\">ゴミ箱へ移動</div>"
"        <div class=\"shortcut-key\">Ctrl+C</div><div class=\"shortcut-copy\">パスをコピー</div>"
"      </div>"
"      <div class=\"dialog-actions\">"
"        <button id=\"help-close\" class=\"dialog-button primary\" onclick=\"hide_help\">閉じる</button>"
"      </div>"
"    </div>"
"  </div>"
"  <div id=\"toast\" class=\"toast\">完了しました</div>"
"</body></html>";

static const char *g_css =
"* { box-sizing: border-box; }"
"body { position: relative; width: 100%; height: 100%; background: rgba(0,0,0,0); color: #f7f7fa; overflow: hidden; font-size: 14px; }"
".titlebar { position: absolute; left: 0; right: 0; top: 0; height: 58px; padding: 0 16px; background: rgba(28,29,34,0.72); backdrop-filter: blur(22px) saturate(1.35) brightness(0.92); border-bottom: 1px solid rgba(255,255,255,0.09); z-index: 50; }"
".traffic { position: absolute; left: 16px; top: 19px; width: 72px; height: 20px; display: flex; align-items: center; gap: 9px; }"
".traffic-button { width: 13px; height: 13px; min-width: 13px; padding: 0; border: 0; border-radius: 50%; cursor: pointer; box-shadow: inset 0 0 0 1px rgba(0,0,0,0.16); }"
".traffic-button.close { background: #ff5f57; }"
".traffic-button.minimize { background: #febc2e; }"
".traffic-button.maximize { background: #28c840; }"
".traffic-button:hover { filter: brightness(1.13); transform: scale(1.08); }"
".open-button { position: absolute; left: 100px; top: 13px; height: 32px; padding: 0 14px; margin: 0; border-radius: 9px; border: 1px solid rgba(255,255,255,0.13); background: rgba(255,255,255,0.09); color: #fff; font-size: 13px; font-weight: 600; cursor: pointer; }"
".open-button:hover { background: rgba(255,255,255,0.15); }"
".title-copy { position: absolute; left: 230px; right: 230px; top: 10px; height: 40px; text-align: center; pointer-events: none; }"
".image-title { height: 20px; font-size: 14px; font-weight: 700; color: rgba(255,255,255,0.96); white-space: nowrap; text-overflow: ellipsis; overflow: hidden; }"
".image-subtitle { height: 17px; margin-top: 1px; font-size: 11px; color: rgba(255,255,255,0.48); white-space: nowrap; text-overflow: ellipsis; overflow: hidden; }"
".title-actions { position: absolute; right: 16px; top: 12px; height: 34px; display: flex; align-items: center; gap: 7px; }"
".top-icon { width: 32px; height: 32px; padding: 0; border: 1px solid rgba(255,255,255,0.09); border-radius: 9px; background: rgba(255,255,255,0.07); color: rgba(255,255,255,0.88); font-size: 15px; cursor: pointer; }"
".top-icon:hover { background: rgba(255,255,255,0.14); }"
".top-icon.active { background: rgba(96,165,250,0.25); border-color: rgba(96,165,250,0.48); color: #dbeafe; }"
".stage { position: absolute; left: 0; right: 0; top: 58px; bottom: 78px; overflow: hidden; z-index: 2; }"
".empty-state { position: absolute; left: 50%; top: 50%; width: 390px; height: 250px; margin-left: -195px; margin-top: -125px; padding: 28px; text-align: center; border-radius: 24px; border: 1px solid rgba(255,255,255,0.09); background: rgba(255,255,255,0.045); box-shadow: 0 24px 70px rgba(0,0,0,0.22); }"
".empty-icon { height: 72px; font-size: 58px; color: rgba(255,255,255,0.32); }"
".empty-title { margin-top: 8px; font-size: 20px; font-weight: 700; color: rgba(255,255,255,0.90); }"
".empty-copy { margin-top: 8px; font-size: 12px; color: rgba(255,255,255,0.48); }"
".empty-open { height: 36px; margin-top: 22px; padding: 0 18px; border: 0; border-radius: 10px; background: #2f7df6; color: #fff; font-weight: 700; cursor: pointer; box-shadow: 0 8px 24px rgba(47,125,246,0.28); }"
".empty-open:hover { filter: brightness(1.09); }"
".bottom-shell { position: absolute; left: 14px; right: 14px; bottom: 12px; height: 54px; z-index: 50; }"
".bottom-bar { width: 100%; height: 54px; display: flex; align-items: center; padding: 0 12px; border-radius: 16px; border: 1px solid rgba(255,255,255,0.10); background: rgba(31,32,38,0.72); backdrop-filter: blur(24px) saturate(1.4) brightness(0.9); box-shadow: 0 14px 42px rgba(0,0,0,0.28); }"
".bottom-info { width: auto; min-width: 0; height: 32px; display: flex; align-items: center; overflow: hidden; flex-grow: 1; flex-shrink: 1; flex-basis: 250px; }"
".image-info { width: 100%; font-size: 11px; color: rgba(255,255,255,0.56); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }"
".viewer-controls { height: 38px; margin-left: auto; margin-right: auto; display: flex; align-items: center; gap: 5px; flex-shrink: 0; }"
".bottom-spacer { width: auto; min-width: 0; flex-grow: 1; flex-shrink: 1; flex-basis: 250px; }"
".control-button, .zoom-label, .text-button { height: 32px; border: 0; border-radius: 9px; background: rgba(255,255,255,0.07); color: rgba(255,255,255,0.88); cursor: pointer; }"
".control-button { width: 34px; min-width: 34px; padding: 0; font-size: 18px; }"
".control-button.nav { font-size: 27px; line-height: 28px; }"
".control-button:hover, .zoom-label:hover, .text-button:hover { background: rgba(255,255,255,0.14); color: #fff; }"
".control-button.danger:hover { background: rgba(255,82,82,0.20); color: #ffd4d4; }"
".zoom-label { width: 58px; min-width: 58px; padding: 0; font-size: 11px; font-weight: 700; }"
".text-button { min-width: 126px; padding: 0 12px; font-size: 11px; font-weight: 600; }"
".separator { width: 1px; height: 20px; margin: 0 4px; background: rgba(255,255,255,0.12); }"
".dialog-overlay { position: fixed; left: 0; right: 0; top: 0; bottom: 0; display: flex; align-items: center; justify-content: center; background: rgba(4,5,8,0.48); backdrop-filter: blur(14px) saturate(0.85); z-index: 200; }"
".dialog-card { width: 430px; min-height: 270px; padding: 28px; border-radius: 24px; border: 1px solid rgba(255,255,255,0.13); background: rgba(40,41,48,0.92); box-shadow: 0 32px 100px rgba(0,0,0,0.52); text-align: center; }"
".help-card { width: 520px; min-height: 500px; }"
".dialog-symbol { width: 58px; height: 58px; margin-left: auto; margin-right: auto; padding-top: 11px; border-radius: 18px; background: rgba(96,165,250,0.16); color: #bfdbfe; font-size: 27px; }"
".danger-symbol { background: rgba(255,82,82,0.16); color: #fecaca; }"
".dialog-title { margin-top: 18px; font-size: 20px; font-weight: 750; color: #fff; }"
".dialog-message { margin-top: 12px; padding: 0 12px; font-size: 13px; line-height: 20px; color: rgba(255,255,255,0.56); }"
".dialog-actions { height: 40px; margin-top: 28px; display: flex; justify-content: center; gap: 10px; }"
".dialog-button { min-width: 130px; height: 38px; padding: 0 16px; border-radius: 11px; border: 1px solid rgba(255,255,255,0.10); color: #fff; font-weight: 700; cursor: pointer; }"
".dialog-button.secondary { background: rgba(255,255,255,0.08); }"
".dialog-button.primary { background: #2f7df6; border-color: rgba(255,255,255,0.08); }"
".dialog-button.destructive { background: #d94141; border-color: rgba(255,255,255,0.08); }"
".dialog-button:hover { filter: brightness(1.10); }"
".shortcut-grid { width: 100%; margin-top: 24px; display: grid; grid-template-columns: 150px 1fr; gap: 10px; text-align: left; }"
".shortcut-key { height: 28px; padding: 5px 10px; border-radius: 8px; background: rgba(255,255,255,0.07); color: rgba(255,255,255,0.86); font-size: 12px; font-weight: 700; text-align: right; }"
".shortcut-copy { height: 28px; padding: 5px 6px; color: rgba(255,255,255,0.58); font-size: 12px; }"
".toast { position: fixed; left: 50%; bottom: 82px; width: 300px; height: 42px; margin-left: -150px; padding-top: 12px; border-radius: 12px; background: rgba(24,25,30,0.92); border: 1px solid rgba(255,255,255,0.12); color: #fff; text-align: center; font-size: 12px; font-weight: 650; opacity: 0; transform: translateY(12px) scale(0.98); pointer-events: none; z-index: 300; box-shadow: 0 15px 45px rgba(0,0,0,0.36); }"
".toast.visible { opacity: 1; transform: translateY(0) scale(1); }"
".hidden { display: none; }";

static void mark_dirty(void) {
    g_dirty = 1;
    if (g_window) glfwPostEmptyEvent();
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static void path_dirname(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, out_size, ".");
    } else if (slash == out) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
}

static int is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int image_extension_supported(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot || !dot[1]) return 0;
    dot++;
    return strcasecmp(dot, "png") == 0 || strcasecmp(dot, "jpg") == 0 ||
           strcasecmp(dot, "jpeg") == 0 || strcasecmp(dot, "bmp") == 0 ||
           strcasecmp(dot, "tga") == 0 || strcasecmp(dot, "gif") == 0 ||
           strcasecmp(dot, "hdr") == 0 || strcasecmp(dot, "pic") == 0 ||
           strcasecmp(dot, "ppm") == 0 || strcasecmp(dot, "pgm") == 0 ||
           strcasecmp(dot, "pnm") == 0;
}

static int natural_compare_text(const char *a, const char *b) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (*pa && *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;
            const unsigned char *ea = pa;
            const unsigned char *eb = pb;
            while (isdigit(*ea)) ea++;
            while (isdigit(*eb)) eb++;
            ptrdiff_t la = ea - pa;
            ptrdiff_t lb = eb - pb;
            if (la != lb) return la < lb ? -1 : 1;
            int cmp = strncasecmp((const char *)pa, (const char *)pb, (size_t)la);
            if (cmp != 0) return cmp;
            pa = ea;
            pb = eb;
            continue;
        }
        int ca = tolower(*pa);
        int cb = tolower(*pb);
        if (ca != cb) return ca < cb ? -1 : 1;
        pa++;
        pb++;
    }
    if (*pa) return 1;
    if (*pb) return -1;
    return 0;
}

static int image_path_compare(const void *lhs, const void *rhs) {
    const char *a = *(const char *const *)lhs;
    const char *b = *(const char *const *)rhs;
    return natural_compare_text(path_basename(a), path_basename(b));
}

static void image_list_clear(ImageList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int image_list_reserve(ImageList *list, size_t needed) {
    if (needed <= list->capacity) return 1;
    size_t next = list->capacity ? list->capacity * 2 : 32;
    while (next < needed) next *= 2;
    char **items = (char **)realloc(list->items, next * sizeof(*items));
    if (!items) return 0;
    list->items = items;
    list->capacity = next;
    return 1;
}

static int image_list_append(ImageList *list, const char *path) {
    if (!list || !path || !path[0] || !image_extension_supported(path)) return 0;
    if (!image_list_reserve(list, list->count + 1)) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    list->items[list->count++] = copy;
    return 1;
}

static int image_list_scan_directory(ImageList *list, const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!image_extension_supported(entry->d_name)) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path)) continue;
        if (is_regular_file(path)) image_list_append(list, path);
    }
    closedir(dir);
    if (list->count > 1) qsort(list->items, list->count, sizeof(*list->items), image_path_compare);
    return list->count > 0;
}

static ssize_t image_list_find(const ImageList *list, const char *path) {
    if (!list || !path) return -1;
    char resolved_target[PATH_MAX];
    const char *target = realpath(path, resolved_target) ? resolved_target : path;
    for (size_t i = 0; i < list->count; i++) {
        char resolved_item[PATH_MAX];
        const char *item = realpath(list->items[i], resolved_item) ? resolved_item : list->items[i];
        if (strcmp(item, target) == 0) return (ssize_t)i;
    }
    return -1;
}

static void set_element_text(int idx, const char *text) {
    if (idx >= 0) luna_set_text(idx, text ? text : "");
}

static void show_toast(const char *message) {
    if (g_toast_idx < 0) return;
    set_element_text(g_toast_idx, message);
    luna_add_class(g_toast_idx, "visible");
    g_toast_until = glfwGetTime() + 2.3;
    mark_dirty();
}

static void hide_toast_if_needed(double now) {
    if (g_toast_until > 0.0 && now >= g_toast_until) {
        luna_remove_class(g_toast_idx, "visible");
        g_toast_until = 0.0;
        mark_dirty();
    }
}

static LunaElement *stage_element(void) {
    return g_stage_idx >= 0 ? luna_element_at(g_stage_idx) : NULL;
}

static float fit_scale_for_stage(void) {
    LunaElement *stage = stage_element();
    if (!stage || !g_image.texture || g_image.width <= 0 || g_image.height <= 0) return 1.0f;
    float iw = (float)g_image.width;
    float ih = (float)g_image.height;
    if (g_rotation_quarters & 1) {
        float tmp = iw;
        iw = ih;
        ih = tmp;
    }
    float usable_w = fmaxf(stage->w - 52.0f, 1.0f);
    float usable_h = fmaxf(stage->h - 42.0f, 1.0f);
    return fminf(usable_w / iw, usable_h / ih);
}

static float current_scale(void) {
    float scale = g_fit_mode ? fit_scale_for_stage() : g_manual_scale;
    if (scale < ZOOM_MIN) scale = ZOOM_MIN;
    if (scale > ZOOM_MAX) scale = ZOOM_MAX;
    return scale;
}

static void clamp_pan(void) {
    LunaElement *stage = stage_element();
    if (!stage || !g_image.texture) {
        g_pan_x = g_pan_y = 0.0f;
        return;
    }
    float scale = current_scale();
    float iw = g_image.width * scale;
    float ih = g_image.height * scale;
    if (g_rotation_quarters & 1) {
        float tmp = iw;
        iw = ih;
        ih = tmp;
    }
    float max_x = fmaxf((iw - stage->w) * 0.5f + 80.0f, 0.0f);
    float max_y = fmaxf((ih - stage->h) * 0.5f + 80.0f, 0.0f);
    if (iw <= stage->w) max_x = 0.0f;
    if (ih <= stage->h) max_y = 0.0f;
    if (g_pan_x < -max_x) g_pan_x = -max_x;
    if (g_pan_x > max_x) g_pan_x = max_x;
    if (g_pan_y < -max_y) g_pan_y = -max_y;
    if (g_pan_y > max_y) g_pan_y = max_y;
}

static void update_ui(void) {
    if (!g_image.texture) {
        set_element_text(g_title_idx, APP_NAME);
        set_element_text(g_subtitle_idx, "画像をドロップするか、画像を開いてください");
        set_element_text(g_info_idx, "画像がありません");
        set_element_text(g_zoom_idx, "—");
        if (g_empty_idx >= 0) luna_remove_class(g_empty_idx, "hidden");
        if (g_window) glfwSetWindowTitle(g_window, APP_NAME);
        mark_dirty();
        return;
    }

    char dir[PATH_MAX];
    char info[256];
    char zoom[64];
    char window_title[PATH_MAX + 64];
    path_dirname(g_image.path, dir, sizeof(dir));
    set_element_text(g_title_idx, path_basename(g_image.path));
    set_element_text(g_subtitle_idx, dir);
    double mib = (double)g_image.file_size / (1024.0 * 1024.0);
    snprintf(info, sizeof(info), "%d × %d  •  %.1f MB  •  %zu / %zu",
             g_image.width, g_image.height, mib,
             g_images.count ? g_images.index + 1 : 0, g_images.count);
    snprintf(zoom, sizeof(zoom), "%d%%", (int)lroundf(current_scale() * 100.0f));
    snprintf(window_title, sizeof(window_title), "%s — %s", path_basename(g_image.path), APP_NAME);
    set_element_text(g_info_idx, info);
    set_element_text(g_zoom_idx, zoom);
    if (g_empty_idx >= 0) luna_add_class(g_empty_idx, "hidden");
    if (g_window) glfwSetWindowTitle(g_window, window_title);
    mark_dirty();
}

static void destroy_current_texture(void) {
    if (g_image.texture) {
        GLuint texture = g_image.texture;
        glDeleteTextures(1, &texture);
    }
    memset(&g_image, 0, sizeof(g_image));
}

static int load_image_texture(const char *path) {
    if (!path || !path[0]) return 0;
    stbi_set_flip_vertically_on_load(1);
    stbi_set_unpremultiply_on_load(1);
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        show_toast("画像を読み込めませんでした");
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    if (!texture) {
        show_toast("OpenGLテクスチャを作成できませんでした");
        return 0;
    }

    destroy_current_texture();
    g_image.texture = texture;
    g_image.width = width;
    g_image.height = height;
    g_image.source_channels = channels;
    snprintf(g_image.path, sizeof(g_image.path), "%s", path);
    struct stat st;
    if (stat(path, &st) == 0) g_image.file_size = st.st_size;

    g_fit_mode = 1;
    g_manual_scale = 1.0f;
    g_pan_x = g_pan_y = 0.0f;
    g_rotation_quarters = 0;
    g_last_slide_time = glfwGetTime();
    update_ui();
    return 1;
}

static int load_current_list_image(void) {
    if (g_images.count == 0 || g_images.index >= g_images.count) {
        destroy_current_texture();
        update_ui();
        return 0;
    }
    return load_image_texture(g_images.items[g_images.index]);
}

static int open_path(const char *path) {
    if (!path || !path[0]) return 0;
    char resolved[PATH_MAX];
    const char *input = realpath(path, resolved) ? resolved : path;

    ImageList next = {0};
    if (is_directory(input)) {
        if (!image_list_scan_directory(&next, input)) {
            image_list_clear(&next);
            show_toast("このフォルダーに対応画像がありません");
            return 0;
        }
        next.index = 0;
    } else if (is_regular_file(input) && image_extension_supported(input)) {
        char directory[PATH_MAX];
        path_dirname(input, directory, sizeof(directory));
        image_list_scan_directory(&next, directory);
        ssize_t found = image_list_find(&next, input);
        if (found < 0) {
            image_list_append(&next, input);
            if (next.count > 1) qsort(next.items, next.count, sizeof(*next.items), image_path_compare);
            found = image_list_find(&next, input);
        }
        if (found < 0) {
            image_list_clear(&next);
            show_toast("画像を一覧へ追加できませんでした");
            return 0;
        }
        next.index = (size_t)found;
    } else {
        show_toast("対応していないファイルです");
        return 0;
    }

    image_list_clear(&g_images);
    g_images = next;
    return load_current_list_image();
}

static void step_image(int direction) {
    if (g_images.count == 0) return;
    if (direction > 0)
        g_images.index = (g_images.index + 1) % g_images.count;
    else
        g_images.index = (g_images.index + g_images.count - 1) % g_images.count;
    load_current_list_image();
}

static int command_exists(const char *command) {
    const char *path = getenv("PATH");
    if (!path || !command || !command[0]) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, command) < (int)sizeof(full) && access(full, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static int read_dialog_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    const char *command = NULL;
    if (command_exists("zenity")) {
        command = "zenity --file-selection --title='画像を開く' --file-filter='画像 | *.png *.jpg *.jpeg *.bmp *.tga *.gif *.hdr *.pic *.ppm *.pgm *.pnm' 2>/dev/null";
    } else if (command_exists("kdialog")) {
        command = "kdialog --getopenfilename . '*.png *.jpg *.jpeg *.bmp *.tga *.gif *.hdr *.pic *.ppm *.pgm *.pnm|画像' 2>/dev/null";
    } else if (command_exists("yad")) {
        command = "yad --file-selection --title='画像を開く' --file-filter='画像 | *.png *.jpg *.jpeg *.bmp *.tga *.gif *.hdr *.pic *.ppm *.pgm *.pnm' 2>/dev/null";
    }
    if (!command) {
        show_toast("zenity、kdialog、またはyadが必要です");
        return 0;
    }
    FILE *pipe = popen(command, "r");
    if (!pipe) return 0;
    if (!fgets(out, (int)out_size, pipe)) out[0] = '\0';
    int status = pclose(pipe);
    size_t length = strlen(out);
    while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r')) out[--length] = '\0';
    return status == 0 && out[0];
}

static int run_trash_command(const char *path) {
    const char *program = command_exists("gio") ? "gio" : command_exists("trash-put") ? "trash-put" : NULL;
    if (!program) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        if (strcmp(program, "gio") == 0)
            execlp("gio", "gio", "trash", "--", path, (char *)NULL);
        else
            execlp("trash-put", "trash-put", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void remove_current_from_list(void) {
    if (g_images.count == 0 || g_images.index >= g_images.count) return;
    free(g_images.items[g_images.index]);
    for (size_t i = g_images.index + 1; i < g_images.count; i++) g_images.items[i - 1] = g_images.items[i];
    g_images.count--;
    if (g_images.count == 0) {
        g_images.index = 0;
        destroy_current_texture();
        update_ui();
    } else {
        if (g_images.index >= g_images.count) g_images.index = g_images.count - 1;
        load_current_list_image();
    }
}

static void zoom_by(float factor) {
    if (!g_image.texture) return;
    g_manual_scale = current_scale() * factor;
    if (g_manual_scale < ZOOM_MIN) g_manual_scale = ZOOM_MIN;
    if (g_manual_scale > ZOOM_MAX) g_manual_scale = ZOOM_MAX;
    g_fit_mode = 0;
    clamp_pan();
    update_ui();
}

static void set_fit(void) {
    if (!g_image.texture) return;
    g_fit_mode = 1;
    g_pan_x = g_pan_y = 0.0f;
    update_ui();
}

static void set_actual(void) {
    if (!g_image.texture) return;
    g_fit_mode = 0;
    g_manual_scale = 1.0f;
    g_pan_x = g_pan_y = 0.0f;
    update_ui();
}

static void rotate_image(int delta) {
    if (!g_image.texture) return;
    g_rotation_quarters = (g_rotation_quarters + delta) % 4;
    if (g_rotation_quarters < 0) g_rotation_quarters += 4;
    if (g_fit_mode) g_pan_x = g_pan_y = 0.0f;
    clamp_pan();
    update_ui();
}

static void toggle_slideshow_state(void) {
    if (g_images.count == 0) return;
    g_slideshow = !g_slideshow;
    g_last_slide_time = glfwGetTime();
    if (g_slide_button_idx >= 0) {
        if (g_slideshow) luna_add_class(g_slide_button_idx, "active");
        else luna_remove_class(g_slide_button_idx, "active");
    }
    show_toast(g_slideshow ? "スライドショーを開始しました" : "スライドショーを停止しました");
}

static void toggle_maximize(void) {
    if (!g_window || g_fullscreen) return;
    if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED)) glfwRestoreWindow(g_window);
    else glfwMaximizeWindow(g_window);
    mark_dirty();
}

static void toggle_fullscreen_state(void) {
    if (!g_window) return;
    if (!g_fullscreen) {
        glfwGetWindowPos(g_window, &g_windowed_x, &g_windowed_y);
        glfwGetWindowSize(g_window, &g_windowed_w, &g_windowed_h);
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : NULL;
        if (monitor && mode) {
            glfwSetWindowMonitor(g_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
            g_fullscreen = 1;
        }
    } else {
        glfwSetWindowMonitor(g_window, NULL, g_windowed_x, g_windowed_y, g_windowed_w, g_windowed_h, 0);
        g_fullscreen = 0;
    }
    mark_dirty();
}

static void close_delete_dialog(void) {
    if (!g_delete_dialog_open) return;
    luna_add_class(g_delete_overlay_idx, "hidden");
    luna_pop_focus_trap(g_delete_overlay_idx);
    g_delete_dialog_open = 0;
    mark_dirty();
}

static void show_delete_dialog(void) {
    if (!g_image.texture || g_delete_dialog_open) return;
    char message[512];
    snprintf(message, sizeof(message), "「%s」をゴミ箱へ移動します。ファイルマネージャーから元に戻せます。", path_basename(g_image.path));
    set_element_text(g_delete_message_idx, message);
    luna_remove_class(g_delete_overlay_idx, "hidden");
    luna_push_focus_trap(g_delete_overlay_idx, NULL, 1);
    luna_focus_element(g_delete_cancel_idx);
    g_delete_dialog_open = 1;
    mark_dirty();
}

static void close_help_dialog(void) {
    if (!g_help_dialog_open) return;
    luna_add_class(g_help_overlay_idx, "hidden");
    luna_pop_focus_trap(g_help_overlay_idx);
    g_help_dialog_open = 0;
    mark_dirty();
}

static void show_help_dialog(void) {
    if (g_help_dialog_open) return;
    luna_remove_class(g_help_overlay_idx, "hidden");
    luna_push_focus_trap(g_help_overlay_idx, NULL, 1);
    luna_focus_element(g_help_close_idx);
    g_help_dialog_open = 1;
    mark_dirty();
}

static void on_app_close(LunaElement *e) { (void)e; glfwSetWindowShouldClose(g_window, GLFW_TRUE); }
static void on_app_minimize(LunaElement *e) { (void)e; glfwIconifyWindow(g_window); }
static void on_app_maximize(LunaElement *e) { (void)e; toggle_maximize(); }
static void on_open_image(LunaElement *e) {
    (void)e;
    char path[PATH_MAX];
    if (read_dialog_path(path, sizeof(path))) open_path(path);
}
static void on_previous_image(LunaElement *e) { (void)e; step_image(-1); }
static void on_next_image(LunaElement *e) { (void)e; step_image(1); }
static void on_rotate_left(LunaElement *e) { (void)e; rotate_image(-1); }
static void on_rotate_right(LunaElement *e) { (void)e; rotate_image(1); }
static void on_zoom_out(LunaElement *e) { (void)e; zoom_by(1.0f / 1.2f); }
static void on_zoom_in(LunaElement *e) { (void)e; zoom_by(1.2f); }
static void on_fit_image(LunaElement *e) { (void)e; set_fit(); }
static void on_actual_size(LunaElement *e) { (void)e; set_actual(); }
static void on_toggle_slideshow(LunaElement *e) { (void)e; toggle_slideshow_state(); }
static void on_toggle_fullscreen(LunaElement *e) { (void)e; toggle_fullscreen_state(); }
static void on_request_delete(LunaElement *e) { (void)e; show_delete_dialog(); }
static void on_cancel_delete(LunaElement *e) { (void)e; close_delete_dialog(); }
static void on_confirm_delete(LunaElement *e) {
    (void)e;
    if (!g_image.texture) { close_delete_dialog(); return; }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", g_image.path);
    close_delete_dialog();
    if (run_trash_command(path)) {
        remove_current_from_list();
        show_toast("画像をゴミ箱へ移動しました");
    } else {
        show_toast("ゴミ箱へ移動できませんでした");
    }
}
static void on_show_help(LunaElement *e) { (void)e; show_help_dialog(); }
static void on_hide_help(LunaElement *e) { (void)e; close_help_dialog(); }

static void register_handlers(void) {
    luna_register_js_handler("app_close", on_app_close);
    luna_register_js_handler("app_minimize", on_app_minimize);
    luna_register_js_handler("app_maximize", on_app_maximize);
    luna_register_js_handler("open_image", on_open_image);
    luna_register_js_handler("previous_image", on_previous_image);
    luna_register_js_handler("next_image", on_next_image);
    luna_register_js_handler("rotate_left", on_rotate_left);
    luna_register_js_handler("rotate_right", on_rotate_right);
    luna_register_js_handler("zoom_out", on_zoom_out);
    luna_register_js_handler("zoom_in", on_zoom_in);
    luna_register_js_handler("fit_image", on_fit_image);
    luna_register_js_handler("actual_size", on_actual_size);
    luna_register_js_handler("toggle_slideshow", on_toggle_slideshow);
    luna_register_js_handler("toggle_fullscreen", on_toggle_fullscreen);
    luna_register_js_handler("request_delete", on_request_delete);
    luna_register_js_handler("cancel_delete", on_cancel_delete);
    luna_register_js_handler("confirm_delete", on_confirm_delete);
    luna_register_js_handler("show_help", on_show_help);
    luna_register_js_handler("hide_help", on_hide_help);
}

static void cache_element_ids(void) {
    g_stage_idx = luna_get_element_by_id("stage");
    g_empty_idx = luna_get_element_by_id("empty-state");
    g_title_idx = luna_get_element_by_id("image-title");
    g_subtitle_idx = luna_get_element_by_id("image-subtitle");
    g_info_idx = luna_get_element_by_id("image-info");
    g_zoom_idx = luna_get_element_by_id("zoom-label");
    g_delete_overlay_idx = luna_get_element_by_id("delete-overlay");
    g_delete_message_idx = luna_get_element_by_id("delete-message");
    g_delete_cancel_idx = luna_get_element_by_id("delete-cancel");
    g_help_overlay_idx = luna_get_element_by_id("help-overlay");
    g_help_close_idx = luna_get_element_by_id("help-close");
    g_toast_idx = luna_get_element_by_id("toast");

    /* Find the top-right slideshow button by its onclick name. */
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement *element = luna_element_at(i);
        if (element && strcmp(element->onclick, "toggle_slideshow") == 0) {
            g_slide_button_idx = i;
            break;
        }
    }
}

static int init_view_shader(void) {
    const char *vertex_source =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "uniform vec2 uResolution;\n"
        "uniform vec2 uCenter;\n"
        "uniform vec2 uSize;\n"
        "uniform float uAngle;\n"
        "out vec2 vUV;\n"
        "out vec2 vScreen;\n"
        "void main(){\n"
        "  vec2 local=(aPos-vec2(0.5))*uSize;\n"
        "  float c=cos(uAngle), s=sin(uAngle);\n"
        "  vec2 rotated=vec2(local.x*c-local.y*s, local.x*s+local.y*c);\n"
        "  vec2 screen=uCenter+rotated;\n"
        "  vec2 ndc=(screen/uResolution)*2.0-1.0;\n"
        "  gl_Position=vec4(ndc.x,-ndc.y,0.0,1.0);\n"
        "  vUV=vec2(aPos.x,1.0-aPos.y);\n"
        "  vScreen=screen;\n"
        "}\n";
    const char *fragment_source =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "in vec2 vScreen;\n"
        "out vec4 FragColor;\n"
        "uniform vec2 uResolution;\n"
        "uniform vec4 uClipRect;\n"
        "uniform int uMode;\n"
        "uniform sampler2D uImage;\n"
        "void main(){\n"
        "  if(uMode==0){\n"
        "    vec2 uv=vScreen/uResolution;\n"
        "    float d=distance(uv,vec2(0.5,0.46));\n"
        "    vec3 top=vec3(0.105,0.110,0.132);\n"
        "    vec3 bottom=vec3(0.035,0.038,0.050);\n"
        "    vec3 col=mix(top,bottom,clamp(uv.y*0.88+d*0.28,0.0,1.0));\n"
        "    col+=vec3(0.018,0.022,0.036)*(1.0-smoothstep(0.0,0.68,d));\n"
        "    FragColor=vec4(col,1.0);\n"
        "    return;\n"
        "  }\n"
        "  if(vScreen.x<uClipRect.x || vScreen.y<uClipRect.y || vScreen.x>uClipRect.z || vScreen.y>uClipRect.w) discard;\n"
        "  vec4 texel=texture(uImage,vUV);\n"
        "  vec2 checker=floor(vScreen/10.0);\n"
        "  float alt=mod(checker.x+checker.y,2.0);\n"
        "  vec3 check=mix(vec3(0.22),vec3(0.29),alt);\n"
        "  vec3 rgb=mix(check,texel.rgb,texel.a);\n"
        "  float edge=min(min(vUV.x,1.0-vUV.x),min(vUV.y,1.0-vUV.y));\n"
        "  rgb=mix(vec3(0.06),rgb,smoothstep(0.0,0.003,edge));\n"
        "  FragColor=vec4(rgb,1.0);\n"
        "}\n";

    GLuint vs = compile_shader(vertex_source, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(fragment_source, GL_FRAGMENT_SHADER);
    if (!vs || !fs) return 0;
    g_shader.program = glCreateProgram();
    glAttachShader(g_shader.program, vs);
    glAttachShader(g_shader.program, fs);
    glLinkProgram(g_shader.program);
    if (!g_shader.program) return 0;
    g_shader.resolution = glGetUniformLocation(g_shader.program, "uResolution");
    g_shader.center = glGetUniformLocation(g_shader.program, "uCenter");
    g_shader.size = glGetUniformLocation(g_shader.program, "uSize");
    g_shader.angle = glGetUniformLocation(g_shader.program, "uAngle");
    g_shader.clip_rect = glGetUniformLocation(g_shader.program, "uClipRect");
    g_shader.mode = glGetUniformLocation(g_shader.program, "uMode");
    g_shader.image = glGetUniformLocation(g_shader.program, "uImage");
    return 1;
}

static void draw_view_quad(float cx, float cy, float width, float height, float angle, int mode,
                           float clip_left, float clip_top, float clip_right, float clip_bottom) {
    glUseProgram(g_shader.program);
    glUniform2f(g_shader.resolution, luna_window_width, luna_window_height);
    glUniform2f(g_shader.center, cx, cy);
    glUniform2f(g_shader.size, width, height);
    glUniform1f(g_shader.angle, angle);
    glUniform4f(g_shader.clip_rect, clip_left, clip_top, clip_right, clip_bottom);
    glUniform1i_(g_shader.mode, mode);
    if (mode == 1) {
        if (glActiveTexture_) glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_image.texture);
        glUniform1i_(g_shader.image, 0);
    }
    glBindVertexArray(g_rect_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    if (mode == 1) glBindTexture(GL_TEXTURE_2D, 0);
}

static void render_viewer_background_and_image(int framebuffer_width, int framebuffer_height) {
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    draw_view_quad(luna_window_width * 0.5f, luna_window_height * 0.5f,
                   luna_window_width, luna_window_height, 0.0f, 0,
                   0.0f, 0.0f, luna_window_width, luna_window_height);

    LunaElement *stage = stage_element();
    if (g_image.texture && stage) {
        float scale = current_scale();
        float width = g_image.width * scale;
        float height = g_image.height * scale;
        float center_x = stage->x + stage->w * 0.5f + g_pan_x;
        float center_y = stage->y + stage->h * 0.5f + g_pan_y;
        float angle = (float)g_rotation_quarters * (float)(M_PI * 0.5);
        draw_view_quad(center_x, center_y, width, height, angle, 1,
                       stage->x, stage->y, stage->x + stage->w, stage->y + stage->h);
    }
    luna_invalidate_gl_state();
}

static int point_in_stage(double x, double y) {
    LunaElement *stage = stage_element();
    return stage && x >= stage->x && x <= stage->x + stage->w && y >= stage->y && y <= stage->y + stage->h;
}

static int point_in_title_drag_zone(double x, double y) {
    return !g_fullscreen && y >= 0.0 && y <= 58.0 && x >= 205.0 && x <= luna_window_width - 175.0;
}

static void platform_request_close(void) { if (g_window) glfwSetWindowShouldClose(g_window, GLFW_TRUE); }
static void platform_iconify(void) { if (g_window) glfwIconifyWindow(g_window); }
static void platform_maximize_toggle(void) { toggle_maximize(); }
static double platform_get_time(void) { return glfwGetTime(); }
static void *platform_get_proc(const char *name) { return (void *)glfwGetProcAddress(name); }

static void window_size_callback(GLFWwindow *window, int width, int height) {
    (void)window;
    luna_resize((float)width, (float)height);
    clamp_pan();
    update_ui();
    mark_dirty();
}

static void framebuffer_size_callback_app(GLFWwindow *window, int width, int height) {
    (void)window; (void)width; (void)height;
    luna_framebuffer_resized();
    mark_dirty();
}

static void cursor_position_callback_app(GLFWwindow *window, double x, double y) {
    if (g_panning) {
        g_pan_x += (float)(x - g_last_mouse_x);
        g_pan_y += (float)(y - g_last_mouse_y);
        clamp_pan();
        update_ui();
    }
    if (g_window_dragging) {
        int wx = 0, wy = 0;
        glfwGetWindowPos(window, &wx, &wy);
        int screen_x = wx + (int)lround(x);
        int screen_y = wy + (int)lround(y);
        glfwSetWindowPos(window, screen_x - g_drag_press_client_x, screen_y - g_drag_press_client_y);
        mark_dirty();
    }
    g_last_mouse_x = x;
    g_last_mouse_y = y;
    luna_mouse_move(x, y);
}

static void mouse_button_callback_app(GLFWwindow *window, int button, int action, int mods) {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    g_last_mouse_x = x;
    g_last_mouse_y = y;

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !g_delete_dialog_open && !g_help_dialog_open) {
        if (point_in_stage(x, y) && g_image.texture) {
            g_panning = 1;
        } else if (point_in_title_drag_zone(x, y)) {
            double now = glfwGetTime();
            if (now - g_last_title_click < 0.32) {
                toggle_maximize();
                g_last_title_click = -10.0;
            } else {
                g_last_title_click = now;
                g_window_dragging = 1;
                g_drag_press_client_x = (int)lround(x);
                g_drag_press_client_y = (int)lround(y);
            }
        }
    }

    luna_mouse_move(x, y);
    luna_mouse_button(button, action, mods, x, y);

    if (action == GLFW_RELEASE) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            g_panning = 0;
            g_window_dragging = 0;
        }
    }
    mark_dirty();
}

static void scroll_callback_app(GLFWwindow *window, double xoffset, double yoffset) {
    (void)xoffset;
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (point_in_stage(x, y) && g_image.texture) {
        zoom_by(yoffset > 0.0 ? 1.12f : 1.0f / 1.12f);
    } else {
        luna_scroll(xoffset, yoffset);
    }
    mark_dirty();
}

static void copy_current_path(void) {
    if (!g_image.texture) return;
    glfwSetClipboardString(g_window, g_image.path);
    show_toast("画像のパスをコピーしました");
}

static void key_callback_app(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)window;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (g_delete_dialog_open) {
            if (key == GLFW_KEY_ESCAPE) close_delete_dialog();
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) on_confirm_delete(NULL);
            luna_key(key, scancode, action, mods);
            return;
        }
        if (g_help_dialog_open) {
            if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) close_help_dialog();
            luna_key(key, scancode, action, mods);
            return;
        }

        if (key == GLFW_KEY_LEFT) step_image(-1);
        else if (key == GLFW_KEY_RIGHT) step_image(1);
        else if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) zoom_by(1.2f);
        else if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) zoom_by(1.0f / 1.2f);
        else if (key == GLFW_KEY_0) set_fit();
        else if (key == GLFW_KEY_1) set_actual();
        else if (key == GLFW_KEY_R) rotate_image((mods & GLFW_MOD_SHIFT) ? -1 : 1);
        else if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) toggle_slideshow_state();
        else if ((key == GLFW_KEY_F || key == GLFW_KEY_F11) && action == GLFW_PRESS) toggle_fullscreen_state();
        else if (key == GLFW_KEY_DELETE && action == GLFW_PRESS) show_delete_dialog();
        else if (key == GLFW_KEY_O && (mods & GLFW_MOD_CONTROL) && action == GLFW_PRESS) on_open_image(NULL);
        else if (key == GLFW_KEY_C && (mods & GLFW_MOD_CONTROL) && action == GLFW_PRESS) copy_current_path();
        else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && g_fullscreen) toggle_fullscreen_state();
        else if (key == GLFW_KEY_SLASH && (mods & GLFW_MOD_SHIFT) && action == GLFW_PRESS) show_help_dialog();
    }
    luna_key(key, scancode, action, mods);
    mark_dirty();
}

static void character_callback_app(GLFWwindow *window, unsigned int codepoint) {
    (void)window;
    luna_char(codepoint);
    mark_dirty();
}

static void drop_callback_app(GLFWwindow *window, int count, const char **paths) {
    (void)window;
    if (count <= 0 || !paths) return;
    if (count == 1) {
        open_path(paths[0]);
        return;
    }
    ImageList next = {0};
    for (int i = 0; i < count; i++) {
        if (is_regular_file(paths[i]) && image_extension_supported(paths[i])) {
            char resolved[PATH_MAX];
            image_list_append(&next, realpath(paths[i], resolved) ? resolved : paths[i]);
        }
    }
    if (next.count == 0) {
        image_list_clear(&next);
        show_toast("対応画像が含まれていません");
        return;
    }
    if (next.count > 1) qsort(next.items, next.count, sizeof(*next.items), image_path_compare);
    image_list_clear(&g_images);
    g_images = next;
    g_images.index = 0;
    load_current_list_image();
}

static void refresh_slideshow(double now) {
    if (g_slideshow && g_images.count > 1 && now - g_last_slide_time >= SLIDESHOW_SECONDS) {
        step_image(1);
        g_last_slide_time = now;
    }
}

static int init_glfw_and_window(void) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 0;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    g_window = glfwCreateWindow(g_windowed_w, g_windowed_h, APP_NAME, NULL, NULL);
    if (!g_window) return 0;
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);
    glfwSetWindowSizeLimits(g_window, 720, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);

    glfwSetWindowSizeCallback(g_window, window_size_callback);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback_app);
    glfwSetCursorPosCallback(g_window, cursor_position_callback_app);
    glfwSetMouseButtonCallback(g_window, mouse_button_callback_app);
    glfwSetScrollCallback(g_window, scroll_callback_app);
    glfwSetKeyCallback(g_window, key_callback_app);
    glfwSetCharCallback(g_window, character_callback_app);
    glfwSetDropCallback(g_window, drop_callback_app);
    return 1;
}

static int init_luna_ui(void) {
    int width = 0, height = 0;
    glfwGetWindowSize(g_window, &width, &height);
    g_luna_glfw_window = g_window;

    LunaPlatform platform = {0};
    platform.get_time = platform_get_time;
    platform.get_proc = platform_get_proc;
    platform.request_close = platform_request_close;
    platform.iconify = platform_iconify;
    platform.maximize_toggle = platform_maximize_toggle;
    luna_set_platform(&platform);

    LunaInitConfig config = {0};
    config.width = (float)width;
    config.height = (float)height;
    config.get_proc = platform_get_proc;
    config.frameless = 1;
    if (!luna_init(&config)) return 0;

    register_handlers();
    luna_parse_css(g_css);
    luna_parse_html(g_html);
    luna_wire_onclick_handlers();
    luna_inject_body_background();
    cache_element_ids();
    luna_resize((float)width, (float)height);
    luna_update(glfwGetTime(), 0.0);
    update_ui();
    return 1;
}

static void parse_startup_paths(int argc, char **argv) {
    if (argc <= 1) return;
    if (argc == 2) {
        open_path(argv[1]);
        return;
    }
    ImageList next = {0};
    for (int i = 1; i < argc; i++) {
        if (is_directory(argv[i])) {
            ImageList dir_list = {0};
            if (image_list_scan_directory(&dir_list, argv[i])) {
                for (size_t j = 0; j < dir_list.count; j++) image_list_append(&next, dir_list.items[j]);
            }
            image_list_clear(&dir_list);
        } else if (is_regular_file(argv[i]) && image_extension_supported(argv[i])) {
            char resolved[PATH_MAX];
            image_list_append(&next, realpath(argv[i], resolved) ? resolved : argv[i]);
        }
    }
    if (next.count == 0) {
        image_list_clear(&next);
        return;
    }
    if (next.count > 1) qsort(next.items, next.count, sizeof(*next.items), image_path_compare);
    image_list_clear(&g_images);
    g_images = next;
    g_images.index = 0;
    load_current_list_image();
}

static void cleanup(void) {
    destroy_current_texture();
    image_list_clear(&g_images);
    luna_shutdown();
    if (g_window) glfwDestroyWindow(g_window);
    glfwTerminate();
}

static void print_help(const char *program) {
    printf("%s %s\n", APP_NAME, APP_VERSION);
    printf("Usage: %s [IMAGE | DIRECTORY]...\n", program);
    printf("A lightweight Luna UI image viewer.\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_help(argv[0]);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }

    if (!init_glfw_and_window()) {
        fprintf(stderr, "%s: GLFW window initialization failed\n", APP_NAME);
        cleanup();
        return 1;
    }
    if (!init_luna_ui()) {
        fprintf(stderr, "%s: Luna UI initialization failed\n", APP_NAME);
        cleanup();
        return 1;
    }
    if (!init_view_shader()) {
        fprintf(stderr, "%s: image shader initialization failed\n", APP_NAME);
        cleanup();
        return 1;
    }

    parse_startup_paths(argc, argv);

    double previous = glfwGetTime();
    while (!glfwWindowShouldClose(g_window)) {
        double now = glfwGetTime();
        double dt = now - previous;
        if (dt < 0.0 || dt > 0.25) dt = 0.016;
        previous = now;

        refresh_slideshow(now);
        hide_toast_if_needed(now);
        int settling = luna_update_settling(now, dt);

        if (g_dirty || settling || g_slideshow || g_toast_until > 0.0) {
            int framebuffer_width = 0, framebuffer_height = 0;
            glfwGetFramebufferSize(g_window, &framebuffer_width, &framebuffer_height);
            if (framebuffer_width > 0 && framebuffer_height > 0) {
                render_viewer_background_and_image(framebuffer_width, framebuffer_height);
                luna_render(framebuffer_width, framebuffer_height);
                glfwSwapBuffers(g_window);
            }
            g_dirty = settling;
        }

        if (g_dirty || settling || g_panning || g_window_dragging)
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(0.05);
    }

    cleanup();
    return 0;
}
