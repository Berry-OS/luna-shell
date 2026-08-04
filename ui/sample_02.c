/*
 * sample_02.c — Luna UI showcase (CSS-only paint + text fields + JP IME)
 *
 * Build (from apps/luna-ui):
 *   make sample
 *   ./ui/sample
 *
 * Japanese input: focus a field and type with your system IME
 * (ibus / fcitx / mozc). Committed characters arrive via GLFW char callback.
 */

#define LUNA_UI_IMPLEMENTATION
#define LUNA_UI_GLFW
#include <stdio.h>
#include <string.h>
#include <GLFW/glfw3.h>
#include "luna-ui.h"

static GLFWwindow* g_window;

static void on_submit(LunaElement* e) {
    (void)e;
    int name = luna_get_element_by_id("field_name");
    int note = luna_get_element_by_id("field_note");
    int status = luna_get_element_by_id("status");
    char buf[640];
    snprintf(buf, sizeof(buf), "Submitted — %s / %s",
             name != -1 ? luna_get_value(name) : "",
             note != -1 ? luna_get_value(note) : "");
    if (status != -1) {
        luna_set_text(status, buf);
        luna_add_class(status, "ok");
        luna_update_element_style(status);
    }
}

static void on_clear(LunaElement* e) {
    (void)e;
    int ids[] = {
        luna_get_element_by_id("field_name"),
        luna_get_element_by_id("field_mail"),
        luna_get_element_by_id("field_pass"),
        luna_get_element_by_id("field_note"),
    };
    for (int i = 0; i < 4; i++) {
        if (ids[i] != -1) {
            luna_set_value(ids[i], "");
            luna_update_element_style(ids[i]);
        }
    }
    int status = luna_get_element_by_id("status");
    if (status != -1) {
        luna_set_text(status, "Cleared. Try Japanese IME in the name field.");
        luna_remove_class(status, "ok");
        luna_update_element_style(status);
    }
}

static void on_close(LunaElement* e) {
    (void)e;
    glfwSetWindowShouldClose(g_window, GLFW_TRUE);
}

/* Entire look is CSS — engine only paints resolved CSS boxes/text. */
static const char* SAMPLE_CSS =
    "html, body { margin: 0; padding: 0; width: 100%; height: 100%; }\n"
    "body {\n"
    "  background: linear-gradient(155deg, #0b1220 0%, #121a2f 42%, #1a2744 100%);\n"
    "}\n"
    "#glow {\n"
    "  position: absolute; left: -80px; top: -120px; width: 420px; height: 420px;\n"
    "  border-radius: 50%;\n"
    "  background: radial-gradient(circle, rgba(56,189,248,0.22) 0%, rgba(56,189,248,0) 70%);\n"
    "  pointer-events: none; z-index: 1;\n"
    "}\n"
    "#glow2 {\n"
    "  position: absolute; right: -60px; bottom: -100px; width: 380px; height: 380px;\n"
    "  border-radius: 50%;\n"
    "  background: radial-gradient(circle, rgba(244,114,182,0.18) 0%, rgba(244,114,182,0) 70%);\n"
    "  pointer-events: none; z-index: 1;\n"
    "}\n"
    "#shell {\n"
    "  position: absolute; left: 70px; top: 48px; width: 720px; height: 520px;\n"
    "  display: flex; flex-direction: row; gap: 0;\n"
    "  border-radius: 22px; overflow: hidden;\n"
    "  background: rgba(15,23,42,0.72);\n"
    "  border: 1px solid rgba(148,163,184,0.22);\n"
    "  box-shadow: 0 28px 80px rgba(0,0,0,0.55);\n"
    "  z-index: 10;\n"
    "}\n"
    "#rail {\n"
    "  width: 220px; height: 100%;\n"
    "  display: flex; flex-direction: column;\n"
    "  background: linear-gradient(180deg, rgba(30,41,59,0.95), rgba(15,23,42,0.98));\n"
    "  border-right: 1px solid rgba(148,163,184,0.16);\n"
    "  padding: 28px 22px; box-sizing: border-box;\n"
    "}\n"
    "#brand {\n"
    "  height: 36px; font-size: 22px; font-weight: bold; color: #f8fafc;\n"
    "  letter-spacing: 2px; text-transform: uppercase;\n"
    "}\n"
    "#tagline {\n"
    "  margin-top: 10px; height: 48px; font-size: 13px;\n"
    "  color: rgba(226,232,240,0.62); line-height: 1.4;\n"
    "}\n"
    "#chip {\n"
    "  margin-top: 28px; height: 34px; width: 150px;\n"
    "  border-radius: 999px; font-size: 12px; font-weight: bold;\n"
    "  text-align: center; color: #e0f2fe;\n"
    "  background: linear-gradient(135deg, #0ea5e9, #6366f1);\n"
    "  box-shadow: 0 8px 24px rgba(14,165,233,0.35);\n"
    "}\n"
    "#hint {\n"
    "  margin-top: auto; height: 64px; font-size: 12px;\n"
    "  color: rgba(148,163,184,0.9); line-height: 1.45;\n"
    "}\n"
    "#panel {\n"
    "  flex-grow: 1; height: 100%; padding: 28px 32px; box-sizing: border-box;\n"
    "  display: flex; flex-direction: column; gap: 12px;\n"
    "  background: linear-gradient(160deg, rgba(255,255,255,0.04), rgba(255,255,255,0.01));\n"
    "}\n"
    "#title {\n"
    "  height: 30px; font-size: 20px; font-weight: bold; color: #f1f5f9;\n"
    "}\n"
    "#subtitle {\n"
    "  height: 22px; font-size: 13px; color: rgba(148,163,184,0.95);\n"
    "}\n"
    ".label {\n"
    "  height: 18px; font-size: 12px; font-weight: bold;\n"
    "  color: rgba(186,230,253,0.85); letter-spacing: 0.5px;\n"
    "  text-transform: uppercase; margin-top: 6px;\n"
    "}\n"
    "input, textarea {\n"
    "  width: 100%; height: 42px; box-sizing: border-box;\n"
    "  border-radius: 12px; padding: 10px 14px;\n"
    "  font-size: 15px; color: #f8fafc;\n"
    "  background: rgba(2,6,23,0.55);\n"
    "  border: 1px solid rgba(148,163,184,0.28);\n"
    "  caret-color: #38bdf8;\n"
    "  outline-width: 2px; outline-color: #38bdf8; outline-offset: 2px;\n"
    "  transition: 0.15s;\n"
    "}\n"
    "input:focus, textarea:focus {\n"
    "  border-color: rgba(56,189,248,0.85);\n"
    "  background: rgba(2,6,23,0.78);\n"
    "  box-shadow: 0 0 0 3px rgba(56,189,248,0.18);\n"
    "}\n"
    "textarea {\n"
    "  height: 88px; white-space: normal;\n"
    "}\n"
    "#row {\n"
    "  display: flex; flex-direction: row; gap: 12px; height: 44px; margin-top: 10px;\n"
    "}\n"
    "#submit_btn, #clear_btn, #close_btn {\n"
    "  height: 42px; border-radius: 12px; cursor: pointer;\n"
    "  font-size: 14px; font-weight: bold; text-align: center; color: #fff;\n"
    "  transition: 0.12s;\n"
    "}\n"
    "#submit_btn {\n"
    "  width: 140px;\n"
    "  background: linear-gradient(135deg, #22d3ee, #6366f1);\n"
    "  box-shadow: 0 10px 28px rgba(99,102,241,0.35);\n"
    "}\n"
    "#submit_btn:hover { transform: scale(1.03); }\n"
    "#clear_btn {\n"
    "  width: 110px;\n"
    "  background: rgba(148,163,184,0.14);\n"
    "  border: 1px solid rgba(148,163,184,0.28);\n"
    "}\n"
    "#clear_btn:hover { background: rgba(148,163,184,0.22); }\n"
    "#close_btn {\n"
    "  width: 96px; margin-left: auto;\n"
    "  background: rgba(248,113,113,0.16);\n"
    "  border: 1px solid rgba(248,113,113,0.35); color: #fecaca;\n"
    "}\n"
    "#close_btn:hover { background: rgba(248,113,113,0.28); }\n"
    "#status {\n"
    "  height: 28px; font-size: 13px; color: rgba(148,163,184,0.9);\n"
    "}\n"
    "#status.ok { color: #34d399; }\n";

static const char* SAMPLE_HTML =
    "<!DOCTYPE html><html><head><title>Luna UI</title></head>\n"
    "<body>\n"
    "  <div id=\"glow\"></div>\n"
    "  <div id=\"glow2\"></div>\n"
    "  <div id=\"shell\">\n"
    "    <div id=\"rail\">\n"
    "      <div id=\"brand\">Luna</div>\n"
    "      <div id=\"tagline\">CSS-painted UI with real text fields and Japanese IME.</div>\n"
    "      <div id=\"chip\">CSS / OpenGL</div>\n"
    "      <div id=\"hint\">Tab to move focus. Click a field and type — mozc / ibus works via system IME.</div>\n"
    "    </div>\n"
    "    <div id=\"panel\">\n"
    "      <div id=\"title\">Profile studio</div>\n"
    "      <div id=\"subtitle\">Everything you see is driven by CSS boxes.</div>\n"
    "      <div class=\"label\">Display name</div>\n"
    "      <input id=\"field_name\" type=\"text\" placeholder=\"山田 太郎 / Luna\" value=\"\" />\n"
    "      <div class=\"label\">Email</div>\n"
    "      <input id=\"field_mail\" type=\"text\" placeholder=\"you@example.com\" />\n"
    "      <div class=\"label\">Password</div>\n"
    "      <input id=\"field_pass\" type=\"password\" placeholder=\"••••••••\" />\n"
    "      <div class=\"label\">Note</div>\n"
    "      <textarea id=\"field_note\" placeholder=\"日本語メモをどうぞ\"></textarea>\n"
    "      <div id=\"row\">\n"
    "        <div id=\"submit_btn\" onclick=\"onSubmit\">Submit</div>\n"
    "        <div id=\"clear_btn\" onclick=\"onClear\">Clear</div>\n"
    "        <div id=\"close_btn\" onclick=\"onClose\">Close</div>\n"
    "      </div>\n"
    "      <div id=\"status\">Ready — focus a text box to type.</div>\n"
    "    </div>\n"
    "  </div>\n"
    "</body></html>\n";

static void on_mouse(GLFWwindow* w, double x, double y) {
    (void)w; luna_mouse_move(x, y);
}

static void on_button(GLFWwindow* w, int button, int action, int mods) {
    double x, y; glfwGetCursorPos(w, &x, &y);
    luna_mouse_button(button, action, mods, x, y);
}

static void on_scroll(GLFWwindow* w, double xoff, double yoff) {
    (void)w; luna_scroll(xoff, yoff);
}

static void on_key(GLFWwindow* w, int key, int scancode, int action, int mods) {
    (void)w; luna_key(key, scancode, action, mods);
}

static void on_char(GLFWwindow* w, unsigned int codepoint) {
    (void)w; luna_char(codepoint);
}

static void on_fb(GLFWwindow* w, int width, int height) {
    (void)w;
    luna_resize((float)width, (float)height);
    glViewport(0, 0, width, height);
}

int main(void) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    g_window = glfwCreateWindow(860, 620, "Luna UI — CSS + Text Fields", NULL, NULL);
    if (!g_window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);

    LunaPlatform plat = {0};
    plat.get_time = glfwGetTime;
    plat.get_proc = (LunaGetProcFn)glfwGetProcAddress;
    luna_set_platform(&plat);

    LunaInitConfig cfg = {860, 620, (LunaGetProcFn)glfwGetProcAddress, 0};
    if (!luna_init(&cfg)) {
        fprintf(stderr, "luna_init failed\n");
        return 1;
    }

    luna_register_js_handler("onSubmit", on_submit);
    luna_register_js_handler("onClear", on_clear);
    luna_register_js_handler("onClose", on_close);

    luna_parse_css(SAMPLE_CSS);
    luna_parse_html(SAMPLE_HTML);
    luna_inject_body_background();
    luna_wire_onclick_handlers();
    luna_mark_layout_dirty();

    for (int i = 0; i < luna_element_count(); i++) {
        LunaElement* e = luna_element_at(i);
        e->cur_r = e->r; e->cur_g = e->g; e->cur_b = e->b; e->cur_a = e->a;
        e->cur_bd_r = e->bd_r; e->cur_bd_g = e->bd_g;
        e->cur_bd_b = e->bd_b; e->cur_bd_a = e->bd_a;
        e->cur_scale = e->transform_scale > 0.0f ? e->transform_scale : 1.0f;
        e->cur_tx = e->transform_tx;
        e->cur_ty = e->transform_ty;
    }

    glfwSetCursorPosCallback(g_window, on_mouse);
    glfwSetMouseButtonCallback(g_window, on_button);
    glfwSetScrollCallback(g_window, on_scroll);
    glfwSetKeyCallback(g_window, on_key);
    glfwSetCharCallback(g_window, on_char);
    glfwSetFramebufferSizeCallback(g_window, on_fb);

    double prev = glfwGetTime();
    while (!glfwWindowShouldClose(g_window)) {
        double now = glfwGetTime();
        luna_update(now, now - prev);
        prev = now;

        int fbw, fbh;
        glfwGetFramebufferSize(g_window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        luna_render(fbw, fbh);

        glfwSwapBuffers(g_window);
        glfwPollEvents();
    }

    luna_shutdown();
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
