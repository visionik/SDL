/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
*/
#include "SDL_internal.h"

#ifndef SDL_kittyprofile_h_
#define SDL_kittyprofile_h_

#ifdef SDL_VIDEO_DRIVER_KITTY

#include <stddef.h>

typedef enum KITTY_TerminalKind
{
    KITTY_TERM_UNKNOWN = 0,
    KITTY_TERM_REX,
    KITTY_TERM_KITTY,
    KITTY_TERM_GHOSTTY,
    KITTY_TERM_WEZTERM
} KITTY_TerminalKind;

/*
 * Static per-host policy. Runtime probes (a=q / SHM / a=f) still decide what
 * is *available*; this struct decides defaults and quirk thresholds.
 * See docs/terminals.md.
 */
typedef struct KITTY_TerminalProfile
{
    KITTY_TerminalKind kind;
    const char *name;

    /* When both SHM and direct probe OK, prefer direct (Rex). */
    bool prefer_direct;

    /* Default aspect-fit FB cap when SDL_KITTY_FB_MAX unset. 0 = no default cap. */
    int default_fb_max_w;
    int default_fb_max_h;

    /* Refuse direct a=T / fallback above this many raw bytes. */
    size_t max_direct_bytes;

    /* Skip a=f (use full a=T) when dirty coverage or pixel count exceeds. */
    float af_max_coverage;
    size_t af_max_pixels;

    /* Read keyboard from stdin (Rex delivers keys on fd0 only). */
    bool keys_on_stdin;

    /* Treat silent a=f probe as success (Rex). */
    bool silent_af_ok;
} KITTY_TerminalProfile;

/* Detect from TERM_PROGRAM / TERM. Never returns NULL. */
const KITTY_TerminalProfile *KITTY_DetectTerminalProfile(void);

#endif /* SDL_VIDEO_DRIVER_KITTY */
#endif /* SDL_kittyprofile_h_ */
