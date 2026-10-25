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
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifdef KITTY_RECT_STANDALONE
#include <SDL3/SDL.h>
#else
#include "../SDL_surface_c.h"
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_RECT_STANDALONE)

#include "SDL_kittyrect.h"

bool KITTY_UnionRects(int fb_w, int fb_h, const SDL_Rect *rects, int numrects, SDL_Rect *out)
{
    SDL_Rect fb;
    SDL_Rect span;
    int i;
    bool any = false;

    if (!out || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    fb.x = 0;
    fb.y = 0;
    fb.w = fb_w;
    fb.h = fb_h;
    if (!rects || numrects <= 0) {
        *out = fb;
        return true;
    }

    span.x = 0;
    span.y = 0;
    span.w = 0;
    span.h = 0;

    for (i = 0; i < numrects; ++i) {
        SDL_Rect clipped;
        if (!SDL_GetRectIntersection(&rects[i], &fb, &clipped)) {
            continue;
        }
        if (clipped.w <= 0 || clipped.h <= 0) {
            continue;
        }
        if (!any) {
            span = clipped;
            any = true;
        } else {
            const int x2 = SDL_max(span.x + span.w, clipped.x + clipped.w);
            const int y2 = SDL_max(span.y + span.h, clipped.y + clipped.h);
            span.x = SDL_min(span.x, clipped.x);
            span.y = SDL_min(span.y, clipped.y);
            span.w = x2 - span.x;
            span.h = y2 - span.y;
        }
    }

    if (!any) {
        out->x = out->y = out->w = out->h = 0;
        return false;
    }
    *out = span;
    return true;
}

float KITTY_CoverageRatio(int fb_w, int fb_h, const SDL_Rect *union_rect)
{
    const float denom = (float)fb_w * (float)fb_h;
    float area;
    if (fb_w <= 0 || fb_h <= 0 || denom <= 0.0f) {
        return 1.0f;
    }
    if (!union_rect || union_rect->w <= 0 || union_rect->h <= 0) {
        return 0.0f;
    }
    area = (float)union_rect->w * (float)union_rect->h;
    if (area >= denom) {
        return 1.0f;
    }
    return area / denom;
}

bool KITTY_FbRectToPlaceCells(int fb_w, int fb_h, const SDL_Rect *fb_rect,
                              int cols, int rows,
                              int *out_col0, int *out_row0,
                              int *out_place_cols, int *out_place_rows)
{
    int x0, y0, x1, y1;
    int col0, row0, col1, row1;

    if (!fb_rect || !out_col0 || !out_row0 || !out_place_cols || !out_place_rows ||
        fb_w <= 0 || fb_h <= 0 || cols <= 0 || rows <= 0 ||
        fb_rect->w <= 0 || fb_rect->h <= 0) {
        return false;
    }

    x0 = fb_rect->x;
    y0 = fb_rect->y;
    x1 = fb_rect->x + fb_rect->w;
    y1 = fb_rect->y + fb_rect->h;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > fb_w) {
        x1 = fb_w;
    }
    if (y1 > fb_h) {
        y1 = fb_h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    col0 = (x0 * cols) / fb_w;
    row0 = (y0 * rows) / fb_h;
    col1 = (x1 * cols + fb_w - 1) / fb_w;
    row1 = (y1 * rows + fb_h - 1) / fb_h;
    if (col1 > cols) {
        col1 = cols;
    }
    if (row1 > rows) {
        row1 = rows;
    }
    if (col1 <= col0) {
        col1 = col0 + 1;
    }
    if (row1 <= row0) {
        row1 = row0 + 1;
    }
    if (col0 >= cols) {
        col0 = cols - 1;
        col1 = cols;
    }
    if (row0 >= rows) {
        row0 = rows - 1;
        row1 = rows;
    }

    *out_col0 = col0;
    *out_row0 = row0;
    *out_place_cols = col1 - col0;
    *out_place_rows = row1 - row0;
    return *out_place_cols > 0 && *out_place_rows > 0;
}

bool KITTY_FbPixelToCellCursor(int x, int y, int cell_w, int cell_h,
                               int *out_col0, int *out_row0,
                               int *out_X, int *out_Y)
{
    if (!out_col0 || !out_row0 || !out_X || !out_Y || cell_w <= 0 || cell_h <= 0 ||
        x < 0 || y < 0) {
        return false;
    }
    *out_col0 = x / cell_w;
    *out_row0 = y / cell_h;
    *out_X = x % cell_w;
    *out_Y = y % cell_h;
    return true;
}

Uint64 KITTY_Fnv1a64(const Uint8 *data, size_t len)
{
    Uint64 h = 14695981039346656037ULL;
    size_t i;
    if (!data) {
        return h;
    }
    for (i = 0; i < len; ++i) {
        h ^= (Uint64)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool KITTY_PlaceCellsTouchesEdge(int col0, int row0, int place_cols, int place_rows,
                                 int cols, int rows)
{
    if (cols <= 0 || rows <= 0 || place_cols <= 0 || place_rows <= 0) {
        return true;
    }
    if (col0 <= 0 || row0 <= 0) {
        return true;
    }
    if (col0 + place_cols >= cols || row0 + place_rows >= rows) {
        return true;
    }
    return false;
}

int KITTY_CoalesceRects(int fb_w, int fb_h, const SDL_Rect *in, int n_in,
                        int gap_px, SDL_Rect *out, int max_out)
{
    SDL_Rect tmp[64];
    int n = 0;
    int i;
    bool merged;

    if (!in || !out || max_out <= 0 || fb_w <= 0 || fb_h <= 0 || n_in <= 0) {
        return 0;
    }
    if (gap_px < 0) {
        gap_px = 0;
    }

    for (i = 0; i < n_in && n < (int)(sizeof(tmp) / sizeof(tmp[0])); ++i) {
        SDL_Rect fb = { 0, 0, fb_w, fb_h };
        SDL_Rect clipped;
        if (!SDL_GetRectIntersection(&in[i], &fb, &clipped) ||
            clipped.w <= 0 || clipped.h <= 0) {
            continue;
        }
        tmp[n++] = clipped;
    }
    if (n == 0) {
        return 0;
    }

    /* Iteratively union near/overlapping pairs until stable. */
    do {
        merged = false;
        for (i = 0; i < n; ++i) {
            int j;
            for (j = i + 1; j < n; ++j) {
                SDL_Rect expanded = {
                    tmp[i].x - gap_px,
                    tmp[i].y - gap_px,
                    tmp[i].w + 2 * gap_px,
                    tmp[i].h + 2 * gap_px
                };
                if (SDL_HasRectIntersection(&expanded, &tmp[j])) {
                    const int x0 = tmp[i].x < tmp[j].x ? tmp[i].x : tmp[j].x;
                    const int y0 = tmp[i].y < tmp[j].y ? tmp[i].y : tmp[j].y;
                    const int x1 = (tmp[i].x + tmp[i].w) > (tmp[j].x + tmp[j].w)
                                       ? (tmp[i].x + tmp[i].w)
                                       : (tmp[j].x + tmp[j].w);
                    const int y1 = (tmp[i].y + tmp[i].h) > (tmp[j].y + tmp[j].h)
                                       ? (tmp[i].y + tmp[i].h)
                                       : (tmp[j].y + tmp[j].h);
                    tmp[i].x = x0;
                    tmp[i].y = y0;
                    tmp[i].w = x1 - x0;
                    tmp[i].h = y1 - y0;
                    tmp[j] = tmp[n - 1];
                    n--;
                    merged = true;
                    break;
                }
            }
            if (merged) {
                break;
            }
        }
    } while (merged && n > 1);

    if (n > max_out) {
        /* Too many — fall back to single union of all remaining. */
        SDL_Rect u = tmp[0];
        for (i = 1; i < n; ++i) {
            const int x0 = u.x < tmp[i].x ? u.x : tmp[i].x;
            const int y0 = u.y < tmp[i].y ? u.y : tmp[i].y;
            const int x1 = (u.x + u.w) > (tmp[i].x + tmp[i].w) ? (u.x + u.w) : (tmp[i].x + tmp[i].w);
            const int y1 = (u.y + u.h) > (tmp[i].y + tmp[i].h) ? (u.y + u.h) : (tmp[i].y + tmp[i].h);
            u.x = x0;
            u.y = y0;
            u.w = x1 - x0;
            u.h = y1 - y0;
        }
        out[0] = u;
        return 1;
    }

    for (i = 0; i < n; ++i) {
        out[i] = tmp[i];
    }
    return n;
}

bool KITTY_SelectFbDiff(const char *env, bool default_on)
{
    if (!env || !env[0]) {
        return default_on;
    }
    if (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
        SDL_strcasecmp(env, "false") == 0 || SDL_strcasecmp(env, "no") == 0) {
        return false;
    }
    return true;
}

int KITTY_SelectFbDiffTile(const char *env, int default_tile)
{
    int tile = default_tile > 0 ? default_tile : 32;
    if (env && env[0]) {
        tile = (int)SDL_strtol(env, NULL, 10);
    }
    if (tile < 8) {
        tile = 8;
    }
    if (tile > 128) {
        tile = 128;
    }
    return tile;
}

float KITTY_SelectFbDiffCover(const char *env, float default_cover)
{
    float cover = default_cover > 0.0f ? default_cover : 0.45f;
    if (env && env[0]) {
        cover = (float)SDL_atof(env);
    }
    if (cover < 0.05f) {
        cover = 0.05f;
    }
    if (cover > 1.0f) {
        cover = 1.0f;
    }
    return cover;
}

KITTY_DiffOutcome KITTY_TileDiffBBox(const Uint8 *prev, const Uint8 *next,
                                     int fb_w, int fb_h, int bpp, int tile,
                                     float cover_limit, SDL_Rect *dirty_out)
{
    int ty, tx;
    int min_x = fb_w, min_y = fb_h, max_x = 0, max_y = 0;
    int dirty_tiles = 0;
    SDL_Rect bbox;

    if (dirty_out) {
        dirty_out->x = 0;
        dirty_out->y = 0;
        dirty_out->w = fb_w;
        dirty_out->h = fb_h;
    }
    if (!prev || !next || fb_w <= 0 || fb_h <= 0 || (bpp != 3 && bpp != 4) || tile <= 0) {
        return KITTY_DIFF_FULL;
    }

    for (ty = 0; ty < fb_h; ty += tile) {
        const int th = (ty + tile <= fb_h) ? tile : (fb_h - ty);
        for (tx = 0; tx < fb_w; tx += tile) {
            const int tw = (tx + tile <= fb_w) ? tile : (fb_w - tx);
            int row;
            bool changed = false;
            for (row = 0; row < th; ++row) {
                const size_t off = ((size_t)(ty + row) * (size_t)fb_w + (size_t)tx) * (size_t)bpp;
                if (SDL_memcmp(prev + off, next + off, (size_t)tw * (size_t)bpp) != 0) {
                    changed = true;
                    break;
                }
            }
            if (changed) {
                dirty_tiles++;
                if (tx < min_x) {
                    min_x = tx;
                }
                if (ty < min_y) {
                    min_y = ty;
                }
                if (tx + tw > max_x) {
                    max_x = tx + tw;
                }
                if (ty + th > max_y) {
                    max_y = ty + th;
                }
            }
        }
    }

    if (dirty_tiles == 0) {
        if (dirty_out) {
            dirty_out->x = dirty_out->y = dirty_out->w = dirty_out->h = 0;
        }
        return KITTY_DIFF_NONE;
    }

    bbox.x = min_x;
    bbox.y = min_y;
    bbox.w = max_x - min_x;
    bbox.h = max_y - min_y;
    if (dirty_out) {
        *dirty_out = bbox;
    }
    if (KITTY_CoverageRatio(fb_w, fb_h, &bbox) >= cover_limit) {
        return KITTY_DIFF_FULL;
    }
    return KITTY_DIFF_RECT;
}

KITTY_PresentConvertMode KITTY_ChooseConvertMode(int fb_w, int fb_h,
                                                 const SDL_Rect *rects, int numrects,
                                                 bool scratch_valid, SDL_Rect *dirty_out)
{
    const char *env;
    SDL_Rect union_rect;

    if (dirty_out) {
        dirty_out->x = 0;
        dirty_out->y = 0;
        dirty_out->w = fb_w;
        dirty_out->h = fb_h;
    }

    env = SDL_getenv("SDL_KITTY_DIRTY");
    if (env && (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
                SDL_strcasecmp(env, "false") == 0 || SDL_strcasecmp(env, "no") == 0)) {
        return KITTY_CONVERT_FULL;
    }

    /* First present / resize (scratch rebuilt) → full convert + a=T. */
    if (!scratch_valid || fb_w <= 0 || fb_h <= 0) {
        return KITTY_CONVERT_FULL;
    }

    if (!KITTY_UnionRects(fb_w, fb_h, rects, numrects, &union_rect)) {
        return KITTY_CONVERT_FULL;
    }

    /* Stay dirty for any union (including large / full-frame) so a=f can
     * edit in place. Full a=T only when scratch invalid or a=f fails. */
    if (dirty_out) {
        *dirty_out = union_rect;
    }
    return KITTY_CONVERT_DIRTY;
}

bool KITTY_PackRGBRect(const Uint8 *rgb_scratch, int fb_w, int fb_h,
                       const SDL_Rect *rect, Uint8 *out, size_t out_cap, size_t *out_len)
{
    SDL_Rect fb, area;
    int y;
    size_t need;
    size_t off = 0;

    if (!rgb_scratch || !rect || !out || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    fb.x = 0;
    fb.y = 0;
    fb.w = fb_w;
    fb.h = fb_h;
    if (!SDL_GetRectIntersection(rect, &fb, &area) || area.w <= 0 || area.h <= 0) {
        return false;
    }
    need = (size_t)area.w * (size_t)area.h * 3u;
    if (need > out_cap) {
        return false;
    }
    for (y = area.y; y < area.y + area.h; ++y) {
        const Uint8 *src = rgb_scratch + ((size_t)y * (size_t)fb_w + (size_t)area.x) * 3u;
        SDL_memcpy(out + off, src, (size_t)area.w * 3u);
        off += (size_t)area.w * 3u;
    }
    if (out_len) {
        *out_len = off;
    }
    return true;
}

bool KITTY_ConvertSurfaceRectToRGB(SDL_Surface *surface, const SDL_Rect *rect, Uint8 *rgb_scratch)
{
    SDL_Rect area;
    SDL_Rect fb;
    int x, y;

    if (!surface || !rgb_scratch || !rect) {
        return false;
    }
    if (surface->w <= 0 || surface->h <= 0) {
        return false;
    }

    fb.x = 0;
    fb.y = 0;
    fb.w = surface->w;
    fb.h = surface->h;
    if (!SDL_GetRectIntersection(rect, &fb, &area) || area.w <= 0 || area.h <= 0) {
        return false;
    }

    for (y = area.y; y < area.y + area.h; ++y) {
        const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch);
        Uint8 *dst = rgb_scratch + ((size_t)y * (size_t)surface->w + (size_t)area.x) * 3u;
        for (x = area.x; x < area.x + area.w; ++x) {
            const Uint32 p = src[x];
            *dst++ = (Uint8)((p >> 16) & 0xFF);
            *dst++ = (Uint8)((p >> 8) & 0xFF);
            *dst++ = (Uint8)(p & 0xFF);
        }
    }
    return true;
}

bool KITTY_ConvertSurfaceRectToRGBA(SDL_Surface *surface, const SDL_Rect *rect, Uint8 *rgba_scratch)
{
    SDL_Rect area;
    SDL_Rect fb;
    int x, y;

    if (!surface || !rgba_scratch || !rect) {
        return false;
    }
    if (surface->w <= 0 || surface->h <= 0) {
        return false;
    }

    fb.x = 0;
    fb.y = 0;
    fb.w = surface->w;
    fb.h = surface->h;
    if (!SDL_GetRectIntersection(rect, &fb, &area) || area.w <= 0 || area.h <= 0) {
        return false;
    }

    /* ARGB8888 packed: A in top byte, then R,G,B. Kitty f=32 wants R,G,B,A bytes. */
    for (y = area.y; y < area.y + area.h; ++y) {
        const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch);
        Uint8 *dst = rgba_scratch + ((size_t)y * (size_t)surface->w + (size_t)area.x) * 4u;
        for (x = area.x; x < area.x + area.w; ++x) {
            const Uint32 p = src[x];
            *dst++ = (Uint8)((p >> 16) & 0xFF); /* R */
            *dst++ = (Uint8)((p >> 8) & 0xFF);  /* G */
            *dst++ = (Uint8)(p & 0xFF);         /* B */
            *dst++ = (Uint8)((p >> 24) & 0xFF); /* A */
        }
    }
    return true;
}

bool KITTY_PackRGBARect(const Uint8 *rgba_scratch, int fb_w, int fb_h,
                        const SDL_Rect *rect, Uint8 *out, size_t out_cap, size_t *out_len)
{
    SDL_Rect fb, area;
    int y;
    size_t need;
    size_t off = 0;

    if (!rgba_scratch || !rect || !out || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    fb.x = 0;
    fb.y = 0;
    fb.w = fb_w;
    fb.h = fb_h;
    if (!SDL_GetRectIntersection(rect, &fb, &area) || area.w <= 0 || area.h <= 0) {
        return false;
    }
    need = (size_t)area.w * (size_t)area.h * 4u;
    if (need > out_cap) {
        return false;
    }
    for (y = area.y; y < area.y + area.h; ++y) {
        const Uint8 *src = rgba_scratch + ((size_t)y * (size_t)fb_w + (size_t)area.x) * 4u;
        SDL_memcpy(out + off, src, (size_t)area.w * 4u);
        off += (size_t)area.w * 4u;
    }
    if (out_len) {
        *out_len = off;
    }
    return true;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_RECT_STANDALONE */
