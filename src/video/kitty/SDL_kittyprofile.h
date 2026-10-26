/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>
*/

/*
 * Per-host terminal profiles for the Kitty video driver: KITTY_TerminalProfile
 * policy/tunables, TOML load/reset API, env-based detect helpers, and small
 * inline accessors for APC chunk / direct-fallback / probe-timeout defaults.
 * Canonical data: config/terminals.toml (see docs/terminal-profiles.md).
 */
#ifdef KITTY_PROFILE_STANDALONE
#include <SDL3/SDL.h>
#else
#include "SDL_internal.h"
#endif

#ifndef SDL_kittyprofile_h_
#define SDL_kittyprofile_h_

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROFILE_STANDALONE)

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
 * Per-host policy + tunable defaults. Runtime probes (a=q / SHM / a=f) still
 * decide what is *available*; this struct decides defaults and quirk thresholds.
 *
 * Canonical file: config/terminals.toml (see docs/terminal-profiles.md).
 * Env SDL_KITTY_* still wins last.
 */
typedef struct KITTY_TerminalProfile
{
    KITTY_TerminalKind kind;
    const char *name;

    /* When both SHM and direct probe OK, prefer direct (Rex). */
    bool prefer_direct;

    /*
     * When not using SHM, prefer zlib-compressed direct (o=z) over raw base64.
     * Env SDL_KITTY_TRANSMIT still wins. Exp 2 soak: wezterm true.
     */
    bool prefer_zlib;

    /*
     * Optional profile force of transmit medium: "direct", "zlib", "shm", or NULL.
     * Applied after env. Ghostty zlib A/B without changing prefer_direct/SHM default.
     */
    const char *force_transmit;

    /* Default aspect-fit FB cap when SDL_KITTY_FB_MAX unset. 0 = no default cap. */
    int default_fb_max_w;
    int default_fb_max_h;

    /*
     * Transport-aware soft-maxes (source-backed; see config/terminals.toml notes).
     * 0 = fall back to default_fb_max_*. Applied from KITTY_ApplyFramebufferCap
     * using transmit_mode after probe (SHM vs direct/zlib).
     */
    int default_fb_max_shm_w;
    int default_fb_max_shm_h;
    int default_fb_max_direct_w;
    int default_fb_max_direct_h;

    /* Refuse direct a=T / fallback above this many raw bytes. */
    size_t max_direct_bytes;

    /* Skip a=f (use full a=T) when dirty coverage or pixel count exceeds. */
    float af_max_coverage;
    size_t af_max_pixels;

    /* Read keyboard from stdin (Rex delivers keys on fd0 only). */
    bool keys_on_stdin;

    /* Treat silent a=f probe as success (Rex). */
    bool silent_af_ok;

    /* --- §B host conditionals (Phase 4 wires call sites) --- */

    /* Ignore TIOCGWINSZ xpixel/ypixel (Rex HiDPI stub). */
    bool ignore_ioctl_pixels;

    /* Allow direct a=f while transmit_mode is SHM (Rex true; Kitty false). */
    bool direct_af_when_shm;

    /*
     * Experiment: sparse dirty updates as stacked a=T overlays (one image per
     * dirty rect) when a=f cannot refresh the display (WezTerm hash bug).
     * Env SDL_KITTY_DIRTY_AT=0|1 overrides. Default off (host hang).
     */
    bool dirty_at_overlays;

    /*
     * Exp 3: single union a=T overlay (DECSC/DECRC + edge skip). Safer than
     * multi dirty_at. Env SDL_KITTY_DIRTY_UNION_AT=0|1 overrides.
     */
    bool dirty_union_at;

    /*
     * Exp 6: skip wire upload when full scratch FNV matches last successful
     * upload (no-op present). Env SDL_KITTY_UPLOAD_SKIP=0|1 overrides.
     */
    bool upload_skip;

    /*
     * Exp 5: merge dirty rects whose expanded boxes intersect (gap in pixels).
     * 0 = off. Env SDL_KITTY_COALESCE_GAP overrides.
     */
    int coalesce_gap_px;

    /* Max raw size for SHM-fail → direct fallback when !prefer_direct. */
    size_t direct_fallback_max_bytes;

    /* --- §C tunables (usually from [defaults]; Phase 4 wires call sites) --- */

    size_t apc_chunk_bytes;
    int probe_timeout_ms;
    int drain_passes;
    int drain_quiet_us;
    int stub_winch_iters;
    int stub_winch_delay_ms;
    int stub_cols;
    int stub_rows;
    int key_hold_ms;
    bool fb_diff;
    int fb_diff_tile;
    float fb_diff_cover;
    int tty_pending_max;
    int window_fallback_w;
    int window_fallback_h;
    bool place_cells;
    bool pingpong;
    /* Protocol: terminal unlinks after read — client must not unlink on success. */
    bool shm_unlink_after_send;

    /* Docs / DEBUG only; may be NULL. */
    const char *notes;
} KITTY_TerminalProfile;

/*
 * Load TOML overlays from SDL_KITTY_PROFILES or default search paths.
 * Idempotent after the first attempt (success or not).
 */
void KITTY_EnsureTerminalProfilesLoaded(void);

/*
 * Overlay or replace profiles from an explicit TOML path. First successful
 * load replaces builtins; later loads overlay. Returns true on success.
 */
bool KITTY_LoadTerminalProfiles(const char *path);

/*
 * Drop the loaded table and clear the load-attempted latch so the next
 * Ensure/Detect uses builtins only until TOML is loaded again.
 */
void KITTY_ResetTerminalProfilesToBuiltin(void);

/*
 * Detect the active profile: ensure TOML loaded, then match env against
 * loaded [[terminal]] rows, else builtin TERM_PROGRAM/TERM heuristics.
 * Never returns NULL.
 */
const KITTY_TerminalProfile *KITTY_DetectTerminalProfile(void);

/*
 * "builtin" or semicolon-joined path(s) of the last successful TOML load
 * (for DEBUG). Never returns NULL.
 */
const char *KITTY_TerminalProfilesSource(void);

/*
 * True when a loaded [[terminal]] matcher hits the current env (used by
 * ProbeSupport). Ensures profiles are loaded first.
 */
bool KITTY_EnvMatchesLoadedProfile(void);

/*
 * Builtin TERM_PROGRAM / TERM / KITTY_WINDOW_ID heuristics for "looks like a
 * Kitty-protocol host" when no TOML match is required.
 */
bool KITTY_BuiltinEnvLooksLikeKittyHost(void);

/*
 * APC payload chunk size from profile, or 2 MiB when unset/zero.
 */
static inline size_t KITTY_ProfileApcChunkBytes(const KITTY_TerminalProfile *p)
{
    return (p && p->apc_chunk_bytes > 0) ? p->apc_chunk_bytes : ((size_t)2 * 1024 * 1024);
}

/*
 * Max raw bytes for SHM-fail → direct fallback, or 256 KiB when unset/zero.
 */
static inline size_t KITTY_ProfileDirectFallbackBytes(const KITTY_TerminalProfile *p)
{
    return (p && p->direct_fallback_max_bytes > 0) ? p->direct_fallback_max_bytes
                                                   : ((size_t)256 * 1024);
}

/*
 * Probe wait budget (attempts) from profile probe_timeout_ms, or 40.
 */
static inline int KITTY_ProfileProbeAttempts(const KITTY_TerminalProfile *p)
{
    return (p && p->probe_timeout_ms > 0) ? p->probe_timeout_ms : 40;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROFILE_STANDALONE */
#endif /* SDL_kittyprofile_h_ */
