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

#ifndef SDL_kittyevents_c_h_
#define SDL_kittyevents_c_h_

/*
 * Event pump entry for the kitty SDL video driver (tty CSI/keyboard/mouse).
 */

#include "SDL_kittyvideo.h"

/*
 * Read tty_in, parse Kitty keyboard / SGR mouse / CSI, handle SIGWINCH-sized
 * panes, flush deferred key-ups, and post SDL keyboard, mouse, and window events.
 */
extern void KITTY_PumpEvents(SDL_VideoDevice *_this);

#endif // SDL_kittyevents_c_h_

#endif // SDL_VIDEO_DRIVER_KITTY
