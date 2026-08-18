from pathlib import Path
import glob
import re


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def function_span(text, signature):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"function not found: {signature}")
    brace = text.find('{', start)
    if brace < 0:
        raise SystemExit(f"opening brace not found: {signature}")
    depth = 0
    for i in range(brace, len(text)):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return start, i + 1
    raise SystemExit(f"closing brace not found: {signature}")


def replace_function(text, signature, new_func):
    start, end = function_span(text, signature)
    return text[:start] + new_func + text[end:]


def insert_before_line(text, marker, block, label):
    pos = text.find(marker)
    if pos < 0:
        raise SystemExit(f"{label}: marker not found")
    line = text.rfind('\n', 0, pos) + 1
    return text[:line] + block + text[line:]


def patch_layout(path):
    html = path.read_text()
    changed = False

    # Notification history: keep the legacy text node for third-party skins,
    # but default/bundled layouts get separate time/title/message fields so a
    # short title cannot wrap one word per line.
    for i in range(8):
        old = (f'<div id="notify_{i}" class="cc_stat_row hidden">'
               f'<span class="cc_stat_label" id="notify_{i}_text">—</span></div>')
        if old in html:
            new = (
                f'<div id="notify_{i}" class="notify_card hidden">'
                f'<div class="notify_meta"><span class="notify_when" id="notify_{i}_time">—</span>'
                f'<span class="notify_title" id="notify_{i}_title">Notification</span></div>'
                f'<span class="notify_message" id="notify_{i}_message"></span>'
                f'<span class="cc_stat_label notify_legacy hidden" id="notify_{i}_text">—</span></div>'
            )
            html = html.replace(old, new, 1)
            changed = True

    # Tooltip node for the compact running-window/status icon slots.
    for i in range(8):
        old = (f'<div id="tray_{i}" class="tray_item hidden">'
               '<span class="tray_glyph luna_icon"></span></div>')
        if old in html:
            new = (f'<div id="tray_{i}" class="tray_item hidden">'
                   '<span class="tray_glyph luna_icon"></span>'
                   '<span class="tray_tip"></span></div>')
            html = html.replace(old, new, 1)
            changed = True

    # Email app in Dock and Launchpad. Thunderbird is the default but remains
    # editable from Settings / LUNA_APP_EMAIL just like the other built-ins.
    if 'id="dock_email"' not in html and 'id="dock_editor"' in html:
        marker = '    <div class="dock_item" id="dock_editor"'
        block = (
            '    <div class="dock_item" id="dock_email" role="button" tabindex="0" aria-label="Email">\n'
            '      <div class="dock_icon g_email luna_icon">&#xf0e0;</div>\n'
            '      <div class="dock_dot" id="dot_email"></div>\n'
            '      <span class="dock_label">Email</span>\n'
            '    </div>\n'
        )
        html = insert_before_line(html, marker, block, f'{path}: dock email')
        changed = True

    if 'id="lp_email"' not in html and 'id="lp_editor"' in html:
        marker = '      <div class="lp_app" id="lp_editor"'
        block = (
            '      <div class="lp_app" id="lp_email" role="button" tabindex="0">\n'
            '        <div class="lp_icon g_email luna_icon">&#xf0e0;</div>\n'
            '        <span class="lp_label">Email</span>\n'
            '      </div>\n'
        )
        html = insert_before_line(html, marker, block, f'{path}: launchpad email')
        changed = True

    if 'id="pref_email"' not in html and 'id="pref_browser"' in html and 'id="pref_editor"' in html:
        ep = html.find('id="pref_editor"')
        row = html.rfind('            <div class="pref_row">', 0, ep)
        if row < 0:
            raise SystemExit(f'{path}: editor preference row not found')
        with_toggle = 'id="dock_pref_browser"' in html
        if with_toggle:
            block = (
                '            <div class="pref_row">\n'
                '              <div class="dock_pref on" id="dock_pref_email" role="switch" tabindex="0"><div class="wm_switch"><div class="wm_switch_knob"></div></div></div>\n'
                '              <span class="pref_label">Email</span>\n'
                '              <input type="text" id="pref_email" class="pref_input" value="thunderbird">\n'
                '            </div>\n'
            )
        else:
            block = (
                '            <div class="pref_row">\n'
                '              <span class="pref_label">Email</span>\n'
                '              <input type="text" id="pref_email" class="pref_input" value="thunderbird">\n'
                '            </div>\n'
            )
        html = html[:row] + block + html[row:]
        changed = True

    # Window controls requested by the user.
    if 'id="wm_focus_outline"' not in html and 'id="wm_shortcuts"' in html:
        marker = '            <span class="pref_section">Tiled window gap</span>'
        block = (
            '            <div class="wm_pref" id="wm_focus_outline" role="switch" tabindex="0">\n'
            '              <div class="wm_pref_text"><span class="wm_pref_name">Active window outline</span><span class="wm_pref_desc">Draw the blue focus frame around the active window.</span></div>\n'
            '              <div class="wm_switch"><div class="wm_switch_knob"></div></div>\n'
            '            </div>\n'
            '            <div class="wm_pref" id="wm_window_tray" role="switch" tabindex="0">\n'
            '              <div class="wm_pref_text"><span class="wm_pref_name">Running-app status icons</span><span class="wm_pref_desc">Show the compact icon-only window list in the top-right status area.</span></div>\n'
            '              <div class="wm_switch"><div class="wm_switch_knob"></div></div>\n'
            '            </div>\n'
            '            <div class="wm_pref" id="wm_cascade" role="switch" tabindex="0">\n'
            '              <div class="wm_pref_text"><span class="wm_pref_name">Cascade new windows</span><span class="wm_pref_desc">Offset newly opened windows instead of placing each at the same center position.</span></div>\n'
            '              <div class="wm_switch"><div class="wm_switch_knob"></div></div>\n'
            '            </div>\n\n'
        )
        html = insert_before_line(html, marker, block, f'{path}: window prefs')
        changed = True

    # Split the combined Sound & Display page in bundled skins. The fallback
    # ui/luna-shell.html predates this panel, so it is intentionally skipped.
    if 'id="settings_panel_sound"' in html and 'id="stab_sound"' in html and 'id="stab_display"' not in html:
        sound_tab_line = re.search(r'^(\s*<div class="stab" id="stab_sound"[^\n]*</div>)$', html, re.M)
        if not sound_tab_line:
            raise SystemExit(f'{path}: sound tab line not found')
        sound_line = sound_tab_line.group(1).replace('Sound &amp; Display', 'Sound')
        indent = re.match(r'\s*', sound_line).group(0)
        display_line = (f'{indent}<div class="stab" id="stab_display" role="tab" tabindex="0">'
                        '<span class="stab_icon luna_icon">&#xf108;</span> Display</div>')
        html = html[:sound_tab_line.start()] + sound_line + '\n' + display_line + html[sound_tab_line.end():]

        panel_start = html.find('          <div id="settings_panel_sound"')
        display_start = html.find('            <span class="panel_heading" style="margin-top: 16px;">Display brightness</span>', panel_start)
        wm_comment = html.find('          <!-- Window-management panel', panel_start)
        if panel_start < 0 or display_start < 0 or wm_comment < 0:
            raise SystemExit(f'{path}: combined sound/display panel markers changed')
        sound_close = html.rfind('          </div>', display_start, wm_comment)
        if sound_close < display_start:
            raise SystemExit(f'{path}: sound panel close not found')
        display_content = html[display_start:sound_close].rstrip() + '\n'
        html = html[:display_start] + html[sound_close:]
        # Insert a sibling panel immediately before Window Management.
        wm_comment = html.find('          <!-- Window-management panel', panel_start)
        new_panel = (
            '          <!-- Display backends and application scaling. -->\n'
            '          <div id="settings_panel_display" class="hidden">\n' +
            display_content +
            '          </div>\n\n'
        )
        html = html[:wm_comment] + new_panel + html[wm_comment:]
        changed = True

    if changed:
        path.write_text(html)
    return changed


def patch_style(path):
    css = path.read_text()
    marker = '/* Luna shell UX refinements: notifications, tray tips, Email app. */'
    if marker in css:
        return False
    css += r'''

/* Luna shell UX refinements: notifications, tray tips, Email app. */
.g_email { background: linear-gradient(145deg, #71b7ff, #5268df); }

.notify_card {
  display: flex; flex-direction: column; gap: 3px; min-width: 0;
  padding: 8px 10px; margin: 3px 0; border-radius: 8px;
  background-color: rgba(255, 255, 255, 0.055);
}
.notify_meta { display: flex; flex-direction: row; align-items: center; gap: 8px; min-width: 0; }
.notify_when { flex: 0 0 auto; font-size: 10px; line-height: 14px; opacity: 0.58; white-space: nowrap; }
.notify_title {
  flex: 1 1 auto; min-width: 0; font-size: 12px; font-weight: 700; line-height: 15px;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.notify_message {
  display: block; min-width: 0; font-size: 11px; line-height: 15px; opacity: 0.78;
  white-space: normal; overflow-wrap: anywhere;
}
.notify_legacy { display: none; }

.tray_item { position: relative; }
.tray_tip {
  position: absolute; top: 30px; right: 0; min-width: 72px; max-width: 260px;
  padding: 5px 8px; border-radius: 6px; background-color: rgba(17, 18, 28, 0.94);
  color: rgba(255, 255, 255, 0.96); font-size: 11px; line-height: 14px;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  opacity: 0; pointer-events: none; z-index: 1200;
  transition: opacity 120ms ease-out;
}
.tray_item:hover .tray_tip, .tray_item:focus-visible .tray_tip { opacity: 1; }
'''
    path.write_text(css)
    return True


layouts = [Path('ui/luna-shell.html')] + [Path(p) for p in glob.glob('skins/*/layout.html')]
styles = [Path('ui/luna-shell.css')] + [Path(p) for p in glob.glob('skins/*/style.css')]
layout_changed = [str(p) for p in layouts if p.exists() and patch_layout(p)]
style_changed = [str(p) for p in styles if p.exists() and patch_style(p)]
print('updated layouts:', ', '.join(layout_changed))
print('updated styles:', ', '.join(style_changed))

# ---- luna-shell.c -------------------------------------------------------
path = Path('ui/luna-shell.c')
text = path.read_text()

text = replace_once(
    text,
    '    { .key = "browser",  .name = "Browser",   .env = "LUNA_APP_BROWSER",  .default_cmd = "firefox",              .dock_visible = 1 },\n',
    '    { .key = "browser",  .name = "Browser",   .env = "LUNA_APP_BROWSER",  .default_cmd = "firefox",              .dock_visible = 1 },\n'
    '    { .key = "email",    .name = "Email",     .env = "LUNA_APP_EMAIL",    .default_cmd = "thunderbird",          .dock_visible = 1 },\n',
    'email app')

text = replace_once(
    text,
    '    int  super_shortcuts;\n    int  dock_enabled;',
    '    int  super_shortcuts;\n'
    '    int  active_window_outline; /* draw the focused-window frame */\n'
    '    int  window_tray_icons;     /* compact running-app icons in status area */\n'
    '    int  cascade_new_windows;   /* offset newly mapped windows */\n'
    '    int  dock_enabled;',
    'settings fields')

text = replace_once(
    text,
    '    g_settings.super_shortcuts = 1;\n    g_settings.dock_enabled = 1;',
    '    g_settings.super_shortcuts = 1;\n'
    '    g_settings.active_window_outline = 1;\n'
    '    g_settings.window_tray_icons = 1;\n'
    '    g_settings.cascade_new_windows = 1;\n'
    '    g_settings.dock_enabled = 1;',
    'settings defaults')

text = replace_once(
    text,
    '            else if (!strcmp(key, "super_shortcuts"))\n                g_settings.super_shortcuts = atoi(val) != 0;\n            else if (!strcmp(key, "dock_enabled"))',
    '            else if (!strcmp(key, "super_shortcuts"))\n                g_settings.super_shortcuts = atoi(val) != 0;\n'
    '            else if (!strcmp(key, "active_window_outline"))\n                g_settings.active_window_outline = atoi(val) != 0;\n'
    '            else if (!strcmp(key, "window_tray_icons"))\n                g_settings.window_tray_icons = atoi(val) != 0;\n'
    '            else if (!strcmp(key, "cascade_new_windows"))\n                g_settings.cascade_new_windows = atoi(val) != 0;\n'
    '            else if (!strcmp(key, "dock_enabled"))',
    'settings load')

text = replace_once(
    text,
    '    fprintf(f, "super_shortcuts=%d\\n", g_settings.super_shortcuts);\n    fprintf(f, "dock_enabled=%d\\n", g_settings.dock_enabled);',
    '    fprintf(f, "super_shortcuts=%d\\n", g_settings.super_shortcuts);\n'
    '    fprintf(f, "active_window_outline=%d\\n", g_settings.active_window_outline);\n'
    '    fprintf(f, "window_tray_icons=%d\\n", g_settings.window_tray_icons);\n'
    '    fprintf(f, "cascade_new_windows=%d\\n", g_settings.cascade_new_windows);\n'
    '    fprintf(f, "dock_enabled=%d\\n", g_settings.dock_enabled);',
    'settings save')

# Email app-id aliases, so Dock dots/cycling work with the common clients.
text = replace_once(
    text,
    '    if (str_contains_ci(app_id, key)) return 1;\n',
    '    if (str_contains_ci(app_id, key)) return 1;\n'
    '    if (!strcmp(key, "email"))\n'
    '        return str_contains_ci(app_id, "thunderbird") || str_contains_ci(app_id, "betterbird") ||\n'
    '               str_contains_ci(app_id, "geary") || str_contains_ci(app_id, "evolution");\n',
    'email app-id aliases')

# New settings panel/tab indices.
text = replace_once(
    text,
    'static int g_settings_panel_sound  = -1;',
    'static int g_settings_panel_sound  = -1;\nstatic int g_settings_panel_display = -1;',
    'display panel global')
text = replace_once(
    text,
    'static int g_stab_sound_idx = -1;',
    'static int g_stab_sound_idx = -1;\nstatic int g_stab_display_idx = -1;',
    'display tab global')

# Tray tooltip cache.
text = replace_once(
    text,
    'static int g_tray_glyph_idx[MAX_TRAY_SLOTS];        /* span.tray_glyph child of tray_N */',
    'static int g_tray_glyph_idx[MAX_TRAY_SLOTS];        /* span.tray_glyph child of tray_N */\n'
    'static int g_tray_tip_idx[MAX_TRAY_SLOTS];          /* span.tray_tip child of tray_N */',
    'tray tip cache')
text = replace_once(
    text,
    '    memset(g_tray_glyph_idx, -1, sizeof(g_tray_glyph_idx));',
    '    memset(g_tray_glyph_idx, -1, sizeof(g_tray_glyph_idx));\n'
    '    memset(g_tray_tip_idx,   -1, sizeof(g_tray_tip_idx));',
    'tray tip init')

old_scan = '''        /* tray_glyph children */
        if (strstr(e->class_name, "tray_glyph")) {
            for (int s = 0; s < MAX_TRAY_SLOTS; s++)
                if (g_tray_slot_idx[s] == p) { g_tray_glyph_idx[s] = i; break; }
            continue;
        }
'''
new_scan = old_scan + '''        if (strstr(e->class_name, "tray_tip")) {
            for (int s = 0; s < MAX_TRAY_SLOTS; s++)
                if (g_tray_slot_idx[s] == p) { g_tray_tip_idx[s] = i; break; }
            continue;
        }
'''
text = replace_once(text, old_scan, new_scan, 'tray tip bind')

# Rich notification rendering with legacy fallback for custom skins.
notif_new = r'''static void notification_history_refresh(void) {
    int empty_idx = luna_get_element_by_id("notify_empty");
    if (empty_idx >= 0) set_hidden(empty_idx, g_notification_history_count > 0);
    for (int i = 0; i < NOTIFICATION_HISTORY_MAX; i++) {
        char row_id[24], time_id[32], title_id[32], message_id[32], legacy_id[32];
        snprintf(row_id, sizeof(row_id), "notify_%d", i);
        snprintf(time_id, sizeof(time_id), "notify_%d_time", i);
        snprintf(title_id, sizeof(title_id), "notify_%d_title", i);
        snprintf(message_id, sizeof(message_id), "notify_%d_message", i);
        snprintf(legacy_id, sizeof(legacy_id), "notify_%d_text", i);
        int row = luna_get_element_by_id(row_id);
        if (row < 0) continue;
        if (i >= g_notification_history_count) {
            set_hidden(row, 1);
            continue;
        }
        const LunaNotificationHistoryEntry* h = &g_notification_history[i];
        int time_idx = luna_get_element_by_id(time_id);
        int title_idx = luna_get_element_by_id(title_id);
        int message_idx = luna_get_element_by_id(message_id);
        if (time_idx >= 0 && title_idx >= 0 && message_idx >= 0) {
            luna_set_text(time_idx, h->when[0] ? h->when : "Now");
            luna_set_text(title_idx, h->title[0] ? h->title : "Notification");
            luna_set_text(message_idx, h->message);
            set_hidden(message_idx, h->message[0] ? 0 : 1);
        } else {
            /* Third-party skins may still expose only notify_N_text. */
            int legacy_idx = luna_get_element_by_id(legacy_id);
            if (legacy_idx >= 0) {
                char line[320];
                if (h->message[0])
                    snprintf(line, sizeof(line), "%s  %s — %s", h->when, h->title, h->message);
                else
                    snprintf(line, sizeof(line), "%s  %s", h->when, h->title);
                luna_set_text(legacy_idx, line);
            }
        }
        set_hidden(row, 0);
    }
}'''
text = replace_function(text, 'static void notification_history_refresh(void)', notif_new)

# Add tray window helper before update_tray_ui.
helper = r'''
static int tray_key_is_window(const char* key) {
    return key && (!strncmp(key, "win:", 4) || !strncmp(key, "app:", 4));
}

static LunaTrayEntry* tray_entry_for_key(const char* key) {
    if (!key || !*key) return NULL;
    for (int i = 0; i < g_tray_count; i++)
        if (!strcmp(g_tray[i].id, key)) return &g_tray[i];
    return NULL;
}

static void tray_sync_window_tips_and_visibility(void) {
    for (int s = 0; s < MAX_TRAY_SLOTS; s++) {
        const char* key = g_tray_slot_key[s];
        int is_window = tray_key_is_window(key);
        LunaTrayEntry* t = is_window ? tray_entry_for_key(key) : NULL;
        if (g_tray_tip_idx[s] >= 0) {
            const char* tip = t ? (t->tooltip[0] ? t->tooltip : t->label) : "";
            luna_set_text(g_tray_tip_idx[s], tip);
            set_hidden(g_tray_tip_idx[s], !tip[0]);
        }
        if (!g_settings.window_tray_icons && is_window && g_tray_slot_idx[s] >= 0)
            set_hidden(g_tray_slot_idx[s], 1);
    }
}

'''
anchor = 'static void update_tray_ui(void) {'
pos = text.find(anchor)
if pos < 0:
    raise SystemExit('update_tray_ui anchor missing')
text = text[:pos] + helper + text[pos:]
start, end = function_span(text, 'static void update_tray_ui(void)')
func = text[start:end]
if 'tray_sync_window_tips_and_visibility();' not in func:
    close = func.rfind('}')
    func = func[:close] + '    tray_sync_window_tips_and_visibility();\n' + func[close:]
    text = text[:start] + func + text[end:]

# Right-click compact running-app icons opens the same window menu as the
# normal list. Left-click remains activate. Slot->entry is resolved by key so
# StatusNotifier items preceding it cannot skew indexes.
tray_click = r'''static void on_tray_click(LunaElement* e) {
    for (int idx = elem_idx_of(e); idx != -1; idx = luna_element_at(idx)->parent_idx) {
        const char* id = luna_element_at(idx)->id;
        if (id[0] == 't' && id[1] == 'r' && id[2] == 'a' && id[3] == 'y' && id[4] == '_') {
            int slot = atoi(id + 5);
            if (slot < 0 || slot >= MAX_TRAY_SLOTS) return;
            LunaElement* slot_el = luna_element_at(idx);
            int ax = slot_el ? (int)slot_el->x : 0;
            int ay = slot_el ? (int)slot_el->y : 0;
            if (g_tray_sni_kind[slot]) {
                luna_sni_request_activate(g_tray_sni_service[slot],
                                          g_tray_sni_path[slot], ax, ay);
                return;
            }

            const char* key = g_tray_slot_key[slot];
            LunaTrayEntry* t = tray_entry_for_key(key);
            uint64_t sid = tray_lookup_surface(key);
            if (sid) {
                LunaWinEntry* w = NULL;
                for (int i = 0; i < g_win_count; i++) {
                    if (g_wins[i].id == sid) { w = &g_wins[i]; break; }
                }
                if (luna_last_click_button() == LUNA_MOUSE_BUTTON_RIGHT) {
                    win_menu_open(sid, idx,
                                  w ? w->title :
                                  (t ? (t->tooltip[0] ? t->tooltip : t->label) : NULL));
                    return;
                }
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "activate %" PRIu64, sid);
                shell_send_cmd(cmd);
                return;
            }
            if (t && !strncmp(t->id, "service:", 8)) {
                tray_send_action(t->id, "activate");
                if (!strcmp(t->id, "service:luna-wifi"))
                    on_wifi_menu(e);
                return;
            }
            if (t && t->tooltip[0])
                toast_show(t->label, t->tooltip, 3.0);
            return;
        }
    }
}'''
text = replace_function(text, 'static void on_tray_click(LunaElement* e)', tray_click)

# Settings UI state.
text = replace_once(
    text,
    '    settings_mark_toggle("wm_shortcuts", g_settings.super_shortcuts);\n',
    '    settings_mark_toggle("wm_shortcuts", g_settings.super_shortcuts);\n'
    '    settings_mark_toggle("wm_focus_outline", g_settings.active_window_outline);\n'
    '    settings_mark_toggle("wm_window_tray", g_settings.window_tray_icons);\n'
    '    settings_mark_toggle("wm_cascade", g_settings.cascade_new_windows);\n',
    'settings mark new wm toggles')
text = replace_once(
    text,
    '    set_hidden(g_settings_panel_sound, 1);\n    set_hidden(g_settings_panel_wm, 1);',
    '    set_hidden(g_settings_panel_sound, 1);\n'
    '    set_hidden(g_settings_panel_display, 1);\n'
    '    set_hidden(g_settings_panel_wm, 1);',
    'settings hide display panel')
text = replace_once(
    text,
    '    if (g_stab_sound_idx >= 0) luna_update_classes(g_stab_sound_idx, "active", NULL);\n    if (g_stab_wm_idx',
    '    if (g_stab_sound_idx >= 0) luna_update_classes(g_stab_sound_idx, "active", NULL);\n'
    '    if (g_stab_display_idx >= 0) luna_update_classes(g_stab_display_idx, "active", NULL);\n'
    '    if (g_stab_wm_idx',
    'settings deactivate display tab')

text = replace_once(
    text,
    '    int is_sound = !strcmp(id, "stab_sound");\n    int is_wm = !strcmp(id, "stab_wm");',
    '    int is_sound = !strcmp(id, "stab_sound");\n'
    '    int is_display = !strcmp(id, "stab_display");\n'
    '    int is_wm = !strcmp(id, "stab_wm");',
    'settings tab display bool')
text = replace_once(
    text,
    '    if (g_stab_sound_idx >= 0)\n        luna_update_classes(g_stab_sound_idx, "active", is_sound ? "active" : NULL);\n    if (g_stab_wm_idx',
    '    if (g_stab_sound_idx >= 0)\n        luna_update_classes(g_stab_sound_idx, "active", is_sound ? "active" : NULL);\n'
    '    if (g_stab_display_idx >= 0)\n        luna_update_classes(g_stab_display_idx, "active", is_display ? "active" : NULL);\n'
    '    if (g_stab_wm_idx',
    'settings tab display class')
text = replace_once(
    text,
    '    set_hidden(g_settings_panel_sound, !is_sound);\n    set_hidden(g_settings_panel_wm, !is_wm);\n    if (is_sound) settings_update_sound_status();',
    '    set_hidden(g_settings_panel_sound, !is_sound);\n'
    '    set_hidden(g_settings_panel_display, !is_display);\n'
    '    set_hidden(g_settings_panel_wm, !is_wm);\n'
    '    if (is_sound || is_display) settings_update_sound_status();',
    'settings tab display panel')

# New wm toggles in the handler.
start, end = function_span(text, 'static void on_wm_toggle(LunaElement* e)')
func = text[start:end]
if 'wm_focus_outline' not in func:
    anchor = '    else if (!strcmp(id, "wm_restore")) value = &g_settings.session_restore;'
    if anchor not in func:
        raise SystemExit('on_wm_toggle wm_restore anchor missing')
    func = func.replace(anchor, anchor + '\n'
        '    else if (!strcmp(id, "wm_focus_outline")) value = &g_settings.active_window_outline;\n'
        '    else if (!strcmp(id, "wm_window_tray")) value = &g_settings.window_tray_icons;\n'
        '    else if (!strcmp(id, "wm_cascade")) value = &g_settings.cascade_new_windows;', 1)
    save_anchor = '    settings_save();'
    if save_anchor not in func:
        raise SystemExit('on_wm_toggle settings_save anchor missing')
    func = func.replace(save_anchor,
        '    if (!strcmp(id, "wm_window_tray")) update_tray_ui();\n'
        '    settings_save();', 1)
    text = text[:start] + func + text[end:]

# Send compositor runtime prefs with the existing WM batch.
start, end = function_span(text, 'static void apply_wm_settings(void)')
func = text[start:end]
if 'wm_config focus_outline' not in func:
    insert = (
        '    snprintf(cmd, sizeof(cmd), "wm_config focus_outline %d", g_settings.active_window_outline);\n'
        '    ok &= shell_send_cmd(cmd);\n'
        '    snprintf(cmd, sizeof(cmd), "wm_config cascade_windows %d", g_settings.cascade_new_windows);\n'
        '    ok &= shell_send_cmd(cmd);\n'
    )
    close = func.rfind('}')
    # Put it before the final retry bookkeeping block if present, otherwise
    # before function end; either way it participates in ok.
    anchor = '    if (!ok)'
    apos = func.find(anchor)
    if apos >= 0:
        func = func[:apos] + insert + func[apos:]
    else:
        func = func[:close] + insert + func[close:]
    text = text[:start] + func + text[end:]

# Resolve/bind new settings panel + tab.
text = replace_once(
    text,
    '    g_settings_panel_sound = luna_get_element_by_id("settings_panel_sound");',
    '    g_settings_panel_sound = luna_get_element_by_id("settings_panel_sound");\n'
    '    g_settings_panel_display = luna_get_element_by_id("settings_panel_display");',
    'bind display panel')
text = replace_once(
    text,
    '    g_stab_sound_idx = luna_get_element_by_id("stab_sound");',
    '    g_stab_sound_idx = luna_get_element_by_id("stab_sound");\n'
    '    g_stab_display_idx = luna_get_element_by_id("stab_display");',
    'bind display tab')
text = replace_once(
    text,
    '    wire_subtree(g_stab_sound_idx, on_settings_tab);',
    '    wire_subtree(g_stab_sound_idx, on_settings_tab);\n'
    '    wire_subtree(g_stab_display_idx, on_settings_tab);',
    'wire display tab')

# Existing wm id list gets the three new switches.
old_ids = '            "wm_dock", "wm_widgets", "wm_dock_mag", "wm_wallpaper_motion", "wm_restore"'
if old_ids not in text:
    raise SystemExit('wm toggle id list changed')
text = text.replace(old_ids,
    '            "wm_dock", "wm_widgets", "wm_dock_mag", "wm_wallpaper_motion", "wm_restore",\n'
    '            "wm_focus_outline", "wm_window_tray", "wm_cascade"', 1)

path.write_text(text)
print('updated:', path)

# ---- Wayland compositor runtime preferences -----------------------------
path = Path('wayland-server-rs/src/server.rs')
text = path.read_text()
text = replace_once(
    text,
    '  wm_titlebar_double_click: bool,\n',
    '  wm_titlebar_double_click: bool,\n'
    '  /// Draw the focused/active color around the outside resize frame.\n'
    '  wm_focus_outline: bool,\n'
    '  /// Offset newly mapped windows by the traditional cascade amount.\n'
    '  wm_cascade_windows: bool,\n',
    'server wm fields')
text = replace_once(
    text,
    '      wm_titlebar_double_click: true,\n',
    '      wm_titlebar_double_click: true,\n'
    '      wm_focus_outline: true,\n'
    '      wm_cascade_windows: true,\n',
    'server wm defaults')

# Keep resize borders present, but gate the blue/active color treatment.
start, end = function_span(text, 'fn draw_window_frame(&mut self')
func = text[start:end]
if 'frame_focused' not in func:
    func = replace_once(func,
        '    const FRAME: i32 = 3;\n',
        '    const FRAME: i32 = 3;\n    let frame_focused = focused && self.wm_focus_outline;\n',
        'frame focus gate')
    func = func.replace('if focused {', 'if frame_focused {')
    func = func.replace('else if focused {', 'else if frame_focused {')
    text = text[:start] + func + text[end:]

cascade_old = '        let cascade = (self.window_stack.len().saturating_sub(1) as i32) * 28;'
count = text.count(cascade_old)
if count != 2:
    raise SystemExit(f'cascade placement: expected 2 matches, found {count}')
text = text.replace(cascade_old,
    '        let cascade = if self.wm_cascade_windows {\n'
    '          (self.window_stack.len().saturating_sub(1) as i32) * 28\n'
    '        } else {\n'
    '          0\n'
    '        };')

wm_match = '          match key {\n'
pos = text.find(wm_match, text.find('(Some("wm_config"), Some(key)) => {'))
if pos < 0:
    raise SystemExit('wm_config match not found')
insert_pos = pos + len(wm_match)
wm_cases = '''            "focus_outline" => {
              self.wm_focus_outline = parts.next().and_then(|s| s.parse::<i32>().ok()).unwrap_or(1) != 0;
              self.dirty = true;
            }
            "cascade_windows" => {
              self.wm_cascade_windows = parts.next().and_then(|s| s.parse::<i32>().ok()).unwrap_or(1) != 0;
            }
'''
text = text[:insert_pos] + wm_cases + text[insert_pos:]
path.write_text(text)
print('updated:', path)

# Sanity checks before the workflow spends time compiling.
c = Path('ui/luna-shell.c').read_text()
r = Path('wayland-server-rs/src/server.rs').read_text()
if c.count('.key = "email"') != 1:
    raise SystemExit('Email app sanity check failed')
for token in ('active_window_outline', 'window_tray_icons', 'cascade_new_windows',
              'tray_sync_window_tips_and_visibility', 'g_settings_panel_display'):
    if token not in c:
        raise SystemExit(f'missing C token: {token}')
for token in ('wm_focus_outline', 'wm_cascade_windows', '"focus_outline"', '"cascade_windows"'):
    if token not in r:
        raise SystemExit(f'missing server token: {token}')
if not any('id="settings_panel_display"' in p.read_text() for p in layouts if p.exists()):
    raise SystemExit('Display settings panel was not generated')
print('patch sanity checks passed')
