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

#include "SDL_kittycapture.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#ifdef SDL_KITTY_HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif

struct KITTY_Capture
{
    bool png_requested;
    bool want_video;
    bool recording;
    bool video_is_png_seq;
    Uint64 rec_start_ms;
    Uint64 last_vid_ms;
    int vid_w;
    int vid_h;
    int vid_fd;
    pid_t vid_pid;
    Uint32 vid_frames;
    Uint32 seq_counter;
    char vid_path[512];
    char vid_base[128];
    Uint8 *rgb_tmp;
    size_t rgb_tmp_cap;
};

static void KITTY_WriteBE32(Uint8 out[4], Uint32 v)
{
    out[0] = (Uint8)((v >> 24) & 0xFFu);
    out[1] = (Uint8)((v >> 16) & 0xFFu);
    out[2] = (Uint8)((v >> 8) & 0xFFu);
    out[3] = (Uint8)(v & 0xFFu);
}

static bool KITTY_WriteFullFd(int fd, const void *buf, size_t len)
{
    const Uint8 *p = (const Uint8 *)buf;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool KITTY_WritePngChunk(int fd, const char type[4], const Uint8 *data, Uint32 len)
{
    Uint8 hdr[8];
    Uint8 crc_b[4];
    Uint32 c;

    KITTY_WriteBE32(hdr, len);
    SDL_memcpy(hdr + 4, type, 4);
#ifdef SDL_KITTY_HAVE_LIBDEFLATE
    c = libdeflate_crc32(0u, type, 4u);
    if (len > 0 && data) {
        c = libdeflate_crc32(c, data, len);
    }
#else
    c = (Uint32)crc32(0L, Z_NULL, 0);
    c = (Uint32)crc32(c, (const Bytef *)type, 4u);
    if (len > 0 && data) {
        c = (Uint32)crc32(c, (const Bytef *)data, len);
    }
#endif
    KITTY_WriteBE32(crc_b, c);
    if (!KITTY_WriteFullFd(fd, hdr, 8)) return false;
    if (len > 0 && data && !KITTY_WriteFullFd(fd, data, len)) return false;
    return KITTY_WriteFullFd(fd, crc_b, 4);
}

bool KITTY_WritePngFile(const char *path, const Uint8 *pixels, int w, int h, int bpp)
{
    static const Uint8 png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const size_t row_bytes = (size_t)w * (size_t)bpp;
    const size_t raw_size = (row_bytes + 1u) * (size_t)h;
    Uint8 *filtered = NULL;
    Uint8 *zbuf = NULL;
    size_t zlen = 0;
    Uint8 ihdr[13];
    int fd = -1;
    int y;
    bool ok = false;

    if (!path || !pixels || w <= 0 || h <= 0 || (bpp != 3 && bpp != 4)) {
        return false;
    }

    filtered = (Uint8 *)SDL_malloc(raw_size);
    if (!filtered) {
        return false;
    }
    /* PNG Filter 1 (Sub): fast and cuts UI/arcade screenshot size ~2-3x. */
    for (y = 0; y < h; ++y) {
        const Uint8 *src = pixels + (size_t)y * row_bytes;
        Uint8 *dst = filtered + (size_t)y * (row_bytes + 1u);
        size_t x;
        dst[0] = 1u;
        for (x = 0; x < (size_t)bpp && x < row_bytes; ++x) {
            dst[1u + x] = src[x];
        }
        for (x = (size_t)bpp; x < row_bytes; ++x) {
            dst[1u + x] = (Uint8)(src[x] - src[x - (size_t)bpp]);
        }
    }

#ifdef SDL_KITTY_HAVE_LIBDEFLATE
    {
        struct libdeflate_compressor *comp = libdeflate_alloc_compressor(4);
        if (comp) {
            const size_t bound = libdeflate_zlib_compress_bound(comp, raw_size);
            zbuf = (Uint8 *)SDL_malloc(bound);
            if (zbuf) {
                zlen = libdeflate_zlib_compress(comp, filtered, raw_size, zbuf, bound);
            }
            libdeflate_free_compressor(comp);
        }
    }
#endif
    if (zlen == 0) {
        uLongf bound = compressBound((uLong)raw_size);
        SDL_free(zbuf);
        zbuf = (Uint8 *)SDL_malloc((size_t)bound);
        if (!zbuf) {
            goto done;
        }
        if (compress2(zbuf, &bound, filtered, (uLong)raw_size, 3) != Z_OK) {
            goto done;
        }
        zlen = (size_t)bound;
    }

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        goto done;
    }
    if (!KITTY_WriteFullFd(fd, png_sig, 8)) {
        goto done;
    }
    KITTY_WriteBE32(ihdr + 0, (Uint32)w);
    KITTY_WriteBE32(ihdr + 4, (Uint32)h);
    ihdr[8] = 8u;                            /* bit depth */
    ihdr[9] = (bpp == 4) ? 6u : 2u;          /* RGBA or RGB */
    ihdr[10] = 0u;                           /* deflate */
    ihdr[11] = 0u;                           /* adaptive filter */
    ihdr[12] = 0u;                           /* non-interlaced */
    if (!KITTY_WritePngChunk(fd, "IHDR", ihdr, 13u)) {
        goto done;
    }
    if (!KITTY_WritePngChunk(fd, "IDAT", zbuf, (Uint32)zlen)) {
        goto done;
    }
    if (!KITTY_WritePngChunk(fd, "IEND", NULL, 0u)) {
        goto done;
    }
    ok = true;

done:
    if (fd >= 0) {
        close(fd);
    }
    SDL_free(zbuf);
    SDL_free(filtered);
    return ok;
}

static void KITTY_EnsureDir(const char *dir)
{
    char tmp[512];
    size_t len, i;
    if (!dir || !dir[0]) return;
    SDL_strlcpy(tmp, dir, sizeof(tmp));
    len = SDL_strlen(tmp);
    for (i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    (void)mkdir(tmp, 0755);
}

static void KITTY_ResolveCaptureDir(char *out, size_t cap)
{
    const char *env = SDL_getenv("SDL_KITTY_CAPTURE_DIR");
    if (env && env[0]) {
        SDL_strlcpy(out, env, cap);
    } else {
        const char *home = SDL_getenv("HOME");
        if (home && home[0]) {
            SDL_snprintf(out, cap, "%s/Pictures/sdl-kitty", home);
        } else {
            SDL_strlcpy(out, "./sdl-kitty-captures", cap);
        }
    }
    KITTY_EnsureDir(out);
}

static void KITTY_FormatTimestamp(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm *tm = localtime_r(&now, &tmbuf);
    if (tm) {
        SDL_snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
        SDL_snprintf(out, cap, "%llu", (unsigned long long)SDL_GetTicks());
    }
}

static bool KITTY_HaveFfmpegOnPath(void)
{
    const char *path = SDL_getenv("PATH");
    const char *p;
    if (!path) return false;
    p = path;
    while (*p) {
        const char *col = p;
        char cand[512];
        size_t len;
        while (*col && *col != ':') col++;
        len = (size_t)(col - p);
        if (len > 0 && len + 10 < sizeof(cand)) {
            SDL_memcpy(cand, p, len);
            SDL_strlcpy(cand + len, "/ffmpeg", sizeof(cand) - len);
            if (access(cand, X_OK) == 0) {
                return true;
            }
        }
        p = *col ? (col + 1) : col;
    }
    return false;
}

static void KITTY_CaptureStopVideoInternal(KITTY_Capture *cap, char *toast_out, size_t toast_cap)
{
    if (!cap || !cap->recording) {
        return;
    }
    if (cap->vid_fd >= 0) {
        close(cap->vid_fd);
        cap->vid_fd = -1;
    }
    if (cap->vid_pid > 0) {
        int status = 0;
        (void)waitpid(cap->vid_pid, &status, 0);
        cap->vid_pid = -1;
    }
    if (toast_out && toast_cap > 0) {
        if (cap->video_is_png_seq) {
            SDL_snprintf(toast_out, toast_cap, "Saved %u PNG frames · %s/",
                         (unsigned)cap->vid_frames, cap->vid_base);
        } else {
            SDL_snprintf(toast_out, toast_cap, "Saved video · %s (%u f)",
                         cap->vid_base, (unsigned)cap->vid_frames);
        }
    }
    cap->recording = false;
    cap->want_video = false;
    cap->video_is_png_seq = false;
    cap->vid_frames = 0;
}

static bool KITTY_CaptureStartVideoInternal(KITTY_Capture *cap, int w, int h, Uint64 now_ms,
                                            char *toast_out, size_t toast_cap)
{
    char dir[384], ts[32];
    KITTY_ResolveCaptureDir(dir, sizeof(dir));
    KITTY_FormatTimestamp(ts, sizeof(ts));
    cap->seq_counter++;
    cap->vid_w = w;
    cap->vid_h = h;
    cap->vid_frames = 0;
    cap->rec_start_ms = now_ms;
    cap->last_vid_ms = 0;

    if (KITTY_HaveFfmpegOnPath()) {
        int pipefd[2];
        pid_t pid;
        char size_arg[32];

        SDL_snprintf(cap->vid_base, sizeof(cap->vid_base), "sdl-kitty-%s.mp4", ts);
        SDL_snprintf(cap->vid_path, sizeof(cap->vid_path), "%s/%s", dir, cap->vid_base);
        SDL_snprintf(size_arg, sizeof(size_arg), "%dx%d", w, h);

        if (pipe(pipefd) == 0) {
            (void)signal(SIGPIPE, SIG_IGN);
            pid = fork();
            if (pid == 0) {
                int devnull = open("/dev/null", O_WRONLY);
                dup2(pipefd[0], STDIN_FILENO);
                if (devnull >= 0) {
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
                close(pipefd[0]);
                close(pipefd[1]);
                {
                    char *argv[] = {
                        (char *)"ffmpeg", (char *)"-y", (char *)"-loglevel", (char *)"error",
                        (char *)"-f", (char *)"rawvideo", (char *)"-pix_fmt", (char *)"rgb24",
                        (char *)"-s", size_arg, (char *)"-r", (char *)"30",
                        (char *)"-i", (char *)"-",
                        (char *)"-c:v", (char *)"libx264", (char *)"-preset", (char *)"ultrafast",
                        (char *)"-crf", (char *)"18", (char *)"-pix_fmt", (char *)"yuv420p",
                        cap->vid_path, NULL
                    };
                    execvp("ffmpeg", argv);
                }
                _exit(127);
            }
            close(pipefd[0]);
            if (pid > 0) {
                cap->vid_fd = pipefd[1];
                cap->vid_pid = pid;
                cap->video_is_png_seq = false;
                cap->recording = true;
                if (toast_out && toast_cap > 0) {
                    SDL_snprintf(toast_out, toast_cap, "Recording MP4 · Shift+F10 to stop");
                }
                return true;
            }
            close(pipefd[1]);
        }
    }

    /* Fallback when ffmpeg is unavailable: lossless PNG sequence directory. */
    SDL_snprintf(cap->vid_base, sizeof(cap->vid_base), "rec-%s", ts);
    SDL_snprintf(cap->vid_path, sizeof(cap->vid_path), "%s/%s", dir, cap->vid_base);
    KITTY_EnsureDir(cap->vid_path);
    cap->vid_fd = -1;
    cap->vid_pid = -1;
    cap->video_is_png_seq = true;
    cap->recording = true;
    if (toast_out && toast_cap > 0) {
        SDL_snprintf(toast_out, toast_cap, "Recording PNG burst (no ffmpeg) · Shift+F10 to stop");
    }
    return true;
}

KITTY_Capture *KITTY_CaptureCreate(void)
{
    KITTY_Capture *cap = (KITTY_Capture *)SDL_calloc(1, sizeof(KITTY_Capture));
    if (cap) {
        cap->vid_fd = -1;
        cap->vid_pid = -1;
    }
    return cap;
}

void KITTY_CaptureDestroy(KITTY_Capture *cap)
{
    if (!cap) return;
    if (cap->recording) {
        KITTY_CaptureStopVideoInternal(cap, NULL, 0);
    }
    SDL_free(cap->rgb_tmp);
    SDL_free(cap);
}

void KITTY_CaptureRequestPng(KITTY_Capture *cap)
{
    if (cap) {
        cap->png_requested = true;
    }
}

void KITTY_CaptureToggleVideo(KITTY_Capture *cap)
{
    if (cap) {
        cap->want_video = !cap->want_video;
    }
}

bool KITTY_CaptureIsRecording(const KITTY_Capture *cap, Uint64 *out_start_ms)
{
    if (!cap || !cap->recording) {
        return false;
    }
    if (out_start_ms) {
        *out_start_ms = cap->rec_start_ms;
    }
    return true;
}

bool KITTY_CaptureOnFrame(KITTY_Capture *cap, const Uint8 *pixels, int w, int h, int bpp,
                          Uint64 now_ms, char *toast_out, size_t toast_cap)
{
    bool has_toast = false;
    if (toast_out && toast_cap > 0) {
        toast_out[0] = '\0';
    }
    if (!cap || !pixels || w <= 0 || h <= 0 || (bpp != 3 && bpp != 4)) {
        return false;
    }

    /* 1) Single-frame lossless PNG screenshot (F10) */
    if (cap->png_requested) {
        char dir[384], ts[32], base[128], full[560];
        cap->png_requested = false;
        cap->seq_counter++;
        KITTY_ResolveCaptureDir(dir, sizeof(dir));
        KITTY_FormatTimestamp(ts, sizeof(ts));
        SDL_snprintf(base, sizeof(base), "sdl-kitty-%s-%02u.png", ts, (unsigned)(cap->seq_counter % 100u));
        SDL_snprintf(full, sizeof(full), "%s/%s", dir, base);
        if (KITTY_WritePngFile(full, pixels, w, h, bpp)) {
            if (toast_out && toast_cap > 0) {
                SDL_snprintf(toast_out, toast_cap, "Saved PNG · %s", base);
                has_toast = true;
            }
        } else if (toast_out && toast_cap > 0) {
            SDL_snprintf(toast_out, toast_cap, "PNG capture failed");
            has_toast = true;
        }
    }

    /* 2) Video recording state transitions (Shift+F10) */
    if (cap->want_video && !cap->recording) {
        if (KITTY_CaptureStartVideoInternal(cap, w, h, now_ms,
                                            has_toast ? NULL : toast_out,
                                            has_toast ? 0 : toast_cap) && !has_toast) {
            has_toast = true;
        }
    } else if (!cap->want_video && cap->recording) {
        KITTY_CaptureStopVideoInternal(cap, has_toast ? NULL : toast_out,
                                       has_toast ? 0 : toast_cap);
        if (!has_toast && toast_out && toast_out[0]) {
            has_toast = true;
        }
    }

    /* 3) Emit video frame (~30 fps cap to keep CPU/IO light) */
    if (cap->recording) {
        if (w != cap->vid_w || h != cap->vid_h) {
            KITTY_CaptureStopVideoInternal(cap, has_toast ? NULL : toast_out,
                                           has_toast ? 0 : toast_cap);
            if (!has_toast && toast_out && toast_out[0]) {
                has_toast = true;
            }
        } else if (cap->last_vid_ms == 0 || (now_ms - cap->last_vid_ms) >= 33u) {
            cap->last_vid_ms = now_ms;
            if (cap->video_is_png_seq) {
                char fpath[576];
                SDL_snprintf(fpath, sizeof(fpath), "%s/frame-%05u.png",
                             cap->vid_path, (unsigned)cap->vid_frames);
                if (KITTY_WritePngFile(fpath, pixels, w, h, bpp)) {
                    cap->vid_frames++;
                }
            } else if (cap->vid_fd >= 0) {
                const size_t rgb_len = (size_t)w * (size_t)h * 3u;
                const Uint8 *send_ptr = pixels;
                if (bpp == 4) {
                    if (cap->rgb_tmp_cap < rgb_len) {
                        Uint8 *nb = (Uint8 *)SDL_realloc(cap->rgb_tmp, rgb_len);
                        if (!nb) return has_toast;
                        cap->rgb_tmp = nb;
                        cap->rgb_tmp_cap = rgb_len;
                    }
                    {
                        const size_t npix = (size_t)w * (size_t)h;
                        size_t i;
                        for (i = 0; i < npix; ++i) {
                            cap->rgb_tmp[i * 3 + 0] = pixels[i * 4 + 0];
                            cap->rgb_tmp[i * 3 + 1] = pixels[i * 4 + 1];
                            cap->rgb_tmp[i * 3 + 2] = pixels[i * 4 + 2];
                        }
                        send_ptr = cap->rgb_tmp;
                    }
                }
                if (KITTY_WriteFullFd(cap->vid_fd, send_ptr, rgb_len)) {
                    cap->vid_frames++;
                } else {
                    KITTY_CaptureStopVideoInternal(cap, has_toast ? NULL : toast_out,
                                                   has_toast ? 0 : toast_cap);
                    if (!has_toast && toast_out && toast_out[0]) {
                        has_toast = true;
                    }
                }
            }
        }
    }

    return has_toast;
}

#endif /* SDL_VIDEO_DRIVER_KITTY */
