/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
*/
#ifdef KITTY_PROBE_STANDALONE
#include <SDL3/SDL.h>
#include <string.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROBE_STANDALONE)

#include "SDL_kittyprobe.h"

#ifndef KITTY_PROBE_STANDALONE
#include "SDL_kittygraphics.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#endif

KITTY_GraphicsQueryStatus KITTY_ParseGraphicsQueryReply(const char *buf, size_t len, Uint32 expect_id)
{
    size_t i;
    if (!buf || len < 8) {
        return KITTY_GQ_NONE;
    }
    for (i = 0; i + 6 < len; ++i) {
        unsigned id = 0;
        char payload[128];
        int n;
        if ((unsigned char)buf[i] != 0x1b || buf[i + 1] != '_') {
            continue;
        }
        if (buf[i + 2] != 'G') {
            continue;
        }
        /* Forms: Gi=31;OK  or  Gi=31;EBADF:...  (ST may be ESC\ or BEL) */
        n = SDL_sscanf(buf + i + 3, "i=%u;%127[^\x1b\x07]", &id, payload);
        if (n < 2 || (Uint32)id != expect_id) {
            continue;
        }
        if (SDL_strncmp(payload, "OK", 2) == 0 &&
            (payload[2] == '\0' || payload[2] == '\x1b' || payload[2] == '\x07' ||
             payload[2] == ' ' || payload[2] == '\r' || payload[2] == '\n')) {
            return KITTY_GQ_OK;
        }
        return KITTY_GQ_ERROR;
    }
    return KITTY_GQ_NONE;
}

int KITTY_FormatTransmitHeader(char *buf, size_t cap,
                               char medium, Uint32 image_id,
                               int w, int h, int place_cols, int place_rows,
                               bool more, const char *extra,
                               size_t shm_bytes)
{
    const bool use_cr = place_cols > 0 && place_rows > 0;
    char extra_part[64];
    extra_part[0] = '\0';
    if (extra && *extra) {
        SDL_snprintf(extra_part, sizeof(extra_part), ",%s", extra);
    }

    if (!buf || cap < 32 || (medium != 'd' && medium != 's') || w <= 0 || h <= 0) {
        return -1;
    }

    if (medium == 's') {
        if (use_cr) {
            return SDL_snprintf(buf, cap,
                                "a=T,f=24,t=s,i=%u,s=%d,v=%d,c=%d,r=%d,S=%zu,%s%s%s",
                                (unsigned)image_id, w, h, place_cols, place_rows, shm_bytes,
                                KITTY_AT_PLACE_KEYS,
                                extra_part,
                                more ? ",m=1" : "");
        }
        return SDL_snprintf(buf, cap,
                            "a=T,f=24,t=s,i=%u,s=%d,v=%d,S=%zu,%s%s%s",
                            (unsigned)image_id, w, h, shm_bytes,
                            KITTY_AT_PLACE_KEYS,
                            extra_part,
                            more ? ",m=1" : "");
    }

    if (use_cr) {
        return SDL_snprintf(buf, cap,
                            "a=T,f=24,t=d,i=%u,s=%d,v=%d,c=%d,r=%d,%s%s%s",
                            (unsigned)image_id, w, h, place_cols, place_rows,
                            KITTY_AT_PLACE_KEYS,
                            extra_part,
                            more ? ",m=1" : "");
    }
    return SDL_snprintf(buf, cap,
                        "a=T,f=24,t=d,i=%u,s=%d,v=%d,%s%s%s",
                        (unsigned)image_id, w, h,
                        KITTY_AT_PLACE_KEYS,
                        extra_part,
                        more ? ",m=1" : "");
}

KITTY_ResizeAction KITTY_ClassifyResize(bool place_cells, bool has_live_image,
                                        int old_pw, int old_ph, int new_pw, int new_ph,
                                        int old_cols, int old_rows, int new_cols, int new_rows)
{
    if (new_pw <= 0 || new_ph <= 0) {
        return KITTY_RESIZE_FULL;
    }
    if (old_pw == new_pw && old_ph == new_ph &&
        old_cols == new_cols && old_rows == new_rows) {
        return KITTY_RESIZE_NONE;
    }
    /* Stretch + same FB pixels + live image: only c/r need updating. */
    if (place_cells && has_live_image &&
        old_pw == new_pw && old_ph == new_ph &&
        (old_cols != new_cols || old_rows != new_rows)) {
        return KITTY_RESIZE_REPLACE;
    }
    /* 1:1 (or no live image) with unchanged FB: nothing to re-encode. */
    if (old_pw == new_pw && old_ph == new_ph) {
        return KITTY_RESIZE_NONE;
    }
    return KITTY_RESIZE_FULL;
}

KITTY_TransmitMode KITTY_SelectTransmitMode(const char *force_env,
                                            const char *term_program,
                                            bool shm_ok,
                                            bool direct_ok)
{
    (void)direct_ok;

    if (force_env && *force_env) {
        if (SDL_strcasecmp(force_env, "direct") == 0) {
            return KITTY_TRANSMIT_DIRECT;
        }
        if (SDL_strcasecmp(force_env, "zlib") == 0) {
            return KITTY_TRANSMIT_DIRECT_ZLIB;
        }
        if (SDL_strcasecmp(force_env, "shm") == 0) {
            return KITTY_TRANSMIT_SHM;
        }
    }

    /* Rex: SHM/zlib historically blank or flaky — stay on direct unless forced. */
    if (term_program && SDL_strcasecmp(term_program, "rex") == 0) {
        return KITTY_TRANSMIT_DIRECT;
    }

    if (shm_ok) {
        return KITTY_TRANSMIT_SHM;
    }
    return KITTY_TRANSMIT_DIRECT;
}

#ifndef KITTY_PROBE_STANDALONE

static bool KITTY_WriteAllProbe(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static size_t KITTY_B64EncodeLocal(const Uint8 *in, size_t in_len, char *out, size_t out_cap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i < in_len) {
        const size_t remain = in_len - i;
        const Uint32 b0 = in[i++];
        const Uint32 b1 = (remain > 1) ? in[i++] : 0;
        const Uint32 b2 = (remain > 2) ? in[i++] : 0;
        const Uint32 triple = (b0 << 16) | (b1 << 8) | b2;
        if (o + 4 >= out_cap) {
            return 0;
        }
        out[o++] = tbl[(triple >> 18) & 63];
        out[o++] = tbl[(triple >> 12) & 63];
        out[o++] = (remain > 1) ? tbl[(triple >> 6) & 63] : '=';
        out[o++] = (remain > 2) ? tbl[triple & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static KITTY_GraphicsQueryStatus KITTY_AwaitGraphicsQuery(int fd, Uint32 id, int attempts)
{
    unsigned char buf[512];
    size_t total = 0;
    int a;

    for (a = 0; a < attempts; ++a) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 25000;
        if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
            const ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
            if (n > 0) {
                total += (size_t)n;
                buf[total] = '\0';
                {
                    const KITTY_GraphicsQueryStatus st =
                        KITTY_ParseGraphicsQueryReply((const char *)buf, total, id);
                    if (st != KITTY_GQ_NONE) {
                        return st;
                    }
                }
            }
        }
        if (total >= sizeof(buf) - 1) {
            break;
        }
    }
    return KITTY_GQ_NONE;
}

static bool KITTY_ProbeDirectQuery(SDL_VideoData *data, Uint32 id)
{
    /* 1x1 RGB black: 3 bytes → base64 "AAAA" */
    char cmd[128];
    const int n = SDL_snprintf(cmd, sizeof(cmd),
                               "\033_Ga=q,t=d,f=24,s=1,v=1,i=%u;AAAA\033\\\033[c",
                               (unsigned)id);
    if (n <= 0 || !KITTY_WriteAllProbe(data->tty_fd, cmd, (size_t)n)) {
        return false;
    }
    return KITTY_AwaitGraphicsQuery(data->tty_fd, id, 40) == KITTY_GQ_OK;
}

static bool KITTY_ProbeShmQuery(SDL_VideoData *data, Uint32 id)
{
    char name[64];
    const Uint8 pixel[3] = { 0, 0, 0 };
    const size_t raw_len = 3;
    int shm_fd = -1;
    void *map = MAP_FAILED;
    bool ok = false;
    char name_b64[128];
    size_t encoded;
    char header[192];
    int header_len;

    SDL_snprintf(name, sizeof(name), "/sdl-kitty-probe-%d", (int)getpid());
    shm_unlink(name);
    shm_fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (shm_fd < 0) {
        return false;
    }
    if (ftruncate(shm_fd, (off_t)raw_len) != 0) {
        goto done;
    }
    map = mmap(NULL, raw_len, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) {
        goto done;
    }
    SDL_memcpy(map, pixel, raw_len);

    encoded = KITTY_B64EncodeLocal((const Uint8 *)name, SDL_strlen(name), name_b64, sizeof(name_b64));
    if (encoded == 0) {
        goto done;
    }
    header_len = SDL_snprintf(header, sizeof(header),
                              "\033_Ga=q,t=s,f=24,s=1,v=1,i=%u,S=%zu;",
                              (unsigned)id, raw_len);
    if (header_len <= 0 ||
        !KITTY_WriteAllProbe(data->tty_fd, header, (size_t)header_len) ||
        !KITTY_WriteAllProbe(data->tty_fd, name_b64, encoded) ||
        !KITTY_WriteAllProbe(data->tty_fd, "\033\\\033[c", 5)) {
        goto done;
    }
    ok = (KITTY_AwaitGraphicsQuery(data->tty_fd, id, 40) == KITTY_GQ_OK);

done:
    if (map != MAP_FAILED) {
        munmap(map, raw_len);
    }
    if (shm_fd >= 0) {
        close(shm_fd);
    }
    if (!ok) {
        shm_unlink(name);
    }
    return ok;
}

bool KITTY_ProbeTransmitModes(SDL_VideoData *data)
{
    const char *force = SDL_getenv("SDL_KITTY_TRANSMIT");
    const char *tp = SDL_getenv("TERM_PROGRAM");
    bool direct_ok = false;
    bool shm_ok = false;
    const Uint32 id_direct = 98;
    const Uint32 id_shm = 99;

    if (!data) {
        return false;
    }

    /* Drain any prior CSI noise. */
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
    }

    direct_ok = KITTY_ProbeDirectQuery(data, id_direct);
    /* Skip SHM probe on Rex — SelectTransmitMode would ignore it anyway, and
     * creating SHM objects just to fail wastes time. Still probe if forced. */
    {
        const bool probe_shm = (force && SDL_strcasecmp(force, "shm") == 0) ||
                               !(tp && SDL_strcasecmp(tp, "rex") == 0);
        if (probe_shm) {
            shm_ok = KITTY_ProbeShmQuery(data, id_shm);
        }

        data->transmit_mode = KITTY_SelectTransmitMode(force, tp, shm_ok, direct_ok);
        data->shm_failed = !shm_ok;

        {
            const char *dbg = SDL_getenv("SDL_KITTY_DEBUG");
            if (dbg && dbg[0] && dbg[0] != '0') {
                FILE *log = fopen("/tmp/sdl_kitty_debug.log", "a");
                if (log) {
                    const char *mode = "direct";
                    if (data->transmit_mode == KITTY_TRANSMIT_SHM) {
                        mode = "shm";
                    } else if (data->transmit_mode == KITTY_TRANSMIT_DIRECT_ZLIB) {
                        mode = "zlib";
                    }
                    fprintf(log, "transmit_probe direct=%s shm=%s force=%s term=%s -> %s\n",
                            direct_ok ? "ok" : "fail",
                            probe_shm ? (shm_ok ? "ok" : "fail") : "skip",
                            force ? force : "(none)",
                            tp ? tp : "(none)",
                            mode);
                    fclose(log);
                }
            }
        }
    }

    return true;
}

#endif /* !KITTY_PROBE_STANDALONE */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROBE_STANDALONE */
