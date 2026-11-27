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
 * Parse SDL_KITTY_VIEWPORT=col,row,w,h (1-based cells) and SDL_KITTY_ALTSCREEN.
 * Viewport set ⇒ default use_alt_screen false unless ALTSCREEN explicitly on.
 * Also parses SDL_KITTY_LEAVE_IMAGES (skip a=d,d=A on quit).
 */
void KITTY_ApplyViewportEnv(SDL_VideoData *data);

/* Clamp viewport to current cols/rows after size query. */
void KITTY_ClampViewport(SDL_VideoData *data);

/* place_cells size: viewport when active, else full grid; 0,0 if not stretching. */
void KITTY_GetPlaceCells(const SDL_VideoData *data, int *out_cols, int *out_rows);

/* CUP to viewport origin, or home if no viewport. */
bool KITTY_WritePlaceOrigin(SDL_VideoData *data);

/* ASCII border one cell outside the live viewport when SDL_KITTY_CARD_CHROME=1.
 * No-op if chrome off, no viewport, or no room. Safe to call again after WINCH. */
bool KITTY_DrawViewportChrome(SDL_VideoData *data);

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
 * Async-signal-safe terminal restore for SIGINT/SIGTERM handlers: writes
 * mode-reset escapes and restores saved termios without allocating or logging.
 */
void KITTY_EmergencyRestore(int tty_in, int tty_out, bool use_alt_screen, bool leave_images);

/* Free persistent transmit scratch buffers and reusable z_stream on vdata. */
void KITTY_FreeTransmitBuffers(SDL_VideoData *vdata);

/* Wait for any in-flight pipelined async PTY write to finish (ordering barrier). */
void KITTY_FlushAsyncWriter(SDL_VideoData *vdata);

/* Coalesce multi-rect a=f APCs into the async writer stage buffer for one submit/frame. */
void KITTY_BeginAsyncBatch(SDL_VideoData *vdata);
bool KITTY_CommitAsyncBatch(SDL_VideoData *vdata);
void KITTY_CancelAsyncBatch(SDL_VideoData *vdata);

/*
 * Full-frame transmit+place (a=T): SHM, zlib direct, or raw base64 per
 * vdata->transmit_mode / SDL_KITTY_TRANSMIT. kitty_f: 24 (RGB) or 32 (RGBA).
 */
bool KITTY_TransmitRGB(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h, int kitty_f);

/*
 * Whether a=f edits go compressed (o=z): SDL_KITTY_AF_ZLIB=1 or 0 if set, else
 * the profile's af_zlib (kitty.frame_edit_zlib, or tuning).
 */
bool KITTY_EditsCompressed(const SDL_VideoData *vdata);

/* The window property apps read to see whether edits go compressed (as
 * KITTY_EditsCompressed decided when the window was made). */
#define KITTY_PROP_WINDOW_AF_ZLIB "SDL.window.kitty.af_zlib"

/*
 * Edit root frame (r=1) in place with packed pixels for rectangle
 * x,y,s,v (Kitty a=f, t=d only). Does not home or re-place.
 */
bool KITTY_TransmitRGBRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                               const Uint8 *packed_rgb, int x, int y, int rw, int rh, int kitty_f);

/*
 * Zero-copy strided parallel-pack + parallel-zlib a=f directly from full-FB
 * rgb_scratch for large compressed dirty rects (>= 128 KB). Returns false to
 * fall back to PackRGBRect + KITTY_TransmitRGBRectEdit.
 */
bool KITTY_TransmitRGBRectEditFromScratch(SDL_VideoData *vdata, Uint32 image_id,
                                          const Uint8 *rgb_scratch, int fb_w, int fb_h,
                                          const SDL_Rect *rect, int kitty_f);

/*
 * Parallel multi-rect a=f pack + zlib + base64 + single async submit.
 * Returns true if all clipped rects were transmitted; false to fall back.
 */
bool KITTY_TransmitMultiRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                                 const Uint8 *rgb_scratch, int fb_w, int fb_h,
                                 const SDL_Rect *rects, int n_rects, int kitty_f,
                                 size_t *out_upload_px);

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

/* Emit OSC 52 clipboard write (\033]52;c;<b64>\033\\) and query (\033]52;c;?\033\\). */
bool KITTY_Osc52SetClipboard(SDL_VideoData *vdata, const char *utf8);
char *KITTY_Osc52GetClipboard(SDL_VideoData *vdata, int timeout_ms);

#endif // SDL_kittygraphics_h_

#endif // SDL_VIDEO_DRIVER_KITTY
