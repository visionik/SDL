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
 * Dirty-rect and present helpers for the Kitty video driver: union/coalesce
 * rects, place_cells / cell-cursor mapping, FNV fingerprint, tile-diff bbox,
 * convert-mode / FB-diff env selectors, and XRGB/ARGB → RGB24/RGBA32 pack.
 */
#ifdef KITTY_RECT_STANDALONE
#include <SDL3/SDL.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_RECT_STANDALONE)

#ifndef SDL_kittyrect_h_
#define SDL_kittyrect_h_

typedef enum KITTY_PresentConvertMode
{
    KITTY_CONVERT_FULL = 0,
    KITTY_CONVERT_DIRTY
} KITTY_PresentConvertMode;

typedef enum KITTY_PresentWireMode
{
    KITTY_WIRE_FULL = 0,  /* full-frame a=T */
    KITTY_WIRE_DIRTY_CPU, /* dirty convert, full a=T (Step 1a) */
    KITTY_WIRE_DIRTY_BW,  /* dirty convert + a=f rect edit (Step 1b) */
    KITTY_WIRE_DIRTY_AT,  /* dirty convert + stacked a=T overlays (experiment) */
    KITTY_WIRE_DIFF_SKIP, /* tile-diff found no changes; upload skipped */
    KITTY_WIRE_UPLOAD_SKIP /* scratch FNV matches last upload; wire skipped */
} KITTY_PresentWireMode;

typedef enum KITTY_DiffOutcome
{
    KITTY_DIFF_NONE = 0, /* buffers identical */
    KITTY_DIFF_RECT,     /* use dirty_out bbox for sparse upload */
    KITTY_DIFF_FULL      /* bbox coverage too high — full present */
} KITTY_DiffOutcome;

/*
 * Clamp rects to the FB and compute their axis-aligned union into *out.
 * Empty/null rect list → full FB. Returns false if nothing remains in-bounds.
 */
bool KITTY_UnionRects(int fb_w, int fb_h, const SDL_Rect *rects, int numrects, SDL_Rect *out);

/*
 * area(union_rect) / (fb_w*fb_h), clamped to [0,1]. Returns 1 if the FB
 * dimensions are invalid/empty.
 */
float KITTY_CoverageRatio(int fb_w, int fb_h, const SDL_Rect *union_rect);

/*
 * Map an FB pixel rect onto the place_cells stretch grid (cols×rows).
 * out_col0/out_row0 are 0-based; place_cols/rows are at least 1 when true.
 */
bool KITTY_FbRectToPlaceCells(int fb_w, int fb_h, const SDL_Rect *fb_rect,
                              int cols, int rows,
                              int *out_col0, int *out_row0,
                              int *out_place_cols, int *out_place_rows);

/*
 * Map FB pixel (x,y) to a 0-based cell + in-cell pixel offset for 1:1 place.
 */
bool KITTY_FbPixelToCellCursor(int x, int y, int cell_w, int cell_h,
                               int *out_col0, int *out_row0,
                               int *out_X, int *out_Y);

/*
 * FNV-1a 64-bit hash over bytes (upload_skip fingerprint). NULL data →
 * the FNV offset basis with no mixing.
 */
Uint64 KITTY_Fnv1a64(const Uint8 *data, size_t len);

/*
 * Merge overlapping / near rects (expanded by gap_px). Writes up to max_out
 * coalesced rects into out; if more remain, collapses to a single union.
 * Returns count (0 if nothing remains).
 */
int KITTY_CoalesceRects(int fb_w, int fb_h, const SDL_Rect *in, int n_in,
                        int gap_px, SDL_Rect *out, int max_out);

/*
 * True if the place_cells box touches the screen edge (Exp 3 skip), or if
 * any dimension is non-positive.
 */
bool KITTY_PlaceCellsTouchesEdge(int col0, int row0, int place_cols, int place_rows,
                                 int cols, int rows);

/*
 * Tile memcmp of prev vs next tightly packed RGB/RGBA frames (bpp 3 or 4).
 * dirty_out gets the changed-tile bbox when RECT/FULL. cover_limit: if bbox
 * coverage >= limit → FULL. NONE when identical; FULL on bad args.
 */
KITTY_DiffOutcome KITTY_TileDiffBBox(const Uint8 *prev, const Uint8 *next,
                                     int fb_w, int fb_h, int bpp, int tile,
                                     float cover_limit, SDL_Rect *dirty_out);

/*
 * SDL_KITTY_FB_DIFF: env "0"/off/false/no → false; non-empty otherwise →
 * true; unset → default_on (usually from profile).
 */
bool KITTY_SelectFbDiff(const char *env, bool default_on);

/*
 * SDL_KITTY_FB_DIFF_TILE: env overrides atoi; else default_tile (or 32).
 * Result clamped to [8, 128].
 */
int KITTY_SelectFbDiffTile(const char *env, int default_tile);

/*
 * SDL_KITTY_FB_DIFF_COVER: env overrides atof; else default_cover (or 0.45).
 * Result clamped to [0.05, 1.0].
 */
float KITTY_SelectFbDiffCover(const char *env, float default_cover);

/*
 * Decide full vs dirty convert. dirty_out set when mode is DIRTY (union of
 * rects). SDL_KITTY_DIRTY=0 forces FULL. First present / invalid scratch →
 * FULL. No coverage threshold: any non-empty union stays DIRTY for a=f.
 */
KITTY_PresentConvertMode KITTY_ChooseConvertMode(int fb_w, int fb_h,
                                                 const SDL_Rect *rects, int numrects,
                                                 bool scratch_valid, SDL_Rect *dirty_out);

/*
 * XRGB8888 (or compatible 32-bit) surface rect → RGB24 scratch rows in a
 * full w*h*3 buffer (only the clipped rect is written).
 */
bool KITTY_ConvertSurfaceRectToRGB(SDL_Surface *surface, const SDL_Rect *rect, Uint8 *rgb_scratch);

/*
 * ARGB8888 surface rect → RGBA32 scratch (full w*h*4, byte order R,G,B,A
 * for Kitty f=32).
 */
bool KITTY_ConvertSurfaceRectToRGBA(SDL_Surface *surface, const SDL_Rect *rect, Uint8 *rgba_scratch);

/*
 * Pack a rect from a full-frame RGB24 scratch into a tightly packed buffer
 * (rw*rh*3). out must hold at least that many bytes; out_len optional.
 */
bool KITTY_PackRGBRect(const Uint8 *rgb_scratch, int fb_w, int fb_h,
                       const SDL_Rect *rect, Uint8 *out, size_t out_cap, size_t *out_len);

/*
 * Pack a rect from a full-frame RGBA32 scratch into tightly packed rw*rh*4.
 */
bool KITTY_PackRGBARect(const Uint8 *rgba_scratch, int fb_w, int fb_h,
                        const SDL_Rect *rect, Uint8 *out, size_t out_cap, size_t *out_len);

#endif /* SDL_kittyrect_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_RECT_STANDALONE */
