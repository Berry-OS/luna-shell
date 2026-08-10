/*
 * Luna private absolute window placement client helper (luna_wm_v1).
 * Include once with LUNA_WM_CLIENT_IMPLEMENTATION defined.
 *
 * Requires: vendored wayland-client headers + wayland-client-rs
 * (see apps/luna-shell/wayland-client-rs/), GLFW native Wayland
 * (glfwGetWayland*).  Do not link system libwayland-client alongside
 * a different client ABI — use luna-wayland-client.mk / the vendored
 * .pc so GLFW's dlopen("libwayland-client.so.0") hits the same .so.
 */
#ifndef LUNA_WM_CLIENT_H
#define LUNA_WM_CLIENT_H

#include <GLFW/glfw3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind luna_wm_v1 if the compositor advertises it.  Safe on non-Luna. */
void luna_wm_client_init(GLFWwindow *window);
void luna_wm_client_shutdown(void);
int luna_wm_client_available(void);

/* Absolute content-box position.  Returns 1 if the request was sent. */
int luna_wm_client_set_pos(GLFWwindow *window, int x, int y);
int luna_wm_client_get_pos(GLFWwindow *window, int *x, int *y);
/* Interactive titlebar move (compositor grab).  Returns 1 if sent. */
int luna_wm_client_start_move(GLFWwindow *window);
/* Ask Luna shell for the window menu at surface-local (x,y).  Returns 1 if sent. */
int luna_wm_client_show_window_menu(GLFWwindow *window, int x, int y);

#ifdef __cplusplus
}
#endif

#ifdef LUNA_WM_CLIENT_IMPLEMENTATION

#if defined(GLFW_EXPOSE_NATIVE_WAYLAND) || defined(LUNA_WM_FORCE_WAYLAND)
#include <string.h>
/* Prefer vendored headers via -I.../wayland-client-rs/include */
#include <wayland-client.h>
#include "luna-wm-v1-client-protocol.h"

#ifndef LUNA_WM_PROTOCOL_C_INCLUDED
#define LUNA_WM_PROTOCOL_C_INCLUDED
#include "luna-wm-v1-protocol.c"
#endif

typedef struct LunaWmClient {
	struct wl_display *display;
	struct wl_registry *registry;
	struct luna_wm_v1 *wm;
	struct wl_surface *pending_surface;
	int pending_x, pending_y;
	int have_pending;
} LunaWmClient;

static LunaWmClient g_luna_wm_client;

static void luna_wm_registry_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface,
                                    uint32_t version)
{
	LunaWmClient *c = (LunaWmClient *)data;
	(void)version;
	if (strcmp(interface, "luna_wm_v1") == 0) {
		c->wm = wl_registry_bind(registry, name, &luna_wm_v1_interface, 1);
	}
}

static void luna_wm_registry_remove(void *data, struct wl_registry *registry,
                                    uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener luna_wm_registry_listener = {
	luna_wm_registry_global,
	luna_wm_registry_remove
};

static void luna_wm_position_evt(void *data, struct luna_wm_v1 *wm,
                                 struct wl_surface *surface, int32_t x, int32_t y)
{
	LunaWmClient *c = (LunaWmClient *)data;
	(void)wm;
	if (c->pending_surface && surface == c->pending_surface) {
		c->pending_x = x;
		c->pending_y = y;
		c->have_pending = 1;
	}
}

static const struct luna_wm_v1_listener luna_wm_listener = {
	luna_wm_position_evt
};

void luna_wm_client_init(GLFWwindow *window)
{
	memset(&g_luna_wm_client, 0, sizeof(g_luna_wm_client));
	if (!window) return;
	g_luna_wm_client.display = glfwGetWaylandDisplay();
	if (!g_luna_wm_client.display) return;
	g_luna_wm_client.registry = wl_display_get_registry(g_luna_wm_client.display);
	if (!g_luna_wm_client.registry) return;
	wl_registry_add_listener(g_luna_wm_client.registry, &luna_wm_registry_listener,
	                         &g_luna_wm_client);
	wl_display_roundtrip(g_luna_wm_client.display);
	if (g_luna_wm_client.wm) {
		luna_wm_v1_add_listener(g_luna_wm_client.wm, &luna_wm_listener,
		                        &g_luna_wm_client);
		wl_display_flush(g_luna_wm_client.display);
	}
}

void luna_wm_client_shutdown(void)
{
	if (g_luna_wm_client.wm) {
		luna_wm_v1_destroy(g_luna_wm_client.wm);
		g_luna_wm_client.wm = NULL;
	}
	if (g_luna_wm_client.registry) {
		wl_registry_destroy(g_luna_wm_client.registry);
		g_luna_wm_client.registry = NULL;
	}
	memset(&g_luna_wm_client, 0, sizeof(g_luna_wm_client));
}

int luna_wm_client_available(void)
{
	return g_luna_wm_client.wm != NULL;
}

int luna_wm_client_set_pos(GLFWwindow *window, int x, int y)
{
	struct wl_surface *surf;
	if (!g_luna_wm_client.wm || !window) return 0;
	surf = glfwGetWaylandWindow(window);
	if (!surf) return 0;
	luna_wm_v1_set_position(g_luna_wm_client.wm, surf, x, y);
	if (g_luna_wm_client.display) wl_display_flush(g_luna_wm_client.display);
	return 1;
}

int luna_wm_client_get_pos(GLFWwindow *window, int *x, int *y)
{
	struct wl_surface *surf;
	if (!g_luna_wm_client.wm || !window) return 0;
	surf = glfwGetWaylandWindow(window);
	if (!surf) return 0;
	g_luna_wm_client.pending_surface = surf;
	g_luna_wm_client.have_pending = 0;
	luna_wm_v1_get_position(g_luna_wm_client.wm, surf);
	wl_display_roundtrip(g_luna_wm_client.display);
	if (!g_luna_wm_client.have_pending) return 0;
	if (x) *x = g_luna_wm_client.pending_x;
	if (y) *y = g_luna_wm_client.pending_y;
	return 1;
}

int luna_wm_client_start_move(GLFWwindow *window)
{
	struct wl_surface *surf;
	if (!g_luna_wm_client.wm || !window) return 0;
	surf = glfwGetWaylandWindow(window);
	if (!surf) return 0;
	luna_wm_v1_start_move(g_luna_wm_client.wm, surf);
	if (g_luna_wm_client.display) wl_display_flush(g_luna_wm_client.display);
	return 1;
}

int luna_wm_client_show_window_menu(GLFWwindow *window, int x, int y)
{
	struct wl_surface *surf;
	if (!g_luna_wm_client.wm || !window) return 0;
	surf = glfwGetWaylandWindow(window);
	if (!surf) return 0;
	luna_wm_v1_show_window_menu(g_luna_wm_client.wm, surf, x, y);
	if (g_luna_wm_client.display) wl_display_flush(g_luna_wm_client.display);
	return 1;
}

#else /* no Wayland native */

void luna_wm_client_init(GLFWwindow *window) { (void)window; }
void luna_wm_client_shutdown(void) {}
int luna_wm_client_available(void) { return 0; }
int luna_wm_client_set_pos(GLFWwindow *window, int x, int y)
{
	(void)window;
	(void)x;
	(void)y;
	return 0;
}
int luna_wm_client_get_pos(GLFWwindow *window, int *x, int *y)
{
	(void)window;
	(void)x;
	(void)y;
	return 0;
}
int luna_wm_client_start_move(GLFWwindow *window)
{
	(void)window;
	return 0;
}
int luna_wm_client_show_window_menu(GLFWwindow *window, int x, int y)
{
	(void)window;
	(void)x;
	(void)y;
	return 0;
}

#endif /* Wayland */

#endif /* LUNA_WM_CLIENT_IMPLEMENTATION */
#endif /* LUNA_WM_CLIENT_H */
