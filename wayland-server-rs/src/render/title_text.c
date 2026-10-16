/*
 * Window titles for server-side decorations: UTF-8 → 8-bit coverage mask.
 * Fonts are loaded once; each codepoint uses the first font that has it, so a
 * Latin face can be followed by a CJK fallback.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TT_MAX_FONTS 4
static stbtt_fontinfo g_fi[TT_MAX_FONTS];
static unsigned char* g_data[TT_MAX_FONTS];
static int g_nf;

int luna_tt_count(void) { return g_nf; }

int luna_tt_add(const char* path) {
    if (g_nf >= TT_MAX_FONTS) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64L << 20)) { fclose(f); return -1; }
    unsigned char* d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return -1; }
    fclose(f);
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || !stbtt_InitFont(&g_fi[g_nf], d, off)) { free(d); return -1; }
    g_data[g_nf] = d;
    return g_nf++;
}

/* Does any loaded font cover this codepoint? */
int luna_tt_covers(unsigned cp) {
    for (int i = 0; i < g_nf; i++)
        if (stbtt_FindGlyphIndex(&g_fi[i], (int)cp)) return 1;
    return 0;
}

static unsigned next_cp(const unsigned char** p, const unsigned char* end) {
    const unsigned char* s = *p;
    unsigned c = *s++;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (extra) c &= (0x3fu >> extra);
    while (extra-- > 0 && s < end && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
    *p = s;
    return c;
}

static int font_for(unsigned cp) {
    for (int i = 0; i < g_nf; i++)
        if (stbtt_FindGlyphIndex(&g_fi[i], (int)cp)) return i;
    return 0;
}

/* Renders `len` bytes of UTF-8 at `px` pixels (em height).  `bold` strikes each
 * glyph a second time half a pixel to the right (the bundled face has no
 * semi-bold).  Returns a malloc'd w*h coverage mask, or NULL. */
unsigned char* luna_tt_text(const char* s, size_t len, int px, int bold, int* ow, int* oh, int* oasc) {
    if (g_nf == 0 || px < 6) return NULL;
    const unsigned char* end = (const unsigned char*)s + len;
    float scale0 = stbtt_ScaleForPixelHeight(&g_fi[0], (float)px);
    int a0, d0, gap;
    stbtt_GetFontVMetrics(&g_fi[0], &a0, &d0, &gap);
    int asc = (int)(a0 * scale0 + 0.5f), desc = (int)(-d0 * scale0 + 0.999f);
    int h = asc + desc + 2;

    float wsum = 0;
    for (const unsigned char* p = (const unsigned char*)s; p < end;) {
        unsigned cp = next_cp(&p, end);
        int f = font_for(cp), adv, lsb;
        stbtt_GetCodepointHMetrics(&g_fi[f], (int)cp, &adv, &lsb);
        wsum += adv * stbtt_ScaleForPixelHeight(&g_fi[f], (float)px);
    }
    int w = (int)wsum + 3;
    unsigned char* out = calloc((size_t)w * (size_t)h, 1);
    if (!out) return NULL;
    float x = 1.0f;
    for (const unsigned char* p = (const unsigned char*)s; p < end;) {
        unsigned cp = next_cp(&p, end);
        int f = font_for(cp), adv, lsb;
        float sc = stbtt_ScaleForPixelHeight(&g_fi[f], (float)px);
        stbtt_GetCodepointHMetrics(&g_fi[f], (int)cp, &adv, &lsb);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            float shift = pass * 0.5f;
            int ix = (int)x, x0, y0, x1, y1;
            float fx = x - ix + shift;
            stbtt_GetCodepointBitmapBoxSubpixel(&g_fi[f], (int)cp, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw < 256 && gh < 256) {
                unsigned char tmp[256 * 256];
                memset(tmp, 0, (size_t)gw * (size_t)gh);
                stbtt_MakeCodepointBitmapSubpixel(&g_fi[f], tmp, gw, gh, gw, sc, sc, fx, 0, (int)cp);
                for (int yy = 0; yy < gh; yy++) {
                    int oy = asc + y0 + yy;
                    if (oy < 0 || oy >= h) continue;
                    for (int xx = 0; xx < gw; xx++) {
                        int ox = ix + x0 + xx;
                        if (ox < 0 || ox >= w) continue;
                        unsigned char v = tmp[yy * gw + xx];
                        if (v > out[oy * w + ox]) out[oy * w + ox] = v;
                    }
                }
            }
        }
        x += adv * sc;
    }
    *ow = w; *oh = h; *oasc = asc;
    return out;
}

void luna_tt_free(unsigned char* p) { free(p); }
