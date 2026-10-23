/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#include "SDL_kittyprofile.h"

/* ~1920x1080 RGBA — refuse larger direct APC dumps into the tty. */
#define KITTY_MAX_DIRECT_BYTES_DEFAULT ((size_t)1920 * 1080 * 4)
#define KITTY_AF_MAX_PX_DEFAULT ((size_t)160 * 1024)

static const KITTY_TerminalProfile KITTY_profile_rex = {
    KITTY_TERM_REX,
    "rex",
    true,  /* prefer_direct — SHM/zlib historically flaky */
    1280,
    720,
    KITTY_MAX_DIRECT_BYTES_DEFAULT,
    /* Keep big fireworks bursts on in-place a=f; full a=T replaces flash on Rex.
     * Kitty/Ghostty keep the stricter 0.40 / 160k gate (base64 flood risk). */
    0.95f,
    (size_t)1280 * 720,
    true,  /* keys_on_stdin */
    true   /* silent_af_ok */
};

static const KITTY_TerminalProfile KITTY_profile_kitty = {
    KITTY_TERM_KITTY,
    "kitty",
    false, /* prefer SHM when ok */
    1920,
    1080,
    KITTY_MAX_DIRECT_BYTES_DEFAULT,
    0.40f,
    KITTY_AF_MAX_PX_DEFAULT,
    true,
    false
};

static const KITTY_TerminalProfile KITTY_profile_ghostty = {
    KITTY_TERM_GHOSTTY,
    "ghostty",
    false,
    1920,
    1080,
    KITTY_MAX_DIRECT_BYTES_DEFAULT,
    0.40f,
    KITTY_AF_MAX_PX_DEFAULT,
    true,
    false
};

static const KITTY_TerminalProfile KITTY_profile_wezterm = {
    KITTY_TERM_WEZTERM,
    "wezterm",
    false,
    1280, /* conservative until soaked */
    720,
    KITTY_MAX_DIRECT_BYTES_DEFAULT,
    0.40f,
    KITTY_AF_MAX_PX_DEFAULT,
    true,
    false
};

static const KITTY_TerminalProfile KITTY_profile_unknown = {
    KITTY_TERM_UNKNOWN,
    "unknown",
    false,
    1280,
    720,
    KITTY_MAX_DIRECT_BYTES_DEFAULT,
    0.40f,
    KITTY_AF_MAX_PX_DEFAULT,
    true,
    true /* silent a=f often means support */
};

const KITTY_TerminalProfile *KITTY_DetectTerminalProfile(void)
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

#endif /* SDL_VIDEO_DRIVER_KITTY */
