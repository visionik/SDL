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
    wdata->overlay_count = 0;
    wdata->last_upload_hash = 0;
    wdata->last_upload_hash_valid = false;
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

    /* Reset per-present perf fields (TransmitRGB / a=f refresh on success). */
    vdata->last_wire_bytes = 0;
    vdata->last_raw_bytes = 0;
    vdata->last_ms_tx = 0.0;
    vdata->last_ms_ack = -1.0;

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
                    /* DIFF_FULL = change bbox is huge. Stay on CONVERT_DIRTY with a
                     * full dirty so af_max_coverage / af_max_pixels still choose
                     * a=f vs a=T. CONVERT_FULL would skip a=f and flash on full a=T
                     * (solid_present on Rex). Sparse DIFF_RECT path unchanged. */
                    if (diff == KITTY_DIFF_FULL) {
                        dirty = full;
                    }
                    convert_mode = KITTY_CONVERT_DIRTY;
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

        /*
         * Exp 6: if the full scratch matches the last successful upload, skip
         * the wire (covers dirty_cpu full a=T storms when pixels did not change).
         */
        {
            const bool want_skip =
                KITTY_SelectUploadSkip(SDL_getenv("SDL_KITTY_UPLOAD_SKIP"),
                                       vdata->term_profile
                                           ? vdata->term_profile->upload_skip
                                           : true) &&
                wdata->placed && wdata->last_upload_hash_valid && wdata->rgb_scratch;
            if (want_skip) {
                const size_t bytes =
                    (size_t)surface->w * (size_t)surface->h * (size_t)bpp;
                const Uint64 h = KITTY_Fnv1a64(wdata->rgb_scratch, bytes);
                if (h == wdata->last_upload_hash) {
                    ok = true;
                    wire_mode = KITTY_WIRE_UPLOAD_SKIP;
                    upload_px = 0;
                    vdata->skip_presents += 1;
                    goto kitty_present_log;
                }
            }
        }

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
                size_t multi_total_px = 0;
                int ri;
                SDL_Rect fb = { 0, 0, surface->w, surface->h };
                SDL_Rect coal_buf[32];
                const SDL_Rect *af_rects = rects;
                int af_n = numrects;
                const int gap = KITTY_SelectCoalesceGap(
                    SDL_getenv("SDL_KITTY_COALESCE_GAP"),
                    vdata->term_profile ? vdata->term_profile->coalesce_gap_px : 0);

                /* Exp 5: merge near/overlapping dirty rects before multi a=f. */
                if (gap > 0) {
                    const int cn = KITTY_CoalesceRects(
                        surface->w, surface->h, rects, numrects, gap, coal_buf,
                        (int)(sizeof(coal_buf) / sizeof(coal_buf[0])));
                    if (cn > 0) {
                        af_rects = coal_buf;
                        af_n = cn;
                        if (KITTY_DebugEnabled() && cn != numrects) {
                            KITTY_DebugLog("kitty: coalesce %d -> %d rects (gap=%d)",
                                           numrects, cn, gap);
                        }
                    }
                }

                /* Same af_max_* gates as the single-rect path: total pixels and
                 * union coverage. Per-rect-only caps allowed alpha_blit (~600k
                 * base64) to flood Kitty's PTY while each rect stayed under max. */
                for (ri = 0; ri < af_n; ++ri) {
                    SDL_Rect clipped;
                    if (!SDL_GetRectIntersection(&af_rects[ri], &fb, &clipped) ||
                        clipped.w <= 0 || clipped.h <= 0) {
                        continue;
                    }
                    multi_total_px += (size_t)clipped.w * (size_t)clipped.h;
                }
                if (cov >= max_cov || multi_total_px > max_af_px || multi_total_px == 0) {
                    multi_ok = false;
                }

                for (ri = 0; multi_ok && ri < af_n; ++ri) {
                    SDL_Rect clipped;
                    size_t rpx;
                    size_t packed_len = 0;
                    bool pack_ok = false;

                    if (!SDL_GetRectIntersection(&af_rects[ri], &fb, &clipped) ||
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

                if (multi_ok && multi_upload > 0 && multi_upload <= max_af_px) {
                    ok = true;
                    wire_mode = KITTY_WIRE_DIRTY_BW;
                    upload_px = multi_upload;
                    wdata->rgb_valid = true;
                    KITTY_ClearOverlays(vdata, wdata);
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
                        /* a=f patched the base; drop any leftover overlays. */
                        KITTY_ClearOverlays(vdata, wdata);
                    } else {
                        SDL_ClearError();
                    }
                    SDL_free(packed);
                    packed = NULL;
                }
            }

            /*
             * Exp 3: single union a=T overlay (safer than multi dirty_at).
             * DECSC/DECRC in TransmitRGBRectPlace; skip if cell box hits edge.
             */
            if (!ok) {
                const bool want_union =
                    convert_mode == KITTY_CONVERT_DIRTY &&
                    KITTY_SelectDirtyUnionAT(SDL_getenv("SDL_KITTY_DIRTY_UNION_AT"),
                                             vdata->term_profile &&
                                                 vdata->term_profile->dirty_union_at) &&
                    wdata->placed &&
                    wdata->live_image_id != 0 &&
                    cov < max_cov &&
                    convert_px <= max_af_px &&
                    dirty.w > 0 && dirty.h > 0;
                if (want_union) {
                    int col0 = 0, row0 = 0, place_c = 0, place_r = 0;
                    int cell_X = 0, cell_Y = 0;
                    bool mapped = false;
                    bool edge = true;

                    if (vdata->place_cells && vdata->cols > 0 && vdata->rows > 0) {
                        mapped = KITTY_FbRectToPlaceCells(surface->w, surface->h, &dirty,
                                                         vdata->cols, vdata->rows,
                                                         &col0, &row0, &place_c, &place_r);
                        edge = KITTY_PlaceCellsTouchesEdge(col0, row0, place_c, place_r,
                                                          vdata->cols, vdata->rows);
                    } else if (vdata->cell_w > 0 && vdata->cell_h > 0) {
                        mapped = KITTY_FbPixelToCellCursor(dirty.x, dirty.y,
                                                          vdata->cell_w, vdata->cell_h,
                                                          &col0, &row0, &cell_X, &cell_Y);
                        place_c = 0;
                        place_r = 0;
                        /* 1:1: treat last cell row/col as edge. */
                        edge = (col0 <= 0 || row0 <= 0 ||
                                (vdata->cols > 0 && col0 >= vdata->cols - 1) ||
                                (vdata->rows > 0 && row0 >= vdata->rows - 1));
                    }

                    if (mapped && !edge) {
                        size_t packed_len = 0;
                        const size_t pack_cap = convert_px * (size_t)bpp;
                        bool pack_ok = false;
                        packed = (Uint8 *)SDL_malloc(pack_cap);
                        if (packed) {
                            pack_ok = use_rgba
                                          ? KITTY_PackRGBARect(wdata->rgb_scratch, surface->w,
                                                               surface->h, &dirty, packed,
                                                               pack_cap, &packed_len)
                                          : KITTY_PackRGBRect(wdata->rgb_scratch, surface->w,
                                                              surface->h, &dirty, packed,
                                                              pack_cap, &packed_len);
                        }
                        KITTY_ClearOverlays(vdata, wdata);
                        /* place_cells: stretch with c/r so overlay matches base scale.
                         * Edge skip + DECSC/DECRC mitigate WezTerm assign_image hang;
                         * natural size (0,0) was small/jumpy and left trails. */
                        if (pack_ok &&
                            KITTY_TransmitRGBRectPlace(vdata, wdata->overlay_ids[0], packed,
                                                       dirty.w, dirty.h,
                                                       col0 + 1, row0 + 1,
                                                       place_c, place_r,
                                                       cell_X, cell_Y, 10, kitty_f)) {
                            ok = true;
                            wire_mode = KITTY_WIRE_DIRTY_AT;
                            upload_px = convert_px;
                            wdata->overlay_count = 1;
                            wdata->rgb_valid = true;
                        } else {
                            SDL_ClearError();
                            wdata->overlay_count = 0;
                        }
                        SDL_free(packed);
                        packed = NULL;
                    }
                }
            }

            /* Multi a=T overlays (dirty_at_overlays) — default off after WezTerm hang. */
            if (!ok) {
                const bool want_at =
                    convert_mode == KITTY_CONVERT_DIRTY &&
                    KITTY_SelectDirtyAT(SDL_getenv("SDL_KITTY_DIRTY_AT"),
                                        vdata->term_profile &&
                                            vdata->term_profile->dirty_at_overlays) &&
                    wdata->placed &&
                    wdata->live_image_id != 0 &&
                    cov < max_cov &&
                    convert_px <= max_af_px;
                if (want_at) {
                    const SDL_Rect *at_rects = rects;
                    int at_n = numrects;
                    SDL_Rect one = dirty;
                    int placed_n = 0;
                    size_t at_upload = 0;
                    bool at_ok = true;
                    int ri;

                    if (!at_rects || at_n <= 0) {
                        at_rects = &one;
                        at_n = 1;
                    }
                    if (at_n > KITTY_OVERLAY_MAX) {
                        at_ok = false;
                    }

                    /* Drop previous overlays before placing this frame's set. */
                    KITTY_ClearOverlays(vdata, wdata);

                    for (ri = 0; at_ok && ri < at_n; ++ri) {
                        SDL_Rect clipped;
                        SDL_Rect fb = { 0, 0, surface->w, surface->h };
                        size_t rpx;
                        size_t packed_len = 0;
                        bool pack_ok = false;
                        int col0 = 0, row0 = 0, place_c = 0, place_r = 0;
                        int cell_X = 0, cell_Y = 0;
                        Uint32 oid;

                        if (!SDL_GetRectIntersection(&at_rects[ri], &fb, &clipped) ||
                            clipped.w <= 0 || clipped.h <= 0) {
                            continue;
                        }
                        rpx = (size_t)clipped.w * (size_t)clipped.h;
                        if (rpx == 0 || at_upload + rpx > max_af_px) {
                            at_ok = false;
                            break;
                        }

                        if (vdata->place_cells && vdata->cols > 0 && vdata->rows > 0) {
                            if (!KITTY_FbRectToPlaceCells(surface->w, surface->h, &clipped,
                                                         vdata->cols, vdata->rows,
                                                         &col0, &row0, &place_c, &place_r)) {
                                at_ok = false;
                                break;
                            }
                        } else if (vdata->cell_w > 0 && vdata->cell_h > 0) {
                            if (!KITTY_FbPixelToCellCursor(clipped.x, clipped.y,
                                                          vdata->cell_w, vdata->cell_h,
                                                          &col0, &row0, &cell_X, &cell_Y)) {
                                at_ok = false;
                                break;
                            }
                            place_c = 0;
                            place_r = 0;
                        } else {
                            at_ok = false;
                            break;
                        }

                        packed = (Uint8 *)SDL_malloc(rpx * (size_t)bpp);
                        if (!packed) {
                            at_ok = false;
                            break;
                        }
                        pack_ok = use_rgba
                                      ? KITTY_PackRGBARect(wdata->rgb_scratch, surface->w, surface->h,
                                                           &clipped, packed, rpx * (size_t)bpp, &packed_len)
                                      : KITTY_PackRGBRect(wdata->rgb_scratch, surface->w, surface->h,
                                                          &clipped, packed, rpx * (size_t)bpp, &packed_len);
                        oid = wdata->overlay_ids[placed_n];
                        if (!pack_ok ||
                            !KITTY_TransmitRGBRectPlace(vdata, oid, packed,
                                                       clipped.w, clipped.h,
                                                       col0 + 1, row0 + 1,
                                                       place_c, place_r,
                                                       cell_X, cell_Y, 10, kitty_f)) {
                            at_ok = false;
                            SDL_ClearError();
                        } else {
                            at_upload += rpx;
                            placed_n++;
                        }
                        SDL_free(packed);
                        packed = NULL;
                    }

                    if (at_ok && placed_n > 0) {
                        ok = true;
                        wire_mode = KITTY_WIRE_DIRTY_AT;
                        upload_px = at_upload;
                        wdata->overlay_count = placed_n;
                        wdata->rgb_valid = true;
                    } else if (placed_n > 0) {
                        /* Partial failure — clear what we placed and fall through. */
                        wdata->overlay_count = placed_n;
                        KITTY_ClearOverlays(vdata, wdata);
                    }
                }
            }
        }

        if (!ok) {
            const bool pingpong = KITTY_SelectPingPong(
                SDL_getenv("SDL_KITTY_PINGPONG"),
                vdata->term_profile && vdata->term_profile->pingpong);
            Uint32 prev = 0;

            /* Full replace must not leave stale overlays on top of the new base. */
            KITTY_ClearOverlays(vdata, wdata);

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
                if (wire_mode != KITTY_WIRE_DIRTY_BW && wire_mode != KITTY_WIRE_DIRTY_AT) {
                    wire_mode = (convert_mode == KITTY_CONVERT_DIRTY) ? KITTY_WIRE_DIRTY_CPU : KITTY_WIRE_FULL;
                }
            }
        }

        /* Fingerprint scratch after any successful wire upload (full or sparse). */
        if (ok && wire_mode != KITTY_WIRE_UPLOAD_SKIP && wdata->rgb_scratch) {
            const size_t bytes =
                (size_t)surface->w * (size_t)surface->h * (size_t)bpp;
            wdata->last_upload_hash = KITTY_Fnv1a64(wdata->rgb_scratch, bytes);
            wdata->last_upload_hash_valid = true;
        }

    kitty_present_log:
        (void)0;
    }

    if (KITTY_DebugEnabled()) {
        static int frames;
        const float cov = KITTY_CoverageRatio(surface->w, surface->h, &dirty);
        /* Log startup, failures, full presents, high-coverage sparse, skips, or every 60th. */
        const bool log_it = frames < 8 || !ok || wire_mode == KITTY_WIRE_FULL ||
                            wire_mode == KITTY_WIRE_UPLOAD_SKIP ||
                            (wire_mode == KITTY_WIRE_DIRTY_BW && cov >= 0.5f) ||
                            (frames % 60) == 0;
        if (log_it) {
            const char *mode_str = "full";
            if (wire_mode == KITTY_WIRE_DIRTY_BW) {
                mode_str = "dirty_bw";
            } else if (wire_mode == KITTY_WIRE_DIRTY_AT) {
                mode_str = (vdata->term_profile && vdata->term_profile->dirty_union_at &&
                            !(vdata->term_profile->dirty_at_overlays))
                               ? "dirty_union"
                               : "dirty_at";
            } else if (wire_mode == KITTY_WIRE_DIRTY_CPU) {
                mode_str = "dirty_cpu";
            } else if (wire_mode == KITTY_WIRE_DIFF_SKIP) {
                mode_str = "diff_skip";
            } else if (wire_mode == KITTY_WIRE_UPLOAD_SKIP) {
                mode_str = "upload_skip";
            }
            KITTY_DebugLog(
                "kitty: present #%d %dx%d place=%d mode=%s rects=%d union=%dx%d coverage=%.2f "
                "convert_px=%zu upload_px=%zu wire=%zu raw=%zu ms_tx=%.2f ms_ack=%.2f "
                "live=%u pingpong=%d -> %s%s",
                frames, surface->w, surface->h,
                vdata->place_cells ? 1 : 0,
                mode_str,
                numrects, dirty.w, dirty.h, (double)cov,
                convert_px, upload_px,
                ok ? vdata->last_wire_bytes : (size_t)0,
                ok ? vdata->last_raw_bytes : (size_t)0,
                ok ? vdata->last_ms_tx : 0.0,
                ok ? vdata->last_ms_ack : -1.0,
                (unsigned)wdata->live_image_id,
                KITTY_SelectPingPong(SDL_getenv("SDL_KITTY_PINGPONG"),
                                     vdata->term_profile && vdata->term_profile->pingpong)
                    ? 1
                    : 0,
                ok ? "ok" : "FAIL ",
                ok ? "" : SDL_GetError());
            if (ok && (frames % 60) == 0 && vdata->wire_presents > 0) {
                const double avg_tx = vdata->total_ms_tx / (double)vdata->wire_presents;
                const double avg_ack =
                    vdata->ack_presents
                        ? (vdata->total_ms_ack / (double)vdata->ack_presents)
                        : -1.0;
                KITTY_DebugLog(
                    "kitty: perf_summary presents=%u wire_total=%llu avg_wire=%llu "
                    "avg_ms_tx=%.2f avg_ms_ack=%.2f ack_n=%u skips=%u",
                    (unsigned)vdata->wire_presents,
                    (unsigned long long)vdata->total_wire_bytes,
                    (unsigned long long)(vdata->total_wire_bytes / vdata->wire_presents),
                    avg_tx, avg_ack, (unsigned)vdata->ack_presents,
                    (unsigned)vdata->skip_presents);
            }
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
