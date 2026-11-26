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
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

/*
 * SDL video device bootstrap for SDL_VIDEO_DRIVER=kitty: create the device,
 * open the tty, load terminal profiles, and wire window/FB/event hooks.
 */

#include "../../events/SDL_events_c.h"
#include "../SDL_pixels_c.h"
#include "../SDL_sysvideo.h"

#include "SDL_kittyevents_c.h"
#include "SDL_kittyframebuffer_c.h"
#include "SDL_kittygraphics.h"
#include "SDL_kittyoverlay.h"
#include "SDL_kittyprobe.h"
#include "SDL_kittyprofile.h"
#include "SDL_kittyrect.h"
#include "SDL_kittyvideo.h"
#include "SDL_kittywindow.h"

#include <signal.h>
#include <unistd.h>

/*
 * Enter raw mode, size the pane, probe APC transmit modes, optionally enable
 * mouse, and register the SDL display from CSI/tty metrics.
 */
static bool KITTY_VideoInit(SDL_VideoDevice *_this);

/*
 * Restore the previous SIGWINCH handler and leave owned terminal modes
 * (keyboard/mouse/APC state) before the video device is torn down.
 */
static void KITTY_VideoQuit(SDL_VideoDevice *_this);

static SDL_VideoData * volatile KITTY_winch_data = NULL;
static void (*KITTY_prev_winch)(int) = SIG_DFL;

static volatile sig_atomic_t KITTY_term_sig_installed = 0;
static volatile sig_atomic_t KITTY_term_tty_in = -1;
static volatile sig_atomic_t KITTY_term_tty_out = -1;
static volatile sig_atomic_t KITTY_term_alt_screen = 1;
static volatile sig_atomic_t KITTY_term_leave_images = 0;
static struct sigaction KITTY_prev_sigint;
static struct sigaction KITTY_prev_sigterm;
static bool KITTY_have_prev_sigint = false;
static bool KITTY_have_prev_sigterm = false;

/*
 * SIGWINCH handler: mark the active SDL_VideoData so PumpEvents/VideoInit can
 * re-query tty size. Does not write the tty or post SDL events itself.
 */
static void KITTY_HandleSIGWINCH(int sig)
{
    (void)sig;
    if (KITTY_winch_data) {
        KITTY_winch_data->got_winch = 1;
    }
}

/*
 * Async-signal-safe SIGINT/SIGTERM cleanup so Ctrl+C or kill never leaves
 * the user's shell stuck in raw mode / alt-screen / SGR mouse tracking.
 */
static void KITTY_HandleTermSignal(int sig)
{
    struct sigaction dfl;
    if (KITTY_term_sig_installed) {
        KITTY_term_sig_installed = 0;
        KITTY_EmergencyRestore((int)KITTY_term_tty_in, (int)KITTY_term_tty_out,
                               KITTY_term_alt_screen != 0, KITTY_term_leave_images != 0);
    }
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void KITTY_InstallTermSignals(const SDL_VideoData *data)
{
    struct sigaction sa, old;
    KITTY_term_tty_in = data->tty_in;
    KITTY_term_tty_out = data->tty_out;
    KITTY_term_alt_screen = data->use_alt_screen ? 1 : 0;
    KITTY_term_leave_images = data->leave_images ? 1 : 0;
    KITTY_term_sig_installed = 1;

    SDL_zero(sa);
    sa.sa_handler = KITTY_HandleTermSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    KITTY_have_prev_sigint = false;
    if (sigaction(SIGINT, NULL, &old) == 0 && old.sa_handler != SIG_IGN) {
        if (sigaction(SIGINT, &sa, &KITTY_prev_sigint) == 0) {
            KITTY_have_prev_sigint = true;
        }
    }
    KITTY_have_prev_sigterm = false;
    if (sigaction(SIGTERM, NULL, &old) == 0 && old.sa_handler != SIG_IGN) {
        if (sigaction(SIGTERM, &sa, &KITTY_prev_sigterm) == 0) {
            KITTY_have_prev_sigterm = true;
        }
    }
}

static void KITTY_UninstallTermSignals(void)
{
    KITTY_term_sig_installed = 0;
    if (KITTY_have_prev_sigint) {
        sigaction(SIGINT, &KITTY_prev_sigint, NULL);
        KITTY_have_prev_sigint = false;
    }
    if (KITTY_have_prev_sigterm) {
        sigaction(SIGTERM, &KITTY_prev_sigterm, NULL);
        KITTY_have_prev_sigterm = false;
    }
}

/*
 * True only when SDL_HINT_VIDEO_DRIVER names "kitty" and KITTY_ProbeSupport
 * succeeds. Never auto-selects this driver.
 */
static bool KITTY_Available(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (!(hint && SDL_strstr(hint, KITTYVID_DRIVER_NAME) != NULL)) {
        // Never auto-select; require SDL_VIDEO_DRIVER=kitty
        return false;
    }
    return KITTY_ProbeSupport();
}

/*
 * Free the SDL_VideoData and the SDL_VideoDevice allocated by KITTY_CreateDevice.
 */
static void KITTY_DeleteDevice(SDL_VideoDevice *device)
{
    if (device->internal) {
        SDL_free(device->internal);
        device->internal = NULL;
    }
    SDL_free(device);
}

/*
 * Allocate SDL_VideoDevice + SDL_VideoData, detect the terminal profile, bind
 * tty in/out fds, set initial transmit/place defaults, and hook window, FB,
 * and event entry points. Returns NULL if the driver is unavailable.
 */
static SDL_VideoDevice *KITTY_CreateDevice(void)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;

    if (!KITTY_Available()) {
        return NULL;
    }

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    data = (SDL_VideoData *)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!data) {
        SDL_free(device);
        return NULL;
    }
    data->term_profile = KITTY_DetectTerminalProfile();
    /* Writes always go to stdout. Reads: profile.keys_on_stdin (Rex) when
     * stdin is a tty; else stdout. */
    data->tty_out = STDOUT_FILENO;
    if (data->term_profile->keys_on_stdin && isatty(STDIN_FILENO)) {
        data->tty_in = STDIN_FILENO;
    } else {
        data->tty_in = isatty(STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;
    }
    data->tty_fd = data->tty_out;
    /* Initial guess; ProbeTransmitModes + profile refine after terminal setup. */
    data->transmit_mode = data->term_profile->prefer_direct ? KITTY_TRANSMIT_DIRECT
                                                           : KITTY_TRANSMIT_SHM;
    data->shm_failed = false;
    data->frame_edit_ok = false;
    data->kitty_keyboard_ok = false;
    /* Stretch FB to the cell grid by default (profile.place_cells); env overrides. */
    {
        const char *place = SDL_getenv("SDL_KITTY_PLACE_CELLS");
        const bool profile_place =
            !data->term_profile || data->term_profile->place_cells;
        if (!place || !place[0]) {
            data->place_cells = profile_place;
        } else if (place[0] == '0' || SDL_strcasecmp(place, "false") == 0 ||
                   SDL_strcasecmp(place, "no") == 0 || SDL_strcasecmp(place, "off") == 0) {
            data->place_cells = false;
        } else {
            data->place_cells = true;
        }
    }
    KITTY_ApplyViewportEnv(data);
    {
        const char *settle = SDL_getenv("SDL_KITTY_RESIZE_SETTLE_MS");
        const long ms = (settle && *settle) ? SDL_strtol(settle, NULL, 10) : 0;
        data->resize_settle_ms = ms > 0 ? (Uint32)ms : 0;
    }
    device->internal = data;

    device->VideoInit = KITTY_VideoInit;
    device->VideoQuit = KITTY_VideoQuit;
    device->PumpEvents = KITTY_PumpEvents;
    device->CreateWindowFramebuffer = SDL_KITTY_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SDL_KITTY_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_KITTY_DestroyWindowFramebuffer;
    device->CreateSDLWindow = KITTY_CreateWindow;
    device->DestroyWindow = KITTY_DestroyWindow;
    device->ShowWindow = KITTY_ShowWindow;
    device->HideWindow = KITTY_HideWindow;
    device->SetWindowSize = KITTY_SetWindowSize;
    device->free = KITTY_DeleteDevice;

    return device;
}

/*
 * SDL VideoBootStrap entry for the kitty driver; CreateDevice is the factory.
 */
VideoBootStrap KITTY_bootstrap = {
    KITTYVID_DRIVER_NAME, "SDL Kitty graphics protocol video driver",
    KITTY_CreateDevice,
    NULL,
    false
};

/*
 * Enter raw mode, size the pane (with stub-WINCH wait), probe APC transmit
 * modes, optionally enable mouse, and register the SDL display from CSI/tty
 * metrics. On failure restores the terminal and clears terminal_owned.
 */
static bool KITTY_VideoInit(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    SDL_DisplayMode mode;
    struct sigaction sa, old_sa;

    if (!KITTY_EnterRawMode(data)) {
        return false;
    }
    /* Before anything reads the profile (keyboard, sizes, probes). */
    KITTY_RecogniseTerminal(data);
    if (!KITTY_SetupTerminal(data)) {
        KITTY_LeaveRawMode(data);
        return false;
    }
    data->terminal_owned = true;
    data->keyboard_enabled = true;
    KITTY_InstallTermSignals(data);

    /* Install WINCH before sizing waits so Rex pane layout is observed. */
    KITTY_winch_data = data;
    data->got_winch = 0;
    SDL_zero(sa);
    sa.sa_handler = KITTY_HandleSIGWINCH;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGWINCH, &sa, &old_sa) == 0) {
        KITTY_prev_winch = old_sa.sa_handler;
    }

    /* Size before mouse: any-event mode floods the tty and can starve CSI 14t. */
    if (!KITTY_QueryTerminalSize(data)) {
        KITTY_UninstallTermSignals();
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
        return false;
    }

    /* Some hosts spawn at a stub grid then deliver SIGWINCH once the pane is
     * laid out. Wait briefly for a non-stub size before enabling mouse / display. */
    {
        const int stub_iters =
            (data->term_profile && data->term_profile->stub_winch_iters > 0)
                ? data->term_profile->stub_winch_iters
                : 40;
        const int stub_delay =
            (data->term_profile && data->term_profile->stub_winch_delay_ms > 0)
                ? data->term_profile->stub_winch_delay_ms
                : 25;
        const int stub_cols =
            (data->term_profile && data->term_profile->stub_cols > 0)
                ? data->term_profile->stub_cols
                : 80;
        const int stub_rows =
            (data->term_profile && data->term_profile->stub_rows > 0)
                ? data->term_profile->stub_rows
                : 24;
        /* Card viewports taller than the stub grid need a real WINCH before
         * the first paint; wait for rows to fit vp_row_req+vp_rows_req. */
        const int need_rows =
            (data->vp_rows_req > 0 && data->vp_row_req > 0)
                ? (data->vp_row_req + data->vp_rows_req)
                : 0;
        for (int i = 0; i < stub_iters; ++i) {
            const bool still_stub =
                (data->cols == stub_cols && data->rows == stub_rows);
            const bool vp_fits =
                (need_rows <= 0) || (data->rows >= need_rows);
            if (!still_stub && vp_fits) {
                break;
            }
            SDL_Delay(stub_delay);
            if (data->got_winch) {
                data->got_winch = 0;
                (void)KITTY_QueryTerminalSize(data);
                continue;
            }
            if ((i % 4) == 3) {
                (void)KITTY_QueryTerminalSize(data);
            }
        }
    }

    /* Card chrome after size is known (and again on WINCH replace). */
    if (!data->use_alt_screen) {
        (void)KITTY_DrawViewportChrome(data);
    }

    /* a=q probe: pick SHM vs direct (Rex stays direct unless forced). */
    (void)KITTY_ProbeTransmitModes(data);

    /* Cap used profile preference before probe; re-fit now that transmit_mode
     * is known (shm vs direct soft-maxes differ on Kitty/Ghostty). */
    KITTY_ApplyFramebufferCap(data);

    /* Resolve hot-path env + profile policy once at init so UpdateWindowFramebuffer
     * and TransmitRGB never call SDL_getenv()/strcasecmp()/atof() per frame. */
    {
        const KITTY_TerminalProfile *prof = data->term_profile;
        const char *env;

        env = SDL_getenv("SDL_KITTY_DIRTY");
        data->cfg_dirty_enabled = !(env && (env[0] == '0' || SDL_strcasecmp(env, "off") == 0 ||
                                            SDL_strcasecmp(env, "false") == 0 ||
                                            SDL_strcasecmp(env, "no") == 0));
        data->cfg_fb_diff = KITTY_SelectFbDiff(SDL_getenv("SDL_KITTY_FB_DIFF"),
                                               prof ? prof->fb_diff : true);
        data->cfg_fb_diff_tile = KITTY_SelectFbDiffTile(SDL_getenv("SDL_KITTY_FB_DIFF_TILE"),
                                                        prof ? prof->fb_diff_tile : 32);
        data->cfg_fb_diff_cover = KITTY_SelectFbDiffCover(SDL_getenv("SDL_KITTY_FB_DIFF_COVER"),
                                                          prof ? prof->fb_diff_cover : 0.45f);
        data->cfg_upload_skip = KITTY_SelectUploadSkip(SDL_getenv("SDL_KITTY_UPLOAD_SKIP"),
                                                       prof ? prof->upload_skip : true);
        data->cfg_af_max_coverage = (prof && prof->af_max_coverage > 0.0f) ? prof->af_max_coverage : 0.40f;
        env = SDL_getenv("SDL_KITTY_AF_MAX_COVERAGE");
        if (env && env[0]) {
            data->cfg_af_max_coverage = (float)SDL_atof(env);
        }
        data->cfg_af_max_pixels = (prof && prof->af_max_pixels > 0)
                                      ? prof->af_max_pixels
                                      : ((size_t)160 * 1024);
        data->cfg_dirty_bw = KITTY_SelectDirtyBW(SDL_getenv("SDL_KITTY_DIRTY_BW"), data->frame_edit_ok);
        data->cfg_coalesce_gap = KITTY_SelectCoalesceGap(SDL_getenv("SDL_KITTY_COALESCE_GAP"),
                                                         prof ? prof->coalesce_gap_px : 0);
        data->cfg_dirty_union_at = KITTY_SelectDirtyUnionAT(SDL_getenv("SDL_KITTY_DIRTY_UNION_AT"),
                                                            prof && prof->dirty_union_at);
        data->cfg_dirty_at = KITTY_SelectDirtyAT(SDL_getenv("SDL_KITTY_DIRTY_AT"),
                                                 prof && prof->dirty_at_overlays);
        data->cfg_pingpong = KITTY_SelectPingPong(SDL_getenv("SDL_KITTY_PINGPONG"),
                                                  prof && prof->pingpong);
        env = SDL_getenv("SDL_KITTY_AF_ZLIB");
        if (env && (env[0] == '0' || env[0] == '1') && env[1] == '\0') {
            data->cfg_af_zlib = (env[0] == '1');
        } else {
            data->cfg_af_zlib = prof && prof->af_zlib;
        }
        env = SDL_getenv("SDL_KITTY_ZLIB_LEVEL");
        if (env && env[0]) {
            const long lv = SDL_strtol(env, NULL, 10);
            if (lv >= 1 && lv <= 9) {
                data->cfg_zlib_level = (int)lv;
            }
        } else if (SDL_getenv("SSH_CONNECTION") != NULL) {
            /* Over SSH/remote links, level 4 shrinks full-frame wire up to ~2x
             * for ~350 us extra parallel CPU, substantially improving FPS. */
            data->cfg_zlib_level = 4;
        }
        env = SDL_getenv("SDL_KITTY_TRANSMIT");
        if (env && env[0]) {
            if (SDL_strcasecmp(env, "direct") == 0) {
                data->transmit_mode = KITTY_TRANSMIT_DIRECT;
            } else if (SDL_strcasecmp(env, "zlib") == 0) {
                data->transmit_mode = KITTY_TRANSMIT_DIRECT_ZLIB;
            } else if (SDL_strcasecmp(env, "shm") == 0) {
                data->transmit_mode = KITTY_TRANSMIT_SHM;
            }
        }
        env = SDL_getenv("SDL_KITTY_PERF_ACK");
        data->cfg_perf_ack = (env && (env[0] == '1' || SDL_strcasecmp(env, "true") == 0 ||
                                      SDL_strcasecmp(env, "yes") == 0 ||
                                      SDL_strcasecmp(env, "on") == 0));
        data->cfg_max_direct_bytes = (prof && prof->max_direct_bytes > 0)
                                         ? prof->max_direct_bytes
                                         : ((size_t)1920 * 1080 * 4);
        data->cfg_apc_chunk_bytes = KITTY_ProfileApcChunkBytes(prof);
        data->overlay = KITTY_OverlayCreate(SDL_getenv("SDL_KITTY_OVERLAY"));
        data->cfg_cached = true;
    }

    {
        const char *no_mouse = SDL_getenv("SDL_KITTY_MOUSE");
        const bool want_mouse = !(no_mouse && (no_mouse[0] == '0' ||
                                               SDL_strcasecmp(no_mouse, "false") == 0 ||
                                               SDL_strcasecmp(no_mouse, "off") == 0 ||
                                               SDL_strcasecmp(no_mouse, "no") == 0));
        if (want_mouse) {
            if (!KITTY_EnableMouse(data)) {
                KITTY_UninstallTermSignals();
                KITTY_RestoreTerminal(data);
                data->terminal_owned = false;
                return false;
            }
        } else {
            data->mouse_enabled = false;
            data->mouse_sgr_pixels = false;
        }
    }

    SDL_zero(mode);
    mode.format = KITTY_SelectFbAlpha(SDL_getenv("SDL_KITTY_FB_ALPHA"))
                      ? SDL_PIXELFORMAT_ARGB8888
                      : SDL_PIXELFORMAT_XRGB8888;
    mode.w = data->pixel_w;
    mode.h = data->pixel_h;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        KITTY_UninstallTermSignals();
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
        return false;
    }

#ifdef SDL_KITTY_RUNTIME_DEBUG
    if (KITTY_DebugEnabled()) {
        KITTY_DebugLog(
            "kitty: init term=%s profiles=%s prefer_direct=%d cols=%d rows=%d fb=%dx%d cell=%dx%d place=%d mouse1016=%d",
            data->term_profile ? data->term_profile->name : "?",
            KITTY_TerminalProfilesSource(),
            data->term_profile && data->term_profile->prefer_direct ? 1 : 0,
            data->cols, data->rows, data->pixel_w, data->pixel_h,
            data->cell_w, data->cell_h, data->place_cells ? 1 : 0,
            data->mouse_sgr_pixels ? 1 : 0);
    }
#endif

    return true;
}

/*
 * Restore the previous SIGWINCH handler and leave owned terminal modes
 * (keyboard/mouse/APC state) before the video device is torn down.
 */
static void KITTY_VideoQuit(SDL_VideoDevice *_this)
{
    SDL_VideoData *data = (SDL_VideoData *)_this->internal;
    struct sigaction sa;

    if (KITTY_winch_data == data) {
        SDL_zero(sa);
        sa.sa_handler = KITTY_prev_winch;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, NULL);
        KITTY_winch_data = NULL;
        KITTY_prev_winch = SIG_DFL;
    }

    KITTY_UninstallTermSignals();

    if (data && data->terminal_owned) {
        KITTY_RestoreTerminal(data);
        data->terminal_owned = false;
    }
    if (data) {
        KITTY_OverlayDestroy((KITTY_ToastOverlay *)data->overlay);
        data->overlay = NULL;
        KITTY_FreeTransmitBuffers(data);
    }
}

#endif // SDL_VIDEO_DRIVER_KITTY
