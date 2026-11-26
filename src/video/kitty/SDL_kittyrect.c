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

/*
 * Implementation of dirty-rect / present helpers: union and coalesce,
 * place_cells mapping, FNV-1a, tile-diff, convert-mode and FB-diff selectors,
 * and surface→RGB/RGBA convert + pack for Kitty APC payloads.
 */
#ifdef KITTY_RECT_STANDALONE
#include <SDL3/SDL.h>
#else
#include "../SDL_surface_c.h"
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_RECT_STANDALONE)

#include "SDL_kittyrect.h"

#if defined(__ARM_NEON) && (SDL_BYTEORDER == SDL_LIL_ENDIAN)
#include <arm_neon.h>
#define KITTY_HAVE_NEON_LE 1
#endif

/*
 * Clamp rects to the FB and compute their axis-aligned union into *out.
 * Empty/null rect list → full FB. Returns false if nothing remains in-bounds.
 */
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

/*
 * area(union_rect) / (fb_w*fb_h), clamped to [0,1]. Returns 1.0 if FB size
 * is invalid; 0.0 if union_rect is empty/NULL.
 */
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

/*
 * Map an FB pixel rect onto the cols×rows place_cells grid (ceil on the
 * far edges). Writes 0-based col/row and place span; false on bad args.
 */
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

/*
 * Map FB pixel (x,y) to 0-based cell indices and in-cell offsets for 1:1
 * place. Requires non-negative x/y and positive cell size.
 */
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

/*
 * FNV-1a 64-bit hash over len bytes. NULL data returns the offset basis.
 */
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

/*
 * Fast 64-bit 4-lane parallel word hash (32 bytes/iter) for upload_skip.
 * Breaks the 3-cycle serial multiply dependency of byte-wise FNV-1a.
 */
static inline Uint64 KITTY_Rotl64(Uint64 x, unsigned r)
{
    return (x << r) | (x >> (64u - r));
}

Uint64 KITTY_FastHash64(const Uint8 *data, size_t len)
{
    static const Uint64 P1 = 11400714785074694791ULL;
    static const Uint64 P2 = 14029467366897019727ULL;
    static const Uint64 P3 = 1609587929392839161ULL;
    static const Uint64 P4 = 9650029242287828579ULL;
    Uint64 h0 = P1 ^ (Uint64)len;
    Uint64 h1 = P2;
    Uint64 h2 = P3;
    Uint64 h3 = P4;
    size_t i = 0;

    if (!data) {
        return h0;
    }

    while (i + 32u <= len) {
        Uint64 w0, w1, w2, w3;
        SDL_memcpy(&w0, data + i, 8);
        SDL_memcpy(&w1, data + i + 8, 8);
        SDL_memcpy(&w2, data + i + 16, 8);
        SDL_memcpy(&w3, data + i + 24, 8);
        h0 = KITTY_Rotl64(h0 + w0 * P2, 31) * P1;
        h1 = KITTY_Rotl64(h1 + w1 * P2, 31) * P1;
        h2 = KITTY_Rotl64(h2 + w2 * P2, 31) * P1;
        h3 = KITTY_Rotl64(h3 + w3 * P2, 31) * P1;
        i += 32u;
    }

    {
        Uint64 h = KITTY_Rotl64(h0, 1) + KITTY_Rotl64(h1, 7) +
                   KITTY_Rotl64(h2, 12) + KITTY_Rotl64(h3, 18);
        while (i + 8u <= len) {
            Uint64 w;
            SDL_memcpy(&w, data + i, 8);
            h ^= KITTY_Rotl64(w * P2, 31) * P1;
            h = KITTY_Rotl64(h, 27) * P1 + P4;
            i += 8u;
        }
        while (i < len) {
            h ^= (Uint64)data[i] * P3;
            h = KITTY_Rotl64(h, 11) * P1;
            i++;
        }
        h ^= h >> 33;
        h *= P2;
        h ^= h >> 29;
        h *= P3;
        h ^= h >> 32;
        return h;
    }
}

/*
 * True if the place box touches any screen edge (or dims are non-positive).
 * Used by Exp 3 to skip edge-touching union overlays.
 */
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

bool KITTY_FitAspectCells(int cols, int rows, int cell_w, int cell_h, int fb_w, int fb_h,
                          int *out_col, int *out_row, int *out_cols, int *out_rows)
{
    double aspect;
    int c, r;

    if (cols <= 0 || rows <= 0 || cell_w <= 0 || cell_h <= 0 || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    aspect = (double)fb_w / (double)fb_h;
    /* Full width first; if that is too tall, full height. */
    c = cols;
    r = (int)((double)c * cell_w / aspect / cell_h + 0.5);
    if (r > rows) {
        r = rows;
        c = (int)((double)r * cell_h * aspect / cell_w + 0.5);
        if (c > cols) {
            c = cols;
        }
    }
    if (c < 1) {
        c = 1;
    }
    if (r < 1) {
        r = 1;
    }
    if (out_col) {
        *out_col = (cols - c) / 2 + 1;
    }
    if (out_row) {
        *out_row = (rows - r) / 2 + 1;
    }
    if (out_cols) {
        *out_cols = c;
    }
    if (out_rows) {
        *out_rows = r;
    }
    return true;
}

/*
 * Clip in[] to the FB, then iteratively union pairs whose gap_px-expanded
 * boxes intersect (tmp cap 64). If more than max_out remain, collapse to
 * one union. Returns written count (0 on bad args / empty).
 */
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

/*
 * SDL_KITTY_FB_DIFF: unset → default_on; "0"/off/false/no → false; else true.
 */
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

/*
 * SDL_KITTY_FB_DIFF_TILE: env atoi overrides default_tile (or 32); clamp
 * to [8, 128].
 */
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

/*
 * SDL_KITTY_FB_DIFF_COVER: env atof overrides default_cover (or 0.45);
 * clamp to [0.05, 1.0].
 */
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

/*
 * Fast streaming byte-difference finder: returns offset of first differing
 * byte in [0, len), or len if a[0..len) == b[0..len). Uses ARM64 NEON 64B/iter
 * when available, else 8B word compares.
 */
static size_t KITTY_FindFirstDiff(const Uint8 *a, const Uint8 *b, size_t len)
{
    size_t i = 0;
#ifdef KITTY_HAVE_NEON_LE
    while (i + 64u <= len) {
        const uint8x16_t d0 = veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i));
        const uint8x16_t d1 = veorq_u8(vld1q_u8(a + i + 16), vld1q_u8(b + i + 16));
        const uint8x16_t d2 = veorq_u8(vld1q_u8(a + i + 32), vld1q_u8(b + i + 32));
        const uint8x16_t d3 = veorq_u8(vld1q_u8(a + i + 48), vld1q_u8(b + i + 48));
        const uint8x16_t any = vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3));
        if (vmaxvq_u32(vreinterpretq_u32_u8(any)) != 0) {
            if (vmaxvq_u32(vreinterpretq_u32_u8(d0)) == 0) {
                i += 16u;
                if (vmaxvq_u32(vreinterpretq_u32_u8(d1)) == 0) {
                    i += 16u;
                    if (vmaxvq_u32(vreinterpretq_u32_u8(d2)) == 0) {
                        i += 16u;
                    }
                }
            }
            while (i < len && a[i] == b[i]) {
                i++;
            }
            return i;
        }
        i += 64u;
    }
    while (i + 16u <= len) {
        const uint8x16_t d = veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i));
        if (vmaxvq_u32(vreinterpretq_u32_u8(d)) != 0) {
            while (i < len && a[i] == b[i]) {
                i++;
            }
            return i;
        }
        i += 16u;
    }
#else
    while (i + 8u <= len) {
        Uint64 wa, wb;
        SDL_memcpy(&wa, a + i, 8);
        SDL_memcpy(&wb, b + i, 8);
        if (wa != wb) {
            while (i < len && a[i] == b[i]) {
                i++;
            }
            return i;
        }
        i += 8u;
    }
#endif
    while (i < len && a[i] == b[i]) {
        i++;
    }
    return i;
}

/*
 * Row-major cache-sequential scan of one horizontal band of tiles [ty, ty+th).
 * Marks tile_dirty[0..tiles_x-1] = 1 for changed tiles and returns count of
 * dirty tiles in the band. Exits early once all tiles in the band are dirty.
 */
static int KITTY_ScanTileRowBand(const Uint8 *prev, const Uint8 *next,
                                 int fb_w, int ty, int th, int bpp, int tile,
                                 int tiles_x, Uint8 *tile_dirty)
{
    const size_t row_bytes = (size_t)fb_w * (size_t)bpp;
    const size_t tile_bytes = (size_t)tile * (size_t)bpp;
    int dirty_in_band = 0;
    int row;

    SDL_memset(tile_dirty, 0, (size_t)tiles_x);

    for (row = 0; row < th && dirty_in_band < tiles_x; ++row) {
        const Uint8 *pa = prev + (size_t)(ty + row) * row_bytes;
        const Uint8 *pb = next + (size_t)(ty + row) * row_bytes;
        size_t pos = 0;
        while (pos < row_bytes) {
            int tx_idx = (int)(pos / tile_bytes);
            int span_end_idx;
            size_t span_end;
            size_t rel;

            while (tx_idx < tiles_x && tile_dirty[tx_idx]) {
                tx_idx++;
            }
            if (tx_idx >= tiles_x) {
                break;
            }
            pos = (size_t)tx_idx * tile_bytes;
            span_end_idx = tx_idx + 1;
            while (span_end_idx < tiles_x && !tile_dirty[span_end_idx]) {
                span_end_idx++;
            }
            span_end = (size_t)span_end_idx * tile_bytes;
            if (span_end > row_bytes) {
                span_end = row_bytes;
            }
            rel = KITTY_FindFirstDiff(pa + pos, pb + pos, span_end - pos);
            if (pos + rel < span_end) {
                const int hit_idx = (int)((pos + rel) / tile_bytes);
                tile_dirty[hit_idx] = 1;
                dirty_in_band++;
                pos = (size_t)(hit_idx + 1) * tile_bytes;
            } else {
                pos = span_end;
            }
        }
    }
    return dirty_in_band;
}

/*
 * Compare prev/next by tile×tile streaming row-major scan (bpp 3 or 4).
 * NONE if identical; RECT with dirty_out bbox if coverage < cover_limit;
 * FULL if coverage high or args invalid (dirty_out defaults to full FB then).
 */
KITTY_DiffOutcome KITTY_TileDiffBBox(const Uint8 *prev, const Uint8 *next,
                                     int fb_w, int fb_h, int bpp, int tile,
                                     float cover_limit, SDL_Rect *dirty_out)
{
    Uint8 stack_dirty[512];
    Uint8 *tile_dirty = stack_dirty;
    int tiles_x;
    int ty, tx_idx;
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

    tiles_x = (fb_w + tile - 1) / tile;
    if (tiles_x > (int)sizeof(stack_dirty)) {
        tile_dirty = (Uint8 *)SDL_malloc((size_t)tiles_x);
        if (!tile_dirty) {
            return KITTY_DIFF_FULL;
        }
    }

    for (ty = 0; ty < fb_h; ty += tile) {
        const int th = (ty + tile <= fb_h) ? tile : (fb_h - ty);
        const int band_dirty = KITTY_ScanTileRowBand(prev, next, fb_w, ty, th, bpp, tile, tiles_x, tile_dirty);
        if (band_dirty == 0) {
            continue;
        }
        dirty_tiles += band_dirty;
        if (ty < min_y) {
            min_y = ty;
        }
        if (ty + th > max_y) {
            max_y = ty + th;
        }
        for (tx_idx = 0; tx_idx < tiles_x; ++tx_idx) {
            if (tile_dirty[tx_idx]) {
                const int tx = tx_idx * tile;
                const int tw = (tx + tile <= fb_w) ? tile : (fb_w - tx);
                if (tx < min_x) {
                    min_x = tx;
                }
                if (tx + tw > max_x) {
                    max_x = tx + tw;
                }
            }
        }
    }

    if (tile_dirty != stack_dirty) {
        SDL_free(tile_dirty);
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

/*
 * Multi-rect tile diff: row-major streaming NEON/word scan per tile band,
 * merging contiguous dirty tiles into horizontal runs and extending matching
 * runs vertically across tile rows.
 */
KITTY_DiffOutcome KITTY_TileDiffRects(const Uint8 *prev, const Uint8 *next,
                                      int fb_w, int fb_h, int bpp, int tile,
                                      float cover_limit,
                                      SDL_Rect *rects_out, int max_rects,
                                      int *out_count, SDL_Rect *bbox_out)
{
    Uint8 stack_dirty[512];
    Uint8 *tile_dirty = stack_dirty;
    SDL_Rect acc[64];
    int n_acc = 0;
    bool overflow = false;
    int tiles_x;
    int ty, tx_idx;
    int min_x = fb_w, min_y = fb_h, max_x = 0, max_y = 0;
    size_t dirty_px = 0;
    SDL_Rect bbox;

    if (out_count) {
        *out_count = 0;
    }
    if (bbox_out) {
        bbox_out->x = 0;
        bbox_out->y = 0;
        bbox_out->w = fb_w;
        bbox_out->h = fb_h;
    }
    if (!prev || !next || fb_w <= 0 || fb_h <= 0 || (bpp != 3 && bpp != 4) || tile <= 0) {
        return KITTY_DIFF_FULL;
    }

    tiles_x = (fb_w + tile - 1) / tile;
    if (tiles_x > (int)sizeof(stack_dirty)) {
        tile_dirty = (Uint8 *)SDL_malloc((size_t)tiles_x);
        if (!tile_dirty) {
            return KITTY_DIFF_FULL;
        }
    }

    for (ty = 0; ty < fb_h; ty += tile) {
        const int th = (ty + tile <= fb_h) ? tile : (fb_h - ty);
        const int band_dirty = KITTY_ScanTileRowBand(prev, next, fb_w, ty, th, bpp, tile, tiles_x, tile_dirty);
        int run_x = -1;
        int run_w = 0;

        if (band_dirty == 0) {
            continue;
        }
        if (ty < min_y) {
            min_y = ty;
        }
        if (ty + th > max_y) {
            max_y = ty + th;
        }

        for (tx_idx = 0; tx_idx < tiles_x; ++tx_idx) {
            const int tx = tx_idx * tile;
            const int tw = (tx + tile <= fb_w) ? tile : (fb_w - tx);
            if (tile_dirty[tx_idx]) {
                dirty_px += (size_t)tw * (size_t)th;
                if (tx < min_x) {
                    min_x = tx;
                }
                if (tx + tw > max_x) {
                    max_x = tx + tw;
                }
                if (run_x < 0) {
                    run_x = tx;
                    run_w = tw;
                } else {
                    run_w += tw;
                }
            } else if (run_x >= 0) {
                bool extended = false;
                if (!overflow) {
                    int k;
                    for (k = n_acc - 1; k >= 0; --k) {
                        if (acc[k].x == run_x && acc[k].w == run_w &&
                            acc[k].y + acc[k].h == ty) {
                            acc[k].h += th;
                            extended = true;
                            break;
                        }
                    }
                    if (!extended) {
                        if (n_acc < (int)(sizeof(acc) / sizeof(acc[0]))) {
                            acc[n_acc].x = run_x;
                            acc[n_acc].y = ty;
                            acc[n_acc].w = run_w;
                            acc[n_acc].h = th;
                            n_acc++;
                        } else {
                            overflow = true;
                        }
                    }
                }
                run_x = -1;
                run_w = 0;
            }
        }
        if (run_x >= 0 && !overflow) {
            bool extended = false;
            int k;
            for (k = n_acc - 1; k >= 0; --k) {
                if (acc[k].x == run_x && acc[k].w == run_w &&
                    acc[k].y + acc[k].h == ty) {
                    acc[k].h += th;
                    extended = true;
                    break;
                }
            }
            if (!extended) {
                if (n_acc < (int)(sizeof(acc) / sizeof(acc[0]))) {
                    acc[n_acc].x = run_x;
                    acc[n_acc].y = ty;
                    acc[n_acc].w = run_w;
                    acc[n_acc].h = th;
                    n_acc++;
                } else {
                    overflow = true;
                }
            }
        }
    }

    if (tile_dirty != stack_dirty) {
        SDL_free(tile_dirty);
    }

    if (dirty_px == 0) {
        if (bbox_out) {
            bbox_out->x = bbox_out->y = bbox_out->w = bbox_out->h = 0;
        }
        return KITTY_DIFF_NONE;
    }

    bbox.x = min_x;
    bbox.y = min_y;
    bbox.w = max_x - min_x;
    bbox.h = max_y - min_y;
    if (bbox_out) {
        *bbox_out = bbox;
    }

    {
        const float fb_area = (float)fb_w * (float)fb_h;
        const float actual_cov = fb_area > 0.0f ? ((float)dirty_px / fb_area) : 1.0f;
        if (actual_cov >= cover_limit) {
            if (rects_out && max_rects > 0) {
                rects_out[0] = bbox;
            }
            if (out_count) {
                *out_count = (rects_out && max_rects > 0) ? 1 : 0;
            }
            return KITTY_DIFF_FULL;
        }
    }

    if (rects_out && max_rects > 0) {
        if (overflow) {
            rects_out[0] = bbox;
            if (out_count) {
                *out_count = 1;
            }
        } else if (n_acc > max_rects) {
            const int cn = KITTY_CoalesceRects(fb_w, fb_h, acc, n_acc, tile, rects_out, max_rects);
            if (cn > 0) {
                if (out_count) {
                    *out_count = cn;
                }
            } else {
                rects_out[0] = bbox;
                if (out_count) {
                    *out_count = 1;
                }
            }
        } else {
            int i;
            for (i = 0; i < n_acc; ++i) {
                rects_out[i] = acc[i];
            }
            if (out_count) {
                *out_count = n_acc;
            }
        }
    }
    return KITTY_DIFF_RECT;
}

static inline void KITTY_ConvertRowRGB(const Uint32 *src, Uint8 *dst, int count)
{
    int x = 0;
#ifdef KITTY_HAVE_NEON_LE
    for (; x + 16 <= count; x += 16) {
        const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
        uint8x16x3_t rgb;
        rgb.val[0] = bgra.val[2];
        rgb.val[1] = bgra.val[1];
        rgb.val[2] = bgra.val[0];
        vst3q_u8(dst, rgb);
        dst += 48;
    }
#endif
    for (; x < count; ++x) {
        const Uint32 p = src[x];
        *dst++ = (Uint8)((p >> 16) & 0xFF);
        *dst++ = (Uint8)((p >> 8) & 0xFF);
        *dst++ = (Uint8)(p & 0xFF);
    }
}

static inline void KITTY_ConvertRowRGBA(const Uint32 *src, Uint8 *dst, int count)
{
    int x = 0;
#ifdef KITTY_HAVE_NEON_LE
    for (; x + 16 <= count; x += 16) {
        const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
        uint8x16x4_t rgba;
        rgba.val[0] = bgra.val[2];
        rgba.val[1] = bgra.val[1];
        rgba.val[2] = bgra.val[0];
        rgba.val[3] = bgra.val[3];
        vst4q_u8(dst, rgba);
        dst += 64;
    }
#endif
    for (; x < count; ++x) {
        const Uint32 p = src[x];
        *dst++ = (Uint8)((p >> 16) & 0xFF);
        *dst++ = (Uint8)((p >> 8) & 0xFF);
        *dst++ = (Uint8)(p & 0xFF);
        *dst++ = (Uint8)((p >> 24) & 0xFF);
    }
}

/*
 * Fused single-pass convert + multi-rect tile diff: compares surface pixels
 * directly against rgb_scratch in-place, updating changed pixels and leaving
 * clean tiles read-only. Already-dirty tiles skip reading rgb_scratch.
 */
KITTY_DiffOutcome KITTY_ConvertAndTileDiffRects(SDL_Surface *surface, Uint8 *rgb_scratch,
                                                bool use_rgba, int tile, float cover_limit,
                                                SDL_Rect *rects_out, int max_rects,
                                                int *out_count, SDL_Rect *bbox_out)
{
    Uint8 stack_dirty[512];
    Uint8 *tile_dirty = stack_dirty;
    SDL_Rect acc[64];
    int n_acc = 0;
    bool overflow = false;
    int fb_w, fb_h, bpp, tiles_x;
    int ty, tx_idx;
    int min_x, min_y, max_x = 0, max_y = 0;
    size_t dirty_px = 0;
    SDL_Rect bbox;

    if (out_count) {
        *out_count = 0;
    }
    if (!surface || !rgb_scratch || surface->w <= 0 || surface->h <= 0 || tile <= 0) {
        if (bbox_out) {
            bbox_out->x = bbox_out->y = 0;
            bbox_out->w = surface ? surface->w : 0;
            bbox_out->h = surface ? surface->h : 0;
        }
        return KITTY_DIFF_FULL;
    }

    fb_w = surface->w;
    fb_h = surface->h;
    bpp = use_rgba ? 4 : 3;
    min_x = fb_w;
    min_y = fb_h;
    if (bbox_out) {
        bbox_out->x = 0;
        bbox_out->y = 0;
        bbox_out->w = fb_w;
        bbox_out->h = fb_h;
    }

    tiles_x = (fb_w + tile - 1) / tile;
    if (tiles_x > (int)sizeof(stack_dirty)) {
        tile_dirty = (Uint8 *)SDL_malloc((size_t)tiles_x);
        if (!tile_dirty) {
            return KITTY_DIFF_FULL;
        }
    }

    for (ty = 0; ty < fb_h; ty += tile) {
        const int th = (ty + tile <= fb_h) ? tile : (fb_h - ty);
        int dirty_in_band = 0;
        int row;

        SDL_memset(tile_dirty, 0, (size_t)tiles_x);

        for (row = 0; row < th; ++row) {
            const Uint32 *row_src = (const Uint32 *)((const Uint8 *)surface->pixels +
                                                     (size_t)(ty + row) * (size_t)surface->pitch);
            Uint8 *row_dst = rgb_scratch + (size_t)(ty + row) * (size_t)fb_w * (size_t)bpp;

            if (dirty_in_band == tiles_x) {
                /* Whole band already dirty: contiguous write-only NEON convert. */
                if (use_rgba) {
                    KITTY_ConvertRowRGBA(row_src, row_dst, fb_w);
                } else {
                    KITTY_ConvertRowRGB(row_src, row_dst, fb_w);
                }
                continue;
            }

            for (tx_idx = 0; tx_idx < tiles_x; ++tx_idx) {
                const int tx = tx_idx * tile;
                const int tw = (tx + tile <= fb_w) ? tile : (fb_w - tx);
                const Uint32 *src = row_src + tx;
                Uint8 *dst = row_dst + (size_t)tx * (size_t)bpp;

                if (tile_dirty[tx_idx]) {
                    if (use_rgba) {
                        KITTY_ConvertRowRGBA(src, dst, tw);
                    } else {
                        KITTY_ConvertRowRGB(src, dst, tw);
                    }
                    continue;
                }

                if (!use_rgba) {
                    int x = 0;
                    bool changed = false;
#ifdef KITTY_HAVE_NEON_LE
                    for (; x + 16 <= tw; x += 16) {
                        const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
                        const uint8x16x3_t old = vld3q_u8(dst);
                        const uint8x16_t d = vorrq_u8(
                            vorrq_u8(veorq_u8(bgra.val[2], old.val[0]),
                                     veorq_u8(bgra.val[1], old.val[1])),
                            veorq_u8(bgra.val[0], old.val[2]));
                        if (vmaxvq_u32(vreinterpretq_u32_u8(d)) != 0) {
                            uint8x16x3_t rgb;
                            rgb.val[0] = bgra.val[2];
                            rgb.val[1] = bgra.val[1];
                            rgb.val[2] = bgra.val[0];
                            vst3q_u8(dst, rgb);
                            changed = true;
                            x += 16;
                            dst += 48;
                            break;
                        }
                        dst += 48;
                    }
#endif
                    if (!changed) {
                        for (; x < tw; ++x) {
                            const Uint32 p = src[x];
                            const Uint8 r = (Uint8)((p >> 16) & 0xFF);
                            const Uint8 g = (Uint8)((p >> 8) & 0xFF);
                            const Uint8 b = (Uint8)(p & 0xFF);
                            if (dst[0] != r || dst[1] != g || dst[2] != b) {
                                dst[0] = r;
                                dst[1] = g;
                                dst[2] = b;
                                changed = true;
                                x++;
                                dst += 3;
                                break;
                            }
                            dst += 3;
                        }
                    }
                    if (changed) {
                        tile_dirty[tx_idx] = 1;
                        dirty_in_band++;
                        if (x < tw) {
                            KITTY_ConvertRowRGB(src + x, dst, tw - x);
                        }
                    }
                } else {
                    int x = 0;
                    bool changed = false;
#ifdef KITTY_HAVE_NEON_LE
                    for (; x + 16 <= tw; x += 16) {
                        const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
                        const uint8x16x4_t old = vld4q_u8(dst);
                        const uint8x16_t d = vorrq_u8(
                            vorrq_u8(veorq_u8(bgra.val[2], old.val[0]),
                                     veorq_u8(bgra.val[1], old.val[1])),
                            vorrq_u8(veorq_u8(bgra.val[0], old.val[2]),
                                     veorq_u8(bgra.val[3], old.val[3])));
                        if (vmaxvq_u32(vreinterpretq_u32_u8(d)) != 0) {
                            uint8x16x4_t rgba;
                            rgba.val[0] = bgra.val[2];
                            rgba.val[1] = bgra.val[1];
                            rgba.val[2] = bgra.val[0];
                            rgba.val[3] = bgra.val[3];
                            vst4q_u8(dst, rgba);
                            changed = true;
                            x += 16;
                            dst += 64;
                            break;
                        }
                        dst += 64;
                    }
#endif
                    if (!changed) {
                        for (; x < tw; ++x) {
                            const Uint32 p = src[x];
                            const Uint8 r = (Uint8)((p >> 16) & 0xFF);
                            const Uint8 g = (Uint8)((p >> 8) & 0xFF);
                            const Uint8 b = (Uint8)(p & 0xFF);
                            const Uint8 a = (Uint8)((p >> 24) & 0xFF);
                            if (dst[0] != r || dst[1] != g || dst[2] != b || dst[3] != a) {
                                dst[0] = r;
                                dst[1] = g;
                                dst[2] = b;
                                dst[3] = a;
                                changed = true;
                                x++;
                                dst += 4;
                                break;
                            }
                            dst += 4;
                        }
                    }
                    if (changed) {
                        tile_dirty[tx_idx] = 1;
                        dirty_in_band++;
                        if (x < tw) {
                            KITTY_ConvertRowRGBA(src + x, dst, tw - x);
                        }
                    }
                }
            }
        }

        if (dirty_in_band == 0) {
            continue;
        }
        if (ty < min_y) {
            min_y = ty;
        }
        if (ty + th > max_y) {
            max_y = ty + th;
        }
        {
            int run_x = -1;
            int run_w = 0;
            for (tx_idx = 0; tx_idx < tiles_x; ++tx_idx) {
                const int tx = tx_idx * tile;
                const int tw = (tx + tile <= fb_w) ? tile : (fb_w - tx);
                if (tile_dirty[tx_idx]) {
                    dirty_px += (size_t)tw * (size_t)th;
                    if (tx < min_x) {
                        min_x = tx;
                    }
                    if (tx + tw > max_x) {
                        max_x = tx + tw;
                    }
                    if (run_x < 0) {
                        run_x = tx;
                        run_w = tw;
                    } else {
                        run_w += tw;
                    }
                } else if (run_x >= 0) {
                    if (!overflow) {
                        bool extended = false;
                        int k;
                        for (k = n_acc - 1; k >= 0; --k) {
                            if (acc[k].x == run_x && acc[k].w == run_w &&
                                acc[k].y + acc[k].h == ty) {
                                acc[k].h += th;
                                extended = true;
                                break;
                            }
                        }
                        if (!extended) {
                            if (n_acc < (int)(sizeof(acc) / sizeof(acc[0]))) {
                                acc[n_acc].x = run_x;
                                acc[n_acc].y = ty;
                                acc[n_acc].w = run_w;
                                acc[n_acc].h = th;
                                n_acc++;
                            } else {
                                overflow = true;
                            }
                        }
                    }
                    run_x = -1;
                    run_w = 0;
                }
            }
            if (run_x >= 0 && !overflow) {
                bool extended = false;
                int k;
                for (k = n_acc - 1; k >= 0; --k) {
                    if (acc[k].x == run_x && acc[k].w == run_w &&
                        acc[k].y + acc[k].h == ty) {
                        acc[k].h += th;
                        extended = true;
                        break;
                    }
                }
                if (!extended) {
                    if (n_acc < (int)(sizeof(acc) / sizeof(acc[0]))) {
                        acc[n_acc].x = run_x;
                        acc[n_acc].y = ty;
                        acc[n_acc].w = run_w;
                        acc[n_acc].h = th;
                        n_acc++;
                    } else {
                        overflow = true;
                    }
                }
            }
        }
    }

    if (tile_dirty != stack_dirty) {
        SDL_free(tile_dirty);
    }

    if (dirty_px == 0) {
        if (bbox_out) {
            bbox_out->x = bbox_out->y = bbox_out->w = bbox_out->h = 0;
        }
        return KITTY_DIFF_NONE;
    }

    bbox.x = min_x;
    bbox.y = min_y;
    bbox.w = max_x - min_x;
    bbox.h = max_y - min_y;
    if (bbox_out) {
        *bbox_out = bbox;
    }

    {
        const float fb_area = (float)fb_w * (float)fb_h;
        const float actual_cov = fb_area > 0.0f ? ((float)dirty_px / fb_area) : 1.0f;
        if (actual_cov >= cover_limit) {
            if (rects_out && max_rects > 0) {
                rects_out[0] = bbox;
            }
            if (out_count) {
                *out_count = (rects_out && max_rects > 0) ? 1 : 0;
            }
            return KITTY_DIFF_FULL;
        }
    }

    if (rects_out && max_rects > 0) {
        if (overflow) {
            rects_out[0] = bbox;
            if (out_count) {
                *out_count = 1;
            }
        } else if (n_acc > max_rects) {
            const int cn = KITTY_CoalesceRects(fb_w, fb_h, acc, n_acc, tile, rects_out, max_rects);
            if (cn > 0) {
                if (out_count) {
                    *out_count = cn;
                }
            } else {
                rects_out[0] = bbox;
                if (out_count) {
                    *out_count = 1;
                }
            }
        } else {
            int i;
            for (i = 0; i < n_acc; ++i) {
                rects_out[i] = acc[i];
            }
            if (out_count) {
                *out_count = n_acc;
            }
        }
    }
    return KITTY_DIFF_RECT;
}

/*
 * FULL if SDL_KITTY_DIRTY disables dirty, scratch invalid, or union empty;
 * else DIRTY with dirty_out = union (no coverage threshold).
 */
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

/*
 * Copy a clipped rect from a full-frame RGB24 scratch into tightly packed
 * out (rw*rh*3). Fails if out_cap is too small.
 */
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

/*
 * Convert a clipped XRGB8888 (32-bit) surface rect into RGB24 rows inside
 * a full-size rgb_scratch (w*h*3). Only the rect's pixels are written.
 */
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
        const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch) + area.x;
        Uint8 *dst = rgb_scratch + ((size_t)y * (size_t)surface->w + (size_t)area.x) * 3u;
        const int count = area.w;
        x = 0;
#ifdef KITTY_HAVE_NEON_LE
        for (; x + 16 <= count; x += 16) {
            const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
            uint8x16x3_t rgb;
            rgb.val[0] = bgra.val[2]; /* R */
            rgb.val[1] = bgra.val[1]; /* G */
            rgb.val[2] = bgra.val[0]; /* B */
            vst3q_u8(dst, rgb);
            dst += 48;
        }
#endif
        for (; x < count; ++x) {
            const Uint32 p = src[x];
            *dst++ = (Uint8)((p >> 16) & 0xFF);
            *dst++ = (Uint8)((p >> 8) & 0xFF);
            *dst++ = (Uint8)(p & 0xFF);
        }
    }
    return true;
}

/*
 * Convert a clipped ARGB8888 surface rect into RGBA32 (R,G,B,A) rows inside
 * a full-size rgba_scratch for Kitty f=32.
 */
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
        const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch) + area.x;
        Uint8 *dst = rgba_scratch + ((size_t)y * (size_t)surface->w + (size_t)area.x) * 4u;
        const int count = area.w;
        x = 0;
#ifdef KITTY_HAVE_NEON_LE
        for (; x + 16 <= count; x += 16) {
            const uint8x16x4_t bgra = vld4q_u8((const uint8_t *)(src + x));
            uint8x16x4_t rgba;
            rgba.val[0] = bgra.val[2]; /* R */
            rgba.val[1] = bgra.val[1]; /* G */
            rgba.val[2] = bgra.val[0]; /* B */
            rgba.val[3] = bgra.val[3]; /* A */
            vst4q_u8(dst, rgba);
            dst += 64;
        }
#endif
        for (; x < count; ++x) {
            const Uint32 p = src[x];
            *dst++ = (Uint8)((p >> 16) & 0xFF); /* R */
            *dst++ = (Uint8)((p >> 8) & 0xFF);  /* G */
            *dst++ = (Uint8)(p & 0xFF);         /* B */
            *dst++ = (Uint8)((p >> 24) & 0xFF); /* A */
        }
    }
    return true;
}

/*
 * Copy a clipped rect from a full-frame RGBA32 scratch into tightly packed
 * out (rw*rh*4). Fails if out_cap is too small.
 */
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
