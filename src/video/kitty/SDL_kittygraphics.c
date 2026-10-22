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
#include "SDL_kittyprobe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

/* Rex rejects multi-chunk (m=1) and zlib (o=z) in practice — keep one APC.
 * ~2MiB covers a 640x400 RGB frame in base64 (~960KiB). */
#define KITTY_CHUNK (2 * 1024 * 1024)

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
                                        int place_cols, int place_rows,
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
        char header[256];
        int header_len;
        if (first) {
            /* c/r stretch when place_cols/rows set; C=1/N=1/p=1 via formatter.
             * Rex: omit m=0 on single-chunk; omit q=2. extra_keys e.g. "o=z". */
            char ctrl[192];
            const int ctrl_len = KITTY_FormatTransmitHeader(ctrl, sizeof(ctrl), 'd', image_id,
                                                            w, h, place_cols, place_rows,
                                                            more != 0, extra_keys, 0);
            if (ctrl_len <= 0) {
                SDL_free(b64);
                return SDL_SetError("transmit header format failed");
            }
            header_len = SDL_snprintf(header, sizeof(header), "\033_G%s;", ctrl);
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

static bool KITTY_TransmitDirectZlib(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h,
                                     int place_cols, int place_rows)
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
    const bool ok = KITTY_TransmitDirectPayload(fd, image_id, w, h, place_cols, place_rows,
                                                "o=z", compressed, (size_t)out_len);
    SDL_free(compressed);
    return ok;
}

static bool KITTY_TransmitDirectRaw(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h,
                                    int place_cols, int place_rows)
{
    const size_t raw_len = (size_t)w * (size_t)h * 3u;
    return KITTY_TransmitDirectPayload(fd, image_id, w, h, place_cols, place_rows, "", rgb, raw_len);
}

static bool KITTY_TransmitShm(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h,
                              int place_cols, int place_rows)
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
        char header[256];
        int header_len;

        if (encoded == 0) {
            SDL_SetError("shm name base64 failed");
            goto done;
        }
        if (!KITTY_WriteAll(fd, "\033[H", 3)) {
            goto done;
        }
        {
            char ctrl[192];
            const int ctrl_len = KITTY_FormatTransmitHeader(ctrl, sizeof(ctrl), 's', image_id,
                                                            w, h, place_cols, place_rows,
                                                            false, NULL, raw_len);
            if (ctrl_len <= 0) {
                SDL_SetError("shm header format failed");
                goto done;
            }
            header_len = SDL_snprintf(header, sizeof(header), "\033_G%s;", ctrl);
        }
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

static void KITTY_DrainTTY(SDL_VideoData *data)
{
    unsigned char junk[256];
    for (;;) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(data->tty_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            break;
        }
        if (read(data->tty_fd, junk, sizeof(junk)) <= 0) {
            break;
        }
    }
    data->tty_pending_len = 0;
}

static bool KITTY_QueryPixelSizeCSI(SDL_VideoData *data)
{
    // CSI 16 t → cell size report CSI 6 ; height ; width t
    // CSI 14 t → text-area pixels  CSI 4 ; height ; width t
    static const char query[] = "\033[16t\033[14t";
    unsigned char buf[512];
    size_t total = 0;
    int cell_w = 0, cell_h = 0, area_w = 0, area_h = 0;
    int got_cell = 0, got_area = 0;

    KITTY_DrainTTY(data);
    if (!KITTY_WriteAll(data->tty_fd, query, sizeof(query) - 1)) {
        return false;
    }

    for (int attempt = 0; attempt < 60 && !(got_cell && got_area); ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(data->tty_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        {
            const ssize_t n = read(data->tty_fd, buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) {
                continue;
            }
            total += (size_t)n;
            buf[total] = '\0';
        }

        for (size_t i = 0; i + 3 < total; ++i) {
            int a = 0, b = 0, c = 0;
            char term = 0;
            if (buf[i] != 0x1b || buf[i + 1] != '[') {
                continue;
            }
            if (SDL_sscanf((const char *)buf + i + 2, "%d;%d;%d%c", &a, &b, &c, &term) == 4 && term == 't') {
                if (a == 6 && b > 0 && c > 0) {
                    cell_h = b;
                    cell_w = c;
                    got_cell = 1;
                } else if (a == 4 && b > 0 && c > 0) {
                    area_h = b;
                    area_w = c;
                    got_area = 1;
                }
            }
        }
        if (total >= sizeof(buf) - 1) {
            break;
        }
    }

    /* Prefer CSI 14t text-area pixels for the framebuffer. On Ghostty/Rex
     * probes, CSI 16t×grid matches 14t; still prefer 14t when both arrive. */
    if (got_area) {
        data->pixel_w = area_w;
        data->pixel_h = area_h;
    }
    if (got_cell) {
        data->cell_w = cell_w;
        data->cell_h = cell_h;
    } else if (got_area && data->cols > 0 && data->rows > 0) {
        data->cell_w = area_w / data->cols;
        data->cell_h = area_h / data->rows;
    }
    if (!got_area && data->cell_w > 0 && data->cell_h > 0 && data->cols > 0 && data->rows > 0) {
        data->pixel_w = data->cols * data->cell_w;
        data->pixel_h = data->rows * data->cell_h;
    }

    return data->pixel_w > 0 && data->pixel_h > 0 && data->cell_w > 0 && data->cell_h > 0;
}

static void KITTY_UpdateMouseCoordSpace(SDL_VideoData *data)
{
    /* Track cols×cell for a safety scale if 1016 ever disagrees with CSI 14t. */
    if (data->cols > 0 && data->rows > 0 && data->cell_w > 0 && data->cell_h > 0) {
        data->mouse_coord_w = data->cols * data->cell_w;
        data->mouse_coord_h = data->rows * data->cell_h;
    } else {
        data->mouse_coord_w = data->pixel_w;
        data->mouse_coord_h = data->pixel_h;
    }
}

/* Optional cap so a single-chunk texture fits hosts that drop huge
 * multi-chunk direct RGB (observed blank on Rex at ~2800×2000).
 * With place_cells, c/r stretch fills the pane; without it the image is
 * native 1:1 at the capped size. SDL_KITTY_FB_MAX=W[xH] e.g. 800 or 800x600. */
static void KITTY_ApplyFramebufferCap(SDL_VideoData *data)
{
    const char *env;
    int max_w = 0, max_h = 0;
    int w, h;

    if (!data) {
        return;
    }
    env = SDL_getenv("SDL_KITTY_FB_MAX");
    if (env && (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
                SDL_strcasecmp(env, "none") == 0)) {
        return; /* explicit uncapped */
    }
    if (env && *env) {
        if (SDL_sscanf(env, "%dx%d", &max_w, &max_h) != 2) {
            max_w = (int)SDL_strtol(env, NULL, 10);
            max_h = max_w;
        }
    } else {
        /* Rex drops huge multi-chunk direct RGB (blank frame). */
        const char *tp = SDL_getenv("TERM_PROGRAM");
        if (tp && SDL_strcasecmp(tp, "rex") == 0) {
            max_w = 1280;
            max_h = 720;
        } else {
            return;
        }
    }
    if (max_w < 16 || max_h < 16) {
        return;
    }
    w = data->pixel_w;
    h = data->pixel_h;
    if (w <= max_w && h <= max_h) {
        return;
    }
    if (w > max_w) {
        h = h * max_w / w;
        w = max_w;
    }
    if (h > max_h) {
        w = w * max_h / h;
        h = max_h;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    data->pixel_w = w;
    data->pixel_h = h;
}

bool KITTY_QueryTerminalSize(SDL_VideoData *data)
{
    struct winsize ws;
    const int prev_cols = data->cols;
    const int prev_rows = data->rows;
    const int prev_pw = data->pixel_w;
    const int prev_ph = data->pixel_h;

    SDL_zero(ws);
    if (ioctl(data->tty_fd, TIOCGWINSZ, &ws) != 0) {
        return SDL_SetError("TIOCGWINSZ failed: %s", strerror(errno));
    }

    data->cols = ws.ws_col > 0 ? ws.ws_col : 80;
    data->rows = ws.ws_row > 0 ? ws.ws_row : 24;
    data->pixel_w = ws.ws_xpixel;
    data->pixel_h = ws.ws_ypixel;

    /* Rex often fills ws_xpixel/ws_ypixel with cell*grid (HiDPI), which is much
     * larger than the CSI 14t drawable area and produces frames the host drops.
     * Ignore ioctl pixels on Rex; prefer CSI / scaled prior area. */
    {
        const char *tp = SDL_getenv("TERM_PROGRAM");
        if (tp && SDL_strcasecmp(tp, "rex") == 0) {
            data->pixel_w = 0;
            data->pixel_h = 0;
        }
    }

    if (data->pixel_w > 0 && data->pixel_h > 0) {
        data->cell_w = data->pixel_w / data->cols;
        data->cell_h = data->pixel_h / data->rows;
        if (data->cell_w <= 0) {
            data->cell_w = 1;
        }
        if (data->cell_h <= 0) {
            data->cell_h = 1;
        }
        KITTY_ApplyFramebufferCap(data);
        KITTY_UpdateMouseCoordSpace(data);
        return true;
    }

    /* Rex often reports a stub 80x24 (CSI 14t 640x384) at process start, then
     * SIGWINCH with the real grid. Always prefer a fresh CSI 14t/16t when
     * ioctl pixels are missing so we do not scale from the stub forever. */
    if (KITTY_QueryPixelSizeCSI(data)) {
        KITTY_ApplyFramebufferCap(data);
        KITTY_UpdateMouseCoordSpace(data);
        return true;
    }

    /* Last resort: scale prior CSI area by the grid change (no CSI reply). */
    if (prev_pw > 0 && prev_ph > 0 && prev_cols > 0 && prev_rows > 0) {
        data->pixel_w = prev_pw * data->cols / prev_cols;
        data->pixel_h = prev_ph * data->rows / prev_rows;
        if (data->pixel_w <= 0) {
            data->pixel_w = 1;
        }
        if (data->pixel_h <= 0) {
            data->pixel_h = 1;
        }
        data->cell_w = data->pixel_w / data->cols;
        data->cell_h = data->pixel_h / data->rows;
        if (data->cell_w <= 0) {
            data->cell_w = 1;
        }
        if (data->cell_h <= 0) {
            data->cell_h = 1;
        }
        KITTY_ApplyFramebufferCap(data);
        KITTY_UpdateMouseCoordSpace(data);
        return true;
    }

    data->cell_w = 8;
    data->cell_h = 16;
    data->pixel_w = data->cols * data->cell_w;
    data->pixel_h = data->rows * data->cell_h;
    KITTY_ApplyFramebufferCap(data);
    KITTY_UpdateMouseCoordSpace(data);
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

static bool KITTY_WantMousePixels(void)
{
    /* Default on: SGR-pixels (1016). Set SDL_KITTY_MOUSE_PIXELS=0 for cell SGR (1006). */
    const char *env = SDL_getenv("SDL_KITTY_MOUSE_PIXELS");
    if (!env || !*env) {
        return true;
    }
    if (SDL_strcmp(env, "0") == 0 || SDL_strcasecmp(env, "false") == 0 ||
        SDL_strcasecmp(env, "no") == 0 || SDL_strcasecmp(env, "off") == 0 ||
        SDL_strcasecmp(env, "cell") == 0 || SDL_strcasecmp(env, "1006") == 0) {
        return false;
    }
    return true;
}

static bool KITTY_ModeRecognized(SDL_VideoData *data, int mode)
{
    /* DECRQM: CSI ? mode $ p  →  CSI ? mode ; value $ y
     * value 0 = unknown; 1/2/3/4 = recognized (set/reset/permanent). */
    char query[32];
    unsigned char buf[64];
    size_t total = 0;
    const int qlen = SDL_snprintf(query, sizeof(query), "\033[?%d$p", mode);
    if (qlen <= 0 || !KITTY_WriteAll(data->tty_fd, query, (size_t)qlen)) {
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(data->tty_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        const ssize_t n = read(data->tty_fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) {
            continue;
        }
        total += (size_t)n;
        buf[total] = '\0';
        for (size_t i = 0; i + 4 < total; ++i) {
            int m = 0, v = -1;
            if (buf[i] == 0x1b && buf[i + 1] == '[' &&
                SDL_sscanf((char *)buf + i + 2, "?%d;%d", &m, &v) >= 2 && m == mode) {
                return v != 0; /* 0 = not recognized */
            }
        }
        if (total >= sizeof(buf) - 1) {
            break;
        }
    }
    return false;
}

bool KITTY_SetupTerminal(SDL_VideoData *data)
{
    /* Alt screen + hide cursor + Kitty keyboard only.
     * Mouse is enabled later (KITTY_EnableMouse) after CSI 14t/16t sizing so
     * any-event reports do not flood the size-query read. */
    static const char setup[] =
        "\033[?1049h"
        "\033[?25l"
        "\033[H"
        "\033[2J"
        "\033[>1u";

    data->mouse_sgr_pixels = false;
    data->mouse_enabled = false;
    return KITTY_WriteAll(data->tty_fd, setup, sizeof(setup) - 1);
}

bool KITTY_EnableMouse(SDL_VideoData *data)
{
    /* Prefer SGR-pixels (1016) whenever the host recognizes it. Probe on
     * Ghostty and Rex showed 1016 matches CSI 14t (FB / c/r text area), so
     * pixel mouse works with place_cells stretch and with 1:1 placement.
     * Set SDL_KITTY_MOUSE_PIXELS=0 to force cell SGR (1006). */
    static const char tracking[] =
        "\033[?1000h"
        "\033[?1002h"
        "\033[?1003h";

    data->mouse_sgr_pixels = false;
    if (KITTY_WantMousePixels()) {
        data->mouse_sgr_pixels = KITTY_ModeRecognized(data, 1016);
    }

    if (!KITTY_WriteAll(data->tty_fd, tracking, sizeof(tracking) - 1)) {
        return false;
    }
    if (data->mouse_sgr_pixels) {
        if (!KITTY_WriteAll(data->tty_fd, "\033[?1016h", 8)) {
            return false;
        }
    } else if (!KITTY_WriteAll(data->tty_fd, "\033[?1006h", 8)) {
        return false;
    }
    data->mouse_enabled = true;
    return true;
}

void KITTY_RestoreTerminal(SDL_VideoData *data)
{
    static const char restore[] =
        "\033[?1016l"
        "\033[?1006l"
        "\033[?1003l"
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

bool KITTY_TransmitRGBRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                               const Uint8 *packed_rgb, int x, int y, int rw, int rh)
{
    const size_t payload_len = (size_t)rw * (size_t)rh * 3u;
    size_t b64_len;
    char *b64;
    size_t encoded;
    size_t offset = 0;
    bool first = true;

    if (!vdata || !packed_rgb || image_id == 0 || rw <= 0 || rh <= 0 || x < 0 || y < 0) {
        return SDL_SetError("invalid RGB rect edit");
    }

    b64_len = ((payload_len + 2) / 3) * 4;
    b64 = (char *)SDL_malloc(b64_len + 1);
    if (!b64) {
        return false;
    }
    encoded = KITTY_Base64Encode(packed_rgb, payload_len, b64, b64_len + 1);
    if (encoded == 0) {
        SDL_free(b64);
        return SDL_SetError("base64 encode failed");
    }

    /* Edit root frame in place — do not home cursor or re-place. */
    while (offset < encoded) {
        size_t chunk = encoded - offset;
        char header[256];
        int header_len;
        int more;
        if (chunk > KITTY_CHUNK) {
            chunk = KITTY_CHUNK;
        }
        more = (offset + chunk < encoded) ? 1 : 0;
        if (first) {
            /* r=1 edits root; X=1 overwrite; x,y,s,v = dirty bbox in image pixels. */
            if (more) {
                header_len = SDL_snprintf(header, sizeof(header),
                                          "\033_Ga=f,f=24,t=d,i=%u,r=1,X=1,x=%d,y=%d,s=%d,v=%d,m=1;",
                                          (unsigned)image_id, x, y, rw, rh);
            } else {
                header_len = SDL_snprintf(header, sizeof(header),
                                          "\033_Ga=f,f=24,t=d,i=%u,r=1,X=1,x=%d,y=%d,s=%d,v=%d;",
                                          (unsigned)image_id, x, y, rw, rh);
            }
            first = false;
        } else {
            header_len = SDL_snprintf(header, sizeof(header), "\033_Ga=f,m=%d;", more);
        }
        if (header_len <= 0) {
            SDL_free(b64);
            return SDL_SetError("frame-edit header snprintf failed");
        }
        if (!KITTY_WriteAll(vdata->tty_fd, header, (size_t)header_len) ||
            !KITTY_WriteAll(vdata->tty_fd, b64 + offset, chunk) ||
            !KITTY_WriteAll(vdata->tty_fd, "\033\\", 2)) {
            SDL_free(b64);
            return false;
        }
        offset += chunk;
    }

    SDL_free(b64);
    return true;
}

bool KITTY_PlaceExistingImage(SDL_VideoData *vdata, Uint32 image_id,
                              int place_cols, int place_rows)
{
    char cmd[160];
    int n;

    if (!vdata || image_id == 0) {
        return SDL_SetError("invalid place-existing args");
    }
    if (!KITTY_WriteAll(vdata->tty_fd, "\033[H", 3)) {
        return false;
    }
    if (place_cols > 0 && place_rows > 0) {
        n = SDL_snprintf(cmd, sizeof(cmd),
                         "\033_Ga=p,i=%u,p=1,C=1,c=%d,r=%d,q=2\033\\",
                         (unsigned)image_id, place_cols, place_rows);
    } else {
        n = SDL_snprintf(cmd, sizeof(cmd),
                         "\033_Ga=p,i=%u,p=1,C=1,q=2\033\\",
                         (unsigned)image_id);
    }
    if (n <= 0) {
        return SDL_SetError("place-existing snprintf failed");
    }
    if (!KITTY_WriteAll(vdata->tty_fd, cmd, (size_t)n)) {
        return false;
    }
    vdata->placed_cols = place_cols;
    vdata->placed_rows = place_rows;
    return true;
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

    {
        /* place_cells (default): stretch to full grid. Else 1:1 pixels. */
        const int place_cols = vdata->place_cells && vdata->cols > 0 ? vdata->cols : 0;
        const int place_rows = vdata->place_cells && vdata->rows > 0 ? vdata->rows : 0;
        bool ok = false;

        if (vdata->transmit_mode == KITTY_TRANSMIT_SHM && !vdata->shm_failed) {
            if (KITTY_TransmitShm(vdata->tty_fd, image_id, rgb, w, h, place_cols, place_rows)) {
                ok = true;
            } else {
                // Fall back for the rest of the session.
                vdata->shm_failed = true;
                vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
                SDL_ClearError();
            }
        }

        if (!ok && vdata->transmit_mode == KITTY_TRANSMIT_DIRECT_ZLIB) {
            if (KITTY_TransmitDirectZlib(vdata->tty_fd, image_id, rgb, w, h, place_cols, place_rows)) {
                ok = true;
            } else {
                vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
                SDL_ClearError();
            }
        }

        if (!ok) {
            ok = KITTY_TransmitDirectRaw(vdata->tty_fd, image_id, rgb, w, h, place_cols, place_rows);
        }
        if (ok) {
            vdata->placed_cols = place_cols;
            vdata->placed_rows = place_rows;
        }
        return ok;
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
