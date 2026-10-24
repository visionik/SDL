/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  Load config/terminals.toml (tomlc17) into KITTY_TerminalProfile entries.
*/
#ifdef KITTY_PROFILE_STANDALONE
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#else
#include "SDL_internal.h"
#endif

#if defined(SDL_VIDEO_DRIVER_KITTY) || defined(KITTY_PROFILE_STANDALONE)

#include "SDL_kittyprofile.h"
#include "tomlc17.h"

#include <stdarg.h>
#include <stdio.h>

#ifndef KITTY_PROFILE_STANDALONE
#include "SDL_kittyprobe.h"
#endif

#define KITTY_MATCH_MAX 8
#define KITTY_PROFILE_MAX 32
#define KITTY_SOURCE_MAX 512

typedef struct KITTY_StringList
{
    char *items[KITTY_MATCH_MAX];
    int count;
} KITTY_StringList;

typedef struct KITTY_ProfileEntry
{
    KITTY_TerminalProfile profile;
    char name_storage[64];
    char *notes_heap; /* owned; profile.notes points here when set */
    char *force_transmit_heap; /* owned; profile.force_transmit points here */
    KITTY_StringList term_program;
    KITTY_StringList term_when_program_empty;
    KITTY_StringList term_substring;
    bool match_kitty_window_id;
    char *version_min; /* owned; match.term_program_version_min */
    char *version_max; /* owned; match.term_program_version_max */
    bool has_matchers;
} KITTY_ProfileEntry;

typedef struct KITTY_ParsedVersion
{
    int major;
    int minor;
    int patch;
    bool has_pre; /* true if a '-' pre-release follows the numeric core */
} KITTY_ParsedVersion;

/* From SDL_kittyprofile.c */
const KITTY_TerminalProfile *KITTY_BuiltinDefaultsProfile(void);

static KITTY_ProfileEntry g_entries[KITTY_PROFILE_MAX];
static int g_entry_count;
static bool g_load_attempted;
static bool g_have_loaded; /* true when at least one TOML file applied */
static char g_source[KITTY_SOURCE_MAX] = "builtin";

static void KITTY_ProfileDebug(const char *fmt, ...)
{
#ifdef SDL_KITTY_RUNTIME_DEBUG
    va_list ap;
    const char *dbg = SDL_getenv("SDL_KITTY_DEBUG");
    if (!dbg || !dbg[0] || dbg[0] == '0') {
        return;
    }
#ifndef KITTY_PROFILE_STANDALONE
    {
        char buf[512];
        va_start(ap, fmt);
        SDL_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        KITTY_DebugLog("%s", buf);
        return;
    }
#else
    va_start(ap, fmt);
    fprintf(stderr, "kitty profile: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
#endif
#else
    (void)fmt;
#endif /* SDL_KITTY_RUNTIME_DEBUG */
}

static void KITTY_FreeStringList(KITTY_StringList *list)
{
    int i;
    if (!list) {
        return;
    }
    for (i = 0; i < list->count; ++i) {
        SDL_free(list->items[i]);
        list->items[i] = NULL;
    }
    list->count = 0;
}

static void KITTY_ClearEntry(KITTY_ProfileEntry *e)
{
    if (!e) {
        return;
    }
    KITTY_FreeStringList(&e->term_program);
    KITTY_FreeStringList(&e->term_when_program_empty);
    KITTY_FreeStringList(&e->term_substring);
    SDL_free(e->notes_heap);
    e->notes_heap = NULL;
    SDL_free(e->force_transmit_heap);
    e->force_transmit_heap = NULL;
    SDL_free(e->version_min);
    e->version_min = NULL;
    SDL_free(e->version_max);
    e->version_max = NULL;
    SDL_zerop(e);
}

static void KITTY_ClearAllEntries(void)
{
    int i;
    for (i = 0; i < g_entry_count; ++i) {
        KITTY_ClearEntry(&g_entries[i]);
    }
    g_entry_count = 0;
    g_have_loaded = false;
    SDL_strlcpy(g_source, "builtin", sizeof(g_source));
}

static KITTY_TerminalKind KITTY_KindFromId(const char *id)
{
    if (!id || !id[0]) {
        return KITTY_TERM_UNKNOWN;
    }
    if (SDL_strcasecmp(id, "rex") == 0) {
        return KITTY_TERM_REX;
    }
    if (SDL_strcasecmp(id, "kitty") == 0 || SDL_strncasecmp(id, "kitty-", 6) == 0) {
        return KITTY_TERM_KITTY;
    }
    if (SDL_strcasecmp(id, "ghostty") == 0 || SDL_strncasecmp(id, "ghostty-", 8) == 0) {
        return KITTY_TERM_GHOSTTY;
    }
    if (SDL_strcasecmp(id, "wezterm") == 0 || SDL_strncasecmp(id, "wezterm-", 8) == 0) {
        return KITTY_TERM_WEZTERM;
    }
    return KITTY_TERM_UNKNOWN;
}

/* Parse leading major[.minor[.patch]] from TERM_PROGRAM_VERSION / match bounds.
 * Accepts prefixes like "ghostty 1.3.1" by skipping to the first digit.
 * Pre-release: "1.3.2-main+abc" → 1.3.2 has_pre=true. */
static bool KITTY_ParseVersion(const char *s, KITTY_ParsedVersion *out)
{
    const char *p;
    long major = 0, minor = 0, patch = 0;
    char *end = NULL;

    if (!s || !out) {
        return false;
    }
    SDL_zerop(out);
    p = s;
    while (*p && (*p < '0' || *p > '9')) {
        ++p;
    }
    if (!*p) {
        return false;
    }
    major = SDL_strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    p = end;
    if (*p == '.') {
        ++p;
        minor = SDL_strtol(p, &end, 10);
        if (end == p) {
            return false;
        }
        p = end;
        if (*p == '.') {
            ++p;
            patch = SDL_strtol(p, &end, 10);
            if (end == p) {
                return false;
            }
            p = end;
        }
    }
    out->major = (int)major;
    out->minor = (int)minor;
    out->patch = (int)patch;
    out->has_pre = (*p == '-');
    return true;
}

/* Semver-ish: major/minor/patch first; when equal, release (no pre) > pre-release. */
static int KITTY_CmpParsedVersion(const KITTY_ParsedVersion *a, const KITTY_ParsedVersion *b)
{
    if (a->major != b->major) {
        return (a->major < b->major) ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return (a->minor < b->minor) ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return (a->patch < b->patch) ? -1 : 1;
    }
    if (a->has_pre != b->has_pre) {
        return a->has_pre ? -1 : 1;
    }
    return 0;
}

static bool KITTY_VersionInRange(const char *actual,
                                 const char *min_s,
                                 const char *max_s)
{
    KITTY_ParsedVersion v, bound;

    if ((!min_s || !min_s[0]) && (!max_s || !max_s[0])) {
        return true; /* no version constraint */
    }
    if (!actual || !actual[0]) {
        return false; /* constrained row needs TERM_PROGRAM_VERSION */
    }
    if (!KITTY_ParseVersion(actual, &v)) {
        return false;
    }
    if (min_s && min_s[0]) {
        if (!KITTY_ParseVersion(min_s, &bound)) {
            return false;
        }
        if (KITTY_CmpParsedVersion(&v, &bound) < 0) {
            return false;
        }
    }
    if (max_s && max_s[0]) {
        if (!KITTY_ParseVersion(max_s, &bound)) {
            return false;
        }
        if (KITTY_CmpParsedVersion(&v, &bound) > 0) {
            return false;
        }
    }
    return true;
}

static bool KITTY_ParseWxH(const char *s, int *w, int *h)
{
    int aw = 0, ah = 0;
    if (!s || !w || !h) {
        return false;
    }
    if (SDL_sscanf(s, "%dx%d", &aw, &ah) != 2 && SDL_sscanf(s, "%dX%d", &aw, &ah) != 2) {
        return false;
    }
    if (aw <= 0 || ah <= 0) {
        return false;
    }
    *w = aw;
    *h = ah;
    return true;
}

static bool KITTY_TomlGetBool(toml_datum_t tab, const char *key, bool *out)
{
    toml_datum_t v = toml_get(tab, key);
    if (v.type != TOML_BOOLEAN) {
        return false;
    }
    *out = v.u.boolean;
    return true;
}

static bool KITTY_TomlGetInt(toml_datum_t tab, const char *key, int64_t *out)
{
    toml_datum_t v = toml_get(tab, key);
    if (v.type != TOML_INT64) {
        return false;
    }
    *out = v.u.int64;
    return true;
}

static bool KITTY_TomlGetFloat(toml_datum_t tab, const char *key, double *out)
{
    toml_datum_t v = toml_get(tab, key);
    if (v.type == TOML_FP64) {
        *out = v.u.fp64;
        return true;
    }
    if (v.type == TOML_INT64) {
        *out = (double)v.u.int64;
        return true;
    }
    return false;
}

static bool KITTY_TomlGetString(toml_datum_t tab, const char *key, const char **out)
{
    toml_datum_t v = toml_get(tab, key);
    if (v.type != TOML_STRING || !v.u.s) {
        return false;
    }
    *out = v.u.s;
    return true;
}

static bool KITTY_CopyStringListFromToml(KITTY_StringList *out, toml_datum_t arr)
{
    int i;
    KITTY_FreeStringList(out);
    if (arr.type != TOML_ARRAY) {
        return false;
    }
    for (i = 0; i < arr.u.arr.size && out->count < KITTY_MATCH_MAX; ++i) {
        toml_datum_t el = arr.u.arr.elem[i];
        char *dup;
        if (el.type != TOML_STRING || !el.u.s) {
            continue;
        }
        dup = SDL_strdup(el.u.s);
        if (!dup) {
            KITTY_FreeStringList(out);
            return false;
        }
        out->items[out->count++] = dup;
    }
    return out->count > 0;
}

static void KITTY_OverlayProfileFromTable(KITTY_TerminalProfile *p, toml_datum_t tab,
                                          KITTY_ProfileEntry *entry)
{
    bool b;
    int64_t i64;
    double fp;
    const char *s;

    if (KITTY_TomlGetBool(tab, "prefer_direct", &b)) {
        p->prefer_direct = b;
    }
    if (KITTY_TomlGetBool(tab, "prefer_zlib", &b)) {
        p->prefer_zlib = b;
    }
    if (entry && KITTY_TomlGetString(tab, "force_transmit", &s)) {
        SDL_free(entry->force_transmit_heap);
        entry->force_transmit_heap = NULL;
        p->force_transmit = NULL;
        if (s && *s && SDL_strcasecmp(s, "off") != 0 && SDL_strcasecmp(s, "none") != 0) {
            entry->force_transmit_heap = SDL_strdup(s);
            p->force_transmit = entry->force_transmit_heap;
        }
    }
    if (KITTY_TomlGetString(tab, "default_fb_max", &s)) {
        int w = 0, h = 0;
        if (KITTY_ParseWxH(s, &w, &h)) {
            p->default_fb_max_w = w;
            p->default_fb_max_h = h;
        } else if (SDL_strcmp(s, "0") == 0 || SDL_strcasecmp(s, "off") == 0 ||
                   SDL_strcasecmp(s, "none") == 0 || s[0] == '\0') {
            p->default_fb_max_w = 0;
            p->default_fb_max_h = 0;
        }
    }
    if (KITTY_TomlGetInt(tab, "max_direct_bytes", &i64) && i64 > 0) {
        p->max_direct_bytes = (size_t)i64;
    }
    if (KITTY_TomlGetFloat(tab, "af_max_coverage", &fp)) {
        p->af_max_coverage = (float)fp;
    }
    if (KITTY_TomlGetInt(tab, "af_max_pixels", &i64) && i64 > 0) {
        p->af_max_pixels = (size_t)i64;
    }
    if (KITTY_TomlGetBool(tab, "keys_on_stdin", &b)) {
        p->keys_on_stdin = b;
    }
    if (KITTY_TomlGetBool(tab, "silent_af_ok", &b)) {
        p->silent_af_ok = b;
    }
    if (KITTY_TomlGetBool(tab, "ignore_ioctl_pixels", &b)) {
        p->ignore_ioctl_pixels = b;
    }
    if (KITTY_TomlGetBool(tab, "direct_af_when_shm", &b)) {
        p->direct_af_when_shm = b;
    }
    if (KITTY_TomlGetBool(tab, "dirty_at_overlays", &b)) {
        p->dirty_at_overlays = b;
    }
    if (KITTY_TomlGetBool(tab, "dirty_union_at", &b)) {
        p->dirty_union_at = b;
    }
    if (KITTY_TomlGetBool(tab, "upload_skip", &b)) {
        p->upload_skip = b;
    }
    if (KITTY_TomlGetInt(tab, "coalesce_gap_px", &i64) && i64 >= 0 && i64 <= 256) {
        p->coalesce_gap_px = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "direct_fallback_max_bytes", &i64) && i64 > 0) {
        p->direct_fallback_max_bytes = (size_t)i64;
    }
    if (KITTY_TomlGetInt(tab, "apc_chunk_bytes", &i64) && i64 > 0) {
        p->apc_chunk_bytes = (size_t)i64;
    }
    if (KITTY_TomlGetInt(tab, "probe_timeout_ms", &i64) && i64 > 0) {
        p->probe_timeout_ms = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "drain_passes", &i64) && i64 > 0) {
        p->drain_passes = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "drain_quiet_us", &i64) && i64 > 0) {
        p->drain_quiet_us = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "stub_winch_iters", &i64) && i64 > 0) {
        p->stub_winch_iters = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "stub_winch_delay_ms", &i64) && i64 > 0) {
        p->stub_winch_delay_ms = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "stub_cols", &i64) && i64 > 0) {
        p->stub_cols = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "stub_rows", &i64) && i64 > 0) {
        p->stub_rows = (int)i64;
    }
    if (KITTY_TomlGetInt(tab, "key_hold_ms", &i64) && i64 > 0) {
        p->key_hold_ms = (int)i64;
    }
    if (KITTY_TomlGetBool(tab, "fb_diff", &b)) {
        p->fb_diff = b;
    }
    if (KITTY_TomlGetInt(tab, "fb_diff_tile", &i64) && i64 > 0) {
        p->fb_diff_tile = (int)i64;
    }
    if (KITTY_TomlGetFloat(tab, "fb_diff_cover", &fp)) {
        p->fb_diff_cover = (float)fp;
    }
    if (KITTY_TomlGetInt(tab, "tty_pending_max", &i64) && i64 > 0) {
        p->tty_pending_max = (int)i64;
    }
    if (KITTY_TomlGetString(tab, "window_fallback", &s)) {
        int w = 0, h = 0;
        if (KITTY_ParseWxH(s, &w, &h)) {
            p->window_fallback_w = w;
            p->window_fallback_h = h;
        }
    }
    if (KITTY_TomlGetBool(tab, "place_cells", &b)) {
        p->place_cells = b;
    }
    if (KITTY_TomlGetBool(tab, "pingpong", &b)) {
        p->pingpong = b;
    }
    if (KITTY_TomlGetBool(tab, "shm_unlink_after_send", &b)) {
        p->shm_unlink_after_send = b;
    }
    if (entry && KITTY_TomlGetString(tab, "notes", &s)) {
        SDL_free(entry->notes_heap);
        entry->notes_heap = SDL_strdup(s);
        p->notes = entry->notes_heap;
    }
}

static void KITTY_OverlayMatchers(KITTY_ProfileEntry *entry, toml_datum_t tab)
{
    toml_datum_t a;

    a = toml_seek(tab, "match.term_program");
    if (a.type == TOML_ARRAY) {
        if (KITTY_CopyStringListFromToml(&entry->term_program, a)) {
            entry->has_matchers = true;
        }
    }
    a = toml_seek(tab, "match.term_when_program_empty");
    if (a.type == TOML_ARRAY) {
        if (KITTY_CopyStringListFromToml(&entry->term_when_program_empty, a)) {
            entry->has_matchers = true;
        }
    }
    a = toml_seek(tab, "match.term_substring");
    if (a.type == TOML_ARRAY) {
        if (KITTY_CopyStringListFromToml(&entry->term_substring, a)) {
            entry->has_matchers = true;
        }
    }
    a = toml_seek(tab, "match.kitty_window_id");
    if (a.type == TOML_BOOLEAN) {
        entry->match_kitty_window_id = a.u.boolean;
        if (a.u.boolean) {
            entry->has_matchers = true;
        }
    }
    a = toml_seek(tab, "match.term_program_version_min");
    if (a.type == TOML_STRING && a.u.s && a.u.s[0]) {
        SDL_free(entry->version_min);
        entry->version_min = SDL_strdup(a.u.s);
        entry->has_matchers = true;
    }
    a = toml_seek(tab, "match.term_program_version_max");
    if (a.type == TOML_STRING && a.u.s && a.u.s[0]) {
        SDL_free(entry->version_max);
        entry->version_max = SDL_strdup(a.u.s);
        entry->has_matchers = true;
    }
}

static bool KITTY_ListContainsCI(const KITTY_StringList *list, const char *s)
{
    int i;
    if (!list || !s) {
        return false;
    }
    for (i = 0; i < list->count; ++i) {
        if (list->items[i] && SDL_strcasecmp(list->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}

static bool KITTY_ListSubstring(const KITTY_StringList *list, const char *s)
{
    int i;
    if (!list || !s) {
        return false;
    }
    for (i = 0; i < list->count; ++i) {
        if (list->items[i] && SDL_strstr(s, list->items[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static bool KITTY_EntryMatches(const KITTY_ProfileEntry *e, const char *tp, const char *term)
{
    bool identity = false;
    const char *tpv;

    if (!e || !e->has_matchers) {
        return false;
    }
    if (tp && *tp && KITTY_ListContainsCI(&e->term_program, tp)) {
        identity = true;
    } else if ((!tp || !*tp) && term && *term &&
               KITTY_ListContainsCI(&e->term_when_program_empty, term)) {
        identity = true;
    } else if (term && *term && KITTY_ListSubstring(&e->term_substring, term)) {
        identity = true;
    } else if (e->match_kitty_window_id && SDL_getenv("KITTY_WINDOW_ID") != NULL) {
        identity = true;
    }
    if (!identity) {
        return false;
    }
    /* Version bounds AND with identity. Missing env fails a constrained row. */
    tpv = SDL_getenv("TERM_PROGRAM_VERSION");
    return KITTY_VersionInRange(tpv, e->version_min, e->version_max);
}

static KITTY_ProfileEntry *KITTY_FindEntryByName(const char *name)
{
    int i;
    if (!name) {
        return NULL;
    }
    for (i = 0; i < g_entry_count; ++i) {
        if (SDL_strcasecmp(g_entries[i].name_storage, name) == 0) {
            return &g_entries[i];
        }
    }
    return NULL;
}

static KITTY_ProfileEntry *KITTY_AddEntryFromDefaults(const KITTY_TerminalProfile *defaults,
                                                     const char *id)
{
    KITTY_ProfileEntry *e;

    if (g_entry_count >= KITTY_PROFILE_MAX || !id || !id[0]) {
        return NULL;
    }
    e = &g_entries[g_entry_count++];
    SDL_zerop(e);
    /* Resolve: copy [defaults], then [[terminal]] overlays set keys only. */
    e->profile = defaults ? *defaults : *KITTY_BuiltinDefaultsProfile();
    e->profile.kind = KITTY_KindFromId(id);
    SDL_strlcpy(e->name_storage, id, sizeof(e->name_storage));
    e->profile.name = e->name_storage;
    e->profile.notes = NULL;
    return e;
}

static bool KITTY_ApplyTomlResult(toml_result_t result, bool replace)
{
    toml_datum_t defaults_tab;
    KITTY_TerminalProfile defaults_profile;
    toml_datum_t terminals;
    int i;

    if (!result.ok) {
        return false;
    }

    defaults_profile = *KITTY_BuiltinDefaultsProfile();
    defaults_tab = toml_get(result.toptab, "defaults");
    if (defaults_tab.type == TOML_TABLE) {
        KITTY_OverlayProfileFromTable(&defaults_profile, defaults_tab, NULL);
    }

    if (replace) {
        KITTY_ClearAllEntries();
    }

    terminals = toml_get(result.toptab, "terminal");
    if (terminals.type != TOML_ARRAY) {
        /* No [[terminal]] rows — keep defaults only as "unknown". */
        KITTY_ProfileEntry *unk = KITTY_FindEntryByName("unknown");
        if (!unk) {
            unk = KITTY_AddEntryFromDefaults(&defaults_profile, "unknown");
        } else {
            unk->profile = defaults_profile;
            unk->profile.name = unk->name_storage;
            unk->profile.kind = KITTY_TERM_UNKNOWN;
        }
        if (unk) {
            unk->has_matchers = false;
        }
        g_have_loaded = true;
        return true;
    }

    for (i = 0; i < terminals.u.arr.size; ++i) {
        toml_datum_t tab = terminals.u.arr.elem[i];
        const char *id = NULL;
        KITTY_ProfileEntry *e;
        bool is_new = false;

        if (tab.type != TOML_TABLE) {
            continue;
        }
        if (!KITTY_TomlGetString(tab, "id", &id) || !id || !id[0]) {
            KITTY_ProfileDebug("kitty: profiles skip [[terminal]] without id");
            continue;
        }

        e = KITTY_FindEntryByName(id);
        if (!e) {
            e = KITTY_AddEntryFromDefaults(&defaults_profile, id);
            is_new = true;
            if (!e) {
                KITTY_ProfileDebug("kitty: profiles table full, skip id=%s", id);
                continue;
            }
        }

        /* Sparse overlay of keys set on this [[terminal]] row. */
        KITTY_OverlayProfileFromTable(&e->profile, tab, e);
        e->profile.name = e->name_storage;
        e->profile.kind = KITTY_KindFromId(id);

        /* Replace matchers when this row defines any match.* key. */
        {
            const bool any_match_key =
                (toml_seek(tab, "match.term_program").type == TOML_ARRAY) ||
                (toml_seek(tab, "match.term_when_program_empty").type == TOML_ARRAY) ||
                (toml_seek(tab, "match.term_substring").type == TOML_ARRAY) ||
                (toml_seek(tab, "match.kitty_window_id").type == TOML_BOOLEAN) ||
                (toml_seek(tab, "match.term_program_version_min").type == TOML_STRING) ||
                (toml_seek(tab, "match.term_program_version_max").type == TOML_STRING);
            if (is_new || any_match_key) {
                if (any_match_key) {
                    KITTY_FreeStringList(&e->term_program);
                    KITTY_FreeStringList(&e->term_when_program_empty);
                    KITTY_FreeStringList(&e->term_substring);
                    e->match_kitty_window_id = false;
                    SDL_free(e->version_min);
                    e->version_min = NULL;
                    SDL_free(e->version_max);
                    e->version_max = NULL;
                    e->has_matchers = false;
                    KITTY_OverlayMatchers(e, tab);
                }
            }
        }

        /* unknown is the explicit no-matcher fallback. */
        if (SDL_strcasecmp(id, "unknown") == 0) {
            e->has_matchers = false;
        }
    }

    g_have_loaded = g_entry_count > 0;
    return g_have_loaded;
}

static bool KITTY_LoadProfilesFile(const char *path, bool replace)
{
    toml_result_t result;
    bool ok;

    if (!path || !path[0]) {
        return false;
    }

    result = toml_parse_file_ex(path);
    if (!result.ok) {
        KITTY_ProfileDebug("kitty: profiles parse failed path=%s err=%s", path,
                           result.errmsg);
        toml_free(result);
        return false;
    }

    ok = KITTY_ApplyTomlResult(result, replace);
    toml_free(result);
    if (ok) {
        if (g_source[0] && SDL_strcmp(g_source, "builtin") != 0) {
            char next[KITTY_SOURCE_MAX];
            SDL_snprintf(next, sizeof(next), "%s;%s", g_source, path);
            SDL_strlcpy(g_source, next, sizeof(g_source));
        } else {
            SDL_strlcpy(g_source, path, sizeof(g_source));
        }
        KITTY_ProfileDebug("kitty: profiles loaded path=%s entries=%d", path, g_entry_count);
    }
    return ok;
}

static void KITTY_AppendPathIfExists(char **paths, int *n, int cap, const char *path)
{
    FILE *fp;
    if (!path || !path[0] || *n >= cap) {
        return;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    fclose(fp);
    paths[*n] = SDL_strdup(path);
    if (paths[*n]) {
        (*n)++;
    }
}

static void KITTY_CollectSearchPaths(char **paths, int *n, int cap)
{
    const char *env = SDL_getenv("SDL_KITTY_PROFILES");
    char *pref = NULL;
    const char *base = NULL;

    *n = 0;

    if (env && *env) {
        /* Colon-separated list (same idea as PATH). */
        char *copy = SDL_strdup(env);
        char *cursor;
        char *tok;
        if (!copy) {
            return;
        }
        cursor = copy;
        while (cursor && *n < cap) {
            tok = cursor;
            while (*cursor && *cursor != ':') {
                cursor++;
            }
            if (*cursor == ':') {
                *cursor++ = '\0';
            } else {
                cursor = NULL;
            }
            if (tok[0]) {
                KITTY_AppendPathIfExists(paths, n, cap, tok);
            }
        }
        SDL_free(copy);
        return; /* explicit list wins; do not also auto-search */
    }

#ifndef KITTY_PROFILE_STANDALONE
    pref = SDL_GetPrefPath("sdl-kitty", "kitty");
    if (pref) {
        char buf[1024];
        SDL_snprintf(buf, sizeof(buf), "%sterminals.toml", pref);
        KITTY_AppendPathIfExists(paths, n, cap, buf);
        SDL_free(pref);
    }
    base = SDL_GetBasePath();
    if (base) {
        char buf[1024];
        SDL_snprintf(buf, sizeof(buf), "%skitty/terminals.toml", base);
        KITTY_AppendPathIfExists(paths, n, cap, buf);
        SDL_snprintf(buf, sizeof(buf), "%sterminals.toml", base);
        KITTY_AppendPathIfExists(paths, n, cap, buf);
    }
#endif

#ifdef KITTY_DEFAULT_PROFILES_PATH
    KITTY_AppendPathIfExists(paths, n, cap, KITTY_DEFAULT_PROFILES_PATH);
#endif
}

void KITTY_ResetTerminalProfilesToBuiltin(void)
{
    KITTY_ClearAllEntries();
    g_load_attempted = false;
    g_have_loaded = false;
}

bool KITTY_LoadTerminalProfiles(const char *path)
{
    bool ok;
    /* First successful explicit load replaces; subsequent overlay. */
    ok = KITTY_LoadProfilesFile(path, !g_have_loaded);
    if (ok) {
        g_load_attempted = true;
    }
    return ok;
}

void KITTY_EnsureTerminalProfilesLoaded(void)
{
    char *paths[8];
    int n = 0;
    int i;
    toml_option_t opt;

    if (g_load_attempted) {
        return;
    }
    g_load_attempted = true;

    /* Prefer SDL allocators for tomlc17 heap. */
    opt = toml_default_option();
    opt.mem_realloc = SDL_realloc;
    opt.mem_free = SDL_free;
    toml_set_option(opt);

    for (i = 0; i < 8; ++i) {
        paths[i] = NULL;
    }
    KITTY_CollectSearchPaths(paths, &n, 8);

    for (i = 0; i < n; ++i) {
        const bool replace = !g_have_loaded;
        (void)KITTY_LoadProfilesFile(paths[i], replace);
        SDL_free(paths[i]);
        paths[i] = NULL;
    }

    if (!g_have_loaded) {
        KITTY_ProfileDebug("kitty: profiles=builtin (no TOML found)");
    }
}

const KITTY_TerminalProfile *KITTY_DetectLoadedTerminalProfile(void)
{
    const char *tp;
    const char *term;
    int i;

    if (!g_have_loaded || g_entry_count <= 0) {
        return NULL;
    }

    tp = SDL_getenv("TERM_PROGRAM");
    term = SDL_getenv("TERM");

    for (i = 0; i < g_entry_count; ++i) {
        if (KITTY_EntryMatches(&g_entries[i], tp, term)) {
            return &g_entries[i].profile;
        }
    }
    for (i = 0; i < g_entry_count; ++i) {
        if (!g_entries[i].has_matchers &&
            SDL_strcasecmp(g_entries[i].name_storage, "unknown") == 0) {
            return &g_entries[i].profile;
        }
    }
    /* Last resort among loaded: first entry without matchers, else first. */
    for (i = 0; i < g_entry_count; ++i) {
        if (!g_entries[i].has_matchers) {
            return &g_entries[i].profile;
        }
    }
    return &g_entries[0].profile;
}

const char *KITTY_TerminalProfilesSource(void)
{
    return g_source[0] ? g_source : "builtin";
}

bool KITTY_EnvMatchesLoadedProfile(void)
{
    const char *tp;
    const char *term;
    int i;

    KITTY_EnsureTerminalProfilesLoaded();
    if (!g_have_loaded || g_entry_count <= 0) {
        return false;
    }
    tp = SDL_getenv("TERM_PROGRAM");
    term = SDL_getenv("TERM");
    for (i = 0; i < g_entry_count; ++i) {
        if (KITTY_EntryMatches(&g_entries[i], tp, term)) {
            return true;
        }
    }
    return false;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROFILE_STANDALONE */
