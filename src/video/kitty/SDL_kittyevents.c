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
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "SDL_kittyevents_c.h"
#include "SDL_kittygraphics.h"
#include "SDL_kittyframebuffer_c.h"

#include <errno.h>
#include <unistd.h>

static void KITTY_HandleWinch(SDL_VideoDevice *_this);

static SDL_Window *KITTY_FocusWindow(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    SDL_Window *window = data ? data->focus_window : NULL;
    if (!window) {
        window = SDL_GetKeyboardFocus();
    }
    if (!window) {
        int count = 0;
        SDL_Window **windows = SDL_GetWindows(&count);
        if (windows && count > 0) {
            window = windows[0];
        }
        SDL_free(windows);
    }
    return window;
}

static void KITTY_SendKey(SDL_Scancode scancode, bool pressed)
{
    SDL_SendKeyboardKey(0, SDL_GLOBAL_KEYBOARD_ID, 0, scancode, pressed);
}

static SDL_Scancode KITTY_AsciiToScancode(unsigned char c)
{
    if (c >= 'a' && c <= 'z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a'));
    }
    if (c >= 'A' && c <= 'Z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A'));
    }
    if (c >= '1' && c <= '9') {
        return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
    }
    if (c == '0') {
        return SDL_SCANCODE_0;
    }
    switch (c) {
    case 0x1b:
        return SDL_SCANCODE_ESCAPE;
    case '\r':
    case '\n':
        return SDL_SCANCODE_RETURN;
    case ' ':
        return SDL_SCANCODE_SPACE;
    case '\t':
        return SDL_SCANCODE_TAB;
    case 0x7f:
    case 0x08:
        return SDL_SCANCODE_BACKSPACE;
    case 'q':
    case 'Q':
        return SDL_SCANCODE_Q;
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

static void KITTY_HandleAscii(SDL_VideoDevice *_this, unsigned char c)
{
    SDL_Scancode sc = KITTY_AsciiToScancode(c);
    (void)_this;
    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    KITTY_SendKey(sc, true);
    KITTY_SendKey(sc, false);
    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_Q) {
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
}

static bool KITTY_ParseSGRMouse(SDL_VideoDevice *_this, const char *seq, size_t len)
{
    // seq is like "<b;x;yM" or "<b;x;ym" (without ESC[)
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    SDL_Window *window;
    int button = 0, cell_x = 0, cell_y = 0;
    char action = 0;
    float px, py;
    Uint8 sdl_button;

    if (len < 4 || seq[0] != '<') {
        return false;
    }
    if (SDL_sscanf(seq, "<%d;%d;%d%c", &button, &cell_x, &cell_y, &action) != 4) {
        return false;
    }
    if (action != 'M' && action != 'm') {
        return false;
    }

    window = KITTY_FocusWindow(_this);
    if (!window || !data) {
        return true;
    }

    px = (float)((cell_x - 1) * data->cell_w + data->cell_w / 2);
    py = (float)((cell_y - 1) * data->cell_h + data->cell_h / 2);
    SDL_SendMouseMotion(0, window, SDL_GLOBAL_MOUSE_ID, false, px, py);

    // button: 0=left,1=middle,2=right, 32+ = drag, 64+ = wheel;
    // with mode 1003, button 35 (32+3) is motion with no buttons.
    if (button >= 64) {
        const float wy = (button == 64) ? 1.0f : ((button == 65) ? -1.0f : 0.0f);
        if (wy != 0.0f) {
            SDL_SendMouseWheel(0, window, SDL_GLOBAL_MOUSE_ID, 0.0f, wy, SDL_MOUSEWHEEL_NORMAL);
        }
        return true;
    }

    if (button >= 32) {
        // Motion (with or without buttons) — position already sent above.
        return true;
    }

    switch (button) {
    case 0:
        sdl_button = SDL_BUTTON_LEFT;
        break;
    case 1:
        sdl_button = SDL_BUTTON_MIDDLE;
        break;
    case 2:
        sdl_button = SDL_BUTTON_RIGHT;
        break;
    default:
        return true;
    }
    SDL_SendMouseButton(0, window, SDL_GLOBAL_MOUSE_ID, sdl_button, action == 'M');
    return true;
}

static bool KITTY_ParseKittyKey(const char *seq, size_t len)
{
    // CSI unicode;modifiers u  — e.g. "27;1u" for Escape
    unsigned code = 0, mods = 0;
    char term = 0;
    SDL_Scancode sc;

    if (len < 2 || seq[len - 1] != 'u') {
        return false;
    }
    if (SDL_sscanf(seq, "%u;%u%c", &code, &mods, &term) < 2) {
        if (SDL_sscanf(seq, "%u%c", &code, &term) != 2 || term != 'u') {
            return false;
        }
    }
    if (code == 27) {
        sc = SDL_SCANCODE_ESCAPE;
    } else if (code == 'q' || code == 'Q') {
        sc = SDL_SCANCODE_Q;
    } else if (code >= 'a' && code <= 'z') {
        sc = (SDL_Scancode)(SDL_SCANCODE_A + (code - 'a'));
    } else if (code >= 'A' && code <= 'Z') {
        sc = (SDL_Scancode)(SDL_SCANCODE_A + (code - 'A'));
    } else {
        return true; // consumed but unmapped
    }
    KITTY_SendKey(sc, true);
    KITTY_SendKey(sc, false);
    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_Q) {
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
    return true;
}

static size_t KITTY_HandleCSI(SDL_VideoDevice *_this, const unsigned char *buf, size_t len)
{
    // buf[0] is '['; find final byte
    size_t i = 1;
    while (i < len) {
        const unsigned char ch = buf[i];
        if (ch >= 0x40 && ch <= 0x7e) {
            char seq[128];
            const size_t seq_len = i; // without leading '['
            if (seq_len < sizeof(seq)) {
                SDL_memcpy(seq, buf + 1, seq_len);
                seq[seq_len] = '\0';
                if (seq[0] == '<') {
                    KITTY_ParseSGRMouse(_this, seq, seq_len + 1);
                } else if (ch == 'u') {
                    KITTY_ParseKittyKey(seq, seq_len + 1);
                }
            }
            return i + 1;
        }
        i++;
    }
    return 0; // incomplete
}

static void KITTY_HandleWinch(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    SDL_Window *window;
    SDL_VideoDisplay *display;
    SDL_DisplayMode mode;
    void *pixels = NULL;
    int pitch = 0;
    SDL_PixelFormat format = SDL_PIXELFORMAT_UNKNOWN;

    if (!data) {
        return;
    }
    data->got_winch = 0;
    if (!KITTY_QueryTerminalSize(data)) {
        return;
    }

    display = SDL_GetVideoDisplay(SDL_GetPrimaryDisplay());
    if (display) {
        SDL_zero(mode);
        mode.format = SDL_PIXELFORMAT_XRGB8888;
        mode.w = data->pixel_w;
        mode.h = data->pixel_h;
        SDL_SetDesktopDisplayMode(display, &mode);
    }

    window = KITTY_FocusWindow(_this);
    if (!window) {
        return;
    }

    SDL_SetWindowSize(window, data->pixel_w, data->pixel_h);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, data->pixel_w, data->pixel_h);

    // Recreate framebuffer at new size.
    SDL_KITTY_DestroyWindowFramebuffer(_this, window);
    if (SDL_KITTY_CreateWindowFramebuffer(_this, window, &format, &pixels, &pitch)) {
        (void)format;
        (void)pixels;
        (void)pitch;
    }
}

void KITTY_PumpEvents(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    unsigned char buf[512];
    ssize_t n;

    if (!data) {
        return;
    }

    if (data->got_winch) {
        KITTY_HandleWinch(_this);
    }

    for (;;) {
        n = read(data->tty_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        for (ssize_t i = 0; i < n;) {
            if (buf[i] == 0x1b) {
                if (i + 1 >= n) {
                    break;
                }
                if (buf[i + 1] == '[') {
                    const size_t consumed = KITTY_HandleCSI(_this, buf + i + 1, (size_t)(n - (i + 1)));
                    if (consumed == 0) {
                        break; // wait for more
                    }
                    i += 1 + (ssize_t)consumed;
                    continue;
                }
                // lone ESC
                KITTY_HandleAscii(_this, 0x1b);
                i++;
                continue;
            }
            KITTY_HandleAscii(_this, buf[i]);
            i++;
        }
        if (n < (ssize_t)sizeof(buf)) {
            break;
        }
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
