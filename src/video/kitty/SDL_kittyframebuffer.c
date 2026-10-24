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

#include "../../SDL_properties_c.h"
#include "../SDL_sysvideo.h"
#include "SDL_kittyframebuffer_c.h"
#include "SDL_kittygraphics.h"
#include "SDL_kittyprobe.h"
#include "SDL_kittyprofile.h"
#include "SDL_kittyrect.h"

#define KITTY_SURFACE "SDL.internal.window.surface"

static void KITTY_FreeRGBScratch(SDL_WindowData *wdata)
{
    if (!wdata) {
        return;
    }
    SDL_free(wdata->rgb_scratch);
    wdata->rgb_scratch = NULL;
    SDL_free(wdata->rgb_back);
    wdata->rgb_back = NULL;
    wdata->rgb_w = 0;
    wdata->rgb_h = 0;
    wdata->scratch_bpp = 0;
    wdata->rgb_valid = false;
    wdata->live_image_id = 0;
}

static bool KITTY_EnsureRGBScratch(SDL_WindowData *wdata, int w, int h, int bpp)
{
    const size_t need = (size_t)w * (size_t)h * (size_t)bpp;
    if (!wdata || w <= 0 || h <= 0 || (bpp != 3 && bpp != 4)) {
        return false;
    }
    if (wdata->rgb_scratch && wdata->rgb_back && wdata->rgb_w == w && wdata->rgb_h == h &&
        wdata->scratch_bpp == bpp) {
        return true;
    }
    KITTY_FreeRGBScratch(wdata);
    wdata->rgb_scratch = (Uint8 *)SDL_malloc(need);
    wdata->rgb_back = (Uint8 *)SDL_malloc(need);
    if (!wdata->rgb_scratch || !wdata->rgb_back) {
        KITTY_FreeRGBScratch(wdata);
        return false;
    }
    wdata->rgb_w = w;
    wdata->rgb_h = h;
    wdata->scratch_bpp = bpp;
    wdata->rgb_valid = false;
    wdata->live_image_id = 0;
    return true;
}

static void KITTY_SwapRGBScratch(SDL_WindowData *wdata)
{
    Uint8 *tmp;
    if (!wdata) {
        return;
    }
    tmp = wdata->rgb_scratch;
    wdata->rgb_scratch = wdata->rgb_back;
    wdata->rgb_back = tmp;
}

bool SDL_KITTY_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *wdata = window ? (SDL_WindowData *)window->internal : NULL;
    SDL_Surface *surface;
    const bool want_alpha = KITTY_SelectFbAlpha(SDL_getenv("SDL_KITTY_FB_ALPHA"));
    const SDL_PixelFormat surface_format = want_alpha ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888;
    int w, h;
    (void)_this;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return SDL_SetError("invalid kitty framebuffer size");
    }
    surface = SDL_CreateSurface(w, h, surface_format);
    if (!surface) {
        return false;
    }

    if (wdata) {
        KITTY_FreeRGBScratch(wdata);
    }

    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), KITTY_SURFACE, surface);
    *format = surface_format;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return true;
}

bool SDL_KITTY_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *vdata = (SDL_VideoData *)_this->internal;
    SDL_WindowData *wdata = (SDL_WindowData *)window->internal;
    SDL_Surface *surface;
    bool ok = false;
    Uint32 image_id;
    SDL_Rect dirty;
    KITTY_PresentConvertMode convert_mode;
    KITTY_PresentWireMode wire_mode = KITTY_WIRE_FULL;
    SDL_Rect full;
    size_t convert_px;
    size_t upload_px;
    Uint8 *packed = NULL;

    surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), KITTY_SURFACE, NULL);
    if (!surface) {
        return SDL_SetError("Couldn't find kitty surface for window");
    }
    if (!vdata || !wdata) {
        return SDL_SetError("kitty video/window data missing");
    }

    {
        const bool use_rgba = SDL_ISPIXELFORMAT_ALPHA(surface->format);
        const int bpp = use_rgba ? 4 : 3;
        const int kitty_f = use_rgba ? 32 : 24;

        if (!KITTY_EnsureRGBScratch(wdata, surface->w, surface->h, bpp)) {
            return false;
        }

        convert_mode = KITTY_ChooseConvertMode(surface->w, surface->h, rects, numrects,
                                               wdata->rgb_valid, &dirty);
        full.x = 0;
        full.y = 0;
        full.w = surface->w;
        full.h = surface->h;

        /*
         * Tile-diff for full-frame presents (MAME soft always flips the whole
         * surface). Convert into rgb_back, memcmp 32x32 tiles vs rgb_scratch,
         * then sparse a=f on the bbox — or skip if identical.
         */
        {
            const float app_cov = KITTY_CoverageRatio(surface->w, surface->h, &dirty);
            const bool want_diff =
                KITTY_SelectFbDiff(SDL_getenv("SDL_KITTY_FB_DIFF"),
                                   vdata->term_profile ? vdata->term_profile->fb_diff : true) &&
                                   wdata->rgb_valid && wdata->rgb_back &&
                                   app_cov >= 0.90f;
            if (want_diff) {
                const bool conv_ok = use_rgba
                                         ? KITTY_ConvertSurfaceRectToRGBA(surface, &full, wdata->rgb_back)
                                         : KITTY_ConvertSurfaceRectToRGB(surface, &full, wdata->rgb_back);
                if (!conv_ok) {
                    return SDL_SetError("kitty RGB convert failed");
                }
                {
                    const int tile = KITTY_SelectFbDiffTile(
                        SDL_getenv("SDL_KITTY_FB_DIFF_TILE"),
                        vdata->term_profile ? vdata->term_profile->fb_diff_tile : 32);
                    const float cover = KITTY_SelectFbDiffCover(
                        SDL_getenv("SDL_KITTY_FB_DIFF_COVER"),
                        vdata->term_profile ? vdata->term_profile->fb_diff_cover : 0.45f);
                    const KITTY_DiffOutcome diff =
                        KITTY_TileDiffBBox(wdata->rgb_scratch, wdata->rgb_back, surface->w, surface->h,
                                           bpp, tile, cover, &dirty);
                    if (diff == KITTY_DIFF_NONE) {
                        if (KITTY_DebugEnabled()) {
                            static int skips;
                            if (skips < 4 || (skips % 120) == 0) {
                                SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                                             "kitty: present diff_skip (identical FB)");
                            }
                            skips++;
                        }
                        return true;
                    }
                    KITTY_SwapRGBScratch(wdata); /* scratch now holds the new frame */
                    if (diff == KITTY_DIFF_FULL) {
                        dirty = full;
                        convert_mode = KITTY_CONVERT_FULL;
                    } else {
                        convert_mode = KITTY_CONVERT_DIRTY;
                    }
                }
                goto kitty_present_after_convert;
            }
        }

        if (convert_mode == KITTY_CONVERT_DIRTY) {
            const bool conv_ok = use_rgba
                                     ? KITTY_ConvertSurfaceRectToRGBA(surface, &dirty, wdata->rgb_scratch)
                                     : KITTY_ConvertSurfaceRectToRGB(surface, &dirty, wdata->rgb_scratch);
            if (!conv_ok) {
                convert_mode = KITTY_CONVERT_FULL;
            }
        }
        if (convert_mode == KITTY_CONVERT_FULL) {
            const bool conv_ok = use_rgba
                                     ? KITTY_ConvertSurfaceRectToRGBA(surface, &full, wdata->rgb_scratch)
                                     : KITTY_ConvertSurfaceRectToRGB(surface, &full, wdata->rgb_scratch);
            if (!conv_ok) {
                return SDL_SetError("kitty RGB convert failed");
            }
            dirty = full;
        }

    kitty_present_after_convert:
        convert_px = (size_t)dirty.w * (size_t)dirty.h;
        upload_px = (size_t)surface->w * (size_t)surface->h;
        wire_mode = (convert_mode == KITTY_CONVERT_DIRTY) ? KITTY_WIRE_DIRTY_CPU : KITTY_WIRE_FULL;

        /* Sparse direct a=f. Prefer per-rect edits when the app passed multiple
         * dirty rects (alpha_blit discs) — a single union of far-apart rects is
         * huge and either flashes (full a=T) or risks base64 (giant direct a=f).
         * SHM a=f blanked on Kitty.app; keep a=f on t=d only. */
        {
            const float cov = KITTY_CoverageRatio(surface->w, surface->h, &dirty);
            const float max_cov =
                (vdata->term_profile && vdata->term_profile->af_max_coverage > 0.0f)
                    ? vdata->term_profile->af_max_coverage
                    : 0.40f;
            const size_t max_af_px =
                (vdata->term_profile && vdata->term_profile->af_max_pixels > 0)
                    ? vdata->term_profile->af_max_pixels
                    : ((size_t)160 * 1024); /* ~160k px ≈ 480KB RGB */
            /* Kitty SHM sessions: skip direct a=f unless the profile allows it
             * (direct_af_when_shm). Rex keeps direct a=f; Kitty uses full SHM a=T. */
            const bool shm_session =
                (vdata->transmit_mode == KITTY_TRANSMIT_SHM && !vdata->shm_failed);
            const bool allow_direct_af =
                !shm_session ||
                (vdata->term_profile && vdata->term_profile->direct_af_when_shm);
            const bool want_af =
                convert_mode == KITTY_CONVERT_DIRTY &&
                allow_direct_af &&
                KITTY_SelectDirtyBW(SDL_getenv("SDL_KITTY_DIRTY_BW"), vdata->frame_edit_ok) &&
                wdata->placed &&
                wdata->live_image_id != 0;

            if (want_af && rects && numrects > 1) {
                bool multi_ok = true;
                size_t multi_upload = 0;
                int ri;
                SDL_Rect fb = { 0, 0, surface->w, surface->h };

                for (ri = 0; ri < numrects && multi_ok; ++ri) {
                    SDL_Rect clipped;
                    size_t rpx;
                    size_t packed_len = 0;
                    bool pack_ok = false;

                    if (!SDL_GetRectIntersection(&rects[ri], &fb, &clipped) ||
                        clipped.w <= 0 || clipped.h <= 0) {
                        continue;
                    }
                    rpx = (size_t)clipped.w * (size_t)clipped.h;
                    if (rpx > max_af_px) {
                        multi_ok = false;
                        break;
                    }
                    packed = (Uint8 *)SDL_malloc(rpx * (size_t)bpp);
                    if (!packed) {
                        multi_ok = false;
                        break;
                    }
                    pack_ok = use_rgba
                                  ? KITTY_PackRGBARect(wdata->rgb_scratch, surface->w, surface->h,
                                                       &clipped, packed, rpx * (size_t)bpp, &packed_len)
                                  : KITTY_PackRGBRect(wdata->rgb_scratch, surface->w, surface->h,
                                                      &clipped, packed, rpx * (size_t)bpp, &packed_len);
                    if (!pack_ok ||
                        !KITTY_TransmitRGBRectEdit(vdata, wdata->live_image_id, packed,
                                                   clipped.x, clipped.y, clipped.w, clipped.h,
                                                   kitty_f)) {
                        multi_ok = false;
                        SDL_ClearError();
                    } else {
                        multi_upload += rpx;
                    }
                    SDL_free(packed);
                    packed = NULL;
                }

                if (multi_ok && multi_upload > 0) {
                    ok = true;
                    wire_mode = KITTY_WIRE_DIRTY_BW;
                    upload_px = multi_upload;
                    wdata->rgb_valid = true;
                }
            }

            if (!ok && want_af) {
                const bool af_small = (cov < max_cov) && (convert_px <= max_af_px);
                if (af_small) {
                    size_t packed_len = 0;
                    const size_t pack_cap = convert_px * (size_t)bpp;
                    bool pack_ok = false;
                    packed = (Uint8 *)SDL_malloc(pack_cap);
                    if (packed) {
                        pack_ok = use_rgba
                                      ? KITTY_PackRGBARect(wdata->rgb_scratch, surface->w, surface->h,
                                                           &dirty, packed, pack_cap, &packed_len)
                                      : KITTY_PackRGBRect(wdata->rgb_scratch, surface->w, surface->h,
                                                          &dirty, packed, pack_cap, &packed_len);
                    }
                    if (pack_ok &&
                        KITTY_TransmitRGBRectEdit(vdata, wdata->live_image_id, packed,
                                                  dirty.x, dirty.y, dirty.w, dirty.h, kitty_f)) {
                        ok = true;
                        wire_mode = KITTY_WIRE_DIRTY_BW;
                        upload_px = convert_px;
                        wdata->rgb_valid = true;
                    } else {
                        SDL_ClearError();
                    }
                    SDL_free(packed);
                    packed = NULL;
                }
            }
        }

        if (!ok) {
            const bool pingpong = KITTY_SelectPingPong(
                SDL_getenv("SDL_KITTY_PINGPONG"),
                vdata->term_profile && vdata->term_profile->pingpong);
            Uint32 prev = 0;

            if (pingpong) {
                wdata->image_index ^= 1;
                image_id = wdata->image_ids[wdata->image_index];
                prev = wdata->image_ids[wdata->image_index ^ 1];
            } else {
                /* Same (i,p=1) replace: re-a=T the live id; no per-frame delete. */
                image_id = wdata->live_image_id ? wdata->live_image_id : wdata->image_ids[0];
            }

            ok = KITTY_TransmitRGB(vdata, image_id, wdata->rgb_scratch, surface->w, surface->h, kitty_f);
            if (ok) {
                if (pingpong && wdata->placed && prev != 0 && prev != image_id) {
                    KITTY_DeleteImage(vdata->tty_fd, prev);
                }
                wdata->placed = true;
                wdata->rgb_valid = true;
                wdata->live_image_id = image_id;
                if (wire_mode != KITTY_WIRE_DIRTY_BW) {
                    wire_mode = (convert_mode == KITTY_CONVERT_DIRTY) ? KITTY_WIRE_DIRTY_CPU : KITTY_WIRE_FULL;
                }
            }
        }
    }

    if (KITTY_DebugEnabled()) {
        static int frames;
        const float cov = KITTY_CoverageRatio(surface->w, surface->h, &dirty);
        /* Log startup, failures, full presents, high-coverage sparse, or every 60th. */
        const bool log_it = frames < 8 || !ok || wire_mode == KITTY_WIRE_FULL ||
                            (wire_mode == KITTY_WIRE_DIRTY_BW && cov >= 0.5f) ||
                            (frames % 60) == 0;
        if (log_it) {
            const char *mode_str = "full";
            if (wire_mode == KITTY_WIRE_DIRTY_BW) {
                mode_str = "dirty_bw";
            } else if (wire_mode == KITTY_WIRE_DIRTY_CPU) {
                mode_str = "dirty_cpu";
            } else if (wire_mode == KITTY_WIRE_DIFF_SKIP) {
                mode_str = "diff_skip";
            }
            KITTY_DebugLog(
                "kitty: present #%d %dx%d place=%d mode=%s rects=%d union=%dx%d coverage=%.2f convert_px=%zu upload_px=%zu live=%u pingpong=%d -> %s%s",
                frames, surface->w, surface->h,
                vdata->place_cells ? 1 : 0,
                mode_str,
                numrects, dirty.w, dirty.h, (double)cov,
                convert_px, upload_px,
                (unsigned)wdata->live_image_id,
                KITTY_SelectPingPong(SDL_getenv("SDL_KITTY_PINGPONG"),
                                     vdata->term_profile && vdata->term_profile->pingpong)
                    ? 1
                    : 0,
                ok ? "ok" : "FAIL ",
                ok ? "" : SDL_GetError());
        }
        frames++;
    }
    return ok;
}

void SDL_KITTY_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *wdata = window ? (SDL_WindowData *)window->internal : NULL;
    (void)_this;
    if (wdata) {
        KITTY_FreeRGBScratch(wdata);
    }
    SDL_ClearProperty(SDL_GetWindowProperties(window), KITTY_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_KITTY
