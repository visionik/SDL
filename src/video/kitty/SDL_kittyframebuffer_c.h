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
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#ifndef SDL_kittyframebuffer_c_h_
#define SDL_kittyframebuffer_c_h_

/*
 * Software framebuffer hooks: SDL surface pixels presented via Kitty APC.
 */

#include "SDL_kittyvideo.h"

/*
 * Allocate an ARGB/XRGB SDL_Surface for the window, store it on window
 * properties, and return format/pixels/pitch to SDL.
 */
extern bool SDL_KITTY_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch);

/*
 * Convert dirty (or full) surface rects to RGB/RGBA scratch and upload via
 * Kitty graphics APC (full place, a=f edit, overlays, or skip). Writes tty.
 */
extern bool SDL_KITTY_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects);

/*
 * Free RGB scratch buffers and clear the window surface property.
 */
extern void SDL_KITTY_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window);

#endif // SDL_kittyframebuffer_c_h_

#endif // SDL_VIDEO_DRIVER_KITTY
