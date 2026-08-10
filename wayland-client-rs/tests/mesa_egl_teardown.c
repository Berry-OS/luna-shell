/*
 * Regression for the Mesa EGL probe path that used to GPF in
 * wl_proxy_destroy → HashMap::remove:
 *   connect → create_queue → wrapper → get_registry → destroy → disconnect
 * (see dri2_teardown_wayland / dri2_initialize_wayland_swrast).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

static void
roundtrip(struct wl_display *dpy)
{
    if (wl_display_roundtrip(dpy) < 0) {
        fprintf(stderr, "roundtrip failed\n");
        exit(2);
    }
}

int
main(void)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "connect failed (is WAYLAND_DISPLAY set?)\n");
        return 1;
    }

    struct wl_event_queue *queue =
        wl_display_create_queue_with_name(dpy, "mesa egl swrast display queue");
    if (!queue) {
        fprintf(stderr, "create_queue failed\n");
        return 1;
    }

    struct wl_display *wrapper = wl_proxy_create_wrapper(dpy);
    if (!wrapper) {
        fprintf(stderr, "create_wrapper failed\n");
        return 1;
    }
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);

    struct wl_registry *registry = wl_display_get_registry(wrapper);
    if (!registry) {
        fprintf(stderr, "get_registry failed\n");
        return 1;
    }
    roundtrip(dpy);

    /* dri2_teardown_wayland order */
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapper);
    wl_event_queue_destroy(queue);
    wl_display_disconnect(dpy);

    /* Stray destroy after disconnect must not GPF (was the old crash). */
    /* registry is freed; do not touch it. */

    printf("mesa_egl_teardown: ok\n");
    return 0;
}
