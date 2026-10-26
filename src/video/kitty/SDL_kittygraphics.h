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
 * Kitty graphics protocol helpers: tty raw mode, CSI sizing, SGR mouse
 * enable/restore, and APC image transmit / place / frame-edit / delete.
 */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#ifndef SDL_kittygraphics_h_
#define SDL_kittygraphics_h_

#include "SDL_kittyvideo.h"

/*
 * Return true when stdout is a tty and the host looks like a Kitty-protocol
 * terminal (SDL_HINT_VIDEO_DRIVER, loaded profile match, or builtin env).
 */
bool KITTY_ProbeSupport(void);

/*
 * Refresh cols/rows and pixel/cell size via TIOCGWINSZ and CSI 14t/16t,
 * then apply framebuffer caps and mouse coordinate space.
 */
bool KITTY_QueryTerminalSize(SDL_VideoData *data);

/*
 * Aspect-fit pixel_w/h into the active soft-max box: env FB_MAX/FIXED, else
 * profile default_fb_max_shm_* when transmit is SHM, else default_fb_max_direct_*,
 * else default_fb_max_*. Call again after ProbeTransmitModes.
 */
void KITTY_ApplyFramebufferCap(SDL_VideoData *data);

/*
 * Put tty_in into raw mode (cfmakeraw) while keeping ISIG for Ctrl+C/Z.
 * Saves prior termios for KITTY_LeaveRawMode.
 */
bool KITTY_EnterRawMode(SDL_VideoData *data);

/*
 * Restore saved termios on tty_in if KITTY_EnterRawMode succeeded earlier.
 */
void KITTY_LeaveRawMode(SDL_VideoData *data);

/*
 * Enter alt screen, hide cursor, home/clear, and enable Kitty keyboard
 * (CSI > 11 u). Mouse tracking is deferred to KITTY_EnableMouse.
 */
bool KITTY_SetupTerminal(SDL_VideoData *data);

/*
 * Enable SGR mouse tracking (1000/1002/1003). Prefer SGR-pixels (1016)
 * when DECRQM recognizes it; otherwise cell SGR (1006).
 */
bool KITTY_EnableMouse(SDL_VideoData *data);

/*
 * Delete all images, disable mouse/keyboard modes, leave alt screen,
 * drain late APC replies, and restore cooked termios.
 */
void KITTY_RestoreTerminal(SDL_VideoData *data);

/*
 * Full-frame transmit+place (a=T): SHM, zlib direct, or raw base64 per
 * vdata->transmit_mode / SDL_KITTY_TRANSMIT. kitty_f: 24 (RGB) or 32 (RGBA).
 */
bool KITTY_TransmitRGB(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h, int kitty_f);

/*
 * Edit root frame (r=1) in place with packed pixels for rectangle
 * x,y,s,v (Kitty a=f, t=d only). Does not home or re-place.
 */
bool KITTY_TransmitRGBRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                               const Uint8 *packed_rgb, int x, int y, int rw, int rh, int kitty_f);

/*
 * Transmit+place a packed rect as its own image at a 1-based CSI CUP cell.
 * place_cols/rows 0 = natural pixel size; cell_X/Y are in-cell pixel offsets;
 * z stacks above the base. Uses DECSC/DECRC around CUP.
 */
bool KITTY_TransmitRGBRectPlace(SDL_VideoData *vdata, Uint32 image_id,
                                const Uint8 *packed_rgb, int rw, int rh,
                                int place_col, int place_row,
                                int place_cols, int place_rows,
                                int cell_X, int cell_Y, int z, int kitty_f);

/*
 * Delete active dirty_at overlay images and clear wdata->overlay_count.
 */
void KITTY_ClearOverlays(SDL_VideoData *vdata, SDL_WindowData *wdata);

/*
 * Re-place an existing image with new c/r (a=p only; no pixel upload).
 * Homes the cursor and updates placed_cols/rows / place_homed.
 */
bool KITTY_PlaceExistingImage(SDL_VideoData *vdata, Uint32 image_id,
                              int place_cols, int place_rows);

/*
 * Delete every Kitty image on fd (a=d,d=A,q=2).
 */
bool KITTY_DeleteAllImages(int fd);

/*
 * Delete one Kitty image by id on fd (a=d,d=I,q=2).
 */
bool KITTY_DeleteImage(int fd, Uint32 image_id);

#endif // SDL_kittygraphics_h_

#endif // SDL_VIDEO_DRIVER_KITTY
