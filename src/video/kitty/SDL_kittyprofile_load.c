/*
  Simple DirectMedia Layer
  Copyright (C) 2026 Visionik <visionik@pobox.com>
*/

/*
 * Terminal profiles from two TOML files (tomlc17):
 *
 *  - terminals.toml, the shared facts (terminal-facts SPEC.md, schema 1):
 *    recognise the terminal (XTVERSION first, then the environment), then
 *    stack every extending row whose conditions hold.
 *  - sdl-kitty.toml, this driver's tuning: [defaults], then
 *    [terminal.<id>] for each row applied, or [terminal.unknown].
 *
 * Resolving: seed (SDL_kittyprofile.c) -> tuning -> facts that decide a
 * setting. SDL_KITTY_* environment variables are read at their call sites
 * and win over all of these.
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
#include "SDL_kittyprofile_data.h"
#include "tomlc17.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#ifndef KITTY_PROFILE_STANDALONE
#include "SDL_kittyprobe.h"
#endif

#define KITTY_LIST_MAX 8
#define KITTY_TEXT_MAX 64
#define KITTY_ROWS_MAX 48
#define KITTY_TUNING_MAX 8
#define KITTY_SOURCE_MAX 1024
#define KITTY_VERSION_PARTS 6
#define KITTY_APC_HEADROOM 256 /* escape-code header and ST around a chunk */

typedef struct KITTY_StringList
{
    char items[KITTY_LIST_MAX][KITTY_TEXT_MAX];
    int count;
} KITTY_StringList;

enum
{
    KITTY_OS_MACOS = 1,
    KITTY_OS_LINUX = 2,
    KITTY_OS_WINDOWS = 4,
    KITTY_OS_BSD = 8
};

/* One [[terminal]] row of terminals.toml. */
typedef struct KITTY_FactRow
{
    char id[KITTY_TEXT_MAX];
    char name[KITTY_TEXT_MAX];
    int extends; /* index of an earlier row; -1 for a base row */
    KITTY_StringList xtversion;
    KITTY_StringList env;
    KITTY_StringList term_program;
    KITTY_StringList term;
    KITTY_StringList term_substring;
    char version_min[KITTY_TEXT_MAX]; /* "" = no bound */
    char version_max[KITTY_TEXT_MAX];
    unsigned os_mask; /* 0 = any OS */
    KITTY_TerminalFacts facts;
} KITTY_FactRow;

typedef struct KITTY_FactTable
{
    KITTY_FactRow rows[KITTY_ROWS_MAX];
    int count;
} KITTY_FactTable;

/* From SDL_kittyprofile.c */
const KITTY_TerminalProfile *KITTY_BuiltinDefaultsProfile(void);

static KITTY_FactTable *g_facts;                 /* heap; NULL until loaded */
static toml_result_t g_tuning[KITTY_TUNING_MAX]; /* kept: profiles point into them */
static int g_tuning_count;
static bool g_loaded;
static char g_source[KITTY_SOURCE_MAX] = "builtin";

/* The resolved profile and the storage its strings point into. */
static KITTY_TerminalProfile g_profile;
static char g_profile_name[KITTY_TEXT_MAX];
static char g_profile_rows[KITTY_TEXT_MAX * 4];
static char g_profile_version[KITTY_TEXT_MAX];

/*
 * Debug log for profile load/match when SDL_KITTY_RUNTIME_DEBUG is built and
 * SDL_KITTY_DEBUG is set. Uses KITTY_DebugLog non-standalone; else stderr.
 */
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

/* tomlc17 on SDL's allocators, once. */
static void KITTY_TomlInit(void)
{
    static bool done;
    toml_option_t opt;
    if (done) {
        return;
    }
    done = true;
    opt = toml_default_option();
    opt.mem_realloc = SDL_realloc;
    opt.mem_free = SDL_free;
    toml_set_option(opt);
}

/* ---- Versions and XTVERSION (SPEC.md: Comparing versions) ---- */

typedef struct KITTY_VersionKey
{
    long parts[KITTY_VERSION_PARTS];
    int released; /* 0 when a '-suffix' follows the numbers */
} KITTY_VersionKey;

/*
 * The first run of dotted numbers in s (after any non-digit prefix), padded
 * with zeros; a '-' straight after it sorts below the same numbers. False if
 * s has no digits.
 */
static bool KITTY_VersionKeyOf(const char *s, KITTY_VersionKey *out)
{
    const char *p = s;
    int n = 0;

    SDL_zerop(out);
    if (!s) {
        return false;
    }
    while (*p && (*p < '0' || *p > '9')) {
        ++p;
    }
    if (!*p) {
        return false;
    }
    for (;;) {
        long v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            ++p;
        }
        if (n < KITTY_VERSION_PARTS) {
            out->parts[n++] = v;
        }
        if (p[0] == '.' && p[1] >= '0' && p[1] <= '9') {
            ++p;
            continue;
        }
        break;
    }
    out->released = (*p == '-') ? 0 : 1;
    return true;
}

static int KITTY_CompareVersionKeys(const KITTY_VersionKey *a, const KITTY_VersionKey *b)
{
    int i;
    for (i = 0; i < KITTY_VERSION_PARTS; ++i) {
        if (a->parts[i] != b->parts[i]) {
            return (a->parts[i] < b->parts[i]) ? -1 : 1;
        }
    }
    if (a->released != b->released) {
        return (a->released < b->released) ? -1 : 1;
    }
    return 0;
}

/*
 * Split an XTVERSION answer into a lower-case name (first word, up to a space
 * or '(') and the version after it: "kitty(0.50.0)" -> kitty, 0.50.0;
 * "WezTerm 20240203-110809-5046fc22" -> wezterm, 20240203-110809-5046fc22.
 */
static void KITTY_SplitXtversion(const char *text, char *name, size_t name_cap,
                                 char *version, size_t version_cap)
{
    const char *p = text;
    size_t n = 0;

    name[0] = '\0';
    version[0] = '\0';
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    while (*p && *p != ' ' && *p != '\t' && *p != '(') {
        if (n + 1 < name_cap) {
            name[n++] = (char)SDL_tolower((unsigned char)*p);
        }
        ++p;
    }
    name[n] = '\0';
    while (*p == ' ' || *p == '\t' || *p == '(') {
        ++p;
    }
    n = 0;
    while (*p && *p != ')') {
        if (n + 1 < version_cap) {
            version[n++] = *p;
        }
        ++p;
    }
    while (n > 0 && (version[n - 1] == ' ' || version[n - 1] == '\t')) {
        --n;
    }
    version[n] = '\0';
}

static unsigned KITTY_CurrentOS(void)
{
#if defined(__APPLE__)
    return KITTY_OS_MACOS;
#elif defined(__linux__)
    return KITTY_OS_LINUX;
#elif defined(_WIN32) || defined(__CYGWIN__)
    return KITTY_OS_WINDOWS;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    return KITTY_OS_BSD;
#else
    return 0;
#endif
}

static unsigned KITTY_OSFromName(const char *name)
{
    if (!name) {
        return 0;
    }
    if (SDL_strcmp(name, "macos") == 0) {
        return KITTY_OS_MACOS;
    }
    if (SDL_strcmp(name, "linux") == 0) {
        return KITTY_OS_LINUX;
    }
    if (SDL_strcmp(name, "windows") == 0) {
        return KITTY_OS_WINDOWS;
    }
    if (SDL_strcmp(name, "bsd") == 0) {
        return KITTY_OS_BSD;
    }
    return 0;
}

/* ---- Reading terminals.toml (SPEC.md: Layout, Row keys, Facts) ---- */

typedef enum KITTY_FactType
{
    KITTY_FACT_BOOL,
    KITTY_FACT_SIZE,
    KITTY_FACT_FORMAT,
    KITTY_FACT_TEXT
} KITTY_FactType;

typedef struct KITTY_FactKey
{
    const char *key;
    KITTY_FactType type;
    size_t offset;
    size_t cap; /* text facts */
} KITTY_FactKey;

/* Every fact in schema 1 (terminal-facts FACTS). */
static const KITTY_FactKey KITTY_fact_keys[] = {
    { "kitty.placeholders", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, placeholders), 0 },
    { "kitty.frame_edit", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, frame_edit), 0 },
    { "kitty.frame_edit_reply", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, frame_edit_reply), 0 },
    { "kitty.frame_edit_zlib", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, frame_edit_zlib), 0 },
    { "kitty.animation", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, animation), 0 },
    { "kitty.shm", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, shm), 0 },
    { "kitty.max_escape_bytes", KITTY_FACT_SIZE, offsetof(KITTY_TerminalFacts, max_escape_bytes), 0 },
    { "kitty.storage_bytes", KITTY_FACT_SIZE, offsetof(KITTY_TerminalFacts, storage_bytes), 0 },
    { "kitty.fastest_format", KITTY_FACT_FORMAT, offsetof(KITTY_TerminalFacts, fastest_format),
      sizeof(((KITTY_TerminalFacts *)0)->fastest_format) },
    { "iterm2.gif_animation", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, iterm2_gif_animation), 0 },
    { "window.ioctl_pixels", KITTY_FACT_BOOL, offsetof(KITTY_TerminalFacts, ioctl_pixels), 0 },
    { "keyboard.kitty_protocol_config", KITTY_FACT_TEXT, offsetof(KITTY_TerminalFacts, keyboard_config),
      sizeof(((KITTY_TerminalFacts *)0)->keyboard_config) },
};

static void KITTY_FactsUnknown(KITTY_TerminalFacts *f)
{
    SDL_zerop(f);
    f->placeholders = KITTY_FACT_UNKNOWN;
    f->frame_edit = KITTY_FACT_UNKNOWN;
    f->frame_edit_reply = KITTY_FACT_UNKNOWN;
    f->frame_edit_zlib = KITTY_FACT_UNKNOWN;
    f->animation = KITTY_FACT_UNKNOWN;
    f->shm = KITTY_FACT_UNKNOWN;
    f->iterm2_gif_animation = KITTY_FACT_UNKNOWN;
    f->ioctl_pixels = KITTY_FACT_UNKNOWN;
}

/* Copy the facts src knows onto dst (a later row wins). */
static void KITTY_FactsOverlay(KITTY_TerminalFacts *dst, const KITTY_TerminalFacts *src)
{
    size_t i;
    for (i = 0; i < SDL_arraysize(KITTY_fact_keys); ++i) {
        const KITTY_FactKey *k = &KITTY_fact_keys[i];
        const char *from = (const char *)src + k->offset;
        char *to = (char *)dst + k->offset;
        switch (k->type) {
        case KITTY_FACT_BOOL:
            if (*(const KITTY_Fact *)from != KITTY_FACT_UNKNOWN) {
                *(KITTY_Fact *)to = *(const KITTY_Fact *)from;
            }
            break;
        case KITTY_FACT_SIZE:
            if (*(const size_t *)from > 0) {
                *(size_t *)to = *(const size_t *)from;
            }
            break;
        case KITTY_FACT_FORMAT:
        case KITTY_FACT_TEXT:
            if (from[0]) {
                SDL_strlcpy(to, from, k->cap);
            }
            break;
        }
    }
}

static bool KITTY_Fail(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return false;
}

/*
 * Read the facts in a row table (dotted keys such as kitty.frame_edit, as
 * nested tables), skipping the row keys at the top. Unknown keys and wrong
 * types are errors, so a typo can't become an unknown fact.
 */
static bool KITTY_ReadFacts(toml_datum_t tab, const char *prefix, const char *where,
                            KITTY_TerminalFacts *facts, char *err, size_t cap)
{
    int i;
    for (i = 0; i < tab.u.tab.size; ++i) {
        const char *key = tab.u.tab.key[i];
        const toml_datum_t v = tab.u.tab.value[i];
        char dotted[128];
        size_t f;
        const KITTY_FactKey *k = NULL;

        if (!prefix[0] && (SDL_strcmp(key, "id") == 0 || SDL_strcmp(key, "name") == 0 ||
                           SDL_strcmp(key, "extends") == 0 || SDL_strcmp(key, "match") == 0 ||
                           SDL_strcmp(key, "evidence") == 0 || SDL_strcmp(key, "notes") == 0)) {
            continue;
        }
        SDL_snprintf(dotted, sizeof(dotted), "%s%s", prefix, key);
        if (v.type == TOML_TABLE) {
            char next[128];
            SDL_snprintf(next, sizeof(next), "%s.", dotted);
            if (!KITTY_ReadFacts(v, next, where, facts, err, cap)) {
                return false;
            }
            continue;
        }
        for (f = 0; f < SDL_arraysize(KITTY_fact_keys); ++f) {
            if (SDL_strcmp(KITTY_fact_keys[f].key, dotted) == 0) {
                k = &KITTY_fact_keys[f];
                break;
            }
        }
        if (!k) {
            return KITTY_Fail(err, cap, "%s: unknown fact '%s'", where, dotted);
        }
        switch (k->type) {
        case KITTY_FACT_BOOL:
            if (v.type != TOML_BOOLEAN) {
                return KITTY_Fail(err, cap, "%s: %s isn't bool", where, dotted);
            }
            *(KITTY_Fact *)((char *)facts + k->offset) = v.u.boolean ? KITTY_FACT_YES : KITTY_FACT_NO;
            break;
        case KITTY_FACT_SIZE:
            if (v.type != TOML_INT64 || v.u.int64 <= 0) {
                return KITTY_Fail(err, cap, "%s: %s isn't a positive int", where, dotted);
            }
            *(size_t *)((char *)facts + k->offset) = (size_t)v.u.int64;
            break;
        case KITTY_FACT_FORMAT:
            if (v.type != TOML_STRING || (SDL_strcmp(v.u.s, "zlib") != 0 &&
                                          SDL_strcmp(v.u.s, "png") != 0 &&
                                          SDL_strcmp(v.u.s, "raw") != 0)) {
                return KITTY_Fail(err, cap, "%s: %s isn't zlib, png or raw", where, dotted);
            }
            SDL_strlcpy((char *)facts + k->offset, v.u.s, k->cap);
            break;
        case KITTY_FACT_TEXT:
            if (v.type != TOML_STRING) {
                return KITTY_Fail(err, cap, "%s: %s isn't a string", where, dotted);
            }
            SDL_strlcpy((char *)facts + k->offset, v.u.s, k->cap);
            break;
        }
    }
    return true;
}

static bool KITTY_ReadList(toml_datum_t match, const char *key, const char *where,
                           KITTY_StringList *out, bool *present, char *err, size_t cap)
{
    const toml_datum_t arr = toml_get(match, key);
    int i;

    out->count = 0;
    if (arr.type == TOML_UNKNOWN) {
        return true;
    }
    if (arr.type != TOML_ARRAY) {
        return KITTY_Fail(err, cap, "%s: match.%s must be a list of strings", where, key);
    }
    for (i = 0; i < arr.u.arr.size; ++i) {
        const toml_datum_t el = arr.u.arr.elem[i];
        if (el.type != TOML_STRING || !el.u.s[0]) {
            return KITTY_Fail(err, cap, "%s: match.%s must be a list of strings", where, key);
        }
        if (out->count < KITTY_LIST_MAX) {
            SDL_strlcpy(out->items[out->count++], el.u.s, KITTY_TEXT_MAX);
        }
    }
    if (out->count > 0) {
        *present = true;
    }
    return true;
}

static bool KITTY_IsIdentityKey(const char *key)
{
    return SDL_strcmp(key, "xtversion") == 0 || SDL_strcmp(key, "env") == 0 ||
           SDL_strcmp(key, "term_program") == 0 || SDL_strcmp(key, "term") == 0 ||
           SDL_strcmp(key, "term_substring") == 0;
}

static bool KITTY_IsConditionKey(const char *key)
{
    return SDL_strcmp(key, "version_min") == 0 || SDL_strcmp(key, "version_max") == 0 ||
           SDL_strcmp(key, "os") == 0;
}

static bool KITTY_ValidId(const char *id)
{
    const char *p;
    if (!id || !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) {
        return false;
    }
    for (p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) {
            return false;
        }
    }
    return SDL_strlen(id) < KITTY_TEXT_MAX;
}

static int KITTY_FindRow(const KITTY_FactTable *t, const char *id)
{
    int i;
    for (i = 0; i < t->count; ++i) {
        if (SDL_strcmp(t->rows[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* One [[terminal]] table into row (rows before it are in t). */
static bool KITTY_ReadRow(const KITTY_FactTable *t, toml_datum_t tab, KITTY_FactRow *row,
                          char *err, size_t cap)
{
    const toml_datum_t id = toml_get(tab, "id");
    const toml_datum_t name = toml_get(tab, "name");
    const toml_datum_t extends = toml_get(tab, "extends");
    const toml_datum_t match = toml_get(tab, "match");
    const toml_datum_t evidence = toml_get(tab, "evidence");
    char where[96];
    bool identity = false;
    int i;

    SDL_zerop(row);
    row->extends = -1;
    KITTY_FactsUnknown(&row->facts);

    if (tab.type != TOML_TABLE) {
        return KITTY_Fail(err, cap, "a [[terminal]] isn't a table");
    }
    if (id.type != TOML_STRING || !KITTY_ValidId(id.u.s)) {
        return KITTY_Fail(err, cap, "bad or missing id");
    }
    SDL_strlcpy(row->id, id.u.s, sizeof(row->id));
    SDL_snprintf(where, sizeof(where), "terminal '%s'", row->id);
    if (KITTY_FindRow(t, row->id) >= 0) {
        return KITTY_Fail(err, cap, "%s: id used twice", where);
    }
    if (match.type != TOML_UNKNOWN && match.type != TOML_TABLE) {
        return KITTY_Fail(err, cap, "%s: match must be a table", where);
    }
    if (evidence.type != TOML_UNKNOWN) {
        if (evidence.type != TOML_TABLE) {
            return KITTY_Fail(err, cap, "%s: evidence must map fact keys to strings", where);
        }
        for (i = 0; i < evidence.u.tab.size; ++i) {
            size_t f;
            bool known = false;
            if (evidence.u.tab.value[i].type != TOML_STRING) {
                return KITTY_Fail(err, cap, "%s: evidence must map fact keys to strings", where);
            }
            for (f = 0; f < SDL_arraysize(KITTY_fact_keys); ++f) {
                known = known || SDL_strcmp(KITTY_fact_keys[f].key, evidence.u.tab.key[i]) == 0;
            }
            if (!known) {
                return KITTY_Fail(err, cap, "%s: evidence for unknown fact '%s'", where,
                                  evidence.u.tab.key[i]);
            }
        }
    }

    if (extends.type == TOML_UNKNOWN) {
        if (name.type != TOML_STRING || !name.u.s[0]) {
            return KITTY_Fail(err, cap, "%s: a base row needs a name", where);
        }
        SDL_strlcpy(row->name, name.u.s, sizeof(row->name));
        if (match.type == TOML_TABLE) {
            for (i = 0; i < match.u.tab.size; ++i) {
                if (!KITTY_IsIdentityKey(match.u.tab.key[i])) {
                    return KITTY_Fail(err, cap, "%s: a base row can't match by '%s'", where,
                                      match.u.tab.key[i]);
                }
            }
            if (!KITTY_ReadList(match, "xtversion", where, &row->xtversion, &identity, err, cap) ||
                !KITTY_ReadList(match, "env", where, &row->env, &identity, err, cap) ||
                !KITTY_ReadList(match, "term_program", where, &row->term_program, &identity, err, cap) ||
                !KITTY_ReadList(match, "term", where, &row->term, &identity, err, cap) ||
                !KITTY_ReadList(match, "term_substring", where, &row->term_substring, &identity, err, cap)) {
                return false;
            }
        }
        if (!identity) {
            return KITTY_Fail(err, cap, "%s: a base row needs at least one identity matcher", where);
        }
    } else {
        toml_datum_t v;
        if (extends.type != TOML_STRING || (row->extends = KITTY_FindRow(t, extends.u.s)) < 0) {
            return KITTY_Fail(err, cap, "%s: extends a row that isn't an earlier row", where);
        }
        if (name.type != TOML_UNKNOWN) {
            return KITTY_Fail(err, cap, "%s: an extending row has no name", where);
        }
        if (match.type != TOML_TABLE || match.u.tab.size == 0) {
            return KITTY_Fail(err, cap, "%s: an extending row needs a condition", where);
        }
        for (i = 0; i < match.u.tab.size; ++i) {
            if (!KITTY_IsConditionKey(match.u.tab.key[i])) {
                return KITTY_Fail(err, cap, "%s: an extending row adds version_min, version_max or os, not '%s'",
                                  where, match.u.tab.key[i]);
            }
        }
        v = toml_get(match, "version_min");
        if (v.type != TOML_UNKNOWN) {
            KITTY_VersionKey key;
            if (v.type != TOML_STRING || !KITTY_VersionKeyOf(v.u.s, &key)) {
                return KITTY_Fail(err, cap, "%s: match.version_min must be a version string", where);
            }
            SDL_strlcpy(row->version_min, v.u.s, sizeof(row->version_min));
        }
        v = toml_get(match, "version_max");
        if (v.type != TOML_UNKNOWN) {
            KITTY_VersionKey key;
            if (v.type != TOML_STRING || !KITTY_VersionKeyOf(v.u.s, &key)) {
                return KITTY_Fail(err, cap, "%s: match.version_max must be a version string", where);
            }
            SDL_strlcpy(row->version_max, v.u.s, sizeof(row->version_max));
        }
        v = toml_get(match, "os");
        if (v.type != TOML_UNKNOWN) {
            if (v.type != TOML_ARRAY || v.u.arr.size == 0) {
                return KITTY_Fail(err, cap, "%s: match.os must list some of macos, linux, windows, bsd", where);
            }
            for (i = 0; i < v.u.arr.size; ++i) {
                const unsigned bit = (v.u.arr.elem[i].type == TOML_STRING)
                                         ? KITTY_OSFromName(v.u.arr.elem[i].u.s)
                                         : 0;
                if (!bit) {
                    return KITTY_Fail(err, cap, "%s: match.os must list some of macos, linux, windows, bsd", where);
                }
                row->os_mask |= bit;
            }
        }
    }
    return KITTY_ReadFacts(tab, "", where, &row->facts, err, cap);
}

/* A whole terminals.toml, checked against schema 1, into t. */
static bool KITTY_ReadFactsFile(toml_result_t result, KITTY_FactTable *t, char *err, size_t cap)
{
    const toml_datum_t top = result.toptab;
    const toml_datum_t schema = toml_get(top, "schema");
    const toml_datum_t rows = toml_get(top, "terminal");
    int i;

    t->count = 0;
    if (schema.type != TOML_INT64 || schema.u.int64 != 1) {
        return KITTY_Fail(err, cap, "schema must be 1");
    }
    for (i = 0; i < top.u.tab.size; ++i) {
        if (SDL_strcmp(top.u.tab.key[i], "schema") != 0 &&
            SDL_strcmp(top.u.tab.key[i], "terminal") != 0) {
            return KITTY_Fail(err, cap, "unknown top-level key '%s'", top.u.tab.key[i]);
        }
    }
    if (rows.type == TOML_UNKNOWN) {
        return true;
    }
    if (rows.type != TOML_ARRAY) {
        return KITTY_Fail(err, cap, "terminal must be an array of tables");
    }
    for (i = 0; i < rows.u.arr.size; ++i) {
        if (t->count >= KITTY_ROWS_MAX) {
            return KITTY_Fail(err, cap, "more than %d rows", KITTY_ROWS_MAX);
        }
        if (!KITTY_ReadRow(t, rows.u.arr.elem[i], &t->rows[t->count], err, cap)) {
            return false;
        }
        t->count++;
    }
    return true;
}

/* Parse facts text (compiled in or from a file); replace g_facts if valid. */
static bool KITTY_TakeFacts(toml_result_t result, const char *what)
{
    KITTY_FactTable *t;
    char err[256];

    if (!result.ok) {
        KITTY_ProfileDebug("kitty: facts %s not TOML: %s", what, result.errmsg);
        return false;
    }
    t = (KITTY_FactTable *)SDL_calloc(1, sizeof(*t));
    if (!t) {
        return false;
    }
    err[0] = '\0';
    if (!KITTY_ReadFactsFile(result, t, err, sizeof(err))) {
        KITTY_ProfileDebug("kitty: facts %s refused: %s", what, err);
        SDL_free(t);
        return false;
    }
    SDL_free(g_facts);
    g_facts = t;
    KITTY_ProfileDebug("kitty: facts %s rows=%d", what, t->count);
    return true;
}

/* ---- Recognising the terminal (SPEC.md: Recognising, Resolving) ---- */

static bool KITTY_ListHasCI(const KITTY_StringList *list, const char *s)
{
    int i;
    for (i = 0; s && i < list->count; ++i) {
        if (SDL_strcasecmp(list->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}

static bool KITTY_ListHas(const KITTY_StringList *list, const char *s)
{
    int i;
    for (i = 0; s && i < list->count; ++i) {
        if (SDL_strcmp(list->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}

static bool KITTY_ListInside(const KITTY_StringList *list, const char *s)
{
    int i;
    for (i = 0; s && i < list->count; ++i) {
        if (SDL_strstr(s, list->items[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static bool KITTY_AnyEnvSet(const KITTY_StringList *list)
{
    int i;
    for (i = 0; i < list->count; ++i) {
        const char *v = SDL_getenv(list->items[i]);
        if (v && v[0]) {
            return true;
        }
    }
    return false;
}

/*
 * The base row for this terminal: XTVERSION answer first (it can't be
 * inherited from a parent terminal), then match.env, TERM_PROGRAM, TERM.
 * -1 if nothing matches. Fills version and how.
 */
static int KITTY_RecogniseBase(const KITTY_FactTable *t, const char *xtversion,
                               char *version, size_t version_cap, const char **how)
{
    const char *program = SDL_getenv("TERM_PROGRAM");
    const char *term = SDL_getenv("TERM");
    int base = -1;
    int i;

    version[0] = '\0';
    *how = "";
    if (xtversion && xtversion[0]) {
        char name[KITTY_TEXT_MAX];
        KITTY_SplitXtversion(xtversion, name, sizeof(name), version, version_cap);
        for (i = 0; i < t->count && base < 0; ++i) {
            if (t->rows[i].extends < 0 && KITTY_ListHasCI(&t->rows[i].xtversion, name)) {
                base = i;
                *how = "xtversion";
            }
        }
        if (base < 0) {
            version[0] = '\0';
        }
    }
    for (i = 0; i < t->count && base < 0; ++i) {
        if (t->rows[i].extends < 0 && KITTY_AnyEnvSet(&t->rows[i].env)) {
            base = i;
            *how = "env";
        }
    }
    for (i = 0; i < t->count && base < 0 && program && program[0]; ++i) {
        if (t->rows[i].extends < 0 && KITTY_ListHasCI(&t->rows[i].term_program, program)) {
            base = i;
            *how = "term_program";
        }
    }
    for (i = 0; i < t->count && base < 0 && term; ++i) {
        if (t->rows[i].extends < 0 && (KITTY_ListHas(&t->rows[i].term, term) ||
                                       KITTY_ListInside(&t->rows[i].term_substring, term))) {
            base = i;
            *how = "term";
        }
    }
    if (base >= 0 && SDL_strcmp(*how, "xtversion") != 0 && program && program[0] &&
        KITTY_ListHasCI(&t->rows[base].term_program, program)) {
        const char *v = SDL_getenv("TERM_PROGRAM_VERSION");
        SDL_strlcpy(version, v ? v : "", version_cap);
    }
    return base;
}

static bool KITTY_ConditionsHold(const KITTY_FactRow *row, const char *version, unsigned os)
{
    KITTY_VersionKey have, bound;

    if (row->os_mask && !(row->os_mask & os)) {
        return false;
    }
    if (row->version_min[0] || row->version_max[0]) {
        if (!version || !version[0] || !KITTY_VersionKeyOf(version, &have)) {
            return false; /* a version bound never holds for an unknown version */
        }
        if (row->version_min[0] && KITTY_VersionKeyOf(row->version_min, &bound) &&
            KITTY_CompareVersionKeys(&have, &bound) < 0) {
            return false;
        }
        if (row->version_max[0] && KITTY_VersionKeyOf(row->version_max, &bound) &&
            KITTY_CompareVersionKeys(&have, &bound) > 0) {
            return false;
        }
    }
    return true;
}

static KITTY_TerminalKind KITTY_KindFromId(const char *id)
{
    if (SDL_strcmp(id, "rex") == 0) {
        return KITTY_TERM_REX;
    }
    if (SDL_strcmp(id, "kitty") == 0) {
        return KITTY_TERM_KITTY;
    }
    if (SDL_strcmp(id, "ghostty") == 0) {
        return KITTY_TERM_GHOSTTY;
    }
    if (SDL_strcmp(id, "wezterm") == 0) {
        return KITTY_TERM_WEZTERM;
    }
    return KITTY_TERM_UNKNOWN;
}

/* ---- Tuning (sdl-kitty.toml) ---- */

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

/* Floats; ints are widened. */
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

/* Strings point into the parse tree (valid while the tuning stays loaded). */
static bool KITTY_TomlGetString(toml_datum_t tab, const char *key, const char **out)
{
    toml_datum_t v = toml_get(tab, key);
    if (v.type != TOML_STRING || !v.u.s) {
        return false;
    }
    *out = v.u.s;
    return true;
}

/* "WxH" (either case of x) into positive width and height. */
static bool KITTY_ParseWxH(const char *s, int *w, int *h)
{
    int aw = 0, ah = 0;
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

/* "WxH", or "0" / "off" / "none" / "" for no cap. */
static void KITTY_TomlGetCap(toml_datum_t tab, const char *key, int *w, int *h)
{
    const char *s;
    if (!KITTY_TomlGetString(tab, key, &s)) {
        return;
    }
    if (!KITTY_ParseWxH(s, w, h) &&
        (SDL_strcmp(s, "0") == 0 || SDL_strcasecmp(s, "off") == 0 ||
         SDL_strcasecmp(s, "none") == 0 || s[0] == '\0')) {
        *w = 0;
        *h = 0;
    }
}

/* Lay the keys set in a tuning table over *p. */
static void KITTY_OverlayTuning(KITTY_TerminalProfile *p, toml_datum_t tab)
{
    bool b;
    int64_t i64;
    double fp;
    const char *s;

    if (tab.type != TOML_TABLE) {
        return;
    }
    if (KITTY_TomlGetBool(tab, "prefer_direct", &b)) {
        p->prefer_direct = b;
    }
    if (KITTY_TomlGetBool(tab, "prefer_zlib", &b)) {
        p->prefer_zlib = b;
    }
    if (KITTY_TomlGetString(tab, "force_transmit", &s)) {
        p->force_transmit = (s[0] && SDL_strcasecmp(s, "off") != 0 &&
                             SDL_strcasecmp(s, "none") != 0) ? s : NULL;
    }
    KITTY_TomlGetCap(tab, "default_fb_max", &p->default_fb_max_w, &p->default_fb_max_h);
    KITTY_TomlGetCap(tab, "default_fb_max_shm", &p->default_fb_max_shm_w, &p->default_fb_max_shm_h);
    KITTY_TomlGetCap(tab, "default_fb_max_direct", &p->default_fb_max_direct_w, &p->default_fb_max_direct_h);
    if (KITTY_TomlGetInt(tab, "max_direct_bytes", &i64) && i64 > 0) {
        p->max_direct_bytes = (size_t)i64;
    }
    if (KITTY_TomlGetFloat(tab, "af_max_coverage", &fp)) {
        p->af_max_coverage = (float)fp;
    }
    if (KITTY_TomlGetBool(tab, "af_zlib", &b)) {
        p->af_zlib = b;
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
    if (KITTY_TomlGetString(tab, "notes", &s)) {
        p->notes = s;
    }
}

/* [terminal.<id>] from every tuning file, in load order. */
static void KITTY_OverlayTuningFor(KITTY_TerminalProfile *p, const char *id)
{
    int i;
    for (i = 0; i < g_tuning_count; ++i) {
        const toml_datum_t terminals = toml_get(g_tuning[i].toptab, "terminal");
        if (terminals.type == TOML_TABLE) {
            KITTY_OverlayTuning(p, toml_get(terminals, id));
        }
    }
}

static bool KITTY_TakeTuning(toml_result_t result, const char *what)
{
    if (!result.ok) {
        KITTY_ProfileDebug("kitty: tuning %s not TOML: %s", what, result.errmsg);
        toml_free(result);
        return false;
    }
    if (g_tuning_count >= KITTY_TUNING_MAX) {
        KITTY_ProfileDebug("kitty: tuning %s skipped: more than %d files", what, KITTY_TUNING_MAX);
        toml_free(result);
        return false;
    }
    g_tuning[g_tuning_count++] = result;
    return true;
}

/* ---- Loading ---- */

static void KITTY_NoteSource(const char *path)
{
    const size_t len = SDL_strlen(g_source);
    SDL_snprintf(g_source + len, sizeof(g_source) - len, ";%s", path);
}

static void KITTY_ClearAll(void)
{
    int i;
    for (i = 0; i < g_tuning_count; ++i) {
        toml_free(g_tuning[i]);
    }
    g_tuning_count = 0;
    SDL_free(g_facts);
    g_facts = NULL;
    SDL_strlcpy(g_source, "builtin", sizeof(g_source));
}

/* The compiled-in terminals.toml and sdl-kitty.toml. */
static void KITTY_LoadEmbedded(void)
{
    toml_result_t facts, tuning;

    KITTY_TomlInit();
    facts = toml_parse(KITTY_EMBEDDED_FACTS_TOML, (int)SDL_strlen(KITTY_EMBEDDED_FACTS_TOML));
    (void)KITTY_TakeFacts(facts, "builtin");
    toml_free(facts);
    tuning = toml_parse(KITTY_EMBEDDED_TUNING_TOML, (int)SDL_strlen(KITTY_EMBEDDED_TUNING_TOML));
    (void)KITTY_TakeTuning(tuning, "builtin");
}

static bool KITTY_Readable(const char *path)
{
    FILE *fp;
    if (!path || !path[0]) {
        return false;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    fclose(fp);
    return true;
}

bool KITTY_LoadTerminalFacts(const char *path)
{
    toml_result_t result;
    bool ok;

    KITTY_EnsureTerminalProfilesLoaded();
    if (!KITTY_Readable(path)) {
        return false;
    }
    result = toml_parse_file_ex(path);
    ok = KITTY_TakeFacts(result, path);
    toml_free(result);
    if (ok) {
        KITTY_NoteSource(path);
    }
    return ok;
}

bool KITTY_LoadTerminalProfiles(const char *path)
{
    KITTY_EnsureTerminalProfilesLoaded();
    if (!KITTY_Readable(path)) {
        return false;
    }
    if (!KITTY_TakeTuning(toml_parse_file_ex(path), path)) {
        return false;
    }
    KITTY_NoteSource(path);
    return true;
}

/* pref path / base path candidates for a file name, in search order. */
static int KITTY_DefaultPaths(const char *file, const char *compiled, char paths[][1024], int cap)
{
    int n = 0;
#ifndef KITTY_PROFILE_STANDALONE
    {
        char *pref = SDL_GetPrefPath("sdl-kitty", "kitty");
        const char *base = SDL_GetBasePath();
        if (pref && n < cap) {
            SDL_snprintf(paths[n++], 1024, "%s%s", pref, file);
        }
        SDL_free(pref);
        if (base && n + 1 < cap) {
            SDL_snprintf(paths[n++], 1024, "%skitty/%s", base, file);
            SDL_snprintf(paths[n++], 1024, "%s%s", base, file);
        }
    }
#else
    (void)file;
#endif
    if (compiled && compiled[0] && n < cap) {
        SDL_strlcpy(paths[n++], compiled, 1024);
    }
    return n;
}

void KITTY_EnsureTerminalProfilesLoaded(void)
{
    char paths[4][1024];
    const char *facts_env;
    const char *tuning_env;
    const char *compiled_facts = NULL;
    const char *compiled_tuning = NULL;
    int n, i;

    if (g_loaded) {
        return;
    }
    g_loaded = true;
    KITTY_LoadEmbedded();
#ifdef KITTY_DEFAULT_FACTS_PATH
    compiled_facts = KITTY_DEFAULT_FACTS_PATH;
#endif
#ifdef KITTY_DEFAULT_PROFILES_PATH
    compiled_tuning = KITTY_DEFAULT_PROFILES_PATH;
#endif

    /* Facts: one file replaces the compiled-in copy; the first found wins. */
    facts_env = SDL_getenv("SDL_KITTY_FACTS");
    if (facts_env && facts_env[0]) {
        (void)KITTY_LoadTerminalFacts(facts_env);
    } else {
        n = KITTY_DefaultPaths("terminals.toml", compiled_facts, paths, 4);
        for (i = 0; i < n; ++i) {
            if (KITTY_Readable(paths[i])) {
                (void)KITTY_LoadTerminalFacts(paths[i]);
                break;
            }
        }
    }

    /* Tuning: every file found overlays, in order. SDL_KITTY_PROFILES (a
     * colon-separated list) replaces the search. */
    tuning_env = SDL_getenv("SDL_KITTY_PROFILES");
    if (tuning_env && tuning_env[0]) {
        char *copy = SDL_strdup(tuning_env);
        char *cursor = copy;
        while (cursor) {
            char *tok = cursor;
            cursor = SDL_strchr(cursor, ':');
            if (cursor) {
                *cursor++ = '\0';
            }
            if (tok[0]) {
                (void)KITTY_LoadTerminalProfiles(tok);
            }
        }
        SDL_free(copy);
    } else {
        n = KITTY_DefaultPaths("sdl-kitty.toml", compiled_tuning, paths, 4);
        for (i = 0; i < n; ++i) {
            (void)KITTY_LoadTerminalProfiles(paths[i]);
        }
    }
    KITTY_ProfileDebug("kitty: profiles=%s", g_source);
}

void KITTY_ResetTerminalProfilesToBuiltin(void)
{
    KITTY_ClearAll();
    g_loaded = false;
}

const char *KITTY_TerminalProfilesSource(void)
{
    return g_source;
}

/* ---- Resolving ---- */

/* Settings the facts decide, over the tuning. */
static void KITTY_ApplyFacts(KITTY_TerminalProfile *p)
{
    const KITTY_TerminalFacts *f = &p->facts;

    if (f->frame_edit_reply != KITTY_FACT_UNKNOWN) {
        p->silent_af_ok = (f->frame_edit_reply == KITTY_FACT_NO);
    }
    if (f->ioctl_pixels != KITTY_FACT_UNKNOWN) {
        p->ignore_ioctl_pixels = (f->ioctl_pixels == KITTY_FACT_NO);
    }
    if (f->max_escape_bytes > KITTY_APC_HEADROOM) {
        p->apc_chunk_bytes = f->max_escape_bytes - KITTY_APC_HEADROOM;
    }
    if (f->shm == KITTY_FACT_NO) {
        p->prefer_direct = true;
    }
    if (f->frame_edit_zlib != KITTY_FACT_UNKNOWN) {
        p->af_zlib = (f->frame_edit_zlib == KITTY_FACT_YES);
    }
    if (f->frame_edit == KITTY_FACT_NO) {
        p->skip_af_probe = true;
        p->direct_af_when_shm = false;
    }
}

const KITTY_TerminalProfile *KITTY_IdentifyTerminalProfile(const char *xtversion,
                                                           const char *os_name)
{
    const unsigned os = os_name ? KITTY_OSFromName(os_name) : KITTY_CurrentOS();
    const char *how = "";
    bool applied[KITTY_ROWS_MAX];
    int base = -1;
    int i;
    int t;

    KITTY_EnsureTerminalProfilesLoaded();
    g_profile = *KITTY_BuiltinDefaultsProfile();
    KITTY_FactsUnknown(&g_profile.facts);
    g_profile_version[0] = '\0';
    g_profile_rows[0] = '\0';
    SDL_strlcpy(g_profile_name, "unknown", sizeof(g_profile_name));

    for (t = 0; t < g_tuning_count; ++t) {
        KITTY_OverlayTuning(&g_profile, toml_get(g_tuning[t].toptab, "defaults"));
    }
    if (g_facts) {
        base = KITTY_RecogniseBase(g_facts, xtversion, g_profile_version,
                                   sizeof(g_profile_version), &how);
    }
    if (base < 0) {
        KITTY_OverlayTuningFor(&g_profile, "unknown");
        how = "";
    } else {
        SDL_zeroa(applied);
        applied[base] = true;
        SDL_strlcpy(g_profile_name, g_facts->rows[base].id, sizeof(g_profile_name));
        SDL_strlcpy(g_profile_rows, g_facts->rows[base].id, sizeof(g_profile_rows));
        g_profile.facts = g_facts->rows[base].facts;
        KITTY_OverlayTuningFor(&g_profile, g_facts->rows[base].id);
        for (i = 0; i < g_facts->count; ++i) {
            const KITTY_FactRow *row = &g_facts->rows[i];
            if (row->extends >= 0 && applied[row->extends] &&
                KITTY_ConditionsHold(row, g_profile_version, os)) {
                applied[i] = true;
                SDL_strlcat(g_profile_rows, ",", sizeof(g_profile_rows));
                SDL_strlcat(g_profile_rows, row->id, sizeof(g_profile_rows));
                KITTY_FactsOverlay(&g_profile.facts, &row->facts);
                KITTY_OverlayTuningFor(&g_profile, row->id);
            }
        }
    }
    KITTY_ApplyFacts(&g_profile);
    g_profile.kind = KITTY_KindFromId(g_profile_name);
    g_profile.name = g_profile_name;
    g_profile.rows = g_profile_rows;
    g_profile.version = g_profile_version;
    g_profile.recognised_by = how;
    KITTY_ProfileDebug("kitty: terminal=%s rows=%s version=%s by=%s", g_profile.name,
                       g_profile.rows[0] ? g_profile.rows : "-",
                       g_profile.version[0] ? g_profile.version : "?",
                       how[0] ? how : "-");
    return &g_profile;
}

const KITTY_TerminalProfile *KITTY_DetectTerminalProfile(void)
{
    return KITTY_IdentifyTerminalProfile(NULL, NULL);
}

bool KITTY_EnvMatchesLoadedProfile(void)
{
    char version[KITTY_TEXT_MAX];
    const char *how;

    KITTY_EnsureTerminalProfilesLoaded();
    return g_facts && KITTY_RecogniseBase(g_facts, NULL, version, sizeof(version), &how) >= 0;
}

#endif /* SDL_VIDEO_DRIVER_KITTY || KITTY_PROFILE_STANDALONE */
