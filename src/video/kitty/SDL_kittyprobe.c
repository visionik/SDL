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

#include <stdio.h>
#include <stdarg.h>

#ifndef KITTY_PROBE_STANDALONE
#include "SDL_kittygraphics.h"
#include "SDL_kittyprofile.h"
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
        const char *p;
        const char *semi;
        char *end = NULL;
        size_t plen;
        char payload[128];

        if ((unsigned char)buf[i] != 0x1b || buf[i + 1] != '_') {
            continue;
        }
        if (buf[i + 2] != 'G') {
            continue;
        }
        /*
         * Forms (ST may be ESC\ or BEL):
         *   Gi=31;OK
         *   Gi=31;EBADF:...
         *   Gi=97,r=1;OK          (Ghostty tip a=f echoes r=)
         *   Gi=1,I=2,p=3;OK
         */
        p = buf + i + 3;
        if (p[0] != 'i' || p[1] != '=') {
            continue;
        }
        id = (unsigned)SDL_strtoul(p + 2, &end, 10);
        if (end == p + 2 || (Uint32)id != expect_id) {
            continue;
        }
        /* Skip optional ,key=value pairs until ';' */
        p = end;
        while (*p && *p != ';' && (unsigned char)*p != 0x1b && *p != '\x07') {
            ++p;
        }
        if (*p != ';') {
            continue;
        }
        semi = p;
        plen = 0;
        p = semi + 1;
        while (plen + 1 < sizeof(payload) && p < buf + len &&
               (unsigned char)*p != 0x1b && *p != '\x07') {
            payload[plen++] = *p++;
        }
        payload[plen] = '\0';
        if (SDL_strncmp(payload, "OK", 2) == 0 &&
            (payload[2] == '\0' || payload[2] == ' ' || payload[2] == '\r' ||
             payload[2] == '\n')) {
            return KITTY_GQ_OK;
        }
        if (payload[0] != '\0') {
            return KITTY_GQ_ERROR;
        }
    }
    return KITTY_GQ_NONE;
}

bool KITTY_SelectFbAlpha(const char *env)
{
    if (!env || !env[0]) {
        return false;
    }
    if (SDL_strcasecmp(env, "1") == 0 ||
        SDL_strcasecmp(env, "true") == 0 ||
        SDL_strcasecmp(env, "yes") == 0 ||
        SDL_strcasecmp(env, "on") == 0) {
        return true;
    }
    return false;
}

int KITTY_FormatTransmitHeader(char *buf, size_t cap,
                               char medium, Uint32 image_id,
                               int w, int h, int place_cols, int place_rows,
                               bool more, const char *extra,
                               size_t shm_bytes,
                               int kitty_f)
{
    const bool use_cr = place_cols > 0 && place_rows > 0;
    char extra_part[64];
    extra_part[0] = '\0';
    if (extra && *extra) {
        SDL_snprintf(extra_part, sizeof(extra_part), ",%s", extra);
    }

    if (kitty_f != 24 && kitty_f != 32) {
        kitty_f = 24;
    }

    if (!buf || cap < 32 || (medium != 'd' && medium != 's') || w <= 0 || h <= 0) {
        return -1;
    }

    if (medium == 's') {
        if (use_cr) {
            return SDL_snprintf(buf, cap,
                                "a=T,f=%d,t=s,i=%u,s=%d,v=%d,c=%d,r=%d,S=%zu,%s%s%s",
                                kitty_f, (unsigned)image_id, w, h, place_cols, place_rows, shm_bytes,
                                KITTY_AT_PLACE_KEYS,
                                extra_part,
                                more ? ",m=1" : "");
        }
        return SDL_snprintf(buf, cap,
                            "a=T,f=%d,t=s,i=%u,s=%d,v=%d,S=%zu,%s%s%s",
                            kitty_f, (unsigned)image_id, w, h, shm_bytes,
                            KITTY_AT_PLACE_KEYS,
                            extra_part,
                            more ? ",m=1" : "");
    }

    if (use_cr) {
        return SDL_snprintf(buf, cap,
                            "a=T,f=%d,t=d,i=%u,s=%d,v=%d,c=%d,r=%d,%s%s%s",
                            kitty_f, (unsigned)image_id, w, h, place_cols, place_rows,
                            KITTY_AT_PLACE_KEYS,
                            extra_part,
                            more ? ",m=1" : "");
    }
    return SDL_snprintf(buf, cap,
                        "a=T,f=%d,t=d,i=%u,s=%d,v=%d,%s%s%s",
                        kitty_f, (unsigned)image_id, w, h,
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

bool KITTY_SelectDirtyBW(const char *env, bool frame_edit_ok)
{
    if (env && *env) {
        if (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
            SDL_strcasecmp(env, "false") == 0 || SDL_strcasecmp(env, "no") == 0) {
            return false;
        }
        return true; /* explicit enable */
    }
    return frame_edit_ok;
}

bool KITTY_SelectPingPong(const char *env, bool default_on)
{
    if (!env || !*env) {
        return default_on;
    }
    if (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
        SDL_strcasecmp(env, "false") == 0 || SDL_strcasecmp(env, "no") == 0) {
        return false;
    }
    return true;
}

bool KITTY_DebugEnabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *dbg = SDL_getenv("SDL_KITTY_DEBUG");
        cached = (dbg && dbg[0] && dbg[0] != '0') ? 1 : 0;
        if (cached) {
#ifndef KITTY_PROBE_STANDALONE
            SDL_SetLogPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_DEBUG);
#endif
            /* macOS SDL_Log often bypasses redirected stderr (os_log). */
            setvbuf(stderr, NULL, _IOLBF, 0);
        }
    }
    return cached != 0;
}

void KITTY_DebugLog(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    if (!KITTY_DebugEnabled() || !fmt) {
        return;
    }
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifndef KITTY_PROBE_STANDALONE
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "%s", buf);
#endif
    fprintf(stderr, "%s\r\n", buf);
    fflush(stderr);
}

KITTY_TransmitMode KITTY_SelectTransmitMode(const char *force_env,
                                            bool prefer_direct,
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

    /* Host profile (e.g. Rex) may prefer direct even when SHM probes OK. */
    if (prefer_direct) {
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
    if (n <= 0 || !KITTY_WriteAllProbe(data->tty_out, cmd, (size_t)n)) {
        return false;
    }
    return KITTY_AwaitGraphicsQuery(data->tty_in, id,
                                    KITTY_ProfileProbeAttempts(data->term_profile)) == KITTY_GQ_OK;
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
        !KITTY_WriteAllProbe(data->tty_out, header, (size_t)header_len) ||
        !KITTY_WriteAllProbe(data->tty_out, name_b64, encoded) ||
        !KITTY_WriteAllProbe(data->tty_out, "\033\\\033[c", 5)) {
        goto done;
    }
    ok = (KITTY_AwaitGraphicsQuery(data->tty_in, id,
                                   KITTY_ProfileProbeAttempts(data->term_profile)) == KITTY_GQ_OK);

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

static void KITTY_DrainTty(int fd)
{
    unsigned char junk[256];
    for (;;) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            break;
        }
        if (read(fd, junk, sizeof(junk)) <= 0) {
            break;
        }
    }
}

/* Transmit-only 2x2 image, then a=f edit one pixel; delete. No on-screen place.
 * Hosts with silent_af_ok (Rex) may apply a=f without an APC OK — treat ERROR
 * as fail and OK or silent (NONE) as success. Strict hosts require OK. */
static bool KITTY_ProbeFrameEdit(SDL_VideoData *data, Uint32 id)
{
    const Uint8 rgb2x2[12] = { 0 };
    char rgb_b64[32];
    size_t enc;
    char cmd[192];
    int n;
    KITTY_GraphicsQueryStatus st;
    bool ok = false;
    const bool silent_ok =
        data->term_profile ? data->term_profile->silent_af_ok : true;

    enc = KITTY_B64EncodeLocal(rgb2x2, sizeof(rgb2x2), rgb_b64, sizeof(rgb_b64));
    if (enc == 0) {
        return false;
    }

    /* a=t stores pixels without placing (avoids a visible probe flash). */
    n = SDL_snprintf(cmd, sizeof(cmd),
                     "\033_Ga=t,f=24,t=d,i=%u,s=2,v=2;%s\033\\",
                     (unsigned)id, rgb_b64);
    if (n <= 0 || !KITTY_WriteAllProbe(data->tty_out, cmd, (size_t)n)) {
        return false;
    }
    {
        int half = KITTY_ProfileProbeAttempts(data->term_profile) / 2;
        if (half < 1) {
            half = 1;
        }
        (void)KITTY_AwaitGraphicsQuery(data->tty_in, id, half);
    }
    KITTY_DrainTty(data->tty_in);

    /* Edit root frame (r=1) — this is what dirty BW needs. */
    n = SDL_snprintf(cmd, sizeof(cmd),
                     "\033_Ga=f,i=%u,r=1,X=1,x=0,y=0,s=1,v=1,f=24,t=d;AAAA\033\\\033[c",
                     (unsigned)id);
    if (n <= 0 || !KITTY_WriteAllProbe(data->tty_out, cmd, (size_t)n)) {
        goto cleanup;
    }
    st = KITTY_AwaitGraphicsQuery(data->tty_in, id,
                                  KITTY_ProfileProbeAttempts(data->term_profile));
    if (silent_ok) {
        ok = (st != KITTY_GQ_ERROR);
    } else {
        ok = (st == KITTY_GQ_OK);
    }

cleanup:
    n = SDL_snprintf(cmd, sizeof(cmd), "\033_Ga=d,d=I,i=%u,q=2\033\\", (unsigned)id);
    if (n > 0) {
        (void)KITTY_WriteAllProbe(data->tty_out, cmd, (size_t)n);
    }
    KITTY_DrainTty(data->tty_in);
    return ok;
}

bool KITTY_ProbeTransmitModes(SDL_VideoData *data)
{
    const char *force = SDL_getenv("SDL_KITTY_TRANSMIT");
    const char *bw_env = SDL_getenv("SDL_KITTY_DIRTY_BW");
    const char *term_name = "unknown";
    bool prefer_direct = false;
    bool direct_ok = false;
    bool shm_ok = false;
    bool frame_ok = false;
    const Uint32 id_direct = 98;
    const Uint32 id_shm = 99;
    const Uint32 id_frame = 97;

    if (!data) {
        return false;
    }
    data->frame_edit_ok = false;
    if (data->term_profile) {
        prefer_direct = data->term_profile->prefer_direct;
        term_name = data->term_profile->name;
    }

    /* Drain any prior CSI noise from the input tty. */
    KITTY_DrainTty(data->tty_in);

    direct_ok = KITTY_ProbeDirectQuery(data, id_direct);
    /* Skip SHM probe when the profile prefers direct (Rex) — SelectTransmitMode
     * would ignore SHM anyway. Still probe if SDL_KITTY_TRANSMIT=shm. */
    {
        const bool probe_shm =
            (force && SDL_strcasecmp(force, "shm") == 0) || !prefer_direct;
        if (probe_shm) {
            shm_ok = KITTY_ProbeShmQuery(data, id_shm);
        }

        data->transmit_mode =
            KITTY_SelectTransmitMode(force, prefer_direct, shm_ok, direct_ok);
        /* If mode is SHM (probe OK or SDL_KITTY_TRANSMIT=shm), clear the
         * failure latch so the first present actually tries t=s. A failed
         * a=q probe must not silently skip SHM when the user forced it. */
        if (data->transmit_mode == KITTY_TRANSMIT_SHM) {
            data->shm_failed = false;
        } else {
            data->shm_failed = !shm_ok;
        }

        /* Frame-edit probe only if graphics direct works (or we still try). */
        frame_ok = KITTY_ProbeFrameEdit(data, id_frame);
        data->frame_edit_ok = frame_ok;

        if (KITTY_DebugEnabled()) {
            const char *mode = "direct";
            const bool bw = KITTY_SelectDirtyBW(bw_env, frame_ok);
            if (data->transmit_mode == KITTY_TRANSMIT_SHM) {
                mode = "shm";
            } else if (data->transmit_mode == KITTY_TRANSMIT_DIRECT_ZLIB) {
                mode = "zlib";
            }
            KITTY_DebugLog("kitty: transmit_probe direct=%s shm=%s force=%s term=%s prefer_direct=%d -> %s",
                           direct_ok ? "ok" : "fail",
                           probe_shm ? (shm_ok ? "ok" : "fail") : "skip",
                           force ? force : "(none)",
                           term_name,
                           prefer_direct ? 1 : 0,
                           mode);
            KITTY_DebugLog("kitty: frame_edit_probe %s -> dirty_bw=%s (env=%s)",
                           frame_ok ? "ok" : "fail",
                           bw ? "on" : "off",
                           bw_env ? bw_env : "(none)");
        }
    }

    return true;
}

#endif /* !KITTY_PROBE_STANDALONE */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROBE_STANDALONE */
