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
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

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

static bool KITTY_TransmitDirectPayload(int fd, Uint32 image_id, int w, int h,
                                        const char *extra_keys,
                                        const Uint8 *payload, size_t payload_len)
{
    const size_t b64_len = ((payload_len + 2) / 3) * 4;
    char *b64 = (char *)SDL_malloc(b64_len + 1);
    if (!b64) {
        return false;
    }
    const size_t encoded = KITTY_Base64Encode(payload, payload_len, b64, b64_len + 1);
    if (encoded == 0) {
        SDL_free(b64);
        return SDL_SetError("base64 encode failed");
    }

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
        char header[192];
        int header_len;
        if (first) {
            header_len = SDL_snprintf(header, sizeof(header),
                                      "\033_Ga=T,f=24,t=d,i=%u,s=%d,v=%d,q=2,m=%d%s;",
                                      (unsigned)image_id, w, h, more,
                                      extra_keys ? extra_keys : "");
            first = false;
        } else {
            header_len = SDL_snprintf(header, sizeof(header), "\033_Gm=%d;", more);
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

static bool KITTY_TransmitDirectZlib(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h)
{
    const uLong raw_len = (uLong)w * (uLong)h * 3u;
    uLongf bound = compressBound(raw_len);
    Uint8 *compressed = (Uint8 *)SDL_malloc((size_t)bound);
    if (!compressed) {
        return false;
    }
    uLongf out_len = bound;
    const int zerr = compress2(compressed, &out_len, rgb, raw_len, Z_BEST_SPEED);
    if (zerr != Z_OK) {
        SDL_free(compressed);
        return SDL_SetError("zlib compress2 failed (%d)", zerr);
    }
    const bool ok = KITTY_TransmitDirectPayload(fd, image_id, w, h, ",o=z", compressed, (size_t)out_len);
    SDL_free(compressed);
    return ok;
}

static bool KITTY_TransmitDirectRaw(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h)
{
    const size_t raw_len = (size_t)w * (size_t)h * 3u;
    return KITTY_TransmitDirectPayload(fd, image_id, w, h, "", rgb, raw_len);
}

static bool KITTY_TransmitShm(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h)
{
    char name[64];
    const size_t raw_len = (size_t)w * (size_t)h * 3u;
    int shm_fd = -1;
    void *map = MAP_FAILED;
    bool ok = false;

    SDL_snprintf(name, sizeof(name), "/sdl-kitty-%d-%u", (int)getpid(), (unsigned)image_id);
    shm_unlink(name); // clear stale name if any

    shm_fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (shm_fd < 0) {
        return SDL_SetError("shm_open failed: %s", strerror(errno));
    }
    if (ftruncate(shm_fd, (off_t)raw_len) != 0) {
        SDL_SetError("ftruncate shm failed: %s", strerror(errno));
        goto done;
    }
    map = mmap(NULL, raw_len, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) {
        SDL_SetError("mmap shm failed: %s", strerror(errno));
        goto done;
    }
    SDL_memcpy(map, rgb, raw_len);

    {
        char name_b64[128];
        const size_t nlen = SDL_strlen(name);
        const size_t encoded = KITTY_Base64Encode((const Uint8 *)name, nlen, name_b64, sizeof(name_b64));
        char header[192];
        int header_len;

        if (encoded == 0) {
            SDL_SetError("shm name base64 failed");
            goto done;
        }
        if (!KITTY_WriteAll(fd, "\033[H", 3)) {
            goto done;
        }
        header_len = SDL_snprintf(header, sizeof(header),
                                  "\033_Ga=T,f=24,t=s,i=%u,s=%d,v=%d,S=%zu,q=2;",
                                  (unsigned)image_id, w, h, raw_len);
        if (header_len <= 0) {
            SDL_SetError("shm header snprintf failed");
            goto done;
        }
        if (!KITTY_WriteAll(fd, header, (size_t)header_len) ||
            !KITTY_WriteAll(fd, name_b64, encoded) ||
            !KITTY_WriteAll(fd, "\033\\", 2)) {
            goto done;
        }
        ok = true;
    }

done:
    if (map != MAP_FAILED) {
        munmap(map, raw_len);
    }
    if (shm_fd >= 0) {
        close(shm_fd);
    }
    // Terminal deletes the object after reading; unlink defensively if transmit failed.
    if (!ok) {
        shm_unlink(name);
    }
    return ok;
}

bool KITTY_ProbeSupport(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return false;
    }

    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (hint && SDL_strstr(hint, KITTYVID_DRIVER_NAME) != NULL) {
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
    (void)data;
    if (KITTY_termios_saved) {
        tcsetattr(data->tty_fd, TCSANOW, &KITTY_saved_termios);
        KITTY_termios_saved = false;
    }
}

bool KITTY_SetupTerminal(SDL_VideoData *data)
{
    // Alt screen, hide cursor, Kitty keyboard, SGR mouse (click+drag).
    static const char setup[] =
        "\033[?1049h"
        "\033[?25l"
        "\033[H"
        "\033[2J"
        "\033[>1u"
        "\033[?1000h"
        "\033[?1002h"
        "\033[?1006h";
    data->mouse_enabled = true;
    return KITTY_WriteAll(data->tty_fd, setup, sizeof(setup) - 1);
}

void KITTY_RestoreTerminal(SDL_VideoData *data)
{
    static const char restore[] =
        "\033[?1006l"
        "\033[?1002l"
        "\033[?1000l"
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

bool KITTY_DeleteImage(int fd, Uint32 image_id)
{
    char cmd[64];
    const int n = SDL_snprintf(cmd, sizeof(cmd), "\033_Ga=d,d=I,i=%u,q=2\033\\", (unsigned)image_id);
    if (n <= 0) {
        return false;
    }
    return KITTY_WriteAll(fd, cmd, (size_t)n);
}

bool KITTY_TransmitRGB(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h)
{
    if (!vdata || !rgb || w <= 0 || h <= 0) {
        return SDL_SetError("invalid RGB buffer");
    }

    const char *force = SDL_getenv("SDL_KITTY_TRANSMIT");
    if (force) {
        if (SDL_strcasecmp(force, "direct") == 0) {
            vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
        } else if (SDL_strcasecmp(force, "zlib") == 0) {
            vdata->transmit_mode = KITTY_TRANSMIT_DIRECT_ZLIB;
        } else if (SDL_strcasecmp(force, "shm") == 0) {
            vdata->transmit_mode = KITTY_TRANSMIT_SHM;
        }
    }

    if (vdata->transmit_mode == KITTY_TRANSMIT_SHM && !vdata->shm_failed) {
        if (KITTY_TransmitShm(vdata->tty_fd, image_id, rgb, w, h)) {
            return true;
        }
        // Fall back for the rest of the session.
        vdata->shm_failed = true;
        vdata->transmit_mode = KITTY_TRANSMIT_DIRECT_ZLIB;
        SDL_ClearError();
    }

    if (vdata->transmit_mode == KITTY_TRANSMIT_DIRECT_ZLIB) {
        if (KITTY_TransmitDirectZlib(vdata->tty_fd, image_id, rgb, w, h)) {
            return true;
        }
        vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
        SDL_ClearError();
    }

    return KITTY_TransmitDirectRaw(vdata->tty_fd, image_id, rgb, w, h);
}

#endif // SDL_VIDEO_DRIVER_KITTY
