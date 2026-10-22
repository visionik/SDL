/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_RECT_STANDALONE)

#ifndef SDL_kittyrect_h_
#define SDL_kittyrect_h_

/* Coverage at or above this uses a full-surface convert. */
#define KITTY_DIRTY_COVERAGE_THRESHOLD 0.5f

typedef enum KITTY_PresentConvertMode
{
    KITTY_CONVERT_FULL = 0,
    KITTY_CONVERT_DIRTY
} KITTY_PresentConvertMode;

typedef enum KITTY_PresentWireMode
{
    KITTY_WIRE_FULL = 0,     /* full-frame a=T */
    KITTY_WIRE_DIRTY_CPU,    /* dirty convert, full a=T (Step 1a) */
    KITTY_WIRE_DIRTY_BW      /* dirty convert + a=f rect edit (Step 1b) */
} KITTY_PresentWireMode;

/* Clamp and union rects into the FB. Returns false if nothing remains. */
bool KITTY_UnionRects(int fb_w, int fb_h, const SDL_Rect *rects, int numrects, SDL_Rect *out);

/* area(union) / (fb_w*fb_h); 1 if fb empty. */
float KITTY_CoverageRatio(int fb_w, int fb_h, const SDL_Rect *union_rect);

/*
 * Decide full vs dirty convert. dirty_out set when mode is DIRTY.
 * SDL_KITTY_DIRTY=0 forces FULL. First present / invalid scratch → FULL.
 */
KITTY_PresentConvertMode KITTY_ChooseConvertMode(int fb_w, int fb_h,
                                                 const SDL_Rect *rects, int numrects,
                                                 bool scratch_valid, SDL_Rect *dirty_out);

/* XRGB8888 (or compatible 32-bit) surface rect → RGB24 scratch (full w*h*3 buffer). */
bool KITTY_ConvertSurfaceRectToRGB(SDL_Surface *surface, const SDL_Rect *rect, Uint8 *rgb_scratch);

/*
 * Pack a rect from a full-frame RGB24 scratch into a tightly packed buffer
 * (rw*rh*3). out must be large enough.
 */
bool KITTY_PackRGBRect(const Uint8 *rgb_scratch, int fb_w, int fb_h,
                       const SDL_Rect *rect, Uint8 *out, size_t out_cap, size_t *out_len);

#endif /* SDL_kittyrect_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_RECT_STANDALONE */
