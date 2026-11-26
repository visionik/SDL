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

#ifndef SDL_kittycapture_h_
#define SDL_kittycapture_h_

/*
 * Driver-level lossless PNG screenshot (F10) and live video capture (Shift+F10,
 * piping raw RGB24 directly to ffmpeg -> .mp4 with PNG-sequence fallback).
 */

typedef struct KITTY_Capture KITTY_Capture;

KITTY_Capture *KITTY_CaptureCreate(void);
void KITTY_CaptureDestroy(KITTY_Capture *cap);

/* Request a single lossless PNG capture on the next present (F10). */
void KITTY_CaptureRequestPng(KITTY_Capture *cap);

/* Toggle live video recording (Shift+F10). */
void KITTY_CaptureToggleVideo(KITTY_Capture *cap);

/* Query active recording state for HUD REC badge. */
bool KITTY_CaptureIsRecording(const KITTY_Capture *cap, Uint64 *out_start_ms);

/*
 * Process pending PNG screenshot or active video frame from clean rgb_scratch.
 * If a user-facing toast message was produced (e.g. "Saved PNG · ..."), writes
 * it into toast_out and returns true.
 */
bool KITTY_CaptureOnFrame(KITTY_Capture *cap, const Uint8 *pixels, int w, int h, int bpp,
                          Uint64 now_ms, char *toast_out, size_t toast_cap);

/* Standalone PNG writer helper (also useful for tests). */
bool KITTY_WritePngFile(const char *path, const Uint8 *pixels, int w, int h, int bpp);

#endif /* SDL_kittycapture_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY */
