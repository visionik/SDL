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
#include "SDL_kittyprobe.h"

#include <errno.h>
#include <stdio.h>
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

    /* 1016: 1-based pixels in the CSI 14t text-area space.
     * 1006: 1-based cells — map into FB via pixel_w/cols. */
    data->mouse_raw_x = cell_x > 0 ? cell_x - 1 : 0;
    data->mouse_raw_y = cell_y > 0 ? cell_y - 1 : 0;
    if (data->mouse_sgr_pixels ||
        cell_x > data->cols + 1 || cell_y > data->rows + 1 ||
        data->cols <= 0 || data->rows <= 0 ||
        data->pixel_w <= 0 || data->pixel_h <= 0) {
        /* place_cells stretch: image fills the pane, so map full CSI/mouse
         * space into the (possibly capped) FB. 1:1 placement: image sits at
         * native size (typically top-left); use raw 1016 coords — do not
         * squash the whole pane into the dark rectangle. */
        if (data->mouse_sgr_pixels && data->place_cells &&
            data->mouse_coord_w > 0 && data->mouse_coord_h > 0 &&
            data->pixel_w > 0 && data->pixel_h > 0 &&
            (data->mouse_coord_w != data->pixel_w || data->mouse_coord_h != data->pixel_h)) {
            px = ((float)data->mouse_raw_x + 0.5f) * (float)data->pixel_w / (float)data->mouse_coord_w;
            py = ((float)data->mouse_raw_y + 0.5f) * (float)data->pixel_h / (float)data->mouse_coord_h;
        } else {
            px = (float)data->mouse_raw_x;
            py = (float)data->mouse_raw_y;
        }
    } else {
        px = ((float)data->mouse_raw_x + 0.5f) * (float)data->pixel_w / (float)data->cols;
        py = ((float)data->mouse_raw_y + 0.5f) * (float)data->pixel_h / (float)data->rows;
    }
    if (SDL_getenv("SDL_KITTY_MOUSE_DEBUG")) {
        static int dbg_n;
        if ((dbg_n++ % 8) == 0) {
            char line[192];
            const int n = SDL_snprintf(line, sizeof(line),
                                       "\rKITTY mouse raw=%d,%d  sdl=%.0f,%.0f  fb=%dx%d  mcoord=%dx%d  place=%d  \r\n",
                                       data->mouse_raw_x, data->mouse_raw_y, px, py,
                                       data->pixel_w, data->pixel_h,
                                       data->mouse_coord_w, data->mouse_coord_h,
                                       data->place_cells ? 1 : 0);
            if (n > 0) {
                (void)write(data->tty_fd, line, (size_t)n);
            }
        }
    }
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
    SDL_WindowData *wdata;
    SDL_VideoDisplay *display;
    SDL_DisplayMode mode;
    int old_pw, old_ph, old_cols, old_rows;
    KITTY_ResizeAction action;
    bool has_live;

    if (!data) {
        return;
    }
    old_pw = data->pixel_w;
    old_ph = data->pixel_h;
    old_cols = data->cols;
    old_rows = data->rows;

    data->got_winch = 0;
    if (!KITTY_QueryTerminalSize(data)) {
        return;
    }

    window = KITTY_FocusWindow(_this);
    wdata = window ? (SDL_WindowData *)window->internal : NULL;
    has_live = wdata && wdata->placed && wdata->live_image_id != 0;
    action = KITTY_ClassifyResize(data->place_cells, has_live,
                                  old_pw, old_ph, data->pixel_w, data->pixel_h,
                                  old_cols, old_rows, data->cols, data->rows);

    {
        const char *dbg = SDL_getenv("SDL_KITTY_DEBUG");
        if (dbg && dbg[0] && dbg[0] != '0') {
            FILE *log = fopen("/tmp/sdl_kitty_debug.log", "a");
            if (log) {
                const char *an = "full";
                if (action == KITTY_RESIZE_REPLACE) {
                    an = "replace";
                } else if (action == KITTY_RESIZE_NONE) {
                    an = "none";
                }
                fprintf(log,
                        "resize action=%s fb %dx%d->%dx%d grid %dx%d->%dx%d live=%u\n",
                        an, old_pw, old_ph, data->pixel_w, data->pixel_h,
                        old_cols, old_rows, data->cols, data->rows,
                        wdata ? (unsigned)wdata->live_image_id : 0u);
                fclose(log);
            }
        }
    }

    if (action == KITTY_RESIZE_NONE) {
        return;
    }

    if (action == KITTY_RESIZE_REPLACE) {
        /* Same FB pixels: only update Kitty c/r stretch. Skip SDL surface recreate. */
        if (!KITTY_PlaceExistingImage(data, wdata->live_image_id, data->cols, data->rows)) {
            action = KITTY_RESIZE_FULL; /* fall through to full resize path */
        } else {
            return;
        }
    }

    display = SDL_GetVideoDisplay(SDL_GetPrimaryDisplay());
    if (display) {
        SDL_zero(mode);
        mode.format = SDL_PIXELFORMAT_XRGB8888;
        mode.w = data->pixel_w;
        mode.h = data->pixel_h;
        SDL_SetDesktopDisplayMode(display, &mode);
    }

    if (!window) {
        return;
    }

    /* Full resize. SDL_OnWindowPixelSizeChanged clears surface_valid; the app
     * must call SDL_GetWindowSurface() again, which recreates the framebuffer. */
    SDL_SetWindowSize(window, data->pixel_w, data->pixel_h);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, data->pixel_w, data->pixel_h);
}

void KITTY_PumpEvents(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    unsigned char readbuf[512];
    ssize_t n;

    if (!data) {
        return;
    }

    if (data->got_winch) {
        KITTY_HandleWinch(_this);
    }

    for (;;) {
        const size_t room = KITTY_TTY_PENDING_MAX - data->tty_pending_len;
        if (room == 0) {
            /* Pathological incomplete sequence larger than the carry buffer.
             * Drop it so the pump cannot stall forever. */
            data->tty_pending_len = 0;
            continue;
        }

        n = read(data->tty_fd, readbuf, room < sizeof(readbuf) ? room : sizeof(readbuf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        SDL_memcpy(data->tty_pending + data->tty_pending_len, readbuf, (size_t)n);
        data->tty_pending_len += (size_t)n;

        {
            size_t i = 0;
            while (i < data->tty_pending_len) {
                const unsigned char *buf = data->tty_pending;
                const size_t len = data->tty_pending_len;

                if (buf[i] != 0x1b) {
                    KITTY_HandleAscii(_this, buf[i]);
                    i++;
                    continue;
                }

                /* Need at least ESC + one byte to classify the sequence. */
                if (i + 1 >= len) {
                    break;
                }

                if (buf[i + 1] == '[') {
                    const size_t consumed = KITTY_HandleCSI(_this, buf + i + 1, len - (i + 1));
                    if (consumed == 0) {
                        break; /* incomplete CSI — keep from ESC onward */
                    }
                    i += 1 + consumed;
                    continue;
                }

                /* APC/PM/SOS (\033_ / \033^ / \033P) end with BEL or ST (\033\\).
                 * Drain without treating payload as keys (graphics OK replies). */
                if (buf[i + 1] == '_' || buf[i + 1] == '^' || buf[i + 1] == 'P') {
                    size_t j = i + 2;
                    int done = 0;
                    while (j < len) {
                        if (buf[j] == 0x07) {
                            done = 1;
                            j++;
                            break;
                        }
                        if (buf[j] == 0x1b && j + 1 < len && buf[j + 1] == '\\') {
                            done = 1;
                            j += 2;
                            break;
                        }
                        j++;
                    }
                    if (!done) {
                        break; /* incomplete APC — keep from ESC onward */
                    }
                    i = j;
                    continue;
                }

                /* ESC followed by something else: treat as Escape key. */
                KITTY_HandleAscii(_this, 0x1b);
                i++;
            }

            if (i > 0) {
                data->tty_pending_len -= i;
                if (data->tty_pending_len > 0) {
                    SDL_memmove(data->tty_pending, data->tty_pending + i, data->tty_pending_len);
                }
            }
        }

        /* Short read: no more bytes waiting on a typical non-blocking tty. */
        if ((size_t)n < sizeof(readbuf) && (size_t)n < room) {
            break;
        }
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
