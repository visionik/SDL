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

#include "../../events/SDL_events_c.h"
#include "../SDL_pixels_c.h"
#include "../SDL_sysvideo.h"

#include "SDL_kittyevents_c.h"
#include "SDL_kittyframebuffer_c.h"
#include "SDL_kittygraphics.h"
#include "SDL_kittyprobe.h"
#include "SDL_kittyprofile.h"
#include "SDL_kittyvideo.h"
#include "SDL_kittywindow.h"

#include <signal.h>
#include <unistd.h>

static bool KITTY_VideoInit(SDL_VideoDevice *_this);
static void KITTY_VideoQuit(SDL_VideoDevice *_this);

static SDL_VideoData *KITTY_winch_data = NULL;
static void (*KITTY_prev_winch)(int) = SIG_DFL;

static void KITTY_HandleSIGWINCH(int sig)
{
    (void)sig;
    if (KITTY_winch_data) {
        KITTY_winch_data->got_winch = 1;
    }
}

static bool KITTY_Available(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (!(hint && SDL_strstr(hint, KITTYVID_DRIVER_NAME) != NULL)) {
        // Never auto-select; require SDL_VIDEO_DRIVER=kitty
        return false;
    }
    return KITTY_ProbeSupport();
}

static void KITTY_DeleteDevice(SDL_VideoDevice *device)
{
    if (device->internal) {
        SDL_free(device->internal);
        device->internal = NULL;
    }
    SDL_free(device);
}

static SDL_VideoDevice *KITTY_CreateDevice(void)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;

    if (!KITTY_Available()) {
        return NULL;
    }

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    data = (SDL_VideoData *)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!data) {
        SDL_free(device);
        return NULL;
    }
    data->term_profile = KITTY_DetectTerminalProfile();
    /* Writes always go to stdout. Reads: profile.keys_on_stdin (Rex) when
     * stdin is a tty; else stdout. */
    data->tty_out = STDOUT_FILENO;
    if (data->term_profile->keys_on_stdin && isatty(STDIN_FILENO)) {
        data->tty_in = STDIN_FILENO;
    } else {
        data->tty_in = isatty(STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;
    }
    data->tty_fd = data->tty_out;
    /* Initial guess; ProbeTransmitModes + profile refine after terminal setup. */
    data->transmit_mode = data->term_profile->prefer_direct ? KITTY_TRANSMIT_DIRECT
                                                           : KITTY_TRANSMIT_SHM;
    data->shm_failed = false;
    data->frame_edit_ok = false;
    /* Stretch FB to the cell grid by default; SDL_KITTY_PLACE_CELLS=0 for 1:1. */
    {
        const char *place = SDL_getenv("SDL_KITTY_PLACE_CELLS");
        data->place_cells = !(place && (place[0] == '0' || SDL_strcasecmp(place, "false") == 0 ||
                                        SDL_strcasecmp(place, "no") == 0 || SDL_strcasecmp(place, "off") == 0));
    }
    device->internal = data;

    device->VideoInit = KITTY_VideoInit;
    device->VideoQuit = KITTY_VideoQuit;
    device->PumpEvents = KITTY_PumpEvents;
    device->CreateWindowFramebuffer = SDL_KITTY_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SDL_KITTY_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_KITTY_DestroyWindowFramebuffer;
    device->CreateSDLWindow = KITTY_CreateWindow;
    device->DestroyWindow = KITTY_DestroyWindow;
    device->SetWindowSize = KITTY_SetWindowSize;
    device->free = KITTY_DeleteDevice;

    return device;
}

VideoBootStrap KITTY_bootstrap = {
    KITTYVID_DRIVER_NAME, "SDL Kitty graphics protocol video driver",
    KITTY_CreateDevice,
    NULL,
    false
};

static bool KITTY_VideoInit(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    SDL_DisplayMode mode;
    struct sigaction sa, old_sa;

    if (!KITTY_EnterRawMode(data)) {
        return false;
    }
    if (!KITTY_SetupTerminal(data)) {
        KITTY_LeaveRawMode(data);
        return false;
    }
    data->terminal_owned = true;
    data->keyboard_enabled = true;

    /* Install WINCH before sizing waits so Rex pane layout is observed. */
    KITTY_winch_data = data;
    data->got_winch = 0;
    SDL_zero(sa);
    sa.sa_handler = KITTY_HandleSIGWINCH;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGWINCH, &sa, &old_sa) == 0) {
        KITTY_prev_winch = old_sa.sa_handler;
    }

    /* Size before mouse: any-event mode floods the tty and can starve CSI 14t. */
    if (!KITTY_QueryTerminalSize(data)) {
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
        return false;
    }

    /* Rex may spawn at stub 80x24 then deliver SIGWINCH once the pane is laid
     * out. Wait briefly for a non-stub size before enabling mouse / display. */
    {
        for (int i = 0; i < 40; ++i) {
            if (!(data->cols == 80 && data->rows == 24)) {
                break;
            }
            SDL_Delay(25);
            if (data->got_winch) {
                data->got_winch = 0;
                (void)KITTY_QueryTerminalSize(data);
                continue;
            }
            if ((i % 4) == 3) {
                (void)KITTY_QueryTerminalSize(data);
            }
        }
    }

    /* a=q probe: pick SHM vs direct (Rex stays direct unless forced). */
    (void)KITTY_ProbeTransmitModes(data);

    {
        const char *no_mouse = SDL_getenv("SDL_KITTY_MOUSE");
        const bool want_mouse = !(no_mouse && (no_mouse[0] == '0' ||
                                               SDL_strcasecmp(no_mouse, "false") == 0 ||
                                               SDL_strcasecmp(no_mouse, "off") == 0 ||
                                               SDL_strcasecmp(no_mouse, "no") == 0));
        if (want_mouse) {
            if (!KITTY_EnableMouse(data)) {
                KITTY_RestoreTerminal(data);
                data->terminal_owned = false;
                return false;
            }
        } else {
            data->mouse_enabled = false;
            data->mouse_sgr_pixels = false;
        }
    }

    SDL_zero(mode);
    mode.format = KITTY_SelectFbAlpha(SDL_getenv("SDL_KITTY_FB_ALPHA"))
                      ? SDL_PIXELFORMAT_ARGB8888
                      : SDL_PIXELFORMAT_XRGB8888;
    mode.w = data->pixel_w;
    mode.h = data->pixel_h;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
        return false;
    }

    if (KITTY_DebugEnabled()) {
        KITTY_DebugLog(
            "kitty: init term=%s cols=%d rows=%d fb=%dx%d cell=%dx%d place=%d mouse1016=%d",
            data->term_profile ? data->term_profile->name : "?",
            data->cols, data->rows, data->pixel_w, data->pixel_h,
            data->cell_w, data->cell_h, data->place_cells ? 1 : 0,
            data->mouse_sgr_pixels ? 1 : 0);
    }

    return true;
}

static void KITTY_VideoQuit(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    struct sigaction sa;

    if (KITTY_winch_data == data) {
        SDL_zero(sa);
        sa.sa_handler = KITTY_prev_winch;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, NULL);
        KITTY_winch_data = NULL;
        KITTY_prev_winch = SIG_DFL;
    }

    if (data && data->terminal_owned) {
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
