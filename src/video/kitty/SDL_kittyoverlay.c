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
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#include "SDL_kittyoverlay.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum KITTY_OverlayPos
{
    KITTY_OV_POS_BOTTOM = 0,
    KITTY_OV_POS_TOP,
    KITTY_OV_POS_CENTER,
    KITTY_OV_POS_TOP_LEFT,
    KITTY_OV_POS_TOP_RIGHT,
    KITTY_OV_POS_BOTTOM_LEFT,
    KITTY_OV_POS_BOTTOM_RIGHT
} KITTY_OverlayPos;

struct KITTY_ToastOverlay
{
    char text[512];
    char file_path[512];
    KITTY_OverlayPos pos;
    Uint64 delay_ms;
    Uint64 duration_ms; /* 0 = stay visible forever */
    Uint64 fade_ms;
    int req_scale; /* 0 = auto */
    SDL_Scancode toggle_key;
    Uint8 bg[4];
    Uint8 border[4];
    Uint8 fg[4];

    bool started;
    bool hidden_by_user;
    Uint64 start_ms;
    Uint64 last_file_poll_ms;
    time_t last_file_mtime;
    off_t last_file_size;
    bool file_loaded;

    /* Cached rendered RGBA toast bitmap for (cache_fb_w, cache_fb_h). */
    int cache_fb_w;
    int cache_fb_h;
    SDL_Rect rect;
    Uint8 *rgba;
    size_t rgba_cap;

    /* Backing store for clean rgb_scratch pixels under rect during present. */
    Uint8 *save_buf;
    size_t save_cap;
    SDL_Rect save_rect;
    bool saved;

    Uint8 last_drawn_alpha;
    SDL_Rect last_drawn_rect;
};

/*
 * Compact public-domain 8x8 bitmap font (ASCII 0x20..0x7E, MSB on left).
 */
static const Uint8 KITTY_font8x8[95][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' ' */
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, /* '!' */
    { 0x6C, 0x6C, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* '"' */
    { 0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00 }, /* '#' */
    { 0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00 }, /* '$' */
    { 0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00 }, /* '%' */
    { 0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00 }, /* '&' */
    { 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ''' */
    { 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 }, /* '(' */
    { 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 }, /* ')' */
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, /* '*' */
    { 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 }, /* '+' */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 }, /* ',' */
    { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 }, /* '-' */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 }, /* '.' */
    { 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00 }, /* '/' */
    { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 }, /* '0' */
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* '1' */
    { 0x3C, 0x66, 0x06, 0x1C, 0x30, 0x60, 0x7E, 0x00 }, /* '2' */
    { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 }, /* '3' */
    { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 }, /* '4' */
    { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 }, /* '5' */
    { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 }, /* '6' */
    { 0x7E, 0x66, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x00 }, /* '7' */
    { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 }, /* '8' */
    { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 }, /* '9' */
    { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 }, /* ':' */
    { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00 }, /* ';' */
    { 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00 }, /* '<' */
    { 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 }, /* '=' */
    { 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00 }, /* '>' */
    { 0x3C, 0x66, 0x06, 0x1C, 0x18, 0x00, 0x18, 0x00 }, /* '?' */
    { 0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00 }, /* '@' */
    { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 }, /* 'A' */
    { 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 }, /* 'B' */
    { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 }, /* 'C' */
    { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 }, /* 'D' */
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 }, /* 'E' */
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 }, /* 'F' */
    { 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3E, 0x00 }, /* 'G' */
    { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 }, /* 'H' */
    { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 }, /* 'I' */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x6C, 0x6C, 0x38, 0x00 }, /* 'J' */
    { 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 }, /* 'K' */
    { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 }, /* 'L' */
    { 0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00 }, /* 'M' */
    { 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00 }, /* 'N' */
    { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* 'O' */
    { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 }, /* 'P' */
    { 0x3C, 0x66, 0x66, 0x66, 0x6A, 0x6C, 0x36, 0x00 }, /* 'Q' */
    { 0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00 }, /* 'R' */
    { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 }, /* 'S' */
    { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 }, /* 'T' */
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* 'U' */
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 }, /* 'V' */
    { 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00 }, /* 'W' */
    { 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 }, /* 'X' */
    { 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 }, /* 'Y' */
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 }, /* 'Z' */
    { 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00 }, /* '[' */
    { 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00 }, /* '\' */
    { 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00 }, /* ']' */
    { 0x18, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* '^' */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00 }, /* '_' */
    { 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* '`' */
    { 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00 }, /* 'a' */
    { 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00 }, /* 'b' */
    { 0x00, 0x00, 0x3C, 0x60, 0x60, 0x60, 0x3C, 0x00 }, /* 'c' */
    { 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00 }, /* 'd' */
    { 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00 }, /* 'e' */
    { 0x1C, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x30, 0x00 }, /* 'f' */
    { 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C }, /* 'g' */
    { 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 }, /* 'h' */
    { 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00 }, /* 'i' */
    { 0x0C, 0x00, 0x1C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38 }, /* 'j' */
    { 0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00 }, /* 'k' */
    { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 }, /* 'l' */
    { 0x00, 0x00, 0xEC, 0xFE, 0xD6, 0xC6, 0xC6, 0x00 }, /* 'm' */
    { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 }, /* 'n' */
    { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* 'o' */
    { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60 }, /* 'p' */
    { 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06 }, /* 'q' */
    { 0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x00 }, /* 'r' */
    { 0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00 }, /* 's' */
    { 0x30, 0x30, 0x7C, 0x30, 0x30, 0x36, 0x1C, 0x00 }, /* 't' */
    { 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00 }, /* 'u' */
    { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 }, /* 'v' */
    { 0x00, 0x00, 0xC6, 0xC6, 0xD6, 0xFE, 0x6C, 0x00 }, /* 'w' */
    { 0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00 }, /* 'x' */
    { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C }, /* 'y' */
    { 0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00 }, /* 'z' */
    { 0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00 }, /* '{' */
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, /* '|' */
    { 0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00 }, /* '}' */
    { 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* '~' */
};

static const Uint8 KITTY_glyph_middot[8] = {
    0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00
};

static Uint64 KITTY_ParseDurationMs(const char *val)
{
    double n;
    size_t len;
    if (!val || !val[0]) {
        return 0;
    }
    n = SDL_atof(val);
    if (n <= 0.0) {
        return 0;
    }
    len = SDL_strlen(val);
    if (len >= 2 && (val[len - 2] == 'm' || val[len - 2] == 'M') &&
        (val[len - 1] == 's' || val[len - 1] == 'S')) {
        return (Uint64)(n + 0.5);
    }
    return (Uint64)(n * 1000.0 + 0.5);
}

static void KITTY_OverlaySetTheme(KITTY_ToastOverlay *ov, const char *name)
{
    if (name && SDL_strcasecmp(name, "amber") == 0) {
        ov->bg[0] = 0x14; ov->bg[1] = 0x12; ov->bg[2] = 0x10; ov->bg[3] = 228;
        ov->border[0] = 0xF5; ov->border[1] = 0xA6; ov->border[2] = 0x23; ov->border[3] = 240;
        ov->fg[0] = 0xF6; ov->fg[1] = 0xED; ov->fg[2] = 0xE0; ov->fg[3] = 255;
    } else if (name && SDL_strcasecmp(name, "nord") == 0) {
        ov->bg[0] = 0x1E; ov->bg[1] = 0x24; ov->bg[2] = 0x30; ov->bg[3] = 228;
        ov->border[0] = 0x88; ov->border[1] = 0xC0; ov->border[2] = 0xD0; ov->border[3] = 235;
        ov->fg[0] = 0xEC; ov->fg[1] = 0xEF; ov->fg[2] = 0xF4; ov->fg[3] = 255;
    } else if (name && SDL_strcasecmp(name, "light") == 0) {
        ov->bg[0] = 0xF4; ov->bg[1] = 0xF6; ov->bg[2] = 0xFA; ov->bg[3] = 236;
        ov->border[0] = 0x09; ov->border[1] = 0x69; ov->border[2] = 0xDA; ov->border[3] = 230;
        ov->fg[0] = 0x1B; ov->fg[1] = 0x22; ov->fg[2] = 0x30; ov->fg[3] = 255;
    } else {
        /* Default: tokyo midnight indigo */
        ov->bg[0] = 0x15; ov->bg[1] = 0x18; ov->bg[2] = 0x28; ov->bg[3] = 228;
        ov->border[0] = 0x7A; ov->border[1] = 0xA2; ov->border[2] = 0xF7; ov->border[3] = 235;
        ov->fg[0] = 0xE6; ov->fg[1] = 0xEC; ov->fg[2] = 0xF8; ov->fg[3] = 255;
    }
}

static void KITTY_CopyTextEscaped(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    size_t i = 0, o = 0;
    while (i < src_len && o + 1 < dst_cap) {
        if (src[i] == '\\' && i + 1 < src_len) {
            const char nx = src[i + 1];
            if (nx == 'n' || nx == 'r') {
                dst[o++] = '\n';
                i += 2;
                /* Collapse escaped \n\r or \r\n into a single line break. */
                if (i + 1 < src_len && src[i] == '\\' &&
                    ((nx == 'n' && src[i + 1] == 'r') || (nx == 'r' && src[i + 1] == 'n'))) {
                    i += 2;
                }
                continue;
            }
            if (nx == 't') {
                int sp;
                for (sp = 0; sp < 4 && o + 1 < dst_cap; ++sp) {
                    dst[o++] = ' ';
                }
                i += 2;
                continue;
            }
            if (nx == '\\') {
                dst[o++] = '\\';
                i += 2;
                continue;
            }
        }
        if (src[i] == '\n' || src[i] == '\r') {
            const char cur = src[i];
            dst[o++] = '\n';
            i += 1;
            /* Collapse literal \n\r or \r\n into a single line break. */
            if (i < src_len &&
                ((cur == '\n' && src[i] == '\r') || (cur == '\r' && src[i] == '\n'))) {
                i += 1;
            }
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

static SDL_Scancode KITTY_ParseOverlayKey(const char *s)
{
    if (!s || !s[0]) {
        return SDL_SCANCODE_UNKNOWN;
    }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = SDL_atoi(s + 1);
        if (n >= 1 && n <= 12) {
            return (SDL_Scancode)(SDL_SCANCODE_F1 + (n - 1));
        }
    }
    if (s[1] == '\0') {
        if (s[0] >= 'a' && s[0] <= 'z') {
            return (SDL_Scancode)(SDL_SCANCODE_A + (s[0] - 'a'));
        }
        if (s[0] >= 'A' && s[0] <= 'Z') {
            return (SDL_Scancode)(SDL_SCANCODE_A + (s[0] - 'A'));
        }
        if (s[0] == '`') {
            return SDL_SCANCODE_GRAVE;
        }
    }
    if (SDL_strcasecmp(s, "grave") == 0 || SDL_strcasecmp(s, "tilde") == 0) {
        return SDL_SCANCODE_GRAVE;
    }
    if (SDL_strcasecmp(s, "tab") == 0) {
        return SDL_SCANCODE_TAB;
    }
    return SDL_SCANCODE_UNKNOWN;
}

static bool KITTY_OverlayPollFile(KITTY_ToastOverlay *ov, Uint64 now_ms)
{
    struct stat st;
    int fd;
    char buf[512];
    ssize_t n;

    if (!ov || !ov->file_path[0]) {
        return false;
    }
    if (ov->last_file_poll_ms != 0 && (now_ms - ov->last_file_poll_ms) < 200u) {
        return false;
    }
    ov->last_file_poll_ms = now_ms;

    if (stat(ov->file_path, &st) != 0) {
        if (ov->file_loaded) {
            ov->file_loaded = false;
            ov->text[0] = '\0';
            ov->hidden_by_user = true;
            ov->cache_fb_w = 0;
            return true;
        }
        return false;
    }
    if (ov->file_loaded && st.st_mtime == ov->last_file_mtime && st.st_size == ov->last_file_size) {
        return false;
    }
    fd = open(ov->file_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1u);
    close(fd);
    if (n < 0) {
        return false;
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    ov->last_file_mtime = st.st_mtime;
    ov->last_file_size = st.st_size;
    ov->file_loaded = true;
    KITTY_CopyTextEscaped(ov->text, sizeof(ov->text), buf, (size_t)n);
    ov->cache_fb_w = 0;
    if (ov->text[0]) {
        ov->hidden_by_user = false;
        ov->started = true;
        ov->start_ms = now_ms;
        ov->delay_ms = 0;
    } else {
        ov->hidden_by_user = true;
    }
    return true;
}

KITTY_ToastOverlay *KITTY_OverlayCreate(const char *spec)
{
    KITTY_ToastOverlay *ov;
    bool has_kv = false;
    const char *p;

    if (!spec || !spec[0]) {
        return NULL;
    }
    ov = (KITTY_ToastOverlay *)SDL_calloc(1, sizeof(KITTY_ToastOverlay));
    if (!ov) {
        return NULL;
    }
    ov->pos = KITTY_OV_POS_BOTTOM;
    ov->delay_ms = 0;
    ov->duration_ms = 5000;
    ov->fade_ms = 400;
    ov->req_scale = 0;
    KITTY_OverlaySetTheme(ov, "tokyo");

    /* Check if spec uses key=value;... format (recognized keys) or plain shorthand text. */
    p = spec;
    while (*p) {
        const char *semi = SDL_strchr(p, ';');
        const size_t tok_len = semi ? (size_t)(semi - p) : SDL_strlen(p);
        const char *eq = NULL;
        {
            size_t qi;
            for (qi = 0; qi < tok_len; ++qi) {
                if (p[qi] == '=') {
                    eq = p + qi;
                    break;
                }
            }
        }
        if (eq) {
            const size_t klen = (size_t)(eq - p);
            const char *v = eq + 1;
            const size_t vlen = tok_len - klen - 1;
            char vbuf[64];
            const size_t copy_v = vlen < sizeof(vbuf) - 1 ? vlen : sizeof(vbuf) - 1;
            SDL_memcpy(vbuf, v, copy_v);
            vbuf[copy_v] = '\0';

            if (klen == 4 && SDL_strncasecmp(p, "text", 4) == 0) {
                /* text= consumes the rest of the string so text can contain ';' if last,
                 * or stop at next ';' only if another known key follows? Keep simple:
                 * consume up to ';' if followed by known key, else rest of spec. */
                KITTY_CopyTextEscaped(ov->text, sizeof(ov->text), v, vlen);
                has_kv = true;
            } else if (klen == 3 && SDL_strncasecmp(p, "pos", 3) == 0) {
                has_kv = true;
                if (SDL_strcasecmp(vbuf, "top") == 0) ov->pos = KITTY_OV_POS_TOP;
                else if (SDL_strcasecmp(vbuf, "center") == 0) ov->pos = KITTY_OV_POS_CENTER;
                else if (SDL_strcasecmp(vbuf, "top-left") == 0) ov->pos = KITTY_OV_POS_TOP_LEFT;
                else if (SDL_strcasecmp(vbuf, "top-right") == 0) ov->pos = KITTY_OV_POS_TOP_RIGHT;
                else if (SDL_strcasecmp(vbuf, "bottom-left") == 0) ov->pos = KITTY_OV_POS_BOTTOM_LEFT;
                else if (SDL_strcasecmp(vbuf, "bottom-right") == 0) ov->pos = KITTY_OV_POS_BOTTOM_RIGHT;
                else ov->pos = KITTY_OV_POS_BOTTOM;
            } else if (klen == 5 && SDL_strncasecmp(p, "delay", 5) == 0) {
                ov->delay_ms = KITTY_ParseDurationMs(vbuf);
                has_kv = true;
            } else if (klen == 8 && SDL_strncasecmp(p, "duration", 8) == 0) {
                ov->duration_ms = KITTY_ParseDurationMs(vbuf);
                has_kv = true;
            } else if (klen == 4 && SDL_strncasecmp(p, "fade", 4) == 0) {
                ov->fade_ms = KITTY_ParseDurationMs(vbuf);
                has_kv = true;
            } else if (klen == 5 && SDL_strncasecmp(p, "scale", 5) == 0) {
                int sc = SDL_atoi(vbuf);
                if (sc >= 1 && sc <= 4) {
                    ov->req_scale = sc;
                }
                has_kv = true;
            } else if (klen == 5 && SDL_strncasecmp(p, "theme", 5) == 0) {
                KITTY_OverlaySetTheme(ov, vbuf);
                has_kv = true;
            } else if ((klen == 6 && SDL_strncasecmp(p, "toggle", 6) == 0) ||
                       (klen == 3 && SDL_strncasecmp(p, "key", 3) == 0)) {
                ov->toggle_key = KITTY_ParseOverlayKey(vbuf);
                has_kv = true;
            } else if (klen == 4 && SDL_strncasecmp(p, "file", 4) == 0) {
                const size_t flen = vlen < sizeof(ov->file_path) - 1 ? vlen : sizeof(ov->file_path) - 1;
                SDL_memcpy(ov->file_path, v, flen);
                ov->file_path[flen] = '\0';
                has_kv = true;
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }

    if (!has_kv) {
        KITTY_CopyTextEscaped(ov->text, sizeof(ov->text), spec, SDL_strlen(spec));
    }
    if (!ov->text[0] && !ov->file_path[0]) {
        SDL_free(ov);
        return NULL;
    }
    return ov;
}

void KITTY_OverlayDestroy(KITTY_ToastOverlay *ov)
{
    if (!ov) {
        return;
    }
    SDL_free(ov->rgba);
    SDL_free(ov->save_buf);
    SDL_free(ov);
}

static const Uint8 *KITTY_NextGlyph(const char **pp)
{
    const unsigned char *s = (const unsigned char *)(*pp);
    unsigned char c = s[0];
    if (c == 0) {
        return NULL;
    }
    /* UTF-8 middle dot U+00B7 (0xC2 0xB7) or bullet U+2022 (0xE2 0x80 0xA2). */
    if (c == 0xC2 && s[1] == 0xB7) {
        *pp += 2;
        return KITTY_glyph_middot;
    }
    if (c == 0xE2 && s[1] == 0x80 && s[2] == 0xA2) {
        *pp += 3;
        return KITTY_glyph_middot;
    }
    /* Skip multi-byte UTF-8 sequences cleanly as space. */
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *pp += 2;
        return KITTY_font8x8[0];
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *pp += 3;
        return KITTY_font8x8[0];
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *pp += 4;
        return KITTY_font8x8[0];
    }
    *pp += 1;
    if (c >= 0x20 && c <= 0x7E) {
        return KITTY_font8x8[c - 0x20];
    }
    return KITTY_font8x8[0];
}

static bool KITTY_OverlayRenderBitmap(KITTY_ToastOverlay *ov, int fb_w, int fb_h)
{
    int scale;
    int max_cols = 0;
    int n_lines = 0;
    const char *lines[16];
    int line_cols[16];
    const char *p = ov->text;
    int pad_x, pad_y, line_h, char_w, bw, bh, rad, border_px, margin_x, margin_y;
    int ox, oy;
    size_t need;

    if (fb_w < 64 || fb_h < 32) {
        return false;
    }

    /* Split into up to 16 lines and count glyph columns per line. */
    while (*p && n_lines < 16) {
        const char *start = p;
        int cols = 0;
        while (*p && *p != '\n') {
            (void)KITTY_NextGlyph(&p);
            cols++;
        }
        lines[n_lines] = start;
        line_cols[n_lines] = cols;
        if (cols > max_cols) {
            max_cols = cols;
        }
        n_lines++;
        if (*p == '\n') {
            p++;
        }
    }
    if (n_lines == 0 || max_cols == 0) {
        return false;
    }

    if (ov->req_scale > 0) {
        scale = ov->req_scale;
    } else if (fb_h >= 780) {
        scale = 3;
    } else if (fb_h >= 360) {
        scale = 2;
    } else {
        scale = 1;
    }

    /* Shrink scale if box would exceed 94% of framebuffer width. */
    while (scale > 1) {
        const int test_w = max_cols * (8 * scale) + 28 * scale;
        if (test_w <= (fb_w * 94) / 100) {
            break;
        }
        scale--;
    }

    char_w = 8 * scale;
    line_h = 11 * scale;
    pad_x = 12 * scale;
    pad_y = 8 * scale;
    border_px = (scale >= 2) ? 2 : 1;
    rad = 6 * scale;

    bw = max_cols * char_w + pad_x * 2;
    bh = n_lines * line_h - (3 * scale) + pad_y * 2;
    if (bw > fb_w - 8) {
        bw = fb_w - 8;
    }
    if (bh > fb_h - 8) {
        bh = fb_h - 8;
    }
    if (bw <= 4 || bh <= 4) {
        return false;
    }

    need = (size_t)bw * (size_t)bh * 4u;
    if (ov->rgba_cap < need) {
        Uint8 *nb = (Uint8 *)SDL_realloc(ov->rgba, need);
        if (!nb) {
            return false;
        }
        ov->rgba = nb;
        ov->rgba_cap = need;
    }
    SDL_memset(ov->rgba, 0, need);

    /* Draw subpixel anti-aliased rounded translucent box + crisp accent border. */
    {
        const float r_out = (float)rad;
        const float r_in = (rad > border_px) ? (float)(rad - border_px) : 0.0f;
        int y, x;
        for (y = 0; y < bh; ++y) {
            Uint8 *row = ov->rgba + (size_t)y * (size_t)bw * 4u;
            for (x = 0; x < bw; ++x) {
                float dx = 0.0f, dy = 0.0f;
                float cov_out = 1.0f;
                float cov_in = 1.0f;

                if (x < rad) dx = (float)(rad - 1 - x) + 0.5f;
                else if (x >= bw - rad) dx = (float)(x - (bw - rad)) + 0.5f;
                if (y < rad) dy = (float)(rad - 1 - y) + 0.5f;
                else if (y >= bh - rad) dy = (float)(y - (bh - rad)) + 0.5f;

                if (dx > 0.0f && dy > 0.0f) {
                    const float dist = SDL_sqrtf(dx * dx + dy * dy);
                    cov_out = r_out - dist + 0.5f;
                    if (cov_out <= 0.0f) {
                        continue;
                    }
                    if (cov_out > 1.0f) {
                        cov_out = 1.0f;
                    }
                    cov_in = r_in - dist + 0.5f;
                    if (cov_in < 0.0f) cov_in = 0.0f;
                    if (cov_in > 1.0f) cov_in = 1.0f;
                }
                if (x < border_px || x >= bw - border_px ||
                    y < border_px || y >= bh - border_px) {
                    cov_in = 0.0f;
                }
                {
                    const float inv_in = 1.0f - cov_in;
                    const float r = ov->bg[0] * cov_in + ov->border[0] * inv_in;
                    const float g = ov->bg[1] * cov_in + ov->border[1] * inv_in;
                    const float b = ov->bg[2] * cov_in + ov->border[2] * inv_in;
                    const float a = (ov->bg[3] * cov_in + ov->border[3] * inv_in) * cov_out;
                    row[x * 4 + 0] = (Uint8)(r + 0.5f);
                    row[x * 4 + 1] = (Uint8)(g + 0.5f);
                    row[x * 4 + 2] = (Uint8)(b + 0.5f);
                    row[x * 4 + 3] = (Uint8)(a + 0.5f);
                }
            }
        }
    }

    /* Rasterize text lines centered horizontally with subtle drop shadow + edge AA. */
    {
        int li;
        for (li = 0; li < n_lines; ++li) {
            const int lw = line_cols[li] * char_w;
            int gx = (bw - lw) / 2;
            const int gy0 = pad_y + li * line_h;
            const char *lp = lines[li];
            int ci;
            if (gx < pad_x) {
                gx = pad_x;
            }
            for (ci = 0; ci < line_cols[li]; ++ci) {
                const Uint8 *glyph = KITTY_NextGlyph(&lp);
                int row_i, col_i;
                if (!glyph) {
                    break;
                }
                /* Subtle 1px drop shadow behind glyphs when scale >= 2 */
                if (scale >= 2) {
                    for (row_i = 0; row_i < 8; ++row_i) {
                        const Uint8 bits = glyph[row_i];
                        if (!bits) continue;
                        for (col_i = 0; col_i < 8; ++col_i) {
                            if (bits & (0x80u >> col_i)) {
                                int sy, sx;
                                for (sy = 0; sy < scale; ++sy) {
                                    const int py = gy0 + row_i * scale + sy + 1;
                                    if (py < border_px || py >= bh - border_px) continue;
                                    Uint8 *drow = ov->rgba + (size_t)py * (size_t)bw * 4u;
                                    for (sx = 0; sx < scale; ++sx) {
                                        const int px = gx + col_i * scale + sx + 1;
                                        if (px < border_px || px >= bw - border_px) continue;
                                        drow[px * 4 + 0] = (Uint8)(drow[px * 4 + 0] / 2);
                                        drow[px * 4 + 1] = (Uint8)(drow[px * 4 + 1] / 2);
                                        drow[px * 4 + 2] = (Uint8)(drow[px * 4 + 2] / 2);
                                    }
                                }
                            }
                        }
                    }
                }
                for (row_i = 0; row_i < 8; ++row_i) {
                    const Uint8 bits = glyph[row_i];
                    if (!bits) {
                        continue;
                    }
                    for (col_i = 0; col_i < 8; ++col_i) {
                        if (bits & (0x80u >> col_i)) {
                            int sy, sx;
                            for (sy = 0; sy < scale; ++sy) {
                                const int py = gy0 + row_i * scale + sy;
                                if (py < border_px || py >= bh - border_px) {
                                    continue;
                                }
                                Uint8 *drow = ov->rgba + (size_t)py * (size_t)bw * 4u;
                                for (sx = 0; sx < scale; ++sx) {
                                    const int px = gx + col_i * scale + sx;
                                    if (px < border_px || px >= bw - border_px) {
                                        continue;
                                    }
                                    drow[px * 4 + 0] = ov->fg[0];
                                    drow[px * 4 + 1] = ov->fg[1];
                                    drow[px * 4 + 2] = ov->fg[2];
                                    drow[px * 4 + 3] = ov->fg[3];
                                }
                            }
                        }
                    }
                }
                gx += char_w;
            }
        }
    }

    margin_x = (fb_w >= 480) ? 18 : 8;
    margin_y = (fb_h >= 360) ? 16 : 8;
    switch (ov->pos) {
    case KITTY_OV_POS_TOP:
        ox = (fb_w - bw) / 2;
        oy = margin_y;
        break;
    case KITTY_OV_POS_CENTER:
        ox = (fb_w - bw) / 2;
        oy = (fb_h - bh) / 2;
        break;
    case KITTY_OV_POS_TOP_LEFT:
        ox = margin_x;
        oy = margin_y;
        break;
    case KITTY_OV_POS_TOP_RIGHT:
        ox = fb_w - bw - margin_x;
        oy = margin_y;
        break;
    case KITTY_OV_POS_BOTTOM_LEFT:
        ox = margin_x;
        oy = fb_h - bh - margin_y;
        break;
    case KITTY_OV_POS_BOTTOM_RIGHT:
        ox = fb_w - bw - margin_x;
        oy = fb_h - bh - margin_y;
        break;
    case KITTY_OV_POS_BOTTOM:
    default:
        ox = (fb_w - bw) / 2;
        oy = fb_h - bh - margin_y;
        break;
    }
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (ox + bw > fb_w) ox = fb_w - bw;
    if (oy + bh > fb_h) oy = fb_h - bh;

    ov->rect.x = ox;
    ov->rect.y = oy;
    ov->rect.w = bw;
    ov->rect.h = bh;
    ov->cache_fb_w = fb_w;
    ov->cache_fb_h = fb_h;
    return true;
}

bool KITTY_OverlayEvaluate(KITTY_ToastOverlay *ov, int fb_w, int fb_h, Uint64 now_ms,
                           SDL_Rect *out_rect, Uint8 *out_alpha, bool *out_needs_redraw)
{
    Uint64 elapsed;
    Uint8 alpha = 0;
    bool file_changed = false;

    if (out_alpha) *out_alpha = 0;
    if (out_needs_redraw) *out_needs_redraw = false;
    if (!ov || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    if (ov->file_path[0]) {
        file_changed = KITTY_OverlayPollFile(ov, now_ms);
    }
    if (!ov->text[0] || ov->hidden_by_user) {
        if (ov->last_drawn_alpha > 0) {
            if (out_rect) *out_rect = ov->last_drawn_rect;
            if (out_needs_redraw) *out_needs_redraw = true;
        }
        return false;
    }
    if (!ov->started) {
        ov->started = true;
        ov->start_ms = now_ms;
    }
    if (ov->cache_fb_w != fb_w || ov->cache_fb_h != fb_h || !ov->rgba) {
        if (!KITTY_OverlayRenderBitmap(ov, fb_w, fb_h)) {
            return false;
        }
        file_changed = true;
    }

    elapsed = (now_ms >= ov->start_ms) ? (now_ms - ov->start_ms) : 0;
    if (elapsed < ov->delay_ms) {
        alpha = 0;
    } else {
        const Uint64 vis = elapsed - ov->delay_ms;
        if (ov->duration_ms > 0 && vis >= ov->duration_ms) {
            alpha = 0;
        } else {
            float a = 1.0f;
            /* Gentle 180 ms fade-in */
            if (vis < 180u) {
                a = (float)vis / 180.0f;
            }
            if (ov->duration_ms > 0 && ov->fade_ms > 0) {
                const Uint64 rem = ov->duration_ms - vis;
                if (rem < ov->fade_ms) {
                    const float fout = (float)rem / (float)ov->fade_ms;
                    if (fout < a) {
                        a = fout;
                    }
                }
            }
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            alpha = (Uint8)(a * 255.0f + 0.5f);
            /* Quantize slightly during fades to avoid micro-jitter redraws */
            if (alpha > 0 && alpha < 255) {
                alpha = (Uint8)((alpha + 7u) & ~7u);
                if (alpha == 0) alpha = 8;
            }
        }
    }

    if (out_rect) {
        *out_rect = (alpha == 0 && ov->last_drawn_alpha > 0) ? ov->last_drawn_rect : ov->rect;
    }
    if (out_alpha) {
        *out_alpha = alpha;
    }
    if (out_needs_redraw) {
        const bool rect_changed =
            (ov->last_drawn_rect.x != ov->rect.x || ov->last_drawn_rect.y != ov->rect.y ||
             ov->last_drawn_rect.w != ov->rect.w || ov->last_drawn_rect.h != ov->rect.h);
        *out_needs_redraw = file_changed || (alpha != ov->last_drawn_alpha) || (alpha > 0 && rect_changed);
    }
    return alpha > 0;
}

bool KITTY_OverlayApply(KITTY_ToastOverlay *ov, Uint8 *rgb_scratch, int fb_w, int fb_h,
                        int bpp, Uint8 alpha)
{
    size_t row_bytes, need;
    int y, x;

    if (!ov || !ov->rgba || !rgb_scratch || alpha == 0 || (bpp != 3 && bpp != 4)) {
        return false;
    }
    if (ov->rect.x < 0 || ov->rect.y < 0 ||
        ov->rect.x + ov->rect.w > fb_w || ov->rect.y + ov->rect.h > fb_h) {
        return false;
    }

    row_bytes = (size_t)ov->rect.w * (size_t)bpp;
    need = row_bytes * (size_t)ov->rect.h;
    if (ov->save_cap < need) {
        Uint8 *nb = (Uint8 *)SDL_realloc(ov->save_buf, need);
        if (!nb) {
            return false;
        }
        ov->save_buf = nb;
        ov->save_cap = need;
    }

    for (y = 0; y < ov->rect.h; ++y) {
        Uint8 *dst = rgb_scratch + ((size_t)(ov->rect.y + y) * (size_t)fb_w + (size_t)ov->rect.x) * (size_t)bpp;
        const Uint8 *src = ov->rgba + (size_t)y * (size_t)ov->rect.w * 4u;
        Uint8 *sav = ov->save_buf + (size_t)y * row_bytes;
        SDL_memcpy(sav, dst, row_bytes);

        for (x = 0; x < ov->rect.w; ++x) {
            const Uint32 sa = ((Uint32)src[x * 4 + 3] * (Uint32)alpha + 127u) / 255u;
            if (sa == 0) {
                continue;
            }
            if (sa == 255u) {
                dst[x * bpp + 0] = src[x * 4 + 0];
                dst[x * bpp + 1] = src[x * 4 + 1];
                dst[x * bpp + 2] = src[x * 4 + 2];
                if (bpp == 4) {
                    dst[x * bpp + 3] = 255u;
                }
            } else {
                const Uint32 inv = 255u - sa;
                dst[x * bpp + 0] = (Uint8)((src[x * 4 + 0] * sa + dst[x * bpp + 0] * inv + 127u) / 255u);
                dst[x * bpp + 1] = (Uint8)((src[x * 4 + 1] * sa + dst[x * bpp + 1] * inv + 127u) / 255u);
                dst[x * bpp + 2] = (Uint8)((src[x * 4 + 2] * sa + dst[x * bpp + 2] * inv + 127u) / 255u);
                if (bpp == 4) {
                    dst[x * bpp + 3] = 255u;
                }
            }
        }
    }

    ov->save_rect = ov->rect;
    ov->saved = true;
    return true;
}

void KITTY_OverlayRestore(KITTY_ToastOverlay *ov, Uint8 *rgb_scratch, int fb_w, int fb_h,
                          int bpp)
{
    size_t row_bytes;
    int y;

    if (!ov || !ov->saved || !ov->save_buf || !rgb_scratch || (bpp != 3 && bpp != 4)) {
        return;
    }
    ov->saved = false;
    if (ov->save_rect.x < 0 || ov->save_rect.y < 0 ||
        ov->save_rect.x + ov->save_rect.w > fb_w || ov->save_rect.y + ov->save_rect.h > fb_h) {
        return;
    }
    row_bytes = (size_t)ov->save_rect.w * (size_t)bpp;
    for (y = 0; y < ov->save_rect.h; ++y) {
        Uint8 *dst = rgb_scratch + ((size_t)(ov->save_rect.y + y) * (size_t)fb_w + (size_t)ov->save_rect.x) * (size_t)bpp;
        const Uint8 *sav = ov->save_buf + (size_t)y * row_bytes;
        SDL_memcpy(dst, sav, row_bytes);
    }
}

void KITTY_OverlayMarkDrawn(KITTY_ToastOverlay *ov, Uint8 alpha)
{
    if (!ov) {
        return;
    }
    ov->last_drawn_alpha = alpha;
    ov->last_drawn_rect = ov->rect;
}

bool KITTY_OverlayHandleKey(KITTY_ToastOverlay *ov, SDL_Scancode scancode, Uint64 now_ms)
{
    if (!ov || ov->toggle_key == SDL_SCANCODE_UNKNOWN || scancode != ov->toggle_key) {
        return false;
    }
    if (!ov->text[0]) {
        return false;
    }
    if (ov->last_drawn_alpha > 0 && !ov->hidden_by_user) {
        /* Currently visible -> dismiss cleanly. */
        ov->hidden_by_user = true;
    } else {
        /* Currently hidden/expired -> re-show with a fresh timer. */
        ov->hidden_by_user = false;
        ov->started = true;
        ov->start_ms = now_ms;
        ov->delay_ms = 0;
    }
    return true;
}

#endif /* SDL_VIDEO_DRIVER_KITTY */
