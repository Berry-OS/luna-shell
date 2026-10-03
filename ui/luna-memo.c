/*
 * Luna Memo -- a small, calm text editor built only with luna-ui.h.
 *
 * Build (Linux):
 *   cc -O2 -std=c11 -Wall -Wextra luna-memo.c -o luna-memo $(pkg-config --cflags --libs glfw3) -lGL -lm
 */

#define _POSIX_C_SOURCE 200809L
#define LUNA_UI_MAX_ELEMENTS 160
#define LUNA_UI_TEXT_CAP 65536
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui/luna-ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME "Luna Memo"

static const char APP_HTML[] =
"<div id='app' class='app'>"
" <header id='titlebar' class='titlebar'><div class='appmark'>L</div><b>Luna Memo</b><span id='docTitle'>Untitled</span>"
"  <div class='title-spacer'></div><button id='winMin' class='win-button' onclick='win_min'>−</button>"
"  <button id='winMax' class='win-button' onclick='win_max'>□</button><button id='winClose' class='win-button close' onclick='win_close'>×</button>"
" </header>"
" <nav class='menubar'>"
"  <button onclick='new_note'>New</button><button onclick='open_note'>Open...</button>"
"  <button onclick='save_note'>Save</button><button onclick='save_as_note'>Save as...</button>"
"  <span class='menu-sep'></span>"
"  <button onclick='cut_text'>Cut</button><button onclick='copy_text'>Copy</button><button onclick='paste_text'>Paste</button>"
" </nav>"
" <main class='document'>"
"  <textarea id='editor' class='editor' placeholder='Type something...'></textarea>"
" </main>"
" <footer class='statusbar'><span id='saveState' class='save-state'>●  Saved</span><span id='countLabel'>0 words  ·  0 characters</span><span>UTF-8</span></footer>"
" <div id='fileOverlay' class='overlay hidden'>"
"  <section id='fileDialog' class='dialog'>"
"   <b id='dialogTitle'>Open file</b><span id='dialogHint'>Enter a text file path.</span>"
"   <input id='pathInput' class='path-input' type='text' placeholder='/path/to/note.txt'>"
"   <span id='dialogError' class='dialog-error'></span>"
"   <div class='dialog-actions'><button class='cancel' onclick='file_cancel'>Cancel</button><button class='primary' onclick='file_confirm'>Open</button></div>"
"  </section>"
" </div>"
"</div>";

static const char APP_CSS[] =
"*{box-sizing:border-box;}body{margin:0;background:#e9e8e2;color:#252823;font-family:sans-serif;overflow:hidden;}"
"button,input,textarea{font-family:sans-serif;}"
".app{width:100%;height:100%;display:flex;flex-direction:column;background:#e9e8e2;}"
".titlebar{height:44px;padding:0 16px;background:#222622;color:#f1f1eb;display:flex;align-items:center;gap:10px;}"
".titlebar,.menubar,.statusbar{flex-shrink:0;}"
".titlebar b{font-size:13px;letter-spacing:.2px;}.titlebar span{color:#90978d;font-size:11px;margin-left:4px;}"
".title-spacer{flex:1;}.win-button{width:34px;height:30px;border:0;border-radius:4px;background:transparent;color:#aeb4aa;font-size:15px;}"
".win-button:hover{background:#353a35;color:#f1f1eb;}.win-button.close:hover{background:#9b4d45;color:#fff;}"
".appmark{width:24px;height:24px;border:1px solid #667060;border-radius:6px;background:#30362f;color:#dce9c9;text-align:center;padding-top:4px;font-size:11px;font-weight:bold;}"
".menubar{height:39px;padding:0 10px;border-bottom:1px solid #cfcec7;background:#f3f2ed;display:flex;align-items:center;gap:2px;}"
".menubar button{height:28px;padding:0 10px;border:1px solid transparent;border-radius:5px;background:transparent;color:#4a4d47;font-size:11px;white-space:nowrap;}"
".menubar button:hover{background:#e4e5de;border-color:#d6d7cf;}.menubar button:active{background:#d9dbd2;}"
".menu-sep{width:1px;height:18px;background:#d3d2cb;margin:0 6px;}"
".document{flex:1;min-height:0;padding:14px;background:#dfded8;display:flex;flex-direction:column;overflow:hidden;}"
".editor{width:100%;flex:1;min-height:0;resize:none;overflow:auto;border:1px solid #c9c8bf;border-radius:3px;background:#fbfaf5;color:#30332e;font-family:monospace;font-size:15px;line-height:1.55;padding:22px 26px;outline:2px solid transparent;outline-offset:1px;caret-color:#607b43;}"
".editor:focus{border-color:#9aa88b;outline-color:#c7d5b6;}.editor::placeholder{color:#aaa9a1;}"
".statusbar{height:31px;padding:0 14px;border-top:1px solid #c8c7c0;background:#efeee9;color:#777b73;display:flex;align-items:center;justify-content:space-between;font-size:9px;letter-spacing:.3px;}"
".save-state{color:#718a55;}.save-state.dirty{color:#ad7438;}"
".overlay{position:fixed;left:0;top:0;width:100%;height:100%;z-index:100;background:rgba(20,23,20,.34);display:flex;align-items:center;justify-content:center;}"
".overlay.hidden{display:none;}"
".dialog{width:460px;padding:24px;border:1px solid #bfc0b8;border-radius:8px;background:#f8f7f2;box-shadow:0 18px 48px rgba(25,28,24,.22);display:flex;flex-direction:column;gap:10px;}"
".dialog b{font-size:18px;}.dialog>span{font-size:11px;color:#797c75;}"
".path-input{width:100%;height:40px;margin-top:6px;border:1px solid #c7c8c0;border-radius:5px;background:#fffef9;color:#292c27;padding:0 11px;font-family:monospace;font-size:12px;outline:2px solid transparent;}"
".path-input:focus{border-color:#92a47e;outline-color:#d3dfc5;}"
".dialog-error{height:16px;color:#a44f45;}.dialog-actions{display:flex;align-items:center;justify-content:flex-end;gap:8px;margin-top:4px;}"
".dialog-actions button{height:34px;padding:0 15px;border-radius:5px;font-size:11px;font-weight:bold;}"
".cancel{border:1px solid #c7c8c0;background:#efeee9;color:#555851;}.primary{border:1px solid #30362f;background:#30362f;color:#f3f4ed;}";

static int id_editor = -1, id_save_state = -1, id_count = -1, id_doc_title = -1;
static int id_file_overlay = -1, id_file_dialog = -1, id_dialog_title = -1;
static int id_dialog_hint = -1, id_dialog_error = -1, id_path_input = -1;
static int id_titlebar = -1, id_win_min = -1, id_win_max = -1, id_win_close = -1;
static char note_path[PATH_MAX];
static char *snapshot;
static int dirty;
static int file_dialog_mode; /* 1=open, 2=save as */

static int app_close(void *userdata);
static void save_as_note(LunaElement *e);

static char *memo_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *data;
    long size;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    data = (char *)malloc((size_t)size + 1);
    if (!data) { fclose(f); return NULL; }
    if (size && fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return NULL; }
    data[size] = '\0'; fclose(f); return data;
}

static int write_file(const char *path, const char *text) {
    char temp[PATH_MAX + 16];
    FILE *f;
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    f = fopen(temp, "wb");
    if (!f) return 0;
    if (fwrite(text, 1, strlen(text), f) != strlen(text) || fflush(f) || fclose(f)) { remove(temp); return 0; }
    if (rename(temp, path)) { remove(temp); return 0; }
    return 1;
}

static const char *base_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void set_snapshot(const char *text) {
    char *copy = strdup(text ? text : "");
    if (!copy) return;
    free(snapshot); snapshot = copy;
}

static void set_dirty(int value) {
    dirty = value;
    if (id_save_state < 0) return;
    luna_set_text(id_save_state, value ? "●  Unsaved" : "●  Saved");
    if (value) luna_add_class(id_save_state, "dirty"); else luna_remove_class(id_save_state, "dirty");
}

static void refresh_meta(void) {
    const char *text = luna_get_value(id_editor);
    const char *p = text;
    size_t chars = 0, words = 0;
    int in_word = 0;
    char buf[160];
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if ((c & 0xc0) != 0x80) chars++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') in_word = 0;
        else if (!in_word) { words++; in_word = 1; }
        p++;
    }
    snprintf(buf, sizeof(buf), "%zu words  ·  %zu characters", words, chars);
    luna_set_text_paint_only(id_count, buf);
    luna_set_text_paint_only(id_doc_title, note_path[0] ? base_name(note_path) : "Untitled");
}

static void save_note(LunaElement *e) {
    const char *text; (void)e;
    if (!note_path[0]) { save_as_note(NULL); return; }
    text = luna_get_value(id_editor);
    if (!write_file(note_path, text)) { fprintf(stderr, "%s: save failed: %s\n", APP_NAME, strerror(errno)); return; }
    set_snapshot(text); set_dirty(0); luna_platform_set_title(base_name(note_path));
}

static void save_as_note(LunaElement *e) {
    (void)e; file_dialog_mode = 2;
    luna_set_text(id_dialog_title, "Save as");
    luna_set_text(id_dialog_hint, "Choose where to save this text file.");
    luna_set_text(id_dialog_error, "");
    luna_set_value(id_path_input, note_path[0] ? note_path : "luna-note.txt");
    luna_remove_class(id_file_overlay, "hidden");
    luna_push_focus_trap(id_file_dialog, NULL, 0);
    luna_focus_element(id_path_input);
}

static void new_note(LunaElement *e) {
    (void)e; note_path[0] = '\0';
    luna_set_value(id_editor, "");
    set_snapshot(""); set_dirty(0); refresh_meta(); luna_platform_set_title(APP_NAME); luna_focus_element(id_editor);
}

static void open_note(LunaElement *e) {
    (void)e; file_dialog_mode = 1;
    luna_set_text(id_dialog_title, "Open file");
    luna_set_text(id_dialog_hint, "Enter the path of a UTF-8 text file.");
    luna_set_text(id_dialog_error, "");
    luna_set_value(id_path_input, note_path);
    luna_remove_class(id_file_overlay, "hidden");
    luna_push_focus_trap(id_file_dialog, NULL, 0);
    luna_focus_element(id_path_input);
}

static void file_cancel(LunaElement *e) {
    (void)e; luna_add_class(id_file_overlay, "hidden");
    luna_pop_focus_trap(id_file_dialog); luna_focus_element(id_editor);
}

static void file_confirm(LunaElement *e) {
    const char *path = luna_get_value(id_path_input); char *loaded; (void)e;
    if (!path || !*path) { luna_set_text(id_dialog_error, "Please enter a file path."); return; }
    if (strlen(path) >= sizeof(note_path)) { luna_set_text(id_dialog_error, "That path is too long."); return; }
    if (file_dialog_mode == 1) {
        loaded = memo_read_file(path);
        if (!loaded) { luna_set_text(id_dialog_error, "Could not open that file."); return; }
        memcpy(note_path, path, strlen(path) + 1);
        luna_set_value(id_editor, loaded); set_snapshot(loaded); free(loaded); set_dirty(0);
        luna_platform_set_title(base_name(note_path)); refresh_meta(); file_cancel(NULL);
    } else {
        memcpy(note_path, path, strlen(path) + 1);
        if (!write_file(note_path, luna_get_value(id_editor))) {
            luna_set_text(id_dialog_error, "Could not save to that path."); return;
        }
        set_snapshot(luna_get_value(id_editor)); set_dirty(0);
        luna_platform_set_title(base_name(note_path)); refresh_meta(); file_cancel(NULL);
    }
}

static void editor_clipboard_key(int key) {
    luna_focus_element(id_editor);
    luna_key(key, 0, LUNA_PRESS, LUNA_MOD_CONTROL);
    luna_key(key, 0, LUNA_RELEASE, LUNA_MOD_CONTROL);
}

static void cut_text(LunaElement *e) { (void)e; editor_clipboard_key(LUNA_KEY_X); }
static void copy_text(LunaElement *e) { (void)e; editor_clipboard_key(LUNA_KEY_C); }
static void paste_text(LunaElement *e) { (void)e; editor_clipboard_key(LUNA_KEY_V); }
static void win_min(LunaElement *e) { (void)e; luna_platform_iconify(); }
static void win_max(LunaElement *e) { (void)e; luna_platform_maximize_toggle(); }
static void win_close(LunaElement *e) { (void)e; luna_platform_request_close(); }

static void titlebar_press(int hit, int button, int mods) {
    (void)mods;
    if (button != LUNA_MOUSE_BUTTON_LEFT || hit < 0) return;
    if (hit == id_win_min || hit == id_win_max || hit == id_win_close) return;
    for (int p = hit; p >= 0; p = luna_element_parent(p)) {
        if (p == id_titlebar) { luna_platform_begin_move(); return; }
    }
}

static void app_init(void *userdata) {
    const char *path = (const char *)userdata; const char *shot; char *loaded = NULL; time_t now; struct tm tmv; char date[48];
    luna_register_js_handler("new_note", new_note);
    luna_register_js_handler("save_note", save_note);
    luna_register_js_handler("save_as_note", save_as_note);
    luna_register_js_handler("open_note", open_note);
    luna_register_js_handler("file_cancel", file_cancel);
    luna_register_js_handler("file_confirm", file_confirm);
    luna_register_js_handler("cut_text", cut_text);
    luna_register_js_handler("copy_text", copy_text);
    luna_register_js_handler("paste_text", paste_text);
    luna_register_js_handler("win_min", win_min);
    luna_register_js_handler("win_max", win_max);
    luna_register_js_handler("win_close", win_close);
    luna_wire_onclick_handlers();
    luna_platform_set_close_handler(app_close, NULL);
    id_editor = luna_get_element_by_id("editor"); id_save_state = luna_get_element_by_id("saveState");
    id_count = luna_get_element_by_id("countLabel"); id_doc_title = luna_get_element_by_id("docTitle");
    id_file_overlay = luna_get_element_by_id("fileOverlay"); id_file_dialog = luna_get_element_by_id("fileDialog");
    id_dialog_title = luna_get_element_by_id("dialogTitle"); id_dialog_hint = luna_get_element_by_id("dialogHint");
    id_dialog_error = luna_get_element_by_id("dialogError"); id_path_input = luna_get_element_by_id("pathInput");
    id_titlebar = luna_get_element_by_id("titlebar"); id_win_min = luna_get_element_by_id("winMin");
    id_win_max = luna_get_element_by_id("winMax"); id_win_close = luna_get_element_by_id("winClose");
    luna_set_mouse_press_hook(titlebar_press);
    (void)now; (void)tmv; (void)date;
    if (path && *path && (loaded = memo_read_file(path))) {
        snprintf(note_path, sizeof(note_path), "%s", path); luna_set_value(id_editor, loaded);
        set_snapshot(loaded); free(loaded); luna_platform_set_title(base_name(path));
    } else set_snapshot("");
    set_dirty(0); refresh_meta(); luna_focus_element(id_editor);
    shot = getenv("LUNA_SCREENSHOT");
    if (shot && *shot) luna_request_screenshot(shot);
}

static void app_render(int fbw, int fbh, void *userdata) {
    (void)fbw; (void)fbh; (void)userdata;
    luna_flush_pending_screenshot();
}

static void app_frame(double dt, void *userdata) {
    const char *text; static double elapsed; (void)userdata; elapsed += dt;
    if (elapsed < .12) return;
    elapsed = 0;
    text = luna_get_value(id_editor);
    set_dirty(strcmp(text ? text : "", snapshot ? snapshot : "") != 0);
    refresh_meta();
}

static int app_key(int key, int scancode, int action, int mods, void *userdata) {
    (void)scancode; (void)userdata;
    if (action == LUNA_PRESS && id_file_overlay >= 0 && luna_element_visible(id_file_overlay)) {
        if (key == LUNA_KEY_ESCAPE) { file_cancel(NULL); return 1; }
        if (key == LUNA_KEY_ENTER || key == LUNA_KEY_KP_ENTER) { file_confirm(NULL); return 1; }
    }
    if (action == LUNA_PRESS && (mods & LUNA_MOD_CONTROL)) {
        if (key == LUNA_KEY_S) { save_note(NULL); return 1; }
        if (key == LUNA_KEY_N) { new_note(NULL); return 1; }
        if (key == LUNA_KEY_O) { open_note(NULL); return 1; }
    }
    return 0;
}

static int app_char(unsigned int codepoint, void *userdata) {
    (void)userdata;
    /* Own the text commit path explicitly.  This keeps character/IME commits
       working even though the application also installs shortcut callbacks. */
    luna_char(codepoint);
    return 1;
}

static int app_close(void *userdata) {
    (void)userdata;
    if (dirty) fprintf(stderr, "%s: closing with unsaved changes\n", APP_NAME);
    return 1;
}

static void app_shutdown(void *userdata) { (void)userdata; free(snapshot); snapshot = NULL; }

int main(int argc, char **argv) {
    LunaAppConfig config = {0};
    config.title = APP_NAME; config.width = 1060; config.height = 720;
    config.resizable = 1; config.frameless = 1; config.vsync = 1; config.html = APP_HTML; config.css = APP_CSS;
    config.on_init = app_init; config.on_frame = app_frame; config.on_key = app_key;
    config.on_char = app_char;
    config.on_render = app_render; config.on_shutdown = app_shutdown;
    config.frame_interval = .12; config.userdata = argc > 1 ? argv[1] : NULL;
    return luna_app_run(&config);
}
