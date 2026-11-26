/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#include "SDL_kittyfont.h"
#include "SDL_kittyfont_data.h"

#include <zlib.h>

/* Route stb_truetype math & allocations through SDL so libSDL3 never needs -lm. */
#define STBTT_ifloor(x) ((int)SDL_floorf(x))
#define STBTT_iceil(x)  ((int)SDL_ceilf(x))
#define STBTT_sqrt(x)   SDL_sqrtf(x)
#define STBTT_pow(x, y) SDL_powf((x), (y))
#define STBTT_fmod(x, y) SDL_fmodf((x), (y))
#define STBTT_cos(x)    SDL_cosf(x)
#define STBTT_acos(x)   SDL_acosf(x)
#define STBTT_fabs(x)   SDL_fabsf(x)
#define STBTT_malloc(x, u) ((void)(u), SDL_malloc(x))
#define STBTT_free(x, u)   ((void)(u), SDL_free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) SDL_strlen(x)
#define STBTT_memcpy    SDL_memcpy
#define STBTT_memset    SDL_memset
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "SDL_kittystb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define KITTY_GLYPH_CACHE_CAP 512

typedef struct KITTY_GlyphCacheEntry
{
    Uint32 codepoint;
    int px_q; /* quantized pixel height (px_size * 2) */
    int w;
    int h;
    int xoff;
    int yoff;
    float advance;
    Uint8 *bitmap;
    bool valid;
} KITTY_GlyphCacheEntry;

typedef struct KITTY_FontState
{
    bool tried;
    bool ready;
    Uint8 *ttf_buf;
    stbtt_fontinfo info;
    int ascent;
    int descent;
    int line_gap;
    KITTY_GlyphCacheEntry cache[KITTY_GLYPH_CACHE_CAP];
} KITTY_FontState;

static KITTY_FontState KITTY_font = { 0 };

bool KITTY_FontEnsureInit(void)
{
    uLongf dst_len;
    Uint8 *buf;

    if (KITTY_font.ready) {
        return true;
    }
    if (KITTY_font.tried) {
        return false;
    }
    KITTY_font.tried = true;

    buf = (Uint8 *)SDL_malloc(KITTY_GEIST_TTF_RAW_SIZE);
    if (!buf) {
        return false;
    }
    dst_len = (uLongf)KITTY_GEIST_TTF_RAW_SIZE;
    if (uncompress(buf, &dst_len, KITTY_geist_ttf_zlib, (uLong)KITTY_GEIST_TTF_ZLIB_SIZE) != Z_OK ||
        dst_len != (uLongf)KITTY_GEIST_TTF_RAW_SIZE) {
        SDL_free(buf);
        return false;
    }
    if (!stbtt_InitFont(&KITTY_font.info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
        SDL_free(buf);
        return false;
    }
    KITTY_font.ttf_buf = buf;
    stbtt_GetFontVMetrics(&KITTY_font.info, &KITTY_font.ascent, &KITTY_font.descent, &KITTY_font.line_gap);
    KITTY_font.ready = true;
    return true;
}

void KITTY_FontShutdown(void)
{
    int i;
    for (i = 0; i < KITTY_GLYPH_CACHE_CAP; ++i) {
        if (KITTY_font.cache[i].bitmap) {
            SDL_free(KITTY_font.cache[i].bitmap);
            KITTY_font.cache[i].bitmap = NULL;
        }
        KITTY_font.cache[i].valid = false;
    }
    SDL_free(KITTY_font.ttf_buf);
    KITTY_font.ttf_buf = NULL;
    KITTY_font.ready = false;
    KITTY_font.tried = false;
}

static Uint32 KITTY_NextUTF8(const char *s, size_t len, size_t *pos)
{
    const Uint8 c0 = (Uint8)s[*pos];
    if (c0 < 0x80u) {
        (*pos)++;
        return c0;
    }
    if ((c0 & 0xE0u) == 0xC0u && *pos + 1 < len) {
        Uint32 cp = ((Uint32)(c0 & 0x1Fu) << 6) | (Uint32)((Uint8)s[*pos + 1] & 0x3Fu);
        *pos += 2;
        return cp;
    }
    if ((c0 & 0xF0u) == 0xE0u && *pos + 2 < len) {
        Uint32 cp = ((Uint32)(c0 & 0x0Fu) << 12) |
                    ((Uint32)((Uint8)s[*pos + 1] & 0x3Fu) << 6) |
                    (Uint32)((Uint8)s[*pos + 2] & 0x3Fu);
        *pos += 3;
        return cp;
    }
    if ((c0 & 0xF8u) == 0xF0u && *pos + 3 < len) {
        Uint32 cp = ((Uint32)(c0 & 0x07u) << 18) |
                    ((Uint32)((Uint8)s[*pos + 1] & 0x3Fu) << 12) |
                    ((Uint32)((Uint8)s[*pos + 2] & 0x3Fu) << 6) |
                    (Uint32)((Uint8)s[*pos + 3] & 0x3Fu);
        *pos += 4;
        return cp;
    }
    (*pos)++;
    return '?';
}

static const KITTY_GlyphCacheEntry *KITTY_GetGlyph(Uint32 cp, float px_size)
{
    const int px_q = (int)(px_size * 2.0f + 0.5f);
    const Uint32 hash = (cp * 2654435761u) ^ ((Uint32)px_q * 40503u);
    const Uint32 slot = hash & (KITTY_GLYPH_CACHE_CAP - 1u);
    KITTY_GlyphCacheEntry *e = &KITTY_font.cache[slot];
    float scale;
    int adv = 0, lsb = 0;

    if (e->valid && e->codepoint == cp && e->px_q == px_q) {
        return e;
    }
    if (e->bitmap) {
        SDL_free(e->bitmap);
        e->bitmap = NULL;
    }
    scale = stbtt_ScaleForPixelHeight(&KITTY_font.info, (float)px_q * 0.5f);
    stbtt_GetCodepointHMetrics(&KITTY_font.info, (int)cp, &adv, &lsb);
    e->codepoint = cp;
    e->px_q = px_q;
    e->advance = (float)adv * scale;
    e->w = 0;
    e->h = 0;
    e->xoff = 0;
    e->yoff = 0;
    if (cp != ' ') {
        e->bitmap = stbtt_GetCodepointBitmap(&KITTY_font.info, scale, scale, (int)cp,
                                             &e->w, &e->h, &e->xoff, &e->yoff);
    }
    e->valid = true;
    return e;
}

static float KITTY_DigitAdvance(float px_size)
{
    const KITTY_GlyphCacheEntry *g = KITTY_GetGlyph('0', px_size);
    return g ? g->advance : px_size * 0.6f;
}

float KITTY_FontMeasureUTF8(const char *utf8, size_t len, float px_size, bool tabular_digits)
{
    size_t pos = 0;
    float x = 0.0f;
    float scale;
    Uint32 prev_cp = 0;
    float digit_adv = 0.0f;

    if (!utf8 || len == 0 || !KITTY_FontEnsureInit()) {
        return 0.0f;
    }
    scale = stbtt_ScaleForPixelHeight(&KITTY_font.info, px_size);
    if (tabular_digits) {
        digit_adv = KITTY_DigitAdvance(px_size);
    }
    while (pos < len) {
        const Uint32 cp = KITTY_NextUTF8(utf8, len, &pos);
        const KITTY_GlyphCacheEntry *g = KITTY_GetGlyph(cp, px_size);
        const bool is_dig = tabular_digits && (cp >= '0' && cp <= '9');
        if (prev_cp != 0 && !is_dig) {
            x += (float)stbtt_GetCodepointKernAdvance(&KITTY_font.info, (int)prev_cp, (int)cp) * scale;
        }
        x += is_dig ? digit_adv : (g ? g->advance : px_size * 0.5f);
        prev_cp = is_dig ? 0u : cp;
    }
    return x;
}

void KITTY_CanvasBlendPixel(KITTY_Canvas *c, int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    Uint8 *p;
    Uint32 sa, da, out_a;

    if (!c || !c->rgba || x < 0 || y < 0 || x >= c->w || y >= c->h || a == 0) {
        return;
    }
    p = c->rgba + ((size_t)y * (size_t)c->w + (size_t)x) * 4u;
    if (a == 255u || p[3] == 0) {
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = a;
        return;
    }
    sa = a;
    da = p[3];
    out_a = sa + (da * (255u - sa) + 127u) / 255u;
    if (out_a == 0) {
        return;
    }
    {
        const Uint32 da_inv = (da * (255u - sa) + 127u) / 255u;
        p[0] = (Uint8)((r * sa + p[0] * da_inv + out_a / 2u) / out_a);
        p[1] = (Uint8)((g * sa + p[1] * da_inv + out_a / 2u) / out_a);
        p[2] = (Uint8)((b * sa + p[2] * da_inv + out_a / 2u) / out_a);
        p[3] = (Uint8)(out_a > 255u ? 255u : out_a);
    }
}

static void KITTY_BlitGlyph(KITTY_Canvas *c, const KITTY_GlyphCacheEntry *g,
                            int gx, int gy, const Uint8 col[4])
{
    int y, x;
    if (!g || !g->bitmap || g->w <= 0 || g->h <= 0 || col[3] == 0) {
        return;
    }
    for (y = 0; y < g->h; ++y) {
        const int dy = gy + y;
        if (dy < 0 || dy >= c->h) {
            continue;
        }
        for (x = 0; x < g->w; ++x) {
            const int dx = gx + x;
            const Uint8 cov = g->bitmap[y * g->w + x];
            if (cov == 0 || dx < 0 || dx >= c->w) {
                continue;
            }
            const Uint8 a = (Uint8)(((Uint32)cov * (Uint32)col[3] + 127u) / 255u);
            KITTY_CanvasBlendPixel(c, dx, dy, col[0], col[1], col[2], a);
        }
    }
}

float KITTY_FontDrawUTF8(KITTY_Canvas *c, float x, float baseline_y,
                         const char *utf8, size_t len, float px_size,
                         const Uint8 color[4], bool shadow, bool tabular_digits)
{
    size_t pos = 0;
    float cur_x = x;
    float scale;
    Uint32 prev_cp = 0;
    float digit_adv = 0.0f;
    const Uint8 sh_col[4] = { 0, 0, 0, (Uint8)((Uint32)color[3] * 145u / 255u) };

    if (!c || !utf8 || len == 0 || !KITTY_FontEnsureInit()) {
        return x;
    }
    scale = stbtt_ScaleForPixelHeight(&KITTY_font.info, px_size);
    if (tabular_digits) {
        digit_adv = KITTY_DigitAdvance(px_size);
    }
    while (pos < len) {
        const Uint32 cp = KITTY_NextUTF8(utf8, len, &pos);
        const KITTY_GlyphCacheEntry *g = KITTY_GetGlyph(cp, px_size);
        const bool is_dig = tabular_digits && (cp >= '0' && cp <= '9');
        float draw_x;
        if (prev_cp != 0 && !is_dig) {
            cur_x += (float)stbtt_GetCodepointKernAdvance(&KITTY_font.info, (int)prev_cp, (int)cp) * scale;
        }
        draw_x = cur_x;
        if (is_dig && g) {
            draw_x += (digit_adv - g->advance) * 0.5f;
        }
        if (g && g->bitmap) {
            const int gx = (int)SDL_floorf(draw_x + (float)g->xoff + 0.5f);
            const int gy = (int)SDL_floorf(baseline_y + (float)g->yoff + 0.5f);
            if (shadow) {
                KITTY_BlitGlyph(c, g, gx, gy + 1, sh_col);
            }
            KITTY_BlitGlyph(c, g, gx, gy, color);
        }
        cur_x += is_dig ? digit_adv : (g ? g->advance : px_size * 0.5f);
        prev_cp = is_dig ? 0u : cp;
    }
    return cur_x;
}

/* Signed distance from point (px, py) to axis-aligned rounded rect [x, x+w) x [y, y+h). */
static inline float KITTY_RoundedRectSDF(float px, float py,
                                         float x, float y, float w, float h, float r)
{
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hx = w * 0.5f - r;
    const float hy = h * 0.5f - r;
    const float qx = SDL_fabsf(px - cx) - hx;
    const float qy = SDL_fabsf(py - cy) - hy;
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float m = (qx > qy) ? qx : qy;
    const float inside = m < 0.0f ? m : 0.0f;
    return SDL_sqrtf(ax * ax + ay * ay) + inside - r;
}

void KITTY_CanvasDrawRoundedCard(KITTY_Canvas *c, int x, int y, int w, int h, float radius,
                                 const Uint8 bg_top[4], const Uint8 bg_bot[4],
                                 const Uint8 border[4], int shadow_pad, Uint8 sheen_alpha)
{
    int py, px;
    const float rx = (float)x;
    const float ry = (float)y;
    const float rw = (float)w;
    const float rh = (float)h;
    const float r = (radius > rh * 0.5f) ? rh * 0.5f : radius;
    const int x0 = SDL_max(0, x - shadow_pad);
    const int y0 = SDL_max(0, y - shadow_pad);
    const int x1 = SDL_min(c->w, x + w + shadow_pad);
    const int y1 = SDL_min(c->h, y + h + shadow_pad + 3);

    if (!c || !c->rgba || w <= 2 || h <= 2) {
        return;
    }

    for (py = y0; py < y1; ++py) {
        const float fy = (float)py + 0.5f;
        float t = (fy - ry) / (rh > 1.0f ? rh : 1.0f);
        Uint8 gr, gg, gb, ga;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        gr = (Uint8)((float)bg_top[0] + ((float)bg_bot[0] - (float)bg_top[0]) * t + 0.5f);
        gg = (Uint8)((float)bg_top[1] + ((float)bg_bot[1] - (float)bg_top[1]) * t + 0.5f);
        gb = (Uint8)((float)bg_top[2] + ((float)bg_bot[2] - (float)bg_top[2]) * t + 0.5f);
        ga = (Uint8)((float)bg_top[3] + ((float)bg_bot[3] - (float)bg_top[3]) * t + 0.5f);

        for (px = x0; px < x1; ++px) {
            const float fx = (float)px + 0.5f;
            const float sdf = KITTY_RoundedRectSDF(fx, fy, rx, ry, rw, rh, r);

            /* Soft outer drop shadow slightly offset downward */
            if (shadow_pad > 0 && sdf > -0.5f) {
                const float ssdf = KITTY_RoundedRectSDF(fx, fy - 2.0f, rx, ry, rw, rh, r);
                if (ssdf >= 0.0f && ssdf < (float)shadow_pad) {
                    const float falloff = 1.0f - (ssdf / (float)shadow_pad);
                    const Uint8 sa = (Uint8)(falloff * falloff * 115.0f);
                    if (sa > 0) {
                        KITTY_CanvasBlendPixel(c, px, py, 2, 4, 10, sa);
                    }
                }
            }

            if (sdf > 0.75f) {
                continue;
            }
            {
                /* Outer anti-aliased coverage */
                float cov = 0.5f - sdf;
                if (cov < 0.0f) cov = 0.0f;
                if (cov > 1.0f) cov = 1.0f;

                /* Fill interior */
                {
                    const Uint8 fa = (Uint8)((float)ga * cov + 0.5f);
                    KITTY_CanvasBlendPixel(c, px, py, gr, gg, gb, fa);
                }

                /* Subtle top specular inner sheen along top edge (1.2px inside border) */
                if (sheen_alpha > 0 && py >= y + 1 && py <= y + 2 && sdf < -1.0f) {
                    const float st = (py == y + 1) ? 1.0f : 0.35f;
                    const Uint8 sha = (Uint8)((float)sheen_alpha * st * cov);
                    KITTY_CanvasBlendPixel(c, px, py, 255, 255, 255, sha);
                }

                /* Crisp 1.15px SDF border ring */
                if (border && border[3] > 0 && sdf > -1.35f) {
                    float bcov = cov;
                    const float inner = sdf + 1.15f;
                    if (inner < 0.5f) {
                        float ic = inner + 0.5f;
                        if (ic < 0.0f) ic = 0.0f;
                        if (ic < bcov) bcov = ic;
                    }
                    if (bcov > 0.0f) {
                        const Uint8 ba = (Uint8)((float)border[3] * bcov + 0.5f);
                        KITTY_CanvasBlendPixel(c, px, py, border[0], border[1], border[2], ba);
                    }
                }
            }
        }
    }
}

void KITTY_CanvasDrawPill(KITTY_Canvas *c, int x, int y, int w, int h, float radius,
                          const Uint8 fill[4], const Uint8 border[4])
{
    KITTY_CanvasDrawRoundedCard(c, x, y, w, h, radius, fill, fill, border, 0, 0);
}

void KITTY_CanvasDrawGlowDot(KITTY_Canvas *c, float cx, float cy, float core_r, float glow_r,
                             const Uint8 color[4])
{
    int x0, y0, x1, y1, py, px;
    if (!c || !c->rgba || glow_r <= 0.0f) {
        return;
    }
    x0 = SDL_max(0, (int)SDL_floorf(cx - glow_r - 1.0f));
    y0 = SDL_max(0, (int)SDL_floorf(cy - glow_r - 1.0f));
    x1 = SDL_min(c->w, (int)SDL_ceilf(cx + glow_r + 1.0f));
    y1 = SDL_min(c->h, (int)SDL_ceilf(cy + glow_r + 1.0f));
    for (py = y0; py < y1; ++py) {
        const float dy = ((float)py + 0.5f) - cy;
        for (px = x0; px < x1; ++px) {
            const float dx = ((float)px + 0.5f) - cx;
            const float d = SDL_sqrtf(dx * dx + dy * dy);
            if (d > glow_r) {
                continue;
            }
            if (d <= core_r + 0.5f) {
                float cov = (core_r + 0.5f) - d;
                if (cov > 1.0f) cov = 1.0f;
                /* Soft white specular core center */
                const float hi = (d < core_r * 0.55f) ? 0.35f : 0.0f;
                const Uint8 r = (Uint8)(color[0] + (255 - color[0]) * hi);
                const Uint8 g = (Uint8)(color[1] + (255 - color[1]) * hi);
                const Uint8 b = (Uint8)(color[2] + (255 - color[2]) * hi);
                KITTY_CanvasBlendPixel(c, px, py, r, g, b, (Uint8)((float)color[3] * cov));
            } else {
                const float t = 1.0f - (d - core_r) / (glow_r - core_r);
                const Uint8 ga = (Uint8)(t * t * (float)color[3] * 0.42f);
                if (ga > 0) {
                    KITTY_CanvasBlendPixel(c, px, py, color[0], color[1], color[2], ga);
                }
            }
        }
    }
}

void KITTY_CanvasDrawProgressBar(KITTY_Canvas *c, int x, int y, int w, int h, float t,
                                 const Uint8 track[4], const Uint8 fill_l[4], const Uint8 fill_r[4])
{
    const float r = (float)h * 0.5f;
    int px, py;
    float fill_w;

    if (!c || !c->rgba || w <= 2 || h <= 1) {
        return;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    KITTY_CanvasDrawPill(c, x, y, w, h, r, track, NULL);
    fill_w = (float)w * t;
    if (fill_w < 1.5f) {
        return;
    }
    for (py = SDL_max(0, y); py < SDL_min(c->h, y + h); ++py) {
        const float fy = (float)py + 0.5f;
        for (px = SDL_max(0, x); px < SDL_min(c->w, x + (int)SDL_ceilf(fill_w) + 1); ++px) {
            const float fx = (float)px + 0.5f;
            const float sdf_track = KITTY_RoundedRectSDF(fx, fy, (float)x, (float)y, (float)w, (float)h, r);
            const float sdf_fill = KITTY_RoundedRectSDF(fx, fy, (float)x, (float)y, fill_w, (float)h,
                                                        fill_w * 0.5f < r ? fill_w * 0.5f : r);
            const float sdf = (sdf_track > sdf_fill) ? sdf_track : sdf_fill;
            if (sdf < 0.5f) {
                float cov = 0.5f - sdf;
                float u = (fx - (float)x) / (float)w;
                Uint8 cr, cg, cb, ca;
                if (cov > 1.0f) cov = 1.0f;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                cr = (Uint8)((float)fill_l[0] + ((float)fill_r[0] - (float)fill_l[0]) * u);
                cg = (Uint8)((float)fill_l[1] + ((float)fill_r[1] - (float)fill_l[1]) * u);
                cb = (Uint8)((float)fill_l[2] + ((float)fill_r[2] - (float)fill_l[2]) * u);
                ca = (Uint8)((float)fill_l[3] * cov);
                KITTY_CanvasBlendPixel(c, px, py, cr, cg, cb, ca);
            }
        }
    }
}

static void KITTY_DrawAALine(KITTY_Canvas *c, float x0, float y0, float x1, float y1,
                             float thickness, const Uint8 col[4])
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float half_t = thickness * 0.5f;
    const int minx = SDL_max(0, (int)SDL_floorf(SDL_min(x0, x1) - half_t - 1.0f));
    const int maxx = SDL_min(c->w - 1, (int)SDL_ceilf(SDL_max(x0, x1) + half_t + 1.0f));
    const int miny = SDL_max(0, (int)SDL_floorf(SDL_min(y0, y1) - half_t - 1.0f));
    const int maxy = SDL_min(c->h - 1, (int)SDL_ceilf(SDL_max(y0, y1) + half_t + 1.0f));
    int py, px;

    for (py = miny; py <= maxy; ++py) {
        const float fy = (float)py + 0.5f;
        for (px = minx; px <= maxx; ++px) {
            const float fx = (float)px + 0.5f;
            float t = (len2 > 1e-6f) ? (((fx - x0) * dx + (fy - y0) * dy) / len2) : 0.0f;
            float cx, cy, dist, cov;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            cx = x0 + t * dx;
            cy = y0 + t * dy;
            dist = SDL_sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
            cov = (half_t + 0.65f) - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                KITTY_CanvasBlendPixel(c, px, py, col[0], col[1], col[2],
                                       (Uint8)((float)col[3] * cov));
            }
        }
    }
}

void KITTY_CanvasDrawSparkline(KITTY_Canvas *c, int x, int y, int w, int h,
                               const float *samples, int count, float max_val,
                               const Uint8 line_col[4], const Uint8 fill_top[4],
                               const Uint8 fill_bot[4], bool draw_tip)
{
    int i, px, py;
    float prev_x = 0.0f, prev_y = 0.0f;
    float last_x = 0.0f, last_y = 0.0f;

    if (!c || !c->rgba || !samples || count < 2 || w <= 4 || h <= 4 || max_val <= 0.0f) {
        return;
    }

    /* 1) Translucent vertical gradient area fill underneath curve */
    if (fill_top && fill_top[3] > 0) {
        for (px = 0; px < w; ++px) {
            const float fidx = ((float)px / (float)(w - 1)) * (float)(count - 1);
            int i0 = (int)fidx;
            int i1 = (i0 + 1 < count) ? (i0 + 1) : (count - 1);
            const float frac = fidx - (float)i0;
            float v = samples[i0] + (samples[i1] - samples[i0]) * frac;
            float norm, top_y;
            if (v < 0.0f) v = 0.0f;
            norm = v / max_val;
            if (norm > 1.0f) norm = 1.0f;
            top_y = (float)(y + h - 1) - norm * (float)(h - 3);
            for (py = (int)SDL_ceilf(top_y); py < y + h; ++py) {
                float gt = ((float)py - top_y) / (float)h;
                Uint8 r, g, b, a;
                if (gt < 0.0f) gt = 0.0f;
                if (gt > 1.0f) gt = 1.0f;
                r = (Uint8)((float)fill_top[0] + ((float)fill_bot[0] - (float)fill_top[0]) * gt);
                g = (Uint8)((float)fill_top[1] + ((float)fill_bot[1] - (float)fill_top[1]) * gt);
                b = (Uint8)((float)fill_top[2] + ((float)fill_bot[2] - (float)fill_top[2]) * gt);
                a = (Uint8)((float)fill_top[3] + ((float)fill_bot[3] - (float)fill_top[3]) * gt);
                KITTY_CanvasBlendPixel(c, x + px, py, r, g, b, a);
            }
        }
    }

    /* 2) Smooth anti-aliased polyline */
    for (i = 0; i < count; ++i) {
        float v = samples[i];
        float norm, sx, sy;
        if (v < 0.0f) v = 0.0f;
        norm = v / max_val;
        if (norm > 1.0f) norm = 1.0f;
        sx = (float)x + ((float)i / (float)(count - 1)) * (float)(w - 1);
        sy = (float)(y + h - 2) - norm * (float)(h - 4);
        if (i > 0) {
            KITTY_DrawAALine(c, prev_x, prev_y, sx, sy, 1.55f, line_col);
        }
        prev_x = sx;
        prev_y = sy;
        last_x = sx;
        last_y = sy;
    }

    /* 3) Bright glowing live tip dot */
    if (draw_tip) {
        KITTY_CanvasDrawGlowDot(c, last_x, last_y, 2.3f, 7.0f, line_col);
    }
}

#endif /* SDL_VIDEO_DRIVER_KITTY */
