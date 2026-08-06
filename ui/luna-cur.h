/*
 * luna-cur.h — Windows .cur / .ani cursor loader + Luna theme manager
 *
 * Roles match luna-ui cursor_type:
 *   0 default  1 pointer  2 text  3 crosshair  4 ew-resize  5 ns-resize
 *
 * Theme resolution (first hit wins):
 *   $LUNA_CURSOR_THEME / settings  → theme name ("miku", "builtin", …)
 *   Search roots:
 *     $LUNA_CURSOR_PATH/<theme>
 *     $HOME/.local/share/luna/cursors/<theme>
 *     /usr/local/share/luna-desktop/cursors/<theme>
 *     /usr/share/luna-desktop/cursors/<theme>
 *     <exe-relative>/cursors/<theme>
 *     ./cursors/<theme>
 *
 * A theme dir holds .cur/.ani files plus optional theme.conf:
 *   name=Display Name
 *   default=arrow.ani
 *   pointer=hand.ani
 *   text=beam.ani
 *   …
 * Missing roles fall back to the built-in vector glyphs.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Luna (Vespera) — MPL 2.0
 */
#ifndef LUNA_CUR_H
#define LUNA_CUR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUNA_CUR_MAX_ROLES   6
#define LUNA_CUR_MAX_FRAMES  64
#define LUNA_CUR_MAX_W       128
#define LUNA_CUR_MAX_H       128
#define LUNA_CUR_PATH_MAX    512

typedef struct {
    int      w, h;
    int      hot_x, hot_y;
    int      delay_ms;     /* display time for this frame */
    uint32_t *argb;        /* w*h, AARRGGBB (host endian) */
} LunaCurFrame;

typedef struct {
    LunaCurFrame frames[LUNA_CUR_MAX_FRAMES];
    int          nframes;
    int          frame_i;
    double       frame_until; /* absolute time when to advance */
    int          loaded;
} LunaCurAnim;

typedef struct {
    char        theme_name[64];
    char        theme_dir[LUNA_CUR_PATH_MAX];
    char        display_name[64];
    LunaCurAnim roles[LUNA_CUR_MAX_ROLES];
    int         active_role;
    int         use_theme; /* 0 = builtin glyphs only */
} LunaCurTheme;

/* Decode a single .cur / .ico image blob into ARGB (caller frees *out_argb). */
static int luna_cur_decode_icon(const uint8_t* data, size_t len,
                                uint32_t** out_argb, int* out_w, int* out_h,
                                int* out_hot_x, int* out_hot_y);

/* Load .cur or .ani from path into anim (clears previous contents). */
static int luna_cur_load_file(LunaCurAnim* anim, const char* path);

/* Resolve and load a named theme ("builtin" / empty → built-in only). */
static int luna_cur_theme_load(LunaCurTheme* th, const char* theme_name);

/* Built-in Aero glyphs (from ui/luna-cur-aero.h via gen_luna_cur_aero.sh). */
static int luna_cur_theme_load_aero_embedded(LunaCurTheme* th);

static void luna_cur_theme_free(LunaCurTheme* th);

/* Select role; returns 1 if pixels available from theme file. */
static int luna_cur_theme_select(LunaCurTheme* th, int role);

/* Advance animation; returns 1 if the visible frame changed. */
static int luna_cur_theme_tick(LunaCurTheme* th, double now);

/* Current frame pixels (NULL if builtin / unloaded). */
static const LunaCurFrame* luna_cur_theme_frame(const LunaCurTheme* th);

#ifdef LUNA_CUR_IMPLEMENTATION

static void luna_cur_anim_clear(LunaCurAnim* a) {
    if (!a) return;
    for (int i = 0; i < a->nframes; i++) {
        free(a->frames[i].argb);
        a->frames[i].argb = NULL;
    }
    memset(a, 0, sizeof(*a));
}

static uint16_t luna_cur_r16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t luna_cur_r32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Decode BMP-style XOR+AND cursor image at `img` (BITMAPINFOHEADER…). */
static int luna_cur_decode_bmp(const uint8_t* img, size_t len,
                               int hot_x, int hot_y,
                               uint32_t** out_argb, int* out_w, int* out_h,
                               int* out_hot_x, int* out_hot_y) {
    if (!img || len < 40) return 0;
    uint32_t biSize = luna_cur_r32(img);
    if (biSize < 40) return 0;
    int32_t biWidth  = (int32_t)luna_cur_r32(img + 4);
    int32_t biHeight = (int32_t)luna_cur_r32(img + 8); /* 2× for XOR+AND */
    uint16_t biBitCount = luna_cur_r16(img + 14);
    uint32_t biCompression = luna_cur_r32(img + 16);
    if (biWidth <= 0 || biHeight <= 0 || biCompression != 0) return 0;
    int h = biHeight / 2;
    int w = biWidth;
    if (h <= 0) h = biHeight;
    if (w > LUNA_CUR_MAX_W || h > LUNA_CUR_MAX_H || w <= 0 || h <= 0) return 0;

    const uint8_t* pal = img + biSize;
    int pal_n = 0;
    if (biBitCount <= 8) {
        uint32_t clrUsed = luna_cur_r32(img + 32);
        pal_n = clrUsed ? (int)clrUsed : (1 << biBitCount);
    }
    const uint8_t* xor_bits = pal + pal_n * 4;
    int xor_stride = ((w * biBitCount + 31) / 32) * 4;
    size_t xor_bytes = (size_t)xor_stride * (size_t)h;
    if ((size_t)(xor_bits - img) + xor_bytes > len) return 0;
    const uint8_t* and_bits = xor_bits + xor_bytes;
    int and_stride = ((w + 31) / 32) * 4;
    size_t and_bytes = (size_t)and_stride * (size_t)h;
    /* AND mask is optional for 32bpp with alpha; tolerate short buffers. */
    int have_and = ((size_t)(and_bits - img) + and_bytes) <= len;

    uint32_t* argb = (uint32_t*)calloc((size_t)w * (size_t)h, 4);
    if (!argb) return 0;

    for (int y = 0; y < h; y++) {
        const uint8_t* row = xor_bits + (size_t)(h - 1 - y) * (size_t)xor_stride;
        for (int x = 0; x < w; x++) {
            uint32_t px = 0;
            if (biBitCount == 32) {
                const uint8_t* p = row + x * 4;
                px = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[1] << 8)  | (uint32_t)p[0];
            } else if (biBitCount == 24) {
                const uint8_t* p = row + x * 3;
                px = 0xFF000000u | ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[1] << 8) | (uint32_t)p[0];
            } else if (biBitCount == 8) {
                uint8_t i = row[x];
                if (i < pal_n) {
                    const uint8_t* p = pal + i * 4;
                    px = 0xFF000000u | ((uint32_t)p[2] << 16) |
                         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
                }
            } else if (biBitCount == 4) {
                uint8_t v = row[x / 2];
                uint8_t i = (x & 1) ? (v & 0xF) : (v >> 4);
                if (i < pal_n) {
                    const uint8_t* p = pal + i * 4;
                    px = 0xFF000000u | ((uint32_t)p[2] << 16) |
                         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
                }
            } else if (biBitCount == 1) {
                uint8_t v = row[x / 8];
                uint8_t i = (v >> (7 - (x & 7))) & 1;
                if (i < pal_n) {
                    const uint8_t* p = pal + i * 4;
                    px = 0xFF000000u | ((uint32_t)p[2] << 16) |
                         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
                }
            }
            if (have_and && biBitCount < 32) {
                const uint8_t* arow = and_bits + (size_t)(h - 1 - y) * (size_t)and_stride;
                uint8_t bit = (arow[x / 8] >> (7 - (x & 7))) & 1;
                if (bit) px = 0; /* transparent */
            }
            argb[y * w + x] = px;
        }
    }

    *out_argb = argb;
    *out_w = w;
    *out_h = h;
    *out_hot_x = hot_x;
    *out_hot_y = hot_y;
    if (*out_hot_x < 0 || *out_hot_x >= w) *out_hot_x = 0;
    if (*out_hot_y < 0 || *out_hot_y >= h) *out_hot_y = 0;
    return 1;
}

static int luna_cur_decode_icon(const uint8_t* data, size_t len,
                                uint32_t** out_argb, int* out_w, int* out_h,
                                int* out_hot_x, int* out_hot_y) {
    if (!data || len < 22) return 0;
    uint16_t reserved = luna_cur_r16(data);
    uint16_t type     = luna_cur_r16(data + 2);
    uint16_t count    = luna_cur_r16(data + 4);
    (void)reserved;
    if ((type != 1 && type != 2) || count == 0) return 0;

    /* Pick the largest entry that still fits our max size. */
    int best = -1, best_area = -1;
    int hot_x = 0, hot_y = 0;
    uint32_t img_off = 0, img_bytes = 0;
    for (int i = 0; i < (int)count; i++) {
        const uint8_t* e = data + 6 + i * 16;
        if ((size_t)(e - data) + 16 > len) break;
        int w = e[0] ? e[0] : 256;
        int h = e[1] ? e[1] : 256;
        uint16_t a = luna_cur_r16(e + 4);
        uint16_t b = luna_cur_r16(e + 6);
        uint32_t nbytes = luna_cur_r32(e + 8);
        uint32_t ioff   = luna_cur_r32(e + 12);
        if (w > LUNA_CUR_MAX_W || h > LUNA_CUR_MAX_H) continue;
        if (ioff + nbytes > len) continue;
        int area = w * h;
        if (area > best_area) {
            best_area = area;
            best = i;
            img_off = ioff;
            img_bytes = nbytes;
            if (type == 2) { hot_x = (int)a; hot_y = (int)b; }
            else { hot_x = 0; hot_y = 0; }
        }
    }
    if (best < 0) return 0;

    const uint8_t* img = data + img_off;
    /* PNG-compressed ICO (rare in classic .ani) — not supported here. */
    if (img_bytes >= 8 && img[0] == 0x89 && img[1] == 'P') return 0;
    return luna_cur_decode_bmp(img, img_bytes, hot_x, hot_y,
                               out_argb, out_w, out_h, out_hot_x, out_hot_y);
}

static int luna_cur_anim_push(LunaCurAnim* anim, uint32_t* argb,
                              int w, int h, int hx, int hy, int delay_ms) {
    if (!anim || !argb || anim->nframes >= LUNA_CUR_MAX_FRAMES) {
        free(argb);
        return 0;
    }
    LunaCurFrame* f = &anim->frames[anim->nframes++];
    f->argb = argb;
    f->w = w; f->h = h;
    f->hot_x = hx; f->hot_y = hy;
    f->delay_ms = delay_ms > 0 ? delay_ms : 100;
    return 1;
}

static int luna_cur_load_cur_blob(LunaCurAnim* anim, const uint8_t* data, size_t len,
                                  int delay_ms) {
    uint32_t* argb = NULL;
    int w = 0, h = 0, hx = 0, hy = 0;
    if (!luna_cur_decode_icon(data, len, &argb, &w, &h, &hx, &hy)) return 0;
    return luna_cur_anim_push(anim, argb, w, h, hx, hy, delay_ms);
}

static int luna_cur_load_ani(LunaCurAnim* anim, const uint8_t* data, size_t len) {
    if (len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "ACON", 4) != 0)
        return 0;

    uint32_t nframes = 0, nsteps = 0, jif_rate = 0;
    uint32_t rates[LUNA_CUR_MAX_FRAMES];
    uint32_t seq[LUNA_CUR_MAX_FRAMES];
    int nrate = 0, nseq = 0;
    memset(rates, 0, sizeof(rates));
    memset(seq, 0, sizeof(seq));

    /* Collect icon blobs in order. */
    const uint8_t* icons[LUNA_CUR_MAX_FRAMES];
    size_t icon_lens[LUNA_CUR_MAX_FRAMES];
    int nicons = 0;

    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t* tag = data + off;
        uint32_t sz = luna_cur_r32(data + off + 4);
        size_t payload_off = off + 8;
        if (payload_off + sz > len) break;

        if (!memcmp(tag, "anih", 4) && sz >= 36) {
            const uint8_t* p = data + payload_off;
            /* cbSize, nFrames, nSteps, cx, cy, bitcount, planes, jifRate, flags */
            nframes = luna_cur_r32(p + 4);
            nsteps  = luna_cur_r32(p + 8);
            jif_rate = luna_cur_r32(p + 28);
            (void)nframes;
        } else if (!memcmp(tag, "rate", 4)) {
            nrate = (int)(sz / 4);
            if (nrate > LUNA_CUR_MAX_FRAMES) nrate = LUNA_CUR_MAX_FRAMES;
            for (int i = 0; i < nrate; i++)
                rates[i] = luna_cur_r32(data + payload_off + (size_t)i * 4);
        } else if (!memcmp(tag, "seq ", 4)) {
            nseq = (int)(sz / 4);
            if (nseq > LUNA_CUR_MAX_FRAMES) nseq = LUNA_CUR_MAX_FRAMES;
            for (int i = 0; i < nseq; i++)
                seq[i] = luna_cur_r32(data + payload_off + (size_t)i * 4);
        } else if (!memcmp(tag, "LIST", 4) && sz >= 4) {
            const uint8_t* list = data + payload_off;
            if (!memcmp(list, "fram", 4)) {
                size_t o = 4;
                while (o + 8 <= sz) {
                    const uint8_t* ct = list + o;
                    uint32_t cs = luna_cur_r32(list + o + 4);
                    if (o + 8 + cs > sz) break;
                    if (!memcmp(ct, "icon", 4) && nicons < LUNA_CUR_MAX_FRAMES) {
                        icons[nicons] = list + o + 8;
                        icon_lens[nicons] = cs;
                        nicons++;
                    }
                    o += 8 + cs + (cs & 1);
                }
            }
        }

        off += 8 + sz + (sz & 1);
    }

    if (nicons <= 0) return 0;
    int steps = nseq > 0 ? nseq : (nsteps > 0 ? (int)nsteps : nicons);
    if (steps > LUNA_CUR_MAX_FRAMES) steps = LUNA_CUR_MAX_FRAMES;

    int default_delay = jif_rate > 0 ? (int)((jif_rate * 1000) / 60) : 100;
    if (default_delay < 16) default_delay = 16;

    for (int s = 0; s < steps; s++) {
        int fi = nseq > 0 ? (int)seq[s] : s;
        if (fi < 0 || fi >= nicons) fi = s % nicons;
        int delay = default_delay;
        if (nrate > 0) {
            int ri = s < nrate ? s : (nrate - 1);
            if (rates[ri] > 0)
                delay = (int)((rates[ri] * 1000) / 60);
            if (delay < 16) delay = 16;
        }
        if (!luna_cur_load_cur_blob(anim, icons[fi], icon_lens[fi], delay))
            continue;
    }
    return anim->nframes > 0;
}

static int luna_cur_load_file(LunaCurAnim* anim, const char* path) {
    if (!anim || !path || !*path) return 0;
    luna_cur_anim_clear(anim);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);

    int ok = 0;
    if (sz >= 12 && !memcmp(buf, "RIFF", 4))
        ok = luna_cur_load_ani(anim, buf, (size_t)sz);
    else
        ok = luna_cur_load_cur_blob(anim, buf, (size_t)sz, 100);
    free(buf);
    if (ok) {
        anim->loaded = 1;
        anim->frame_i = 0;
        anim->frame_until = 0;
    } else {
        luna_cur_anim_clear(anim);
    }
    return ok;
}

static const char* luna_cur_role_key(int role) {
    switch (role) {
        case 0: return "default";
        case 1: return "pointer";
        case 2: return "text";
        case 3: return "crosshair";
        case 4: return "ew-resize";
        case 5: return "ns-resize";
        default: return NULL;
    }
}

static const char* luna_cur_role_fallback_file(int role) {
    /* Sensible filenames when theme.conf is missing (Miku / Windows names). */
    switch (role) {
        case 0: return "Blinking Cursor.ani";
        case 1: return "hand2_.ani";
        case 2: return "beam.ani";
        case 3: return "Precision Select.ani";
        case 4: return "Horizontal Resize.ani";
        case 5: return "Vertical Resize.ani";
        default: return NULL;
    }
}

static int luna_cur_file_exists(const char* path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int luna_cur_dir_exists(const char* path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int luna_cur_join_path(char* out, size_t out_n, const char* dir, const char* leaf) {
    size_t dl = strnlen(dir, out_n);
    size_t ll = strnlen(leaf, out_n);
    if (dl >= out_n || ll >= out_n || dl + 1 >= out_n || ll > out_n - dl - 2)
        return 0;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, leaf, ll);
    out[dl + 1 + ll] = '\0';
    return 1;
}

static int luna_cur_find_theme_dir(const char* theme, char* out, size_t out_n) {
    if (!theme || !*theme || !strcmp(theme, "builtin") || !strcmp(theme, "none"))
        return 0;

    const char* roots[8];
    int n = 0;
    char home_buf[LUNA_CUR_PATH_MAX];
    const char* env_path = getenv("LUNA_CURSOR_PATH");
    if (env_path && *env_path) roots[n++] = env_path;

    const char* home = getenv("HOME");
    if (home && *home) {
        snprintf(home_buf, sizeof(home_buf), "%s/.local/share/luna/cursors", home);
        roots[n++] = home_buf;
    }
    roots[n++] = "/usr/local/share/luna-desktop/cursors";
    roots[n++] = "/usr/share/luna-desktop/cursors";
    roots[n++] = "cursors";
    roots[n++] = "../cursors";
    roots[n++] = "apps/luna-shell/cursors";

    char trial[LUNA_CUR_PATH_MAX];
    for (int i = 0; i < n; i++) {
        snprintf(trial, sizeof(trial), "%s/%s", roots[i], theme);
        if (luna_cur_dir_exists(trial)) {
            snprintf(out, out_n, "%s", trial);
            return 1;
        }
    }
    /* Allow LUNA_CURSOR_PATH to point directly at the theme directory. */
    if (env_path && luna_cur_dir_exists(env_path)) {
        snprintf(out, out_n, "%s", env_path);
        return 1;
    }
    return 0;
}

static void luna_cur_parse_theme_conf(LunaCurTheme* th, char role_files[LUNA_CUR_MAX_ROLES][256]) {
    char path[LUNA_CUR_PATH_MAX];
    if (!luna_cur_join_path(path, sizeof(path), th->theme_dir, "theme.conf")) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#' || *s == ';') continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = s;
        char* val = eq + 1;
        /* trim key */
        char* ke = key + strlen(key);
        while (ke > key && isspace((unsigned char)ke[-1])) *--ke = 0;
        while (*val && isspace((unsigned char)*val)) val++;
        if (!strcmp(key, "name")) {
            snprintf(th->display_name, sizeof(th->display_name), "%s", val);
            continue;
        }
        for (int r = 0; r < LUNA_CUR_MAX_ROLES; r++) {
            const char* rk = luna_cur_role_key(r);
            if (rk && !strcmp(key, rk)) {
                snprintf(role_files[r], 256, "%s", val);
                break;
            }
        }
    }
    fclose(f);
}

#include "luna-cur-aero.h"

static int luna_cur_anim_load_embedded(LunaCurAnim* anim, const LunaCurAeroRole* role) {
    if (!anim || !role || role->nframes <= 0) return 0;
    luna_cur_anim_clear(anim);
    for (int i = 0; i < role->nframes; i++) {
        const LunaCurAeroFrame* ef = role->frames[i];
        if (!ef || !ef->argb || ef->w <= 0 || ef->h <= 0) continue;
        size_t n = (size_t)ef->w * (size_t)ef->h;
        uint32_t* copy = (uint32_t*)malloc(n * sizeof(uint32_t));
        if (!copy) continue;
        memcpy(copy, ef->argb, n * sizeof(uint32_t));
        if (!luna_cur_anim_push(anim, copy, ef->w, ef->h, ef->hot_x, ef->hot_y, ef->delay_ms)) {
            free(copy);
            break;
        }
    }
    if (anim->nframes > 0) {
        anim->loaded = 1;
        anim->frame_i = 0;
        anim->frame_until = 0;
        return 1;
    }
    luna_cur_anim_clear(anim);
    return 0;
}

static int luna_cur_theme_load_aero_embedded(LunaCurTheme* th) {
    if (!th) return 0;
    int any = 0;
    for (int r = 0; r < LUNA_AERO_ROLE_COUNT && r < LUNA_CUR_MAX_ROLES; r++) {
        const LunaCurAeroRole* role = luna_aero_roles[r];
        if (!role) continue;
        if (luna_cur_anim_load_embedded(&th->roles[r], role))
            any = 1;
    }
    th->use_theme = any ? 1 : 0;
    snprintf(th->theme_name, sizeof(th->theme_name), "aero");
    snprintf(th->display_name, sizeof(th->display_name), "Aero");
    if (any)
        fprintf(stderr, "[luna-cur] embedded Aero cursors loaded\n");
    return any;
}

static int luna_cur_theme_load(LunaCurTheme* th, const char* theme_name) {
    if (!th) return 0;
    luna_cur_theme_free(th);
    memset(th, 0, sizeof(*th));
    th->active_role = 0;

    const char* name = theme_name && *theme_name ? theme_name : "aero";
    snprintf(th->theme_name, sizeof(th->theme_name), "%s", name);
    snprintf(th->display_name, sizeof(th->display_name), "%s", name);

    if (!strcmp(name, "aero") || !strcmp(name, "builtin") || !strcmp(name, "none") ||
        !strcmp(name, "default-vector")) {
        return luna_cur_theme_load_aero_embedded(th);
    }

    if (!luna_cur_find_theme_dir(name, th->theme_dir, sizeof(th->theme_dir))) {
        fprintf(stderr, "[luna-cur] theme '%s' not found — using embedded Aero\n", name);
        return luna_cur_theme_load_aero_embedded(th);
    }

    char role_files[LUNA_CUR_MAX_ROLES][256];
    memset(role_files, 0, sizeof(role_files));
    luna_cur_parse_theme_conf(th, role_files);

    int any = 0;
    for (int r = 0; r < LUNA_CUR_MAX_ROLES; r++) {
        const char* file = role_files[r][0] ? role_files[r] : luna_cur_role_fallback_file(r);
        if (!file) continue;
        char path[LUNA_CUR_PATH_MAX];
        if (!luna_cur_join_path(path, sizeof(path), th->theme_dir, file)) continue;
        if (!luna_cur_file_exists(path)) {
            /* try lowercase aliases */
            continue;
        }
        if (luna_cur_load_file(&th->roles[r], path)) {
            any = 1;
            fprintf(stderr, "[luna-cur] role %s ← %s (%d frames)\n",
                    luna_cur_role_key(r), file, th->roles[r].nframes);
        }
    }

    th->use_theme = any ? 1 : 0;
    if (!any) {
        fprintf(stderr, "[luna-cur] theme '%s' has no usable .cur/.ani — embedded Aero\n", name);
        return luna_cur_theme_load_aero_embedded(th);
    }
    fprintf(stderr, "[luna-cur] theme '%s' loaded from %s\n",
            th->display_name[0] ? th->display_name : name, th->theme_dir);
    return 1;
}

static void luna_cur_theme_free(LunaCurTheme* th) {
    if (!th) return;
    for (int i = 0; i < LUNA_CUR_MAX_ROLES; i++)
        luna_cur_anim_clear(&th->roles[i]);
    memset(th, 0, sizeof(*th));
}

static int luna_cur_theme_select(LunaCurTheme* th, int role) {
    if (!th) return 0;
    if (role < 0 || role >= LUNA_CUR_MAX_ROLES) role = 0;
    th->active_role = role;
    LunaCurAnim* a = &th->roles[role];
    if (!th->use_theme || !a->loaded || a->nframes <= 0) return 0;
    return 1;
}

static int luna_cur_theme_tick(LunaCurTheme* th, double now) {
    if (!th || !th->use_theme) return 0;
    LunaCurAnim* a = &th->roles[th->active_role];
    if (!a->loaded || a->nframes <= 1) return 0;
    if (a->frame_until <= 0.0) {
        a->frame_until = now + a->frames[a->frame_i].delay_ms / 1000.0;
        return 0;
    }
    if (now < a->frame_until) return 0;
    a->frame_i = (a->frame_i + 1) % a->nframes;
    a->frame_until = now + a->frames[a->frame_i].delay_ms / 1000.0;
    return 1;
}

static const LunaCurFrame* luna_cur_theme_frame(const LunaCurTheme* th) {
    if (!th || !th->use_theme) return NULL;
    const LunaCurAnim* a = &th->roles[th->active_role];
    if (!a->loaded || a->nframes <= 0) return NULL;
    return &a->frames[a->frame_i];
}

#endif /* LUNA_CUR_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* LUNA_CUR_H */
