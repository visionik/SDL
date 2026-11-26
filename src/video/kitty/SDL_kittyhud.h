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

#ifndef SDL_kittyhud_h_
#define SDL_kittyhud_h_

/*
 * Live glassmorphic performance HUD (SDL_KITTY_HUD=1 or F12 toggle) +
 * pulsing REC indicator badge for Shift+F10 video capture.
 */

typedef struct KITTY_PerfHud KITTY_PerfHud;

KITTY_PerfHud *KITTY_HudCreate(const char *spec);
void KITTY_HudDestroy(KITTY_PerfHud *hud);

/* Toggle visibility on F12. Returns true if the key was consumed. */
bool KITTY_HudHandleKey(KITTY_PerfHud *hud, SDL_Scancode scancode);

/* Record per-frame telemetry (called on every present, O(1) when hidden). */
void KITTY_HudRecordFrame(KITTY_PerfHud *hud, Uint64 now_ns, double tx_ms,
                          size_t wire_bytes, float dirty_pct,
                          const char *transport_tag, bool async_drop_active,
                          Uint64 dropped_frames);

/* Update recording indicator state (0 = not recording). */
void KITTY_HudSetRecording(KITTY_PerfHud *hud, bool active, Uint64 start_ms);

/* Evaluate whether HUD needs compositing this frame. */
bool KITTY_HudEvaluate(KITTY_PerfHud *hud, int fb_w, int fb_h, Uint64 now_ms,
                       SDL_Rect *out_rect, bool *out_needs_redraw);

/* Alpha-composite HUD onto rgb_scratch (saving clean pixels for restore). */
bool KITTY_HudApply(KITTY_PerfHud *hud, Uint8 *rgb_scratch, int fb_w, int fb_h, int bpp);
void KITTY_HudRestore(KITTY_PerfHud *hud, Uint8 *rgb_scratch, int fb_w, int fb_h, int bpp);
void KITTY_HudMarkDrawn(KITTY_PerfHud *hud, bool visible);

#endif /* SDL_kittyhud_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY */
