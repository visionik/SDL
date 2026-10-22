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
#include "../../SDL_properties_c.h"
#include "SDL_kittyframebuffer_c.h"
#include "SDL_kittygraphics.h"

#define KITTY_SURFACE "SDL.internal.window.surface"

bool SDL_KITTY_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_Surface *surface;
    const SDL_PixelFormat surface_format = SDL_PIXELFORMAT_XRGB8888;
    int w, h;
    (void)_this;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return SDL_SetError("invalid kitty framebuffer size");
    }
    surface = SDL_CreateSurface(w, h, surface_format);
    if (!surface) {
        return false;
    }

    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), KITTY_SURFACE, surface);
    *format = surface_format;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return true;
}

bool SDL_KITTY_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *wdata = (SDL_WindowData *)window->internal;
    SDL_Surface *surface;
    Uint8 *rgb = NULL;
    int x, y;
    bool ok;
    Uint32 image_id;
    (void)rects;
    (void)numrects;

    surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), KITTY_SURFACE, NULL);
    if (!surface) {
        return SDL_SetError("Couldn't find kitty surface for window");
    }
    if (!vdata || !wdata) {
        return SDL_SetError("kitty video/window data missing");
    }

    rgb = (Uint8 *)SDL_malloc((size_t)surface->w * (size_t)surface->h * 3u);
    if (!rgb) {
        return false;
    }

    for (y = 0; y < surface->h; ++y) {
        const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + y * surface->pitch);
        Uint8 *dst = rgb + (size_t)y * (size_t)surface->w * 3u;
        for (x = 0; x < surface->w; ++x) {
            const Uint32 p = src[x];
            dst[x * 3 + 0] = (Uint8)((p >> 16) & 0xFF);
            dst[x * 3 + 1] = (Uint8)((p >> 8) & 0xFF);
            dst[x * 3 + 2] = (Uint8)(p & 0xFF);
        }
    }

    wdata->image_index ^= 1;
    image_id = wdata->image_ids[wdata->image_index];
    ok = KITTY_TransmitRGB(vdata, image_id, rgb, surface->w, surface->h);
    if (ok) {
        // Drop the previous buffer's image to limit terminal VRAM use.
        const Uint32 prev = wdata->image_ids[wdata->image_index ^ 1];
        if (wdata->placed) {
            KITTY_DeleteImage(vdata->tty_fd, prev);
        }
        wdata->placed = true;
    }
    SDL_free(rgb);
    return ok;
}

void SDL_KITTY_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_ClearProperty(SDL_GetWindowProperties(window), KITTY_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_KITTY
