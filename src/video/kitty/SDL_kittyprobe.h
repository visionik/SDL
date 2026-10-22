/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
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

/* Scan buf for APC reply ESC_Gi=<id>;OK|error ESC\ */
KITTY_GraphicsQueryStatus KITTY_ParseGraphicsQueryReply(const char *buf, size_t len, Uint32 expect_id);

/*
 * Choose transmit mode from env + probe results.
 * force_env: SDL_KITTY_TRANSMIT value or NULL
 * term_program: TERM_PROGRAM or NULL
 * shm_ok / direct_ok: a=q outcomes
 */
#ifdef KITTY_PROBE_STANDALONE
typedef enum KITTY_TransmitMode
{
    KITTY_TRANSMIT_SHM = 0,
    KITTY_TRANSMIT_DIRECT_ZLIB,
    KITTY_TRANSMIT_DIRECT
} KITTY_TransmitMode;
#endif

KITTY_TransmitMode KITTY_SelectTransmitMode(const char *force_env,
                                            const char *term_program,
                                            bool shm_ok,
                                            bool direct_ok);

/* Step 3: no cursor advance, transient image, stable placement id. */
#define KITTY_AT_PLACE_KEYS "C=1,N=1,p=1"

/*
 * Format first-chunk a=T APC header (without ESC_G prefix or trailing payload).
 * medium: 'd' or 's'. extra: e.g. "o=z" or NULL (no leading comma).
 * more: true → include m=1. Returns bytes written (excl NUL) or -1.
 */
int KITTY_FormatTransmitHeader(char *buf, size_t cap,
                               char medium, Uint32 image_id,
                               int w, int h, int place_cols, int place_rows,
                               bool more, const char *extra,
                               size_t shm_bytes /* 0 if not t=s */);

typedef enum KITTY_ResizeAction
{
    KITTY_RESIZE_NONE = 0,   /* grid and FB unchanged */
    KITTY_RESIZE_REPLACE,    /* stretch: FB same, cols/rows changed → a=p only */
    KITTY_RESIZE_FULL        /* FB size changed or no live image → recreate */
} KITTY_ResizeAction;

KITTY_ResizeAction KITTY_ClassifyResize(bool place_cells, bool has_live_image,
                                        int old_pw, int old_ph, int new_pw, int new_ph,
                                        int old_cols, int old_rows, int new_cols, int new_rows);

#ifndef KITTY_PROBE_STANDALONE
/* Run a=q probes on the tty and set data->transmit_mode / shm_failed. */
bool KITTY_ProbeTransmitModes(SDL_VideoData *data);
#endif

#endif /* SDL_kittyprobe_h_ */

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROBE_STANDALONE */
