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

/*
 * SDL window lifecycle for the kitty driver: size to the tty pixel pane,
 * allocate SDL_WindowData / Kitty image ids, and tear down APC images.
 */

#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "../SDL_sysvideo.h"
#include "SDL_kittygraphics.h"
#include "SDL_kittyprofile.h"
#include "SDL_kittywindow.h"

/*
 * Create a terminal-filling window: refresh tty size, force w/h to CSI pixel
 * metrics (or profile fallback), allocate SDL_WindowData and dual Kitty image
 * ids, and post RESIZED. Focus waits for the first PumpEvents; SHOWN comes
 * from FinishWindowCreation → SDL_ShowWindow → KITTY_ShowWindow.
 */
bool KITTY_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    (void)create_props;

    if (!data) {
        return false;
    }

    // Keep terminal metrics fresh; some hosts omit TIOCGWINSZ pixels until later.
    (void)KITTY_QueryTerminalSize(vdata);

    if (window->x == SDL_WINDOWPOS_UNDEFINED) {
        window->x = 0;
    }
    if (window->y == SDL_WINDOWPOS_UNDEFINED) {
        window->y = 0;
    }

    /* Kitty is a terminal-filling framebuffer: always match CSI/text-area
     * pixels. SDL may pass 1x1 (from 0x0) or an app size like 640x480; neither
     * should letterbox inside the pane or mouse/graphics disagree with the
     * host cursor. Keep windowed/floating in sync so FinishWindowCreation
     * cannot restore stubs. */
    if (vdata->pixel_w > 0 && vdata->pixel_h > 0) {
        window->w = vdata->pixel_w;
        window->h = vdata->pixel_h;
    } else if (window->w <= 1 || window->h <= 1) {
        window->w = (vdata->term_profile && vdata->term_profile->window_fallback_w > 0)
                        ? vdata->term_profile->window_fallback_w
                        : 640;
        window->h = (vdata->term_profile && vdata->term_profile->window_fallback_h > 0)
                        ? vdata->term_profile->window_fallback_h
                        : 480;
    }
    window->windowed.w = window->floating.w = window->w;
    window->windowed.h = window->floating.h = window->h;
    window->windowed.x = window->floating.x = window->x;
    window->windowed.y = window->floating.y = window->y;

    data->sdl_window = window;
    data->image_ids[0] = 1;
    data->image_ids[1] = 2;
    data->image_index = 0;
    data->placed = false;
    data->rgb_scratch = NULL;
    data->rgb_back = NULL;
    data->rgb_w = 0;
    data->rgb_h = 0;
    data->scratch_bpp = 0;
    data->rgb_valid = false;
    data->live_image_id = 0;
    data->overlay_count = 0;
    {
        int oi;
        for (oi = 0; oi < KITTY_OVERLAY_MAX; ++oi) {
            data->overlay_ids[oi] = KITTY_OVERLAY_ID_BASE + (Uint32)oi;
        }
    }
    window->internal = data;

    vdata->focus_window = window;
    /* Do not clear focus_announced here: GetWindowSurface may RecreateWindow
     * (texture-framebuffer probe → OpenGL/Metal reconfigure) and would otherwise
     * re-announce FOCUS_GAINED on the next pump. calloc leaves it false once. */
    if (vdata->focus_announced && SDL_GetKeyboardFocus() == window) {
        /* RecreateWindow replaces window->flags and drops INPUT_FOCUS without a
         * LOST event; restore the flag so we do not emit a second GAINED. */
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
    }
    /* Defer first SetKeyboardFocus / SetMouseFocus to PumpEvents so apps that
     * register the window after CreateWindow (e.g. MAME) still see one
     * FOCUS_GAINED. Leave HIDDEN set so FinishWindowCreation → ShowWindow runs
     * our no-op KITTY_ShowWindow (avoids SDL's default SetKeyboardFocus). */
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    return true;
}

/*
 * Clear focus if this was the focus window, delete all Kitty images on the
 * tty, free RGB scratch buffers and SDL_WindowData, and null window->internal.
 */
void KITTY_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *data = (SDL_WindowData *)window->internal;

    if (vdata) {
        if (vdata->focus_window == window) {
            vdata->focus_window = NULL;
        }
        KITTY_DeleteAllImages(vdata->tty_fd);
    }
    if (data) {
        SDL_free(data->rgb_scratch);
        data->rgb_scratch = NULL;
        SDL_free(data->rgb_back);
        data->rgb_back = NULL;
        SDL_free(data);
    }
    window->internal = NULL;
}

/*
 * Apply a pending SDL window size by posting SDL_EVENT_WINDOW_RESIZED.
 * Does not rewrite tty geometry; WINCH/query paths own real pane size.
 */
void KITTY_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->pending.w, window->pending.h);
}

/*
 * Visibility is implicit in the tty framebuffer. Stub so SDL_ShowWindow does
 * not call the default SetKeyboardFocus (PumpEvents owns the single announce).
 */
void KITTY_ShowWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    (void)window;
}

/*
 * Stub so SDL_HideWindow does not call the default SetKeyboardFocus(NULL),
 * which produced FOCUS_LOST between CreateWindow/ShowWindow and the deferred
 * PumpEvents FOCUS_GAINED.
 */
void KITTY_HideWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    (void)window;
}

#endif // SDL_VIDEO_DRIVER_KITTY
