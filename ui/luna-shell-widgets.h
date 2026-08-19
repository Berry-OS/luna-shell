#ifndef LUNA_SHELL_WIDGETS_H
#define LUNA_SHELL_WIDGETS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Shell-owned desktop widgets. Skins may style desktop chrome, but they do
 * not own or replace these controls. Legacy copies found in a skin layout
 * are stripped at load time and this fragment is injected into #bg_layer. */
static const char g_luna_shell_widgets_html[] =
"\n    <!-- Luna Shell common widgets (skin-independent) -->\n"
"    <div id=\"widget_clock\" class=\"widget\">\n"
"      <div id=\"wg_clock_view\">\n"
"        <div id=\"wg_stopwatch_open\" class=\"wg_clock_icon luna_icon\" role=\"button\" tabindex=\"0\" aria-label=\"Open stopwatch\">&#xf2f2;</div>\n"
"        <div id=\"wg_time\">23:00</div>\n"
"        <div id=\"wg_date\">Tuesday, July 28</div>\n"
"      </div>\n"
"      <div id=\"wg_stopwatch_view\" class=\"hidden\">\n"
"        <div id=\"wg_stopwatch_head\">\n"
"          <div id=\"wg_stopwatch_back\" class=\"wg_clock_icon luna_icon\" role=\"button\" tabindex=\"0\" aria-label=\"Back to clock\">&#xf060;</div>\n"
"          <span id=\"wg_stopwatch_title\">Stopwatch</span>\n"
"          <span id=\"wg_stopwatch_state\">Ready</span>\n"
"        </div>\n"
"        <div id=\"wg_stopwatch_time\">00:00.0</div>\n"
"        <div id=\"wg_stopwatch_actions\">\n"
"          <div id=\"wg_stopwatch_toggle\" class=\"dlg_btn primary\" role=\"button\" tabindex=\"0\">Start</div>\n"
"          <div id=\"wg_stopwatch_reset\" class=\"dlg_btn\" role=\"button\" tabindex=\"0\">Reset</div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div id=\"widget_stats\" class=\"widget\">\n"
"      <div class=\"st_row\"><span class=\"st_label\">CPU Usage</span><span class=\"st_val\" id=\"st_cpu_val\">0%</span></div>\n"
"      <div class=\"st_bar\"><div class=\"st_fill\" id=\"st_cpu_fill\" style=\"width: 0%;\"></div></div>\n"
"      <div class=\"st_row\" style=\"margin-top: 12px;\"><span class=\"st_label\">Memory</span><span class=\"st_val\" id=\"st_mem_val\">—</span></div>\n"
"      <div class=\"st_bar\"><div class=\"st_fill g_mem\" id=\"st_mem_fill\" style=\"width: 0%;\"></div></div>\n"
"      <div class=\"st_row\" style=\"margin-top: 12px;\"><span class=\"st_label\">Storage</span><span class=\"st_val\" id=\"st_disk_val\">—</span></div>\n"
"      <div class=\"st_bar\"><div class=\"st_fill g_disk\" id=\"st_disk_fill\" style=\"width: 0%;\"></div></div>\n"
"    </div>\n"
"    <div id=\"widget_weather\" class=\"widget\">\n"
"      <div class=\"st_row\"><span class=\"st_label\" id=\"wg_wx_city\">Weather</span><span class=\"st_val\" id=\"wg_wx_temp\">—</span></div>\n"
"      <div id=\"wg_wx_main\"><span class=\"luna_icon\" id=\"wg_wx_icon\">&#xf2c9;</span><span id=\"wg_wx_desc\">Fetching…</span></div>\n"
"      <div id=\"wg_wx_forecast\">\n"
"        <div id=\"wg_wf_0\" class=\"wf_day\"><div class=\"wf_name\">—</div><div class=\"wf_icon luna_icon\">&#xf2c9;</div><div class=\"wf_temp\">—</div></div>\n"
"        <div id=\"wg_wf_1\" class=\"wf_day\"><div class=\"wf_name\">—</div><div class=\"wf_icon luna_icon\">&#xf2c9;</div><div class=\"wf_temp\">—</div></div>\n"
"        <div id=\"wg_wf_2\" class=\"wf_day\"><div class=\"wf_name\">—</div><div class=\"wf_icon luna_icon\">&#xf2c9;</div><div class=\"wf_temp\">—</div></div>\n"
"        <div id=\"wg_wf_3\" class=\"wf_day\"><div class=\"wf_name\">—</div><div class=\"wf_icon luna_icon\">&#xf2c9;</div><div class=\"wf_temp\">—</div></div>\n"
"        <div id=\"wg_wf_4\" class=\"wf_day\"><div class=\"wf_name\">—</div><div class=\"wf_icon luna_icon\">&#xf2c9;</div><div class=\"wf_temp\">—</div></div>\n"
"      </div>\n"
"    </div>\n";

/* These rules intentionally target only the three shell widget roots. Skin
 * styles continue to own menus, dialogs and other .wf_* / .st_* classes. */
static const char g_luna_shell_widgets_css[] =
"#widget_clock,#widget_stats,#widget_weather{position:absolute;background:linear-gradient(135deg,rgba(18,19,42,.52) 0%,rgba(10,11,26,.65) 100%);backdrop-filter:blur(24px) saturate(1.6);border:1px solid rgba(255,255,255,.12);border-radius:22px;box-shadow:0 10px 28px rgba(0,0,0,.22),inset 0 1px 0 rgba(255,255,255,.08);padding:16px 18px;}"
"#widget_clock{right:24px;top:52px;width:220px;height:110px;}#widget_clock.stopwatch_mode{height:160px;padding:14px 16px;}"
"#wg_clock_view{position:relative;width:100%;height:100%;}#wg_time{color:#fff;font-size:46px;font-weight:800;line-height:1;width:calc(100% - 32px);}"
"#wg_date{color:rgba(215,212,242,.75);font-size:12px;font-weight:500;margin-top:6px;}"
"#widget_clock .wg_clock_icon{display:flex;align-items:center;justify-content:center;width:24px;height:24px;border-radius:8px;color:rgba(215,212,242,.75);font-size:12px;cursor:pointer;background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.08);}"
"#widget_clock .wg_clock_icon:hover{background:rgba(255,255,255,.14);color:#fff;}#wg_stopwatch_open{position:absolute;right:0;top:0;z-index:20;}"
"#wg_stopwatch_view{width:100%;height:100%;}#wg_stopwatch_head{display:flex;flex-direction:row;align-items:center;gap:8px;width:100%;height:24px;}"
"#wg_stopwatch_title{flex:1 1 auto;color:#f8f7ff;font-size:12px;font-weight:700;}#wg_stopwatch_state{color:rgba(165,170,210,.60);font-size:10px;}"
"#wg_stopwatch_time{width:100%;margin-top:8px;color:#fff;font-size:34px;font-weight:800;line-height:1;text-align:center;}"
"#wg_stopwatch_actions{display:flex;flex-direction:row;gap:8px;width:100%;margin-top:12px;}"
"#wg_stopwatch_actions .dlg_btn{display:flex;align-items:center;justify-content:center;flex:1 1 0;min-width:0;height:30px;border-radius:7px;background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.16);color:#f8f7ff;font-size:12px;cursor:pointer;}"
"#wg_stopwatch_actions .dlg_btn.primary{background:linear-gradient(180deg,#a898ff,#8d7bff);color:#fff;}"
"#widget_stats.clock_below_stopwatch{top:226px;}#widget_weather.clock_below_stopwatch{top:398px;}#widget_stats{right:24px;top:176px;width:220px;height:156px;}"
"#widget_stats .st_row,#widget_weather .st_row{display:flex;flex-direction:row;justify-content:space-between;margin-top:8px;}#widget_stats .st_label,#widget_weather .st_label{color:rgba(215,212,242,.75);font-size:11px;font-weight:500;}#widget_stats .st_val,#widget_weather .st_val{color:#fff;font-size:11px;font-weight:700;text-align:right;}"
"#widget_stats .st_bar{height:4px;border-radius:2px;background:rgba(255,255,255,.10);margin-top:5px;position:relative;overflow:hidden;}#widget_stats .st_fill{position:absolute;left:0;top:0;height:100%;width:20%;border-radius:2px;background:linear-gradient(90deg,#c9a6ff,#8d7bff);}#widget_stats .st_fill.g_mem{background:linear-gradient(90deg,#bf5af2,#9b3fe0);}#widget_stats .st_fill.g_disk{background:linear-gradient(90deg,#30d158,#25a244);}"
"#widget_weather{right:24px;top:348px;width:220px;height:168px;}#wg_wx_main{display:flex;flex-direction:row;align-items:center;gap:8px;margin-top:8px;margin-bottom:10px;}#wg_wx_icon{font-size:22px;color:#8d7bff;width:28px;text-align:center;}#wg_wx_desc{color:rgba(215,212,242,.75);font-size:12px;font-weight:500;}"
"#wg_wx_forecast{display:flex;flex-direction:row;gap:6px;justify-content:space-between;}#widget_weather .wf_day{flex:1 1 0;text-align:center;font-size:11px;color:rgba(255,255,255,.55);}#widget_weather .wf_name{font-size:10px;opacity:.7;margin-bottom:2px;}#widget_weather .wf_icon{font-size:16px;color:#c9a6ff;margin:4px 0;}#widget_weather .wf_temp{font-weight:600;font-size:11px;color:#fff;margin-top:2px;}";

static const char* luna_shell_rt_div_close(const char* open) {
    int depth = 0;
    const char* p = open;
    if (!p) return NULL;
    while (*p) {
        if (!strncmp(p, "<!--", 4)) {
            const char* end = strstr(p + 4, "-->");
            if (!end) return NULL;
            p = end + 3;
            continue;
        }
        if (!strncmp(p, "<div", 4) &&
            (p[4] == '>' || p[4] == ' ' || p[4] == '\t' || p[4] == '\n')) {
            depth++;
            p += 4;
            continue;
        }
        if (!strncmp(p, "</div>", 6)) {
            depth--;
            if (depth == 0) return p;
            p += 6;
            continue;
        }
        p++;
    }
    return NULL;
}

static int luna_shell_rt_legacy_widget_open(const char* p) {
    return !strncmp(p, "<div id=\"widget_clock\"", 22) ||
           !strncmp(p, "<div id=\"widget_stats\"", 22) ||
           !strncmp(p, "<div id=\"widget_weather\"", 24) ||
           !strncmp(p, "<div id=\"luna_shell_widgets\"", 28);
}

static char* luna_shell_rt_merge_widgets(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char* clean = (char*)malloc(n + 1);
    if (!clean) return NULL;
    const char* p = src;
    char* q = clean;
    while (*p) {
        if (luna_shell_rt_legacy_widget_open(p)) {
            const char* close = luna_shell_rt_div_close(p);
            if (close) {
                p = close + 6;
                continue;
            }
        }
        *q++ = *p++;
    }
    *q = '\0';

    const char* insert = NULL;
    const char* bg = strstr(clean, "<div id=\"bg_layer\"");
    if (bg) insert = luna_shell_rt_div_close(bg);
    if (!insert) insert = strstr(clean, "</body>");
    if (!insert) insert = clean + strlen(clean);

    size_t clean_n = strlen(clean);
    size_t head_n = (size_t)(insert - clean);
    size_t widgets_n = strlen(g_luna_shell_widgets_html);
    char* out = (char*)malloc(clean_n + widgets_n + 1);
    if (!out) { free(clean); return NULL; }
    memcpy(out, clean, head_n);
    memcpy(out + head_n, g_luna_shell_widgets_html, widgets_n);
    memcpy(out + head_n + widgets_n, insert, clean_n - head_n + 1);
    free(clean);
    return out;
}

/* Macro predicates: preserve luna-ui declarations/definitions, wrap shell
 * call sites only. */
#define LUNA_SHELL_RT_CAT_I(a,b) a##b
#define LUNA_SHELL_RT_CAT(a,b) LUNA_SHELL_RT_CAT_I(a,b)
#define LUNA_SHELL_RT_ARG3(_0,_1,_2,...) _2
#define LUNA_SHELL_RT_HAS_COMMA(...) LUNA_SHELL_RT_ARG3(__VA_ARGS__,1,0)
#define LUNA_SHELL_RT_IS_CONST_const ,
#define LUNA_SHELL_RT_IS_INT_int ,
#define LUNA_SHELL_RT_IS_VOID_void ,
#define LUNA_SHELL_RT_IS_CONST(x) \
    LUNA_SHELL_RT_HAS_COMMA(LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_IS_CONST_,x))
#define LUNA_SHELL_RT_IS_INT(x) \
    LUNA_SHELL_RT_HAS_COMMA(LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_IS_INT_,x))
#define LUNA_SHELL_RT_IS_VOID(x) \
    LUNA_SHELL_RT_HAS_COMMA(LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_IS_VOID_,x))

#define LUNA_SHELL_RT_PARSE_HTML_DECL(arg) luna_parse_html(arg)
#define LUNA_SHELL_RT_PARSE_HTML_CALL(arg) do { \
    const char* _lsr_src = (arg); \
    char* _lsr_html = luna_shell_rt_merge_widgets(_lsr_src); \
    (luna_parse_html)(_lsr_html ? _lsr_html : _lsr_src); \
    free(_lsr_html); \
} while (0)
#define LUNA_SHELL_RT_PARSE_HTML_SEL(v) LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_PARSE_HTML_,v)
#define LUNA_SHELL_RT_PARSE_HTML_1 LUNA_SHELL_RT_PARSE_HTML_DECL
#define LUNA_SHELL_RT_PARSE_HTML_0 LUNA_SHELL_RT_PARSE_HTML_CALL
#define luna_parse_html(arg) \
    LUNA_SHELL_RT_PARSE_HTML_SEL(LUNA_SHELL_RT_IS_CONST(arg))(arg)

#define LUNA_SHELL_RT_LOAD_HTML_DECL(arg) luna_load_html_file(arg)
#define LUNA_SHELL_RT_LOAD_HTML_CALL(arg) ({ \
    const char* _lsr_path = (arg); \
    char* _lsr_src = read_file(_lsr_path); \
    int _lsr_ok = 0; \
    if (_lsr_src) { \
        char* _lsr_html = luna_shell_rt_merge_widgets(_lsr_src); \
        (luna_parse_html)(_lsr_html ? _lsr_html : _lsr_src); \
        free(_lsr_html); \
        free(_lsr_src); \
        _lsr_ok = 1; \
    } \
    _lsr_ok; \
})
#define LUNA_SHELL_RT_LOAD_HTML_SEL(v) LUNA_SHELL_RT_CAT(LUNA_SHELL_RT_LOAD_HTML_,v)
#define LUNA_SHELL_RT_LOAD_HTML_1 LUNA_SHELL_RT_LOAD_HTML_DECL
#define LUNA_SHELL_RT_LOAD_HTML_0 LUNA_SHELL_RT_LOAD_HTML_CALL
#define luna_load_html_file(arg) \
    LUNA_SHELL_RT_LOAD_HTML_SEL(LUNA_SHELL_RT_IS_CONST(arg))(arg)

#endif /* LUNA_SHELL_WIDGETS_H */