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

#ifndef SDL_kittyoverlay_h_
#define SDL_kittyoverlay_h_

/*
 * Driver-level translucent HUD toast overlay configured via SDL_KITTY_OVERLAY.
 * Pre-renders once into a cached RGBA pill and composites temporarily onto
 * rgb_scratch during present (saving and restoring the clean game pixels so
 * tile-diff and upload_skip never see overlay pixels).
 */

typedef struct KITTY_ToastOverlay KITTY_ToastOverlay;

KITTY_ToastOverlay *KITTY_OverlayCreate(const char *spec);
void KITTY_OverlayDestroy(KITTY_ToastOverlay *ov);

/*
 * Evaluate overlay visibility/alpha at now_ms for framebuffer fb_w x fb_h.
 * Returns true if the overlay is currently visible (alpha > 0).
 * Sets out_rect (clamped to framebuffer), out_alpha (0..255), and
 * out_needs_redraw (true when alpha or geometry changed since last drawn).
 */
bool KITTY_OverlayEvaluate(KITTY_ToastOverlay *ov, int fb_w, int fb_h, Uint64 now_ms,
                           SDL_Rect *out_rect, Uint8 *out_alpha, bool *out_needs_redraw);

/* Save underlying clean pixels from rgb_scratch and alpha-blend overlay at alpha (1..255). */
bool KITTY_OverlayApply(KITTY_ToastOverlay *ov, Uint8 *rgb_scratch, int fb_w, int fb_h,
                        int bpp, Uint8 alpha);

/* Restore saved clean pixels back into rgb_scratch immediately after transmit. */
void KITTY_OverlayRestore(KITTY_ToastOverlay *ov, Uint8 *rgb_scratch, int fb_w, int fb_h,
                          int bpp);

/* Record the alpha actually transmitted on the last successful present. */
void KITTY_OverlayMarkDrawn(KITTY_ToastOverlay *ov, Uint8 alpha);

/*
 * Check if scancode matches the configured toggle= hotkey (e.g. F1).
 * On match, toggles overlay visibility (shows or fades out) and returns true.
 */
bool KITTY_OverlayHandleKey(KITTY_ToastOverlay *ov, SDL_Scancode scancode, Uint64 now_ms);

#endif /* SDL_kittyoverlay_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY */
