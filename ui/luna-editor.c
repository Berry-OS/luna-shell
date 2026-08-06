/*
 * Luna Editor: a fast, macOS-inspired text editor built on luna-ui.h
 *
 * Features:
 *   - Virtualized editor rendering (only visible lines are painted)
 *   - UTF-8 aware caret movement and selection
 *   - Native clipboard copy/cut/paste through GLFW
 *   - Multi-document tabs, explorer sidebar, drag & drop open
 *   - Undo/redo, find, go-to-line, auto-indent, syntax highlighting
 *   - Atomic save, zoom, minimap, line numbers and status bar
 *   - Persistent settings dialog including font, spacing and caret options
 *
 * Build:
 *   cc -O2 -std=c11 -Wall -Wextra luna-editor.c -o luna-editor \
 *      $(pkg-config --cflags --libs glfw3) -lGL -lm
 *
 * Keep luna-ui.h, stb_truetype.h, stb_image.h, stb_image_write.h and
 * cssparser.h beside this source file.
 */

#define _GNU_SOURCE
#define LUNA_UI_GLFW
#ifndef LUNA_UI_MAX_ELEMENTS
#define LUNA_UI_MAX_ELEMENTS 900
#endif
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME "Luna Editor"
#define MAX_DOCS 8
#define MAX_EXPLORER_ITEMS 16
#define MAX_UNDO 2048
#define MAX_FIND 256
#define EDITOR_GUTTER_W_DEFAULT 58.0f
#define EDITOR_MINIMAP_W 82.0f
#define EDITOR_PAD_X 14.0f
#define DEFAULT_FONT_SIZE 14.0f
#define MIN_FONT_SIZE 10.0f
#define MAX_FONT_SIZE 28.0f
#define DEFAULT_EDITOR_FONT_FACE 3 /* luna-ui: editor monospace face */
#define DEFAULT_LINE_HEIGHT 1.52f
#define MIN_LINE_HEIGHT 1.20f
#define MAX_LINE_HEIGHT 2.00f
#define DEFAULT_LETTER_SPACING 0.0f
#define MIN_LETTER_SPACING -0.5f
#define MAX_LETTER_SPACING 2.0f
#define DEFAULT_TAB_WIDTH 4
#define MAX_TAB_WIDTH 8

/* ------------------------------------------------------------------------- */
/* Small text-buffer core                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} TextLine;

typedef struct {
    TextLine *lines;
    size_t count;
    size_t cap;
} TextBuffer;

typedef struct {
    size_t line;
    size_t col; /* UTF-8 byte offset */
} TextPos;

typedef enum {
    EDIT_INSERT = 1,
    EDIT_DELETE = 2
} EditKind;

typedef struct {
    EditKind kind;
    TextPos at;
    char *text;
} UndoEdit;

typedef struct {
    UndoEdit *items;
    size_t count;
    size_t cap;
    size_t cursor;
    ssize_t saved_cursor;
} UndoStack;

typedef enum {
    LANG_TEXT,
    LANG_C,
    LANG_CPP,
    LANG_PYTHON,
    LANG_JS,
    LANG_JSON,
    LANG_HTML,
    LANG_CSS,
    LANG_MARKDOWN,
    LANG_SHELL
} Language;

typedef struct {
    TextBuffer buffer;
    UndoStack undo;
    TextPos caret;
    TextPos anchor;
    float scroll_y;
    float scroll_x;
    float desired_x;
    int desired_x_valid;
    char path[PATH_MAX];
    char display_name[256];
    Language language;
    int read_only;
} Document;

typedef struct {
    char name[256];
    char path[PATH_MAX];
    int is_dir;
} ExplorerEntry;

typedef enum {
    PROMPT_NONE,
    PROMPT_OPEN_FILE,
    PROMPT_SAVE_AS,
    PROMPT_FIND,
    PROMPT_GOTO,
    PROMPT_OPEN_FOLDER
} PromptMode;

static GLFWwindow *g_window;
static Document g_docs[MAX_DOCS];
static int g_doc_count = 0;
static int g_active_doc = -1;
static ExplorerEntry g_entries[MAX_EXPLORER_ITEMS];
static int g_entry_count = 0;
static char g_working_dir[PATH_MAX];
static char g_find_query[MAX_FIND];
static PromptMode g_prompt_mode = PROMPT_NONE;
static int g_editor_focused = 1;
static int g_mouse_selecting = 0;
static double g_last_click_time = 0.0;
static TextPos g_last_click_pos = {0, 0};
typedef enum {
    CURSOR_LINE = 0,
    CURSOR_BLOCK = 1,
    CURSOR_UNDERLINE = 2
} CursorStyle;

typedef struct {
    float font_size;
    int font_face;              /* 3 = monospace, 0 = UI sans-serif */
    float line_height;
    float letter_spacing;
    int tab_width;
    int insert_spaces;
    int auto_indent;
    int smart_dedent;
    int syntax_highlighting;
    int line_numbers;
    int highlight_current_line;
    int minimap_visible;
    int sidebar_visible;
    int statusbar_visible;
    int cursor_blink;
    int cursor_style;
    int show_whitespace;
    int indent_guides;
    int dark_theme;
} EditorSettings;

static float g_editor_font_size = DEFAULT_FONT_SIZE;
static int g_editor_font_face = DEFAULT_EDITOR_FONT_FACE;
static float g_editor_line_height = DEFAULT_LINE_HEIGHT;
static float g_editor_letter_spacing = DEFAULT_LETTER_SPACING;
static int g_tab_width = DEFAULT_TAB_WIDTH;
static int g_insert_spaces = 1;
static int g_auto_indent = 1;
static int g_smart_dedent = 1;
static int g_syntax_highlighting = 1;
static int g_line_numbers_visible = 1;
static int g_highlight_current_line = 1;
static int g_sidebar_visible = 1;
static int g_minimap_visible = 0;
static int g_statusbar_visible = 1;
static int g_cursor_blink = 1;
static int g_cursor_style = CURSOR_LINE;
static int g_show_whitespace = 0;
static int g_indent_guides = 1;
static int g_dark_theme = 0;
static int g_settings_visible = 0;
static EditorSettings g_settings_draft;
static int g_needs_redraw = 1;
/* Number of complete frames still required after a structural document/UI
 * change.  A file open changes tab visibility and editor contents during an
 * input callback; one additional frame is needed after Luna's layout pass. */
static int g_followup_redraw_frames = 0;
static double g_last_render_time = 0.0;
static char g_status_message[256];
static double g_status_until = 0.0;

/* DOM indices cached after parsing. */
static int id_app = -1;
static int id_editor_host = -1;
static int id_titlebar = -1;
static int id_workspace = -1;
static int id_tabs = -1;
static int id_sidebar = -1;
static int id_settings_overlay = -1;
static int id_settings_card = -1;
static int id_setting_theme = -1;
static int id_setting_font_family = -1;
static int id_setting_font_size = -1;
static int id_setting_line_height = -1;
static int id_setting_letter_spacing = -1;
static int id_setting_tab_width = -1;
static int id_setting_indent = -1;
static int id_setting_auto_indent = -1;
static int id_setting_smart_dedent = -1;
static int id_setting_syntax = -1;
static int id_setting_line_numbers = -1;
static int id_setting_current_line = -1;
static int id_setting_minimap = -1;
static int id_setting_sidebar = -1;
static int id_setting_statusbar = -1;
static int id_setting_cursor_style = -1;
static int id_setting_cursor_blink = -1;
static int id_setting_whitespace = -1;
static int id_setting_indent_guides = -1;
static int id_prompt_overlay = -1;
static int id_prompt_card = -1;
static int id_prompt_title = -1;
static int id_prompt_input = -1;
static int id_title = -1;
static int id_folder = -1;
static int id_status_left = -1;
static int id_status_center = -1;
static int id_status_right = -1;
static int id_statusbar = -1;
static int id_tab_slot[MAX_DOCS];
static int id_tab[MAX_DOCS];
static int id_file[MAX_EXPLORER_ITEMS];

static void request_redraw(void) { g_needs_redraw = 1; }

static void request_followup_redraw(int frames) {
    if (frames < 1) frames = 1;
    if (g_followup_redraw_frames < frames)
        g_followup_redraw_frames = frames;
    g_needs_redraw = 1;
}

static void statusf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_message, sizeof(g_status_message), fmt, ap);
    va_end(ap);
    g_status_until = glfwGetTime() + 3.5;
    request_redraw();
}

static void settings_defaults(EditorSettings *s) {
    if (!s) return;
    *s = (EditorSettings){
        .font_size = DEFAULT_FONT_SIZE,
        .font_face = DEFAULT_EDITOR_FONT_FACE,
        .line_height = DEFAULT_LINE_HEIGHT,
        .letter_spacing = DEFAULT_LETTER_SPACING,
        .tab_width = DEFAULT_TAB_WIDTH,
        .insert_spaces = 1,
        .auto_indent = 1,
        .smart_dedent = 1,
        .syntax_highlighting = 1,
        .line_numbers = 1,
        .highlight_current_line = 1,
        .minimap_visible = 0,
        .sidebar_visible = 1,
        .statusbar_visible = 1,
        .cursor_blink = 1,
        .cursor_style = CURSOR_LINE,
        .show_whitespace = 0,
        .indent_guides = 1,
        .dark_theme = 0
    };
}

static void settings_normalize(EditorSettings *s) {
    if (!s) return;
    if (s->font_size < MIN_FONT_SIZE) s->font_size = MIN_FONT_SIZE;
    if (s->font_size > MAX_FONT_SIZE) s->font_size = MAX_FONT_SIZE;
    if (s->font_face != 0 && s->font_face != 3)
        s->font_face = DEFAULT_EDITOR_FONT_FACE;
    if (s->line_height < MIN_LINE_HEIGHT) s->line_height = MIN_LINE_HEIGHT;
    if (s->line_height > MAX_LINE_HEIGHT) s->line_height = MAX_LINE_HEIGHT;
    if (s->letter_spacing < MIN_LETTER_SPACING) s->letter_spacing = MIN_LETTER_SPACING;
    if (s->letter_spacing > MAX_LETTER_SPACING) s->letter_spacing = MAX_LETTER_SPACING;
    if (s->tab_width != 2 && s->tab_width != 4 && s->tab_width != 8)
        s->tab_width = DEFAULT_TAB_WIDTH;
    if (s->cursor_style < CURSOR_LINE || s->cursor_style > CURSOR_UNDERLINE)
        s->cursor_style = CURSOR_LINE;
#define BOOL_FIELD(name) s->name = s->name ? 1 : 0
    BOOL_FIELD(insert_spaces);
    BOOL_FIELD(auto_indent);
    BOOL_FIELD(smart_dedent);
    BOOL_FIELD(syntax_highlighting);
    BOOL_FIELD(line_numbers);
    BOOL_FIELD(highlight_current_line);
    BOOL_FIELD(minimap_visible);
    BOOL_FIELD(sidebar_visible);
    BOOL_FIELD(statusbar_visible);
    BOOL_FIELD(cursor_blink);
    BOOL_FIELD(show_whitespace);
    BOOL_FIELD(indent_guides);
    BOOL_FIELD(dark_theme);
#undef BOOL_FIELD
}

static void settings_capture(EditorSettings *s) {
    if (!s) return;
    *s = (EditorSettings){
        .font_size = g_editor_font_size,
        .font_face = g_editor_font_face,
        .line_height = g_editor_line_height,
        .letter_spacing = g_editor_letter_spacing,
        .tab_width = g_tab_width,
        .insert_spaces = g_insert_spaces,
        .auto_indent = g_auto_indent,
        .smart_dedent = g_smart_dedent,
        .syntax_highlighting = g_syntax_highlighting,
        .line_numbers = g_line_numbers_visible,
        .highlight_current_line = g_highlight_current_line,
        .minimap_visible = g_minimap_visible,
        .sidebar_visible = g_sidebar_visible,
        .statusbar_visible = g_statusbar_visible,
        .cursor_blink = g_cursor_blink,
        .cursor_style = g_cursor_style,
        .show_whitespace = g_show_whitespace,
        .indent_guides = g_indent_guides,
        .dark_theme = g_dark_theme
    };
}

static void settings_assign(const EditorSettings *src) {
    if (!src) return;
    EditorSettings s = *src;
    settings_normalize(&s);
    g_editor_font_size = s.font_size;
    g_editor_font_face = s.font_face;
    g_editor_line_height = s.line_height;
    g_editor_letter_spacing = s.letter_spacing;
    g_tab_width = s.tab_width;
    g_insert_spaces = s.insert_spaces;
    g_auto_indent = s.auto_indent;
    g_smart_dedent = s.smart_dedent;
    g_syntax_highlighting = s.syntax_highlighting;
    g_line_numbers_visible = s.line_numbers;
    g_highlight_current_line = s.highlight_current_line;
    g_minimap_visible = s.minimap_visible;
    g_sidebar_visible = s.sidebar_visible;
    g_statusbar_visible = s.statusbar_visible;
    g_cursor_blink = s.cursor_blink;
    g_cursor_style = s.cursor_style;
    g_show_whitespace = s.show_whitespace;
    g_indent_guides = s.indent_guides;
    g_dark_theme = s.dark_theme;
}

static int settings_file_path(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        snprintf(out, out_sz, "%s/luna-editor/settings.conf", base);
        return 1;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;
    snprintf(out, out_sz, "%s/.config/luna-editor/settings.conf", home);
    return 1;
}

static void mkdir_parents_for_file(const char *path) {
    if (!path || !*path) return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *last = strrchr(tmp, '/');
    if (!last) return;
    *last = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return;
        *p = '/';
    }
    (void)mkdir(tmp, 0755);
}

static void settings_save(void) {
    char path[PATH_MAX];
    if (!settings_file_path(path, sizeof(path))) return;
    mkdir_parents_for_file(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "font_size=%.1f\n", g_editor_font_size);
    fprintf(fp, "font_face=%d\n", g_editor_font_face);
    fprintf(fp, "line_height=%.2f\n", g_editor_line_height);
    fprintf(fp, "letter_spacing=%.2f\n", g_editor_letter_spacing);
    fprintf(fp, "tab_width=%d\n", g_tab_width);
    fprintf(fp, "insert_spaces=%d\n", g_insert_spaces);
    fprintf(fp, "auto_indent=%d\n", g_auto_indent);
    fprintf(fp, "smart_dedent=%d\n", g_smart_dedent);
    fprintf(fp, "syntax_highlighting=%d\n", g_syntax_highlighting);
    fprintf(fp, "line_numbers=%d\n", g_line_numbers_visible);
    fprintf(fp, "highlight_current_line=%d\n", g_highlight_current_line);
    fprintf(fp, "minimap=%d\n", g_minimap_visible);
    fprintf(fp, "sidebar=%d\n", g_sidebar_visible);
    fprintf(fp, "statusbar=%d\n", g_statusbar_visible);
    fprintf(fp, "cursor_blink=%d\n", g_cursor_blink);
    fprintf(fp, "cursor_style=%d\n", g_cursor_style);
    fprintf(fp, "show_whitespace=%d\n", g_show_whitespace);
    fprintf(fp, "indent_guides=%d\n", g_indent_guides);
    fprintf(fp, "dark_theme=%d\n", g_dark_theme);
    fclose(fp);
}

static void settings_load(void) {
    EditorSettings s;
    settings_defaults(&s);
    char path[PATH_MAX];
    if (!settings_file_path(path, sizeof(path))) {
        settings_assign(&s);
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        settings_assign(&s);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        char *nl = strpbrk(eq, "\r\n");
        if (nl) *nl = '\0';
        if (strcmp(line, "font_size") == 0) s.font_size = strtof(eq, NULL);
        else if (strcmp(line, "font_face") == 0) s.font_face = atoi(eq);
        else if (strcmp(line, "line_height") == 0) s.line_height = strtof(eq, NULL);
        else if (strcmp(line, "letter_spacing") == 0) s.letter_spacing = strtof(eq, NULL);
        else if (strcmp(line, "tab_width") == 0) s.tab_width = atoi(eq);
        else if (strcmp(line, "insert_spaces") == 0) s.insert_spaces = atoi(eq);
        else if (strcmp(line, "auto_indent") == 0) s.auto_indent = atoi(eq);
        else if (strcmp(line, "smart_dedent") == 0) s.smart_dedent = atoi(eq);
        else if (strcmp(line, "syntax_highlighting") == 0) s.syntax_highlighting = atoi(eq);
        else if (strcmp(line, "line_numbers") == 0) s.line_numbers = atoi(eq);
        else if (strcmp(line, "highlight_current_line") == 0) s.highlight_current_line = atoi(eq);
        else if (strcmp(line, "minimap") == 0) s.minimap_visible = atoi(eq);
        else if (strcmp(line, "sidebar") == 0) s.sidebar_visible = atoi(eq);
        else if (strcmp(line, "statusbar") == 0) s.statusbar_visible = atoi(eq);
        else if (strcmp(line, "cursor_blink") == 0) s.cursor_blink = atoi(eq);
        else if (strcmp(line, "cursor_style") == 0) s.cursor_style = atoi(eq);
        else if (strcmp(line, "show_whitespace") == 0) s.show_whitespace = atoi(eq);
        else if (strcmp(line, "indent_guides") == 0) s.indent_guides = atoi(eq);
        else if (strcmp(line, "dark_theme") == 0) s.dark_theme = atoi(eq);
    }
    fclose(fp);
    settings_assign(&s);
}

static char *xstrdup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void line_init(TextLine *line) {
    memset(line, 0, sizeof(*line));
    line->cap = 32;
    line->data = (char *)calloc(line->cap, 1);
}

static void line_free(TextLine *line) {
    if (!line) return;
    free(line->data);
    memset(line, 0, sizeof(*line));
}

static int line_reserve(TextLine *line, size_t need) {
    if (need + 1 <= line->cap) return 1;
    size_t cap = line->cap ? line->cap : 32;
    while (cap < need + 1) {
        if (cap > SIZE_MAX / 2) return 0;
        cap *= 2;
    }
    char *p = (char *)realloc(line->data, cap);
    if (!p) return 0;
    line->data = p;
    line->cap = cap;
    return 1;
}

static int line_set(TextLine *line, const char *data, size_t len) {
    if (!line_reserve(line, len)) return 0;
    if (len) memcpy(line->data, data, len);
    line->data[len] = '\0';
    line->len = len;
    return 1;
}

static int line_insert(TextLine *line, size_t col, const char *data, size_t len) {
    if (col > line->len) col = line->len;
    if (!line_reserve(line, line->len + len)) return 0;
    memmove(line->data + col + len, line->data + col, line->len - col + 1);
    if (len) memcpy(line->data + col, data, len);
    line->len += len;
    return 1;
}

static void line_delete(TextLine *line, size_t start, size_t end) {
    if (start > line->len) start = line->len;
    if (end > line->len) end = line->len;
    if (end <= start) return;
    memmove(line->data + start, line->data + end, line->len - end + 1);
    line->len -= end - start;
}

static int buffer_reserve_lines(TextBuffer *tb, size_t need) {
    if (need <= tb->cap) return 1;
    size_t cap = tb->cap ? tb->cap : 16;
    while (cap < need) cap *= 2;
    TextLine *p = (TextLine *)realloc(tb->lines, cap * sizeof(TextLine));
    if (!p) return 0;
    tb->lines = p;
    tb->cap = cap;
    return 1;
}

static int buffer_init(TextBuffer *tb) {
    memset(tb, 0, sizeof(*tb));
    if (!buffer_reserve_lines(tb, 1)) return 0;
    line_init(&tb->lines[0]);
    tb->count = 1;
    return tb->lines[0].data != NULL;
}

static void buffer_clear(TextBuffer *tb) {
    if (!tb) return;
    for (size_t i = 0; i < tb->count; i++) line_free(&tb->lines[i]);
    free(tb->lines);
    memset(tb, 0, sizeof(*tb));
}

static int buffer_insert_line(TextBuffer *tb, size_t index, TextLine *line) {
    if (index > tb->count) index = tb->count;
    if (!buffer_reserve_lines(tb, tb->count + 1)) return 0;
    memmove(tb->lines + index + 1, tb->lines + index,
            (tb->count - index) * sizeof(TextLine));
    tb->lines[index] = *line;
    memset(line, 0, sizeof(*line));
    tb->count++;
    return 1;
}

static void buffer_remove_lines(TextBuffer *tb, size_t index, size_t count) {
    if (!count || index >= tb->count) return;
    if (index + count > tb->count) count = tb->count - index;
    for (size_t i = 0; i < count; i++) line_free(&tb->lines[index + i]);
    memmove(tb->lines + index, tb->lines + index + count,
            (tb->count - index - count) * sizeof(TextLine));
    tb->count -= count;
    if (tb->count == 0) {
        if (buffer_reserve_lines(tb, 1)) {
            line_init(&tb->lines[0]);
            tb->count = 1;
        }
    }
}

static int pos_cmp(TextPos a, TextPos b) {
    if (a.line < b.line) return -1;
    if (a.line > b.line) return 1;
    if (a.col < b.col) return -1;
    if (a.col > b.col) return 1;
    return 0;
}

static int pos_equal(TextPos a, TextPos b) {
    return a.line == b.line && a.col == b.col;
}

static void normalize_range(TextPos *a, TextPos *b) {
    if (pos_cmp(*a, *b) > 0) {
        TextPos t = *a;
        *a = *b;
        *b = t;
    }
}

static size_t utf8_prev(const char *s, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)s[pos] & 0xC0) == 0x80)) pos--;
    return pos;
}

static size_t utf8_next(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && (((unsigned char)s[pos] & 0xC0) == 0x80)) pos++;
    return pos;
}

static TextPos buffer_clamp_pos(const TextBuffer *tb, TextPos p) {
    if (!tb || tb->count == 0) return (TextPos){0, 0};
    if (p.line >= tb->count) p.line = tb->count - 1;
    const TextLine *ln = &tb->lines[p.line];
    if (p.col > ln->len) p.col = ln->len;
    while (p.col > 0 && p.col < ln->len &&
           (((unsigned char)ln->data[p.col] & 0xC0) == 0x80)) p.col--;
    return p;
}

static TextPos pos_after_text(TextPos at, const char *text) {
    TextPos p = at;
    const char *s = text ? text : "";
    const char *last = s;
    size_t newlines = 0;
    for (const char *q = s; *q; q++) {
        if (*q == '\n') {
            newlines++;
            last = q + 1;
        }
    }
    if (newlines == 0) p.col += strlen(s);
    else {
        p.line += newlines;
        p.col = strlen(last);
    }
    return p;
}

static char *normalize_newlines(const char *text) {
    if (!text) return xstrdup("");
    size_t n = strlen(text);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r') {
            if (i + 1 < n && text[i + 1] == '\n') i++;
            out[w++] = '\n';
        } else {
            out[w++] = text[i];
        }
    }
    out[w] = '\0';
    return out;
}

static TextPos buffer_insert_text(TextBuffer *tb, TextPos at, const char *text) {
    at = buffer_clamp_pos(tb, at);
    char *norm = normalize_newlines(text);
    if (!norm) return at;
    TextLine *cur = &tb->lines[at.line];
    const char *segment = norm;
    TextPos p = at;

    for (const char *q = norm;; q++) {
        if (*q != '\n' && *q != '\0') continue;
        size_t seg_len = (size_t)(q - segment);
        if (!line_insert(cur, p.col, segment, seg_len)) break;
        p.col += seg_len;
        if (*q == '\0') break;

        TextLine tail;
        line_init(&tail);
        if (!tail.data || !line_set(&tail, cur->data + p.col, cur->len - p.col)) {
            line_free(&tail);
            break;
        }
        line_delete(cur, p.col, cur->len);
        if (!buffer_insert_line(tb, p.line + 1, &tail)) {
            line_free(&tail);
            break;
        }
        p.line++;
        p.col = 0;
        cur = &tb->lines[p.line];
        segment = q + 1;
    }
    free(norm);
    return p;
}

static char *buffer_get_range(const TextBuffer *tb, TextPos a, TextPos b) {
    a = buffer_clamp_pos(tb, a);
    b = buffer_clamp_pos(tb, b);
    normalize_range(&a, &b);
    if (pos_equal(a, b)) return xstrdup("");

    size_t total = 1;
    if (a.line == b.line) total += b.col - a.col;
    else {
        total += tb->lines[a.line].len - a.col + 1;
        for (size_t i = a.line + 1; i < b.line; i++) total += tb->lines[i].len + 1;
        total += b.col;
    }
    char *out = (char *)malloc(total);
    if (!out) return NULL;
    size_t w = 0;
    if (a.line == b.line) {
        size_t n = b.col - a.col;
        memcpy(out + w, tb->lines[a.line].data + a.col, n);
        w += n;
    } else {
        size_t n = tb->lines[a.line].len - a.col;
        memcpy(out + w, tb->lines[a.line].data + a.col, n);
        w += n;
        out[w++] = '\n';
        for (size_t i = a.line + 1; i < b.line; i++) {
            memcpy(out + w, tb->lines[i].data, tb->lines[i].len);
            w += tb->lines[i].len;
            out[w++] = '\n';
        }
        memcpy(out + w, tb->lines[b.line].data, b.col);
        w += b.col;
    }
    out[w] = '\0';
    return out;
}

static void buffer_delete_range(TextBuffer *tb, TextPos a, TextPos b) {
    a = buffer_clamp_pos(tb, a);
    b = buffer_clamp_pos(tb, b);
    normalize_range(&a, &b);
    if (pos_equal(a, b)) return;
    if (a.line == b.line) {
        line_delete(&tb->lines[a.line], a.col, b.col);
        return;
    }

    TextLine *first = &tb->lines[a.line];
    TextLine *last = &tb->lines[b.line];
    size_t suffix_len = last->len - b.col;
    char *suffix = (char *)malloc(suffix_len + 1);
    if (!suffix) return;
    memcpy(suffix, last->data + b.col, suffix_len);
    suffix[suffix_len] = '\0';
    line_delete(first, a.col, first->len);
    line_insert(first, first->len, suffix, suffix_len);
    free(suffix);
    buffer_remove_lines(tb, a.line + 1, b.line - a.line);
}

static int buffer_load_file(TextBuffer *tb, const char *path, int *binary) {
    if (binary) *binary = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return 0; }
    rewind(fp);
    char *data = (char *)malloc((size_t)sz + 1);
    if (!data) { fclose(fp); return 0; }
    size_t got = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    data[got] = '\0';
    if (memchr(data, '\0', got)) {
        if (binary) *binary = 1;
        free(data);
        return 0;
    }

    buffer_clear(tb);
    memset(tb, 0, sizeof(*tb));
    if (!buffer_reserve_lines(tb, 64)) { free(data); return 0; }
    const char *start = data;
    for (size_t i = 0; i <= got; i++) {
        if (i != got && data[i] != '\n') continue;
        size_t len = (size_t)(data + i - start);
        if (len && start[len - 1] == '\r') len--;
        if (!buffer_reserve_lines(tb, tb->count + 1)) { free(data); return 0; }
        line_init(&tb->lines[tb->count]);
        if (!tb->lines[tb->count].data || !line_set(&tb->lines[tb->count], start, len)) {
            free(data); return 0;
        }
        tb->count++;
        start = data + i + 1;
    }
    if (tb->count == 0) {
        buffer_reserve_lines(tb, 1);
        line_init(&tb->lines[0]);
        tb->count = 1;
    }
    free(data);
    return 1;
}

static int buffer_save_file(const TextBuffer *tb, const char *path) {
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.luna-tmp-%ld", path, (long)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return 0;
    int ok = 1;
    for (size_t i = 0; i < tb->count; i++) {
        if (tb->lines[i].len &&
            fwrite(tb->lines[i].data, 1, tb->lines[i].len, fp) != tb->lines[i].len) {
            ok = 0; break;
        }
        if (i + 1 < tb->count && fputc('\n', fp) == EOF) { ok = 0; break; }
    }
    if (fflush(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    if (!ok) { unlink(tmp); return 0; }

    struct stat st;
    if (stat(path, &st) == 0) chmod(tmp, st.st_mode & 0777);
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Undo / document operations                                                 */
/* ------------------------------------------------------------------------- */

static void undo_clear(UndoStack *us) {
    if (!us) return;
    for (size_t i = 0; i < us->count; i++) free(us->items[i].text);
    free(us->items);
    memset(us, 0, sizeof(*us));
    us->saved_cursor = 0;
}

static int undo_reserve(UndoStack *us, size_t need) {
    if (need <= us->cap) return 1;
    size_t cap = us->cap ? us->cap * 2 : 64;
    while (cap < need) cap *= 2;
    UndoEdit *p = (UndoEdit *)realloc(us->items, cap * sizeof(UndoEdit));
    if (!p) return 0;
    us->items = p;
    us->cap = cap;
    return 1;
}

static void undo_push(UndoStack *us, EditKind kind, TextPos at, const char *text) {
    if (!text || !*text) return;
    while (us->count > us->cursor) {
        free(us->items[--us->count].text);
    }
    if (us->saved_cursor > (ssize_t)us->cursor) us->saved_cursor = -1;
    if (us->count >= MAX_UNDO) {
        free(us->items[0].text);
        memmove(us->items, us->items + 1, (us->count - 1) * sizeof(UndoEdit));
        us->count--;
        if (us->cursor) us->cursor--;
        if (us->saved_cursor > 0) us->saved_cursor--;
        else if (us->saved_cursor == 0) us->saved_cursor = -1;
    }
    if (!undo_reserve(us, us->count + 1)) return;
    us->items[us->count++] = (UndoEdit){kind, at, xstrdup(text)};
    us->cursor = us->count;
}

static int doc_is_dirty(const Document *doc) {
    return doc && doc->undo.saved_cursor != (ssize_t)doc->undo.cursor;
}

static int doc_has_selection(const Document *doc) {
    return doc && !pos_equal(doc->caret, doc->anchor);
}

static void doc_selection(const Document *doc, TextPos *a, TextPos *b) {
    *a = doc->anchor;
    *b = doc->caret;
    normalize_range(a, b);
}

static void doc_set_caret(Document *doc, TextPos p, int keep_anchor) {
    doc->caret = buffer_clamp_pos(&doc->buffer, p);
    if (!keep_anchor) doc->anchor = doc->caret;
    doc->desired_x_valid = 0;
    request_redraw();
}

static void doc_delete_selection(Document *doc, int record) {
    if (!doc_has_selection(doc) || doc->read_only) return;
    TextPos a, b;
    doc_selection(doc, &a, &b);
    char *deleted = buffer_get_range(&doc->buffer, a, b);
    buffer_delete_range(&doc->buffer, a, b);
    if (record && deleted) undo_push(&doc->undo, EDIT_DELETE, a, deleted);
    free(deleted);
    doc->caret = doc->anchor = a;
    doc->desired_x_valid = 0;
    request_redraw();
}

static void doc_insert_text(Document *doc, const char *text, int record) {
    if (!doc || doc->read_only || !text || !*text) return;
    if (doc_has_selection(doc)) doc_delete_selection(doc, record);
    char *norm = normalize_newlines(text);
    if (!norm) return;
    TextPos at = doc->caret;
    TextPos end = buffer_insert_text(&doc->buffer, at, norm);
    if (record) undo_push(&doc->undo, EDIT_INSERT, at, norm);
    doc->caret = doc->anchor = end;
    doc->desired_x_valid = 0;
    free(norm);
    request_redraw();
}

static void doc_delete_range(Document *doc, TextPos a, TextPos b, int record) {
    if (!doc || doc->read_only) return;
    a = buffer_clamp_pos(&doc->buffer, a);
    b = buffer_clamp_pos(&doc->buffer, b);
    normalize_range(&a, &b);
    if (pos_equal(a, b)) return;
    char *deleted = buffer_get_range(&doc->buffer, a, b);
    buffer_delete_range(&doc->buffer, a, b);
    if (record && deleted) undo_push(&doc->undo, EDIT_DELETE, a, deleted);
    free(deleted);
    doc->caret = doc->anchor = a;
    doc->desired_x_valid = 0;
    request_redraw();
}

static void doc_undo(Document *doc) {
    if (!doc || doc->read_only || doc->undo.cursor == 0) return;
    UndoEdit *e = &doc->undo.items[doc->undo.cursor - 1];
    if (e->kind == EDIT_INSERT) {
        TextPos end = pos_after_text(e->at, e->text);
        buffer_delete_range(&doc->buffer, e->at, end);
        doc->caret = doc->anchor = e->at;
    } else {
        TextPos end = buffer_insert_text(&doc->buffer, e->at, e->text);
        doc->caret = doc->anchor = end;
    }
    doc->undo.cursor--;
    doc->desired_x_valid = 0;
    request_redraw();
}

static void doc_redo(Document *doc) {
    if (!doc || doc->read_only || doc->undo.cursor >= doc->undo.count) return;
    UndoEdit *e = &doc->undo.items[doc->undo.cursor];
    if (e->kind == EDIT_INSERT) {
        TextPos end = buffer_insert_text(&doc->buffer, e->at, e->text);
        doc->caret = doc->anchor = end;
    } else {
        TextPos end = pos_after_text(e->at, e->text);
        buffer_delete_range(&doc->buffer, e->at, end);
        doc->caret = doc->anchor = e->at;
    }
    doc->undo.cursor++;
    doc->desired_x_valid = 0;
    request_redraw();
}

static const char *path_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

static const char *path_ext(const char *path) {
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

static int str_ieq(const char *a, const char *b) {
    return strcasecmp(a ? a : "", b ? b : "") == 0;
}

static Language language_for_path(const char *path) {
    const char *e = path_ext(path);
    if (str_ieq(e, "c")) return LANG_C;
    if (str_ieq(e, "h") || str_ieq(e, "hpp") || str_ieq(e, "hh") ||
        str_ieq(e, "cc") || str_ieq(e, "cpp") || str_ieq(e, "cxx")) return LANG_CPP;
    if (str_ieq(e, "py") || str_ieq(e, "pyw")) return LANG_PYTHON;
    if (str_ieq(e, "js") || str_ieq(e, "jsx") || str_ieq(e, "ts") || str_ieq(e, "tsx")) return LANG_JS;
    if (str_ieq(e, "json") || str_ieq(e, "jsonc")) return LANG_JSON;
    if (str_ieq(e, "html") || str_ieq(e, "htm") || str_ieq(e, "xml") || str_ieq(e, "svg")) return LANG_HTML;
    if (str_ieq(e, "css") || str_ieq(e, "scss") || str_ieq(e, "less")) return LANG_CSS;
    if (str_ieq(e, "md") || str_ieq(e, "markdown")) return LANG_MARKDOWN;
    if (str_ieq(e, "sh") || str_ieq(e, "bash") || str_ieq(e, "zsh")) return LANG_SHELL;
    return LANG_TEXT;
}

static const char *language_name(Language lang) {
    switch (lang) {
        case LANG_C: return "C";
        case LANG_CPP: return "C++";
        case LANG_PYTHON: return "Python";
        case LANG_JS: return "JavaScript";
        case LANG_JSON: return "JSON";
        case LANG_HTML: return "HTML";
        case LANG_CSS: return "CSS";
        case LANG_MARKDOWN: return "Markdown";
        case LANG_SHELL: return "Shell";
        default: return "Plain Text";
    }
}

static void doc_init(Document *doc, const char *name) {
    memset(doc, 0, sizeof(*doc));
    buffer_init(&doc->buffer);
    doc->undo.saved_cursor = 0;
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", name ? name : "Untitled");
    doc->language = LANG_TEXT;
}

static void doc_free(Document *doc) {
    if (!doc) return;
    buffer_clear(&doc->buffer);
    undo_clear(&doc->undo);
    memset(doc, 0, sizeof(*doc));
}

static Document *active_doc(void) {
    if (g_active_doc < 0 || g_active_doc >= g_doc_count) return NULL;
    return &g_docs[g_active_doc];
}

/* ------------------------------------------------------------------------- */
/* Document and explorer management                                           */
/* ------------------------------------------------------------------------- */

static int absolute_path(const char *path, char *out, size_t out_sz) {
    if (!path || !*path || !out || out_sz == 0) return 0;
    if (path[0] == '/') {
        snprintf(out, out_sz, "%s", path);
        return 1;
    }
    if (!getcwd(out, out_sz)) return 0;
    size_t n = strlen(out);
    if (n + 1 < out_sz && out[n - 1] != '/') out[n++] = '/';
    snprintf(out + n, out_sz - n, "%s", path);
    return 1;
}

static int find_open_document(const char *path) {
    char abs[PATH_MAX];
    if (!absolute_path(path, abs, sizeof(abs))) return -1;
    char resolved[PATH_MAX];
    const char *cmp = realpath(abs, resolved) ? resolved : abs;
    for (int i = 0; i < g_doc_count; i++) {
        if (!g_docs[i].path[0]) continue;
        char existing[PATH_MAX];
        const char *ep = realpath(g_docs[i].path, existing) ? existing : g_docs[i].path;
        if (strcmp(ep, cmp) == 0) return i;
    }
    return -1;
}

static void update_ui(void);
static void update_settings_dialog(void);
static void ensure_caret_visible(Document *doc);
static void platform_maximize(void);
static size_t visual_col_at(const char *s, size_t byte_col);
static void apply_visual_settings(void);

static void switch_document(int index) {
    if (index < 0 || index >= g_doc_count) return;
    g_active_doc = index;
    g_editor_focused = 1;
    update_ui();
    request_followup_redraw(2);
}

static int new_document(void) {
    if (g_doc_count >= MAX_DOCS) {
        statusf("開けるタブは最大%d個です", MAX_DOCS);
        return -1;
    }
    char name[64];
    snprintf(name, sizeof(name), "Untitled %d", g_doc_count + 1);
    doc_init(&g_docs[g_doc_count], name);
    g_active_doc = g_doc_count++;
    update_ui();
    luna_mark_layout_dirty();
    request_followup_redraw(2);
    return g_active_doc;
}

static int open_document(const char *path) {
    if (!path || !*path) return 0;
    int existing = find_open_document(path);
    if (existing >= 0) {
        switch_document(existing);
        return 1;
    }
    if (g_doc_count >= MAX_DOCS) {
        statusf("タブ上限のためファイルを開けません");
        return 0;
    }
    char abs[PATH_MAX];
    if (!absolute_path(path, abs, sizeof(abs))) {
        statusf("パスを解決できません: %s", path);
        return 0;
    }
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
        statusf("ファイルが見つかりません: %s", abs);
        return 0;
    }
    Document *doc = &g_docs[g_doc_count];
    doc_init(doc, path_basename(abs));
    int binary = 0;
    if (!buffer_load_file(&doc->buffer, abs, &binary)) {
        doc_free(doc);
        statusf(binary ? "バイナリファイルは開けません" : "開けません: %s", abs);
        return 0;
    }
    snprintf(doc->path, sizeof(doc->path), "%s", abs);
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", path_basename(abs));
    doc->language = language_for_path(abs);
    doc->read_only = access(abs, W_OK) != 0;
    doc->undo.saved_cursor = 0;
    g_active_doc = g_doc_count++;
    update_ui();
    /* Resolve the tab/editor flex geometry once before the first visible frame.
     * A second normal pass is still requested below for font-atlas/layout work
     * that can be discovered while painting the newly loaded document. */
    luna_mark_layout_dirty();
    luna_update(glfwGetTime(), 0.0);
    luna_mark_layout_dirty();
    statusf("%zu行を読み込みました", doc->buffer.count);
    request_followup_redraw(2);
    return 1;
}

static int save_document_to(Document *doc, const char *path) {
    if (!doc || !path || !*path) return 0;
    char abs[PATH_MAX];
    if (!absolute_path(path, abs, sizeof(abs))) return 0;
    if (!buffer_save_file(&doc->buffer, abs)) {
        statusf("保存できません: %s", strerror(errno));
        return 0;
    }
    snprintf(doc->path, sizeof(doc->path), "%s", abs);
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", path_basename(abs));
    doc->language = language_for_path(abs);
    doc->read_only = 0;
    doc->undo.saved_cursor = (ssize_t)doc->undo.cursor;
    update_ui();
    statusf("保存しました: %s", doc->display_name);
    return 1;
}

static int save_active_document(void) {
    Document *doc = active_doc();
    if (!doc) return 0;
    if (!doc->path[0]) return 0;
    return save_document_to(doc, doc->path);
}

static void close_document_at(int index, int force) {
    if (index < 0 || index >= g_doc_count) return;
    Document *doc = &g_docs[index];
    if (!force && doc_is_dirty(doc)) {
        statusf("%s は未保存です。保存してから閉じてください", doc->display_name);
        return;
    }

    doc_free(doc);
    memmove(&g_docs[index], &g_docs[index + 1],
            (size_t)(g_doc_count - index - 1) * sizeof(Document));
    g_doc_count--;

    if (g_doc_count == 0) {
        g_active_doc = -1;
        new_document();
        return;
    }
    if (g_active_doc > index) g_active_doc--;
    else if (g_active_doc == index && g_active_doc >= g_doc_count)
        g_active_doc = g_doc_count - 1;

    update_ui();
    luna_mark_layout_dirty();
    request_followup_redraw(2);
}

static void close_active_document(int force) {
    close_document_at(g_active_doc, force);
}

static int has_unsaved_documents(void) {
    for (int i = 0; i < g_doc_count; i++) if (doc_is_dirty(&g_docs[i])) return 1;
    return 0;
}

static int is_text_candidate(const char *name) {
    const char *e = path_ext(name);
    if (!*e) return 1;
    static const char *exts[] = {
        "c","h","cc","cpp","cxx","hh","hpp","py","js","jsx","ts","tsx",
        "json","jsonc","html","htm","xml","svg","css","scss","less","md",
        "markdown","txt","log","ini","conf","cfg","toml","yaml","yml","sh",
        "bash","zsh","sql","rs","go","java","kt","lua","rb","php","tex"
    };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (str_ieq(e, exts[i])) return 1;
    return 0;
}

static int explorer_cmp(const void *aa, const void *bb) {
    const ExplorerEntry *a = (const ExplorerEntry *)aa;
    const ExplorerEntry *b = (const ExplorerEntry *)bb;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return strcasecmp(a->name, b->name);
}

static void scan_explorer(void) {
    g_entry_count = 0;
    DIR *dir = opendir(g_working_dir);
    if (!dir) {
        statusf("フォルダを開けません: %s", g_working_dir);
        return;
    }
    if (strcmp(g_working_dir, "/") != 0 && g_entry_count < MAX_EXPLORER_ITEMS) {
        ExplorerEntry *e = &g_entries[g_entry_count++];
        snprintf(e->name, sizeof(e->name), "..  Parent Folder");
        snprintf(e->path, sizeof(e->path), "%s/..", g_working_dir);
        e->is_dir = 1;
    }
    struct dirent *de;
    ExplorerEntry temp[256];
    int count = 0;
    while ((de = readdir(dir)) != NULL && count < (int)(sizeof(temp) / sizeof(temp[0]))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", g_working_dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        if (S_ISREG(st.st_mode) && !is_text_candidate(de->d_name)) continue;
        ExplorerEntry *e = &temp[count++];
        snprintf(e->name, sizeof(e->name), "%s%s", S_ISDIR(st.st_mode) ? "▸  " : "   ", de->d_name);
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    }
    closedir(dir);
    qsort(temp, (size_t)count, sizeof(temp[0]), explorer_cmp);
    int room = MAX_EXPLORER_ITEMS - g_entry_count;
    if (count > room) count = room;
    memcpy(&g_entries[g_entry_count], temp, (size_t)count * sizeof(temp[0]));
    g_entry_count += count;
    update_ui();
}

static void explorer_activate(int index) {
    if (index < 0 || index >= g_entry_count) return;
    ExplorerEntry *e = &g_entries[index];
    if (e->is_dir) {
        char resolved[PATH_MAX];
        if (!realpath(e->path, resolved)) {
            statusf("フォルダを開けません");
            return;
        }
        snprintf(g_working_dir, sizeof(g_working_dir), "%s", resolved);
        scan_explorer();
    } else {
        open_document(e->path);
    }
}

/* ------------------------------------------------------------------------- */
/* UI document                                                                */
/* ------------------------------------------------------------------------- */

static const char *APP_HTML =
"<div id=\"app\" class=\"app\">"
"  <div id=\"titlebar\" class=\"toolbar\">"
"    <div class=\"tool-group\">"
"      <button class=\"tool-btn primary\" onclick=\"new_doc\">＋ New</button>"
"      <button class=\"tool-btn\" onclick=\"open_file\">Open</button>"
"      <button class=\"tool-btn\" onclick=\"save_doc\">Save</button>"
"    </div>"
"    <div class=\"tool-divider\"></div>"
"    <div class=\"tool-group compact\">"
"      <button class=\"tool-btn icon-only\" onclick=\"undo_doc\" title=\"Undo\">↶</button>"
"      <button class=\"tool-btn icon-only\" onclick=\"redo_doc\" title=\"Redo\">↷</button>"
"      <button class=\"tool-btn\" onclick=\"show_find\">Find</button>"
"    </div>"
"    <div id=\"appTitle\" class=\"app-title\">Luna Editor</div>"
"    <div class=\"tool-group compact right-tools\">"
"      <button class=\"tool-btn\" onclick=\"toggle_sidebar\">Sidebar</button>"
"      <button class=\"tool-btn\" onclick=\"toggle_minimap\">Minimap</button>"
"      <button class=\"tool-btn icon-only settings-button\" onclick=\"settings_open\" title=\"Settings (Ctrl+,)\">⚙</button>"
"    </div>"
"  </div>"
"  <div id=\"body\" class=\"body\">"
"    <div id=\"sidebar\" class=\"sidebar\">"
"      <div class=\"sidebar-head\"><span>EXPLORER</span><button class=\"side-action\" onclick=\"open_folder\">＋</button></div>"
"      <div id=\"folderName\" class=\"folder-name\">Folder</div>"
"      <div class=\"file-list\">"
"        <button id=\"file0\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file1\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file2\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file3\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file4\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file5\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file6\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file7\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file8\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file9\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file10\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file11\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file12\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file13\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file14\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"        <button id=\"file15\" class=\"file-item hidden\" onclick=\"file_activate\"></button>"
"      </div>"
"    </div>"
"    <div id=\"workspace\" class=\"workspace\">"
"      <div id=\"tabs\" class=\"tabs\">"
"        <div class=\"tab-strip\">"
"          <div id=\"tabSlot0\" class=\"tab-slot hidden\"><button id=\"tab0\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab0\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot1\" class=\"tab-slot hidden\"><button id=\"tab1\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab1\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot2\" class=\"tab-slot hidden\"><button id=\"tab2\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab2\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot3\" class=\"tab-slot hidden\"><button id=\"tab3\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab3\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot4\" class=\"tab-slot hidden\"><button id=\"tab4\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab4\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot5\" class=\"tab-slot hidden\"><button id=\"tab5\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab5\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot6\" class=\"tab-slot hidden\"><button id=\"tab6\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab6\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"          <div id=\"tabSlot7\" class=\"tab-slot hidden\"><button id=\"tab7\" class=\"tab\" onclick=\"tab_activate\"></button><button id=\"closeTab7\" class=\"tab-close-inline\" onclick=\"close_tab_activate\" title=\"Close\">×</button></div>"
"        </div>"
"        <button class=\"new-tab\" onclick=\"new_doc\">＋</button>"
"      </div>"
"      <div id=\"editorHost\" class=\"editor-host\" tabindex=\"0\"></div>"
"    </div>"
"  </div>"
"  <div id=\"statusbar\" class=\"statusbar\">"
"    <div id=\"statusLeft\" class=\"status-left\">Ready</div>"
"    <div id=\"statusCenter\" class=\"status-center\"></div>"
"    <div id=\"statusRight\" class=\"status-right\">Ln 1, Col 1</div>"
"  </div>"
"  <div id=\"settingsOverlay\" class=\"settings-overlay hidden\">"
"    <div id=\"settingsCard\" class=\"settings-card\" tabindex=\"0\">"
"      <div class=\"settings-head\"><div><div class=\"settings-title\">設定</div><div class=\"settings-subtitle\">表示と編集動作をカスタマイズします</div></div><button class=\"settings-close\" onclick=\"settings_cancel\">×</button></div>"
"      <div class=\"settings-columns\">"
"        <div class=\"settings-column\">"
"          <div class=\"settings-section-title\">外観</div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">テーマ</div><div class=\"setting-help\">エディタ全体の配色</div></div><div class=\"setting-stepper\"><button onclick=\"settings_theme_prev\">‹</button><span id=\"settingThemeValue\" class=\"setting-value\">ライト</span><button onclick=\"settings_theme_next\">›</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">エディタフォント</div><div class=\"setting-help\">本文の書体（初期値は等幅）</div></div><div class=\"setting-stepper\"><button onclick=\"settings_font_family_prev\">‹</button><span id=\"settingFontFamilyValue\" class=\"setting-value extra-wide\">等幅</span><button onclick=\"settings_font_family_next\">›</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">文字サイズ</div><div class=\"setting-help\">10〜28 px</div></div><div class=\"setting-stepper\"><button onclick=\"settings_font_down\">−</button><span id=\"settingFontValue\" class=\"setting-value\">14 px</span><button onclick=\"settings_font_up\">＋</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">行間</div><div class=\"setting-help\">120〜200%</div></div><div class=\"setting-stepper\"><button onclick=\"settings_line_height_down\">−</button><span id=\"settingLineHeightValue\" class=\"setting-value\">152%</span><button onclick=\"settings_line_height_up\">＋</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">字間</div><div class=\"setting-help\">−0.5〜2.0 px</div></div><div class=\"setting-stepper\"><button onclick=\"settings_letter_spacing_down\">−</button><span id=\"settingLetterSpacingValue\" class=\"setting-value wide\">0.0 px</span><button onclick=\"settings_letter_spacing_up\">＋</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">行番号</div><div class=\"setting-help\">左端に行番号を表示</div></div><button id=\"settingLineNumbersValue\" class=\"setting-toggle\" onclick=\"settings_toggle_line_numbers\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">現在行の強調</div><div class=\"setting-help\">カーソル行に背景色を表示</div></div><button id=\"settingCurrentLineValue\" class=\"setting-toggle\" onclick=\"settings_toggle_current_line\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">ミニマップ</div><div class=\"setting-help\">右側に文書全体を表示</div></div><button id=\"settingMinimapValue\" class=\"setting-toggle\" onclick=\"settings_toggle_minimap\">OFF</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">サイドバー</div><div class=\"setting-help\">ファイル一覧を表示</div></div><button id=\"settingSidebarValue\" class=\"setting-toggle\" onclick=\"settings_toggle_sidebar\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">ステータスバー</div><div class=\"setting-help\">行・列・言語情報を表示</div></div><button id=\"settingStatusbarValue\" class=\"setting-toggle\" onclick=\"settings_toggle_statusbar\">ON</button></div>"
"        </div>"
"        <div class=\"settings-column\">"
"          <div class=\"settings-section-title\">編集</div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">タブ幅</div><div class=\"setting-help\">2・4・8文字から選択</div></div><div class=\"setting-stepper\"><button onclick=\"settings_tab_prev\">‹</button><span id=\"settingTabValue\" class=\"setting-value\">4</span><button onclick=\"settings_tab_next\">›</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">インデント</div><div class=\"setting-help\">スペースまたはタブ文字</div></div><div class=\"setting-stepper\"><button onclick=\"settings_indent_prev\">‹</button><span id=\"settingIndentValue\" class=\"setting-value wide\">スペース</span><button onclick=\"settings_indent_next\">›</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">自動インデント</div><div class=\"setting-help\">改行時に字下げを継承</div></div><button id=\"settingAutoIndentValue\" class=\"setting-toggle\" onclick=\"settings_toggle_auto_indent\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">スマート逆インデント</div><div class=\"setting-help\">閉じ括弧入力時に字下げを戻す</div></div><button id=\"settingSmartDedentValue\" class=\"setting-toggle\" onclick=\"settings_toggle_smart_dedent\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">シンタックス強調</div><div class=\"setting-help\">言語に合わせて色分け</div></div><button id=\"settingSyntaxValue\" class=\"setting-toggle\" onclick=\"settings_toggle_syntax\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">空白文字を表示</div><div class=\"setting-help\">スペースとタブを薄く表示</div></div><button id=\"settingWhitespaceValue\" class=\"setting-toggle\" onclick=\"settings_toggle_whitespace\">OFF</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">インデントガイド</div><div class=\"setting-help\">字下げ位置に縦線を表示</div></div><button id=\"settingIndentGuidesValue\" class=\"setting-toggle\" onclick=\"settings_toggle_indent_guides\">ON</button></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">カーソル形状</div><div class=\"setting-help\">ライン・ブロック・下線</div></div><div class=\"setting-stepper\"><button onclick=\"settings_cursor_prev\">‹</button><span id=\"settingCursorStyleValue\" class=\"setting-value wide\">ライン</span><button onclick=\"settings_cursor_next\">›</button></div></div>"
"          <div class=\"setting-row\"><div class=\"setting-copy\"><div class=\"setting-name\">カーソル点滅</div><div class=\"setting-help\">入力位置を点滅表示</div></div><button id=\"settingCursorBlinkValue\" class=\"setting-toggle\" onclick=\"settings_toggle_cursor_blink\">ON</button></div>"
"          <div class=\"settings-shortcut\"><span>ショートカット</span><strong>Ctrl + ,</strong></div>"
"        </div>"
"      </div>"
"      <div class=\"settings-actions\"><button class=\"settings-reset\" onclick=\"settings_reset\">初期設定に戻す</button><div class=\"settings-actions-right\"><button class=\"settings-cancel\" onclick=\"settings_cancel\">キャンセル</button><button class=\"settings-apply\" onclick=\"settings_apply\">適用</button></div></div>"
"    </div>"
"  </div>"
"  <div id=\"promptOverlay\" class=\"prompt-overlay hidden\">"
"    <div id=\"promptCard\" class=\"prompt-card\">"
"      <div id=\"promptTitle\" class=\"prompt-title\">Open</div>"
"      <input id=\"promptInput\" class=\"prompt-input\" type=\"text\" onclick=\"prompt_submit\" placeholder=\"Type here…\">"
"      <div class=\"prompt-actions\">"
"        <button class=\"prompt-cancel\" onclick=\"prompt_cancel\">Cancel</button>"
"        <button class=\"prompt-ok\" onclick=\"prompt_submit\">OK</button>"
"      </div>"
"    </div>"
"  </div>"
"</div>";

static const char *APP_CSS =
"* { box-sizing:border-box; }"
"html,body { width:100%; height:100%; margin:0; padding:0; overflow:hidden; background:#f3f4f6; color:#20242c; font-family:Inter,sans-serif; }"
"button,input { font-family:Inter,sans-serif; }"
".app { width:100%; height:100%; display:flex; flex-direction:column; background:#f3f4f6; }"
".toolbar { height:42px; min-height:42px; display:flex; align-items:center; padding:0 8px; background:#f8f9fb; border-bottom:1px solid #d8dce3; }"
".tool-group { display:flex; align-items:center; gap:5px; }"
".tool-group.compact { gap:3px; }"
".tool-divider { width:1px; height:22px; margin:0 7px; background:#d9dde4; }"
".tool-btn { height:29px; min-width:48px; padding:0 10px; border:1px solid #d2d7df; border-radius:6px; background:#ffffff; color:#404754; font-size:12px; }"
".tool-btn:hover { background:#eef1f5; border-color:#c4cad4; color:#202630; }"
".tool-btn:active { background:#e4e8ee; }"
".tool-btn.primary { border-color:#5369d8; background:#6175df; color:#ffffff; }"
".tool-btn.primary:hover { background:#566bd7; }"
".tool-btn.icon-only { min-width:31px; width:31px; padding:0; font-size:17px; }"
".app-title { flex:1; min-width:80px; padding:0 12px; text-align:center; color:#555d6b; font-size:12px; font-weight:600; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".right-tools { margin-left:auto; }"
".body { flex:1; min-height:0; display:flex; background:#ffffff; }"
".sidebar { width:218px; min-width:180px; max-width:300px; display:flex; flex-direction:column; background:#f4f5f7; border-right:1px solid #d9dde4; }"
".sidebar.hidden { display:none; }"
".sidebar-head { height:34px; min-height:34px; padding:0 8px 0 12px; display:flex; align-items:center; justify-content:space-between; color:#727986; font-size:10px; font-weight:700; letter-spacing:1px; }"
".side-action { width:25px; height:25px; border:0; border-radius:5px; background:transparent; color:#596273; font-size:17px; padding:0; }"
".side-action:hover { background:#e3e6eb; }"
".folder-name { min-height:31px; padding:8px 12px 7px; border-top:1px solid #e3e6eb; border-bottom:1px solid #dde1e7; color:#343a45; font-size:12px; font-weight:600; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".file-list { flex:1; min-height:0; overflow:hidden; padding:4px 6px 7px; }"
".file-item { width:100%; height:27px; display:block; padding:0 8px; border:0; border-radius:5px; background:transparent; color:#4c5360; text-align:left; font-size:12px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".file-item:hover { background:#e4e7ec; color:#202630; }"
".workspace { flex:1; min-width:0; min-height:0; display:flex; flex-direction:column; background:#ffffff; }"
".tabs { height:35px; min-height:35px; display:flex; align-items:stretch; background:#eef0f3; border-bottom:1px solid #d9dde4; }"
".tab-strip { flex:1; min-width:0; display:flex; overflow:hidden; }"
".tab-slot { min-width:104px; max-width:190px; height:35px; display:flex; align-items:stretch; border-right:1px solid #d9dde4; background:transparent; }"
".tab-slot:hover { background:#f7f8fa; }"
".tab-slot.active { background:#ffffff; border-top:2px solid #6477dd; }"
".tab { flex:1; min-width:0; height:35px; padding:0 3px 0 12px; border:0; border-radius:0; background:transparent; color:#676f7c; text-align:left; font-size:12px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".tab:hover { color:#303641; }"
".tab-slot.active .tab { height:33px; color:#202630; font-weight:600; }"
".tab-close-inline { width:27px; min-width:27px; height:35px; padding:0 5px 0 2px; border:0; border-radius:0; background:transparent; color:#89919e; font-size:14px; }"
".tab-close-inline:hover { background:#e5e8ed; color:#303641; }"
".tab-slot.active .tab-close-inline { height:33px; }"
".new-tab { width:34px; min-width:34px; border:0; border-left:1px solid #d9dde4; background:#eef0f3; color:#687181; font-size:16px; padding:0; }"
".new-tab:hover { background:#e1e4e9; color:#202630; }"
".editor-host { flex:1; min-height:0; position:relative; overflow:hidden; background:#ffffff; cursor:text; }"
".statusbar { height:23px; min-height:23px; padding:0 9px; display:flex; align-items:center; background:#eceff3; border-top:1px solid #d7dbe2; color:#626a78; font-size:11px; }"
".status-left { width:38%; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".status-center { flex:1; text-align:center; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".status-right { width:38%; text-align:right; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".prompt-overlay { position:fixed; left:0; top:0; width:100%; height:100%; display:flex; align-items:flex-start; justify-content:center; padding-top:74px; background:rgba(24,30,40,.24); z-index:900; }"
".prompt-card { width:500px; max-width:84%; padding:16px; display:flex; flex-direction:column; gap:11px; border:1px solid #cbd1da; border-radius:10px; background:#ffffff; box-shadow:0 12px 32px rgba(20,28,42,.22); }"
".prompt-title { font-size:14px; font-weight:700; color:#252b35; }"
".prompt-input { width:100%; height:38px; padding:0 11px; border:1px solid #c7cdd7; border-radius:7px; background:#ffffff; color:#222936; font-size:13px; caret-color:#5268dd; }"
".prompt-input:focus { border-color:#7182df; }"
".prompt-actions { display:flex; justify-content:flex-end; gap:7px; }"
".prompt-cancel,.prompt-ok { height:30px; padding:0 14px; border-radius:6px; font-size:12px; }"
".prompt-cancel { border:1px solid #d0d5dd; background:#f5f6f8; color:#555e6c; }"
".prompt-ok { border:1px solid #5367d5; background:#6175df; color:#ffffff; }"
".settings-button { font-size:15px; }"
".settings-overlay { position:fixed; left:0; top:0; width:100%; height:100%; display:flex; align-items:center; justify-content:center; padding:28px; background:rgba(24,30,40,.32); z-index:950; }"
".settings-card { width:760px; max-width:96%; max-height:92%; padding:0; display:flex; flex-direction:column; border:1px solid #c8ced8; border-radius:12px; background:#ffffff; box-shadow:0 18px 48px rgba(20,28,42,.26); overflow:hidden; }"
".settings-head { min-height:67px; padding:13px 16px 12px 19px; display:flex; align-items:center; justify-content:space-between; border-bottom:1px solid #e0e3e8; background:#f8f9fb; }"
".settings-title { color:#242a34; font-size:17px; font-weight:700; }"
".settings-subtitle { margin-top:3px; color:#747c89; font-size:11px; }"
".settings-close { width:30px; height:30px; padding:0; border:0; border-radius:7px; background:transparent; color:#727a87; font-size:20px; }"
".settings-close:hover { background:#e8ebef; color:#2b313b; }"
".settings-columns { min-height:0; display:flex; gap:20px; padding:15px 18px 10px; overflow:auto; }"
".settings-column { flex:1; min-width:0; display:flex; flex-direction:column; gap:7px; }"
".settings-section-title { padding:0 2px 4px; color:#697180; font-size:10px; font-weight:700; letter-spacing:1px; }"
".setting-row { min-height:50px; padding:8px 9px 8px 11px; display:flex; align-items:center; gap:10px; border:1px solid #e1e4e9; border-radius:8px; background:#fbfbfc; }"
".setting-copy { flex:1; min-width:0; }"
".setting-name { color:#303641; font-size:12px; font-weight:600; }"
".setting-help { margin-top:3px; color:#858d99; font-size:10px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }"
".setting-stepper { display:flex; align-items:center; border:1px solid #d2d7df; border-radius:7px; background:#ffffff; overflow:hidden; }"
".setting-stepper button { width:27px; height:29px; padding:0; border:0; border-radius:0; background:#f3f5f7; color:#596273; font-size:16px; }"
".setting-stepper button:hover { background:#e7eaef; }"
".setting-value { min-width:54px; padding:0 5px; color:#353c47; text-align:center; font-size:11px; font-weight:600; }"
".setting-value.wide { min-width:66px; }"
".setting-value.extra-wide { min-width:82px; }"
".setting-toggle { width:46px; min-width:46px; height:29px; padding:0; border:1px solid #cfd4dc; border-radius:15px; background:#eef0f3; color:#7a828e; font-size:10px; font-weight:700; }"
".setting-toggle.on { border-color:#6073da; background:#6679df; color:#ffffff; }"
".settings-shortcut { min-height:50px; padding:0 11px; display:flex; align-items:center; justify-content:space-between; border:1px dashed #d7dbe2; border-radius:8px; color:#7b8390; font-size:10px; }"
".settings-shortcut strong { padding:5px 8px; border:1px solid #d3d8df; border-radius:6px; background:#f5f6f8; color:#4b5360; font-size:10px; }"
".settings-actions { min-height:58px; padding:11px 17px; display:flex; align-items:center; justify-content:space-between; border-top:1px solid #e0e3e8; background:#f8f9fb; }"
".settings-actions-right { display:flex; gap:7px; }"
".settings-reset,.settings-cancel,.settings-apply { height:32px; padding:0 14px; border-radius:7px; font-size:11px; }"
".settings-reset,.settings-cancel { border:1px solid #d0d5dd; background:#ffffff; color:#59616e; }"
".settings-apply { border:1px solid #5367d5; background:#6175df; color:#ffffff; }"
".app.dark { background:#181b22; color:#d8dce5; }"
".app.dark .toolbar { background:#20242c; border-color:#343a45; }"
".app.dark .tool-divider { background:#3a404b; }"
".app.dark .tool-btn { border-color:#3b424e; background:#292e37; color:#c4cad4; }"
".app.dark .tool-btn:hover { background:#343a45; border-color:#48515f; color:#ffffff; }"
".app.dark .tool-btn.primary { border-color:#6679df; background:#6175df; color:#ffffff; }"
".app.dark .app-title { color:#aeb5c1; }"
".app.dark .body,.app.dark .workspace,.app.dark .editor-host { background:#1b1f26; }"
".app.dark .sidebar { background:#20242b; border-color:#343a45; }"
".app.dark .sidebar-head { color:#8f98a7; }"
".app.dark .side-action { color:#aab2bf; }"
".app.dark .side-action:hover { background:#303640; }"
".app.dark .folder-name { border-color:#343a45; color:#d1d6df; }"
".app.dark .file-item { color:#b7beca; }"
".app.dark .file-item:hover { background:#303640; color:#ffffff; }"
".app.dark .tabs { background:#242932; border-color:#343a45; }"
".app.dark .tab-slot { border-color:#343a45; }"
".app.dark .tab-slot:hover { background:#2b3039; }"
".app.dark .tab-slot.active { background:#1b1f26; }"
".app.dark .tab { color:#9da5b3; }"
".app.dark .tab-slot.active .tab { color:#e4e7ed; }"
".app.dark .tab-close-inline:hover,.app.dark .new-tab:hover { background:#343a45; color:#ffffff; }"
".app.dark .new-tab { border-color:#343a45; background:#242932; color:#aab2bf; }"
".app.dark .statusbar { background:#20242b; border-color:#343a45; color:#aeb5c1; }"
".app.dark .prompt-card,.app.dark .settings-card { border-color:#434a56; background:#252a33; }"
".app.dark .prompt-title,.app.dark .settings-title { color:#edf0f5; }"
".app.dark .prompt-input { border-color:#48515e; background:#1d2128; color:#e4e7ed; }"
".app.dark .prompt-cancel,.app.dark .settings-reset,.app.dark .settings-cancel { border-color:#48515e; background:#303640; color:#cdd2db; }"
".app.dark .settings-head,.app.dark .settings-actions { border-color:#414854; background:#20242c; }"
".app.dark .settings-subtitle,.app.dark .settings-section-title,.app.dark .setting-help { color:#9099a8; }"
".app.dark .settings-close { color:#aeb6c3; }"
".app.dark .settings-close:hover { background:#353b46; color:#ffffff; }"
".app.dark .setting-row { border-color:#3c434f; background:#292e37; }"
".app.dark .setting-name,.app.dark .setting-value { color:#dce0e7; }"
".app.dark .setting-stepper { border-color:#48515e; background:#20242b; }"
".app.dark .setting-stepper button { background:#343a45; color:#c8ced8; }"
".app.dark .setting-stepper button:hover { background:#414854; }"
".app.dark .setting-toggle { border-color:#48515e; background:#343a45; color:#aeb6c3; }"
".app.dark .setting-toggle.on { border-color:#687be0; background:#6175df; color:#ffffff; }"
".app.dark .settings-shortcut { border-color:#454c58; color:#9da6b4; }"
".app.dark .settings-shortcut strong { border-color:#48515e; background:#303640; color:#d6dbe3; }"
".hidden { display:none; }";

/* ------------------------------------------------------------------------- */
/* UI handlers and prompt                                                     */
/* ------------------------------------------------------------------------- */

static void show_prompt(PromptMode mode, const char *title, const char *initial) {
    g_prompt_mode = mode;
    luna_set_text(id_prompt_title, title ? title : "Input");
    luna_set_value(id_prompt_input, initial ? initial : "");
    luna_remove_class(id_prompt_overlay, "hidden");
    luna_focus_element(id_prompt_input);
    g_editor_focused = 0;
    request_redraw();
}

static void hide_prompt(void) {
    g_prompt_mode = PROMPT_NONE;
    luna_add_class(id_prompt_overlay, "hidden");
    g_editor_focused = 1;
    luna_focus_element(id_editor_host);
    request_redraw();
}

static void find_next(Document *doc, const char *query, int backwards) {
    if (!doc || !query || !*query) return;
    size_t start_line = doc->caret.line;
    size_t qlen = strlen(query);
    if (!backwards) {
        for (size_t pass = 0; pass < 2; pass++) {
            size_t begin = pass == 0 ? start_line : 0;
            size_t end = pass == 0 ? doc->buffer.count : start_line + 1;
            for (size_t line = begin; line < end; line++) {
                const TextLine *ln = &doc->buffer.lines[line];
                size_t from = (pass == 0 && line == start_line) ? doc->caret.col : 0;
                if (from > ln->len) from = ln->len;
                const char *hit = strstr(ln->data + from, query);
                if (hit) {
                    TextPos a = {line, (size_t)(hit - ln->data)};
                    TextPos b = {line, a.col + qlen};
                    doc->anchor = a;
                    doc->caret = b;
                    ensure_caret_visible(doc);
                    statusf("検索: %s", query);
                    return;
                }
            }
        }
    } else {
        for (size_t pass = 0; pass < 2; pass++) {
            ssize_t begin = pass == 0 ? (ssize_t)start_line : (ssize_t)doc->buffer.count - 1;
            ssize_t end = pass == 0 ? -1 : (ssize_t)start_line;
            for (ssize_t line = begin; line > end; line--) {
                const TextLine *ln = &doc->buffer.lines[line];
                size_t limit = (pass == 0 && (size_t)line == start_line) ? doc->caret.col : ln->len;
                if (limit > ln->len) limit = ln->len;
                const char *best = NULL;
                const char *p = ln->data;
                while ((p = strstr(p, query)) != NULL) {
                    if ((size_t)(p - ln->data) >= limit) break;
                    best = p;
                    p++;
                }
                if (best) {
                    TextPos a = {(size_t)line, (size_t)(best - ln->data)};
                    TextPos b = {(size_t)line, a.col + qlen};
                    doc->anchor = a;
                    doc->caret = b;
                    ensure_caret_visible(doc);
                    statusf("検索: %s", query);
                    return;
                }
            }
        }
    }
    statusf("見つかりません: %s", query);
}

static void prompt_submit_cb(LunaElement *e) {
    (void)e;
    const char *value = luna_get_value(id_prompt_input);
    char text[PATH_MAX];
    snprintf(text, sizeof(text), "%s", value ? value : "");
    PromptMode mode = g_prompt_mode;
    hide_prompt();

    switch (mode) {
        case PROMPT_OPEN_FILE:
            open_document(text);
            break;
        case PROMPT_SAVE_AS:
            if (*text) save_document_to(active_doc(), text);
            break;
        case PROMPT_FIND:
            snprintf(g_find_query, sizeof(g_find_query), "%s", text);
            find_next(active_doc(), g_find_query, 0);
            break;
        case PROMPT_GOTO: {
            long n = strtol(text, NULL, 10);
            Document *doc = active_doc();
            if (doc && n > 0) {
                size_t line = (size_t)(n - 1);
                if (line >= doc->buffer.count) line = doc->buffer.count - 1;
                doc_set_caret(doc, (TextPos){line, 0}, 0);
                ensure_caret_visible(doc);
            }
            break;
        }
        case PROMPT_OPEN_FOLDER: {
            char abs[PATH_MAX], resolved[PATH_MAX];
            if (absolute_path(text, abs, sizeof(abs)) && realpath(abs, resolved)) {
                struct stat st;
                if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                    snprintf(g_working_dir, sizeof(g_working_dir), "%s", resolved);
                    scan_explorer();
                } else statusf("フォルダではありません");
            } else statusf("フォルダが見つかりません");
            break;
        }
        default:
            break;
    }
}

static void prompt_cancel_cb(LunaElement *e) { (void)e; hide_prompt(); }
static void new_doc_cb(LunaElement *e) { (void)e; new_document(); }
static void close_doc_cb(LunaElement *e) { (void)e; close_active_document(0); }
static void open_file_cb(LunaElement *e) {
    (void)e;
    show_prompt(PROMPT_OPEN_FILE, "Open File", g_working_dir);
}
static void save_doc_cb(LunaElement *e) {
    (void)e;
    Document *doc = active_doc();
    if (!doc) return;
    if (doc->path[0]) save_active_document();
    else show_prompt(PROMPT_SAVE_AS, "Save As", g_working_dir);
}
static void undo_doc_cb(LunaElement *e) {
    (void)e;
    Document *doc = active_doc();
    if (!doc) return;
    doc_undo(doc);
    ensure_caret_visible(doc);
    update_ui();
}
static void redo_doc_cb(LunaElement *e) {
    (void)e;
    Document *doc = active_doc();
    if (!doc) return;
    doc_redo(doc);
    ensure_caret_visible(doc);
    update_ui();
}
static void show_find_cb(LunaElement *e) { (void)e; show_prompt(PROMPT_FIND, "Find", g_find_query); }
static void open_folder_cb(LunaElement *e) { (void)e; show_prompt(PROMPT_OPEN_FOLDER, "Open Folder", g_working_dir); }
static void toggle_minimap_cb(LunaElement *e) {
    (void)e;
    g_minimap_visible = !g_minimap_visible;
    settings_save();
    statusf("Minimap: %s", g_minimap_visible ? "ON" : "OFF");
    update_ui();
    request_redraw();
}

static void win_close_cb(LunaElement *e) {
    (void)e;
    if (has_unsaved_documents()) {
        statusf("未保存のタブがあります。保存するか Ctrl+Shift+W で閉じてください");
        return;
    }
    glfwSetWindowShouldClose(g_window, GLFW_TRUE);
}
static void win_min_cb(LunaElement *e) { (void)e; glfwIconifyWindow(g_window); }
static void win_max_cb(LunaElement *e) { (void)e; platform_maximize(); }

static void toggle_sidebar_cb(LunaElement *e) {
    (void)e;
    g_sidebar_visible = !g_sidebar_visible;
    if (g_sidebar_visible) luna_remove_class(id_sidebar, "hidden");
    else luna_add_class(id_sidebar, "hidden");
    luna_mark_layout_dirty();
    settings_save();
    request_redraw();
}

static void show_settings_dialog(void) {
    if (g_prompt_mode != PROMPT_NONE) hide_prompt();
    settings_capture(&g_settings_draft);
    g_settings_visible = 1;

    /* Make the modal subtree visible before updating its contents.  A subtree
       behind display:none has no usable intrinsic flex geometry; laying it out
       once synchronously prevents the first painted frame from using the
       parser defaults (the small, collapsed card seen on the first open). */
    luna_remove_class(id_settings_overlay, "hidden");
    update_settings_dialog();
    luna_mark_layout_dirty();
    luna_update(glfwGetTime(), 0.0);

    /* Re-run one full pass in the normal frame update.  This lets percentage
       max sizes and nested auto-height flex containers settle against the
       dimensions established by the warm-up pass, without ever painting the
       intermediate geometry. */
    luna_mark_layout_dirty();
    luna_focus_element(id_settings_card);
    g_editor_focused = 0;
    request_redraw();
}

static void hide_settings_dialog(void) {
    g_settings_visible = 0;
    luna_add_class(id_settings_overlay, "hidden");
    g_editor_focused = 1;
    luna_focus_element(id_editor_host);
    request_redraw();
}

static void settings_open_cb(LunaElement *e) { (void)e; show_settings_dialog(); }
static void settings_cancel_cb(LunaElement *e) { (void)e; hide_settings_dialog(); }
static void settings_apply_cb(LunaElement *e) {
    (void)e;
    settings_assign(&g_settings_draft);
    apply_visual_settings();
    settings_save();
    hide_settings_dialog();
    statusf("設定を保存しました");
}
static void settings_reset_cb(LunaElement *e) {
    (void)e;
    settings_defaults(&g_settings_draft);
    update_settings_dialog();
}
static void settings_theme_prev_cb(LunaElement *e) { (void)e; g_settings_draft.dark_theme = !g_settings_draft.dark_theme; update_settings_dialog(); }
static void settings_theme_next_cb(LunaElement *e) { settings_theme_prev_cb(e); }
static void settings_font_family_prev_cb(LunaElement *e) { (void)e; g_settings_draft.font_face = g_settings_draft.font_face == 3 ? 0 : 3; update_settings_dialog(); }
static void settings_font_family_next_cb(LunaElement *e) { settings_font_family_prev_cb(e); }
static void settings_font_down_cb(LunaElement *e) { (void)e; g_settings_draft.font_size -= 1.0f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_font_up_cb(LunaElement *e) { (void)e; g_settings_draft.font_size += 1.0f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_line_height_down_cb(LunaElement *e) { (void)e; g_settings_draft.line_height -= 0.05f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_line_height_up_cb(LunaElement *e) { (void)e; g_settings_draft.line_height += 0.05f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_letter_spacing_down_cb(LunaElement *e) { (void)e; g_settings_draft.letter_spacing -= 0.1f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_letter_spacing_up_cb(LunaElement *e) { (void)e; g_settings_draft.letter_spacing += 0.1f; settings_normalize(&g_settings_draft); update_settings_dialog(); }
static void settings_cursor_prev_cb(LunaElement *e) {
    (void)e;
    g_settings_draft.cursor_style = g_settings_draft.cursor_style == CURSOR_LINE
                                  ? CURSOR_UNDERLINE : g_settings_draft.cursor_style - 1;
    update_settings_dialog();
}
static void settings_cursor_next_cb(LunaElement *e) {
    (void)e;
    g_settings_draft.cursor_style = g_settings_draft.cursor_style == CURSOR_UNDERLINE
                                  ? CURSOR_LINE : g_settings_draft.cursor_style + 1;
    update_settings_dialog();
}
static void settings_tab_prev_cb(LunaElement *e) {
    (void)e;
    g_settings_draft.tab_width = g_settings_draft.tab_width == 8 ? 4 : g_settings_draft.tab_width == 4 ? 2 : 8;
    update_settings_dialog();
}
static void settings_tab_next_cb(LunaElement *e) {
    (void)e;
    g_settings_draft.tab_width = g_settings_draft.tab_width == 2 ? 4 : g_settings_draft.tab_width == 4 ? 8 : 2;
    update_settings_dialog();
}
static void settings_indent_prev_cb(LunaElement *e) { (void)e; g_settings_draft.insert_spaces = !g_settings_draft.insert_spaces; update_settings_dialog(); }
static void settings_indent_next_cb(LunaElement *e) { settings_indent_prev_cb(e); }
#define DEFINE_SETTINGS_TOGGLE_CB(name, field) \
    static void name##_cb(LunaElement *e) { (void)e; g_settings_draft.field = !g_settings_draft.field; update_settings_dialog(); }
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_auto_indent, auto_indent)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_smart_dedent, smart_dedent)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_syntax, syntax_highlighting)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_line_numbers, line_numbers)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_current_line, highlight_current_line)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_minimap, minimap_visible)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_sidebar, sidebar_visible)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_statusbar, statusbar_visible)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_cursor_blink, cursor_blink)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_whitespace, show_whitespace)
DEFINE_SETTINGS_TOGGLE_CB(settings_toggle_indent_guides, indent_guides)
#undef DEFINE_SETTINGS_TOGGLE_CB

/* All tabs/files share three JS handler names.  luna-ui.h currently keeps a
 * fixed 64-entry handler registry; registering one callback per file caused
 * later explorer entries to be silently unwired after the settings expansion. */
static int element_index_suffix(const LunaElement *e, const char *prefix) {
    if (!e || !prefix) return -1;
    size_t n = strlen(prefix);
    if (strncmp(e->id, prefix, n) != 0 || !e->id[n]) return -1;
    char *end = NULL;
    long value = strtol(e->id + n, &end, 10);
    if (!end || *end != '\0' || value < 0 || value > INT_MAX) return -1;
    return (int)value;
}

static void tab_activate_cb(LunaElement *e) {
    int index = element_index_suffix(e, "tab");
    if (index >= 0) switch_document(index);
}

static void close_tab_activate_cb(LunaElement *e) {
    int index = element_index_suffix(e, "closeTab");
    if (index >= 0) close_document_at(index, 0);
}

static void file_activate_cb(LunaElement *e) {
    int index = element_index_suffix(e, "file");
    if (index >= 0) explorer_activate(index);
}

static void register_handlers(void) {
    luna_register_js_handler("prompt_submit", prompt_submit_cb);
    luna_register_js_handler("prompt_cancel", prompt_cancel_cb);
    luna_register_js_handler("new_doc", new_doc_cb);
    luna_register_js_handler("close_doc", close_doc_cb);
    luna_register_js_handler("open_file", open_file_cb);
    luna_register_js_handler("save_doc", save_doc_cb);
    luna_register_js_handler("undo_doc", undo_doc_cb);
    luna_register_js_handler("redo_doc", redo_doc_cb);
    luna_register_js_handler("show_find", show_find_cb);
    luna_register_js_handler("toggle_minimap", toggle_minimap_cb);
    luna_register_js_handler("open_folder", open_folder_cb);
    luna_register_js_handler("win_close", win_close_cb);
    luna_register_js_handler("win_min", win_min_cb);
    luna_register_js_handler("win_max", win_max_cb);
    luna_register_js_handler("toggle_sidebar", toggle_sidebar_cb);
    luna_register_js_handler("settings_open", settings_open_cb);
    luna_register_js_handler("settings_cancel", settings_cancel_cb);
    luna_register_js_handler("settings_apply", settings_apply_cb);
    luna_register_js_handler("settings_reset", settings_reset_cb);
    luna_register_js_handler("settings_theme_prev", settings_theme_prev_cb);
    luna_register_js_handler("settings_theme_next", settings_theme_next_cb);
    luna_register_js_handler("settings_font_family_prev", settings_font_family_prev_cb);
    luna_register_js_handler("settings_font_family_next", settings_font_family_next_cb);
    luna_register_js_handler("settings_font_down", settings_font_down_cb);
    luna_register_js_handler("settings_font_up", settings_font_up_cb);
    luna_register_js_handler("settings_line_height_down", settings_line_height_down_cb);
    luna_register_js_handler("settings_line_height_up", settings_line_height_up_cb);
    luna_register_js_handler("settings_letter_spacing_down", settings_letter_spacing_down_cb);
    luna_register_js_handler("settings_letter_spacing_up", settings_letter_spacing_up_cb);
    luna_register_js_handler("settings_cursor_prev", settings_cursor_prev_cb);
    luna_register_js_handler("settings_cursor_next", settings_cursor_next_cb);
    luna_register_js_handler("settings_tab_prev", settings_tab_prev_cb);
    luna_register_js_handler("settings_tab_next", settings_tab_next_cb);
    luna_register_js_handler("settings_indent_prev", settings_indent_prev_cb);
    luna_register_js_handler("settings_indent_next", settings_indent_next_cb);
    luna_register_js_handler("settings_toggle_auto_indent", settings_toggle_auto_indent_cb);
    luna_register_js_handler("settings_toggle_smart_dedent", settings_toggle_smart_dedent_cb);
    luna_register_js_handler("settings_toggle_syntax", settings_toggle_syntax_cb);
    luna_register_js_handler("settings_toggle_line_numbers", settings_toggle_line_numbers_cb);
    luna_register_js_handler("settings_toggle_current_line", settings_toggle_current_line_cb);
    luna_register_js_handler("settings_toggle_minimap", settings_toggle_minimap_cb);
    luna_register_js_handler("settings_toggle_sidebar", settings_toggle_sidebar_cb);
    luna_register_js_handler("settings_toggle_statusbar", settings_toggle_statusbar_cb);
    luna_register_js_handler("settings_toggle_cursor_blink", settings_toggle_cursor_blink_cb);
    luna_register_js_handler("settings_toggle_whitespace", settings_toggle_whitespace_cb);
    luna_register_js_handler("settings_toggle_indent_guides", settings_toggle_indent_guides_cb);
    luna_register_js_handler("tab_activate", tab_activate_cb);
    luna_register_js_handler("close_tab_activate", close_tab_activate_cb);
    luna_register_js_handler("file_activate", file_activate_cb);
}

static void cache_dom_indices(void) {
    id_app = luna_get_element_by_id("app");
    id_editor_host = luna_get_element_by_id("editorHost");
    id_titlebar = luna_get_element_by_id("titlebar");
    id_workspace = luna_get_element_by_id("workspace");
    id_tabs = luna_get_element_by_id("tabs");
    id_sidebar = luna_get_element_by_id("sidebar");
    id_settings_overlay = luna_get_element_by_id("settingsOverlay");
    id_settings_card = luna_get_element_by_id("settingsCard");
    id_setting_theme = luna_get_element_by_id("settingThemeValue");
    id_setting_font_family = luna_get_element_by_id("settingFontFamilyValue");
    id_setting_font_size = luna_get_element_by_id("settingFontValue");
    id_setting_line_height = luna_get_element_by_id("settingLineHeightValue");
    id_setting_letter_spacing = luna_get_element_by_id("settingLetterSpacingValue");
    id_setting_tab_width = luna_get_element_by_id("settingTabValue");
    id_setting_indent = luna_get_element_by_id("settingIndentValue");
    id_setting_auto_indent = luna_get_element_by_id("settingAutoIndentValue");
    id_setting_smart_dedent = luna_get_element_by_id("settingSmartDedentValue");
    id_setting_syntax = luna_get_element_by_id("settingSyntaxValue");
    id_setting_line_numbers = luna_get_element_by_id("settingLineNumbersValue");
    id_setting_current_line = luna_get_element_by_id("settingCurrentLineValue");
    id_setting_minimap = luna_get_element_by_id("settingMinimapValue");
    id_setting_sidebar = luna_get_element_by_id("settingSidebarValue");
    id_setting_statusbar = luna_get_element_by_id("settingStatusbarValue");
    id_setting_cursor_style = luna_get_element_by_id("settingCursorStyleValue");
    id_setting_cursor_blink = luna_get_element_by_id("settingCursorBlinkValue");
    id_setting_whitespace = luna_get_element_by_id("settingWhitespaceValue");
    id_setting_indent_guides = luna_get_element_by_id("settingIndentGuidesValue");
    id_prompt_overlay = luna_get_element_by_id("promptOverlay");
    id_prompt_card = luna_get_element_by_id("promptCard");
    id_prompt_title = luna_get_element_by_id("promptTitle");
    id_prompt_input = luna_get_element_by_id("promptInput");
    id_title = luna_get_element_by_id("appTitle");
    id_folder = luna_get_element_by_id("folderName");
    id_status_left = luna_get_element_by_id("statusLeft");
    id_status_center = luna_get_element_by_id("statusCenter");
    id_status_right = luna_get_element_by_id("statusRight");
    id_statusbar = luna_get_element_by_id("statusbar");
    for (int i = 0; i < MAX_DOCS; i++) {
        char id[24];
        snprintf(id, sizeof(id), "tabSlot%d", i);
        id_tab_slot[i] = luna_get_element_by_id(id);
        snprintf(id, sizeof(id), "tab%d", i);
        id_tab[i] = luna_get_element_by_id(id);
    }
    for (int i = 0; i < MAX_EXPLORER_ITEMS; i++) {
        char id[16]; snprintf(id, sizeof(id), "file%d", i);
        id_file[i] = luna_get_element_by_id(id);
    }
}

static void set_text_if_changed(int idx, const char *text) {
    if (idx < 0) return;
    LunaElement *e = luna_element_at(idx);
    if (!e || strcmp(e->text, text ? text : "") == 0) return;
    luna_set_text(idx, text ? text : "");
}

static void set_toggle_state(int idx, int enabled) {
    if (idx < 0) return;
    set_text_if_changed(idx, enabled ? "ON" : "OFF");
    if (enabled) luna_add_class(idx, "on");
    else luna_remove_class(idx, "on");
}

static void update_settings_dialog(void) {
    settings_normalize(&g_settings_draft);
    char value[64];
    set_text_if_changed(id_setting_theme, g_settings_draft.dark_theme ? "ダーク" : "ライト");
    set_text_if_changed(id_setting_font_family, g_settings_draft.font_face == 3 ? "等幅" : "サンセリフ");
    snprintf(value, sizeof(value), "%.0f px", g_settings_draft.font_size);
    set_text_if_changed(id_setting_font_size, value);
    snprintf(value, sizeof(value), "%.0f%%", g_settings_draft.line_height * 100.0f);
    set_text_if_changed(id_setting_line_height, value);
    snprintf(value, sizeof(value), "%+.1f px", g_settings_draft.letter_spacing);
    set_text_if_changed(id_setting_letter_spacing, value);
    snprintf(value, sizeof(value), "%d", g_settings_draft.tab_width);
    set_text_if_changed(id_setting_tab_width, value);
    set_text_if_changed(id_setting_indent, g_settings_draft.insert_spaces ? "スペース" : "タブ");
    set_toggle_state(id_setting_auto_indent, g_settings_draft.auto_indent);
    set_toggle_state(id_setting_smart_dedent, g_settings_draft.smart_dedent);
    set_toggle_state(id_setting_syntax, g_settings_draft.syntax_highlighting);
    set_toggle_state(id_setting_line_numbers, g_settings_draft.line_numbers);
    set_toggle_state(id_setting_current_line, g_settings_draft.highlight_current_line);
    set_toggle_state(id_setting_minimap, g_settings_draft.minimap_visible);
    set_toggle_state(id_setting_sidebar, g_settings_draft.sidebar_visible);
    set_toggle_state(id_setting_statusbar, g_settings_draft.statusbar_visible);
    static const char *cursor_names[] = {"ライン", "ブロック", "下線"};
    set_text_if_changed(id_setting_cursor_style, cursor_names[g_settings_draft.cursor_style]);
    set_toggle_state(id_setting_cursor_blink, g_settings_draft.cursor_blink);
    set_toggle_state(id_setting_whitespace, g_settings_draft.show_whitespace);
    set_toggle_state(id_setting_indent_guides, g_settings_draft.indent_guides);
    request_redraw();
}

static void apply_visual_settings(void) {
    if (id_app >= 0) {
        if (g_dark_theme) luna_add_class(id_app, "dark");
        else luna_remove_class(id_app, "dark");
    }
    if (id_sidebar >= 0) {
        if (g_sidebar_visible) luna_remove_class(id_sidebar, "hidden");
        else luna_add_class(id_sidebar, "hidden");
    }
    if (id_statusbar >= 0) {
        if (g_statusbar_visible) luna_remove_class(id_statusbar, "hidden");
        else luna_add_class(id_statusbar, "hidden");
    }
    luna_mark_layout_dirty();
    ensure_caret_visible(active_doc());
    update_ui();
    request_redraw();
}

static void update_ui(void) {
    Document *doc = active_doc();
    char title[512];
    if (doc) snprintf(title, sizeof(title), "%s%s — %s",
                      doc_is_dirty(doc) ? "● " : "", doc->display_name, APP_NAME);
    else snprintf(title, sizeof(title), "%s", APP_NAME);
    set_text_if_changed(id_title, title);
    glfwSetWindowTitle(g_window, title);

    for (int i = 0; i < MAX_DOCS; i++) {
        if (i < g_doc_count) {
            char tab[300];
            snprintf(tab, sizeof(tab), "%s%s", doc_is_dirty(&g_docs[i]) ? "● " : "", g_docs[i].display_name);
            set_text_if_changed(id_tab[i], tab);
            luna_remove_class(id_tab_slot[i], "hidden");
            if (i == g_active_doc) luna_add_class(id_tab_slot[i], "active");
            else luna_remove_class(id_tab_slot[i], "active");
        } else {
            luna_add_class(id_tab_slot[i], "hidden");
        }
    }

    set_text_if_changed(id_folder, path_basename(g_working_dir)[0] ? path_basename(g_working_dir) : "/");
    for (int i = 0; i < MAX_EXPLORER_ITEMS; i++) {
        if (i < g_entry_count) {
            set_text_if_changed(id_file[i], g_entries[i].name);
            luna_remove_class(id_file[i], "hidden");
        } else luna_add_class(id_file[i], "hidden");
    }

    char left[256], center[256], right[256];
    if (g_status_until > glfwGetTime() && g_status_message[0])
        snprintf(left, sizeof(left), "%s", g_status_message);
    else if (doc)
        snprintf(left, sizeof(left), "%s%s", doc->read_only ? "Read only · " : "", doc->path[0] ? doc->path : "Unsaved document");
    else snprintf(left, sizeof(left), "Ready");

    if (doc) snprintf(center, sizeof(center), "%zu lines · %s · UTF-8", doc->buffer.count, language_name(doc->language));
    else center[0] = '\0';
    if (doc) snprintf(right, sizeof(right), "Ln %zu, Col %zu   %s: %d   %s · %.0f%%",
                      doc->caret.line + 1,
                      visual_col_at(doc->buffer.lines[doc->caret.line].data, doc->caret.col) + 1,
                      g_insert_spaces ? "Spaces" : "Tab",
                      g_tab_width, g_editor_font_face == 3 ? "Mono" : "Sans",
                      g_editor_font_size / DEFAULT_FONT_SIZE * 100.0f);
    else snprintf(right, sizeof(right), "Ln 1, Col 1");
    set_text_if_changed(id_status_left, left);
    set_text_if_changed(id_status_center, center);
    set_text_if_changed(id_status_right, right);
}

/* ------------------------------------------------------------------------- */
/* Editor metrics, hit testing and rendering                                  */
/* ------------------------------------------------------------------------- */

typedef struct { float r, g, b, a; } Color;
typedef enum {
    TOK_NORMAL,
    TOK_COMMENT,
    TOK_STRING,
    TOK_NUMBER,
    TOK_KEYWORD,
    TOK_TYPE,
    TOK_PREPROC,
    TOK_TAG,
    TOK_PROPERTY
} TokenKind;

typedef struct {
    size_t start, end;
    TokenKind kind;
} TokenSpan;

static const Color C_TEXT       = {0.18f, 0.20f, 0.24f, 1.0f};
static const Color C_MUTED      = {0.48f, 0.51f, 0.58f, 1.0f};
static const Color C_COMMENT    = {0.44f, 0.51f, 0.45f, 1.0f};
static const Color C_STRING     = {0.72f, 0.34f, 0.29f, 1.0f};
static const Color C_NUMBER     = {0.46f, 0.33f, 0.72f, 1.0f};
static const Color C_KEYWORD    = {0.25f, 0.36f, 0.78f, 1.0f};
static const Color C_TYPE       = {0.13f, 0.52f, 0.55f, 1.0f};
static const Color C_PREPROC    = {0.68f, 0.30f, 0.55f, 1.0f};
static const Color C_TAG        = {0.12f, 0.48f, 0.67f, 1.0f};
static const Color C_PROPERTY   = {0.58f, 0.37f, 0.13f, 1.0f};
static const Color D_TEXT       = {0.84f, 0.86f, 0.90f, 1.0f};
static const Color D_MUTED      = {0.53f, 0.57f, 0.64f, 1.0f};
static const Color D_COMMENT    = {0.48f, 0.65f, 0.50f, 1.0f};
static const Color D_STRING     = {0.91f, 0.57f, 0.48f, 1.0f};
static const Color D_NUMBER     = {0.72f, 0.61f, 0.94f, 1.0f};
static const Color D_KEYWORD    = {0.48f, 0.62f, 1.00f, 1.0f};
static const Color D_TYPE       = {0.35f, 0.78f, 0.80f, 1.0f};
static const Color D_PREPROC    = {0.90f, 0.50f, 0.76f, 1.0f};
static const Color D_TAG        = {0.38f, 0.72f, 0.92f, 1.0f};
static const Color D_PROPERTY   = {0.88f, 0.70f, 0.40f, 1.0f};

static LunaElement g_editor_fx;

static float editor_line_height(void) { return floorf(g_editor_font_size * g_editor_line_height + 0.5f); }
static float editor_gutter_width(void) { return g_line_numbers_visible ? EDITOR_GUTTER_W_DEFAULT : 0.0f; }

static int editor_box_valid(const LunaElement *e, float min_w, float min_h) {
    if (!e || e->display_none) return 0;
    if (!isfinite(e->x) || !isfinite(e->y) ||
        !isfinite(e->w) || !isfinite(e->h)) return 0;
    return e->w >= min_w && e->h >= min_h;
}

/* The custom editor is rendered outside Luna's normal DOM paint.  When a tab
 * is added, Luna can expose intermediate flex coordinates for one frame.  The
 * previous version returned without painting in that frame, which is why a
 * later mouse click made the text suddenly reappear.  Keep the last complete
 * editor rectangle and use it while the new layout is settling. */
static int g_editor_rect_cached = 0;
static float g_editor_rect_x = 0.0f;
static float g_editor_rect_y = 0.0f;
static float g_editor_rect_w = 0.0f;
static float g_editor_rect_h = 0.0f;

static int editor_cached_rect(float *x, float *y, float *w, float *h) {
    if (!g_editor_rect_cached) return 0;
    float x0 = fmaxf(0.0f, g_editor_rect_x);
    float y0 = fmaxf(0.0f, g_editor_rect_y);
    float x1 = fminf(luna_window_width,  g_editor_rect_x + g_editor_rect_w);
    float y1 = fminf(luna_window_height, g_editor_rect_y + g_editor_rect_h);
    if (x1 - x0 <= 1.0f || y1 - y0 <= 1.0f) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

static int editor_rect(float *x, float *y, float *w, float *h) {
    LunaElement *host = id_editor_host >= 0 ? luna_element_at(id_editor_host) : NULL;
    LunaElement *workspace = id_workspace >= 0 ? luna_element_at(id_workspace) : NULL;
    LunaElement *tabs = id_tabs >= 0 ? luna_element_at(id_tabs) : NULL;
    LunaElement *titlebar = id_titlebar >= 0 ? luna_element_at(id_titlebar) : NULL;
    LunaElement *sidebar = id_sidebar >= 0 ? luna_element_at(id_sidebar) : NULL;
    LunaElement *statusbar = id_statusbar >= 0 ? luna_element_at(id_statusbar) : NULL;

    if (!editor_box_valid(host, 8.0f, 8.0f))
        return editor_cached_rect(x, y, w, h);

    float x0 = host->x;
    float y0 = host->y;
    float x1 = host->x + host->w;
    float y1 = host->y + host->h;

    /* Intersections are optional.  If one structural element is temporarily
     * unavailable, retain the host rectangle instead of suppressing the whole
     * editor paint. */
    if (editor_box_valid(workspace, 32.0f, 32.0f)) {
        x0 = fmaxf(x0, workspace->x);
        y0 = fmaxf(y0, workspace->y);
        x1 = fminf(x1, workspace->x + workspace->w);
        y1 = fminf(y1, workspace->y + workspace->h);
    }
    if (editor_box_valid(tabs, 32.0f, 12.0f))
        y0 = fmaxf(y0, tabs->y + tabs->h);
    if (editor_box_valid(titlebar, 32.0f, 12.0f))
        y0 = fmaxf(y0, titlebar->y + titlebar->h);
    if (g_sidebar_visible && editor_box_valid(sidebar, 20.0f, 32.0f))
        x0 = fmaxf(x0, sidebar->x + sidebar->w);
    if (g_statusbar_visible && editor_box_valid(statusbar, 32.0f, 8.0f))
        y1 = fminf(y1, statusbar->y);

    x0 = fmaxf(x0, 0.0f);
    y0 = fmaxf(y0, 0.0f);
    x1 = fminf(x1, luna_window_width);
    y1 = fminf(y1, luna_window_height);

    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        x1 - x0 <= 8.0f || y1 - y0 <= 8.0f)
        return editor_cached_rect(x, y, w, h);

    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    g_editor_rect_x = x0; g_editor_rect_y = y0;
    g_editor_rect_w = x1 - x0; g_editor_rect_h = y1 - y0;
    g_editor_rect_cached = 1;
    return 1;
}

static FontAtlas *editor_metrics_begin(void) {
    FontAtlas *atlas = get_atlas(g_editor_font_size, 0, NULL);
    text_metrics_begin(g_editor_font_size, 0, g_editor_font_face, atlas);
    return atlas;
}

static float editor_space_advance(FontAtlas *atlas) {
    float advance = glyph_advance(atlas, ' ', g_editor_font_size) + g_editor_letter_spacing;
    return advance < 1.0f ? 1.0f : advance;
}

static float editor_range_width(const char *s, size_t len, size_t initial_visual_col) {
    FontAtlas *atlas = editor_metrics_begin();
    float w = 0.0f;
    float space = editor_space_advance(atlas);
    size_t visual = initial_visual_col;
    const char *p = s;
    const char *end = s + len;
    while (p < end && *p) {
        const char *before = p;
        int cp = utf8_decode(&p);
        if (cp == '\t') {
            size_t count = g_tab_width - (visual % g_tab_width);
            w += space * (float)count;
            visual += count;
        } else {
            float advance = glyph_advance(atlas, cp, g_editor_font_size) + g_editor_letter_spacing;
            w += advance < 1.0f ? 1.0f : advance;
            visual++;
        }
        if (p <= before) break;
    }
    text_metrics_end();
    return w;
}

static float editor_prefix_width(const TextLine *line, size_t col) {
    if (!line) return 0.0f;
    if (col > line->len) col = line->len;
    return editor_range_width(line->data, col, 0);
}

static size_t editor_col_from_x(const TextLine *line, float target_x) {
    if (!line || target_x <= 0.0f) return 0;
    FontAtlas *atlas = editor_metrics_begin();
    float x = 0.0f;
    float space = editor_space_advance(atlas);
    size_t visual = 0;
    size_t pos = 0;
    while (pos < line->len) {
        size_t next = utf8_next(line->data, line->len, pos);
        const char *p = line->data + pos;
        int cp = utf8_decode(&p);
        float adv;
        if (cp == '\t') {
            size_t count = g_tab_width - (visual % g_tab_width);
            adv = space * (float)count;
            visual += count;
        } else {
            adv = glyph_advance(atlas, cp, g_editor_font_size) + g_editor_letter_spacing;
            if (adv < 1.0f) adv = 1.0f;
            visual++;
        }
        if (target_x < x + adv * 0.5f) break;
        x += adv;
        pos = next;
    }
    text_metrics_end();
    return pos;
}

static TextPos editor_pos_from_mouse(Document *doc, double mx, double my) {
    float hx, hy, hw, hh;
    if (!editor_rect(&hx, &hy, &hw, &hh) || !doc) return (TextPos){0, 0};
    float line_h = editor_line_height();
    float local_y = (float)my - hy + doc->scroll_y;
    ssize_t line = (ssize_t)floorf(local_y / line_h);
    if (line < 0) line = 0;
    if ((size_t)line >= doc->buffer.count) line = (ssize_t)doc->buffer.count - 1;
    float text_x = hx + editor_gutter_width() + EDITOR_PAD_X;
    float local_x = (float)mx - text_x + doc->scroll_x;
    size_t col = editor_col_from_x(&doc->buffer.lines[line], local_x);
    return (TextPos){(size_t)line, col};
}

static void set_editor_scissor(float x, float y, float w, float h, int fbw, int fbh) {
    float sx = luna_window_width > 0 ? (float)fbw / luna_window_width : 1.0f;
    float sy = luna_window_height > 0 ? (float)fbh / luna_window_height : 1.0f;
    int px = (int)floorf(x * sx);
    int py = (int)floorf((luna_window_height - y - h) * sy);
    int pw = (int)ceilf(w * sx);
    int ph = (int)ceilf(h * sy);
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px + pw > fbw) pw = fbw - px;
    if (py + ph > fbh) ph = fbh - py;
    if (pw < 0) pw = 0;
    if (ph < 0) ph = 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(px, py, pw, ph);
}

static int is_identifier_start(unsigned char c) {
    return isalpha(c) || c == '_' || c >= 0x80;
}
static int is_identifier_char(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

static int word_in_list(const char *s, size_t len, const char *const *words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (strlen(words[i]) == len && strncmp(s, words[i], len) == 0) return 1;
    return 0;
}

static TokenKind classify_word(Language lang, const char *s, size_t len) {
    static const char *const c_kw[] = {
        "if","else","for","while","do","switch","case","default","break","continue",
        "return","goto","sizeof","typedef","struct","union","enum","static","extern",
        "const","volatile","inline","restrict","auto","register","signed","unsigned"
    };
    static const char *const c_types[] = {
        "void","char","short","int","long","float","double","size_t","ssize_t","bool",
        "uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t"
    };
    static const char *const py_kw[] = {
        "and","as","assert","async","await","break","class","continue","def","del","elif",
        "else","except","False","finally","for","from","global","if","import","in","is",
        "lambda","None","nonlocal","not","or","pass","raise","return","True","try","while","with","yield"
    };
    static const char *const js_kw[] = {
        "break","case","catch","class","const","continue","debugger","default","delete","do",
        "else","export","extends","finally","for","function","if","import","in","instanceof",
        "let","new","return","static","super","switch","this","throw","try","typeof","var","void","while","with","yield","async","await"
    };
    if (lang == LANG_C || lang == LANG_CPP) {
        if (word_in_list(s, len, c_kw, sizeof(c_kw)/sizeof(c_kw[0]))) return TOK_KEYWORD;
        if (word_in_list(s, len, c_types, sizeof(c_types)/sizeof(c_types[0]))) return TOK_TYPE;
        if (lang == LANG_CPP) {
            static const char *const cpp_kw[] = {"namespace","template","typename","public","private","protected","virtual","override","using","new","delete","nullptr","true","false","constexpr","noexcept"};
            if (word_in_list(s, len, cpp_kw, sizeof(cpp_kw)/sizeof(cpp_kw[0]))) return TOK_KEYWORD;
        }
    } else if (lang == LANG_PYTHON) {
        if (word_in_list(s, len, py_kw, sizeof(py_kw)/sizeof(py_kw[0]))) return TOK_KEYWORD;
    } else if (lang == LANG_JS) {
        if (word_in_list(s, len, js_kw, sizeof(js_kw)/sizeof(js_kw[0]))) return TOK_KEYWORD;
        if ((len == 4 && strncmp(s,"true",4)==0) || (len == 5 && strncmp(s,"false",5)==0) ||
            (len == 4 && strncmp(s,"null",4)==0) || (len == 9 && strncmp(s,"undefined",9)==0)) return TOK_NUMBER;
    } else if (lang == LANG_SHELL) {
        static const char *const sh_kw[] = {"if","then","else","elif","fi","for","while","in","do","done","case","esac","function","select","time"};
        if (word_in_list(s, len, sh_kw, sizeof(sh_kw)/sizeof(sh_kw[0]))) return TOK_KEYWORD;
    }
    return TOK_NORMAL;
}

static void add_token(TokenSpan *out, int *count, int max, size_t a, size_t b, TokenKind kind) {
    if (b <= a || *count >= max) return;
    out[*count] = (TokenSpan){a, b, kind};
    (*count)++;
}

static int lex_line(Language lang, const TextLine *line, int *block_comment,
                    TokenSpan *out, int max) {
    const char *s = line->data;
    size_t n = line->len;
    int count = 0;
    size_t i = 0, plain = 0;

    if (lang == LANG_MARKDOWN) {
        size_t p = 0; while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p < n && s[p] == '#') { add_token(out,&count,max,0,n,TOK_KEYWORD); return count; }
    }
    if ((lang == LANG_C || lang == LANG_CPP) && n) {
        size_t p = 0; while (p < n && isspace((unsigned char)s[p])) p++;
        if (p < n && s[p] == '#') { add_token(out,&count,max,0,n,TOK_PREPROC); return count; }
    }
    if (lang == LANG_HTML) {
        while (i < n) {
            if (i + 3 < n && strncmp(s + i, "<!--", 4) == 0) {
                if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
                const char *end = strstr(s + i + 4, "-->");
                size_t e = end ? (size_t)(end - s) + 3 : n;
                add_token(out,&count,max,i,e,TOK_COMMENT); i = e; plain = i; continue;
            }
            if (s[i] == '<') {
                if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
                size_t e = i + 1; while (e < n && s[e] != '>') e++;
                if (e < n) e++;
                add_token(out,&count,max,i,e,TOK_TAG); i = e; plain = i; continue;
            }
            i++;
        }
        if (plain < n) add_token(out,&count,max,plain,n,TOK_NORMAL);
        return count;
    }

    while (i < n) {
        if (*block_comment) {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            const char *end = strstr(s + i, "*/");
            size_t e = end ? (size_t)(end - s) + 2 : n;
            add_token(out,&count,max,i,e,TOK_COMMENT);
            if (!end) return count;
            *block_comment = 0; i = e; plain = i; continue;
        }
        int slash_comments = (lang == LANG_C || lang == LANG_CPP || lang == LANG_JS || lang == LANG_JSON || lang == LANG_CSS);
        int hash_comments = (lang == LANG_PYTHON || lang == LANG_SHELL);
        if (slash_comments && i + 1 < n && s[i] == '/' && s[i+1] == '/') {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            add_token(out,&count,max,i,n,TOK_COMMENT); return count;
        }
        if ((lang == LANG_C || lang == LANG_CPP || lang == LANG_JS || lang == LANG_CSS) &&
            i + 1 < n && s[i] == '/' && s[i+1] == '*') {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            const char *end = strstr(s + i + 2, "*/");
            size_t e = end ? (size_t)(end - s) + 2 : n;
            add_token(out,&count,max,i,e,TOK_COMMENT);
            if (!end) { *block_comment = 1; return count; }
            i = e; plain = i; continue;
        }
        if (hash_comments && s[i] == '#') {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            add_token(out,&count,max,i,n,TOK_COMMENT); return count;
        }
        if (s[i] == '\'' || s[i] == '"' || (lang == LANG_JS && s[i] == '`')) {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            char quote = s[i++];
            size_t a = i - 1;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i++] == quote) break;
            }
            TokenKind k = TOK_STRING;
            if (lang == LANG_JSON) {
                size_t p = i; while (p < n && isspace((unsigned char)s[p])) p++;
                if (p < n && s[p] == ':') k = TOK_PROPERTY;
            }
            add_token(out,&count,max,a,i,k); plain = i; continue;
        }
        if (isdigit((unsigned char)s[i]) && (i == 0 || !is_identifier_char((unsigned char)s[i-1]))) {
            if (plain < i) add_token(out,&count,max,plain,i,TOK_NORMAL);
            size_t a = i++;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i]=='.' || s[i]=='_' || s[i]=='x' || s[i]=='X')) i++;
            add_token(out,&count,max,a,i,TOK_NUMBER); plain = i; continue;
        }
        if (is_identifier_start((unsigned char)s[i])) {
            size_t a = i++;
            while (i < n && is_identifier_char((unsigned char)s[i])) i++;
            TokenKind k = classify_word(lang, s + a, i - a);
            if (k != TOK_NORMAL) {
                if (plain < a) add_token(out,&count,max,plain,a,TOK_NORMAL);
                add_token(out,&count,max,a,i,k); plain = i;
            }
            continue;
        }
        i++;
    }
    if (plain < n) add_token(out,&count,max,plain,n,TOK_NORMAL);
    return count;
}

static Color token_color(TokenKind kind) {
    if (g_dark_theme) {
        switch (kind) {
            case TOK_COMMENT: return D_COMMENT;
            case TOK_STRING: return D_STRING;
            case TOK_NUMBER: return D_NUMBER;
            case TOK_KEYWORD: return D_KEYWORD;
            case TOK_TYPE: return D_TYPE;
            case TOK_PREPROC: return D_PREPROC;
            case TOK_TAG: return D_TAG;
            case TOK_PROPERTY: return D_PROPERTY;
            default: return D_TEXT;
        }
    }
    switch (kind) {
        case TOK_COMMENT: return C_COMMENT;
        case TOK_STRING: return C_STRING;
        case TOK_NUMBER: return C_NUMBER;
        case TOK_KEYWORD: return C_KEYWORD;
        case TOK_TYPE: return C_TYPE;
        case TOK_PREPROC: return C_PREPROC;
        case TOK_TAG: return C_TAG;
        case TOK_PROPERTY: return C_PROPERTY;
        default: return C_TEXT;
    }
}

static float draw_editor_text_span(const char *s, size_t len, float x, float y,
                                   float max_right, Color c, size_t visual_col) {
    if (!len) return 0.0f;
    char expanded[1024];
    size_t w = 0;
    size_t visual = visual_col;
    const char *p = s;
    const char *end = s + len;
    float total = 0.0f;
    while (p < end) {
        w = 0;
        const char *chunk_start = p;
        (void)chunk_start;
        while (p < end && w + g_tab_width + 5 < sizeof(expanded)) {
            const char *before = p;
            int cp = utf8_decode(&p);
            if (cp == '\t') {
                size_t spaces = g_tab_width - (visual % g_tab_width);
                for (size_t k = 0; k < spaces; k++) expanded[w++] = ' ';
                visual += spaces;
            } else {
                size_t bytes = (size_t)(p - before);
                memcpy(expanded + w, before, bytes);
                w += bytes;
                visual++;
            }
        }
        expanded[w] = '\0';

        /* render_text_fx() treats leading spaces as collapsible layout
         * whitespace and removes them.  The editor, however, must preserve
         * indentation exactly.  Advance past the leading spaces ourselves,
         * then render only the first visible glyph onward. */
        size_t leading = 0;
        while (leading < w && expanded[leading] == ' ') leading++;
        float leading_width = editor_range_width(expanded, leading, 0);
        float width = editor_range_width(expanded, w, 0);
        float draw_x = x + total + leading_width;
        float glyph_width = width - leading_width;
        if (leading < w && draw_x + glyph_width >= -200.0f && draw_x < max_right + 100.0f) {
            render_text_fx(expanded + leading, draw_x, y, max_right - draw_x, editor_line_height(),
                           0, 0, c.r, c.g, c.b, c.a, g_editor_font_size, 0,
                           editor_line_height(), 1, 0, 0, &g_editor_fx);
        }
        total += width;
        if (p >= end) break;
    }
    return total;
}

static size_t visual_advance(const char *s, size_t byte_len, size_t initial) {
    size_t visual = initial, pos = 0;
    while (pos < byte_len && s[pos]) {
        size_t next = utf8_next(s, byte_len, pos);
        const char *p = s + pos;
        int cp = utf8_decode(&p);
        if (cp == '\t') visual += g_tab_width - (visual % g_tab_width);
        else visual++;
        pos = next;
    }
    return visual - initial;
}

static size_t visual_col_at(const char *s, size_t byte_col) {
    return visual_advance(s, byte_col, 0);
}

static void draw_indent_guides(const TextLine *ln, float text_x, float y,
                               float right, float scroll_x) {
    if (!g_indent_guides || !ln || !ln->len) return;
    size_t visual = 0;
    size_t pos = 0;
    while (pos < ln->len) {
        int cp;
        const char *p = ln->data + pos;
        cp = utf8_decode(&p);
        if (cp == ' ') visual++;
        else if (cp == '\t') visual += g_tab_width - (visual % g_tab_width);
        else break;
        pos = (size_t)(p - ln->data);
    }
    if (visual < (size_t)g_tab_width) return;

    FontAtlas *atlas = editor_metrics_begin();
    float cell = editor_space_advance(atlas);
    text_metrics_end();
    float alpha = g_dark_theme ? 0.16f : 0.11f;
    for (size_t col = (size_t)g_tab_width; col <= visual; col += (size_t)g_tab_width) {
        float gx = text_x + cell * (float)col - scroll_x;
        if (gx >= text_x && gx < right)
            draw_rect(gx, y + 1.0f, 1.0f, editor_line_height() - 2.0f,
                      g_dark_theme ? 0.70f : 0.32f,
                      g_dark_theme ? 0.74f : 0.38f,
                      g_dark_theme ? 0.82f : 0.52f,
                      alpha, 0,0,0,0,0,0);
    }
}

static void draw_whitespace_marks(const TextLine *ln, float text_x, float y,
                                  float right, float scroll_x) {
    if (!g_show_whitespace || !ln || !ln->len) return;
    FontAtlas *atlas = editor_metrics_begin();
    float space = editor_space_advance(atlas);
    float x = text_x - scroll_x;
    size_t visual = 0;
    size_t pos = 0;
    float line_h = editor_line_height();
    float r = g_dark_theme ? 0.62f : 0.38f;
    float g = g_dark_theme ? 0.66f : 0.43f;
    float b = g_dark_theme ? 0.73f : 0.55f;
    float a = g_dark_theme ? 0.34f : 0.25f;

    while (pos < ln->len) {
        size_t next = utf8_next(ln->data, ln->len, pos);
        const char *p = ln->data + pos;
        int cp = utf8_decode(&p);
        float adv;
        if (cp == '\t') {
            size_t count = g_tab_width - (visual % g_tab_width);
            adv = space * (float)count;
            if (x + adv >= text_x && x < right) {
                float mark_x = fmaxf(x + 2.0f, text_x);
                float mark_w = fminf(adv - 5.0f, right - mark_x);
                if (mark_w > 2.0f) {
                    float mark_y = y + line_h * 0.62f;
                    draw_rect(mark_x, mark_y, mark_w, 1.0f, r,g,b,a, 0,0,0,0,0,0);
                    draw_rect(mark_x + mark_w - 1.0f, mark_y - 2.0f, 1.0f, 5.0f,
                              r,g,b,a, 0,0,0,0,0,0);
                }
            }
            visual += count;
        } else {
            adv = glyph_advance(atlas, cp, g_editor_font_size) + g_editor_letter_spacing;
            if (adv < 1.0f) adv = 1.0f;
            if (cp == ' ' && x + adv >= text_x && x < right) {
                float dot = g_editor_font_size >= 16.0f ? 2.0f : 1.5f;
                draw_rect(x + adv * 0.5f - dot * 0.5f, y + line_h * 0.58f,
                          dot, dot, r,g,b,a, dot * 0.5f,0,0,0,0,0);
            }
            visual++;
        }
        x += adv;
        if (x > right + 100.0f) break;
        pos = next;
    }
    text_metrics_end();
}

static void draw_search_matches(const Document *doc, size_t line_index,
                                float text_x, float y, float right) {
    if (!g_find_query[0]) return;
    const TextLine *ln = &doc->buffer.lines[line_index];
    size_t qlen = strlen(g_find_query);
    const char *p = ln->data;
    while ((p = strstr(p, g_find_query)) != NULL) {
        size_t a = (size_t)(p - ln->data);
        size_t b = a + qlen;
        float x1 = text_x + editor_prefix_width(ln, a) - doc->scroll_x;
        float x2 = text_x + editor_prefix_width(ln, b) - doc->scroll_x;
        if (x2 > text_x && x1 < right) {
            if (g_dark_theme)
                draw_rect(x1, y + 2, x2 - x1, editor_line_height() - 4,
                          0.96f, 0.72f, 0.22f, 0.28f, 3, 0, 0,0,0,0);
            else
                draw_rect(x1, y + 2, x2 - x1, editor_line_height() - 4,
                          1.0f, 0.78f, 0.25f, 0.24f, 3, 0, 0,0,0,0);
        }
        p++;
    }
}

static void draw_selection_for_line(const Document *doc, size_t line_index,
                                    float text_x, float y, float right) {
    if (!doc_has_selection(doc)) return;
    TextPos a, b; doc_selection(doc, &a, &b);
    if (line_index < a.line || line_index > b.line) return;
    const TextLine *ln = &doc->buffer.lines[line_index];
    size_t ca = line_index == a.line ? a.col : 0;
    size_t cb = line_index == b.line ? b.col : ln->len;
    float x1 = text_x + editor_prefix_width(ln, ca) - doc->scroll_x;
    float x2 = text_x + editor_prefix_width(ln, cb) - doc->scroll_x;
    if (line_index < b.line && cb == ln->len) x2 += 7.0f;
    if (x2 < x1 + 2.0f) x2 = x1 + 2.0f;
    if (x2 > text_x && x1 < right) {
        if (g_dark_theme)
            draw_rect(x1, y + 1, x2 - x1, editor_line_height() - 2,
                      0.38f, 0.53f, 1.0f, 0.34f, 3, 0, 0,0,0,0);
        else
            draw_rect(x1, y + 1, x2 - x1, editor_line_height() - 2,
                      0.35f, 0.47f, 0.96f, 0.25f, 3, 0, 0,0,0,0);
    }
}

static void editor_render(int fbw, int fbh) {
    Document *doc = active_doc();
    float hx, hy, hw, hh;
    if (!doc || !editor_rect(&hx, &hy, &hw, &hh)) return;
    float line_h = editor_line_height();
    float gutter_w = editor_gutter_width();
    float minimap_w = g_minimap_visible && hw > 620 ? EDITOR_MINIMAP_W : 0.0f;
    float content_right = hx + hw - minimap_w;
    float text_x = hx + gutter_w + EDITOR_PAD_X;
    Color text_color = token_color(TOK_NORMAL);
    Color muted_color = g_dark_theme ? D_MUTED : C_MUTED;

    memset(&g_editor_fx, 0, sizeof(g_editor_fx));
    g_editor_fx.font_face = g_editor_font_face;
    g_editor_fx.font_size = g_editor_font_size;
    g_editor_fx.line_height = line_h;
    g_editor_fx.letter_spacing = g_editor_letter_spacing;

    set_editor_scissor(hx, hy, hw, hh, fbw, fbh);
    if (g_dark_theme)
        draw_rect(hx, hy, hw, hh, 0.106f,0.122f,0.149f,1, 0,0,0,0,0,0);
    else
        draw_rect(hx, hy, hw, hh, 1,1,1,1, 0,0,0,0,0,0);

    if (gutter_w > 0.0f) {
        if (g_dark_theme) {
            draw_rect(hx, hy, gutter_w, hh, 0.125f,0.141f,0.169f,1, 0,0,0,0,0,0);
            draw_rect(hx + gutter_w - 1, hy, 1, hh, 0.20f,0.22f,0.26f,1, 0,0,0,0,0,0);
        } else {
            draw_rect(hx, hy, gutter_w, hh, 0.975f,0.978f,0.986f,1, 0,0,0,0,0,0);
            draw_rect(hx + gutter_w - 1, hy, 1, hh, 0.88f,0.89f,0.92f,1, 0,0,0,0,0,0);
        }
    }

    size_t first = (size_t)floorf(doc->scroll_y / line_h);
    float offset = fmodf(doc->scroll_y, line_h);
    size_t visible = (size_t)ceilf(hh / line_h) + 2;
    if (first >= doc->buffer.count) first = doc->buffer.count - 1;

    int block_comment = 0;
    if (g_syntax_highlighting) {
        size_t state_start = first > 300 ? first - 300 : 0;
        TokenSpan scratch[8];
        for (size_t i = state_start; i < first; i++)
            (void)lex_line(doc->language, &doc->buffer.lines[i], &block_comment, scratch, 8);
    }

    for (size_t row = 0; row < visible; row++) {
        size_t line_index = first + row;
        if (line_index >= doc->buffer.count) break;
        float y = hy + (float)row * line_h - offset;
        if (y + line_h < hy || y > hy + hh) continue;
        const TextLine *ln = &doc->buffer.lines[line_index];

        if (g_highlight_current_line && line_index == doc->caret.line) {
            if (g_dark_theme)
                draw_rect(hx + gutter_w, y, content_right - (hx + gutter_w), line_h,
                          0.38f,0.48f,0.78f,0.10f, 0,0,0,0,0,0);
            else
                draw_rect(hx + gutter_w, y, content_right - (hx + gutter_w), line_h,
                          0.32f,0.39f,0.62f,0.055f, 0,0,0,0,0,0);
        }
        draw_indent_guides(ln, text_x, y, content_right, doc->scroll_x);
        draw_search_matches(doc, line_index, text_x, y, content_right);
        draw_selection_for_line(doc, line_index, text_x, y, content_right);

        if (g_line_numbers_visible) {
            char number[32];
            snprintf(number, sizeof(number), "%zu", line_index + 1);
            float num_w = editor_range_width(number, strlen(number), 0);
            render_text_fx(number, hx + gutter_w - 10 - num_w, y, num_w + 2, line_h,
                           0,0, muted_color.r,muted_color.g,muted_color.b,0.78f,
                           g_editor_font_size - 1,0,line_h,1,0,0,&g_editor_fx);
        }

        TokenSpan spans[256];
        int span_count = g_syntax_highlighting
                       ? lex_line(doc->language, ln, &block_comment, spans, 256) : 0;
        float x = text_x - doc->scroll_x;
        size_t visual = 0;
        if (span_count == 0 && ln->len) {
            draw_editor_text_span(ln->data, ln->len, x, y, content_right, text_color, 0);
        } else {
            for (int i = 0; i < span_count; i++) {
                TokenSpan sp = spans[i];
                Color c = token_color(sp.kind);
                float adv = draw_editor_text_span(ln->data + sp.start, sp.end - sp.start,
                                                  x, y, content_right, c, visual);
                visual += visual_advance(ln->data + sp.start, sp.end - sp.start, visual);
                x += adv;
                if (x > content_right + 200) break;
            }
        }
        draw_whitespace_marks(ln, text_x, y, content_right, doc->scroll_x);
    }

    if (g_editor_focused && g_prompt_mode == PROMPT_NONE && !g_settings_visible) {
        double phase = fmod(glfwGetTime(), 1.0);
        if (!g_cursor_blink || phase < 0.58) {
            size_t row = doc->caret.line >= first ? doc->caret.line - first : SIZE_MAX;
            if (row != SIZE_MAX && row < visible) {
                float cy = hy + (float)row * line_h - offset;
                const TextLine *ln = &doc->buffer.lines[doc->caret.line];
                float cx = text_x + editor_prefix_width(ln, doc->caret.col) - doc->scroll_x;
                if (cx >= text_x - 2 && cx <= content_right) {
                    float cr = g_dark_theme ? 0.56f : 0.20f;
                    float cg = g_dark_theme ? 0.67f : 0.29f;
                    float cb = g_dark_theme ? 1.00f : 0.67f;
                    if (g_cursor_style == CURSOR_LINE) {
                        draw_rect(cx, cy + 2, 1.6f, line_h - 4,
                                  cr,cg,cb,g_dark_theme ? 0.98f : 0.95f,
                                  0,0,0,0,0,0);
                    } else {
                        float cw;
                        if (doc->caret.col < ln->len) {
                            size_t next = utf8_next(ln->data, ln->len, doc->caret.col);
                            cw = editor_range_width(ln->data + doc->caret.col,
                                                    next - doc->caret.col,
                                                    visual_col_at(ln->data, doc->caret.col));
                        } else {
                            FontAtlas *atlas = editor_metrics_begin();
                            cw = editor_space_advance(atlas);
                            text_metrics_end();
                        }
                        if (cw < 3.0f) cw = 3.0f;
                        if (cw > 40.0f) cw = 40.0f;
                        if (g_cursor_style == CURSOR_BLOCK)
                            draw_rect(cx, cy + 2, cw, line_h - 4,
                                      cr,cg,cb,0.30f, 2,0,0,0,0,0);
                        else
                            draw_rect(cx, cy + line_h - 3, cw, 2.0f,
                                      cr,cg,cb,0.95f, 1,0,0,0,0,0);
                    }
                }
            }
        }
    }

    /* Minimap and scroll thumb. */
    if (minimap_w > 0.0f) {
        float mx = hx + hw - minimap_w;
        if (g_dark_theme) {
            draw_rect(mx, hy, minimap_w, hh, 0.125f,0.141f,0.169f,1, 0,0,0,0,0,0);
            draw_rect(mx, hy, 1, hh, 0.20f,0.22f,0.26f,1, 0,0,0,0,0,0);
        } else {
            draw_rect(mx, hy, minimap_w, hh, 0.975f,0.978f,0.986f,1, 0,0,0,0,0,0);
            draw_rect(mx, hy, 1, hh, 0.90f,0.91f,0.94f,1, 0,0,0,0,0,0);
        }
        float scale_y = doc->buffer.count > 0 ? hh / (float)doc->buffer.count : 1.0f;
        float mini_line_h = scale_y < 2.0f ? scale_y : 2.0f;
        if (mini_line_h < 0.45f) mini_line_h = 0.45f;
        size_t max_samples = (size_t)fmaxf(96.0f, hh * 0.55f);
        size_t step = doc->buffer.count > max_samples
                    ? (doc->buffer.count + max_samples - 1) / max_samples : 1;
        for (size_t i = 0; i < doc->buffer.count; i += step) {
            const TextLine *ln = &doc->buffer.lines[i];
            float yy = hy + (float)i * scale_y;
            float ww = fminf(minimap_w - 13, 5.0f + (float)ln->len * 0.34f);
            float alpha = ln->len ? (g_dark_theme ? 0.34f : 0.24f)
                                  : (g_dark_theme ? 0.12f : 0.08f);
            if (g_dark_theme)
                draw_rect(mx + 6, yy, ww, mini_line_h, 0.50f,0.61f,0.88f,alpha, 0,0,0,0,0,0);
            else
                draw_rect(mx + 6, yy, ww, mini_line_h, 0.28f,0.36f,0.55f,alpha, 0,0,0,0,0,0);
        }
        float total_h = (float)doc->buffer.count * line_h;
        float view_y = total_h > 0 ? doc->scroll_y / total_h * hh : 0;
        float view_h = total_h > 0 ? hh / total_h * hh : hh;
        if (view_h < 24) view_h = 24;
        if (view_y + view_h > hh) view_y = hh - view_h;
        if (g_dark_theme)
            draw_rect(mx + 2, hy + view_y, minimap_w - 4, view_h,
                      0.48f,0.60f,0.94f,0.17f, 4, 1, 0.48f,0.60f,0.94f,0.22f);
        else
            draw_rect(mx + 2, hy + view_y, minimap_w - 4, view_h,
                      0.35f,0.44f,0.68f,0.10f, 4, 1, 0.35f,0.44f,0.68f,0.14f);
    }

    float total_h = (float)doc->buffer.count * line_h;
    if (total_h > hh) {
        float thumb_h = fmaxf(28.0f, hh * hh / total_h);
        float max_scroll = total_h - hh;
        float thumb_y = max_scroll > 0 ? doc->scroll_y / max_scroll * (hh - thumb_h) : 0;
        float sx = content_right - 7;
        if (g_dark_theme)
            draw_rect(sx, hy + thumb_y + 2, 4, thumb_h - 4,
                      0.66f,0.70f,0.78f,0.30f, 2,0,0,0,0,0);
        else
            draw_rect(sx, hy + thumb_y + 2, 4, thumb_h - 4,
                      0.33f,0.38f,0.49f,0.28f, 2,0,0,0,0,0);
    }

    glDisable(GL_SCISSOR_TEST);
    rc_scissor_reset();
}

static void ensure_caret_visible(Document *doc) {
    if (!doc) return;
    float hx, hy, hw, hh;
    if (!editor_rect(&hx,&hy,&hw,&hh)) return;
    float line_h = editor_line_height();
    float top = (float)doc->caret.line * line_h;
    float bottom = top + line_h;
    if (top < doc->scroll_y) doc->scroll_y = top;
    if (bottom > doc->scroll_y + hh) doc->scroll_y = bottom - hh;
    float max_y = fmaxf(0.0f, (float)doc->buffer.count * line_h - hh);
    if (doc->scroll_y > max_y) doc->scroll_y = max_y;
    if (doc->scroll_y < 0) doc->scroll_y = 0;

    float minimap_w = g_minimap_visible && hw > 620 ? EDITOR_MINIMAP_W : 0.0f;
    float visible_w = hw - editor_gutter_width() - EDITOR_PAD_X * 2 - minimap_w;
    const TextLine *ln = &doc->buffer.lines[doc->caret.line];
    float cx = editor_prefix_width(ln, doc->caret.col);
    if (cx < doc->scroll_x + 16) doc->scroll_x = fmaxf(0, cx - 16);
    if (cx > doc->scroll_x + visible_w - 24) doc->scroll_x = cx - visible_w + 24;
    if (doc->scroll_x < 0) doc->scroll_x = 0;
    request_redraw();
}

/* ------------------------------------------------------------------------- */
/* Editing commands and keyboard handling                                     */
/* ------------------------------------------------------------------------- */

static int is_word_byte(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

static void doc_move_left(Document *doc, int ctrl, int shift) {
    if (!shift && doc_has_selection(doc)) {
        TextPos a,b; doc_selection(doc,&a,&b); doc_set_caret(doc,a,0); return;
    }
    TextPos p = doc->caret;
    if (ctrl) {
        for (;;) {
            if (p.col == 0) {
                if (p.line == 0) break;
                p.line--; p.col = doc->buffer.lines[p.line].len;
            }
            TextLine *ln = &doc->buffer.lines[p.line];
            size_t prev = utf8_prev(ln->data, p.col);
            unsigned char c = (unsigned char)ln->data[prev];
            if (!isspace(c)) break;
            p.col = prev;
        }
        for (;;) {
            if (p.col == 0) break;
            TextLine *ln = &doc->buffer.lines[p.line];
            size_t prev = utf8_prev(ln->data, p.col);
            if (!is_word_byte((unsigned char)ln->data[prev])) break;
            p.col = prev;
        }
    } else if (p.col > 0) {
        p.col = utf8_prev(doc->buffer.lines[p.line].data, p.col);
    } else if (p.line > 0) {
        p.line--;
        p.col = doc->buffer.lines[p.line].len;
    }
    doc_set_caret(doc,p,shift);
}

static void doc_move_right(Document *doc, int ctrl, int shift) {
    if (!shift && doc_has_selection(doc)) {
        TextPos a,b; doc_selection(doc,&a,&b); doc_set_caret(doc,b,0); return;
    }
    TextPos p = doc->caret;
    TextLine *ln = &doc->buffer.lines[p.line];
    if (ctrl) {
        while (p.col < ln->len && is_word_byte((unsigned char)ln->data[p.col]))
            p.col = utf8_next(ln->data, ln->len, p.col);
        for (;;) {
            ln = &doc->buffer.lines[p.line];
            while (p.col < ln->len && isspace((unsigned char)ln->data[p.col]))
                p.col = utf8_next(ln->data, ln->len, p.col);
            if (p.col < ln->len || p.line + 1 >= doc->buffer.count) break;
            p.line++; p.col = 0;
        }
    } else if (p.col < ln->len) {
        p.col = utf8_next(ln->data, ln->len, p.col);
    } else if (p.line + 1 < doc->buffer.count) {
        p.line++; p.col = 0;
    }
    doc_set_caret(doc,p,shift);
}

static void doc_move_vertical(Document *doc, int delta, int shift) {
    TextPos p = doc->caret;
    if (!doc->desired_x_valid) {
        doc->desired_x = editor_prefix_width(&doc->buffer.lines[p.line], p.col);
        doc->desired_x_valid = 1;
    }
    ssize_t line = (ssize_t)p.line + delta;
    if (line < 0) line = 0;
    if ((size_t)line >= doc->buffer.count) line = (ssize_t)doc->buffer.count - 1;
    p.line = (size_t)line;
    p.col = editor_col_from_x(&doc->buffer.lines[p.line], doc->desired_x);
    doc->caret = buffer_clamp_pos(&doc->buffer,p);
    if (!shift) doc->anchor = doc->caret;
    ensure_caret_visible(doc);
}

static void doc_home(Document *doc, int ctrl, int shift) {
    TextPos p = doc->caret;
    if (ctrl) { p.line = 0; p.col = 0; }
    else {
        TextLine *ln = &doc->buffer.lines[p.line];
        size_t first = 0;
        while (first < ln->len && (ln->data[first] == ' ' || ln->data[first] == '\t')) first++;
        p.col = p.col == first ? 0 : first;
    }
    doc_set_caret(doc,p,shift);
}

static void doc_end(Document *doc, int ctrl, int shift) {
    TextPos p = doc->caret;
    if (ctrl) p.line = doc->buffer.count - 1;
    p.col = doc->buffer.lines[p.line].len;
    doc_set_caret(doc,p,shift);
}

static void doc_backspace(Document *doc) {
    if (doc_has_selection(doc)) { doc_delete_selection(doc,1); return; }
    TextPos p = doc->caret;
    if (p.col > 0) {
        TextLine *ln = &doc->buffer.lines[p.line];
        size_t leading = 0;
        while (leading < ln->len && ln->data[leading] == ' ') leading++;
        if (p.col <= leading) {
            size_t remove = p.col % g_tab_width;
            if (remove == 0) remove = g_tab_width;
            if (remove > p.col) remove = p.col;
            doc_delete_range(doc,(TextPos){p.line,p.col-remove},p,1);
        } else {
            size_t prev = utf8_prev(ln->data,p.col);
            doc_delete_range(doc,(TextPos){p.line,prev},p,1);
        }
    } else if (p.line > 0) {
        TextPos a = {p.line - 1, doc->buffer.lines[p.line - 1].len};
        doc_delete_range(doc,a,p,1);
    }
}

static void doc_delete_forward(Document *doc) {
    if (doc_has_selection(doc)) { doc_delete_selection(doc,1); return; }
    TextPos p = doc->caret;
    TextLine *ln = &doc->buffer.lines[p.line];
    if (p.col < ln->len) {
        size_t next = utf8_next(ln->data,ln->len,p.col);
        doc_delete_range(doc,p,(TextPos){p.line,next},1);
    } else if (p.line + 1 < doc->buffer.count) {
        doc_delete_range(doc,p,(TextPos){p.line+1,0},1);
    }
}

static void doc_insert_newline(Document *doc) {
    if (!doc || doc->read_only) return;
    if (doc_has_selection(doc)) doc_delete_selection(doc, 1);
    if (!g_auto_indent) {
        doc_insert_text(doc, "\n", 1);
        return;
    }

    TextLine *ln = &doc->buffer.lines[doc->caret.line];
    size_t indent_len = 0;
    while (indent_len < ln->len &&
           (ln->data[indent_len] == ' ' || ln->data[indent_len] == '\t')) indent_len++;

    size_t p = doc->caret.col;
    while (p > 0 && isspace((unsigned char)ln->data[p - 1])) p--;
    int add_level = p > 0 && strchr("{[(:", ln->data[p - 1]) != NULL;
    size_t extra = add_level ? (g_insert_spaces ? (size_t)g_tab_width : 1u) : 0u;

    char *insert = (char *)malloc(1 + indent_len + extra + 1);
    if (!insert) return;
    insert[0] = '\n';
    memcpy(insert + 1, ln->data, indent_len);
    if (extra) {
        if (g_insert_spaces) memset(insert + 1 + indent_len, ' ', extra);
        else insert[1 + indent_len] = '\t';
    }
    insert[1 + indent_len + extra] = '\0';
    doc_insert_text(doc, insert, 1);
    free(insert);
}

static void doc_insert_tab(Document *doc, int shift) {
    if (!doc || doc->read_only) return;

    char indent[MAX_TAB_WIDTH + 1];
    size_t indent_len = g_insert_spaces ? (size_t)g_tab_width : 1u;
    if (g_insert_spaces) memset(indent, ' ', indent_len);
    else indent[0] = '\t';
    indent[indent_len] = '\0';

    if (doc_has_selection(doc)) {
        TextPos a, b;
        doc_selection(doc, &a, &b);
        size_t last = b.line;
        if (b.col == 0 && last > a.line) last--;
        if (!shift) {
            for (size_t line = a.line; line <= last; line++) {
                TextPos at = {line, 0};
                buffer_insert_text(&doc->buffer, at, indent);
                undo_push(&doc->undo, EDIT_INSERT, at, indent);
            }
            doc->anchor.col += indent_len;
            doc->caret.col += indent_len;
        } else {
            for (size_t line = a.line; line <= last; line++) {
                TextLine *ln = &doc->buffer.lines[line];
                size_t n = 0;
                if (ln->len && ln->data[0] == '\t') n = 1;
                else while (n < ln->len && n < (size_t)g_tab_width && ln->data[n] == ' ') n++;
                if (n) doc_delete_range(doc, (TextPos){line, 0}, (TextPos){line, n}, 1);
            }
        }
        request_redraw();
        return;
    }

    if (shift) return;
    if (!g_insert_spaces) {
        doc_insert_text(doc, "\t", 1);
        return;
    }

    size_t visual = visual_col_at(doc->buffer.lines[doc->caret.line].data, doc->caret.col);
    size_t spaces = (size_t)g_tab_width - (visual % (size_t)g_tab_width);
    char buf[MAX_TAB_WIDTH + 1];
    memset(buf, ' ', spaces);
    buf[spaces] = '\0';
    doc_insert_text(doc, buf, 1);
}

static void doc_select_all(Document *doc) {
    if (!doc) return;
    doc->anchor = (TextPos){0,0};
    doc->caret.line = doc->buffer.count - 1;
    doc->caret.col = doc->buffer.lines[doc->caret.line].len;
    ensure_caret_visible(doc);
}

static void doc_copy(Document *doc, int cut) {
    if (!doc) return;
    char *text = NULL;
    TextPos a,b;
    if (doc_has_selection(doc)) {
        doc_selection(doc,&a,&b);
        text = buffer_get_range(&doc->buffer,a,b);
    } else {
        a = (TextPos){doc->caret.line,0};
        if (doc->caret.line + 1 < doc->buffer.count) b = (TextPos){doc->caret.line+1,0};
        else b = (TextPos){doc->caret.line,doc->buffer.lines[doc->caret.line].len};
        text = buffer_get_range(&doc->buffer,a,b);
        if (text && b.line == a.line) {
            size_t n = strlen(text);
            char *with_nl = (char *)realloc(text,n+2);
            if (with_nl) { text=with_nl; text[n]='\n'; text[n+1]='\0'; }
        }
    }
    if (text) {
        glfwSetClipboardString(g_window,text);
        if (cut && !doc->read_only) {
            if (!doc_has_selection(doc)) { doc->anchor=a; doc->caret=b; }
            doc_delete_selection(doc,1);
        }
        free(text);
        statusf(cut ? "切り取りました" : "コピーしました");
    }
}

static void doc_paste(Document *doc) {
    if (!doc || doc->read_only) return;
    const char *clip = glfwGetClipboardString(g_window);
    if (clip && *clip) doc_insert_text(doc,clip,1);
}

static void doc_duplicate_line(Document *doc) {
    if (!doc || doc->read_only) return;
    TextLine *ln = &doc->buffer.lines[doc->caret.line];
    char *text = (char *)malloc(ln->len + 2);
    if (!text) return;
    text[0]='\n'; memcpy(text+1,ln->data,ln->len); text[ln->len+1]='\0';
    TextPos at = {doc->caret.line,ln->len};
    doc->caret=doc->anchor=at;
    doc_insert_text(doc,text,1);
    free(text);
}

static void doc_toggle_comment(Document *doc) {
    if (!doc || doc->read_only) return;
    const char *mark = (doc->language == LANG_PYTHON || doc->language == LANG_SHELL) ? "# " : "// ";
    if (doc->language == LANG_TEXT || doc->language == LANG_MARKDOWN || doc->language == LANG_HTML || doc->language == LANG_CSS) mark = "# ";
    TextPos a,b;
    if (doc_has_selection(doc)) doc_selection(doc,&a,&b);
    else a=b=doc->caret;
    size_t last=b.line; if (b.col==0 && last>a.line) last--;
    int all_commented=1;
    for(size_t i=a.line;i<=last;i++){
        TextLine *ln=&doc->buffer.lines[i]; size_t p=0; while(p<ln->len&&isspace((unsigned char)ln->data[p]))p++;
        if(strncmp(ln->data+p,mark,strlen(mark))!=0){all_commented=0;break;}
    }
    for(size_t i=a.line;i<=last;i++){
        TextLine *ln=&doc->buffer.lines[i]; size_t p=0; while(p<ln->len&&(ln->data[p]==' '||ln->data[p]=='\t'))p++;
        if(all_commented) doc_delete_range(doc,(TextPos){i,p},(TextPos){i,p+strlen(mark)},1);
        else { buffer_insert_text(&doc->buffer,(TextPos){i,p},mark); undo_push(&doc->undo,EDIT_INSERT,(TextPos){i,p},mark); }
    }
    request_redraw();
}

static void insert_codepoint(Document *doc, unsigned int cp) {
    if (!doc || doc->read_only || cp < 32) return;
    char utf8[8] = {0};
    int n = utf8_encode((int)cp, utf8);
    utf8[n] = '\0';

    /* Smart closing-brace dedent on an otherwise blank indentation line. */
    if (g_smart_dedent && (cp == '}' || cp == ']') && !doc_has_selection(doc)) {
        TextLine *ln = &doc->buffer.lines[doc->caret.line];
        int only_ws = 1;
        for (size_t i = 0; i < doc->caret.col; i++) {
            if (ln->data[i] != ' ' && ln->data[i] != '\t') {
                only_ws = 0;
                break;
            }
        }
        if (only_ws && doc->caret.col > 0) {
            size_t remove = 1;
            if (ln->data[doc->caret.col - 1] == ' ') {
                remove = 0;
                while (remove < (size_t)g_tab_width && remove < doc->caret.col &&
                       ln->data[doc->caret.col - remove - 1] == ' ') remove++;
            }
            doc_delete_range(doc,
                             (TextPos){doc->caret.line, doc->caret.col - remove},
                             doc->caret, 1);
        }
    }
    doc_insert_text(doc, utf8, 1);
}

static void zoom_editor(float delta) {
    g_editor_font_size += delta;
    if (g_editor_font_size < MIN_FONT_SIZE) g_editor_font_size = MIN_FONT_SIZE;
    if (g_editor_font_size > MAX_FONT_SIZE) g_editor_font_size = MAX_FONT_SIZE;
    ensure_caret_visible(active_doc());
    settings_save();
    update_ui();
}

static void key_callback_app(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)window;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    int ctrl = (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0;
    int shift = (mods & GLFW_MOD_SHIFT) != 0;
    int alt = (mods & GLFW_MOD_ALT) != 0;

    if (g_settings_visible) {
        if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) { hide_settings_dialog(); return; }
        luna_key(key, scancode, action, mods);
        request_redraw();
        return;
    }

    if (action == GLFW_PRESS && ctrl && key == GLFW_KEY_COMMA && g_prompt_mode == PROMPT_NONE) {
        show_settings_dialog();
        return;
    }

    if (g_prompt_mode != PROMPT_NONE) {
        if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) { hide_prompt(); return; }
        if (action == GLFW_PRESS && ctrl && key == GLFW_KEY_V) {
            const char *clip=glfwGetClipboardString(g_window);
            if(clip){ const char *p=clip; while(*p){ int cp=utf8_decode(&p); luna_char((unsigned)cp); } }
            request_redraw(); return;
        }
        luna_key(key,scancode,action,mods);
        request_redraw();
        return;
    }

    if (!g_editor_focused) {
        luna_key(key,scancode,action,mods);
        request_redraw();
        return;
    }

    Document *doc=active_doc();
    if(!doc) return;

    if (action == GLFW_PRESS && ctrl) {
        if (key==GLFW_KEY_N){new_document();return;}
        if (key==GLFW_KEY_O){show_prompt(PROMPT_OPEN_FILE,"Open File",g_working_dir);return;}
        if (key==GLFW_KEY_S){
            if (shift || !doc->path[0])
                show_prompt(PROMPT_SAVE_AS,"Save As",doc->path[0]?doc->path:g_working_dir);
            else
                save_active_document();
            return;
        }
        if (key==GLFW_KEY_W){close_active_document(shift);return;}
        if (key==GLFW_KEY_Z){if(shift)doc_redo(doc);else doc_undo(doc);ensure_caret_visible(doc);update_ui();return;}
        if (key==GLFW_KEY_Y){doc_redo(doc);ensure_caret_visible(doc);update_ui();return;}
        if (key==GLFW_KEY_A){doc_select_all(doc);return;}
        if (key==GLFW_KEY_C){doc_copy(doc,0);return;}
        if (key==GLFW_KEY_X){doc_copy(doc,1);update_ui();return;}
        if (key==GLFW_KEY_V){doc_paste(doc);ensure_caret_visible(doc);update_ui();return;}
        if (key==GLFW_KEY_F){show_prompt(PROMPT_FIND,"Find",g_find_query);return;}
        if (key==GLFW_KEY_G){show_prompt(PROMPT_GOTO,"Go to Line","");return;}
        if (key==GLFW_KEY_B){toggle_sidebar_cb(NULL);return;}
        if (key==GLFW_KEY_D){doc_duplicate_line(doc);ensure_caret_visible(doc);update_ui();return;}
        if (key==GLFW_KEY_SLASH){doc_toggle_comment(doc);update_ui();return;}
        if (key==GLFW_KEY_EQUAL || key==GLFW_KEY_KP_ADD){zoom_editor(1);return;}
        if (key==GLFW_KEY_MINUS || key==GLFW_KEY_KP_SUBTRACT){zoom_editor(-1);return;}
        if (key==GLFW_KEY_0){g_editor_font_size=DEFAULT_FONT_SIZE;settings_save();ensure_caret_visible(doc);update_ui();return;}
        if (key==GLFW_KEY_HOME){doc_home(doc,1,shift);ensure_caret_visible(doc);return;}
        if (key==GLFW_KEY_END){doc_end(doc,1,shift);ensure_caret_visible(doc);return;}
    }

    if (action == GLFW_PRESS && key==GLFW_KEY_F3) { find_next(doc,g_find_query,shift); return; }
    if (action == GLFW_PRESS && key==GLFW_KEY_F11) { g_minimap_visible=!g_minimap_visible;settings_save();update_ui();request_redraw();return; }

    switch(key){
        case GLFW_KEY_LEFT: doc_move_left(doc,ctrl,shift); break;
        case GLFW_KEY_RIGHT: doc_move_right(doc,ctrl,shift); break;
        case GLFW_KEY_UP: doc_move_vertical(doc,-1,shift); break;
        case GLFW_KEY_DOWN: doc_move_vertical(doc,1,shift); break;
        case GLFW_KEY_PAGE_UP: doc_move_vertical(doc,-(int)fmaxf(1, luna_element_at(id_editor_host)->h/editor_line_height()-1),shift); break;
        case GLFW_KEY_PAGE_DOWN: doc_move_vertical(doc,(int)fmaxf(1, luna_element_at(id_editor_host)->h/editor_line_height()-1),shift); break;
        case GLFW_KEY_HOME: doc_home(doc,0,shift); break;
        case GLFW_KEY_END: doc_end(doc,0,shift); break;
        case GLFW_KEY_BACKSPACE: doc_backspace(doc); break;
        case GLFW_KEY_DELETE: doc_delete_forward(doc); break;
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: doc_insert_newline(doc); break;
        case GLFW_KEY_TAB: doc_insert_tab(doc,shift); break;
        case GLFW_KEY_ESCAPE: doc->anchor=doc->caret; g_find_query[0]='\0'; request_redraw(); break;
        default:
            if(alt && key==GLFW_KEY_UP){ /* reserved for future line move */ }
            break;
    }
    ensure_caret_visible(doc);
    update_ui();
}

static void char_callback_app(GLFWwindow *window, unsigned int codepoint) {
    (void)window;
    if (g_settings_visible) { luna_char(codepoint); request_redraw(); return; }
    if (g_prompt_mode != PROMPT_NONE) { luna_char(codepoint); request_redraw(); return; }
    if (!g_editor_focused) { luna_char(codepoint); request_redraw(); return; }
    if (codepoint >= 32) {
        insert_codepoint(active_doc(),codepoint);
        ensure_caret_visible(active_doc());
        update_ui();
    }
}

/* ------------------------------------------------------------------------- */
/* Mouse, scrolling and platform callbacks                                    */
/* ------------------------------------------------------------------------- */

static int point_in_rect(double x,double y,float rx,float ry,float rw,float rh){
    return x>=rx && x<rx+rw && y>=ry && y<ry+rh;
}

static void select_word_at(Document *doc, TextPos p) {
    TextLine *ln=&doc->buffer.lines[p.line];
    if(p.col>ln->len)p.col=ln->len;
    size_t a=p.col,b=p.col;
    if(a==ln->len && a>0)a=utf8_prev(ln->data,a);
    if(a<ln->len && is_word_byte((unsigned char)ln->data[a])){
        b=utf8_next(ln->data,ln->len,a);
        while(a>0){size_t q=utf8_prev(ln->data,a);if(!is_word_byte((unsigned char)ln->data[q]))break;a=q;}
        while(b<ln->len && is_word_byte((unsigned char)ln->data[b]))b=utf8_next(ln->data,ln->len,b);
    }else{
        b=a<ln->len?utf8_next(ln->data,ln->len,a):a;
    }
    doc->anchor=(TextPos){p.line,a}; doc->caret=(TextPos){p.line,b};
}

static void cursor_pos_callback_app(GLFWwindow *window,double x,double y){
    (void)window;
    int ui_changed = luna_mouse_move_changed(x,y);
    if(g_mouse_selecting && g_prompt_mode==PROMPT_NONE && !g_settings_visible){
        Document *doc=active_doc();
        if(doc){doc->caret=editor_pos_from_mouse(doc,x,y);ensure_caret_visible(doc);update_ui();}
        request_redraw();
    } else if (ui_changed) {
        request_redraw();
    }
}

static void mouse_button_callback_app(GLFWwindow *window,int button,int action,int mods){
    (void)window;
    double mx,my;glfwGetCursorPos(g_window,&mx,&my);
    float hx,hy,hw,hh;
    int in_editor=editor_rect(&hx,&hy,&hw,&hh)&&point_in_rect(mx,my,hx,hy,hw,hh);
    if(g_settings_visible){
        luna_mouse_button(button,action,mods,mx,my);request_redraw();return;
    }
    if(g_prompt_mode!=PROMPT_NONE){
        luna_mouse_button(button,action,mods,mx,my);request_redraw();return;
    }
    if(button==GLFW_MOUSE_BUTTON_LEFT && in_editor){
        Document *doc=active_doc();
        if(!doc)return;
        g_editor_focused=1;luna_focus_element(id_editor_host);
        if(action==GLFW_PRESS){
            float minimap_w=g_minimap_visible&&hw>620?EDITOR_MINIMAP_W:0;
            if(minimap_w>0 && mx>=hx+hw-minimap_w){
                float line_h=editor_line_height();
                float total=(float)doc->buffer.count*line_h;
                doc->scroll_y=(float)((my-hy)/hh)*total-hh*.5f;
                float max=fmaxf(0,total-hh);if(doc->scroll_y<0)doc->scroll_y=0;if(doc->scroll_y>max)doc->scroll_y=max;
                request_redraw();return;
            }
            TextPos p=editor_pos_from_mouse(doc,mx,my);
            if(mx<hx+editor_gutter_width()){
                doc->anchor=(TextPos){p.line,0};
                if(p.line+1<doc->buffer.count)doc->caret=(TextPos){p.line+1,0};
                else doc->caret=(TextPos){p.line,doc->buffer.lines[p.line].len};
                g_mouse_selecting=1;request_redraw();update_ui();return;
            }
            double now=glfwGetTime();
            int dbl=(now-g_last_click_time<0.34 && p.line==g_last_click_pos.line &&
                     (p.col==g_last_click_pos.col || p.col==utf8_next(doc->buffer.lines[p.line].data,doc->buffer.lines[p.line].len,g_last_click_pos.col)));
            g_last_click_time=now;g_last_click_pos=p;
            if(dbl){select_word_at(doc,p);g_mouse_selecting=0;}
            else{
                if(mods&GLFW_MOD_SHIFT)doc->caret=p;
                else doc->anchor=doc->caret=p;
                g_mouse_selecting=1;
            }
            doc->desired_x_valid=0;ensure_caret_visible(doc);update_ui();
        }else if(action==GLFW_RELEASE){g_mouse_selecting=0;}
        return;
    }
    if(button==GLFW_MOUSE_BUTTON_LEFT && action==GLFW_RELEASE)g_mouse_selecting=0;
    g_editor_focused=0;
    luna_mouse_button(button,action,mods,mx,my);
    request_redraw();
}

static void scroll_callback_app(GLFWwindow *window,double xoff,double yoff){
    (void)window;
    double mx,my;glfwGetCursorPos(g_window,&mx,&my);
    float hx,hy,hw,hh;
    if(g_settings_visible){luna_scroll(xoff,yoff);request_redraw();return;}
    if(g_prompt_mode==PROMPT_NONE && editor_rect(&hx,&hy,&hw,&hh)&&point_in_rect(mx,my,hx,hy,hw,hh)){
        Document *doc=active_doc();if(!doc)return;
        int ctrl=glfwGetKey(g_window,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS||glfwGetKey(g_window,GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
        int shift=glfwGetKey(g_window,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS||glfwGetKey(g_window,GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS;
        if(ctrl){zoom_editor((float)yoff);return;}
        if(shift || fabs(xoff)>fabs(yoff)){
            doc->scroll_x-=(float)(fabs(xoff)>0.01?xoff:yoff)*45.0f;
            if(doc->scroll_x<0)doc->scroll_x=0;
        }else{
            doc->scroll_y-=(float)yoff*editor_line_height()*3.0f;
            float max=fmaxf(0,(float)doc->buffer.count*editor_line_height()-hh);
            if (doc->scroll_y < 0) doc->scroll_y = 0;
            if (doc->scroll_y > max) doc->scroll_y = max;
        }
        request_redraw();return;
    }
    luna_scroll(xoff,yoff);request_redraw();
}

static void drop_callback_app(GLFWwindow *window,int count,const char **paths){
    (void)window;
    for(int i=0;i<count;i++){
        struct stat st;if(stat(paths[i],&st)!=0)continue;
        if(S_ISDIR(st.st_mode)){
            char resolved[PATH_MAX];if(realpath(paths[i],resolved)){snprintf(g_working_dir,sizeof(g_working_dir),"%s",resolved);scan_explorer();}
        }else if(S_ISREG(st.st_mode))open_document(paths[i]);
    }
}

static void window_size_callback_app(GLFWwindow *window,int width,int height){
    (void)window;luna_resize((float)width,(float)height);request_redraw();
}
static void framebuffer_size_callback_app(GLFWwindow *window,int width,int height){
    (void)window;(void)width;(void)height;luna_framebuffer_resized();request_redraw();
}
static void focus_callback_app(GLFWwindow *window,int focused){(void)window;(void)focused;request_redraw();}
static void refresh_callback_app(GLFWwindow *window){(void)window;request_redraw();}
static void window_close_callback_app(GLFWwindow *window){
    if (has_unsaved_documents()) {
        glfwSetWindowShouldClose(window, GLFW_FALSE);
        statusf("未保存のタブがあります。保存してから終了してください");
    }
}

static double platform_time(void){return glfwGetTime();}
static void *platform_get_proc(const char *name){return (void *)glfwGetProcAddress(name);}
static void platform_close(void){if(!has_unsaved_documents())glfwSetWindowShouldClose(g_window,GLFW_TRUE);else statusf("未保存のタブがあります");}
static void platform_iconify(void){glfwIconifyWindow(g_window);}
static void platform_maximize(void){
    if(glfwGetWindowAttrib(g_window,GLFW_MAXIMIZED))glfwRestoreWindow(g_window);else glfwMaximizeWindow(g_window);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                       */
/* ------------------------------------------------------------------------- */

int main(int argc,char **argv){
    settings_load();
    if(!glfwInit()){fprintf(stderr,"Luna Editor: GLFW initialization failed\n");return 1;}
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED,GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES,0);
    /* Do not expose GLFW's initial blank back buffer.  Build the DOM layout
       and paint complete frames while the window is still hidden, then show
       the already-rendered front buffer. */
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
#ifdef GLFW_SCALE_TO_MONITOR
    glfwWindowHint(GLFW_SCALE_TO_MONITOR,GLFW_TRUE);
#endif
    g_window=glfwCreateWindow(900,700,APP_NAME,NULL,NULL);
    if(!g_window){fprintf(stderr,"Luna Editor: window creation failed\n");glfwTerminate();return 1;}
    glfwSetWindowSizeLimits(g_window,760,480,GLFW_DONT_CARE,GLFW_DONT_CARE);
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);
    g_luna_glfw_window=g_window;

    LunaPlatform platform={0};
    platform.get_time=platform_time;platform.get_proc=platform_get_proc;
    platform.request_close=platform_close;platform.iconify=platform_iconify;
    platform.maximize_toggle=platform_maximize;
    luna_set_platform(&platform);
    int ww,wh;glfwGetWindowSize(g_window,&ww,&wh);
    LunaInitConfig cfg={(float)ww,(float)wh,platform_get_proc,0};
    if(!luna_init(&cfg)){fprintf(stderr,"Luna Editor: luna-ui initialization failed\n");glfwDestroyWindow(g_window);glfwTerminate();return 1;}

    register_handlers();
    luna_parse_html(APP_HTML);
    luna_parse_css(APP_CSS);
    luna_inject_body_background();
    luna_wire_onclick_handlers();
    cache_dom_indices();
    apply_visual_settings();
    luna_focus_element(id_editor_host);

    glfwSetCursorPosCallback(g_window,cursor_pos_callback_app);
    glfwSetMouseButtonCallback(g_window,mouse_button_callback_app);
    glfwSetScrollCallback(g_window,scroll_callback_app);
    glfwSetKeyCallback(g_window,key_callback_app);
    glfwSetCharCallback(g_window,char_callback_app);
    glfwSetDropCallback(g_window,drop_callback_app);
    glfwSetWindowSizeCallback(g_window,window_size_callback_app);
    glfwSetFramebufferSizeCallback(g_window,framebuffer_size_callback_app);
    glfwSetWindowFocusCallback(g_window,focus_callback_app);
    glfwSetWindowRefreshCallback(g_window,refresh_callback_app);
    glfwSetWindowCloseCallback(g_window,window_close_callback_app);

    if(!getcwd(g_working_dir,sizeof(g_working_dir)))snprintf(g_working_dir,sizeof(g_working_dir),".");
    new_document();
    for(int i=1;i<argc;i++){
        if(i==1 && g_doc_count==1 && !doc_is_dirty(&g_docs[0]) && !g_docs[0].path[0]){
            doc_free(&g_docs[0]);g_doc_count=0;g_active_doc=-1;
        }
        open_document(argv[i]);
    }
    if(g_doc_count==0)new_document();
    scan_explorer();
    update_ui();
    luna_mark_layout_dirty();
    request_redraw();

    /* Startup must be synchronous.  On some X11/Wayland drivers the window is
       visible before Luna has completed its first flex/layout pass, leaving a
       blank white front buffer until the next input event.  Prime layout,
       animations and both swap-chain buffers while the window is hidden. */
    {
        double warm_last = glfwGetTime();
        for (int warm = 0; warm < 4; warm++) {
            glfwPollEvents();
            int warm_fbw, warm_fbh, warm_ww, warm_wh;
            glfwGetFramebufferSize(g_window, &warm_fbw, &warm_fbh);
            glfwGetWindowSize(g_window, &warm_ww, &warm_wh);
            if (warm_fbw <= 0 || warm_fbh <= 0 || warm_ww <= 0 || warm_wh <= 0)
                continue;

            /* Resize first because luna_resize() marks layout dirty.  Updating
               before it would render one frame with stale startup geometry. */
            luna_resize((float)warm_ww, (float)warm_wh);
            double warm_now = glfwGetTime();
            double warm_dt = warm_now - warm_last;
            warm_last = warm_now;
            if (warm_dt <= 0.0 || warm_dt > 0.1) warm_dt = 1.0 / 60.0;
            (void)luna_update_settling(warm_now, warm_dt);

            glViewport(0, 0, warm_fbw, warm_fbh);
            luna_invalidate_gl_state();
            if (g_dark_theme) glClearColor(0.075f,0.086f,0.105f,1.0f);
            else glClearColor(0.94f,0.95f,0.97f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            luna_render(warm_fbw, warm_fbh);
            editor_render(warm_fbw, warm_fbh);
            if (g_settings_visible)
                luna_render_region(id_settings_overlay, warm_fbw, warm_fbh,
                                   0, 0, (float)warm_ww, (float)warm_wh);
            else if (g_prompt_mode != PROMPT_NONE)
                luna_render_region(id_prompt_overlay, warm_fbw, warm_fbh,
                                   0, 0, (float)warm_ww, (float)warm_wh);
            glfwSwapBuffers(g_window);
        }
    }

    /* The front buffer is now complete before the compositor maps the window. */
    glfwShowWindow(g_window);

    /* Paint once synchronously after mapping too.  Some compositors allocate a
       fresh drawable when a hidden window becomes visible. */
    {
        int first_fbw, first_fbh, first_ww, first_wh;
        glfwGetFramebufferSize(g_window, &first_fbw, &first_fbh);
        glfwGetWindowSize(g_window, &first_ww, &first_wh);
        if (first_fbw > 0 && first_fbh > 0 && first_ww > 0 && first_wh > 0) {
            luna_resize((float)first_ww, (float)first_wh);
            double first_now = glfwGetTime();
            (void)luna_update_settling(first_now, 1.0 / 60.0);
            glViewport(0, 0, first_fbw, first_fbh);
            luna_invalidate_gl_state();
            if (g_dark_theme) glClearColor(0.075f,0.086f,0.105f,1.0f);
            else glClearColor(0.94f,0.95f,0.97f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            luna_render(first_fbw, first_fbh);
            editor_render(first_fbw, first_fbh);
            if (g_settings_visible)
                luna_render_region(id_settings_overlay, first_fbw, first_fbh,
                                   0, 0, (float)first_ww, (float)first_wh);
            else if (g_prompt_mode != PROMPT_NONE)
                luna_render_region(id_prompt_overlay, first_fbw, first_fbh,
                                   0, 0, (float)first_ww, (float)first_wh);
            glfwSwapBuffers(g_window);
            g_last_render_time = first_now;
        }
    }
    glfwPollEvents();
    request_followup_redraw(3);

    double last=glfwGetTime();
    int settling=1;
    int startup_frames=3;
    while(!glfwWindowShouldClose(g_window)){
        double wait_time = g_mouse_selecting ? 0.008 : (settling ? 0.016 : 0.20);
        /* Paint startup and explicitly requested frames immediately.  Waiting
           first left the initial back buffer exposed in a partially rendered
           state on some X11/Wayland drivers. */
        if (g_needs_redraw || g_followup_redraw_frames > 0 || settling ||
            g_mouse_selecting || startup_frames > 0) glfwPollEvents();
        else glfwWaitEventsTimeout(wait_time);
        double now=glfwGetTime();double dt=now-last;last=now;if(dt>0.1)dt=0.1;
        settling=luna_update_settling(now,dt);
        if(g_status_until>0 && now>=g_status_until){g_status_until=0;g_status_message[0]='\0';update_ui();request_redraw();}
        int blink_due=g_cursor_blink && g_editor_focused && g_prompt_mode==PROMPT_NONE && !g_settings_visible && (now-g_last_render_time)>0.55;
        if(!g_needs_redraw && g_followup_redraw_frames <= 0 && !settling &&
           !blink_due && startup_frames <= 0)continue;

        int fbw,fbh;glfwGetFramebufferSize(g_window,&fbw,&fbh);
        glfwGetWindowSize(g_window,&ww,&wh);
        luna_resize((float)ww,(float)wh);
        glViewport(0,0,fbw,fbh);
        /* glClear obeys the scissor test.  Reset shared GL state before the
           clear as well as inside luna_render(), otherwise a leaked editor
           scissor can produce alternating incomplete frames. */
        luna_invalidate_gl_state();
        if(g_dark_theme) glClearColor(0.075f,0.086f,0.105f,1.0f);
        else glClearColor(0.94f,0.95f,0.97f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        luna_render(fbw,fbh);
        editor_render(fbw,fbh);
        /* The custom editor is painted after the DOM. Repaint the modal subtree
           once more so translucent prompts remain above the editor text. */
        if (g_settings_visible)
            luna_render_region(id_settings_overlay, fbw, fbh, 0, 0, (float)ww, (float)wh);
        else if (g_prompt_mode != PROMPT_NONE)
            luna_render_region(id_prompt_overlay, fbw, fbh, 0, 0, (float)ww, (float)wh);
        luna_flush_pending_screenshot();
        glfwSwapBuffers(g_window);
        if (startup_frames > 0) startup_frames--;
        if (g_followup_redraw_frames > 0) g_followup_redraw_frames--;
        g_needs_redraw = startup_frames > 0 || g_followup_redraw_frames > 0;
        g_last_render_time=now;
    }

    if(has_unsaved_documents()){
        /* The close button/shortcut normally prevents this; a window manager may still force close. */
        fprintf(stderr,"Luna Editor: closing with unsaved documents\n");
    }
    for(int i=0;i<g_doc_count;i++)doc_free(&g_docs[i]);
    luna_shutdown();
    glfwDestroyWindow(g_window);glfwTerminate();
    return 0;
}
