/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>
*/

/*
 * Kitty graphics probe helpers: parse APC query replies, select transmit /
 * dirty-present policy from env + profile + probe results, format a=T
 * headers, classify pane resize, and (non-standalone) run tty a=q / a=f
 * capability probes. Also optional runtime debug and PERF_ACK wait helpers.
 */
#ifdef KITTY_PROBE_STANDALONE
#include <SDL3/SDL.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROBE_STANDALONE)

#ifndef SDL_kittyprobe_h_
#define SDL_kittyprobe_h_

#ifndef KITTY_PROBE_STANDALONE
#include "SDL_kittyvideo.h"
#endif

typedef enum KITTY_GraphicsQueryStatus
{
    KITTY_GQ_NONE = 0,
    KITTY_GQ_OK,
    KITTY_GQ_ERROR
} KITTY_GraphicsQueryStatus;

/*
 * Scan buf for an APC graphics reply ESC_G i=<expect_id>…;OK|error ST
 * (ST is ESC\ or BEL). Returns OK, ERROR, or NONE if no matching reply.
 */
KITTY_GraphicsQueryStatus KITTY_ParseGraphicsQueryReply(const char *buf, size_t len, Uint32 expect_id);

#ifdef SDL_KITTY_PERF_ACK
/*
 * Block on fd until Gi=<id>;OK|ERROR (or timeout). Optional present-path
 * timing aid; compiled only with cmake -DSDL_KITTY_PERF_ACK=ON.
 */
KITTY_GraphicsQueryStatus KITTY_AwaitGraphicsAck(int fd, Uint32 id, int timeout_ms);
#endif

#ifdef KITTY_PROBE_STANDALONE
typedef enum KITTY_TransmitMode
{
    KITTY_TRANSMIT_SHM = 0,
    KITTY_TRANSMIT_DIRECT_ZLIB,
    KITTY_TRANSMIT_DIRECT
} KITTY_TransmitMode;
#endif

/*
 * Choose transmit medium: force_env (SDL_KITTY_TRANSMIT) wins, else
 * force_profile (TOML force_transmit), else prefer_direct / prefer_zlib /
 * shm_ok. direct_ok is currently unused by the policy.
 */
KITTY_TransmitMode KITTY_SelectTransmitMode(const char *force_env,
                                            const char *force_profile,
                                            bool prefer_direct,
                                            bool prefer_zlib,
                                            bool shm_ok,
                                            bool direct_ok);

/*
 * Dirty-BW (a=f rect edit) policy: env SDL_KITTY_DIRTY_BW forces on/off;
 * if unset, follow frame_edit_ok from the a=f probe.
 */
bool KITTY_SelectDirtyBW(const char *env, bool frame_edit_ok);

/*
 * Dirty-AT overlays: env SDL_KITTY_DIRTY_AT forces on/off; if unset, follow
 * profile dirty_at_overlays (WezTerm experiment).
 */
bool KITTY_SelectDirtyAT(const char *env, bool profile_default);

/*
 * SDL_KITTY_DIRTY_UNION_AT: env forces on/off; else profile dirty_union_at.
 */
bool KITTY_SelectDirtyUnionAT(const char *env, bool profile_default);

/*
 * SDL_KITTY_UPLOAD_SKIP: env forces on/off; else profile upload_skip.
 */
bool KITTY_SelectUploadSkip(const char *env, bool profile_default);

/*
 * SDL_KITTY_COALESCE_GAP: unset → profile; "0"/off → 0; else atoi clamped
 * to [0, 256].
 */
int KITTY_SelectCoalesceGap(const char *env, int profile_default);

/*
 * SDL_KITTY_PINGPONG: unset → default_on; "0"/off → false; else true.
 * When on, presents use delete+swap image ids instead of same-(i,p) replace.
 */
bool KITTY_SelectPingPong(const char *env, bool default_on);

/* Step 3 place keys: no cursor advance, transient, quiet (q=2 = no APC OK). */
#define KITTY_AT_PLACE_KEYS "C=1,N=1,p=1,q=2"

/*
 * SDL_KITTY_FB_ALPHA=1/true/yes/on → ARGB8888 window FB + Kitty f=32.
 * Unset or any other value → false (RGB24 / f=24).
 */
bool KITTY_SelectFbAlpha(const char *env);

/*
 * Runtime debug (present lines, probes) is compiled only when
 * SDL_KITTY_RUNTIME_DEBUG is defined (CMake -DSDL_KITTY_RUNTIME_DEBUG=ON).
 * Then SDL_KITTY_DEBUG=1 enables VIDEO logs at runtime.
 */
#ifdef SDL_KITTY_RUNTIME_DEBUG
/*
 * True when SDL_KITTY_DEBUG is set and not "0". Caches the result; first
 * enable also raises VIDEO log priority and line-buffers stderr.
 */
bool KITTY_DebugEnabled(void);

/*
 * Format and emit a debug line to SDL_LogDebug (non-standalone) and stderr
 * when KITTY_DebugEnabled(). No-op if debug is off or fmt is NULL.
 */
void KITTY_DebugLog(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
#else
#define KITTY_DebugEnabled() (false)
#define KITTY_DebugLog(...) ((void)0)
#endif

/*
 * Format first-chunk a=T APC header (without ESC_G prefix or trailing
 * payload). medium: 'd' or 's'. kitty_f: 24 (RGB) or 32 (RGBA).
 * extra: e.g. "o=z" or NULL (comma-prefixed when set). more: true → m=1.
 * shm_bytes used only for t=s (S=). Returns bytes written (excl NUL) or -1.
 */
int KITTY_FormatTransmitHeader(char *buf, size_t cap,
                               char medium, Uint32 image_id,
                               int w, int h, int place_cols, int place_rows,
                               bool more, const char *extra,
                               size_t shm_bytes /* 0 if not t=s */,
                               int kitty_f /* 24 or 32 */);

typedef enum KITTY_ResizeAction
{
    KITTY_RESIZE_NONE = 0, /* grid and FB unchanged */
    KITTY_RESIZE_REPLACE,  /* stretch: FB same, cols/rows changed → a=p only */
    KITTY_RESIZE_FULL      /* FB size changed or no live image → recreate */
} KITTY_ResizeAction;

/*
 * Decide how to handle a pane/FB size change: NONE (nothing), REPLACE
 * (place_cells stretch, same pixels, a=p only), or FULL recreate.
 */
KITTY_ResizeAction KITTY_ClassifyResize(bool place_cells, bool has_live_image,
                                        int old_pw, int old_ph, int new_pw, int new_ph,
                                        int old_cols, int old_rows, int new_cols, int new_rows);

#ifndef KITTY_PROBE_STANDALONE
/*
 * Run a=q (+ optional a=f) probes on the tty; set data->transmit_mode,
 * shm_failed, and frame_edit_ok. Honors SDL_KITTY_TRANSMIT / DIRTY_BW env.
 */
bool KITTY_ProbeTransmitModes(SDL_VideoData *data);
#endif

#endif /* SDL_kittyprobe_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROBE_STANDALONE */
