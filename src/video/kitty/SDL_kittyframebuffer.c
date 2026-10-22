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
#include "SDL_kittyprobe.h"
#include "SDL_kittyrect.h"

#define KITTY_SURFACE "SDL.internal.window.surface"

static void KITTY_FreeRGBScratch(SDL_WindowData *wdata)
{
    if (!wdata) {
        return;
    }
    SDL_free(wdata->rgb_scratch);
    wdata->rgb_scratch = NULL;
    wdata->rgb_w = 0;
    wdata->rgb_h = 0;
    wdata->rgb_valid = false;
    wdata->live_image_id = 0;
}

static bool KITTY_EnsureRGBScratch(SDL_WindowData *wdata, int w, int h)
{
    const size_t need = (size_t)w * (size_t)h * 3u;
    if (!wdata || w <= 0 || h <= 0) {
        return false;
    }
    if (wdata->rgb_scratch && wdata->rgb_w == w && wdata->rgb_h == h) {
        return true;
    }
    KITTY_FreeRGBScratch(wdata);
    wdata->rgb_scratch = (Uint8 *)SDL_malloc(need);
    if (!wdata->rgb_scratch) {
        return false;
    }
    wdata->rgb_w = w;
    wdata->rgb_h = h;
    wdata->rgb_valid = false;
    wdata->live_image_id = 0;
    return true;
}

bool SDL_KITTY_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *wdata = window ? (SDL_WindowData *)window->internal : NULL;
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

    if (wdata) {
        KITTY_FreeRGBScratch(wdata);
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
    bool ok = false;
    Uint32 image_id;
    SDL_Rect dirty;
    KITTY_PresentConvertMode convert_mode;
    KITTY_PresentWireMode wire_mode = KITTY_WIRE_FULL;
    SDL_Rect full;
    size_t convert_px;
    size_t upload_px;
    Uint8 *packed = NULL;

    surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), KITTY_SURFACE, NULL);
    if (!surface) {
        return SDL_SetError("Couldn't find kitty surface for window");
    }
    if (!vdata || !wdata) {
        return SDL_SetError("kitty video/window data missing");
    }

    if (!KITTY_EnsureRGBScratch(wdata, surface->w, surface->h)) {
        return false;
    }

    convert_mode = KITTY_ChooseConvertMode(surface->w, surface->h, rects, numrects,
                                           wdata->rgb_valid, &dirty);
    full.x = 0;
    full.y = 0;
    full.w = surface->w;
    full.h = surface->h;

    if (convert_mode == KITTY_CONVERT_DIRTY) {
        if (!KITTY_ConvertSurfaceRectToRGB(surface, &dirty, wdata->rgb_scratch)) {
            convert_mode = KITTY_CONVERT_FULL;
        }
    }
    if (convert_mode == KITTY_CONVERT_FULL) {
        if (!KITTY_ConvertSurfaceRectToRGB(surface, &full, wdata->rgb_scratch)) {
            return SDL_SetError("kitty RGB convert failed");
        }
        dirty = full;
    }

    convert_px = (size_t)dirty.w * (size_t)dirty.h;
    upload_px = (size_t)surface->w * (size_t)surface->h;
    wire_mode = (convert_mode == KITTY_CONVERT_DIRTY) ? KITTY_WIRE_DIRTY_CPU : KITTY_WIRE_FULL;

    /* Step 1b: optional bandwidth path — edit live image root frame. */
    if (convert_mode == KITTY_CONVERT_DIRTY &&
        KITTY_SelectDirtyBW(SDL_getenv("SDL_KITTY_DIRTY_BW"), vdata->frame_edit_ok) &&
        wdata->placed &&
        wdata->live_image_id != 0) {
        size_t packed_len = 0;
        const size_t pack_cap = convert_px * 3u;
        packed = (Uint8 *)SDL_malloc(pack_cap);
        if (packed &&
            KITTY_PackRGBRect(wdata->rgb_scratch, surface->w, surface->h, &dirty,
                              packed, pack_cap, &packed_len) &&
            KITTY_TransmitRGBRectEdit(vdata, wdata->live_image_id, packed,
                                      dirty.x, dirty.y, dirty.w, dirty.h)) {
            ok = true;
            wire_mode = KITTY_WIRE_DIRTY_BW;
            upload_px = convert_px;
            wdata->rgb_valid = true;
        } else {
            /* Fall back to full a=T. */
            SDL_ClearError();
        }
        SDL_free(packed);
        packed = NULL;
    }

    if (!ok) {
        const bool pingpong = KITTY_SelectPingPong(SDL_getenv("SDL_KITTY_PINGPONG"));
        Uint32 prev = 0;

        if (pingpong) {
            wdata->image_index ^= 1;
            image_id = wdata->image_ids[wdata->image_index];
            prev = wdata->image_ids[wdata->image_index ^ 1];
        } else {
            /* Same (i,p=1) replace: re-a=T the live id; no per-frame delete. */
            image_id = wdata->live_image_id ? wdata->live_image_id : wdata->image_ids[0];
        }

        ok = KITTY_TransmitRGB(vdata, image_id, wdata->rgb_scratch, surface->w, surface->h);
        if (ok) {
            if (pingpong && wdata->placed && prev != 0 && prev != image_id) {
                KITTY_DeleteImage(vdata->tty_fd, prev);
            }
            wdata->placed = true;
            wdata->rgb_valid = true;
            wdata->live_image_id = image_id;
            if (wire_mode != KITTY_WIRE_DIRTY_BW) {
                wire_mode = (convert_mode == KITTY_CONVERT_DIRTY) ? KITTY_WIRE_DIRTY_CPU : KITTY_WIRE_FULL;
            }
        }
    }

    {
        const char *dbg = SDL_getenv("SDL_KITTY_DEBUG");
        if (dbg && dbg[0] && dbg[0] != '0') {
            static int frames;
            if (frames < 8 || !ok) {
                FILE *log = fopen("/tmp/sdl_kitty_debug.log", "a");
                if (log) {
                    const float cov = KITTY_CoverageRatio(surface->w, surface->h, &dirty);
                    const char *mode_str = "full";
                    if (wire_mode == KITTY_WIRE_DIRTY_BW) {
                        mode_str = "dirty_bw";
                    } else if (wire_mode == KITTY_WIRE_DIRTY_CPU) {
                        mode_str = "dirty_cpu";
                    }
                    fprintf(log,
                            "present #%d %dx%d place=%d mode=%s rects=%d union=%dx%d coverage=%.2f convert_px=%zu upload_px=%zu live=%u pingpong=%d -> %s%s\n",
                            frames, surface->w, surface->h,
                            vdata->place_cells ? 1 : 0,
                            mode_str,
                            numrects, dirty.w, dirty.h, (double)cov,
                            convert_px, upload_px,
                            (unsigned)wdata->live_image_id,
                            KITTY_SelectPingPong(SDL_getenv("SDL_KITTY_PINGPONG")) ? 1 : 0,
                            ok ? "ok" : "FAIL ",
                            ok ? "" : SDL_GetError());
                    fclose(log);
                }
            }
            frames++;
        }
    }
    return ok;
}

void SDL_KITTY_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *wdata = window ? (SDL_WindowData *)window->internal : NULL;
    (void)_this;
    if (wdata) {
        KITTY_FreeRGBScratch(wdata);
    }
    SDL_ClearProperty(SDL_GetWindowProperties(window), KITTY_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_KITTY
