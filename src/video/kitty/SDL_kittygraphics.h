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
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#ifndef SDL_kittygraphics_h_
#define SDL_kittygraphics_h_

#include "SDL_kittyvideo.h"

bool KITTY_ProbeSupport(void);
bool KITTY_QueryTerminalSize(SDL_VideoData *data);
bool KITTY_EnterRawMode(SDL_VideoData *data);
void KITTY_LeaveRawMode(SDL_VideoData *data);
bool KITTY_SetupTerminal(SDL_VideoData *data);
bool KITTY_EnableMouse(SDL_VideoData *data);
void KITTY_RestoreTerminal(SDL_VideoData *data);
/* kitty_f: 24 (RGB bytes) or 32 (RGBA bytes). */
bool KITTY_TransmitRGB(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h, int kitty_f);
/* Edit root frame (r=1) with packed pixels for rectangle x,y,s,v (Kitty a=f). */
bool KITTY_TransmitRGBRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                               const Uint8 *packed_rgb, int x, int y, int rw, int rh, int kitty_f);
/* Re-place existing image with new c/r (no pixel upload). */
bool KITTY_PlaceExistingImage(SDL_VideoData *vdata, Uint32 image_id,
                              int place_cols, int place_rows);
bool KITTY_DeleteAllImages(int fd);
bool KITTY_DeleteImage(int fd, Uint32 image_id);

#endif // SDL_kittygraphics_h_

#endif // SDL_VIDEO_DRIVER_KITTY
