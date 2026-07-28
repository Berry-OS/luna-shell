#!/bin/sh
# Generate embedded Aero cursor blobs for luna-shell (C) and luna-compositor (Rust).
# Requires: gcc, libXcursor, libX11, aero cursor theme on disk.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AERO_DIR="${LUNA_AERO_CURSOR_DIR:-/usr/share/icons/aero/cursors}"
OUT_C="${SCRIPT_DIR}/luna-cur-aero.h"
OUT_RS="${SCRIPT_DIR}/../wayland-server-rs/src/cursor_aero.rs"
TMPGEN="$(mktemp /tmp/luna_xcur_gen.XXXXXX)"

if [ ! -d "$AERO_DIR" ]; then
    echo "error: Aero cursor dir not found: $AERO_DIR" >&2
    exit 1
fi

cat > "${TMPGEN}.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xcursor/Xcursor.h>

static void emit_blob_c(const char* name, XcursorImage* im) {
    int n = im->width * im->height;
    printf("static const uint32_t %s_argb[] = {\n", name);
    for (int i = 0; i < n; i++) {
        if (i % 8 == 0) printf("    ");
        printf("0x%08xu", im->pixels[i]);
        if (i + 1 < n) printf(",");
        if (i % 8 == 7) printf("\n");
    }
    if (n % 8) printf("\n");
    printf("};\n");
}

static void emit_blob_rs(const char* name, XcursorImage* im) {
    int n = im->width * im->height;
    printf("static CUR_%s: [u32; %d] = [\n", name, n);
    for (int i = 0; i < n; i++) {
        if (i % 8 == 0) printf("    ");
        printf("0x%08x", im->pixels[i]);
        if (i + 1 < n) printf(", ");
        if (i % 8 == 7) printf("\n");
    }
    if (n % 8) printf("\n");
    printf("];\n");
}

static int load_role_c(const char* dir, const char* file, const char* sym) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    XcursorImages* imgs = XcursorFilenameLoadImages(path, 32);
    if (!imgs || imgs->nimage <= 0) {
        fprintf(stderr, "skip %s\n", path);
        return 0;
    }
    printf("/* %s */\n", file);
    for (int i = 0; i < imgs->nimage; i++) {
        char name[128];
        snprintf(name, sizeof(name), "luna_aero_%s_%d", sym, i);
        emit_blob_c(name, imgs->images[i]);
        printf("static const LunaCurAeroFrame luna_aero_%s_%d_frame = { %d, %d, %d, %d, %d, %s_argb };\n",
               sym, i, imgs->images[i]->width, imgs->images[i]->height,
               imgs->images[i]->xhot, imgs->images[i]->yhot, imgs->images[i]->delay, name);
    }
    printf("static const LunaCurAeroFrame* luna_aero_%s_frames[] = {\n", sym);
    for (int i = 0; i < imgs->nimage; i++)
        printf("    &luna_aero_%s_%d_frame,\n", sym, i);
    printf("};\n");
    printf("static const LunaCurAeroRole luna_aero_role_%s = { %d, luna_aero_%s_frames };\n\n",
           sym, imgs->nimage, sym);
    int n = imgs->nimage;
    XcursorImagesDestroy(imgs);
    return n;
}

static int load_role_rs(const char* dir, const char* file, const char* sym) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    XcursorImages* imgs = XcursorFilenameLoadImages(path, 32);
    if (!imgs || imgs->nimage <= 0) return 0;
    for (int i = 0; i < imgs->nimage; i++) {
        char name[128];
        snprintf(name, sizeof(name), "%s_%d", sym, i);
        emit_blob_rs(name, imgs->images[i]);
        printf("static const EMBED_FRAME %s_%d = EMBED_FRAME { w: %d, h: %d, hot_x: %d, hot_y: %d, delay_ms: %d, pixels: &CUR_%s };\n",
               sym, i, imgs->images[i]->width, imgs->images[i]->height,
               imgs->images[i]->xhot, imgs->images[i]->yhot, imgs->images[i]->delay, name);
    }
    printf("static const [EMBED_FRAME; %d] ROLE_%s = [\n", imgs->nimage, sym);
    for (int i = 0; i < imgs->nimage; i++)
        printf("    %s_%d,\n", sym, i);
    printf("];\n\n");
    int n = imgs->nimage;
    XcursorImagesDestroy(imgs);
    return n;
}

int main(int argc, char** argv) {
    const char* dir = argv[1];
    const char* mode = argv[2];
    if (!strcmp(mode, "c")) {
        printf("/* Embedded Aero cursors from %s — regenerate with ui/gen_luna_cur_aero.sh */\n", dir);
        printf("#ifndef LUNA_CUR_AERO_H\n#define LUNA_CUR_AERO_H\n#include <stdint.h>\n\n");
        printf("typedef struct { int w,h,hot_x,hot_y,delay_ms; const uint32_t* argb; } LunaCurAeroFrame;\n");
        printf("typedef struct { int nframes; const LunaCurAeroFrame** frames; } LunaCurAeroRole;\n\n");
        load_role_c(dir, "left_ptr", "default");
        load_role_c(dir, "hand2", "pointer");
        load_role_c(dir, "xterm", "text");
        load_role_c(dir, "crosshair", "crosshair");
        load_role_c(dir, "h_double_arrow", "ew");
        load_role_c(dir, "v_double_arrow", "ns");
        load_role_c(dir, "watch", "watch");
        printf("static const LunaCurAeroRole* luna_aero_roles[6] = {\n");
        printf("    &luna_aero_role_default, &luna_aero_role_pointer, &luna_aero_role_text,\n");
        printf("    &luna_aero_role_crosshair, &luna_aero_role_ew, &luna_aero_role_ns,\n");
        printf("};\n#define LUNA_AERO_ROLE_COUNT 6\n#endif\n");
    } else {
        printf("// Embedded Aero cursors from %s — regenerate with ui/gen_luna_cur_aero.sh\n", dir);
        printf("#[allow(dead_code)]\nstruct EmbedFrame { w: i32, h: i32, hot_x: i32, hot_y: i32, delay_ms: i32, pixels: &'static [u32] }\n");
        printf("type EMBED_FRAME = EmbedFrame;\n\n");
        load_role_rs(dir, "left_ptr", "DEFAULT");
        printf("pub fn blit_default_cursor(fb: &mut crate::render::Framebuffer, x: i32, y: i32) {\n");
        printf("    blit_embed_frame(fb, &ROLE_DEFAULT[0], x, y);\n");
        printf("}\n\n");
        printf("pub fn blit_embed_frame(fb: &mut crate::render::Framebuffer, frame: &EmbedFrame, x: i32, y: i32) {\n");
        printf("    let w = frame.w as u32;\n    let h = frame.h as u32;\n");
        printf("    for row in 0..h {\n");
        printf("        for col in 0..w {\n");
        printf("            let px = frame.pixels[row as usize * w as usize + col as usize];\n");
        printf("            let a = (px >> 24) & 0xff;\n            if a == 0 { continue; }\n");
        printf("            let dx = x + col as i32 - frame.hot_x;\n");
        printf("            let dy = y + row as i32 - frame.hot_y;\n");
        printf("            if dx < 0 || dy < 0 || dx >= fb.width as i32 || dy >= fb.height as i32 { continue; }\n");
        printf("            let idx = dy as usize * fb.width as usize + dx as usize;\n");
        printf("            fb.pixels[idx] = blend_px(fb.pixels[idx], px);\n");
        printf("        }\n    }\n}\n\n");
        printf("fn blend_px(dst: u32, src: u32) -> u32 {\n");
        printf("    let a = (src >> 24) & 0xff;\n");
        printf("    if a == 0xff { return src; }\n");
        printf("    if a == 0 { return dst; }\n");
        printf("    let inv = 255 - a;\n");
        printf("    let dr = (dst >> 16) & 0xff; let dg = (dst >> 8) & 0xff; let db = dst & 0xff;\n");
        printf("    let sr = (src >> 16) & 0xff; let sg = (src >> 8) & 0xff; let sb = src & 0xff;\n");
        printf("    let r = (sr * a + dr * inv) / 255;\n");
        printf("    let g = (sg * a + dg * inv) / 255;\n");
        printf("    let b = (sb * a + db * inv) / 255;\n");
        printf("    0xff000000 | (r << 16) | (g << 8) | b\n");
        printf("}\n");
    }
    return 0;
}
EOF

gcc -o "${TMPGEN}" "${TMPGEN}.c" -lXcursor -lX11
"${TMPGEN}" "$AERO_DIR" c > "$OUT_C"
"${TMPGEN}" "$AERO_DIR" rs > "$OUT_RS"
rm -f "${TMPGEN}" "${TMPGEN}.c"
echo "→ wrote $OUT_C"
echo "→ wrote $OUT_RS"
