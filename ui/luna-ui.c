/*
 * luna-ui — GLFW host for Aurora Noir (luna-ui.html + luna-ui.css)
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 */

#define LUNA_UI_IMPLEMENTATION
#define LUNA_UI_GLFW
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <GLFW/glfw3.h>
#include "luna-ui.h"

static GLFWwindow* g_window = NULL;
static int g_desktop_mode = 0;
static int g_fullscreen = 0;

static int g_toast_idx = -1;
static int g_modal_overlay_idx = -1;
static int g_info_win_idx = -1;
static int g_select_panel_idx = -1;
static int g_progress_fill_idx = -1;
static int g_progress_track_idx = -1;

static const char* g_layout_path = NULL;
static const char* g_css_path = NULL;
static const char* default_css =
#include "luna-ui.css.h"
;
static const char* default_html =
#include "luna-ui.html.h"
;

static int el_has_class(LunaElement* e, const char* cls) {
    return e && cls && strstr(e->class_name, cls) != NULL;
}

static int elem_idx(LunaElement* e) {
    for (int i = 0; i < luna_element_count(); i++)
        if (luna_element_at(i) == e) return i;
    return -1;
}

static int find_child_knob(int toggle_idx) {
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* el = luna_element_at(i);
        if (el->parent_idx == toggle_idx && strstr(el->class_name, "toggle_knob"))
            return i;
    }
    return -1;
}

/* ── GLFW window chrome ── */

static void handle_close(LunaElement* e) {
    (void)e;
    int title = luna_get_element_by_id("title_text");
    if (title != -1) luna_set_text(title, "Closing...");
    glfwSetWindowShouldClose(g_window, GLFW_TRUE);
}

static void on_minimize(LunaElement* e) { (void)e; glfwIconifyWindow(g_window); }

static void on_maximize(LunaElement* e) {
    (void)e;
    if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED)) glfwRestoreWindow(g_window);
    else glfwMaximizeWindow(g_window);
}

static void on_tab_switch(LunaElement* e) {
    if (!e || !e->data_tab[0]) return;
    char panel_id[64];
    snprintf(panel_id, sizeof(panel_id), "tab_%s", e->data_tab);
    int ei = -1;
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* el = luna_element_at(i);
        if (el == e) ei = i;
        if (strstr(el->class_name, "nav_item")) luna_remove_class(i, "active");
        if (strstr(el->class_name, "tab_panel")) luna_remove_class(i, "active");
    }
    if (ei != -1) { luna_add_class(ei, "active"); luna_update_element_style(ei); }
    int panel = luna_get_element_by_id(panel_id);
    if (panel != -1) { luna_add_class(panel, "active"); luna_update_element_style(panel); }
}

/* ── Modal ── */

static void close_modal(void) {
    if (g_modal_overlay_idx == -1) return;
    luna_add_class(g_modal_overlay_idx, "hidden");
    luna_update_element_style(g_modal_overlay_idx);
    luna_pop_focus_trap(g_modal_overlay_idx);
}

static void dismiss_modal_trap(int trap_idx) {
    (void)trap_idx;
    close_modal();
}

static void dismiss_select_trap(int trap_idx) {
    (void)trap_idx;
    if (g_select_panel_idx == -1) return;
    luna_add_class(g_select_panel_idx, "hidden");
    luna_update_element_style(g_select_panel_idx);
    luna_pop_focus_trap(g_select_panel_idx);
    int box = luna_get_element_by_id("theme_select_box");
    if (box != -1) luna_element_at(box)->aria_expanded = 0;
}

static void btn_click(LunaElement* e) {
    (void)e;
    if (g_modal_overlay_idx == -1) return;
    luna_remove_class(g_modal_overlay_idx, "hidden");
    luna_update_element_style(g_modal_overlay_idx);
    luna_push_focus_trap(g_modal_overlay_idx, dismiss_modal_trap, 1);
}

static void modal_cancel_click(LunaElement* e) {
    (void)e;
    close_modal();
}

static void modal_confirm_click(LunaElement* e) {
    (void)e;
    close_modal();
}

/* ── Controls ── */

static void toggle_click(LunaElement* e) {
    int ei = elem_idx(e);
    if (ei == -1) return;
    int knob = find_child_knob(ei);
    if (el_has_class(e, "on")) {
        luna_remove_class(ei, "on");
        if (knob != -1) {
            luna_element_at(knob)->rel_x = 3;
            luna_element_at(knob)->pos_overridden_x = 1;
        }
    } else {
        luna_add_class(ei, "on");
        if (knob != -1) {
            luna_element_at(knob)->rel_x = 23;
            luna_element_at(knob)->pos_overridden_x = 1;
        }
    }
    luna_update_element_style(ei);
}

static void checkbox_click(LunaElement* e) {
    int ei = elem_idx(e);
    if (ei == -1) return;
    if (el_has_class(e, "checked")) luna_remove_class(ei, "checked");
    else luna_add_class(ei, "checked");
    luna_update_element_style(ei);
}

static void swatch_click(LunaElement* e) {
    int ei = elem_idx(e);
    if (ei == -1) return;
    for (int i = 0; i < luna_element_count(); i++) {
        if (strstr(luna_element_at(i)->class_name, "swatch"))
            luna_remove_class(i, "sel");
    }
    luna_add_class(ei, "sel");
    luna_update_element_style(ei);
}

static void tab_pill_click(LunaElement* e) {
    int ei = elem_idx(e);
    if (ei == -1) return;
    int p = e->parent_idx;
    if (p != -1) {
        for (int i = 0; i < luna_element_count(); i++) {
            LunaElement* el = luna_element_at(i);
            if (el->parent_idx == p && strstr(el->class_name, "tab_pill"))
                luna_remove_class(i, "active");
        }
    }
    luna_add_class(ei, "active");
    luna_update_element_style(ei);
}

static void select_toggle_click(LunaElement* e) {
    (void)e;
    if (g_select_panel_idx == -1) return;
    LunaElement* panel = luna_element_at(g_select_panel_idx);
    if (el_has_class(panel, "hidden")) {
        luna_remove_class(g_select_panel_idx, "hidden");
        luna_update_element_style(g_select_panel_idx);
        luna_push_focus_trap(g_select_panel_idx, dismiss_select_trap, 0);
        int box = luna_get_element_by_id("theme_select_box");
        if (box != -1) luna_element_at(box)->aria_expanded = 1;
    } else {
        dismiss_select_trap(g_select_panel_idx);
    }
}

static void select_option_click(LunaElement* e) {
    int box = luna_get_element_by_id("theme_select_box");
    if (box != -1) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.80s ▾", e->text);
        luna_set_text(box, buf);
    }
    dismiss_select_trap(g_select_panel_idx);
}

static void toast_dismiss(LunaElement* e) {
    (void)e;
    if (g_toast_idx != -1) luna_element_at(g_toast_idx)->display_none = 1;
}

static void info_close_click(LunaElement* e) {
    (void)e;
    if (g_info_win_idx != -1) luna_element_at(g_info_win_idx)->display_none = 1;
}

static void dock_toggle_info_click(LunaElement* e) {
    (void)e;
    if (g_info_win_idx != -1) {
        LunaElement* w = luna_element_at(g_info_win_idx);
        w->display_none = !w->display_none;
    }
}

static void dock_toggle_theme_click(LunaElement* e) {
    (void)e;
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* el = luna_element_at(i);
        if (strstr(el->class_name, "toggle")) { toggle_click(el); break; }
    }
}

static void dock_toggle_notify_click(LunaElement* e) {
    (void)e;
    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* el = luna_element_at(i);
        if (strstr(el->class_name, "checkbox")) { checkbox_click(el); break; }
    }
}

static void dock_reopen_toast_click(LunaElement* e) {
    (void)e;
    if (g_toast_idx != -1) luna_element_at(g_toast_idx)->display_none = 0;
}

static void on_screenshot(LunaElement* e) {
    (void)e;
    char path[512];
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info && strftime(path, sizeof(path), "screenshot_%Y%m%d_%H%M%S.png", tm_info) > 0)
        luna_take_screenshot(path);
    else
        luna_take_screenshot("screenshot.png");
}

static void register_demo_handlers(void) {
    luna_register_js_handler("onClose", handle_close);
    luna_register_js_handler("onMinimize", on_minimize);
    luna_register_js_handler("onMaximize", on_maximize);
    luna_register_js_handler("onTabSwitch", on_tab_switch);
    luna_register_js_handler("onApply", btn_click);
    luna_register_js_handler("onToggleClick", toggle_click);
    luna_register_js_handler("onCheckboxClick", checkbox_click);
    luna_register_js_handler("onToastDismiss", toast_dismiss);
    luna_register_js_handler("onInfoClose", info_close_click);
    luna_register_js_handler("onDockInfo", dock_toggle_info_click);
    luna_register_js_handler("onDockTheme", dock_toggle_theme_click);
    luna_register_js_handler("onDockNotify", dock_toggle_notify_click);
    luna_register_js_handler("onDockToast", dock_reopen_toast_click);
    luna_register_js_handler("onModalCancel", modal_cancel_click);
    luna_register_js_handler("onModalConfirm", modal_confirm_click);
    luna_register_js_handler("onScreenshot", on_screenshot);
    luna_register_js_handler("onSwatchClick", swatch_click);
    luna_register_js_handler("onTabPillClick", tab_pill_click);
}

static void bind_demo_indices(void) {
    g_toast_idx         = luna_get_element_by_id("toast");
    g_modal_overlay_idx = luna_get_element_by_id("modal_overlay");
    g_info_win_idx      = luna_get_element_by_id("info_win");
    g_select_panel_idx  = luna_get_element_by_id("select_panel");
    g_progress_fill_idx = luna_get_element_by_id("progress_fill");
    g_progress_track_idx = luna_get_element_by_id("progress_track");
}

static void wire_extra_handlers(void) {
    int box = luna_get_element_by_id("theme_select_box");
    if (box != -1) luna_set_on_click(box, select_toggle_click);
    for (int i = 0; i < luna_element_count(); i++) {
        if (strstr(luna_element_at(i)->class_name, "select_option"))
            luna_set_on_click(i, select_option_click);
    }
}

static void on_mouse_release(int hit, int drag_moved) {
    if (drag_moved || g_select_panel_idx == -1) return;
    LunaElement* panel = luna_element_at(g_select_panel_idx);
    if (!panel || el_has_class(panel, "hidden")) return;
    int inside = 0;
    for (int p = hit; p != -1; p = luna_element_at(p)->parent_idx) {
        if (p == g_select_panel_idx) { inside = 1; break; }
        if (p == luna_get_element_by_id("theme_select_box")) { inside = 1; break; }
        if (p == luna_get_element_by_id("theme_combo")) { inside = 1; break; }
    }
    if (!inside) dismiss_select_trap(g_select_panel_idx);
}

static void update_progress(double now) {
    if (g_progress_fill_idx == -1) return;
    float progress = fmodf((float)now * 0.09f, 1.0f);
    float track_w = (g_progress_track_idx != -1)
        ? luna_element_at(g_progress_track_idx)->w : 400.0f;
    luna_element_at(g_progress_fill_idx)->w = track_w * progress;
}

static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--desktop") || !strcmp(argv[i], "-d")) { g_desktop_mode = 1; g_fullscreen = 1; }
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%fx%f", &luna_window_width, &luna_window_height);
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc) g_layout_path = argv[++i];
        else if (!strcmp(argv[i], "--css") && i + 1 < argc) g_css_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            luna_request_screenshot(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr, "luna-ui [--desktop] [--fullscreen] [--size WxH] [--layout PATH] [--css PATH] [--screenshot PATH]\n");
            fprintf(stderr, "  Aurora Noir demo — luna-ui.html + luna-ui.css\n");
            fprintf(stderr, "  F12 — capture screenshot (PNG)\n");
            exit(0);
        }
    }
    if (g_desktop_mode) {
        const char* dl = getenv("LU_DESKTOP_LAYOUT");
        const char* dc = getenv("LU_DESKTOP_CSS");
        if (dl) g_layout_path = dl;
        if (dc) g_css_path = dc;
    }
}

static void glfw_error_cb(int err, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", err, desc);
}

static double plat_time(void) { return glfwGetTime(); }
static void* plat_proc(const char* n) { return (void*)glfwGetProcAddress(n); }
static void plat_close(void) { glfwSetWindowShouldClose(g_window, GLFW_TRUE); }
static void plat_iconify(void) { glfwIconifyWindow(g_window); }
static void plat_maximize(void) {
    if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED)) glfwRestoreWindow(g_window);
    else glfwMaximizeWindow(g_window);
}

static GLFWcursor* g_cursors[6];
static void plat_cursor(int t) {
    GLFWcursor* c = (t >= 0 && t < 6) ? g_cursors[t] : NULL;
    glfwSetCursor(g_window, c);
}

static void on_resize(GLFWwindow* w, int width, int height) {
    (void)w; luna_resize((float)width, (float)height);
}
static void on_cursor(GLFWwindow* w, double x, double y) { (void)w; luna_mouse_move(x, y); }
static void on_mouse(GLFWwindow* w, int btn, int act, int mods) {
    (void)w;
    double x, y; glfwGetCursorPos(g_window, &x, &y);
    luna_mouse_button(btn, act, mods, x, y);
}
static void on_scroll(GLFWwindow* w, double xo, double yo) { (void)w; luna_scroll(xo, yo); }
static void on_key(GLFWwindow* w, int key, int sc, int act, int mods) { (void)w; luna_key(key, sc, act, mods); }

int main(int argc, char** argv) {
    luna_window_width = 1024.0f;
    luna_window_height = 700.0f;
    parse_args(argc, argv);

#if defined(GLFW_PLATFORM_WAYLAND)
    if (getenv("WAYLAND_DISPLAY"))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
#endif
    glfwInit();
    glfwSetErrorCallback(glfw_error_cb);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    if (g_fullscreen) { glfwWindowHint(GLFW_DECORATED, GLFW_FALSE); glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE); }

    const char* title = g_desktop_mode ? "Lu Shell" : (luna_doc_title[0] ? luna_doc_title : "Aurora Noir");
    g_window = glfwCreateWindow((int)luna_window_width, (int)luna_window_height, title,
                                g_fullscreen ? glfwGetPrimaryMonitor() : NULL, NULL);
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);

    g_cursors[0] = NULL;
    g_cursors[1] = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    g_cursors[2] = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    g_cursors[3] = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);
    g_cursors[4] = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    g_cursors[5] = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);

    LunaPlatform plat = {
        .get_time = plat_time,
        .get_proc = plat_proc,
        .set_cursor = plat_cursor,
        .request_close = plat_close,
        .iconify = plat_iconify,
        .maximize_toggle = plat_maximize,
    };
    luna_set_platform(&plat);

    LunaInitConfig cfg = { luna_window_width, luna_window_height, plat_proc, 0 };
    if (!luna_init(&cfg)) { fprintf(stderr, "luna_init failed\n"); return 1; }

    if (g_layout_path) {
        luna_set_html_base_dir(g_layout_path);
        if (!luna_load_html_file(g_layout_path)) {
            luna_set_html_base_dir("ui");
            luna_parse_html(default_html);
        }
    } else {
        luna_set_html_base_dir("ui");
        luna_parse_html(default_html);
    }
    if (!luna_css_from_document) {
        if (g_css_path) {
            if (!luna_load_css_file(g_css_path))
                luna_parse_css(default_css);
        } else {
            luna_parse_css(default_css);
        }
    }
    luna_inject_body_background();
    register_demo_handlers();
    luna_wire_onclick_handlers();
    wire_extra_handlers();
    bind_demo_indices();
    luna_set_mouse_release_hook(on_mouse_release);

    glfwSetFramebufferSizeCallback(g_window, on_resize);
    glfwSetCursorPosCallback(g_window, on_cursor);
    glfwSetMouseButtonCallback(g_window, on_mouse);
    glfwSetScrollCallback(g_window, on_scroll);
    glfwSetKeyCallback(g_window, on_key);

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(g_window)) {
        double now = glfwGetTime(), dt = now - last;
        last = now;
        int ww, wh, fbw, fbh;
        glfwGetWindowSize(g_window, &ww, &wh);
        glfwGetFramebufferSize(g_window, &fbw, &fbh);
        luna_resize((float)ww, (float)wh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.06f, 0.06f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        luna_update(now, dt);
        update_progress(now);
        luna_render(fbw, fbh);
        glfwSwapBuffers(g_window);
        if (now > 1.0) /* let entrance transitions settle before capturing */
            luna_flush_pending_screenshot();
        glfwPollEvents();
    }
    luna_shutdown();
    glfwTerminate();
    return 0;
}
