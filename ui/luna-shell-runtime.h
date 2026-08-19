#ifndef LUNA_SHELL_RUNTIME_H
#define LUNA_SHELL_RUNTIME_H

#include "luna-shell-widgets.h"

/* Right-button press capture. Release-time on_click can be lost when the
 * Wayland menubar changes pointer focus while opening an overlay. */
static unsigned long long g_luna_shell_rt_css_seen;
static int g_luna_shell_rt_pending_right;
static double g_luna_shell_rt_right_x;
static double g_luna_shell_rt_right_y;

static inline void luna_shell_rt_note_mouse(int button, int action, int mods,
                                             double x, double y) {
    (void)mods;
    if (button == 1 && action == 1) {
        g_luna_shell_rt_pending_right = 1;
        g_luna_shell_rt_right_x = x;
        g_luna_shell_rt_right_y = y;
    }
}

#define LUNA_SHELL_RT_MOUSE_DECL(button,action,mods,x,y) \
    luna_mouse_button(button,action,mods,x,y)
#define LUNA_SHELL_RT_MOUSE_CALL(button,action,mods,x,y) \
    ((luna_mouse_button)((button),(action),(mods),(x),(y)), \
     luna_shell_rt_note_mouse((button),(action),(mods),(x),(y)))
#define LUNA_SHELL_RT_MOUSE_SEL(v) LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_MOUSE_,v)
#define LUNA_SHELL_RT_MOUSE_1 LUNA_SHELL_RT_MOUSE_DECL
#define LUNA_SHELL_RT_MOUSE_0 LUNA_SHELL_RT_MOUSE_CALL
#define luna_mouse_button(button,action,mods,x,y) \
    LUNA_SHELL_RT_MOUSE_SEL(LUNA_SHELL_RT_IS_INT(button))(button,action,mods,x,y)

#define LUNA_SHELL_RT_MAINTENANCE() do { \
    /* Widgets are shell-owned: re-append their stylesheet after a skin CSS reload. */ \
    if (g_luna_shell_rt_css_seen != (unsigned long long)luna_css_generation()) { \
        (luna_parse_css)(g_luna_shell_widgets_css); \
        g_luna_shell_rt_css_seen = (unsigned long long)luna_css_generation(); \
    } \
    /* Window tray tooltips always come from the live compositor window title. */ \
    for (int _lsr_s = 0; _lsr_s < MAX_TRAY_SLOTS; _lsr_s++) { \
        if (!tray_key_is_window(g_tray_slot_key[_lsr_s])) continue; \
        uint64_t _lsr_sid = tray_lookup_surface(g_tray_slot_key[_lsr_s]); \
        const char* _lsr_tip = NULL; \
        for (int _lsr_w = 0; _lsr_w < g_win_count; _lsr_w++) \
            if (g_wins[_lsr_w].id == _lsr_sid && g_wins[_lsr_w].title[0]) { \
                _lsr_tip = g_wins[_lsr_w].title; break; \
            } \
        if (!_lsr_tip) { \
            LunaTrayEntry* _lsr_t = tray_entry_for_key(g_tray_slot_key[_lsr_s]); \
            if (_lsr_t) \
                _lsr_tip = _lsr_t->tooltip[0] ? _lsr_t->tooltip : _lsr_t->label; \
        } \
        if (_lsr_tip && _lsr_tip[0] && g_tray_tip_idx[_lsr_s] >= 0) { \
            luna_set_text(g_tray_tip_idx[_lsr_s], _lsr_tip); \
            set_hidden(g_tray_tip_idx[_lsr_s], 0); \
        } \
    } \
    if (tray_sync_hover_tip()) shell_request_repaint(1); \
    /* Dispatch window-menu right-click on the press and consume its release. */ \
    if (g_luna_shell_rt_pending_right) { \
        g_luna_shell_rt_pending_right = 0; \
        int _lsr_hit = hit_test_at(g_luna_shell_rt_right_x, \
                                   g_luna_shell_rt_right_y); \
        for (int _lsr_i = _lsr_hit; _lsr_i != -1; ) { \
            LunaElement* _lsr_e = luna_element_at(_lsr_i); \
            if (!_lsr_e) break; \
            const char* _lsr_id = _lsr_e->id; \
            int _lsr_handled = 0; \
            if (_lsr_id && !strncmp(_lsr_id, "tray_", 5) && \
                _lsr_id[5] >= '0' && _lsr_id[5] <= '9') { \
                int _lsr_slot = atoi(_lsr_id + 5); \
                if (_lsr_slot >= 0 && _lsr_slot < MAX_TRAY_SLOTS && \
                    tray_key_is_window(g_tray_slot_key[_lsr_slot])) { \
                    on_tray_click(_lsr_e); \
                    _lsr_handled = 1; \
                } \
            } else if (_lsr_id && !strncmp(_lsr_id, "win_", 4) && \
                       _lsr_id[4] >= '0' && _lsr_id[4] <= '9') { \
                on_win_click(_lsr_e); \
                _lsr_handled = 1; \
            } \
            if (_lsr_handled) { \
                luna_consume_pointer_event(); \
                shell_request_repaint(-1); \
                break; \
            } \
            _lsr_i = _lsr_e->parent_idx; \
        } \
    } \
    /* fit-content computes the DOM width. Copy it to the Wayland layer only \
     * when it changes; between Dock settings changes the surface stays fixed. */ \
    if (g_backend == &g_wl_backend && g_surfs[LUNA_SURF_DOCK].layer_surf) { \
        int _lsr_di = luna_get_element_by_id("dock"); \
        LunaElement* _lsr_de = _lsr_di >= 0 ? luna_element_at(_lsr_di) : NULL; \
        if (_lsr_de && is_shown(_lsr_di) && _lsr_de->w > 0.0f) { \
            int _lsr_dw = (int)ceilf(_lsr_de->w); \
            int _lsr_max = (int)luna_window_width - 24; \
            if (_lsr_dw < 96) _lsr_dw = 96; \
            if (_lsr_dw > _lsr_max) _lsr_dw = _lsr_max; \
            if (_lsr_dw != g_surfs[LUNA_SURF_DOCK].fixed_w) { \
                g_surfs[LUNA_SURF_DOCK].fixed_w = _lsr_dw; \
                surf_reconfigure_chrome(&g_surfs[LUNA_SURF_DOCK]); \
                shell_request_repaint(2); \
                if (g_wl.display) wl_display_flush(g_wl.display); \
            } \
        } \
    } \
} while (0)

#define LUNA_SHELL_RT_CURSOR_DECL(arg) cursor_theme_tick_and_refresh(arg)
#define LUNA_SHELL_RT_CURSOR_CALL(arg) do { \
    cursor_theme_tick_and_refresh(); \
    LUNA_SHELL_RT_MAINTENANCE(); \
} while (0)
#define LUNA_SHELL_RT_CURSOR_SEL(v) LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_CURSOR_,v)
#define LUNA_SHELL_RT_CURSOR_1 LUNA_SHELL_RT_CURSOR_DECL
#define LUNA_SHELL_RT_CURSOR_0 LUNA_SHELL_RT_CURSOR_CALL
#define cursor_theme_tick_and_refresh(arg) \
    LUNA_SHELL_RT_CURSOR_SEL(LUNA_SHELL_RT_IS_VOID(arg))(arg)

#endif /* LUNA_SHELL_RUNTIME_H */