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

#ifndef SDL_kittyvideo_h_
#define SDL_kittyvideo_h_

/*
 * Shared types for the kitty SDL video driver: per-device tty/APC state
 * (SDL_VideoData) and per-window image/scratch buffers (SDL_WindowData).
 */

#include "../SDL_sysvideo.h"

#include <signal.h>

#define KITTYVID_DRIVER_NAME "kitty"

/* Carry incomplete CSI/APC across read() calls in KITTY_PumpEvents. */
#define KITTY_TTY_PENDING_MAX 4096

/*
 * How full-frame RGB is sent over the Kitty graphics protocol: POSIX SHM,
 * zlib-compressed direct APC, or raw direct APC.
 */
typedef enum KITTY_TransmitMode
{
    KITTY_TRANSMIT_SHM = 0,
    KITTY_TRANSMIT_DIRECT_ZLIB,
    KITTY_TRANSMIT_DIRECT
} KITTY_TransmitMode;

struct KITTY_TerminalProfile; /* SDL_kittyprofile.h */

/*
 * Driver-private state for one kitty SDL_VideoDevice: tty fds, terminal
 * profile, cell/pixel geometry, mouse mode, transmit path, and pending CSI.
 */
typedef struct SDL_VideoData
{
    /* Rex (and some hosts) deliver keyboard on stdin while APC/CSI go out
     * on stdout. Keep separate fds; tty_fd aliases tty_out for write helpers. */
    int tty_in;
    int tty_out;
    int tty_fd;
    const struct KITTY_TerminalProfile *term_profile;
    bool terminal_owned;
    bool keyboard_enabled;
    /* True when CSI ? u answered after CSI > 11 u (Kitty keyboard protocol
     * active). WezTerm stays false unless enable_kitty_keyboard=true. */
    bool kitty_keyboard_ok;
    bool mouse_enabled;
    bool mouse_sgr_pixels; /* true: DECSET 1016 (pixel coords); false: 1006 (cells) */
    bool place_cells;      /* true: Kitty c/r stretch to grid; scale mouse into FB */
    bool use_alt_screen;   /* false: TUI card mode — no smcup/clear (SDL_KITTY_ALTSCREEN=0) */
    /* Optional cell viewport for card mode (docs/tui-card.md). 1-based origin;
     * vp_cols/vp_rows > 0 enables. Place uses this c×r; CUP to (vp_row,vp_col). */
    int vp_col;
    int vp_row;
    int vp_cols;
    int vp_rows;
    bool shm_failed;
    Uint32 shm_seq; /* unique POSIX SHM object names (avoid reuse races) */
    bool frame_edit_ok; /* a=f root-frame edit probe; gates dirty BW default */
    KITTY_TransmitMode transmit_mode;
    int cell_w;
    int cell_h;
    int cols;
    int rows;
    int pixel_w;
    int pixel_h;
    /* 1016 coordinate space (often cols*cell_w on Rex); 0 = treat as FB pixels. */
    int mouse_coord_w;
    int mouse_coord_h;
    int mouse_raw_x; /* last 1016/1006 report, 0-based */
    int mouse_raw_y;
    int placed_cols; /* last c/r sent with stretch place; 0 = none */
    int placed_rows;
    bool place_homed; /* CSI home already sent for current place; skip on replace */
    SDL_Window *focus_window;
    bool focus_announced; /* SetKeyboardFocus once from first PumpEvents (deferred from CreateWindow) */
    volatile sig_atomic_t got_winch;
    unsigned char tty_pending[KITTY_TTY_PENDING_MAX];
    size_t tty_pending_len;
    /* Perf counters (SDL_KITTY_DEBUG present lines + kitty_perf_summary.py). */
    size_t last_wire_bytes; /* APC bytes written on last successful upload */
    size_t last_raw_bytes;  /* payload before b64 (zlib size or raw RGB) */
    double last_ms_tx;      /* client compress+encode+write wall time */
    double last_ms_ack;     /* host round-trip wait (-1 if not measured) */
    Uint64 total_wire_bytes;
    double total_ms_tx;
    double total_ms_ack;
    Uint32 wire_presents;
    Uint32 ack_presents; /* presents where ms_ack was measured */
    Uint32 skip_presents;
} SDL_VideoData;

/* Stacked a=T dirty overlays (dirty_at_overlays experiment). Ids 100+. */
#define KITTY_OVERLAY_MAX 16
#define KITTY_OVERLAY_ID_BASE 100u

/*
 * Driver-private state for one SDL_Window: Kitty image ids, retained RGB
 * scratch/back buffers for dirty present, and optional overlay stack.
 */
typedef struct SDL_WindowData
{
    SDL_Window *sdl_window;
    Uint32 image_ids[2];
    int image_index;
    bool placed;
    /* Retained RGB24/RGBA32: rgb_scratch = last presented; rgb_back = convert target
     * for tile-diff (pointer-swapped after each present). */
    Uint8 *rgb_scratch;
    Uint8 *rgb_back;
    int rgb_w;
    int rgb_h;
    int scratch_bpp; /* 3 (f=24) or 4 (f=32) */
    bool rgb_valid;
    /* Image id of the last successful full place (Step 1b a=f target). */
    Uint32 live_image_id;
    /* Active dirty_at overlay image ids from the previous present. */
    Uint32 overlay_ids[KITTY_OVERLAY_MAX];
    int overlay_count;
    /* Exp 6: FNV of last successfully uploaded scratch (full buffer). */
    Uint64 last_upload_hash;
    bool last_upload_hash_valid;
} SDL_WindowData;

#endif // SDL_kittyvideo_h_

#endif // SDL_VIDEO_DRIVER_KITTY
