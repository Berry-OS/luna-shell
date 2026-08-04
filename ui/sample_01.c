/*
 * sample_01.c — Minimal Luna UI example (CSS-only layout + paint)
 *
 * Build (from this directory):
 *   gcc -O2 sample_01.c -o sample_01 -lglfw -lGL -lm -lpthread
 *   ./sample_01
 */

#define LUNA_UI_IMPLEMENTATION
#define LUNA_UI_GLFW
#include <stdio.h>
#include <string.h>
#include <GLFW/glfw3.h>
#include "luna-ui.h"

static GLFWwindow* g_window;

static void on_hello(LunaElement* e) {
    (void)e;
    int msg = luna_get_element_by_id("msg");
    if (msg != -1) {
        luna_set_text(msg, "Clicked! Styles come from CSS.");
        luna_add_class(msg, "highlight");
        luna_update_element_style(msg);
    }
}

static void on_close(LunaElement* e) {
    (void)e;
    glfwSetWindowShouldClose(g_window, GLFW_TRUE);
}

/* Pixel positions keep the demo portable; painting is still 100% CSS. */
static const char* SAMPLE_CSS =
    "html, body { margin: 0; padding: 0; width: 100%; height: 100%; }\n"
    "body {\n"
    "  background: linear-gradient(160deg, #1a1a2e 0%, #16213e 55%, #0f3460 100%);\n"
    "}\n"
    "#card {\n"
    "  position: absolute;\n"
    "  left: 190px; top: 140px;\n"
    "  width: 420px; height: 220px;\n"
    "  box-sizing: border-box;\n"
    "  border-radius: 16px;\n"
    "  background: rgba(255,255,255,0.10);\n"
    "  border: 1px solid rgba(255,255,255,0.18);\n"
    "  box-shadow: 0 18px 48px rgba(0,0,0,0.45);\n"
    "  z-index: 10;\n"
    "}\n"
    "#title {\n"
    "  position: absolute; left: 28px; top: 28px;\n"
    "  width: 360px; height: 36px;\n"
    "  font-size: 22px; font-weight: bold;\n"
    "  letter-spacing: 1px;\n"
    "  text-transform: uppercase;\n"
    "  color: #e2e8f0;\n"
    "}\n"
    "#msg {\n"
    "  position: absolute; left: 28px; top: 78px;\n"
    "  width: 360px; height: 48px;\n"
    "  font-size: 15px; color: rgba(226,232,240,0.72);\n"
    "}\n"
    "#msg.highlight { color: #34d399; }\n"
    "#hello_btn, #close_btn {\n"
    "  position: absolute; bottom: 28px; height: 40px; width: 120px;\n"
    "  border-radius: 10px; cursor: pointer;\n"
    "  font-size: 14px; font-weight: bold;\n"
    "  text-align: center; color: #fff;\n"
    "}\n"
    "#hello_btn {\n"
    "  left: 28px;\n"
    "  background: linear-gradient(135deg, #6366f1, #a855f7);\n"
    "}\n"
    "#hello_btn:hover { transform: scale(1.04); }\n"
    "#close_btn {\n"
    "  left: 272px;\n"
    "  background: rgba(255,255,255,0.12);\n"
    "  border: 1px solid rgba(255,255,255,0.20);\n"
    "}\n"
    "#close_btn:hover { background: rgba(255,255,255,0.18); }\n";

static const char* SAMPLE_HTML =
    "<!DOCTYPE html><html><head><title>Luna UI Sample</title></head>\n"
    "<body>\n"
    "  <div id=\"card\">\n"
    "    <div id=\"title\">Luna UI</div>\n"
    "    <div id=\"msg\">CSS layouts this card. Click Hello.</div>\n"
    "    <div id=\"hello_btn\" onclick=\"onHello\">Hello</div>\n"
    "    <div id=\"close_btn\" onclick=\"onClose\">Close</div>\n"
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
    g_window = glfwCreateWindow(800, 500, "Luna UI Sample", NULL, NULL);
    if (!g_window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);

    LunaPlatform plat = {0};
    plat.get_time = glfwGetTime;
    plat.get_proc = (LunaGetProcFn)glfwGetProcAddress;
    luna_set_platform(&plat);

    LunaInitConfig cfg = {800, 500, (LunaGetProcFn)glfwGetProcAddress, 0};
    if (!luna_init(&cfg)) {
        fprintf(stderr, "luna_init failed\n");
        return 1;
    }

    luna_register_js_handler("onHello", on_hello);
    luna_register_js_handler("onClose", on_close);

    luna_parse_css(SAMPLE_CSS);
    luna_parse_html(SAMPLE_HTML);
    luna_inject_body_background();
    luna_wire_onclick_handlers();
    luna_mark_layout_dirty();

    /* Snap animated style channels so the first frame is fully visible. */
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
    glfwSetFramebufferSizeCallback(g_window, on_fb);

    double prev = glfwGetTime();
    while (!glfwWindowShouldClose(g_window)) {
        double now = glfwGetTime();
        luna_update(now, now - prev);
        prev = now;

        int fbw, fbh;
        glfwGetFramebufferSize(g_window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
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
