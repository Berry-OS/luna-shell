#ifndef LUNA_SHELL_INTERACTION_FIXES_H
#define LUNA_SHELL_INTERACTION_FIXES_H

/*
 * Compatibility hooks for shell input/live-widget regressions.
 *
 * This file is force-included before luna-shell.c (through
 * luna-client-env-policy.h), so it intentionally includes no system headers.
 * The macros below preserve the original function declarations/definitions and
 * only wrap call sites later in luna-shell.c, after the shell's types/statics
 * are visible.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LUNA_SHELL_FIX_UNUSED __attribute__((unused))
#else
#define LUNA_SHELL_FIX_UNUSED
#endif

static int g_luna_shell_fix_pending_right LUNA_SHELL_FIX_UNUSED;
static double g_luna_shell_fix_right_x LUNA_SHELL_FIX_UNUSED;
static double g_luna_shell_fix_right_y LUNA_SHELL_FIX_UNUSED;
static const void *g_luna_shell_fix_bg_surface LUNA_SHELL_FIX_UNUSED;
static unsigned long long g_luna_shell_fix_bg_sig LUNA_SHELL_FIX_UNUSED;
static int g_luna_shell_fix_bg_sig_valid LUNA_SHELL_FIX_UNUSED;

static inline void luna_shell_fix_note_mouse_button(int button, int action,
                                                     int mods, double x, double y)
    LUNA_SHELL_FIX_UNUSED;
static inline void luna_shell_fix_note_mouse_button(int button, int action,
                                                     int mods, double x, double y) {
    (void)mods;
    /* LUNA_MOUSE_BUTTON_RIGHT == 1, LUNA_PRESS == 1 in luna-keys.h.  Literal
     * values keep this prelude independent from headers included later. */
    if (button == 1 && action == 1) {
        g_luna_shell_fix_pending_right = 1;
        g_luna_shell_fix_right_x = x;
        g_luna_shell_fix_right_y = y;
    }
}

/* Tiny preprocessor predicates used to distinguish a typed function
 * declaration/definition from an ordinary call. */
#define LUNA_SHELL_FIX_CAT_I(a, b) a##b
#define LUNA_SHELL_FIX_CAT(a, b) LUNA_SHELL_FIX_CAT_I(a, b)
#define LUNA_SHELL_FIX_ARG3(_0, _1, _2, ...) _2
#define LUNA_SHELL_FIX_HAS_COMMA(...) LUNA_SHELL_FIX_ARG3(__VA_ARGS__, 1, 0)
#define LUNA_SHELL_FIX_IS_INT_int ,
#define LUNA_SHELL_FIX_IS_VOID_void ,
#define LUNA_SHELL_FIX_IS_INT(x) \
    LUNA_SHELL_FIX_HAS_COMMA(LUNA_SHELL_FIX_CAT(LUNA_SHELL_FIX_IS_INT_, x))
#define LUNA_SHELL_FIX_IS_VOID(x) \
    LUNA_SHELL_FIX_HAS_COMMA(LUNA_SHELL_FIX_CAT(LUNA_SHELL_FIX_IS_VOID_, x))

/* Preserve the public declaration/implementation in luna-ui.h; wrap only
 * runtime calls so a right-button press can be handled before release-time
 * click bubbling. */
#define LUNA_SHELL_FIX_MOUSE_DECL(button, action, mods, x, y) \
    luna_mouse_button(button, action, mods, x, y)
#define LUNA_SHELL_FIX_MOUSE_CALL(button, action, mods, x, y) \
    (luna_mouse_button((button), (action), (mods), (x), (y)), \
     luna_shell_fix_note_mouse_button((button), (action), (mods), (x), (y)))
#define LUNA_SHELL_FIX_MOUSE_SELECT(v) LUNA_SHELL_FIX_CAT(LUNA_SHELL_FIX_MOUSE_, v)
#define LUNA_SHELL_FIX_MOUSE_1 LUNA_SHELL_FIX_MOUSE_DECL
#define LUNA_SHELL_FIX_MOUSE_0 LUNA_SHELL_FIX_MOUSE_CALL
#define luna_mouse_button(button, action, mods, x, y) \
    LUNA_SHELL_FIX_MOUSE_SELECT(LUNA_SHELL_FIX_IS_INT(button))(button, action, mods, x, y)

/* Fill the two legacy Storage cache slots that old layouts omitted.  This is
 * deliberately DOM-based so every installed skin benefits without rewriting
 * its HTML. */
#define LUNA_SHELL_FIX_RESOLVE_DISK_IDS() do { \
    if (g_ui_idx[UI_ST_DISK_VAL] < 0 || g_ui_idx[UI_ST_DISK_FILL] < 0) { \
        int _lsf_stats = luna_get_element_by_id("widget_stats"); \
        if (_lsf_stats >= 0) { \
            for (int _lsf_i = 0; _lsf_i < luna_element_count(); _lsf_i++) { \
                LunaElement *_lsf_e = luna_element_at(_lsf_i); \
                if (!_lsf_e) continue; \
                int _lsf_inside = (_lsf_i == _lsf_stats); \
                for (int _lsf_p = _lsf_e->parent_idx; !_lsf_inside && _lsf_p != -1; \
                     _lsf_p = luna_element_at(_lsf_p)->parent_idx) \
                    if (_lsf_p == _lsf_stats) _lsf_inside = 1; \
                if (!_lsf_inside) continue; \
                if (g_ui_idx[UI_ST_DISK_FILL] < 0 && \
                    strstr(_lsf_e->class_name, "g_disk")) \
                    g_ui_idx[UI_ST_DISK_FILL] = _lsf_i; \
                if (g_ui_idx[UI_ST_DISK_VAL] < 0 && \
                    strstr(_lsf_e->class_name, "st_val") && \
                    _lsf_i != g_ui_idx[UI_ST_CPU_VAL] && \
                    _lsf_i != g_ui_idx[UI_ST_MEM_VAL]) \
                    g_ui_idx[UI_ST_DISK_VAL] = _lsf_i; \
            } \
        } \
    } \
} while (0)

/* update_async_status still had a !shell_desktop_busy() gate after #11.  When
 * widgets are enabled, suppress only that gate for the duration of this status
 * apply; the main loop's interaction deferral and the 2-second monitor worker
 * still bound update cadence. */
#define LUNA_SHELL_FIX_STATUS_DECL(arg) update_async_status(arg)
#define LUNA_SHELL_FIX_STATUS_CALL(arg) do { \
    LUNA_SHELL_FIX_RESOLVE_DISK_IDS(); \
    int _lsf_visible_windows = g_visible_window_count; \
    if (g_settings.widgets_enabled) g_visible_window_count = 0; \
    update_async_status(); \
    g_visible_window_count = _lsf_visible_windows; \
} while (0)
#define LUNA_SHELL_FIX_STATUS_1 LUNA_SHELL_FIX_STATUS_DECL
#define LUNA_SHELL_FIX_STATUS_0 LUNA_SHELL_FIX_STATUS_CALL
#define LUNA_SHELL_FIX_STATUS_SELECT(v) LUNA_SHELL_FIX_CAT(LUNA_SHELL_FIX_STATUS_, v)
#define update_async_status(arg) \
    LUNA_SHELL_FIX_STATUS_SELECT(LUNA_SHELL_FIX_IS_VOID(arg))(arg)

/* Re-publish the sparse background input region whenever the Wayland surface,
 * widget geometry, or widgets-enabled state changes.  The original #11 cache
 * was function-static and could incorrectly suppress the first region on a
 * recreated wl_surface, leaving stopwatch controls click-through. */
#define LUNA_SHELL_FIX_SYNC_BG_INPUT() do { \
    if (g_backend == &g_wl_backend && g_wl.compositor && \
        g_surfs[LUNA_SURF_BG].wl_surf) { \
        LunaSurface *_lsf_bg = &g_surfs[LUNA_SURF_BG]; \
        unsigned long long _lsf_sig = 1469598103934665603ULL; \
        int _lsf_rects[3][4]; \
        int _lsf_n = 0; \
        const char *_lsf_ids[3] = { "widget_clock", "widget_stats", "widget_weather" }; \
        if (g_settings.widgets_enabled) { \
            for (int _lsf_wi = 0; _lsf_wi < 3; _lsf_wi++) { \
                int _lsf_idx = luna_get_element_by_id(_lsf_ids[_lsf_wi]); \
                LunaElement *_lsf_e = _lsf_idx >= 0 ? luna_element_at(_lsf_idx) : 0; \
                if (!_lsf_e || _lsf_e->w <= 0.0f || _lsf_e->h <= 0.0f) continue; \
                int _lsf_x = (int)floorf(_lsf_e->x - _lsf_bg->doc_x); \
                int _lsf_y = (int)floorf(_lsf_e->y - _lsf_bg->doc_y); \
                int _lsf_w = (int)ceilf(_lsf_e->w); \
                int _lsf_h = (int)ceilf(_lsf_e->h); \
                if (_lsf_w <= 0 || _lsf_h <= 0) continue; \
                _lsf_rects[_lsf_n][0] = _lsf_x; \
                _lsf_rects[_lsf_n][1] = _lsf_y; \
                _lsf_rects[_lsf_n][2] = _lsf_w; \
                _lsf_rects[_lsf_n][3] = _lsf_h; \
                _lsf_n++; \
                unsigned _lsf_vals[4] = { (unsigned)_lsf_x, (unsigned)_lsf_y, \
                                          (unsigned)_lsf_w, (unsigned)_lsf_h }; \
                for (int _lsf_v = 0; _lsf_v < 4; _lsf_v++) { \
                    _lsf_sig ^= (unsigned long long)_lsf_vals[_lsf_v]; \
                    _lsf_sig *= 1099511628211ULL; \
                } \
            } \
        } \
        _lsf_sig ^= (unsigned long long)_lsf_n; \
        _lsf_sig *= 1099511628211ULL; \
        if (!g_luna_shell_fix_bg_sig_valid || \
            g_luna_shell_fix_bg_surface != (const void *)_lsf_bg->wl_surf || \
            g_luna_shell_fix_bg_sig != _lsf_sig) { \
            struct wl_region *_lsf_region = wl_compositor_create_region(g_wl.compositor); \
            if (_lsf_region) { \
                for (int _lsf_ri = 0; _lsf_ri < _lsf_n; _lsf_ri++) \
                    wl_region_add(_lsf_region, _lsf_rects[_lsf_ri][0], \
                                  _lsf_rects[_lsf_ri][1], _lsf_rects[_lsf_ri][2], \
                                  _lsf_rects[_lsf_ri][3]); \
                wl_surface_set_input_region(_lsf_bg->wl_surf, _lsf_region); \
                wl_region_destroy(_lsf_region); \
                wl_surface_commit(_lsf_bg->wl_surf); \
                g_luna_shell_fix_bg_surface = (const void *)_lsf_bg->wl_surf; \
                g_luna_shell_fix_bg_sig = _lsf_sig; \
                g_luna_shell_fix_bg_sig_valid = 1; \
            } \
        } \
    } \
} while (0)

/* Runs once per outer shell iteration at an existing cheap maintenance point.
 * This makes .tip_open backend-neutral, handles window context menus on the
 * press (then consumes the release), and refreshes the sparse widget input
 * region when necessary. */
#define LUNA_SHELL_FIX_MAINTENANCE() do { \
    if (tray_sync_hover_tip()) shell_request_repaint(1); \
    if (g_luna_shell_fix_pending_right) { \
        g_luna_shell_fix_pending_right = 0; \
        int _lsf_hit = hit_test_at(g_luna_shell_fix_right_x, g_luna_shell_fix_right_y); \
        for (int _lsf_i = _lsf_hit; _lsf_i != -1; ) { \
            LunaElement *_lsf_e = luna_element_at(_lsf_i); \
            if (!_lsf_e) break; \
            const char *_lsf_id = _lsf_e->id; \
            int _lsf_handled = 0; \
            if (!strncmp(_lsf_id, "tray_", 5) && \
                _lsf_id[5] >= '0' && _lsf_id[5] <= '9') { \
                int _lsf_slot = _lsf_id[5] - '0'; \
                if (_lsf_slot >= 0 && _lsf_slot < MAX_TRAY_SLOTS && \
                    tray_key_is_window(g_tray_slot_key[_lsf_slot])) { \
                    on_tray_click(luna_element_at(_lsf_hit)); \
                    _lsf_handled = 1; \
                } \
            } else if (!strncmp(_lsf_id, "win_", 4) && \
                       _lsf_id[4] >= '0' && _lsf_id[4] <= '9') { \
                on_win_click(luna_element_at(_lsf_hit)); \
                _lsf_handled = 1; \
            } \
            if (_lsf_handled) { \
                luna_consume_pointer_event(); \
                shell_request_repaint(-1); \
                break; \
            } \
            _lsf_i = _lsf_e->parent_idx; \
        } \
    } \
    LUNA_SHELL_FIX_SYNC_BG_INPUT(); \
} while (0)

#define LUNA_SHELL_FIX_CURSOR_DECL(arg) cursor_theme_tick_and_refresh(arg)
#define LUNA_SHELL_FIX_CURSOR_CALL(arg) do { \
    cursor_theme_tick_and_refresh(); \
    LUNA_SHELL_FIX_MAINTENANCE(); \
} while (0)
#define LUNA_SHELL_FIX_CURSOR_1 LUNA_SHELL_FIX_CURSOR_DECL
#define LUNA_SHELL_FIX_CURSOR_0 LUNA_SHELL_FIX_CURSOR_CALL
#define LUNA_SHELL_FIX_CURSOR_SELECT(v) LUNA_SHELL_FIX_CAT(LUNA_SHELL_FIX_CURSOR_, v)
#define cursor_theme_tick_and_refresh(arg) \
    LUNA_SHELL_FIX_CURSOR_SELECT(LUNA_SHELL_FIX_IS_VOID(arg))(arg)

#endif /* LUNA_SHELL_INTERACTION_FIXES_H */
