/* luna_linux.h - header-only GLFW/OpenGL host for Luna UI
 *
 * Linux applications only need GLFW and OpenGL at build time. This header does
 * not include Xlib, Xutil or GLX headers and does not call X11 directly.
 */

#if defined(LUNA_UI_PLATFORM_PRELUDE)
#ifndef LUNA_LINUX_PRELUDE_INCLUDED
#define LUNA_LINUX_PRELUDE_INCLUDED

#include <GLFW/glfw3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#define LUNA_UI_PLATFORM_GL_INCLUDED 1
#endif
#endif /* LUNA_UI_PLATFORM_PRELUDE */

#if defined(LUNA_UI_PLATFORM_BODY)
#ifndef LUNA_LINUX_BODY_INCLUDED
#define LUNA_LINUX_BODY_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LunaLinuxOptions {
    /* Retained for source compatibility. GLFW selects the display backend. */
    const char* display_name;
    int screen;
} LunaLinuxOptions;

void luna_linux_set_options(const LunaLinuxOptions* options);
GLFWwindow* luna_linux_window(void);

#ifdef __cplusplus
}
#endif
#endif /* LUNA_LINUX_BODY_INCLUDED */

#if defined(LUNA_UI_IMPLEMENTATION) && !defined(LUNA_LINUX_IMPLEMENTATION_INCLUDED)
#define LUNA_LINUX_IMPLEMENTATION_INCLUDED

typedef struct LunaLinuxState {
    GLFWwindow* window;
    GLFWcursor* cursors[6];
    int window_width;
    int window_height;
    int framebuffer_width;
    int framebuffer_height;
    int glfw_initialized;
    LunaLinuxOptions options;
    LunaAppConfig config;
} LunaLinuxState;

static LunaLinuxState luna_linux_state;
static LunaLinuxOptions luna_linux_options;

static void luna_linux_error_callback(int code, const char* description) {
    fprintf(stderr, "[luna-ui] GLFW error %d: %s\n", code,
            description ? description : "unknown error");
}

static void luna_linux_sleep_millis(long milliseconds) {
    struct timeval timeout;
    if (milliseconds <= 0) return;
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    (void)select(0, NULL, NULL, NULL, &timeout);
}

static double luna_linux_time_impl(void) {
    return glfwGetTime();
}

static void* luna_linux_get_proc(const char* name) {
    GLFWglproc proc;
    void* symbol;
    if (!name) return NULL;
    proc = glfwGetProcAddress(name);
    if (!proc) return NULL;
    /* ISO C does not guarantee conversion between function and data pointers.
     * GLFW exposes GL entry points as GLFWglproc, while Luna's host ABI uses
     * void*. Copy the representation without an explicit cast warning. */
    symbol = NULL;
    memcpy(&symbol, &proc,
           sizeof(symbol) < sizeof(proc) ? sizeof(symbol) : sizeof(proc));
    return symbol;
}

static void luna_linux_set_cursor_impl(int type) {
    int shape = GLFW_ARROW_CURSOR;
    GLFWcursor* cursor;
    if (!luna_linux_state.window) return;
    if (type < 0 || type >= 6) type = 0;

    switch (type) {
        case 1: shape = GLFW_HAND_CURSOR; break;
        case 2: shape = GLFW_IBEAM_CURSOR; break;
        case 3: shape = GLFW_CROSSHAIR_CURSOR; break;
        case 4: shape = GLFW_HRESIZE_CURSOR; break;
        case 5: shape = GLFW_VRESIZE_CURSOR; break;
        default: shape = GLFW_ARROW_CURSOR; break;
    }

    cursor = luna_linux_state.cursors[type];
    if (!cursor) {
        cursor = glfwCreateStandardCursor(shape);
        luna_linux_state.cursors[type] = cursor;
    }
    glfwSetCursor(luna_linux_state.window, cursor);
}

static void luna_linux_close_impl(void) {
    if (luna_linux_state.window)
        glfwSetWindowShouldClose(luna_linux_state.window, GLFW_TRUE);
}

static void luna_linux_redraw_impl(void) {
    /* luna_app_run currently paints continuously, so no wake-up is required. */
}

static void luna_linux_iconify_impl(void) {
    if (luna_linux_state.window) glfwIconifyWindow(luna_linux_state.window);
}

static void luna_linux_maximize_impl(void) {
    if (!luna_linux_state.window) return;
#if defined(GLFW_MAXIMIZED)
    if (glfwGetWindowAttrib(luna_linux_state.window, GLFW_MAXIMIZED))
        glfwRestoreWindow(luna_linux_state.window);
    else
        glfwMaximizeWindow(luna_linux_state.window);
#else
    glfwMaximizeWindow(luna_linux_state.window);
#endif
}

static void luna_linux_set_clipboard_impl(const char* utf8) {
    if (luna_linux_state.window)
        glfwSetClipboardString(luna_linux_state.window, utf8 ? utf8 : "");
}

static char* luna_linux_get_clipboard_impl(void) {
    const char* text;
    if (!luna_linux_state.window) return NULL;
    text = glfwGetClipboardString(luna_linux_state.window);
    return text ? luna_strdup_local(text) : NULL;
}

static float luna_linux_scale_impl(void) {
#if GLFW_VERSION_MAJOR > 3 || \
    (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xscale = 1.0f;
    float yscale = 1.0f;
    if (!luna_linux_state.window) return 1.0f;
    glfwGetWindowContentScale(luna_linux_state.window, &xscale, &yscale);
    if (xscale <= 0.0f || xscale > 8.0f) xscale = 1.0f;
    if (yscale <= 0.0f || yscale > 8.0f) yscale = 1.0f;
    return xscale > yscale ? xscale : yscale;
#else
    return 1.0f;
#endif
}

static void luna_linux_cursor_position_callback(GLFWwindow* window,
                                                double x, double y) {
    (void)window;
    luna_mouse_move(x, y);
}

static void luna_linux_mouse_button_callback(GLFWwindow* window,
                                             int button, int action, int mods) {
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    luna_mouse_button(button, action, mods, x, y);
}

static void luna_linux_scroll_callback(GLFWwindow* window,
                                       double xoffset, double yoffset) {
    (void)window;
    luna_scroll(xoffset, yoffset);
}

static void luna_linux_key_callback(GLFWwindow* window, int key, int scancode,
                                    int action, int mods) {
    (void)window;
    luna_key(key, scancode, action, mods);
}

static void luna_linux_char_callback(GLFWwindow* window, unsigned int codepoint) {
    (void)window;
    luna_char(codepoint);
}

static void luna_linux_window_size_callback(GLFWwindow* window,
                                            int width, int height) {
    (void)window;
    luna_linux_state.window_width = width;
    luna_linux_state.window_height = height;
    if (width > 0 && height > 0)
        luna_resize((float)width, (float)height);
}

static void luna_linux_framebuffer_size_callback(GLFWwindow* window,
                                                 int width, int height) {
    (void)window;
    luna_linux_state.framebuffer_width = width;
    luna_linux_state.framebuffer_height = height;
    luna_framebuffer_resized();
}

#if GLFW_VERSION_MAJOR > 3 || \
    (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
static void luna_linux_content_scale_callback(GLFWwindow* window,
                                              float xscale, float yscale) {
    (void)window;
    (void)xscale;
    (void)yscale;
    luna_framebuffer_resized();
    luna_linux_redraw_impl();
}
#endif

static void luna_linux_install_callbacks(GLFWwindow* window) {
    glfwSetCursorPosCallback(window, luna_linux_cursor_position_callback);
    glfwSetMouseButtonCallback(window, luna_linux_mouse_button_callback);
    glfwSetScrollCallback(window, luna_linux_scroll_callback);
    glfwSetKeyCallback(window, luna_linux_key_callback);
    glfwSetCharCallback(window, luna_linux_char_callback);
    glfwSetWindowSizeCallback(window, luna_linux_window_size_callback);
    glfwSetFramebufferSizeCallback(window, luna_linux_framebuffer_size_callback);
#if GLFW_VERSION_MAJOR > 3 || \
    (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    glfwSetWindowContentScaleCallback(window, luna_linux_content_scale_callback);
#endif
}

static void luna_linux_destroy_host(void) {
    int i;
    for (i = 0; i < 6; ++i) {
        if (luna_linux_state.cursors[i]) {
            glfwDestroyCursor(luna_linux_state.cursors[i]);
            luna_linux_state.cursors[i] = NULL;
        }
    }
    if (luna_linux_state.window) {
        glfwDestroyWindow(luna_linux_state.window);
        luna_linux_state.window = NULL;
    }
    if (luna_linux_state.glfw_initialized) {
        glfwTerminate();
        luna_linux_state.glfw_initialized = 0;
    }
}

void luna_linux_set_options(const LunaLinuxOptions* options) {
    if (options) luna_linux_options = *options;
    else memset(&luna_linux_options, 0, sizeof(luna_linux_options));
}

GLFWwindow* luna_linux_window(void) {
    return luna_linux_state.window;
}

void* luna_app_native_handle(void) {
    /* Deliberately returns GLFWwindow*, not an X11 Window. Applications that
     * require a native X11/Wayland handle may use GLFW's native-access API in
     * their own platform-specific translation unit. */
    return (void*)luna_linux_state.window;
}

void luna_app_quit(void) {
    luna_linux_close_impl();
}

void luna_app_request_redraw(void) {
    luna_linux_redraw_impl();
}

int luna_app_run(const LunaAppConfig* user_config) {
    LunaAppConfig config;
    LunaPlatform platform;
    LunaInitConfig init;
    GLFWwindow* window;
    double previous;
    int luna_initialized = 0;
    int result = 1;

    memset(&config, 0, sizeof(config));
    if (user_config) config = *user_config;
    if (!config.title) config.title = "Luna UI";
    if (config.width <= 0) config.width = 1024;
    if (config.height <= 0) config.height = 768;
    if (!user_config) {
        config.resizable = 1;
        config.vsync = 1;
    }

    memset(&luna_linux_state, 0, sizeof(luna_linux_state));
    luna_linux_state.options = luna_linux_options;
    luna_linux_state.config = config;

    glfwSetErrorCallback(luna_linux_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "[luna-ui] glfwInit failed\n");
        return 1;
    }
    luna_linux_state.glfw_initialized = 1;

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, config.frameless ? GLFW_FALSE : GLFW_TRUE);
#ifdef GLFW_TRANSPARENT_FRAMEBUFFER
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,
                   config.transparent ? GLFW_TRUE : GLFW_FALSE);
#endif
#ifdef GLFW_SCALE_TO_MONITOR
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

    window = glfwCreateWindow(config.width, config.height, config.title, NULL, NULL);
    if (!window) {
        fprintf(stderr, "[luna-ui] glfwCreateWindow failed (OpenGL 3.3 required)\n");
        goto cleanup;
    }
    luna_linux_state.window = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(config.vsync ? 1 : 0);
    luna_linux_install_callbacks(window);

    glfwGetWindowSize(window, &luna_linux_state.window_width,
                      &luna_linux_state.window_height);
    glfwGetFramebufferSize(window, &luna_linux_state.framebuffer_width,
                          &luna_linux_state.framebuffer_height);

    memset(&platform, 0, sizeof(platform));
    platform.struct_size = sizeof(platform);
    platform.api_version = LUNA_UI_API_VERSION;
    platform.get_time = luna_linux_time_impl;
    platform.get_proc = luna_linux_get_proc;
    platform.set_cursor = luna_linux_set_cursor_impl;
    platform.request_close = luna_linux_close_impl;
    platform.iconify = luna_linux_iconify_impl;
    platform.maximize_toggle = luna_linux_maximize_impl;
    platform.request_redraw = luna_linux_redraw_impl;
    platform.set_clipboard = luna_linux_set_clipboard_impl;
    platform.get_clipboard = luna_linux_get_clipboard_impl;
    platform.get_scale = luna_linux_scale_impl;
    luna_set_platform(&platform);

    memset(&init, 0, sizeof(init));
    init.width = (float)luna_linux_state.window_width;
    init.height = (float)luna_linux_state.window_height;
    init.get_proc = luna_linux_get_proc;
    init.frameless = config.frameless;
    if (!luna_init(&init)) {
        fprintf(stderr, "[luna-ui] luna_init failed\n");
        goto cleanup;
    }
    luna_initialized = 1;

    if (config.html) luna_parse_html(config.html);
    else if (config.html_path && !luna_load_html_file(config.html_path))
        fprintf(stderr, "[luna-ui] could not load HTML: %s\n", config.html_path);

    if (config.css) luna_parse_css(config.css);
    else if (config.css_path && !luna_load_css_file(config.css_path))
        fprintf(stderr, "[luna-ui] could not load CSS: %s\n", config.css_path);

    luna_inject_body_background();
    if (config.on_init) config.on_init(config.userdata);
    luna_wire_onclick_handlers();

    glfwShowWindow(window);
    previous = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double now;
        double dt;
        int framebuffer_width;
        int framebuffer_height;

        glfwPollEvents();
        now = glfwGetTime();
        dt = now - previous;
        previous = now;
        if (dt < 0.0 || dt > 0.25) dt = 1.0 / 60.0;

        if (config.on_frame) config.on_frame(dt, config.userdata);
        luna_update(now, dt);

        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        luna_linux_state.framebuffer_width = framebuffer_width;
        luna_linux_state.framebuffer_height = framebuffer_height;
        if (framebuffer_width > 0 && framebuffer_height > 0) {
            luna_render(framebuffer_width, framebuffer_height);
            glfwSwapBuffers(window);
        }

        if (!config.vsync) luna_linux_sleep_millis(1);
    }

    result = 0;

cleanup:
    if (luna_initialized) {
        if (config.on_shutdown) config.on_shutdown(config.userdata);
        luna_shutdown();
    }
    luna_linux_destroy_host();
    return result;
}

#endif /* LUNA_UI_IMPLEMENTATION */
#endif /* LUNA_UI_PLATFORM_BODY */
