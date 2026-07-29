#!/usr/bin/env python3
"""Extract Luna UI CSS engine from opengl_gui.c into luna-ui.h (STB-style header)."""
import re
import sys

SRC = "opengl_gui.c"
DST = "luna-ui.h"

API_HEADER = r'''/*
 * luna-ui.h — Luna UI: CSS/HTML layout + OpenGL renderer (single-file library)
 *
 *   #define LUNA_UI_IMPLEMENTATION
 *   #include "luna-ui.h"
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 */
#ifndef LUNA_UI_H
#define LUNA_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LUNA_UI_MAX_ELEMENTS
#define LUNA_UI_MAX_ELEMENTS 500
#endif
#ifndef LUNA_UI_MAX_RULES
#define LUNA_UI_MAX_RULES 600
#endif

typedef struct LunaElement LunaElement;
typedef void (*LunaEventHandler)(LunaElement* e);

typedef void* (*LunaGetProcFn)(const char* name);
typedef double (*LunaGetTimeFn)(void);
typedef void (*LunaSetCursorFn)(int cursor_type);
typedef void (*LunaRequestCloseFn)(void);
typedef void (*LunaIconifyFn)(void);
typedef void (*LunaMaximizeToggleFn)(void);

typedef struct LunaPlatform {
    LunaGetTimeFn         get_time;
    LunaGetProcFn         get_proc;
    LunaSetCursorFn       set_cursor;
    LunaRequestCloseFn    request_close;
    LunaIconifyFn         iconify;
    LunaMaximizeToggleFn  maximize_toggle;
} LunaPlatform;

typedef struct LunaInitConfig {
    float width, height;
    LunaGetProcFn get_proc;
} LunaInitConfig;

void luna_set_platform(const LunaPlatform* p);
int  luna_init(const LunaInitConfig* cfg);
void luna_shutdown(void);
void luna_set_html_base_dir(const char* path);
int  luna_load_html_file(const char* path);
int  luna_load_css_file(const char* path);
void luna_parse_html(const char* html);
void luna_parse_css(const char* css);
void luna_inject_body_background(void);
void luna_wire_onclick_handlers(void);
void luna_resize(float w, float h);
void luna_mark_layout_dirty(void);
void luna_update(double now, double dt);
void luna_render(int fbw, int fbh);
int  luna_element_count(void);
LunaElement* luna_element_at(int idx);
int  luna_get_element_by_id(const char* id);
void luna_set_text(int idx, const char* text);
void luna_add_class(int idx, const char* cls);
void luna_remove_class(int idx, const char* cls);
void luna_update_element_style(int idx);
void luna_register_js_handler(const char* name, LunaEventHandler fn);
void luna_mouse_move(double x, double y);
void luna_mouse_button(int button, int action, int mods, double x, double y);
void luna_scroll(double xoff, double yoff);
void luna_key(int key, int scancode, int action, int mods);
void luna_framebuffer_resized(void);
void luna_request_screenshot(const char* path);

extern float luna_window_width;
extern float luna_window_height;
extern char  luna_doc_title[128];
extern int   luna_css_from_document;

#ifdef LUNA_UI_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef LUNA_UI_GLFW
#include <GLFW/glfw3.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define CSS_PARSER_IMPLEMENTATION
#include "cssparser.h"

#undef MAX_ELEMENTS
#undef MAX_RULES
#define MAX_ELEMENTS LUNA_UI_MAX_ELEMENTS
#define MAX_RULES    LUNA_UI_MAX_RULES

static LunaPlatform g_luna_platform;

static double luna_now(void) {
    if (g_luna_platform.get_time) return g_luna_platform.get_time();
#ifdef LUNA_UI_GLFW
    return glfwGetTime();
#else
    return 0.0;
#endif
}

void luna_set_platform(const LunaPlatform* p) {
    if (p) g_luna_platform = *p;
    else memset(&g_luna_platform, 0, sizeof(g_luna_platform));
}

float luna_window_width = 1024.0f;
float luna_window_height = 768.0f;
char  luna_doc_title[128] = "Luna UI";
int   luna_css_from_document = 0;

#define window_width  luna_window_width
#define window_height luna_window_height
#define g_doc_title   luna_doc_title
#define g_css_from_document luna_css_from_document

'''

API_FOOTER = r'''

/* ── Public wrappers ── */
int luna_element_count(void) { return elem_count; }
LunaElement* luna_element_at(int i) { return (i >= 0 && i < elem_count) ? &elements[i] : NULL; }
int luna_get_element_by_id(const char* id) { return get_element_by_id(id); }
void luna_set_text(int i, const char* t) { set_text(i, t); }
void luna_add_class(int i, const char* c) { if (i >= 0) add_class(&elements[i], c); }
void luna_remove_class(int i, const char* c) { if (i >= 0) remove_class(&elements[i], c); }
void luna_update_element_style(int i) { if (i >= 0) update_element_style(&elements[i]); }
void luna_register_js_handler(const char* n, LunaEventHandler fn) { register_js_handler(n, fn); }
void luna_set_html_base_dir(const char* p) { set_html_base_dir(p); }
int luna_load_html_file(const char* p) { char* s = read_file(p); if (!s) return 0; parse_html(s); free(s); return 1; }
int luna_load_css_file(const char* p) { char* s = read_file(p); if (!s) return 0; parse_css(s); free(s); return 1; }
void luna_parse_html(const char* h) { parse_html(h); }
void luna_parse_css(const char* c) { parse_css(c); }
void luna_wire_onclick_handlers(void) { wire_element_onclick_handlers(); }
void luna_resize(float w, float h) { luna_window_width = w; luna_window_height = h; g_layout_dirty = 1; }
void luna_mark_layout_dirty(void) { g_layout_dirty = 1; }
void luna_framebuffer_resized(void) { g_layout_dirty = 1; }
void luna_request_screenshot(const char* p) { strncpy(g_screenshot_path, p, sizeof(g_screenshot_path)-1); g_screenshot_pending = 1; }

void luna_inject_body_background(void) {
    if (elem_count >= MAX_ELEMENTS) return;
    LunaElement body_e;
    memset(&body_e, 0, sizeof(body_e));
    strncpy(body_e.type, "body", 31);
    body_e.id_idx = elem_count;
    body_e.parent_idx = -1;
    body_e.z_index = -9999;
    body_e.pct_w = 1; body_e.raw_w = 1.0f;
    body_e.pct_h = 1; body_e.raw_h = 1.0f;
    body_e.w = luna_window_width; body_e.h = luna_window_height;
    body_e.opacity = 1.0f;
    body_e.transform_scale = 1.0f;
    body_e.cur_scale = 1.0f;
    body_e.anim_speed = 14.0f;
    elements[elem_count] = body_e;
    update_element_style(&elements[elem_count]);
    elements[elem_count].pct_w = 1; elements[elem_count].raw_w = 1.0f;
    elements[elem_count].pct_h = 1; elements[elem_count].raw_h = 1.0f;
    LunaElement* ne = &elements[elem_count];
    ne->cur_r = ne->r; ne->cur_g = ne->g; ne->cur_b = ne->b; ne->cur_a = ne->a;
    elem_count++;
}

void luna_update(double now, double dt) {
    tick_smooth_scroll(dt);
    if (g_layout_dirty) { update_layout_pass(); g_layout_dirty = 0; }
    update_css_keyframe_animations(now);
    update_animations(dt);
    update_clock(now);
}

void luna_render(int fbw, int fbh) {
    build_render_order();
    glDisable(GL_SCISSOR_TEST);
    for (int ri = 0; ri < elem_count; ri++) {
        int i = render_order[ri];
        LunaElement* e = &elements[i];
        if (!is_rendered(i)) continue;
        float eff_op = element_effective_opacity(i);
        float dx, dy, dw, dh;
        get_element_draw_bounds(e, &dx, &dy, &dw, &dh);
        float scale = e->cur_scale;
        if (e->has_shadow && e->sh_a > 0.0f) {
            set_element_scissor(i, fbw, fbh);
            draw_shadow(dx, dy, dw, dh, e->sh_dx, e->sh_dy, e->sh_blur,
                        e->sh_r, e->sh_g, e->sh_b, e->sh_a, e->border_radius * scale, eff_op);
        }
        LunaElement draw_e = *e;
        set_element_scissor(i, fbw, fbh);
        draw_rect_full(dx, dy, dw, dh, draw_e.cur_r, draw_e.cur_g, draw_e.cur_b, draw_e.cur_a * eff_op,
                       draw_e.border_radius * scale, draw_e.border_width,
                       draw_e.cur_bd_r, draw_e.cur_bd_g, draw_e.cur_bd_b, draw_e.cur_bd_a * eff_op, &draw_e);
        if (e->has_bg_image && e->bg_image_path[0] && e->bg_image_tex) {
            float bw = draw_e.border_width;
            draw_image(dx+bw, dy+bw, dw-2*bw, dh-2*bw, draw_e.border_radius*scale, e->bg_image_tex, eff_op);
        }
        if (e->text[0]) {
            float pad = e->padding * scale;
            render_text(e->text, dx+pad, dy+pad, dw-pad*2, dh-pad*2, e->text_align?e->text_align:1, 1,
                        e->t_r, e->t_g, e->t_b, e->t_a*eff_op, e->font_size, e->font_bold,
                        e->line_height, e->white_space, e->text_overflow, e->overflow_wrap);
        }
        if (g_focus_via_keyboard && g_focused_element_idx == i && e->has_outline && e->outline_width > 0) {
            float ow = e->outline_width, off = e->outline_offset, p = off + ow;
            draw_rect(dx-p, dy-p, dw+p*2, dh+p*2, 0,0,0,0, e->border_radius*scale+off,
                      ow, e->ol_r, e->ol_g, e->ol_b, e->ol_a*eff_op);
        }
        glDisable(GL_SCISSOR_TEST);
    }
}

void luna_mouse_move(double x, double y) { cursor_position_callback(NULL, x, y); }
void luna_mouse_button(int b, int a, int m, double x, double y) {
    (void)x;(void)y; mouse_button_callback(NULL, b, a, m);
}
void luna_scroll(double xo, double yo) { scroll_callback(NULL, xo, yo); }
void luna_key(int k, int sc, int a, int m) { key_callback(NULL, k, sc, a, m); }

int luna_init(const LunaInitConfig* cfg) {
    if (!cfg || !cfg->get_proc) return 0;
    if (!g_luna_platform.get_proc) g_luna_platform.get_proc = cfg->get_proc;
    luna_window_width = cfg->width;
    luna_window_height = cfg->height;
    load_gl_functions();
    GLuint vs = compile_shader(bg_vs, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(bg_fs, GL_FRAGMENT_SHADER);
    bg_program = glCreateProgram();
    glAttachShader(bg_program, vs); glAttachShader(bg_program, fs);
    glLinkProgram(bg_program);
    GLuint tvs = compile_shader(text_vs, GL_VERTEX_SHADER);
    GLuint tfs = compile_shader(text_fs, GL_FRAGMENT_SHADER);
    text_program = glCreateProgram();
    glAttachShader(text_program, tvs); glAttachShader(text_program, tfs);
    glLinkProgram(text_program);
    GLuint svs = compile_shader(bg_vs, GL_VERTEX_SHADER);
    GLuint sfs = compile_shader(shadow_fs, GL_FRAGMENT_SHADER);
    shadow_program = glCreateProgram();
    glAttachShader(shadow_program, svs); glAttachShader(shadow_program, sfs);
    glLinkProgram(shadow_program);
    GLuint ivs = compile_shader(bg_vs, GL_VERTEX_SHADER);
    GLuint ifs = compile_shader(img_fs, GL_FRAGMENT_SHADER);
    img_program = glCreateProgram();
    glAttachShader(img_program, ivs); glAttachShader(img_program, ifs);
    glLinkProgram(img_program);
    cache_uniform_locations();
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    init_rect_geometry();
    init_font();
    return 1;
}
void luna_shutdown(void) {}

#endif /* LUNA_UI_IMPLEMENTATION */
#ifdef __cplusplus
}
#endif
#endif /* LUNA_UI_H */
'''

def extract_engine(src: str) -> str:
    start = src.find("typedef void (*PFNGLCREATEVERTEXARRAYSPROC)")
    demo = src.find("\nvoid onTabSwitch(Element* e)")
    if start < 0 or demo < 0:
        raise SystemExit("engine markers not found")
    part1 = src[start:demo]
    after_demo = src.find("\nstatic void update_clock(double now)")
    gl_setup = src.find("\n// ============================================================\n// GL setup")
    builtin = src.find("\n// ============================================================\n// Built-in demo CSS/HTML")
    if after_demo < 0 or gl_setup < 0:
        raise SystemExit("tail markers not found")
    part2 = src[after_demo:gl_setup]
    gl_part = src[gl_setup:builtin]
    gl_part = re.sub(r"static void parse_args\(.*?\n\}\n\n", "", gl_part, flags=re.DOTALL)
    body = part1 + part2 + gl_part
    body = body.replace("typedef struct Element {", "struct LunaElement {")
    body = body.replace("} Element;", "} LunaElement;")
    body = body.replace("typedef void (*EventHandler)(struct Element* e);",
                        "typedef LunaEventHandler EventHandler;")
    body = body.replace("Element  elements", "LunaElement  elements")
    body = body.replace("struct Element*", "LunaElement*")
    body = re.sub(r"\bElement\b", "LunaElement", body)
    body = body.replace("glfwGetTime()", "luna_now()")
    body = body.replace("GLFWwindow* window", "void* window")
    body = body.replace("GLFWwindow* g_window", "void* g_window_ptr")
    body = body.replace("g_window", "g_window_ptr")
    body = body.replace("glfwGetProcAddress", "g_luna_platform.get_proc")
    body = re.sub(
        r"static void set_window_cursor\(void\* window, int cursor_type\) \{.*?\n\}",
        "static void set_window_cursor(void* window, int cursor_type) {\n"
        "    (void)window;\n"
        "    if (cursor_type == g_current_cursor) return;\n"
        "    g_current_cursor = cursor_type;\n"
        "    if (g_luna_platform.set_cursor) g_luna_platform.set_cursor(cursor_type);\n"
        "#ifdef LUNA_UI_GLFW\n"
        "    else if (g_luna_glfw_window) {\n"
        "        GLFWcursor* cur = NULL;\n"
        "        extern GLFWcursor *g_hand_cursor, *g_cursor_ibeam, *g_cursor_crosshair;\n"
        "        extern GLFWcursor *g_cursor_hresize, *g_cursor_vresize;\n"
        "        switch (cursor_type) {\n"
        "            case 1: cur = g_hand_cursor; break; case 2: cur = g_cursor_ibeam; break;\n"
        "            case 3: cur = g_cursor_crosshair; break; case 4: cur = g_cursor_hresize; break;\n"
        "            case 5: cur = g_cursor_vresize; break;\n"
        "        }\n"
        "        glfwSetCursor((GLFWwindow*)g_luna_glfw_window, cur);\n"
        "    }\n"
        "#endif\n}",
        body, flags=re.DOTALL)
    body = body.replace(
        "glfwSetWindowShouldClose(g_window_ptr, GLFW_TRUE);",
        "if (g_luna_platform.request_close) g_luna_platform.request_close();")
    body = body.replace("glfwIconifyWindow(g_window_ptr);",
                        "if (g_luna_platform.iconify) g_luna_platform.iconify();")
    body = re.sub(
        r"if \(glfwGetWindowAttrib\(g_window_ptr, GLFW_MAXIMIZED\)\).*?glfwMaximizeWindow\(g_window_ptr\);",
        "if (g_luna_platform.maximize_toggle) g_luna_platform.maximize_toggle();",
        body, flags=re.DOTALL)
    body = body.replace("void load_gl_functions()", "static void load_gl_functions()")
    body = body.replace("GLuint compile_shader(", "static GLuint compile_shader(")
    # cache uniform loc block rename
    body = body.replace(
        "    glUniform1i_ = (PFNGLUNIFORM1IPROC)glfwGetProcAddress(\"glUniform1i\");",
        "    glUniform1i_ = (PFNGLUNIFORM1IPROC)g_luna_platform.get_proc(\"glUniform1i\");")
    return body

def main():
    with open(SRC, encoding="utf-8") as f:
        src = f.read()
    engine = extract_engine(src)
    out = API_HEADER + engine + API_FOOTER
    with open(DST, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"Wrote {DST} ({len(out)} bytes)")

if __name__ == "__main__":
    main()
