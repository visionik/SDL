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

#ifndef SDL_kittywindow_h_
#define SDL_kittywindow_h_

/*
 * Window create/destroy/resize hooks for the kitty SDL video driver.
 */

#include "SDL_kittyvideo.h"

/*
 * Create a terminal-filling window: size to CSI pixels, allocate
 * SDL_WindowData / Kitty image ids, and post RESIZED. Focus is deferred to
 * the first PumpEvents; visibility is left to FinishWindowCreation/ShowWindow.
 */
extern bool KITTY_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props);

/*
 * Delete Kitty images on the tty, free SDL_WindowData / RGB scratch, and
 * clear focus if this window owned it.
 */
extern void KITTY_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window);

/*
 * Post SDL_EVENT_WINDOW_RESIZED from window->pending; does not change tty size.
 */
extern void KITTY_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window);

/*
 * No-op show/hide hooks. Without these, SDL_ShowWindow/HideWindow take the
 * default branch and SetKeyboardFocus(window/NULL), which races the deferred
 * PumpEvents focus announce (FOCUS_GAINED/LOST chatter). Same pattern as
 * RPI/KMSDRM/Vita framebuffer drivers.
 */
extern void KITTY_ShowWindow(SDL_VideoDevice *_this, SDL_Window *window);
extern void KITTY_HideWindow(SDL_VideoDevice *_this, SDL_Window *window);

#endif // SDL_kittywindow_h_

#endif // SDL_VIDEO_DRIVER_KITTY
