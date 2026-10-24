/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
*/
#ifdef KITTY_PROFILE_STANDALONE
#include <SDL3/SDL.h>
#include <string.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROFILE_STANDALONE)

#include "SDL_kittyprofile.h"

/* ~1920x1080 RGBA — refuse larger direct APC dumps into the tty. */
#define KITTY_MAX_DIRECT_BYTES_DEFAULT ((size_t)1920 * 1080 * 4)
#define KITTY_AF_MAX_PX_DEFAULT ((size_t)160 * 1024)
#define KITTY_DIRECT_FALLBACK_DEFAULT ((size_t)256 * 1024)
#define KITTY_APC_CHUNK_DEFAULT ((size_t)2 * 1024 * 1024)

/* Shared §C tunables matching config/terminals.toml [defaults]. */
#define KITTY_PROFILE_TUNABLES                                                 \
    .direct_fallback_max_bytes = KITTY_DIRECT_FALLBACK_DEFAULT,                \
    .apc_chunk_bytes = KITTY_APC_CHUNK_DEFAULT,                                \
    .probe_timeout_ms = 40,                                                    \
    .drain_passes = 8,                                                         \
    .drain_quiet_us = 15000,                                                   \
    .stub_winch_iters = 40,                                                    \
    .stub_winch_delay_ms = 25,                                                 \
    .stub_cols = 80,                                                           \
    .stub_rows = 24,                                                           \
    .key_hold_ms = 80,                                                         \
    .fb_diff = true,                                                           \
    .fb_diff_tile = 32,                                                        \
    .fb_diff_cover = 0.45f,                                                    \
    .tty_pending_max = 4096,                                                   \
    .window_fallback_w = 640,                                                  \
    .window_fallback_h = 480,                                                  \
    .place_cells = true,                                                       \
    .pingpong = false,                                                         \
    .shm_unlink_after_send = false,                                            \
    .notes = NULL

static const KITTY_TerminalProfile KITTY_profile_rex = {
    .kind = KITTY_TERM_REX,
    .name = "rex",
    .prefer_direct = true, /* SHM/zlib historically flaky / unsupported medium */
    .prefer_zlib = false,
    .force_transmit = NULL,
    .default_fb_max_w = 1280,
    .default_fb_max_h = 720,
    .max_direct_bytes = KITTY_MAX_DIRECT_BYTES_DEFAULT,
    .af_max_coverage = 0.95f,
    .af_max_pixels = (size_t)1280 * 720,
    .keys_on_stdin = true,
    .silent_af_ok = true,
    .ignore_ioctl_pixels = true,
    .direct_af_when_shm = true,
    KITTY_PROFILE_TUNABLES
};

static const KITTY_TerminalProfile KITTY_profile_kitty = {
    .kind = KITTY_TERM_KITTY,
    .name = "kitty",
    .prefer_direct = false, /* SHM first-class in graphics.c */
    .prefer_zlib = false,
    .force_transmit = NULL,
    .default_fb_max_w = 1920,
    .default_fb_max_h = 1080,
    .max_direct_bytes = KITTY_MAX_DIRECT_BYTES_DEFAULT,
    .af_max_coverage = 0.40f,
    .af_max_pixels = KITTY_AF_MAX_PX_DEFAULT,
    .keys_on_stdin = true,
    .silent_af_ok = false, /* Kitty replies Gi=…[,r=N];OK */
    .ignore_ioctl_pixels = false,
    .direct_af_when_shm = true, /* a=f t=d edits root of SHM-placed image */
    /* Override tunables: Kitty ignores APC > MAX_ESCAPE_CODE_LENGTH (256KiB). */
    .shm_unlink_after_send = false,
    .place_cells = true,
    .pingpong = false,
    .fb_diff = true,
    .apc_chunk_bytes = 261888, /* 262144 - header/ST headroom */
    .tty_pending_max = 4096,
};

static const KITTY_TerminalProfile KITTY_profile_ghostty = {
    .kind = KITTY_TERM_GHOSTTY,
    .name = "ghostty",
    .prefer_direct = false,
    .prefer_zlib = false,
    .force_transmit = NULL,
    .default_fb_max_w = 1920,
    .default_fb_max_h = 1080,
    .max_direct_bytes = KITTY_MAX_DIRECT_BYTES_DEFAULT,
    .af_max_coverage = 0.40f,
    .af_max_pixels = KITTY_AF_MAX_PX_DEFAULT,
    .keys_on_stdin = true,
    /* 1.3.1: a=f unimplemented ("ERROR: unimplemented action"); tip #13943 adds it. */
    .silent_af_ok = false,
    .ignore_ioctl_pixels = false,
    .direct_af_when_shm = false,
    KITTY_PROFILE_TUNABLES
};

static const KITTY_TerminalProfile KITTY_profile_wezterm = {
    .kind = KITTY_TERM_WEZTERM,
    .name = "wezterm",
    /* macOS SHM t=s broken (seek/read ENXIO) until wezterm#8063. */
    .prefer_direct = true,
    .prefer_zlib = true,
    .force_transmit = NULL,
    .default_fb_max_w = 960,
    .default_fb_max_h = 540,
    .max_direct_bytes = KITTY_MAX_DIRECT_BYTES_DEFAULT,
    .af_max_coverage = 0.40f,
    .af_max_pixels = KITTY_AF_MAX_PX_DEFAULT,
    .keys_on_stdin = true,
    /* a=f edits pixels but ImageData.hash is creation-time only → GPU stale. */
    .silent_af_ok = false,
    .ignore_ioctl_pixels = false,
    .direct_af_when_shm = false,
    /* dirty_at crashes WezTerm 20240203 (assign_image_to_cells overflow). */
    .dirty_at_overlays = false,
    KITTY_PROFILE_TUNABLES
};

static const KITTY_TerminalProfile KITTY_profile_unknown = {
    .kind = KITTY_TERM_UNKNOWN,
    .name = "unknown",
    .prefer_direct = false,
    .prefer_zlib = false,
    .force_transmit = NULL,
    .default_fb_max_w = 1280,
    .default_fb_max_h = 720,
    .max_direct_bytes = KITTY_MAX_DIRECT_BYTES_DEFAULT,
    .af_max_coverage = 0.40f,
    .af_max_pixels = KITTY_AF_MAX_PX_DEFAULT,
    .keys_on_stdin = true,
    .silent_af_ok = true, /* silent a=f often means support */
    .ignore_ioctl_pixels = false,
    .direct_af_when_shm = false,
    KITTY_PROFILE_TUNABLES
};

/* Filled by SDL_kittyprofile_load.c when a TOML file loads. */
extern const KITTY_TerminalProfile *KITTY_DetectLoadedTerminalProfile(void);
extern void KITTY_EnsureTerminalProfilesLoaded(void);
extern const char *KITTY_TerminalProfilesSource(void);

static const KITTY_TerminalProfile *KITTY_DetectBuiltinTerminalProfile(void)
{
    const char *tp = SDL_getenv("TERM_PROGRAM");
    const char *term = SDL_getenv("TERM");

    if (tp && *tp) {
        if (SDL_strcasecmp(tp, "rex") == 0) {
            return &KITTY_profile_rex;
        }
        if (SDL_strcasecmp(tp, "kitty") == 0) {
            return &KITTY_profile_kitty;
        }
        if (SDL_strcasecmp(tp, "ghostty") == 0) {
            return &KITTY_profile_ghostty;
        }
        if (SDL_strcasecmp(tp, "WezTerm") == 0 || SDL_strcasecmp(tp, "wezterm") == 0) {
            return &KITTY_profile_wezterm;
        }
    }

    /* Kitty.app often leaves TERM_PROGRAM unset but sets TERM=xterm-kitty. */
    if ((!tp || !*tp) && term && SDL_strcmp(term, "xterm-kitty") == 0) {
        return &KITTY_profile_kitty;
    }

    return &KITTY_profile_unknown;
}

const KITTY_TerminalProfile *KITTY_DetectTerminalProfile(void)
{
    const KITTY_TerminalProfile *loaded;

    KITTY_EnsureTerminalProfilesLoaded();
    loaded = KITTY_DetectLoadedTerminalProfile();
    if (loaded) {
        return loaded;
    }
    return KITTY_DetectBuiltinTerminalProfile();
}

/* Builtin [defaults]-shaped seed for the TOML loader. */
const KITTY_TerminalProfile *KITTY_BuiltinDefaultsProfile(void)
{
    return &KITTY_profile_unknown;
}

bool KITTY_BuiltinEnvLooksLikeKittyHost(void)
{
    const char *term_program = SDL_getenv("TERM_PROGRAM");
    const char *term;

    if (term_program && *term_program) {
        if (SDL_strcasecmp(term_program, "kitty") == 0 ||
            SDL_strcasecmp(term_program, "ghostty") == 0 ||
            SDL_strcasecmp(term_program, "WezTerm") == 0 ||
            SDL_strcasecmp(term_program, "wezterm") == 0 ||
            SDL_strcasecmp(term_program, "rex") == 0) {
            return true;
        }
    }
    if (SDL_getenv("KITTY_WINDOW_ID") != NULL) {
        return true;
    }
    term = SDL_getenv("TERM");
    if (term && (SDL_strstr(term, "kitty") || SDL_strstr(term, "ghostty"))) {
        return true;
    }
    return false;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROFILE_STANDALONE */
