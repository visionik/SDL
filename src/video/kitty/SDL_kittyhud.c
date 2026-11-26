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
  2. Altered source versions must be plainly marked as such, and must not
     be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KITTY

#include "SDL_kittyfont.h"
#include "SDL_kittyhud.h"

#define KITTY_HUD_SAMPLES 96

typedef enum KITTY_HudPos
{
    KITTY_HUD_TOP_RIGHT = 0,
    KITTY_HUD_TOP_LEFT,
    KITTY_HUD_BOTTOM_RIGHT,
    KITTY_HUD_BOTTOM_LEFT
} KITTY_HudPos;

typedef struct KITTY_HudPalette
{
    Uint8 bg_top[4];
    Uint8 bg_bot[4];
    Uint8 border[4];
    Uint8 well_bg[4];
    Uint8 well_border[4];
    Uint8 fg[4];
    Uint8 muted[4];
    Uint8 accent[4];
    Uint8 accent2[4];
    Uint8 spark_line[4];
    Uint8 spark_top[4];
    Uint8 spark_bot[4];
    Uint8 tx_line[4];
    Uint8 track[4];
    Uint8 sheen;
} KITTY_HudPalette;

struct KITTY_PerfHud
{
    bool enabled;
    KITTY_HudPos pos;
    KITTY_HudPalette pal;

    /* Telemetry ring buffer (bucketed at ~20 Hz for silky ~4.8s history) */
    Uint64 last_frame_ns;
    Uint64 bucket_start_ns;
    double bucket_ft_sum;
    double bucket_tx_sum;
    int bucket_n;
    float frame_ms_ring[KITTY_HUD_SAMPLES];
    float tx_ms_ring[KITTY_HUD_SAMPLES];
    int ring_head;
    int ring_count;
    float smooth_max_ms;

    double ewma_fps;
    double ewma_tx_ms;
    double ewma_wire_kb;
    double ewma_dirty_pct;
    double bw_mbps;
    Uint64 bw_window_ns;
    Uint64 bw_window_bytes;
    char transport[24];
    bool async_drop;
    Uint64 dropped_frames;

    /* Recording indicator */
    bool rec_active;
    Uint64 rec_start_ms;

    /* Rendered RGBA bitmap cache (re-rendered at ~15 Hz) */
    Uint64 last_render_ms;
    int cache_fb_w;
    int cache_fb_h;
    bool dirty_visual;
    SDL_Rect rect;
    Uint8 *rgba;
    size_t rgba_cap;

    /* Backing store for clean rgb_scratch pixels under rect */
    Uint8 *save_buf;
    size_t save_cap;
    SDL_Rect save_rect;
    bool saved;

    bool last_drawn_visible;
    SDL_Rect last_drawn_rect;
};

static void KITTY_HudInitPalette(KITTY_HudPalette *p, const char *theme)
{
    if (theme && SDL_strcasecmp(theme, "amber") == 0) {
        const KITTY_HudPalette amber = {
            { 24, 18, 14, 232 }, { 14, 11, 9, 238 },
            { 245, 166, 35, 185 },
            { 10, 8, 6, 165 }, { 245, 166, 35, 48 },
            { 252, 244, 232, 255 }, { 166, 145, 122, 225 },
            { 245, 166, 35, 255 }, { 255, 214, 102, 255 },
            { 255, 186, 66, 255 }, { 245, 158, 11, 95 }, { 245, 158, 11, 4 },
            { 56, 189, 248, 200 },
            { 255, 255, 255, 28 }, 38
        };
        *p = amber;
    } else if (theme && SDL_strcasecmp(theme, "nord") == 0) {
        const KITTY_HudPalette nord = {
            { 34, 41, 56, 232 }, { 23, 28, 39, 238 },
            { 136, 192, 208, 175 },
            { 17, 21, 30, 165 }, { 136, 192, 208, 45 },
            { 236, 239, 244, 255 }, { 147, 161, 183, 225 },
            { 136, 192, 208, 255 }, { 163, 190, 140, 255 },
            { 136, 192, 208, 255 }, { 136, 192, 208, 95 }, { 94, 129, 172, 6 },
            { 235, 203, 139, 215 },
            { 255, 255, 255, 28 }, 36
        };
        *p = nord;
    } else if (theme && SDL_strcasecmp(theme, "light") == 0) {
        const KITTY_HudPalette light = {
            { 248, 250, 253, 238 }, { 235, 240, 248, 242 },
            { 9, 105, 218, 165 },
            { 225, 232, 244, 190 }, { 9, 105, 218, 52 },
            { 22, 29, 43, 255 }, { 92, 106, 128, 230 },
            { 9, 105, 218, 255 }, { 14, 165, 233, 255 },
            { 9, 105, 218, 255 }, { 14, 165, 233, 82 }, { 14, 165, 233, 6 },
            { 217, 119, 6, 220 },
            { 15, 23, 42, 28 }, 60
        };
        *p = light;
    } else {
        /* Default: Tokyo Night glassmorphic obsidian-indigo */
        const KITTY_HudPalette tokyo = {
            { 22, 27, 46, 232 }, { 12, 15, 28, 240 },
            { 122, 162, 247, 175 },
            { 8, 11, 22, 175 }, { 122, 162, 247, 48 },
            { 238, 243, 255, 255 }, { 138, 152, 188, 230 },
            { 122, 162, 247, 255 }, { 42, 195, 222, 255 },
            { 42, 195, 222, 255 }, { 42, 195, 222, 96 }, { 122, 162, 247, 6 },
            { 247, 178, 103, 215 },
            { 255, 255, 255, 28 }, 42
        };
        *p = tokyo;
    }
}

KITTY_PerfHud *KITTY_HudCreate(const char *spec)
{
    KITTY_PerfHud *hud = (KITTY_PerfHud *)SDL_calloc(1, sizeof(KITTY_PerfHud));
    if (!hud) {
        return NULL;
    }
    hud->pos = KITTY_HUD_TOP_RIGHT;
    hud->smooth_max_ms = 33.333f;
    KITTY_HudInitPalette(&hud->pal, "tokyo");
    SDL_strlcpy(hud->transport, "KITTY", sizeof(hud->transport));

    if (spec && spec[0]) {
        if (SDL_strcasecmp(spec, "1") == 0 || SDL_strcasecmp(spec, "true") == 0 ||
            SDL_strcasecmp(spec, "on") == 0 || SDL_strcasecmp(spec, "yes") == 0) {
            hud->enabled = true;
        } else if (SDL_strcasecmp(spec, "0") == 0 || SDL_strcasecmp(spec, "false") == 0 ||
                   SDL_strcasecmp(spec, "off") == 0 || SDL_strcasecmp(spec, "no") == 0) {
            hud->enabled = false;
        } else {
            /* Parse key=val pairs: e.g. pos=top-left,theme=amber */
            const char *p = spec;
            hud->enabled = true;
            while (*p) {
                const char *sep = p;
                char key[32], val[64];
                const char *eq = NULL;
                size_t klen, vlen;
                while (*sep && *sep != ',' && *sep != ';') {
                    if (!eq && *sep == '=') eq = sep;
                    sep++;
                }
                if (eq) {
                    klen = (size_t)(eq - p);
                    vlen = (size_t)(sep - (eq + 1));
                    if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                    if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
                    SDL_memcpy(key, p, klen); key[klen] = '\0';
                    SDL_memcpy(val, eq + 1, vlen); val[vlen] = '\0';
                    if (SDL_strcasecmp(key, "pos") == 0) {
                        if (SDL_strcasecmp(val, "top-left") == 0 || SDL_strcasecmp(val, "tl") == 0) {
                            hud->pos = KITTY_HUD_TOP_LEFT;
                        } else if (SDL_strcasecmp(val, "bottom-right") == 0 || SDL_strcasecmp(val, "br") == 0) {
                            hud->pos = KITTY_HUD_BOTTOM_RIGHT;
                        } else if (SDL_strcasecmp(val, "bottom-left") == 0 || SDL_strcasecmp(val, "bl") == 0) {
                            hud->pos = KITTY_HUD_BOTTOM_LEFT;
                        } else {
                            hud->pos = KITTY_HUD_TOP_RIGHT;
                        }
                    } else if (SDL_strcasecmp(key, "theme") == 0) {
                        KITTY_HudInitPalette(&hud->pal, val);
                    }
                }
                p = *sep ? (sep + 1) : sep;
            }
        }
    }
    return hud;
}

void KITTY_HudDestroy(KITTY_PerfHud *hud)
{
    if (!hud) {
        return;
    }
    SDL_free(hud->rgba);
    SDL_free(hud->save_buf);
    SDL_free(hud);
}

bool KITTY_HudHandleKey(KITTY_PerfHud *hud, SDL_Scancode scancode)
{
    if (!hud || scancode != SDL_SCANCODE_F12) {
        return false;
    }
    hud->enabled = !hud->enabled;
    hud->dirty_visual = true;
    return true;
}

void KITTY_HudSetRecording(KITTY_PerfHud *hud, bool active, Uint64 start_ms)
{
    if (!hud) {
        return;
    }
    if (hud->rec_active != active || hud->rec_start_ms != start_ms) {
        hud->rec_active = active;
        hud->rec_start_ms = start_ms;
        hud->dirty_visual = true;
    }
}

void KITTY_HudRecordFrame(KITTY_PerfHud *hud, Uint64 now_ns, double tx_ms,
                          size_t wire_bytes, float dirty_pct,
                          const char *transport_tag, bool async_drop_active,
                          Uint64 dropped_frames)
{
    double dt_ms = 16.667;
    if (!hud) {
        return;
    }
    if (hud->last_frame_ns > 0 && now_ns > hud->last_frame_ns) {
        dt_ms = (double)(now_ns - hud->last_frame_ns) / 1000000.0;
        if (dt_ms < 0.5) dt_ms = 0.5;
        if (dt_ms > 120.0) dt_ms = 120.0;
    }
    hud->last_frame_ns = now_ns;

    /* Bucket into calm 50 ms (~20 Hz) averages so single-tick OS timer jitter
     * doesn't vibrate the sparkline (96 samples = ~4.8s of smooth history). */
    if (hud->bucket_start_ns == 0) {
        hud->bucket_start_ns = now_ns;
    }
    hud->bucket_ft_sum += dt_ms;
    hud->bucket_tx_sum += (tx_ms >= 0.0 ? tx_ms : 0.0);
    hud->bucket_n++;

    if (now_ns >= hud->bucket_start_ns + 50000000ULL || hud->ring_count < 2) {
        const float avg_ft = (float)(hud->bucket_ft_sum / (double)hud->bucket_n);
        const float avg_tx = (float)(hud->bucket_tx_sum / (double)hud->bucket_n);
        hud->frame_ms_ring[hud->ring_head] = avg_ft;
        hud->tx_ms_ring[hud->ring_head] = avg_tx;
        hud->ring_head = (hud->ring_head + 1) % KITTY_HUD_SAMPLES;
        if (hud->ring_count < KITTY_HUD_SAMPLES) {
            hud->ring_count++;
        }
        hud->bucket_start_ns = now_ns;
        hud->bucket_ft_sum = 0.0;
        hud->bucket_tx_sum = 0.0;
        hud->bucket_n = 0;
    }

    {
        const double inst_fps = 1000.0 / dt_ms;
        const double alpha = (hud->ewma_fps <= 0.0) ? 1.0 : 0.08;
        hud->ewma_fps = hud->ewma_fps + alpha * (inst_fps - hud->ewma_fps);
        hud->ewma_tx_ms = hud->ewma_tx_ms + 0.10 * (tx_ms - hud->ewma_tx_ms);
        hud->ewma_wire_kb = hud->ewma_wire_kb + 0.10 * (((double)wire_bytes / 1024.0) - hud->ewma_wire_kb);
        hud->ewma_dirty_pct = hud->ewma_dirty_pct + 0.12 * ((double)dirty_pct - hud->ewma_dirty_pct);
    }

    if (hud->bw_window_ns == 0) {
        hud->bw_window_ns = now_ns;
    }
    hud->bw_window_bytes += (Uint64)wire_bytes;
    if (now_ns > hud->bw_window_ns + 500000000ULL) {
        const double sec = (double)(now_ns - hud->bw_window_ns) / 1e9;
        hud->bw_mbps = ((double)hud->bw_window_bytes / (1024.0 * 1024.0)) / (sec > 0.01 ? sec : 0.01);
        hud->bw_window_ns = now_ns;
        hud->bw_window_bytes = 0;
    }

    /* Keep active transport badge stable; don't flicker to "SKIP" on idle frames. */
    if (transport_tag && transport_tag[0] && SDL_strcmp(transport_tag, "SKIP") != 0) {
        SDL_strlcpy(hud->transport, transport_tag, sizeof(hud->transport));
    }
    hud->async_drop = async_drop_active;
    hud->dropped_frames = dropped_frames;
}

static bool KITTY_HudRenderBitmap(KITTY_PerfHud *hud, int fb_w, int fb_h, Uint64 now_ms)
{
    KITTY_Canvas canvas;
    float s;
    int shadow_pad, card_w, card_h, bw, bh, cx, cy;
    int ox, oy, margin_x, margin_y;
    size_t need;

    if (!hud || fb_w <= 64 || fb_h <= 64) {
        return false;
    }

    /* Compact standalone REC pill when HUD is off but recording is active. */
    if (!hud->enabled && hud->rec_active) {
        const Uint64 rec_s = (now_ms >= hud->rec_start_ms) ? ((now_ms - hud->rec_start_ms) / 1000u) : 0u;
        const unsigned mins = (unsigned)(rec_s / 60u);
        const unsigned secs = (unsigned)(rec_s % 60u);
        char label[32];
        float px_sz, tw;
        const bool pulse_hi = ((now_ms / 450u) & 1u) == 0;
        const Uint8 bg_t[4] = { 34, 12, 18, 232 };
        const Uint8 bg_b[4] = { 20, 8, 12, 238 };
        const Uint8 brd[4] = { 244, 63, 94, 210 };
        const Uint8 dot_c[4] = { 255, 72, 102, pulse_hi ? 255 : 140 };
        const Uint8 txt_c[4] = { 255, 236, 240, 255 };

        s = (float)fb_h / 720.0f;
        if (s < 0.72f) s = 0.72f;
        if (s > 1.35f) s = 1.35f;
        px_sz = 14.5f * s;
        SDL_snprintf(label, sizeof(label), "REC %02u:%02u", mins, secs);
        tw = KITTY_FontMeasureUTF8(label, SDL_strlen(label), px_sz, true);
        shadow_pad = (int)(6.0f * s);
        card_w = (int)(tw + 38.0f * s);
        card_h = (int)(30.0f * s);
        bw = card_w + shadow_pad * 2;
        bh = card_h + shadow_pad * 2 + 2;
        if (bw > fb_w - 8) bw = fb_w - 8;
        if (bh > fb_h - 8) bh = fb_h - 8;

        need = (size_t)bw * (size_t)bh * 4u;
        if (hud->rgba_cap < need) {
            Uint8 *nb = (Uint8 *)SDL_realloc(hud->rgba, need);
            if (!nb) return false;
            hud->rgba = nb;
            hud->rgba_cap = need;
        }
        SDL_memset(hud->rgba, 0, need);
        canvas.rgba = hud->rgba;
        canvas.w = bw;
        canvas.h = bh;

        KITTY_CanvasDrawRoundedCard(&canvas, shadow_pad, shadow_pad, card_w, card_h,
                                    (float)card_h * 0.5f, bg_t, bg_b, brd, shadow_pad, 32);
        KITTY_CanvasDrawGlowDot(&canvas, (float)shadow_pad + 15.0f * s,
                                (float)shadow_pad + (float)card_h * 0.5f,
                                4.0f * s, (pulse_hi ? 10.0f : 7.0f) * s, dot_c);
        KITTY_FontDrawUTF8(&canvas, (float)shadow_pad + 26.0f * s,
                           (float)shadow_pad + (float)card_h * 0.5f + px_sz * 0.34f,
                           label, SDL_strlen(label), px_sz, txt_c, true, true);

        hud->rect.x = (int)(14.0f * s);
        hud->rect.y = (int)(14.0f * s);
        hud->rect.w = bw;
        hud->rect.h = bh;
        hud->cache_fb_w = fb_w;
        hud->cache_fb_h = fb_h;
        return true;
    }

    /* Sleek compact glassmorphic Performance HUD: compact via tight layout
     * (header + sparkline + single telemetry strip) while keeping crisp
     * readable Geist-Regular font sizes (never below 11px). */
    {
        const float sh = (float)fb_h / 900.0f;
        s = sh;
        if (s < 0.85f) s = 0.85f;
        if (s > 1.35f) s = 1.35f;
    }

    shadow_pad = (int)(7.0f * s + 0.5f);
    card_w = (int)(296.0f * s + 0.5f);
    card_h = (int)(98.0f * s + 0.5f);
    if (card_w > fb_w - 16) {
        card_w = fb_w - 16;
        s = (float)card_w / 296.0f;
        card_h = (int)(98.0f * s + 0.5f);
    }
    if (card_h > fb_h - 16) card_h = fb_h - 16;
    if (card_w < 120 || card_h < 56) {
        return false;
    }

    bw = card_w + shadow_pad * 2;
    bh = card_h + shadow_pad * 2 + 2;
    if (bw > fb_w) bw = fb_w;
    if (bh > fb_h) bh = fb_h;

    need = (size_t)bw * (size_t)bh * 4u;
    if (hud->rgba_cap < need) {
        Uint8 *nb = (Uint8 *)SDL_realloc(hud->rgba, need);
        if (!nb) {
            return false;
        }
        hud->rgba = nb;
        hud->rgba_cap = need;
    }
    SDL_memset(hud->rgba, 0, need);
    canvas.rgba = hud->rgba;
    canvas.w = bw;
    canvas.h = bh;
    cx = shadow_pad;
    cy = shadow_pad;

    /* 1) Floating glassmorphic card */
    KITTY_CanvasDrawRoundedCard(&canvas, cx, cy, card_w, card_h, 11.0f * s,
                                hud->pal.bg_top, hud->pal.bg_bot,
                                hud->pal.border, shadow_pad, hud->pal.sheen);

    {
        const int pad = (int)(11.0f * s + 0.5f);
        const int inner_x = cx + pad;
        const int inner_w = card_w - pad * 2;

        /* 2) Header row: Glowing status orb + crisp FPS + petite fixed-width pill */
        {
            const double fps = hud->ewma_fps;
            Uint8 orb_col[4] = { 74, 222, 128, 255 }; /* Emerald >= 55 */
            char fps_str[16];
            const float fps_px = 19.5f * s;
            const float unit_px = 11.0f * s;
            const float head_base = (float)cy + 24.5f * s;
            float nx;

            if (fps < 29.5 && hud->ring_count > 4) {
                orb_col[0] = 248; orb_col[1] = 113; orb_col[2] = 113; /* Rose */
            } else if (fps < 54.0 && hud->ring_count > 4) {
                orb_col[0] = 251; orb_col[1] = 191; orb_col[2] = 36; /* Amber */
            }
            KITTY_CanvasDrawGlowDot(&canvas, (float)inner_x + 5.0f * s,
                                    (float)cy + 18.0f * s, 3.6f * s, 8.5f * s, orb_col);

            SDL_snprintf(fps_str, sizeof(fps_str), "%.1f", fps > 999.9 ? 999.9 : fps);
            nx = KITTY_FontDrawUTF8(&canvas, (float)inner_x + 15.0f * s, head_base,
                                    fps_str, SDL_strlen(fps_str), fps_px,
                                    hud->pal.fg, true, true);
            KITTY_FontDrawUTF8(&canvas, nx + 4.0f * s, head_base - 0.5f * s,
                               "FPS", 3, unit_px, hud->pal.muted, false, false);

            /* Fixed-width petite transport badge pill with "MODE" label (never jumps) */
            {
                char badge[24];
                Uint8 pill_bg[4] = { hud->pal.accent2[0], hud->pal.accent2[1], hud->pal.accent2[2], 38 };
                Uint8 pill_bd[4] = { hud->pal.accent2[0], hud->pal.accent2[1], hud->pal.accent2[2], 170 };
                Uint8 pill_fg[4] = { 196, 245, 255, 255 };
                const float badge_px = 10.5f * s;
                const float mlbl_px = 9.2f * s;
                const float max_tw = KITTY_FontMeasureUTF8("A=F +Z", 6, badge_px, true);
                float btw, mlw;
                int pw, ph, px, py;

                if (hud->rec_active) {
                    const Uint64 rec_s = (now_ms >= hud->rec_start_ms) ? ((now_ms - hud->rec_start_ms) / 1000u) : 0u;
                    SDL_snprintf(badge, sizeof(badge), "REC %02u:%02u",
                                 (unsigned)(rec_s / 60u), (unsigned)(rec_s % 60u));
                    pill_bg[0] = 244; pill_bg[1] = 63; pill_bg[2] = 94; pill_bg[3] = 56;
                    pill_bd[0] = 244; pill_bd[1] = 63; pill_bd[2] = 94; pill_bd[3] = 215;
                    pill_fg[0] = 255; pill_fg[1] = 214; pill_fg[2] = 224; pill_fg[3] = 255;
                } else {
                    SDL_strlcpy(badge, hud->transport, sizeof(badge));
                }

                btw = KITTY_FontMeasureUTF8(badge, SDL_strlen(badge), badge_px, true);
                pw = (int)((max_tw > btw ? max_tw : btw) + 12.0f * s + 0.5f);
                ph = (int)(17.5f * s + 0.5f);
                px = inner_x + inner_w - pw;
                py = cy + (int)(9.0f * s + 0.5f);
                if (!hud->rec_active) {
                    mlw = KITTY_FontMeasureUTF8("MODE", 4, mlbl_px, false);
                    KITTY_FontDrawUTF8(&canvas, (float)px - mlw - 5.0f * s,
                                       (float)py + (float)ph * 0.5f + mlbl_px * 0.34f,
                                       "MODE", 4, mlbl_px, hud->pal.muted, false, false);
                }
                KITTY_CanvasDrawPill(&canvas, px, py, pw, ph, (float)ph * 0.5f, pill_bg, pill_bd);
                KITTY_FontDrawUTF8(&canvas, (float)px + ((float)pw - btw) * 0.5f,
                                   (float)py + (float)ph * 0.5f + badge_px * 0.34f,
                                   badge, SDL_strlen(badge), badge_px, pill_fg, false, true);
            }
        }

        /* 3) Silky-smooth Sparkline Chart Well */
        {
            const int chart_x = inner_x;
            const int chart_y = cy + (int)(32.0f * s + 0.5f);
            const int chart_w = inner_w;
            const int chart_h = (int)(36.0f * s + 0.5f);
            float raw_ft[KITTY_HUD_SAMPLES];
            float smooth_ft[KITTY_HUD_SAMPLES];
            float smooth_tx[KITTY_HUD_SAMPLES];
            int n = hud->ring_count;
            float peak_ms = 33.333f;
            float max_ms;
            int i;

            KITTY_CanvasDrawPill(&canvas, chart_x, chart_y, chart_w, chart_h, 6.0f * s,
                                 hud->pal.well_bg, hud->pal.well_border);

            if (n >= 2) {
                const int start = (hud->ring_head - n + KITTY_HUD_SAMPLES) % KITTY_HUD_SAMPLES;
                for (i = 0; i < n; ++i) {
                    const int idx = (start + i) % KITTY_HUD_SAMPLES;
                    raw_ft[i] = hud->frame_ms_ring[idx];
                    smooth_tx[i] = hud->tx_ms_ring[idx];
                }
                /* 3-tap Gaussian smoothing eliminates visual micro-jitter */
                for (i = 0; i < n; ++i) {
                    const float p = (i > 0) ? raw_ft[i - 1] : raw_ft[i];
                    const float m = raw_ft[i];
                    const float q = (i + 1 < n) ? raw_ft[i + 1] : raw_ft[i];
                    smooth_ft[i] = 0.25f * p + 0.50f * m + 0.25f * q;
                    if (smooth_ft[i] > peak_ms) {
                        peak_ms = smooth_ft[i];
                    }
                }
                if (peak_ms > 55.0f) peak_ms = 55.0f;
                /* Asymmetric smooth Y-axis ceiling: rises gently, decays slowly */
                if (hud->smooth_max_ms < 20.0f) {
                    hud->smooth_max_ms = peak_ms;
                } else if (peak_ms > hud->smooth_max_ms) {
                    hud->smooth_max_ms += (peak_ms - hud->smooth_max_ms) * 0.22f;
                } else {
                    hud->smooth_max_ms += (peak_ms - hud->smooth_max_ms) * 0.03f;
                }
                max_ms = hud->smooth_max_ms;
                if (max_ms < 33.333f) max_ms = 33.333f;

                /* Subtle dashed 16.6ms (60Hz) guideline */
                {
                    const float norm60 = 16.667f / max_ms;
                    const int gy = chart_y + chart_h - 2 - (int)(norm60 * (float)(chart_h - 4));
                    int gx;
                    if (gy > chart_y + 2 && gy < chart_y + chart_h - 2) {
                        for (gx = chart_x + 3; gx < chart_x + chart_w - 3; ++gx) {
                            if (((gx - chart_x) & 7) < 4) {
                                KITTY_CanvasBlendPixel(&canvas, gx, gy, 255, 255, 255, 32);
                            }
                        }
                    }
                }
                /* Secondary TX-time trace (amber, no fill) */
                KITTY_CanvasDrawSparkline(&canvas, chart_x + 2, chart_y + 2,
                                          chart_w - 4, chart_h - 4,
                                          smooth_tx, n, max_ms,
                                          hud->pal.tx_line, NULL, NULL, false);
                /* Primary frame-time curve (glowing cyan + vertical gradient area fill) */
                KITTY_CanvasDrawSparkline(&canvas, chart_x + 2, chart_y + 2,
                                          chart_w - 4, chart_h - 4,
                                          smooth_ft, n, max_ms,
                                          hud->pal.spark_line,
                                          hud->pal.spark_top, hud->pal.spark_bot, true);
            }
            {
                const char *leg = "60Hz";
                const float lpx = 9.5f * s;
                const float lw = KITTY_FontMeasureUTF8(leg, 4, lpx, true);
                KITTY_FontDrawUTF8(&canvas, (float)(chart_x + chart_w) - lw - 5.0f * s,
                                   (float)chart_y + 11.0f * s, leg, 4, lpx,
                                   hud->pal.muted, false, true);
            }
        }

        /* 4) Crisp labeled Telemetry Strip: TX <ms>  WIRE <KB>  DIRTY [bar] */
        {
            const float lbl_px = 9.5f * s;
            const float val_px = 11.8f * s;
            const float row_y = (float)cy + 86.5f * s;
            char tx_buf[20], wire_buf[24];
            float xcur = (float)inner_x;
            float dirty_t, dlw;
            int bar_w, bar_x;

            xcur = KITTY_FontDrawUTF8(&canvas, xcur, row_y, "TX", 2, lbl_px,
                                      hud->pal.muted, false, false);
            SDL_snprintf(tx_buf, sizeof(tx_buf), "%.2fms", hud->ewma_tx_ms);
            xcur = KITTY_FontDrawUTF8(&canvas, xcur + 3.5f * s, row_y, tx_buf, SDL_strlen(tx_buf),
                                      val_px, hud->pal.tx_line, true, true);

            xcur = KITTY_FontDrawUTF8(&canvas, xcur + 11.0f * s, row_y, "WIRE", 4, lbl_px,
                                      hud->pal.muted, false, false);
            if (hud->ewma_wire_kb >= 100.0) {
                SDL_snprintf(wire_buf, sizeof(wire_buf), "%.0fK", hud->ewma_wire_kb);
            } else {
                SDL_snprintf(wire_buf, sizeof(wire_buf), "%.1fK", hud->ewma_wire_kb);
            }
            (void)KITTY_FontDrawUTF8(&canvas, xcur + 3.5f * s, row_y, wire_buf, SDL_strlen(wire_buf),
                                     val_px, hud->pal.fg, true, true);

            /* Compact right-aligned DIRTY label + petite ~44px gradient meter */
            bar_w = (int)(44.0f * s + 0.5f);
            bar_x = (inner_x + inner_w) - bar_w;
            dlw = KITTY_FontMeasureUTF8("DIRTY", 5, lbl_px, false);
            KITTY_FontDrawUTF8(&canvas, (float)bar_x - dlw - 5.0f * s, row_y, "DIRTY", 5,
                               lbl_px, hud->pal.muted, false, false);
            dirty_t = (float)(hud->ewma_dirty_pct / 100.0);
            if (dirty_t < 0.0f) dirty_t = 0.0f;
            if (dirty_t > 1.0f) dirty_t = 1.0f;
            KITTY_CanvasDrawProgressBar(&canvas, bar_x, cy + (int)(79.0f * s + 0.5f),
                                        bar_w, (int)(6.5f * s + 0.5f),
                                        dirty_t, hud->pal.track, hud->pal.accent, hud->pal.accent2);
        }
    }

    margin_x = SDL_max(8, (int)(12.0f * s));
    margin_y = SDL_max(8, (int)(12.0f * s));
    switch (hud->pos) {
    case KITTY_HUD_TOP_LEFT:
        ox = margin_x;
        oy = margin_y;
        break;
    case KITTY_HUD_BOTTOM_RIGHT:
        ox = fb_w - bw - margin_x;
        oy = fb_h - bh - margin_y;
        break;
    case KITTY_HUD_BOTTOM_LEFT:
        ox = margin_x;
        oy = fb_h - bh - margin_y;
        break;
    case KITTY_HUD_TOP_RIGHT:
    default:
        ox = fb_w - bw - margin_x;
        oy = margin_y;
        break;
    }
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (ox + bw > fb_w) ox = fb_w - bw;
    if (oy + bh > fb_h) oy = fb_h - bh;

    hud->rect.x = ox;
    hud->rect.y = oy;
    hud->rect.w = bw;
    hud->rect.h = bh;
    hud->cache_fb_w = fb_w;
    hud->cache_fb_h = fb_h;
    return true;
}

bool KITTY_HudEvaluate(KITTY_PerfHud *hud, int fb_w, int fb_h, Uint64 now_ms,
                       SDL_Rect *out_rect, bool *out_needs_redraw)
{
    bool visible;
    bool rerender = false;

    if (out_needs_redraw) *out_needs_redraw = false;
    if (!hud || fb_w <= 0 || fb_h <= 0) {
        return false;
    }
    visible = hud->enabled || hud->rec_active;
    if (!visible) {
        if (hud->last_drawn_visible) {
            if (out_rect) *out_rect = hud->last_drawn_rect;
            if (out_needs_redraw) *out_needs_redraw = true;
        }
        return false;
    }

    if (hud->dirty_visual || !hud->rgba || hud->cache_fb_w != fb_w || hud->cache_fb_h != fb_h ||
        hud->last_render_ms == 0 || (now_ms - hud->last_render_ms) >= 90u) {
        if (!KITTY_HudRenderBitmap(hud, fb_w, fb_h, now_ms)) {
            return false;
        }
        hud->last_render_ms = now_ms;
        hud->dirty_visual = false;
        rerender = true;
    }

    if (out_rect) {
        *out_rect = hud->rect;
    }
    if (out_needs_redraw) {
        const bool rect_changed =
            (!hud->last_drawn_visible ||
             hud->last_drawn_rect.x != hud->rect.x || hud->last_drawn_rect.y != hud->rect.y ||
             hud->last_drawn_rect.w != hud->rect.w || hud->last_drawn_rect.h != hud->rect.h);
        *out_needs_redraw = rerender || rect_changed;
    }
    return true;
}

bool KITTY_HudApply(KITTY_PerfHud *hud, Uint8 *rgb_scratch, int fb_w, int fb_h, int bpp)
{
    size_t row_bytes, need;
    int y, x;

    if (!hud || !hud->rgba || !rgb_scratch || (bpp != 3 && bpp != 4)) {
        return false;
    }
    if (hud->rect.x < 0 || hud->rect.y < 0 ||
        hud->rect.x + hud->rect.w > fb_w || hud->rect.y + hud->rect.h > fb_h) {
        return false;
    }

    row_bytes = (size_t)hud->rect.w * (size_t)bpp;
    need = row_bytes * (size_t)hud->rect.h;
    if (hud->save_cap < need) {
        Uint8 *nb = (Uint8 *)SDL_realloc(hud->save_buf, need);
        if (!nb) {
            return false;
        }
        hud->save_buf = nb;
        hud->save_cap = need;
    }

    for (y = 0; y < hud->rect.h; ++y) {
        Uint8 *dst = rgb_scratch + ((size_t)(hud->rect.y + y) * (size_t)fb_w + (size_t)hud->rect.x) * (size_t)bpp;
        const Uint8 *src = hud->rgba + (size_t)y * (size_t)hud->rect.w * 4u;
        Uint8 *sav = hud->save_buf + (size_t)y * row_bytes;
        SDL_memcpy(sav, dst, row_bytes);

        for (x = 0; x < hud->rect.w; ++x) {
            const Uint32 sa = src[x * 4 + 3];
            if (sa == 0) {
                continue;
            }
            if (sa == 255u) {
                dst[x * bpp + 0] = src[x * 4 + 0];
                dst[x * bpp + 1] = src[x * 4 + 1];
                dst[x * bpp + 2] = src[x * 4 + 2];
                if (bpp == 4) dst[x * bpp + 3] = 255u;
            } else {
                const Uint32 inv = 255u - sa;
                dst[x * bpp + 0] = (Uint8)((src[x * 4 + 0] * sa + dst[x * bpp + 0] * inv + 127u) / 255u);
                dst[x * bpp + 1] = (Uint8)((src[x * 4 + 1] * sa + dst[x * bpp + 1] * inv + 127u) / 255u);
                dst[x * bpp + 2] = (Uint8)((src[x * 4 + 2] * sa + dst[x * bpp + 2] * inv + 127u) / 255u);
                if (bpp == 4) dst[x * bpp + 3] = 255u;
            }
        }
    }

    hud->save_rect = hud->rect;
    hud->saved = true;
    return true;
}

void KITTY_HudRestore(KITTY_PerfHud *hud, Uint8 *rgb_scratch, int fb_w, int fb_h, int bpp)
{
    size_t row_bytes;
    int y;

    if (!hud || !hud->saved || !hud->save_buf || !rgb_scratch || (bpp != 3 && bpp != 4)) {
        return;
    }
    hud->saved = false;
    if (hud->save_rect.x < 0 || hud->save_rect.y < 0 ||
        hud->save_rect.x + hud->save_rect.w > fb_w || hud->save_rect.y + hud->save_rect.h > fb_h) {
        return;
    }
    row_bytes = (size_t)hud->save_rect.w * (size_t)bpp;
    for (y = 0; y < hud->save_rect.h; ++y) {
        Uint8 *dst = rgb_scratch + ((size_t)(hud->save_rect.y + y) * (size_t)fb_w + (size_t)hud->save_rect.x) * (size_t)bpp;
        const Uint8 *sav = hud->save_buf + (size_t)y * row_bytes;
        SDL_memcpy(dst, sav, row_bytes);
    }
}

void KITTY_HudMarkDrawn(KITTY_PerfHud *hud, bool visible)
{
    if (!hud) {
        return;
    }
    hud->last_drawn_visible = visible;
    hud->last_drawn_rect = hud->rect;
}

#endif /* SDL_VIDEO_DRIVER_KITTY */
