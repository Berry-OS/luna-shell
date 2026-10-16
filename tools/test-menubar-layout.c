/* Test the actual shell styles without a display or GL context.
 * From apps/luna-shell:
 * cc -O0 -w tools/test-menubar-layout.c -o /tmp/luna-menubar-test -lGL -lm
 * /tmp/luna-menubar-test
 */
#define LUNA_UI_NO_PLATFORM
#define LUNA_UI_IMPLEMENTATION
#include "../../luna-ui/luna-ui.h"
int luna_app_run(const LunaAppConfig *cfg) { (void)cfg; return 0; }
void luna_app_request_redraw(void) {}
static int failures;
static void check(int ok, const char *skin, int width, const char *message) {
    if (!ok) { fprintf(stderr, "%s %d: %s\n", skin, width, message); failures++; }
}
int main(void) {
    char *html = read_file("skins/default/layout.html");
    char *base = read_file("skins/default/style.css");
    char *shared = read_file("skins/widgets.css");
    if (!html || !base || !shared) return 2;
    const char *skins[] = {"default", "nocturne-atelier", "windows-xp", "windows-95", "classic-mac", "beos", "amiga-workbench"};
    int widths[] = {480, 640, 800, 1024, 1280, 1920};
    for (unsigned s = 0; s < sizeof skins / sizeof skins[0]; s++) {
        char path[128]; snprintf(path, sizeof path, "skins/%s/style.css", skins[s]);
        char *skin = read_file(path);
        if (!skin) return 2;
        for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; w++) {
            luna_window_width = widths[w]; luna_window_height = 800;
            luna_reset_css(); luna_parse_html(html); luna_parse_css(base);
            if (s) luna_parse_css(skin);
            luna_parse_css(shared);
            luna_set_text(luna_get_element_by_id("mb_app"), "Very long browser application title that must not push the clock away");
            for (int i = 0; i < 8; i++) {
                char id[32]; snprintf(id, sizeof id, "tray_%d", i);
                luna_update_classes(luna_get_element_by_id(id), "hidden", NULL);
            }
            update_layout_pass();
            LunaElement *right = luna_element_at(luna_get_element_by_id("mb_right"));
            LunaElement *clock = luna_element_at(luna_get_element_by_id("mb_clock"));
            int tray_idx = luna_get_element_by_id("tray_area");
            LunaElement *tray = luna_element_at(tray_idx);
            check(right->x + right->w <= widths[w] + 1, skins[s], widths[w], "status bar outside viewport");
            check(clock->x + clock->w <= widths[w] + 1, skins[s], widths[w], "clock outside viewport");
            if (is_visible(tray_idx) && tray->w > 1) {
                check(tray->x + tray->w <= right->x + right->w + 1, skins[s], widths[w], "tray viewport outside status bar");
                if (tray->scroll_content_w > tray->w + 1) {
                    add_scroll_left(tray_idx, 10000, 1);
                    update_layout_pass();
                    LunaElement *last = luna_element_at(luna_get_element_by_id("tray_7"));
                    check(tray->scroll_left > 0, skins[s], widths[w], "overflow does not scroll");
                    check(last->x + last->w <= tray->x + tray->w + 2, skins[s], widths[w], "last task cannot be reached");
                }
            }
        }
        free(skin);
    }
    free(html); free(base); free(shared);
    if (!failures) puts("PASS: seven skins, six widths, long app title, eight tasks and scrolling to last task");
    return failures != 0;
}
