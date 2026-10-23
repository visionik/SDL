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

/*
 * MAME (and similar) drain the full SDL event queue then sample keyboard
 * state. A same-queue down+up leaves state cleared. Keep keys down for a
 * minimum time so at least one input poll sees them pressed.
 */
#define KITTY_KEY_HOLD_MS  80u
#define KITTY_KEY_HOLD_MAX 64
typedef struct KITTY_KeyHold
{
    SDL_Scancode sc;
    Uint64 earliest_up_ms;
    bool want_up;
    bool down;
} KITTY_KeyHold;

static KITTY_KeyHold KITTY_key_holds[KITTY_KEY_HOLD_MAX];
static int KITTY_key_hold_n;

static KITTY_KeyHold *KITTY_FindHold(SDL_Scancode sc, bool create)
{
    int i;
    for (i = 0; i < KITTY_key_hold_n; ++i) {
        if (KITTY_key_holds[i].sc == sc) {
            return &KITTY_key_holds[i];
        }
    }
    if (!create || KITTY_key_hold_n >= KITTY_KEY_HOLD_MAX) {
        return NULL;
    }
    KITTY_key_holds[KITTY_key_hold_n].sc = sc;
    KITTY_key_holds[KITTY_key_hold_n].earliest_up_ms = 0;
    KITTY_key_holds[KITTY_key_hold_n].want_up = false;
    KITTY_key_holds[KITTY_key_hold_n].down = false;
    return &KITTY_key_holds[KITTY_key_hold_n++];
}

static void KITTY_RemoveHoldAt(int idx)
{
    if (idx < 0 || idx >= KITTY_key_hold_n) {
        return;
    }
    KITTY_key_holds[idx] = KITTY_key_holds[KITTY_key_hold_n - 1];
    KITTY_key_hold_n--;
}

static void KITTY_FlushDueKeyUps(void)
{
    const Uint64 now = SDL_GetTicks();
    int i = 0;
    while (i < KITTY_key_hold_n) {
        KITTY_KeyHold *h = &KITTY_key_holds[i];
        if (h->down && h->want_up && now >= h->earliest_up_ms) {
            SDL_SendKeyboardKey(0, SDL_GLOBAL_KEYBOARD_ID, 0, h->sc, false);
            KITTY_RemoveHoldAt(i);
            continue;
        }
        i++;
    }
}

static void KITTY_SendKey(SDL_Scancode scancode, bool pressed)
{
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    SDL_SendKeyboardKey(0, SDL_GLOBAL_KEYBOARD_ID, 0, scancode, pressed);
}

static void KITTY_LogKey(SDL_Scancode sc, int event_type, const char *via)
{
    if (!SDL_getenv("SDL_KITTY_KEY_DEBUG")) {
        return;
    }
    /* Prefer stderr: LogDebug on INPUT is often filtered, and stdout is the
     * Kitty graphics channel once the alt-screen is owned. */
    SDL_Log("kitty key sc=%d event=%d via=%s", (int)sc, event_type, via ? via : "?");
}

/* Kitty event-type subfield: 1=press (default), 2=repeat, 3=release. */
static void KITTY_ApplyKeyEvent(SDL_Scancode sc, int event_type, bool has_release)
{
    KITTY_KeyHold *h;
    const Uint64 now = SDL_GetTicks();

    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    KITTY_LogKey(sc, event_type, has_release ? "csi" : "legacy");

    if (event_type == 3) {
        h = KITTY_FindHold(sc, false);
        if (h && h->down) {
            h->want_up = true;
            if (now >= h->earliest_up_ms) {
                KITTY_SendKey(sc, false);
                /* remove: linear search again after mutation */
                {
                    int i;
                    for (i = 0; i < KITTY_key_hold_n; ++i) {
                        if (KITTY_key_holds[i].sc == sc) {
                            KITTY_RemoveHoldAt(i);
                            break;
                        }
                    }
                }
            }
        } else {
            KITTY_SendKey(sc, false);
        }
        return;
    }

    if (event_type == 2) {
        KITTY_SendKey(sc, true);
        h = KITTY_FindHold(sc, true);
        if (h) {
            h->down = true;
            h->want_up = false;
            h->earliest_up_ms = now + KITTY_KEY_HOLD_MS;
        }
        return;
    }

    /* Press */
    KITTY_SendKey(sc, true);
    h = KITTY_FindHold(sc, true);
    if (h) {
        h->down = true;
        h->earliest_up_ms = now + KITTY_KEY_HOLD_MS;
        /* Legacy (no release reports): schedule up after min hold. */
        h->want_up = !has_release;
    }
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
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

/* Kitty mods field is (1 + bitmask). Bit0=shift … see keyboard protocol. */
static SDL_Keymod KITTY_ModsToSDL(unsigned mods_field)
{
    const unsigned bits = (mods_field > 0) ? (mods_field - 1u) : 0u;
    SDL_Keymod m = SDL_KMOD_NONE;
    if (bits & 1u) {
        m |= SDL_KMOD_SHIFT;
    }
    if (bits & 2u) {
        m |= SDL_KMOD_ALT;
    }
    if (bits & 4u) {
        m |= SDL_KMOD_CTRL;
    }
    if (bits & 8u) {
        m |= SDL_KMOD_GUI;
    }
    if (bits & 64u) {
        m |= SDL_KMOD_CAPS;
    }
    if (bits & 128u) {
        m |= SDL_KMOD_NUM;
    }
    return m;
}

static SDL_Scancode KITTY_CodeToScancode(unsigned code)
{
    if (code >= 'a' && code <= 'z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (code - 'a'));
    }
    if (code >= 'A' && code <= 'Z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (code - 'A'));
    }
    if (code >= '1' && code <= '9') {
        return (SDL_Scancode)(SDL_SCANCODE_1 + (code - '1'));
    }
    if (code == '0') {
        return SDL_SCANCODE_0;
    }
    switch (code) {
    case 27:
        return SDL_SCANCODE_ESCAPE;
    case 13:
        return SDL_SCANCODE_RETURN;
    case 9:
        return SDL_SCANCODE_TAB;
    case 127:
    case 8:
        return SDL_SCANCODE_BACKSPACE;
    case 32:
        return SDL_SCANCODE_SPACE;
    case 57358:
        return SDL_SCANCODE_CAPSLOCK;
    case 57359:
        return SDL_SCANCODE_SCROLLLOCK;
    case 57360:
        return SDL_SCANCODE_NUMLOCKCLEAR;
    case 57361:
        return SDL_SCANCODE_PRINTSCREEN;
    case 57362:
        return SDL_SCANCODE_PAUSE;
    case 57363:
        return SDL_SCANCODE_MENU;
    case 57417:
        return SDL_SCANCODE_LEFT;
    case 57418:
        return SDL_SCANCODE_RIGHT;
    case 57419:
        return SDL_SCANCODE_UP;
    case 57420:
        return SDL_SCANCODE_DOWN;
    /* Modifiers (flag 8 reports these as CSI-u). */
    case 57441:
        return SDL_SCANCODE_LSHIFT;
    case 57442:
        return SDL_SCANCODE_LCTRL;
    case 57443:
        return SDL_SCANCODE_LALT;
    case 57444:
        return SDL_SCANCODE_LGUI;
    case 57447:
        return SDL_SCANCODE_RSHIFT;
    case 57448:
        return SDL_SCANCODE_RCTRL;
    case 57449:
        return SDL_SCANCODE_RALT;
    case 57450:
        return SDL_SCANCODE_RGUI;
    default:
        break;
    }
    /* F1–F12 as CSI-u use legacy numbers under 30 in some hosts; PUA F13+. */
    if (code >= 57376 && code <= 57387) {
        return (SDL_Scancode)(SDL_SCANCODE_F13 + (int)(code - 57376));
    }
    return SDL_SCANCODE_UNKNOWN;
}

static SDL_Scancode KITTY_LegacyFinalToScancode(unsigned char final_byte, unsigned number)
{
    switch (final_byte) {
    case 'A':
        return SDL_SCANCODE_UP;
    case 'B':
        return SDL_SCANCODE_DOWN;
    case 'C':
        return SDL_SCANCODE_RIGHT;
    case 'D':
        return SDL_SCANCODE_LEFT;
    case 'H':
        return SDL_SCANCODE_HOME;
    case 'F':
        return SDL_SCANCODE_END;
    case 'P':
        return SDL_SCANCODE_F1;
    case 'Q':
        return SDL_SCANCODE_F2;
    case 'R':
        return SDL_SCANCODE_F3;
    case 'S':
        return SDL_SCANCODE_F4;
    case '~':
        switch (number) {
        case 2:
            return SDL_SCANCODE_INSERT;
        case 3:
            return SDL_SCANCODE_DELETE;
        case 5:
            return SDL_SCANCODE_PAGEUP;
        case 6:
            return SDL_SCANCODE_PAGEDOWN;
        case 7:
            return SDL_SCANCODE_HOME;
        case 8:
            return SDL_SCANCODE_END;
        case 11:
            return SDL_SCANCODE_F1;
        case 12:
            return SDL_SCANCODE_F2;
        case 13:
            return SDL_SCANCODE_F3;
        case 14:
            return SDL_SCANCODE_F4;
        case 15:
            return SDL_SCANCODE_F5;
        case 17:
            return SDL_SCANCODE_F6;
        case 18:
            return SDL_SCANCODE_F7;
        case 19:
            return SDL_SCANCODE_F8;
        case 20:
            return SDL_SCANCODE_F9;
        case 21:
            return SDL_SCANCODE_F10;
        case 23:
            return SDL_SCANCODE_F11;
        case 24:
            return SDL_SCANCODE_F12;
        default:
            return SDL_SCANCODE_UNKNOWN;
        }
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

static void KITTY_HandleAscii(SDL_VideoDevice *_this, unsigned char c)
{
    SDL_Scancode sc = KITTY_AsciiToScancode(c);
    (void)_this;
    /* Legacy text keys have no release; hold until next pump. */
    KITTY_ApplyKeyEvent(sc, 1, false);
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
                (void)write(data->tty_out, line, (size_t)n);
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

/*
 * Parse Kitty CSI-u body (no leading '['), e.g. "97;1:3u", "27u", "97:65;5u".
 * Event types (when flag 2 is on): 1=press, 2=repeat, 3=release.
 */
static bool KITTY_ParseKittyKey(const char *seq, size_t len)
{
    unsigned code = 0;
    unsigned mods = 1;
    unsigned event = 1;
    size_t i = 0;
    SDL_Scancode sc;
    bool has_event_field = false;

    if (len < 2 || seq[len - 1] != 'u') {
        return false;
    }
    while (i < len - 1 && seq[i] >= '0' && seq[i] <= '9') {
        code = code * 10u + (unsigned)(seq[i] - '0');
        i++;
    }
    /* Skip :shifted:base alternates before ';' or 'u'. */
    while (i < len - 1 && seq[i] != ';') {
        i++;
    }
    if (i < len - 1 && seq[i] == ';') {
        i++;
        mods = 0;
        while (i < len - 1 && seq[i] >= '0' && seq[i] <= '9') {
            mods = mods * 10u + (unsigned)(seq[i] - '0');
            i++;
        }
        if (i < len - 1 && seq[i] == ':') {
            i++;
            event = 0;
            has_event_field = true;
            while (i < len - 1 && seq[i] >= '0' && seq[i] <= '9') {
                event = event * 10u + (unsigned)(seq[i] - '0');
                i++;
            }
            if (event == 0) {
                event = 1;
            }
        }
        if (mods == 0) {
            mods = 1;
        }
    }
    /*
     * Kitty: omitted event-type subfield means press (1), not "no releases".
     * Rex sends digit presses as CSI 53 u and releases as CSI 53;1:3u.
     * We always enable flag 2 (report event types), so CSI-u always has
     * real releases — never arm the legacy min-hold auto-up path here.
     */
    (void)has_event_field;
    SDL_SetModState(KITTY_ModsToSDL(mods));
    sc = KITTY_CodeToScancode(code);
    KITTY_ApplyKeyEvent(sc, (int)event, true /* has_release */);
    return true;
}

/* CSI 1;mods:event A  /  CSI A  /  CSI 15;mods~  etc. */
static void KITTY_ParseLegacyCSIKey(const char *seq, size_t len, unsigned char final_byte)
{
    unsigned number = 1;
    unsigned mods = 1;
    unsigned event = 1;
    bool has_event_field = false;
    size_t i = 0;
    SDL_Scancode sc;

    if (len == 0) {
        return;
    }
    if (seq[0] >= '0' && seq[0] <= '9') {
        number = 0;
        while (i < len && seq[i] >= '0' && seq[i] <= '9') {
            number = number * 10u + (unsigned)(seq[i] - '0');
            i++;
        }
        if (i < len && seq[i] == ';') {
            i++;
            mods = 0;
            while (i < len && seq[i] >= '0' && seq[i] <= '9') {
                mods = mods * 10u + (unsigned)(seq[i] - '0');
                i++;
            }
            if (i < len && seq[i] == ':') {
                i++;
                event = 0;
                has_event_field = true;
                while (i < len && seq[i] >= '0' && seq[i] <= '9') {
                    event = event * 10u + (unsigned)(seq[i] - '0');
                    i++;
                }
                if (event == 0) {
                    event = 1;
                }
            }
            if (mods == 0) {
                mods = 1;
            }
        }
    }
    /* With flag 2, Rex sends CSI 1;1:1A / 1;1:3A — treat as release-capable
     * whenever the event subfield is present; also when omitted after we
     * requested event types, still expect a later :3 (same as CSI-u). */
    SDL_SetModState(KITTY_ModsToSDL(mods));
    sc = KITTY_LegacyFinalToScancode(final_byte, number);
    KITTY_ApplyKeyEvent(sc, (int)event, true /* has_release */);
    (void)has_event_field;
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
                /* seq holds intermediate bytes + final (e.g. "53u", "1;1:3A"). */
                SDL_memcpy(seq, buf + 1, seq_len);
                seq[seq_len] = '\0';
                if (seq[0] == '<') {
                    KITTY_ParseSGRMouse(_this, seq, seq_len);
                } else if (ch == 'u') {
                    /* Must pass seq_len (not +1): ParseKittyKey checks seq[len-1]=='u'.
                     * Off-by-one made every CSI-u fail (digits/letters/Esc); arrows
                     * still worked via the legacy A/B/C/D path. */
                    KITTY_ParseKittyKey(seq, seq_len);
                } else if (ch == '~' || ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D' ||
                           ch == 'H' || ch == 'F' || ch == 'P' || ch == 'Q' || ch == 'R' ||
                           ch == 'S') {
                    KITTY_ParseLegacyCSIKey(seq, seq_len, ch);
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

    if (KITTY_DebugEnabled()) {
        const char *an = "full";
        if (action == KITTY_RESIZE_REPLACE) {
            an = "replace";
        } else if (action == KITTY_RESIZE_NONE) {
            an = "none";
        }
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                     "kitty: resize action=%s fb %dx%d->%dx%d grid %dx%d->%dx%d live=%u",
                     an, old_pw, old_ph, data->pixel_w, data->pixel_h,
                     old_cols, old_rows, data->cols, data->rows,
                     wdata ? (unsigned)wdata->live_image_id : 0u);
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
        mode.format = KITTY_SelectFbAlpha(SDL_getenv("SDL_KITTY_FB_ALPHA"))
                          ? SDL_PIXELFORMAT_ARGB8888
                          : SDL_PIXELFORMAT_XRGB8888;
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

    /* Release keys whose minimum hold elapsed. */
    KITTY_FlushDueKeyUps();

    /*
     * CreateWindow emits FOCUS_GAINED before some apps (MAME) register the
     * window id. Re-announce once from the pump so has_focus()/input poll works.
     */
    if (!data->focus_announced && data->focus_window) {
        data->focus_announced = true;
        data->focus_window->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        SDL_SetKeyboardFocus(data->focus_window);
        SDL_SetMouseFocus(data->focus_window);
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

        n = read(data->tty_in, readbuf, room < sizeof(readbuf) ? room : sizeof(readbuf));
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

                /* SS3 arrows / F1–F4: ESC O A|B|C|D|P|Q|R|S */
                if (buf[i + 1] == 'O') {
                    if (i + 2 >= len) {
                        break;
                    }
                    {
                        const unsigned char f = buf[i + 2];
                        const SDL_Scancode sc = KITTY_LegacyFinalToScancode(f, 1);
                        KITTY_ApplyKeyEvent(sc, 1, false);
                    }
                    i += 3;
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
