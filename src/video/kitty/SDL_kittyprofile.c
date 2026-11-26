/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>
*/

/*
 * The seed every profile starts from: the driver's own defaults, before
 * sdl-kitty.toml's [defaults] and per-terminal tuning, and before facts from
 * terminals.toml. No per-terminal values live here; those are in the TOML
 * files (SDL_kittyprofile_load.c).
 */
#ifdef KITTY_PROFILE_STANDALONE
#include <SDL3/SDL.h>
#include <string.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROFILE_STANDALONE)

#include "SDL_kittyprofile.h"

static const KITTY_TerminalProfile KITTY_profile_seed = {
    .kind = KITTY_TERM_UNKNOWN,
    .name = "unknown",
    .rows = "",
    .version = "",
    .recognised_by = "",
    .facts = {
        .placeholders = KITTY_FACT_UNKNOWN,
        .frame_edit = KITTY_FACT_UNKNOWN,
        .frame_edit_reply = KITTY_FACT_UNKNOWN,
        .frame_edit_zlib = KITTY_FACT_UNKNOWN,
        .animation = KITTY_FACT_UNKNOWN,
        .shm = KITTY_FACT_UNKNOWN,
        .iterm2_gif_animation = KITTY_FACT_UNKNOWN,
        .ioctl_pixels = KITTY_FACT_UNKNOWN,
    },
    .prefer_direct = false,
    .prefer_zlib = false,
    .force_transmit = NULL,
    .default_fb_max_w = 1280,
    .default_fb_max_h = 720,
    .default_fb_max_shm_w = 1280,
    .default_fb_max_shm_h = 720,
    .default_fb_max_direct_w = 1280,
    .default_fb_max_direct_h = 720,
    .max_direct_bytes = (size_t)1920 * 1080 * 4, /* refuse larger direct dumps */
    .af_max_coverage = 0.40f,
    .af_max_pixels = (size_t)160 * 1024,
    .af_zlib = false,
    .keys_on_stdin = true,
    .silent_af_ok = true, /* unknown terminal: silent a=f often means support */
    .skip_af_probe = false,
    .ignore_ioctl_pixels = false,
    .direct_af_when_shm = false,
    .dirty_at_overlays = false,
    .dirty_union_at = false,
    .upload_skip = true,
    .coalesce_gap_px = 0,
    .direct_fallback_max_bytes = (size_t)256 * 1024,
    .apc_chunk_bytes = (size_t)2 * 1024 * 1024,
    .probe_timeout_ms = 40,
    .drain_passes = 8,
    .drain_quiet_us = 15000,
    .stub_winch_iters = 40,
    .stub_winch_delay_ms = 25,
    .stub_cols = 80,
    .stub_rows = 24,
    .key_hold_ms = 80,
    .fb_diff = true,
    .fb_diff_tile = 32,
    .fb_diff_cover = 0.45f,
    .tty_pending_max = 4096,
    .window_fallback_w = 640,
    .window_fallback_h = 480,
    .place_cells = true,
    .pingpong = false,
    .shm_unlink_after_send = false,
    .notes = NULL
};

/*
 * The seed profile (SDL_kittyprofile_load.c copies it before overlaying).
 */
const KITTY_TerminalProfile *KITTY_BuiltinDefaultsProfile(void)
{
    return &KITTY_profile_seed;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROFILE_STANDALONE */
