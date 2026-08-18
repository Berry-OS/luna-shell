#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one replacement target, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


layout_old = '''  <!-- Desktop Widgets -->
  <div id="widget_clock" class="widget">
    <div id="wg_time">23:00</div>
    <div id="wg_date">Tuesday, July 28</div>
  </div>
'''
layout_new = '''  <!-- Desktop Widgets -->
  <div id="widget_clock" class="widget">
    <div id="wg_clock_view">
      <div id="wg_stopwatch_open" class="wg_clock_icon luna_icon" role="button" tabindex="0" aria-label="Open stopwatch">&#xf2f2;</div>
      <div id="wg_time">23:00</div>
      <div id="wg_date">Tuesday, July 28</div>
    </div>
    <div id="wg_stopwatch_view" class="hidden">
      <div id="wg_stopwatch_head">
        <div id="wg_stopwatch_back" class="wg_clock_icon luna_icon" role="button" tabindex="0" aria-label="Back to clock">&#xf060;</div>
        <span id="wg_stopwatch_title">Stopwatch</span>
        <span id="wg_stopwatch_state">Ready</span>
      </div>
      <div id="wg_stopwatch_time">00:00.0</div>
      <div id="wg_stopwatch_actions">
        <div id="wg_stopwatch_toggle" class="dlg_btn primary" role="button" tabindex="0">Start</div>
        <div id="wg_stopwatch_reset" class="dlg_btn" role="button" tabindex="0">Reset</div>
      </div>
    </div>
  </div>
'''
replace_once("skins/default/layout.html", layout_old, layout_new)

css_old = '''#widget_clock { right: 24px; top: 52px; width: 220px; height: 110px; }
#wg_time {
  font-family: var(--display-font);
  color: #ffffff;
  font-size: 46px; font-weight: 800; letter-spacing: -0.02em;
  line-height: 1;
}
#wg_date { color: var(--luna-text-dim); font-size: 12px; font-weight: 500; margin-top: 6px; }
'''
css_new = '''#widget_clock {
  right: 24px; top: 52px; width: 220px; height: 110px;
  transition: height var(--dur-short) var(--ease-out), padding var(--dur-short) var(--ease-out);
}
#widget_clock.stopwatch_mode { height: 160px; padding: 14px 16px; }
#wg_clock_view { position: relative; width: 100%; height: 100%; }
#wg_time {
  font-family: var(--display-font);
  color: #ffffff;
  font-size: 46px; font-weight: 800; letter-spacing: -0.02em;
  line-height: 1; padding-right: 28px;
}
#wg_date { color: var(--luna-text-dim); font-size: 12px; font-weight: 500; margin-top: 6px; }
.wg_clock_icon {
  display: flex; align-items: center; justify-content: center;
  width: 24px; height: 24px; border-radius: 8px;
  color: var(--luna-text-dim); font-size: 12px; cursor: pointer;
  background: rgba(255, 255, 255, 0.06);
  border: 1px solid rgba(255, 255, 255, 0.08);
  transition: background-color var(--dur-fast) var(--ease-out), color var(--dur-fast) var(--ease-out);
}
.wg_clock_icon:hover { background: rgba(255, 255, 255, 0.14); color: #ffffff; }
.wg_clock_icon:focus-visible { outline: 2px solid var(--luna-accent-2); outline-offset: 2px; }
#wg_stopwatch_open { position: absolute; right: 0; top: 0; }
#wg_stopwatch_view { width: 100%; height: 100%; }
#wg_stopwatch_head {
  display: flex; flex-direction: row; align-items: center; gap: 8px;
  width: 100%; height: 24px;
}
#wg_stopwatch_title {
  flex: 1 1 auto; color: var(--luna-text); font-size: 12px; font-weight: 700;
}
#wg_stopwatch_state { color: var(--luna-text-muted); font-size: 10px; }
#wg_stopwatch_time {
  width: 100%; margin-top: 8px;
  font-family: var(--display-font); font-variant-numeric: tabular-nums;
  color: #ffffff; font-size: 34px; font-weight: 800; letter-spacing: -0.02em;
  line-height: 1; text-align: center;
}
#wg_stopwatch_actions {
  display: flex; flex-direction: row; gap: 8px;
  width: 100%; margin-top: 12px;
}
#wg_stopwatch_actions .dlg_btn { flex: 1 1 0; min-width: 0; height: 30px; font-size: 12px; }
#widget_stats.clock_below_stopwatch { top: 226px; }
#widget_weather.clock_below_stopwatch { top: 398px; }
'''
replace_once("skins/default/style.css", css_old, css_new)

replace_once(
    "ui/luna-shell.c",
    "static double g_now = 0.0;\n",
    '''static double g_now = 0.0;
/* Stopwatch state uses the shell's monotonic clock.  The elapsed value is
 * accumulated only when paused; while running the displayed value is derived
 * from g_now so hiding the stopwatch never loses time or needs background
 * repaint work. */
static int g_stopwatch_mode = 0;
static int g_stopwatch_running = 0;
static double g_stopwatch_started_at = 0.0;
static double g_stopwatch_elapsed = 0.0;
static double g_stopwatch_next_deadline = 0.0;
''',
)

replace_once(
    "ui/luna-shell.c",
    '''    UI_WG_TIME,
    UI_WG_DATE,
    UI_ST_CPU_VAL,
''',
    '''    UI_WG_TIME,
    UI_WG_DATE,
    UI_WG_CLOCK_VIEW,
    UI_WG_STOPWATCH_VIEW,
    UI_WG_STOPWATCH_TIME,
    UI_WG_STOPWATCH_TOGGLE,
    UI_WG_STOPWATCH_STATE,
    UI_WIDGET_CLOCK,
    UI_WIDGET_STATS,
    UI_WIDGET_WEATHER,
    UI_ST_CPU_VAL,
''',
)

replace_once(
    "ui/luna-shell.c",
    '''    "wg_time", "wg_date", "st_cpu_val", "st_cpu_fill",
''',
    '''    "wg_time", "wg_date", "wg_clock_view", "wg_stopwatch_view",
    "wg_stopwatch_time", "wg_stopwatch_toggle", "wg_stopwatch_state",
    "widget_clock", "widget_stats", "widget_weather",
    "st_cpu_val", "st_cpu_fill",
''',
)

stopwatch_functions = r'''static double stopwatch_elapsed_now(void) {
    double elapsed = g_stopwatch_elapsed;
    if (g_stopwatch_running) {
        double delta = g_now - g_stopwatch_started_at;
        if (delta > 0.0) elapsed += delta;
    }
    return elapsed > 0.0 ? elapsed : 0.0;
}

static void stopwatch_format_time(char* out, size_t n, double elapsed) {
    unsigned long long tenths = (unsigned long long)floor(elapsed * 10.0 + 0.000001);
    unsigned long long hours = tenths / 36000ULL;
    unsigned long long minutes = (tenths / 600ULL) % 60ULL;
    unsigned long long seconds = (tenths / 10ULL) % 60ULL;
    unsigned long long tenth = tenths % 10ULL;
    if (hours > 0ULL)
        snprintf(out, n, "%llu:%02llu:%02llu.%llu", hours, minutes, seconds, tenth);
    else
        snprintf(out, n, "%02llu:%02llu.%llu", minutes, seconds, tenth);
}

static void stopwatch_set_text_if_changed(int idx, const char* text, int paint_only) {
    if (idx < 0 || !text) return;
    LunaElement* el = luna_element_at(idx);
    if (!el || strcmp(el->text, text) == 0) return;
    if (paint_only) luna_set_text_paint_only(idx, text);
    else luna_set_text(idx, text);
}

static void stopwatch_render(void) {
    char buf[32];
    double elapsed = stopwatch_elapsed_now();
    stopwatch_format_time(buf, sizeof(buf), elapsed);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_TIME], buf, 1);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_TOGGLE],
                                  g_stopwatch_running ? "Pause" :
                                  (elapsed > 0.0 ? "Resume" : "Start"), 0);
    stopwatch_set_text_if_changed(g_ui_idx[UI_WG_STOPWATCH_STATE],
                                  g_stopwatch_running ? "Running" :
                                  (elapsed > 0.0 ? "Paused" : "Ready"), 0);
    if (g_stopwatch_mode) shell_request_repaint(0);
}

static void stopwatch_set_mode(int enabled) {
    enabled = enabled != 0;
    g_stopwatch_mode = enabled;
    set_hidden(g_ui_idx[UI_WG_CLOCK_VIEW], enabled);
    set_hidden(g_ui_idx[UI_WG_STOPWATCH_VIEW], !enabled);
    luna_update_classes(g_ui_idx[UI_WIDGET_CLOCK],
                        enabled ? "stopwatch_mode" : NULL,
                        enabled ? NULL : "stopwatch_mode");
    luna_update_classes(g_ui_idx[UI_WIDGET_STATS],
                        enabled ? "clock_below_stopwatch" : NULL,
                        enabled ? NULL : "clock_below_stopwatch");
    luna_update_classes(g_ui_idx[UI_WIDGET_WEATHER],
                        enabled ? "clock_below_stopwatch" : NULL,
                        enabled ? NULL : "clock_below_stopwatch");
    g_stopwatch_next_deadline = (enabled && g_stopwatch_running) ? g_now : 0.0;
    if (enabled) stopwatch_render();
    luna_mark_layout_dirty();
    shell_request_repaint(0);
}

static void on_stopwatch_open(LunaElement* e) {
    (void)e;
    dismiss_calendar_menu();
    stopwatch_set_mode(1);
}

static void on_stopwatch_back(LunaElement* e) {
    (void)e;
    stopwatch_set_mode(0);
}

static void on_stopwatch_toggle(LunaElement* e) {
    (void)e;
    if (g_stopwatch_running) {
        double delta = g_now - g_stopwatch_started_at;
        if (delta > 0.0) g_stopwatch_elapsed += delta;
        g_stopwatch_running = 0;
        g_stopwatch_next_deadline = 0.0;
    } else {
        g_stopwatch_started_at = g_now;
        g_stopwatch_running = 1;
        g_stopwatch_next_deadline = g_now;
    }
    stopwatch_render();
}

static void on_stopwatch_reset(LunaElement* e) {
    (void)e;
    g_stopwatch_elapsed = 0.0;
    if (g_stopwatch_running) {
        g_stopwatch_started_at = g_now;
        g_stopwatch_next_deadline = g_now;
    }
    stopwatch_render();
}

static void stopwatch_tick(void) {
    if (!g_stopwatch_running || !g_stopwatch_mode) return;
    if (g_stopwatch_next_deadline > 0.0 && g_now < g_stopwatch_next_deadline) return;
    stopwatch_render();
    g_stopwatch_next_deadline = g_now + 0.1;
}

'''
replace_once(
    "ui/luna-shell.c",
    "static void on_calendar_menu(LunaElement* e) {\n",
    stopwatch_functions + "static void on_calendar_menu(LunaElement* e) {\n",
)

replace_once(
    "ui/luna-shell.c",
    '    wire_subtree(luna_get_element_by_id("widget_clock"),  on_calendar_menu);\n',
    '''    wire_subtree(luna_get_element_by_id("wg_time"),       on_calendar_menu);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_open"), on_stopwatch_open);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_back"), on_stopwatch_back);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_toggle"), on_stopwatch_toggle);
    wire_subtree(luna_get_element_by_id("wg_stopwatch_reset"), on_stopwatch_reset);
    stopwatch_set_mode(g_stopwatch_mode);
    stopwatch_render();
''',
)

replace_once(
    "ui/luna-shell.c",
    '''            alarm_tick();
            update_launchpad_filter();
''',
    '''            alarm_tick();
            stopwatch_tick();
            update_launchpad_filter();
''',
)

replace_once(
    "ui/luna-shell.c",
    '''        SOONER(g_alarm_next_deadline);
        SOONER(g_session_restore_at);
''',
    '''        SOONER(g_alarm_next_deadline);
        if (g_stopwatch_running && g_stopwatch_mode)
            SOONER(g_stopwatch_next_deadline);
        SOONER(g_session_restore_at);
''',
)

print("Stopwatch widget sources updated successfully.")
