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

#include "../SDL_sysvideo.h"
#include "../../events/SDL_windowevents_c.h"
#include "SDL_kittywindow.h"
#include "SDL_kittygraphics.h"

bool KITTY_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    (void)create_props;

    if (!data) {
        return false;
    }

    if (window->x == SDL_WINDOWPOS_UNDEFINED) {
        window->x = 0;
    }
    if (window->y == SDL_WINDOWPOS_UNDEFINED) {
        window->y = 0;
    }

    // Default window to the full terminal pixel size.
    if (window->w <= 1 || window->h <= 1) {
        window->w = vdata->pixel_w;
        window->h = vdata->pixel_h;
    }

    data->sdl_window = window;
    data->image_id = 1;
    data->placed = false;
    window->internal = data;
    return true;
}

void KITTY_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *data = (SDL_WindowData *)window->internal;

    if (vdata) {
        KITTY_DeleteAllImages(vdata->tty_fd);
    }
    if (data) {
        SDL_free(data);
    }
    window->internal = NULL;
}

void KITTY_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->pending.w, window->pending.h);
}

#endif // SDL_VIDEO_DRIVER_KITTY
