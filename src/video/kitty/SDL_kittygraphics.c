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

#include "SDL_kittygraphics.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define KITTY_CHUNK 4096

static struct termios KITTY_saved_termios;
static bool KITTY_termios_saved = false;

static const char KITTY_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t KITTY_Base64Encode(const Uint8 *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t i = 0, o = 0;
    while (i + 2 < src_len) {
        if (o + 4 >= dst_cap) {
            break;
        }
        const Uint32 n = ((Uint32)src[i] << 16) | ((Uint32)src[i + 1] << 8) | src[i + 2];
        dst[o++] = KITTY_b64_table[(n >> 18) & 63];
        dst[o++] = KITTY_b64_table[(n >> 12) & 63];
        dst[o++] = KITTY_b64_table[(n >> 6) & 63];
        dst[o++] = KITTY_b64_table[n & 63];
        i += 3;
    }
    if (i < src_len && o + 4 < dst_cap) {
        Uint32 n = (Uint32)src[i] << 16;
        dst[o++] = KITTY_b64_table[(n >> 18) & 63];
        if (i + 1 < src_len) {
            n |= (Uint32)src[i + 1] << 8;
            dst[o++] = KITTY_b64_table[(n >> 12) & 63];
            dst[o++] = KITTY_b64_table[(n >> 6) & 63];
            dst[o++] = '=';
        } else {
            dst[o++] = KITTY_b64_table[(n >> 12) & 63];
            dst[o++] = '=';
            dst[o++] = '=';
        }
    }
    if (o < dst_cap) {
        dst[o] = '\0';
    }
    return o;
}

static bool KITTY_WriteAll(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SDL_SetError("kitty write failed: %s", strerror(errno));
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

bool KITTY_ProbeSupport(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return false;
    }

    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (hint && SDL_strstr(hint, KITTYVID_DRIVER_NAME) != NULL) {
        // Forced on: still require a tty, but skip env heuristics.
        return true;
    }

    const char *term_program = SDL_getenv("TERM_PROGRAM");
    if (term_program) {
        if (SDL_strcasecmp(term_program, "kitty") == 0 ||
            SDL_strcasecmp(term_program, "ghostty") == 0 ||
            SDL_strcasecmp(term_program, "WezTerm") == 0 ||
            SDL_strcasecmp(term_program, "rex") == 0) {
            return true;
        }
    }
    if (SDL_getenv("KITTY_WINDOW_ID") != NULL) {
        return true;
    }
    const char *term = SDL_getenv("TERM");
    if (term && (SDL_strstr(term, "kitty") || SDL_strstr(term, "ghostty"))) {
        return true;
    }
    return false;
}

bool KITTY_QueryTerminalSize(SDL_VideoData *data)
{
    struct winsize ws;
    SDL_zero(ws);
    if (ioctl(data->tty_fd, TIOCGWINSZ, &ws) != 0) {
        return SDL_SetError("TIOCGWINSZ failed: %s", strerror(errno));
    }

    data->cols = ws.ws_col > 0 ? ws.ws_col : 80;
    data->rows = ws.ws_row > 0 ? ws.ws_row : 24;
    data->pixel_w = ws.ws_xpixel;
    data->pixel_h = ws.ws_ypixel;

    if (data->pixel_w <= 0 || data->pixel_h <= 0) {
        // Fallback cell size guess when the tty does not report pixels.
        data->cell_w = 10;
        data->cell_h = 20;
        data->pixel_w = data->cols * data->cell_w;
        data->pixel_h = data->rows * data->cell_h;
    } else {
        data->cell_w = data->pixel_w / data->cols;
        data->cell_h = data->pixel_h / data->rows;
        if (data->cell_w <= 0) {
            data->cell_w = 1;
        }
        if (data->cell_h <= 0) {
            data->cell_h = 1;
        }
    }
    return true;
}

bool KITTY_EnterRawMode(SDL_VideoData *data)
{
    struct termios raw;
    if (tcgetattr(data->tty_fd, &KITTY_saved_termios) != 0) {
        return SDL_SetError("tcgetattr failed: %s", strerror(errno));
    }
    KITTY_termios_saved = true;
    raw = KITTY_saved_termios;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(data->tty_fd, TCSANOW, &raw) != 0) {
        return SDL_SetError("tcsetattr failed: %s", strerror(errno));
    }
    return true;
}

void KITTY_LeaveRawMode(SDL_VideoData *data)
{
    if (KITTY_termios_saved) {
        tcsetattr(data->tty_fd, TCSANOW, &KITTY_saved_termios);
        KITTY_termios_saved = false;
    }
}

bool KITTY_SetupTerminal(SDL_VideoData *data)
{
    // Alternate screen, hide cursor, enable Kitty keyboard protocol (all events).
    static const char setup[] =
        "\033[?1049h"
        "\033[?25l"
        "\033[H"
        "\033[2J"
        "\033[>1u";
    return KITTY_WriteAll(data->tty_fd, setup, sizeof(setup) - 1);
}

void KITTY_RestoreTerminal(SDL_VideoData *data)
{
    static const char restore[] =
        "\033[<u"
        "\033[?25h"
        "\033[?1049l";
    KITTY_DeleteAllImages(data->tty_fd);
    KITTY_WriteAll(data->tty_fd, restore, sizeof(restore) - 1);
    KITTY_LeaveRawMode(data);
}

bool KITTY_DeleteAllImages(int fd)
{
    static const char del[] = "\033_Ga=d,d=A,q=2\033\\";
    return KITTY_WriteAll(fd, del, sizeof(del) - 1);
}

bool KITTY_TransmitRGB(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0) {
        return SDL_SetError("invalid RGB buffer");
    }

    const size_t raw_len = (size_t)w * (size_t)h * 3u;
    const size_t b64_len = ((raw_len + 2) / 3) * 4;
    char *b64 = (char *)SDL_malloc(b64_len + 1);
    if (!b64) {
        return false;
    }
    const size_t encoded = KITTY_Base64Encode(rgb, raw_len, b64, b64_len + 1);
    if (encoded == 0) {
        SDL_free(b64);
        return SDL_SetError("base64 encode failed");
    }

    // Move cursor home so a=T places at the top-left.
    if (!KITTY_WriteAll(fd, "\033[H", 3)) {
        SDL_free(b64);
        return false;
    }

    size_t offset = 0;
    bool first = true;
    while (offset < encoded) {
        size_t chunk = encoded - offset;
        if (chunk > KITTY_CHUNK) {
            chunk = KITTY_CHUNK;
        }
        const int more = (offset + chunk < encoded) ? 1 : 0;
        char header[128];
        int header_len;
        if (first) {
            header_len = SDL_snprintf(header, sizeof(header),
                                      "\033_Ga=T,f=24,t=d,i=%u,s=%d,v=%d,q=2,m=%d;",
                                      (unsigned)image_id, w, h, more);
            first = false;
        } else {
            header_len = SDL_snprintf(header, sizeof(header),
                                      "\033_Gm=%d;", more);
        }
        if (header_len <= 0) {
            SDL_free(b64);
            return SDL_SetError("header snprintf failed");
        }
        if (!KITTY_WriteAll(fd, header, (size_t)header_len) ||
            !KITTY_WriteAll(fd, b64 + offset, chunk) ||
            !KITTY_WriteAll(fd, "\033\\", 2)) {
            SDL_free(b64);
            return false;
        }
        offset += chunk;
    }

    SDL_free(b64);
    return true;
}

#endif // SDL_VIDEO_DRIVER_KITTY
