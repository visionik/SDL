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
#include "SDL_kittyevents_c.h"
#include "SDL_kittygraphics.h"

#include <errno.h>
#include <unistd.h>

// Minimal keyboard: ASCII bytes + a few CSI sequences. Kitty keyboard
// protocol (CSI u) is parsed for plain keys when present.

static void KITTY_SendKey(SDL_Window *window, SDL_Scancode scancode, bool pressed)
{
    if (!window) {
        return;
    }
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

static void KITTY_HandleByte(SDL_VideoDevice *_this, unsigned char c)
{
    SDL_Window *window = SDL_GetKeyboardFocus();
    SDL_Scancode sc;
    (void)_this;

    if (!window) {
        window = SDL_GetWindowFromID(1);
    }

    sc = KITTY_AsciiToScancode(c);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    // Synthesize press+release for raw bytes (no key-up in classic tty).
    KITTY_SendKey(window, sc, true);
    KITTY_SendKey(window, sc, false);

    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_Q) {
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
}

void KITTY_PumpEvents(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    unsigned char buf[256];
    ssize_t n;

    if (!data) {
        return;
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
        for (ssize_t i = 0; i < n; ++i) {
            // Skip incomplete escape noise for MVP; handle plain bytes and ESC.
            if (buf[i] == 0x1b && i + 1 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                // Drain CSI / SS3 sequence roughly.
                i++;
                while (i + 1 < n) {
                    i++;
                    const unsigned char ch = buf[i];
                    if ((ch >= 0x40 && ch <= 0x7e) || ch == 'u') {
                        break;
                    }
                }
                continue;
            }
            KITTY_HandleByte(_this, buf[i]);
        }
        if (n < (ssize_t)sizeof(buf)) {
            break;
        }
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
