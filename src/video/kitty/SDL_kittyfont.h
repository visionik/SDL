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

#ifndef SDL_kittyfont_h_
#define SDL_kittyfont_h_

/*
 * Embedded Geist-Regular TTF font renderer (via stb_truetype + zlib) and
 * anti-aliased RGBA32 2D card/pill/sparkline primitives for SDL_KITTY_OVERLAY
 * and the F12 live performance HUD.
 */

typedef struct KITTY_Canvas
{
    Uint8 *rgba;
    int w;
    int h;
} KITTY_Canvas;

/* Lazy-init embedded Geist-Regular TTF; safe to call repeatedly. */
bool KITTY_FontEnsureInit(void);
void KITTY_FontShutdown(void);

/* Measure UTF-8 string width (px) at pixel height px_size. */
float KITTY_FontMeasureUTF8(const char *utf8, size_t len, float px_size, bool tabular_digits);

/* Draw anti-aliased UTF-8 text onto canvas at (x, baseline_y). Returns advance x. */
float KITTY_FontDrawUTF8(KITTY_Canvas *c, float x, float baseline_y,
                         const char *utf8, size_t len, float px_size,
                         const Uint8 color[4], bool shadow, bool tabular_digits);

/* Alpha-blend a single pixel (r,g,b,a) onto RGBA32 canvas. */
void KITTY_CanvasBlendPixel(KITTY_Canvas *c, int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a);

/* Draw a glassmorphic rounded card with multi-ring soft drop shadow,
 * vertical gradient fill, SDF anti-aliased border, and 1px top specular sheen. */
void KITTY_CanvasDrawRoundedCard(KITTY_Canvas *c, int x, int y, int w, int h, float radius,
                                 const Uint8 bg_top[4], const Uint8 bg_bot[4],
                                 const Uint8 border[4], int shadow_pad, Uint8 sheen_alpha);

/* Draw an anti-aliased rounded pill/rect with optional 1px border. */
void KITTY_CanvasDrawPill(KITTY_Canvas *c, int x, int y, int w, int h, float radius,
                          const Uint8 fill[4], const Uint8 border[4]);

/* Draw a glowing circular status orb (radial halo + bright solid core). */
void KITTY_CanvasDrawGlowDot(KITTY_Canvas *c, float cx, float cy, float core_r, float glow_r,
                             const Uint8 color[4]);

/* Draw a sleek rounded gradient progress bar (t in [0, 1]). */
void KITTY_CanvasDrawProgressBar(KITTY_Canvas *c, int x, int y, int w, int h, float t,
                                 const Uint8 track[4], const Uint8 fill_l[4], const Uint8 fill_r[4]);

/* Draw an anti-aliased sparkline with vertical gradient area fill underneath
 * and a bright glowing tip dot on the latest sample. */
void KITTY_CanvasDrawSparkline(KITTY_Canvas *c, int x, int y, int w, int h,
                               const float *samples, int count, float max_val,
                               const Uint8 line_col[4], const Uint8 fill_top[4],
                               const Uint8 fill_bot[4], bool draw_tip);

#endif /* SDL_kittyfont_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY */
