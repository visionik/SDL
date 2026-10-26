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

/*
 * Kitty graphics protocol over the tty: raw mode, CSI sizing, SGR mouse,
 * and APC image transmit (POSIX SHM, zlib, or chunked base64 direct).
 */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#include "SDL_kittygraphics.h"
#include "SDL_kittyprobe.h"
#include "SDL_kittyprofile.h"

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

/* Fallback APC chunk when no profile is available yet. */
#define KITTY_CHUNK_DEFAULT ((size_t)2 * 1024 * 1024)

static struct termios KITTY_saved_termios;
static bool KITTY_termios_saved = false;

static const char KITTY_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Encode src as standard base64 into dst (NUL-terminated when capacity
 * allows). Returns bytes written excluding the terminator.
 */
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

/*
 * Write the full buffer to fd, retrying on EINTR. Sets SDL error on failure.
 */
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

/*
 * Chunked direct (t=d) a=T transmit: base64-encode payload, optionally
 * home with CSI CUP, then write APC chunks sized by chunk_bytes.
 * extra_keys may add o=z or overlay z/X/Y. out_wire_bytes gets bytes sent.
 */
static bool KITTY_TransmitDirectPayload(int fd, Uint32 image_id, int w, int h,
                                        int place_cols, int place_rows,
                                        const char *extra_keys,
                                        const Uint8 *payload, size_t payload_len,
                                        int kitty_f, bool home_cursor,
                                        size_t chunk_bytes,
                                        size_t *out_wire_bytes)
{
    const size_t b64_len = ((payload_len + 2) / 3) * 4;
    const size_t chunk_cap = chunk_bytes > 0 ? chunk_bytes : KITTY_CHUNK_DEFAULT;
    size_t wire = 0;
    char *b64 = (char *)SDL_malloc(b64_len + 1);
    if (!b64) {
        return false;
    }
    const size_t encoded = KITTY_Base64Encode(payload, payload_len, b64, b64_len + 1);
    if (encoded == 0) {
        SDL_free(b64);
        return SDL_SetError("base64 encode failed");
    }

    /* Prefer KITTY_WritePlaceOrigin at the call site (viewport CUP). This
     * fallback homes to 1;1 when a caller still passes home_cursor. */
    if (home_cursor) {
        if (!KITTY_WriteAll(fd, "\033[H", 3)) {
            SDL_free(b64);
            return false;
        }
        wire += 3;
    }

    size_t offset = 0;
    bool first = true;
    while (offset < encoded) {
        size_t chunk = encoded - offset;
        if (chunk > chunk_cap) {
            chunk = chunk_cap;
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
                                                            more != 0, extra_keys, 0, kitty_f);
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
        wire += (size_t)header_len + chunk + 2;
        offset += chunk;
    }

    SDL_free(b64);
    if (out_wire_bytes) {
        *out_wire_bytes = wire;
    }
    return true;
}

/*
 * zlib-compress RGB/RGBA then transmit as direct a=T with o=z.
 * out_raw_bytes receives compressed payload size (pre-base64).
 */
static bool KITTY_TransmitDirectZlib(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h,
                                     int place_cols, int place_rows, int kitty_f, bool home_cursor,
                                     size_t chunk_bytes, size_t *out_wire_bytes, size_t *out_raw_bytes)
{
    const int bpp = (kitty_f == 32) ? 4 : 3;
    const uLong raw_len = (uLong)w * (uLong)h * (uLong)bpp;
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
    if (out_raw_bytes) {
        *out_raw_bytes = (size_t)out_len; /* compressed payload size (pre-b64) */
    }
    {
        const bool ok = KITTY_TransmitDirectPayload(fd, image_id, w, h, place_cols, place_rows,
                                                    "o=z", compressed, (size_t)out_len, kitty_f,
                                                    home_cursor, chunk_bytes, out_wire_bytes);
        SDL_free(compressed);
        return ok;
    }
}

/*
 * Transmit uncompressed RGB/RGBA as direct a=T (no o=z).
 * out_raw_bytes receives the raw pixel byte count.
 */
static bool KITTY_TransmitDirectRaw(int fd, Uint32 image_id, const Uint8 *rgb, int w, int h,
                                    int place_cols, int place_rows, int kitty_f, bool home_cursor,
                                    size_t chunk_bytes, size_t *out_wire_bytes, size_t *out_raw_bytes)
{
    const int bpp = (kitty_f == 32) ? 4 : 3;
    const size_t raw_len = (size_t)w * (size_t)h * (size_t)bpp;
    if (out_raw_bytes) {
        *out_raw_bytes = raw_len;
    }
    return KITTY_TransmitDirectPayload(fd, image_id, w, h, place_cols, place_rows, "", rgb, raw_len,
                                       kitty_f, home_cursor, chunk_bytes, out_wire_bytes);
}

/*
 * POSIX SHM transmit (t=s): unique /sdl-kitty-* name per call, mmap+copy
 * pixels, send base64 path in one APC. Unlink on failure or when the
 * profile sets shm_unlink_after_send; otherwise the host unlinks.
 */
static bool KITTY_TransmitShm(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h,
                              int place_cols, int place_rows, int kitty_f, bool home_cursor)
{
    char name[80];
    const int bpp = (kitty_f == 32) ? 4 : 3;
    const size_t raw_len = (size_t)w * (size_t)h * (size_t)bpp;
    const Uint32 seq = vdata ? ++vdata->shm_seq : 1u;
    int shm_fd = -1;
    void *map = MAP_FAILED;
    bool ok = false;
    int fd;

    if (!vdata) {
        return SDL_SetError("invalid SHM transmit");
    }
    fd = vdata->tty_fd;

    SDL_snprintf(name, sizeof(name), "/sdl-kitty-%d-%u-%u", (int)getpid(), (unsigned)image_id,
                 (unsigned)seq);
    shm_unlink(name);

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
        if (home_cursor && !KITTY_WritePlaceOrigin(vdata)) {
            goto done;
        }
        {
            char ctrl[192];
            const int ctrl_len = KITTY_FormatTransmitHeader(ctrl, sizeof(ctrl), 's', image_id,
                                                            w, h, place_cols, place_rows,
                                                            false, NULL, raw_len, kitty_f);
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
    /* Protocol: the terminal unlinks after reading. Only unlink on failure, or
     * when the profile explicitly asks for client unlink (legacy / buggy hosts). */
    if (!ok || (vdata->term_profile && vdata->term_profile->shm_unlink_after_send)) {
        shm_unlink(name);
    }
    return ok;
}

/*
 * Return true when stdout is a tty and the host looks like a Kitty-protocol
 * terminal (SDL_HINT_VIDEO_DRIVER, loaded profile match, or builtin env).
 */
bool KITTY_ProbeSupport(void)
{
    /* Need a tty for graphics out; keyboard prefers stdin when available. */
    if (!isatty(STDOUT_FILENO) && !isatty(STDIN_FILENO)) {
        return false;
    }
    if (!isatty(STDOUT_FILENO)) {
        return false;
    }

    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (hint && SDL_strstr(hint, KITTYVID_DRIVER_NAME) != NULL) {
        return true;
    }

    /* Prefer matchers from loaded profiles (config/terminals.toml). */
    if (KITTY_EnvMatchesLoadedProfile()) {
        return true;
    }
    return KITTY_BuiltinEnvLooksLikeKittyHost();
}

/*
 * Non-blocking drain of tty_in and clear the pending CSI/APC carry buffer.
 * Used before size queries so stale replies are not misparsed.
 */
static void KITTY_DrainTTY(SDL_VideoData *data)
{
    unsigned char junk[256];
    for (;;) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_in, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
            break;
        }
        if (read(data->tty_in, junk, sizeof(junk)) <= 0) {
            break;
        }
    }
    data->tty_pending_len = 0;
}

/*
 * Query cell size (CSI 16 t → 6;h;w t) and text-area pixels (CSI 14 t →
 * 4;h;w t). Prefer 14t for pixel_w/h; derive cells from area/grid if needed.
 */
static bool KITTY_QueryPixelSizeCSI(SDL_VideoData *data)
{
    static const char query[] = "\033[16t\033[14t";
    unsigned char buf[512];
    size_t total = 0;
    int cell_w = 0, cell_h = 0, area_w = 0, area_h = 0;
    int got_cell = 0, got_area = 0;

    KITTY_DrainTTY(data);
    if (!KITTY_WriteAll(data->tty_out, query, sizeof(query) - 1)) {
        return false;
    }

    for (int attempt = 0; attempt < 60 && !(got_cell && got_area); ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_in, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        {
            const ssize_t n = read(data->tty_in, buf + total, sizeof(buf) - 1 - total);
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

/*
 * Set mouse_coord_w/h from cols×cell when known; else fall back to pixel_w/h.
 * Used if SGR-pixels (1016) ever disagree with CSI 14t.
 */
static void KITTY_UpdateMouseCoordSpace(SDL_VideoData *data)
{
    if (data->cols > 0 && data->rows > 0 && data->cell_w > 0 && data->cell_h > 0) {
        data->mouse_coord_w = data->cols * data->cell_w;
        data->mouse_coord_h = data->rows * data->cell_h;
    } else {
        data->mouse_coord_w = data->pixel_w;
        data->mouse_coord_h = data->pixel_h;
    }
}

/*
 * Optional size policy so a single-chunk texture fits hosts that drop huge
 * multi-chunk direct RGB (observed blank on Rex at ~2800×2000).
 * With place_cells, c/r stretch fills the pane; without it the image is
 * native 1:1 at the chosen size.
 *
 * SDL_KITTY_FB_FIXED=WxH — exact FB size (resize keeps pixels; a=p re-stretch).
 * SDL_KITTY_FB_MAX=W[xH] — aspect-fit into a max box (e.g. 800 or 800x600).
 * Unset → transport-aware profile soft-max (shm vs direct; docs/terminals.md).
 */
void KITTY_ApplyFramebufferCap(SDL_VideoData *data)
{
    const char *fixed;
    const char *env;
    const KITTY_TerminalProfile *prof;
    int max_w = 0, max_h = 0;
    int w, h;

    if (!data) {
        return;
    }

    fixed = SDL_getenv("SDL_KITTY_FB_FIXED");
    if (fixed && *fixed &&
        !(fixed[0] == '0' || SDL_strcasecmp(fixed, "off") == 0 ||
          SDL_strcasecmp(fixed, "none") == 0)) {
        int fw = 0, fh = 0;
        if (SDL_sscanf(fixed, "%dx%d", &fw, &fh) != 2) {
            fw = (int)SDL_strtol(fixed, NULL, 10);
            fh = fw;
        }
        if (fw >= 16 && fh >= 16) {
            data->pixel_w = fw;
            data->pixel_h = fh;
            KITTY_UpdateMouseCoordSpace(data);
            return;
        }
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
        prof = data->term_profile;
        if (prof) {
            const bool use_shm =
                (data->transmit_mode == KITTY_TRANSMIT_SHM && !data->shm_failed);
            if (use_shm && prof->default_fb_max_shm_w > 0 &&
                prof->default_fb_max_shm_h > 0) {
                max_w = prof->default_fb_max_shm_w;
                max_h = prof->default_fb_max_shm_h;
            } else if (!use_shm && prof->default_fb_max_direct_w > 0 &&
                       prof->default_fb_max_direct_h > 0) {
                max_w = prof->default_fb_max_direct_w;
                max_h = prof->default_fb_max_direct_h;
            } else if (prof->default_fb_max_w > 0 && prof->default_fb_max_h > 0) {
                max_w = prof->default_fb_max_w;
                max_h = prof->default_fb_max_h;
            }
        }
        if (max_w < 16 || max_h < 16) {
            return; /* profile says no default cap */
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
    KITTY_UpdateMouseCoordSpace(data);
}

void KITTY_ApplyViewportEnv(SDL_VideoData *data)
{
    const char *vp;
    const char *alt;
    int col = 0, row = 0, w = 0, h = 0;

    if (!data) {
        return;
    }
    data->vp_col = 0;
    data->vp_row = 0;
    data->vp_cols = 0;
    data->vp_rows = 0;
    data->use_alt_screen = true;

    vp = SDL_getenv("SDL_KITTY_VIEWPORT");
    if (vp && *vp && SDL_sscanf(vp, "%d,%d,%d,%d", &col, &row, &w, &h) == 4 &&
        col > 0 && row > 0 && w > 0 && h > 0) {
        data->vp_col = col;
        data->vp_row = row;
        data->vp_cols = w;
        data->vp_rows = h;
        /* Card mode: leave primary buffer for TUI chrome unless forced on. */
        data->use_alt_screen = false;
    }

    alt = SDL_getenv("SDL_KITTY_ALTSCREEN");
    if (alt && *alt) {
        if (alt[0] == '0' || SDL_strcasecmp(alt, "false") == 0 ||
            SDL_strcasecmp(alt, "off") == 0 || SDL_strcasecmp(alt, "no") == 0) {
            data->use_alt_screen = false;
        } else {
            data->use_alt_screen = true;
        }
    }
}

void KITTY_ClampViewport(SDL_VideoData *data)
{
    if (!data || data->vp_cols <= 0 || data->vp_rows <= 0) {
        return;
    }
    if (data->cols > 0 && data->vp_col > data->cols) {
        data->vp_col = data->cols;
    }
    if (data->rows > 0 && data->vp_row > data->rows) {
        data->vp_row = data->rows;
    }
    if (data->cols > 0 && data->vp_col + data->vp_cols - 1 > data->cols) {
        data->vp_cols = data->cols - data->vp_col + 1;
    }
    if (data->rows > 0 && data->vp_row + data->vp_rows - 1 > data->rows) {
        data->vp_rows = data->rows - data->vp_row + 1;
    }
    if (data->vp_cols < 1) {
        data->vp_cols = 1;
    }
    if (data->vp_rows < 1) {
        data->vp_rows = 1;
    }
}

void KITTY_GetPlaceCells(const SDL_VideoData *data, int *out_cols, int *out_rows)
{
    int pc = 0, pr = 0;
    if (data && data->place_cells) {
        if (data->vp_cols > 0 && data->vp_rows > 0) {
            pc = data->vp_cols;
            pr = data->vp_rows;
        } else if (data->cols > 0 && data->rows > 0) {
            pc = data->cols;
            pr = data->rows;
        }
    }
    if (out_cols) {
        *out_cols = pc;
    }
    if (out_rows) {
        *out_rows = pr;
    }
}

bool KITTY_WritePlaceOrigin(SDL_VideoData *data)
{
    char buf[32];
    int n;
    if (!data) {
        return false;
    }
    if (data->vp_cols > 0 && data->vp_rows > 0 && data->vp_col > 0 && data->vp_row > 0) {
        n = SDL_snprintf(buf, sizeof(buf), "\033[%d;%dH", data->vp_row, data->vp_col);
    } else {
        n = SDL_snprintf(buf, sizeof(buf), "\033[H");
    }
    if (n <= 0) {
        return false;
    }
    return KITTY_WriteAll(data->tty_fd, buf, (size_t)n);
}

/*
 * Refresh cols/rows via TIOCGWINSZ; fill pixels from ioctl, CSI 14t/16t,
 * or scaled prior size; then apply FB caps and mouse coord space.
 */
bool KITTY_QueryTerminalSize(SDL_VideoData *data)
{
    struct winsize ws;
    const int prev_cols = data->cols;
    const int prev_rows = data->rows;
    const int prev_pw = data->pixel_w;
    const int prev_ph = data->pixel_h;

    SDL_zero(ws);
    if (ioctl(data->tty_in, TIOCGWINSZ, &ws) != 0) {
        return SDL_SetError("TIOCGWINSZ failed: %s", strerror(errno));
    }

    data->cols = ws.ws_col > 0 ? ws.ws_col : 80;
    data->rows = ws.ws_row > 0 ? ws.ws_row : 24;
    data->pixel_w = ws.ws_xpixel;
    data->pixel_h = ws.ws_ypixel;
    KITTY_ClampViewport(data);

    /* Some hosts fill ws_xpixel/ws_ypixel with cell*grid (HiDPI), which is much
     * larger than the CSI 14t drawable area and produces frames the host drops.
     * Profile ignore_ioctl_pixels (Rex) clears them; prefer CSI / scaled prior. */
    if (data->term_profile && data->term_profile->ignore_ioctl_pixels) {
        data->pixel_w = 0;
        data->pixel_h = 0;
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

/*
 * Put tty_in into raw mode (cfmakeraw) while keeping ISIG for Ctrl+C/Z.
 * Saves prior termios for KITTY_LeaveRawMode; VMIN/VTIME are non-blocking.
 */
bool KITTY_EnterRawMode(SDL_VideoData *data)
{
    struct termios raw;
    /* Raw mode must apply to the input tty so keyboard reads see uncooked bytes. */
    if (tcgetattr(data->tty_in, &KITTY_saved_termios) != 0) {
        return SDL_SetError("tcgetattr failed: %s", strerror(errno));
    }
    KITTY_termios_saved = true;
    raw = KITTY_saved_termios;
    cfmakeraw(&raw);
    /* Keep ISIG so Ctrl+C / Ctrl+Z still generate signals — otherwise a
     * stuck app with a broken quit path cannot be killed from the tty. */
    raw.c_lflag |= ISIG;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(data->tty_in, TCSANOW, &raw) != 0) {
        return SDL_SetError("tcsetattr failed: %s", strerror(errno));
    }
    return true;
}

/*
 * Restore saved termios on tty_in if KITTY_EnterRawMode succeeded earlier.
 */
void KITTY_LeaveRawMode(SDL_VideoData *data)
{
    if (KITTY_termios_saved && data) {
        tcsetattr(data->tty_in, TCSANOW, &KITTY_saved_termios);
        KITTY_termios_saved = false;
    }
}

/*
 * Swallow unread APC/CSI (e.g. Gi=1,p=1;OK) so the next shell prompt
 * does not treat graphics replies as typed input. Quiet waits and pass
 * count come from the terminal profile (defaults 8×15ms).
 */
static void KITTY_DrainInput(SDL_VideoData *data)
{
    unsigned char junk[512];
    int passes;
    const int max_passes =
        (data && data->term_profile && data->term_profile->drain_passes > 0)
            ? data->term_profile->drain_passes
            : 8;
    const int quiet_us =
        (data && data->term_profile && data->term_profile->drain_quiet_us > 0)
            ? data->term_profile->drain_quiet_us
            : 15000;

    if (!data || data->tty_in < 0) {
        return;
    }
    data->tty_pending_len = 0;

    /* Non-blocking drain, then a few short waits for late host replies
     * (delete / last a=T without q=2 often arrive after restore starts). */
    for (passes = 0; passes < max_passes; ++passes) {
        for (;;) {
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(data->tty_in, &fds);
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
                break;
            }
            if (read(data->tty_in, junk, sizeof(junk)) <= 0) {
                break;
            }
        }
        if (passes < max_passes - 1) {
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(data->tty_in, &fds);
            tv.tv_sec = 0;
            tv.tv_usec = quiet_us;
            if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
                break;
            }
            (void)read(data->tty_in, junk, sizeof(junk));
        }
    }
}

/*
 * Default true (SGR-pixels / 1016). SDL_KITTY_MOUSE_PIXELS=0|false|no|off|cell|1006
 * forces cell SGR (1006).
 */
static bool KITTY_WantMousePixels(void)
{
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

/*
 * DECRQM probe: CSI ? mode $ p → CSI ? mode ; value $ y.
 * Returns true when value is non-zero (recognized set/reset/permanent).
 */
static bool KITTY_ModeRecognized(SDL_VideoData *data, int mode)
{
    char query[32];
    unsigned char buf[64];
    size_t total = 0;
    const int qlen = SDL_snprintf(query, sizeof(query), "\033[?%d$p", mode);
    if (qlen <= 0 || !KITTY_WriteAll(data->tty_out, query, (size_t)qlen)) {
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_in, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        const ssize_t n = read(data->tty_in, buf + total, sizeof(buf) - 1 - total);
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

/*
 * Query whether the host honored Kitty keyboard push (CSI > 27 u).
 * Sends CSI ? u and waits briefly for CSI ? <flags> u. WezTerm only replies
 * when enable_kitty_keyboard=true; otherwise the query is ignored.
 */
static bool KITTY_ProbeKittyKeyboard(SDL_VideoData *data)
{
    static const char query[] = "\033[?u";
    unsigned char buf[64];
    size_t total = 0;

    if (!data || data->tty_in < 0 || data->tty_out < 0) {
        return false;
    }
    if (!KITTY_WriteAll(data->tty_out, query, sizeof(query) - 1)) {
        return false;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(data->tty_in, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(data->tty_in + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        {
            const ssize_t n = read(data->tty_in, buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) {
                continue;
            }
            total += (size_t)n;
            buf[total] = '\0';
        }
        for (size_t i = 0; i + 3 < total; ++i) {
            unsigned flags = 0;
            if (buf[i] == 0x1b && buf[i + 1] == '[' && buf[i + 2] == '?' &&
                SDL_sscanf((char *)buf + i + 3, "%u", &flags) == 1) {
                /* Reply is CSI ? <flags> u — confirm trailing 'u'. */
                const char *p = (char *)buf + i + 3;
                while (*p >= '0' && *p <= '9') {
                    p++;
                }
                if (*p == 'u') {
                    return true;
                }
            }
        }
        if (total >= sizeof(buf) - 1) {
            break;
        }
    }
    return false;
}

/*
 * Enter alt screen, hide cursor, home/clear, and request Kitty keyboard
 * (CSI > 27 u = flags 1|2|8|16: disambiguate, event types, all keys as CSI,
 * associated text). Then probe with CSI ? u; sets data->kitty_keyboard_ok.
 * Mouse is deferred to KITTY_EnableMouse so reports do not flood size queries.
 */
bool KITTY_SetupTerminal(SDL_VideoData *data)
{
    /* Full-pane: alt screen + clear. Card mode (use_alt_screen=false): keep
     * primary buffer so a host TUI can draw chrome around the viewport. */
    static const char setup_alt[] =
        "\033[?1049h"
        "\033[?25l"
        "\033[H"
        "\033[2J"
        "\033[>27u";
    static const char setup_card[] =
        "\033[?25l"
        "\033[>27u";

    data->mouse_sgr_pixels = false;
    data->mouse_enabled = false;
    data->place_homed = false;
    data->placed_cols = 0;
    data->placed_rows = 0;
    data->kitty_keyboard_ok = false;
    if (data->use_alt_screen) {
        if (!KITTY_WriteAll(data->tty_out, setup_alt, sizeof(setup_alt) - 1)) {
            return false;
        }
    } else if (!KITTY_WriteAll(data->tty_out, setup_card, sizeof(setup_card) - 1)) {
        return false;
    }
    data->kitty_keyboard_ok = KITTY_ProbeKittyKeyboard(data);
    if (!data->kitty_keyboard_ok && data->term_profile && data->term_profile->name &&
        SDL_strcasecmp(data->term_profile->name, "wezterm") == 0) {
        /* Always warn (not RUNTIME_DEBUG): games need press/release. */
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "kitty: WezTerm kitty keyboard inactive (CSI ? u unanswered). "
                    "Set enable_kitty_keyboard = true in wezterm.lua for "
                    "press/release keys; until then arrow holds use OS key-repeat.");
    }
#ifdef SDL_KITTY_RUNTIME_DEBUG
    if (KITTY_DebugEnabled()) {
        KITTY_DebugLog("kitty: keyboard_protocol %s",
                       data->kitty_keyboard_ok ? "ok" : "off");
    }
#endif
    return true;
}

/*
 * Enable SGR mouse tracking (1000/1002/1003). Prefer SGR-pixels (1016)
 * when DECRQM recognizes it and WantMousePixels is true; else 1006.
 */
bool KITTY_EnableMouse(SDL_VideoData *data)
{
    static const char tracking[] =
        "\033[?1000h"
        "\033[?1002h"
        "\033[?1003h";

    data->mouse_sgr_pixels = false;
    if (KITTY_WantMousePixels()) {
        data->mouse_sgr_pixels = KITTY_ModeRecognized(data, 1016);
    }

    if (!KITTY_WriteAll(data->tty_out, tracking, sizeof(tracking) - 1)) {
        return false;
    }
    if (data->mouse_sgr_pixels) {
        if (!KITTY_WriteAll(data->tty_out, "\033[?1016h", 8)) {
            return false;
        }
    } else if (!KITTY_WriteAll(data->tty_out, "\033[?1006h", 8)) {
        return false;
    }
    data->mouse_enabled = true;
    return true;
}

/*
 * Delete all images, disable mouse/keyboard modes, leave alt screen,
 * drain late APC replies, and restore cooked termios.
 */
void KITTY_RestoreTerminal(SDL_VideoData *data)
{
    static const char restore_modes[] =
        "\033[?1016l"
        "\033[?1006l"
        "\033[?1003l"
        "\033[?1002l"
        "\033[?1000l"
        "\033[<u"
        "\033[?25h";
    static const char leave_alt[] = "\033[?1049l";
    KITTY_DeleteAllImages(data->tty_out);
    KITTY_WriteAll(data->tty_out, restore_modes, sizeof(restore_modes) - 1);
    if (data->use_alt_screen) {
        KITTY_WriteAll(data->tty_out, leave_alt, sizeof(leave_alt) - 1);
    }
    /* Rex omits q=2 on a=T/SHM, so hosts reply with APC OK. Drain before
     * cooked mode or the next prompt shows Gi=1,p=1;OK as typed text. */
    KITTY_DrainInput(data);
    data->place_homed = false;
    data->placed_cols = 0;
    data->placed_rows = 0;
    KITTY_LeaveRawMode(data);
}

/*
 * Delete every Kitty image on fd (a=d,d=A,q=2).
 */
bool KITTY_DeleteAllImages(int fd)
{
    static const char del[] = "\033_Ga=d,d=A,q=2\033\\";
    return KITTY_WriteAll(fd, del, sizeof(del) - 1);
}

/*
 * Delete one Kitty image by id on fd (a=d,d=I,q=2).
 */
bool KITTY_DeleteImage(int fd, Uint32 image_id)
{
    char cmd[64];
    const int n = SDL_snprintf(cmd, sizeof(cmd), "\033_Ga=d,d=I,i=%u,q=2\033\\", (unsigned)image_id);
    if (n <= 0) {
        return false;
    }
    return KITTY_WriteAll(fd, cmd, (size_t)n);
}

/*
 * Delete active dirty_at overlay images and clear wdata->overlay_count.
 */
void KITTY_ClearOverlays(SDL_VideoData *vdata, SDL_WindowData *wdata)
{
    int i;
    if (!wdata) {
        return;
    }
    if (vdata) {
        for (i = 0; i < wdata->overlay_count; ++i) {
            if (wdata->overlay_ids[i] != 0) {
                KITTY_DeleteImage(vdata->tty_fd, wdata->overlay_ids[i]);
            }
        }
    }
    wdata->overlay_count = 0;
}

/*
 * Transmit+place a packed rect as its own a=T image at a 1-based CSI CUP
 * cell. DECSC/DECRC bracket CUP; place_cols/rows 0 = natural size; z/X/Y
 * go in extra APC keys. Refuses payloads above profile max_direct_bytes.
 */
bool KITTY_TransmitRGBRectPlace(SDL_VideoData *vdata, Uint32 image_id,
                                const Uint8 *packed_rgb, int rw, int rh,
                                int place_col, int place_row,
                                int place_cols, int place_rows,
                                int cell_X, int cell_Y, int z, int kitty_f)
{
    const int bpp = (kitty_f == 32) ? 4 : 3;
    const size_t payload_len = (size_t)rw * (size_t)rh * (size_t)bpp;
    char cup[32];
    char extra[64];
    int cup_len;
    int extra_len;
    size_t max_direct;
    size_t chunk_bytes;

    if (kitty_f != 24 && kitty_f != 32) {
        kitty_f = 24;
    }
    if (!vdata || !packed_rgb || image_id == 0 || rw <= 0 || rh <= 0 ||
        place_col < 1 || place_row < 1) {
        return SDL_SetError("invalid RGB rect place");
    }

    max_direct = (vdata->term_profile && vdata->term_profile->max_direct_bytes > 0)
                     ? vdata->term_profile->max_direct_bytes
                     : ((size_t)1920 * 1080 * 4);
    if (payload_len > max_direct) {
        return SDL_SetError(
            "kitty: refusing direct a=T overlay of %dx%d (~%zu bytes)",
            rw, rh, payload_len);
    }

    if (cell_X < 0) {
        cell_X = 0;
    }
    if (cell_Y < 0) {
        cell_Y = 0;
    }
    if (z == 0) {
        z = 10; /* above default base placements */
    }

    /* Save/restore cursor so CUP for overlays does not drift the base origin. */
    if (!KITTY_WriteAll(vdata->tty_fd, "\0337", 2)) {
        return false;
    }

    cup_len = SDL_snprintf(cup, sizeof(cup), "\033[%d;%dH", place_row, place_col);
    if (cup_len <= 0 || !KITTY_WriteAll(vdata->tty_fd, cup, (size_t)cup_len)) {
        KITTY_WriteAll(vdata->tty_fd, "\0338", 2);
        return false;
    }

    if (cell_X > 0 || cell_Y > 0) {
        extra_len = SDL_snprintf(extra, sizeof(extra), "z=%d,X=%d,Y=%d", z, cell_X, cell_Y);
    } else {
        extra_len = SDL_snprintf(extra, sizeof(extra), "z=%d", z);
    }
    if (extra_len <= 0) {
        KITTY_WriteAll(vdata->tty_fd, "\0338", 2);
        return SDL_SetError("overlay extra keys failed");
    }

    chunk_bytes = KITTY_ProfileApcChunkBytes(vdata->term_profile);
    {
        size_t wire = 0;
        const bool ok = KITTY_TransmitDirectPayload(vdata->tty_fd, image_id, rw, rh,
                                                   place_cols, place_rows, extra,
                                                   packed_rgb, payload_len, kitty_f,
                                                   false, chunk_bytes, &wire);
        KITTY_WriteAll(vdata->tty_fd, "\0338", 2); /* DECRC */
        if (ok) {
            vdata->last_wire_bytes = wire + (size_t)cup_len + 4; /* + DECSC/DECRC */
            vdata->last_raw_bytes = payload_len;
            vdata->total_wire_bytes += vdata->last_wire_bytes;
            vdata->wire_presents += 1;
        }
        return ok;
    }
}

/*
 * Edit root frame (r=1) in place with packed RGB/RGBA for rectangle
 * x,y,s,v (Kitty a=f, t=d only). Chunked base64; does not home or re-place.
 */
bool KITTY_TransmitRGBRectEdit(SDL_VideoData *vdata, Uint32 image_id,
                               const Uint8 *packed_rgb, int x, int y, int rw, int rh, int kitty_f)
{
    const int bpp = (kitty_f == 32) ? 4 : 3;
    const size_t payload_len = (size_t)rw * (size_t)rh * (size_t)bpp;
    size_t b64_len;
    char *b64;
    size_t encoded;
    size_t offset = 0;
    bool first = true;

    if (kitty_f != 24 && kitty_f != 32) {
        kitty_f = 24;
    }
    if (!vdata || !packed_rgb || image_id == 0 || rw <= 0 || rh <= 0 || x < 0 || y < 0) {
        return SDL_SetError("invalid RGB rect edit");
    }

    /* a=f stays on direct (t=d). SHM a=f (t=s) wrote successfully on Kitty.app
     * but left a blank image; frame_edit_probe also fails there. Full a=T/SHM
     * is used for large updates instead. */

    /* Direct a=f ceiling — huge chunks paint base64 when the host drops the APC. */
    {
        const size_t max_direct =
            (vdata->term_profile && vdata->term_profile->max_direct_bytes > 0)
                ? vdata->term_profile->max_direct_bytes
                : ((size_t)1920 * 1080 * 4);
        if (payload_len > max_direct) {
            return SDL_SetError(
                "kitty: refusing direct a=f of %dx%d (~%zu bytes); use smaller FB or SHM full present",
                rw, rh, payload_len);
        }
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
    {
        const size_t chunk_cap = KITTY_ProfileApcChunkBytes(vdata->term_profile);
        size_t wire = 0;
        const Uint64 t0 = SDL_GetTicksNS();

        while (offset < encoded) {
            size_t chunk = encoded - offset;
            char header[256];
            int header_len;
            int more;
            if (chunk > chunk_cap) {
                chunk = chunk_cap;
            }
            more = (offset + chunk < encoded) ? 1 : 0;
            if (first) {
                /* r=1 edits root; X=1 overwrite; q=2 quiet (no APC OK). */
                if (more) {
                    header_len = SDL_snprintf(header, sizeof(header),
                                              "\033_Ga=f,f=%d,t=d,i=%u,r=1,X=1,x=%d,y=%d,s=%d,v=%d,q=2,m=1;",
                                              kitty_f, (unsigned)image_id, x, y, rw, rh);
                } else {
                    header_len = SDL_snprintf(header, sizeof(header),
                                              "\033_Ga=f,f=%d,t=d,i=%u,r=1,X=1,x=%d,y=%d,s=%d,v=%d,q=2;",
                                              kitty_f, (unsigned)image_id, x, y, rw, rh);
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
            wire += (size_t)header_len + chunk + 2;
            offset += chunk;
        }

        {
            const Uint64 t1 = SDL_GetTicksNS();
            const double ms = (double)(t1 - t0) / 1000000.0;
            /* Accumulate: framebuffer zeros last_* at present start (multi a=f). */
            vdata->last_wire_bytes += wire;
            vdata->last_raw_bytes += payload_len;
            vdata->last_ms_tx += ms;
            vdata->last_ms_ack = -1.0;
            vdata->total_wire_bytes += wire;
            vdata->total_ms_tx += ms;
            vdata->wire_presents += 1;
        }
    }

    SDL_free(b64);
    return true;
}

/*
 * Re-place an existing image with new c/r (a=p only; no pixel upload).
 * Homes the cursor and updates placed_cols/rows / place_homed.
 */
bool KITTY_PlaceExistingImage(SDL_VideoData *vdata, Uint32 image_id,
                              int place_cols, int place_rows)
{
    char cmd[160];
    int n;

    if (!vdata || image_id == 0) {
        return SDL_SetError("invalid place-existing args");
    }
    if (!KITTY_WritePlaceOrigin(vdata)) {
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
    vdata->place_homed = true;
    return true;
}

/*
 * Full-frame transmit+place (a=T): try SHM, then zlib direct, then raw
 * base64 per transmit_mode / SDL_KITTY_TRANSMIT. place_cells stretches to
 * the grid; homes only when place size changes. Tracks wire timing stats.
 */
bool KITTY_TransmitRGB(SDL_VideoData *vdata, Uint32 image_id, const Uint8 *rgb, int w, int h, int kitty_f)
{
    if (!vdata || !rgb || w <= 0 || h <= 0) {
        return SDL_SetError("invalid RGB buffer");
    }
    if (kitty_f != 24 && kitty_f != 32) {
        kitty_f = 24;
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
        /* place_cells: stretch to viewport (card) or full grid. Else 1:1. */
        int place_cols = 0, place_rows = 0;
        bool home_cursor;
        bool ok = false;
        size_t wire_bytes = 0;
        size_t raw_bytes = 0;
        const Uint64 t0 = SDL_GetTicksNS();

        KITTY_GetPlaceCells(vdata, &place_cols, &place_rows);
        home_cursor =
            !vdata->place_homed ||
            vdata->placed_cols != place_cols ||
            vdata->placed_rows != place_rows;
        if (home_cursor) {
            if (!KITTY_WritePlaceOrigin(vdata)) {
                return false;
            }
            /* Lower helpers must not emit a second CUP/home. */
            home_cursor = false;
        }

        {
            const int bpp = (kitty_f == 32) ? 4 : 3;
            const size_t raw_len = (size_t)w * (size_t)h * (size_t)bpp;
            /* Multi-chunk direct of huge FBs often lands as literal base64 in the
             * tty when the host rejects/drops the APC (seen with dvd_sprite on
             * Kitty.app at ~3360x2100 after SHM failure). Refuse giant direct;
             * ceiling comes from the host profile (docs/terminals.md). */
            const size_t max_direct =
                (vdata->term_profile && vdata->term_profile->max_direct_bytes > 0)
                    ? vdata->term_profile->max_direct_bytes
                    : ((size_t)1920 * 1080 * 4);

            if (vdata->transmit_mode == KITTY_TRANSMIT_SHM && !vdata->shm_failed) {
                if (KITTY_TransmitShm(vdata, image_id, rgb, w, h, place_cols, place_rows, kitty_f,
                                      home_cursor)) {
                    ok = true;
                    raw_bytes = raw_len;
                    /* SHM APC is path-sized; approximate from name+header ~200B. */
                    wire_bytes = 256;
                } else {
                    /* One retry with a fresh object name (seq bumps inside). */
                    SDL_ClearError();
                    if (KITTY_TransmitShm(vdata, image_id, rgb, w, h, place_cols, place_rows, kitty_f,
                                          home_cursor)) {
                        ok = true;
                        raw_bytes = raw_len;
                        wire_bytes = 256;
                    } else {
                        const bool prefer_direct =
                            vdata->term_profile && vdata->term_profile->prefer_direct;
                        /* Kitty/Ghostty: never fall back to giant direct — that is the
                         * mid-run base64 flood. Rex may still use direct. */
                        const bool allow_direct_fallback =
                            prefer_direct ||
                            raw_len <= KITTY_ProfileDirectFallbackBytes(vdata->term_profile);
                        if (!allow_direct_fallback) {
                            return SDL_SetError(
                                "kitty: SHM transmit failed (%s); refusing direct fallback "
                                "of %zux%zu (~%zu bytes) to avoid base64 flood",
                                SDL_GetError(), (size_t)w, (size_t)h, raw_len);
                        }
                        vdata->shm_failed = true;
                        vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
                        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                                    "kitty: SHM transmit failed (%s); falling back to direct",
                                    SDL_GetError());
                        SDL_ClearError();
                    }
                }
            }

            if (!ok && vdata->transmit_mode == KITTY_TRANSMIT_DIRECT_ZLIB) {
                if (KITTY_TransmitDirectZlib(vdata->tty_fd, image_id, rgb, w, h, place_cols, place_rows,
                                             kitty_f, home_cursor,
                                             KITTY_ProfileApcChunkBytes(vdata->term_profile),
                                             &wire_bytes, &raw_bytes)) {
                    ok = true;
                } else {
                    vdata->transmit_mode = KITTY_TRANSMIT_DIRECT;
                    SDL_ClearError();
                }
            }

            if (!ok) {
                if (raw_len > max_direct) {
                    return SDL_SetError(
                        "kitty: refusing direct transmit of %zux%zu (~%zu bytes); "
                        "set SDL_KITTY_FB_MAX=1280x720 or fix SHM",
                        (size_t)w, (size_t)h, raw_len);
                }
                ok = KITTY_TransmitDirectRaw(vdata->tty_fd, image_id, rgb, w, h, place_cols, place_rows,
                                             kitty_f, home_cursor,
                                             KITTY_ProfileApcChunkBytes(vdata->term_profile),
                                             &wire_bytes, &raw_bytes);
            }
        }
        {
            const Uint64 t1 = SDL_GetTicksNS();
            vdata->last_ms_tx = (double)(t1 - t0) / 1000000.0;
            vdata->last_ms_ack = -1.0;
        }
        if (ok) {
            vdata->placed_cols = place_cols;
            vdata->placed_rows = place_rows;
            vdata->place_homed = true;
            vdata->last_wire_bytes = wire_bytes;
            vdata->last_raw_bytes = raw_bytes;
            vdata->total_wire_bytes += wire_bytes;
            vdata->total_ms_tx += vdata->last_ms_tx;
            vdata->wire_presents += 1;

#ifdef SDL_KITTY_PERF_ACK
            /* Optional host round-trip: wait for Gi=<id>;OK after a=T.
             * Compile with -DSDL_KITTY_PERF_ACK=ON; then SDL_KITTY_PERF_ACK=1 at run. */
            {
                const char *ack_env = SDL_getenv("SDL_KITTY_PERF_ACK");
                if (ack_env && (ack_env[0] == '1' || SDL_strcasecmp(ack_env, "true") == 0 ||
                                SDL_strcasecmp(ack_env, "yes") == 0 ||
                                SDL_strcasecmp(ack_env, "on") == 0)) {
                    const Uint64 a0 = SDL_GetTicksNS();
                    const KITTY_GraphicsQueryStatus st =
                        KITTY_AwaitGraphicsAck(vdata->tty_in, image_id, 100);
                    const Uint64 a1 = SDL_GetTicksNS();
                    if (st == KITTY_GQ_OK || st == KITTY_GQ_ERROR) {
                        vdata->last_ms_ack = (double)(a1 - a0) / 1000000.0;
                        vdata->total_ms_ack += vdata->last_ms_ack;
                        vdata->ack_presents += 1;
                    }
                }
            }
#endif /* SDL_KITTY_PERF_ACK */
        }
        return ok;
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
