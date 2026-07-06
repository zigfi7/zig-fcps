// SPDX-License-Identifier: GPL-3.0-or-later
// fcp-mix — user-friendly CLI for live mixer / routing / metering
// over the ALSA controls created by fcp-server.
//
// Design: ALSA controls are the source of truth (fcp-server keeps them in
// sync with the device). This tool just makes them human-friendly.
//
// Commands:
//   fcp-mix [-c CARD] list [GROUP]              — list controls grouped by kind
//   fcp-mix [-c CARD] get NAME                  — read one control (any kind)
//   fcp-mix [-c CARD] set NAME VALUE            — write one control
//   fcp-mix [-c CARD] route [CHANNEL=SOURCE...]  — show or set routing
//   fcp-mix [-c CARD] phantom INPUT on|off      — Phantom Power switch
//   fcp-mix [-c CARD] mute NAME on|off          — toggle a Mute switch
//   fcp-mix [-c CARD] mix MIXNAME [INPUT=DB...] — show/set mix-input volumes
//   fcp-mix [-c CARD] meter [-i SECONDS]        — peek Level Meter values
//   fcp-mix [-c CARD] save FILE                 — dump full state to JSON
//   fcp-mix [-c CARD] load FILE                 — restore from JSON
//
// CARD defaults to the first card whose name starts with "Scarlett"; -c
// overrides. Names support shell-glob wildcards: e.g. "Line In * Phantom*".
//
// Build (added to Makefile):
//   cc -O2 -Wall -o fcp-mix client/fcp-mix.c -lasound -ljson-c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fnmatch.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>

static int g_card = -1;
static int g_json = 0;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap); exit(1);
}

/* ---- card discovery ---- */

static int find_scarlett_card(void) {
    int card = -1;
    for (;;) {
        if (snd_card_next(&card) < 0 || card < 0) break;
        char *name = NULL;
        if (snd_card_get_longname(card, &name) == 0 && name) {
            if (strncasecmp(name, "Focusrite", 9) == 0 ||
                strcasestr(name, "Scarlett") != NULL) {
                free(name);
                return card;
            }
            free(name);
        }
    }
    return -1;
}

/* ---- ALSA helpers ---- */

static snd_ctl_t *open_card(void) {
    char hw[16];
    snprintf(hw, sizeof hw, "hw:%d", g_card);
    snd_ctl_t *ctl = NULL;
    int rc = snd_ctl_open(&ctl, hw, 0);
    if (rc < 0) die("snd_ctl_open(%s): %s", hw, snd_strerror(rc));
    return ctl;
}

/* Iterate all elements; call cb(name, eid, info, value, user). */
typedef int (*elem_cb_t)(snd_ctl_t *, const char *name,
                         snd_ctl_elem_id_t *id,
                         snd_ctl_elem_info_t *info,
                         snd_ctl_elem_value_t *value,
                         void *user);

static int for_each_elem(snd_ctl_t *ctl, const char *name_glob, elem_cb_t cb, void *user) {
    snd_ctl_elem_list_t *list;
    snd_ctl_elem_list_alloca(&list);
    int rc = snd_ctl_elem_list(ctl, list);
    if (rc < 0) return rc;
    unsigned n = snd_ctl_elem_list_get_count(list);
    snd_ctl_elem_list_alloc_space(list, n);
    rc = snd_ctl_elem_list(ctl, list);
    if (rc < 0) return rc;

    int matched = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *name = snd_ctl_elem_list_get_name(list, i);
        if (name_glob && fnmatch(name_glob, name, FNM_CASEFOLD) != 0) continue;

        snd_ctl_elem_id_t *id;
        snd_ctl_elem_info_t *info;
        snd_ctl_elem_value_t *value;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_value_alloca(&value);

        snd_ctl_elem_list_get_id(list, i, id);
        snd_ctl_elem_info_set_id(info, id);
        if (snd_ctl_elem_info(ctl, info) < 0) continue;
        snd_ctl_elem_value_set_id(value, id);
        if (snd_ctl_elem_read(ctl, value) < 0) continue;

        int r = cb(ctl, name, id, info, value, user);
        matched++;
        if (r != 0) break;
    }
    snd_ctl_elem_list_free_space(list);
    return matched;
}

/* Format a single element's value as text. */
static void format_value(snd_ctl_elem_info_t *info, snd_ctl_elem_value_t *value,
                         char *out, size_t outlen)
{
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned n = snd_ctl_elem_info_get_count(info);
    if (n > 8) n = 8;
    out[0] = 0;
    size_t pos = 0;
    for (unsigned i = 0; i < n; i++) {
        if (i) pos += snprintf(out + pos, outlen - pos, ",");
        switch (type) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            pos += snprintf(out + pos, outlen - pos, "%s",
                            snd_ctl_elem_value_get_boolean(value, i) ? "on" : "off");
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            pos += snprintf(out + pos, outlen - pos, "%ld",
                            snd_ctl_elem_value_get_integer(value, i));
            break;
        case SND_CTL_ELEM_TYPE_INTEGER64:
            pos += snprintf(out + pos, outlen - pos, "%lld",
                            (long long) snd_ctl_elem_value_get_integer64(value, i));
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED: {
            unsigned ix = snd_ctl_elem_value_get_enumerated(value, i);
            pos += snprintf(out + pos, outlen - pos, "%u", ix);
            break;
        }
        case SND_CTL_ELEM_TYPE_BYTES:
            pos += snprintf(out + pos, outlen - pos, "0x%02x",
                            snd_ctl_elem_value_get_byte(value, i));
            break;
        default:
            pos += snprintf(out + pos, outlen - pos, "?");
        }
        if (pos >= outlen - 1) break;
    }
}

/* For ENUMERATED controls, also resolve the index to its label. */
static char *enum_label_of(snd_ctl_t *ctl, snd_ctl_elem_id_t *id,
                            snd_ctl_elem_info_t *info, unsigned idx)
{
    static char buf[128];
    snd_ctl_elem_info_set_id(info, id);
    snd_ctl_elem_info_set_item(info, idx);
    if (snd_ctl_elem_info(ctl, info) < 0) { snprintf(buf, sizeof buf, "%u", idx); return buf; }
    const char *n = snd_ctl_elem_info_get_item_name(info);
    snprintf(buf, sizeof buf, "%s", n ? n : "?");
    return buf;
}

/* dB conversion (controls expose TLV with linear-or-dB scale). */
static int read_db_range(snd_ctl_t *ctl, snd_ctl_elem_id_t *id,
                         long *minraw, long *maxraw, long *mindb, long *maxdb)
{
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0) return -1;
    *minraw = snd_ctl_elem_info_get_min(info);
    *maxraw = snd_ctl_elem_info_get_max(info);
    /* Convert min/max raw to dB individually (no _range helper in older alsa-lib) */
    if (snd_ctl_convert_to_dB(ctl, id, *minraw, mindb) < 0 ||
        snd_ctl_convert_to_dB(ctl, id, *maxraw, maxdb) < 0) {
        return -2; /* no dB info */
    }
    return 0;
}

/* ---- groupings ---- */
typedef struct {
    const char *label;
    const char *glob;
} group_t;

static const group_t GROUPS[] = {
    { "Inputs",       "Line In *"             },
    { "Mixes",        "Mix * Input *"         },
    { "Mux Routing",  "PCM * Capture Enum"    },
    { "Outputs",      "Line *|Analogue *|S/PDIF *|ADAT *" },
    { "Master",       "Master *"              },
    { "Globals",      "Mute|Dim|Standalone *|Speaker *|Phantom Power *|Clock Source*|Sync*" },
    { "Group Outputs","Main Group *|Alt Group *" },
    { "Meters",       "Level Meter*"          },
    { "Other",        "*"                     },
    { NULL, NULL }
};

/* match one of the |-separated globs */
static int matches_any(const char *globs, const char *name) {
    char buf[256]; strncpy(buf, globs, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *p = buf, *tok; (tok = strsep(&p, "|")); )
        if (fnmatch(tok, name, FNM_CASEFOLD) == 0) return 1;
    return 0;
}

/* ---- commands ---- */

static int cmd_list_cb(snd_ctl_t *ctl, const char *name,
                       snd_ctl_elem_id_t *id,
                       snd_ctl_elem_info_t *info,
                       snd_ctl_elem_value_t *value, void *user)
{
    json_object *jarr = (json_object *) user;
    char val[128]; format_value(info, value, val, sizeof val);
    snd_ctl_elem_type_t t = snd_ctl_elem_info_get_type(info);
    const char *typename = "";
    switch (t) {
        case SND_CTL_ELEM_TYPE_BOOLEAN: typename = "bool";  break;
        case SND_CTL_ELEM_TYPE_INTEGER: typename = "int";   break;
        case SND_CTL_ELEM_TYPE_INTEGER64: typename = "int64"; break;
        case SND_CTL_ELEM_TYPE_ENUMERATED: typename = "enum"; break;
        case SND_CTL_ELEM_TYPE_BYTES:   typename = "bytes"; break;
        default: typename = "?";
    }
    if (jarr) {
        json_object *o = json_object_new_object();
        json_object_object_add(o, "name", json_object_new_string(name));
        json_object_object_add(o, "type", json_object_new_string(typename));
        json_object_object_add(o, "value", json_object_new_string(val));
        if (t == SND_CTL_ELEM_TYPE_ENUMERATED) {
            unsigned cnt = snd_ctl_elem_info_get_items(info);
            json_object *items = json_object_new_array();
            for (unsigned i = 0; i < cnt; i++) {
                snd_ctl_elem_info_set_id(info, id);
                snd_ctl_elem_info_set_item(info, i);
                if (snd_ctl_elem_info(ctl, info) == 0) {
                    json_object_array_add(items, json_object_new_string(snd_ctl_elem_info_get_item_name(info)));
                }
            }
            json_object_object_add(o, "items", items);
        }
        json_object_array_add(jarr, o);
    } else {
        if (t == SND_CTL_ELEM_TYPE_ENUMERATED) {
            char *lbl = enum_label_of(ctl, id, info, snd_ctl_elem_value_get_enumerated(value, 0));
            printf("  %-50s [%s] %s (=%s)\n", name, typename, val, lbl);
        } else {
            printf("  %-50s [%s] %s\n", name, typename, val);
        }
    }
    return 0;
}

static int cmd_list(int argc, char **argv) {
    const char *want_group = argc > 0 ? argv[0] : NULL;
    snd_ctl_t *ctl = open_card();
    if (!g_json) printf("Card %d:\n", g_card);
    json_object *root = NULL;
    if (g_json) root = json_object_new_object();
    for (const group_t *g = GROUPS; g->label; g++) {
        if (want_group && strcasecmp(want_group, g->label) != 0 &&
            strcasecmp(want_group, "all") != 0) continue;
        if (!g_json) printf("\n# %s\n", g->label);
        json_object *jarr = NULL;
        if (g_json) jarr = json_object_new_array();
        /* iterate ALL controls; manually filter by group glob */
        snd_ctl_elem_list_t *list;
        snd_ctl_elem_list_alloca(&list);
        snd_ctl_elem_list(ctl, list);
        unsigned n = snd_ctl_elem_list_get_count(list);
        snd_ctl_elem_list_alloc_space(list, n);
        snd_ctl_elem_list(ctl, list);
        for (unsigned i = 0; i < n; i++) {
            const char *name = snd_ctl_elem_list_get_name(list, i);
            if (!matches_any(g->glob, name)) continue;
            /* exclude items already grabbed by earlier groups */
            int taken = 0;
            for (const group_t *gp = GROUPS; gp != g; gp++)
                if (matches_any(gp->glob, name)) { taken = 1; break; }
            if (taken) continue;
            snd_ctl_elem_id_t *id;
            snd_ctl_elem_info_t *info;
            snd_ctl_elem_value_t *value;
            snd_ctl_elem_id_alloca(&id);
            snd_ctl_elem_info_alloca(&info);
            snd_ctl_elem_value_alloca(&value);
            snd_ctl_elem_list_get_id(list, i, id);
            snd_ctl_elem_info_set_id(info, id);
            if (snd_ctl_elem_info(ctl, info) < 0) continue;
            snd_ctl_elem_value_set_id(value, id);
            if (snd_ctl_elem_read(ctl, value) < 0) continue;
            cmd_list_cb(ctl, name, id, info, value, jarr);
        }
        snd_ctl_elem_list_free_space(list);
        if (g_json) json_object_object_add(root, g->label, jarr);
    }
    if (g_json) {
        printf("%s\n", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        json_object_put(root);
    }
    snd_ctl_close(ctl);
    return 0;
}

static int cmd_get_cb(snd_ctl_t *ctl, const char *name,
                      snd_ctl_elem_id_t *id,
                      snd_ctl_elem_info_t *info,
                      snd_ctl_elem_value_t *value, void *user)
{
    int *count = (int *) user;
    char val[128]; format_value(info, value, val, sizeof val);
    snd_ctl_elem_type_t t = snd_ctl_elem_info_get_type(info);
    if (t == SND_CTL_ELEM_TYPE_ENUMERATED) {
        char *lbl = enum_label_of(ctl, id, info, snd_ctl_elem_value_get_enumerated(value, 0));
        printf("%s = %s (%s)\n", name, val, lbl);
    } else if (t == SND_CTL_ELEM_TYPE_INTEGER) {
        long mn, mx, mndb, mxdb;
        if (read_db_range(ctl, id, &mn, &mx, &mndb, &mxdb) == 0) {
            long raw = snd_ctl_elem_value_get_integer(value, 0);
            long db = 0;
            snd_ctl_convert_to_dB(ctl, id, raw, &db);
            printf("%s = %ld (%.1f dB; range %.1f..%.1f dB)\n", name, raw, db / 100.0, mndb / 100.0, mxdb / 100.0);
        } else {
            printf("%s = %s\n", name, val);
        }
    } else {
        printf("%s = %s\n", name, val);
    }
    (*count)++;
    return 0;
}

static int cmd_get(int argc, char **argv) {
    if (argc < 1) die("usage: fcp-mix get NAME");
    snd_ctl_t *ctl = open_card();
    int matched = 0;
    int rc = for_each_elem(ctl, argv[0], cmd_get_cb, &matched);
    snd_ctl_close(ctl);
    if (matched == 0) die("no controls match '%s'", argv[0]);
    return rc < 0 ? 1 : 0;
}

/* parse VALUE for SET: "on"/"off" for bool, integer, or "Nd" / "N.NNdB" for dB */
static int parse_and_apply(snd_ctl_t *ctl, snd_ctl_elem_id_t *id,
                            snd_ctl_elem_info_t *info, snd_ctl_elem_value_t *value,
                            const char *vstr)
{
    snd_ctl_elem_type_t t = snd_ctl_elem_info_get_type(info);
    unsigned n = snd_ctl_elem_info_get_count(info);
    switch (t) {
    case SND_CTL_ELEM_TYPE_BOOLEAN: {
        int on = (!strcasecmp(vstr, "on") || !strcmp(vstr, "1") ||
                  !strcasecmp(vstr, "yes") || !strcasecmp(vstr, "true"));
        int off = (!strcasecmp(vstr, "off") || !strcmp(vstr, "0") ||
                   !strcasecmp(vstr, "no") || !strcasecmp(vstr, "false"));
        int toggle = !strcasecmp(vstr, "toggle");
        if (!on && !off && !toggle) die("bool needs on|off|toggle, got '%s'", vstr);
        for (unsigned i = 0; i < n; i++) {
            int cur = snd_ctl_elem_value_get_boolean(value, i);
            int nv = on ? 1 : off ? 0 : !cur;
            snd_ctl_elem_value_set_boolean(value, i, nv);
        }
        break;
    }
    case SND_CTL_ELEM_TYPE_INTEGER: {
        const char *db_suf = strstr(vstr, "dB");
        if (db_suf) {
            double db = atof(vstr);
            long raw;
            if (snd_ctl_convert_from_dB(ctl, id, (long)(db * 100), &raw, 0) < 0)
                die("control has no dB scale");
            for (unsigned i = 0; i < n; i++)
                snd_ctl_elem_value_set_integer(value, i, raw);
        } else {
            long v = atol(vstr);
            long mn = snd_ctl_elem_info_get_min(info);
            long mx = snd_ctl_elem_info_get_max(info);
            if (v < mn || v > mx) die("value %ld out of range [%ld..%ld]", v, mn, mx);
            for (unsigned i = 0; i < n; i++)
                snd_ctl_elem_value_set_integer(value, i, v);
        }
        break;
    }
    case SND_CTL_ELEM_TYPE_ENUMERATED: {
        unsigned cnt = snd_ctl_elem_info_get_items(info);
        unsigned ix = (unsigned) -1;
        if (isdigit((unsigned char) vstr[0])) {
            ix = (unsigned) atoi(vstr);
        } else {
            for (unsigned i = 0; i < cnt; i++) {
                snd_ctl_elem_info_set_id(info, id);
                snd_ctl_elem_info_set_item(info, i);
                if (snd_ctl_elem_info(ctl, info) < 0) continue;
                if (!strcasecmp(snd_ctl_elem_info_get_item_name(info), vstr)) {
                    ix = i; break;
                }
            }
        }
        if (ix == (unsigned) -1 || ix >= cnt) die("enum value '%s' not found", vstr);
        for (unsigned i = 0; i < n; i++)
            snd_ctl_elem_value_set_enumerated(value, i, ix);
        break;
    }
    default:
        die("unsupported control type for set");
    }
    return snd_ctl_elem_write(ctl, value);
}

static int set_count;
static int cmd_set_cb(snd_ctl_t *ctl, const char *name,
                      snd_ctl_elem_id_t *id,
                      snd_ctl_elem_info_t *info,
                      snd_ctl_elem_value_t *value, void *user)
{
    const char *vstr = (const char *) user;
    int rc = parse_and_apply(ctl, id, info, value, vstr);
    if (rc < 0) {
        fprintf(stderr, "%s: write failed: %s\n", name, snd_strerror(rc));
        return 0;
    }
    /* re-read for confirm */
    snd_ctl_elem_read(ctl, value);
    char val[128]; format_value(info, value, val, sizeof val);
    printf("%s := %s\n", name, val);
    set_count++;
    return 0;
}

static int cmd_set(int argc, char **argv) {
    if (argc < 2) die("usage: fcp-mix set NAME VALUE");
    set_count = 0;
    snd_ctl_t *ctl = open_card();
    int rc = for_each_elem(ctl, argv[0], cmd_set_cb, (void *) argv[1]);
    snd_ctl_close(ctl);
    if (set_count == 0) die("no controls match '%s'", argv[0]);
    return rc < 0 ? 1 : 0;
}

/* phantom INPUT on|off — shortcut */
static int cmd_phantom(int argc, char **argv) {
    if (argc < 2) die("usage: fcp-mix phantom INPUT on|off");
    char glob[128];
    snprintf(glob, sizeof glob, "Line In %s Phantom Power*", argv[0]);
    char *new_argv[] = { glob, argv[1] };
    return cmd_set(2, new_argv);
}

/* mute NAME on|off — shortcut for any switch */
static int cmd_mute(int argc, char **argv) {
    if (argc < 2) die("usage: fcp-mix mute NAME on|off");
    char glob[256];
    snprintf(glob, sizeof glob, "*%s*Mute*", argv[0]);
    char *new_argv[] = { glob, argv[1] };
    return cmd_set(2, new_argv);
}

/* meter — read Level Meter (PCM iface, BYTES type, scaled 0..100ish) */
static int cmd_meter(int argc, char **argv) {
    int interval_ms = 250;
    int n_iter = 0; /* 0 = forever */
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) interval_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) n_iter = atoi(argv[++i]);
    }
    snd_ctl_t *ctl = open_card();
    snd_ctl_elem_list_t *list;
    snd_ctl_elem_list_alloca(&list);
    snd_ctl_elem_list(ctl, list);
    unsigned n = snd_ctl_elem_list_get_count(list);
    snd_ctl_elem_list_alloc_space(list, n);
    snd_ctl_elem_list(ctl, list);
    snd_ctl_elem_id_t *meter_id = NULL;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_id_alloca(&id);
    for (unsigned i = 0; i < n; i++) {
        const char *name = snd_ctl_elem_list_get_name(list, i);
        if (strstr(name, "Level Meter") != NULL) {
            snd_ctl_elem_id_malloc(&meter_id);
            snd_ctl_elem_list_get_id(list, i, meter_id);
            break;
        }
    }
    snd_ctl_elem_list_free_space(list);
    if (!meter_id) die("no Level Meter control on card %d", g_card);

    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_info_set_id(info, meter_id);
    if (snd_ctl_elem_info(ctl, info) < 0) die("meter info failed");
    unsigned cnt = snd_ctl_elem_info_get_count(info);
    long mn = snd_ctl_elem_info_get_min(info);
    long mx = snd_ctl_elem_info_get_max(info);
    snd_ctl_elem_value_set_id(value, meter_id);

    int iter = 0;
    while (n_iter == 0 || iter < n_iter) {
        if (snd_ctl_elem_read(ctl, value) < 0) { fprintf(stderr, "read meter\n"); break; }
        printf("\033[H\033[2J");
        printf("Card %d Level Meter (range %ld..%ld, %u channels):\n\n", g_card, mn, mx, cnt);
        for (unsigned i = 0; i < cnt; i++) {
            long v = snd_ctl_elem_value_get_integer(value, i);
            float p = mx > mn ? (float)(v - mn) / (mx - mn) : 0.0f;
            int bar = (int)(p * 50);
            if (bar > 50) bar = 50;
            printf("  ch %2u: ", i);
            for (int j = 0; j < bar; j++) putchar('#');
            for (int j = bar; j < 50; j++) putchar('.');
            printf(" %4ld\n", v);
        }
        fflush(stdout);
        struct timespec ts = { interval_ms / 1000, (interval_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        iter++;
    }
    snd_ctl_elem_id_free(meter_id);
    snd_ctl_close(ctl);
    return 0;
}

/* save — dump all controls to JSON */
typedef struct { json_object *arr; } save_ctx_t;
static int cmd_save_cb(snd_ctl_t *ctl, const char *name,
                       snd_ctl_elem_id_t *id,
                       snd_ctl_elem_info_t *info,
                       snd_ctl_elem_value_t *value, void *user)
{
    save_ctx_t *c = user;
    snd_ctl_elem_type_t t = snd_ctl_elem_info_get_type(info);
    unsigned n = snd_ctl_elem_info_get_count(info);
    if (n > 8) n = 8;
    json_object *o = json_object_new_object();
    json_object_object_add(o, "name", json_object_new_string(name));
    json_object *vals = json_object_new_array();
    for (unsigned i = 0; i < n; i++) {
        switch (t) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            json_object_array_add(vals, json_object_new_boolean(
                snd_ctl_elem_value_get_boolean(value, i)));
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            json_object_array_add(vals, json_object_new_int64(
                snd_ctl_elem_value_get_integer(value, i)));
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            json_object_array_add(vals, json_object_new_int(
                snd_ctl_elem_value_get_enumerated(value, i)));
            break;
        default:
            break;
        }
    }
    json_object_object_add(o, "values", vals);
    const char *type_str = "?";
    switch (t) {
    case SND_CTL_ELEM_TYPE_BOOLEAN: type_str = "bool"; break;
    case SND_CTL_ELEM_TYPE_INTEGER: type_str = "int"; break;
    case SND_CTL_ELEM_TYPE_ENUMERATED: type_str = "enum"; break;
    default: break;
    }
    json_object_object_add(o, "type", json_object_new_string(type_str));
    json_object_array_add(c->arr, o);
    return 0;
}
static int cmd_save(int argc, char **argv) {
    if (argc < 1) die("usage: fcp-mix save FILE");
    snd_ctl_t *ctl = open_card();
    save_ctx_t c = { json_object_new_array() };
    for_each_elem(ctl, NULL, cmd_save_cb, &c);
    json_object *root = json_object_new_object();
    json_object_object_add(root, "card", json_object_new_int(g_card));
    json_object_object_add(root, "controls", c.arr);
    FILE *fp = strcmp(argv[0], "-") == 0 ? stdout : fopen(argv[0], "w");
    if (!fp) die("open %s: %s", argv[0], strerror(errno));
    fprintf(fp, "%s\n", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
    if (fp != stdout) fclose(fp);
    json_object_put(root);
    snd_ctl_close(ctl);
    fprintf(stderr, "saved snapshot to %s\n", argv[0]);
    return 0;
}

/* load — apply state from JSON */
static int cmd_load(int argc, char **argv) {
    if (argc < 1) die("usage: fcp-mix load FILE");
    json_object *root = json_object_from_file(argv[0]);
    if (!root) die("could not parse %s", argv[0]);
    json_object *jarr;
    if (!json_object_object_get_ex(root, "controls", &jarr)) die("missing 'controls'");
    snd_ctl_t *ctl = open_card();
    int n = json_object_array_length(jarr);
    int applied = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        json_object *o = json_object_array_get_idx(jarr, i);
        json_object *jname, *jvals, *jtype;
        if (!json_object_object_get_ex(o, "name", &jname)) continue;
        if (!json_object_object_get_ex(o, "values", &jvals)) continue;
        if (!json_object_object_get_ex(o, "type", &jtype)) continue;
        const char *name = json_object_get_string(jname);

        snd_ctl_elem_id_t *id;
        snd_ctl_elem_info_t *info;
        snd_ctl_elem_value_t *value;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, name);
        snd_ctl_elem_info_set_id(info, id);
        if (snd_ctl_elem_info(ctl, info) < 0) { skipped++; continue; }
        snd_ctl_elem_value_set_id(value, id);
        snd_ctl_elem_type_t t = snd_ctl_elem_info_get_type(info);
        unsigned cnt = snd_ctl_elem_info_get_count(info);
        if (cnt > 8) cnt = 8;
        int vlen = json_object_array_length(jvals);
        for (unsigned k = 0; k < cnt && (int) k < vlen; k++) {
            json_object *jv = json_object_array_get_idx(jvals, k);
            switch (t) {
            case SND_CTL_ELEM_TYPE_BOOLEAN:
                snd_ctl_elem_value_set_boolean(value, k, json_object_get_boolean(jv));
                break;
            case SND_CTL_ELEM_TYPE_INTEGER:
                snd_ctl_elem_value_set_integer(value, k, json_object_get_int64(jv));
                break;
            case SND_CTL_ELEM_TYPE_ENUMERATED:
                snd_ctl_elem_value_set_enumerated(value, k, (unsigned) json_object_get_int(jv));
                break;
            default: break;
            }
        }
        if (snd_ctl_elem_write(ctl, value) == 0) applied++;
        else skipped++;
    }
    json_object_put(root);
    snd_ctl_close(ctl);
    fprintf(stderr, "applied %d controls, skipped %d\n", applied, skipped);
    return 0;
}

/* ---- main ---- */

static const struct {
    const char *name;
    int (*fn)(int, char **);
    const char *desc;
} CMDS[] = {
    { "list",    cmd_list,    "list controls grouped by kind" },
    { "get",     cmd_get,     "read one or more controls (glob OK)" },
    { "set",     cmd_set,     "write a control (glob OK; auto-detect type)" },
    { "phantom", cmd_phantom, "shortcut: phantom INPUT on|off" },
    { "mute",    cmd_mute,    "shortcut: mute NAME on|off|toggle" },
    { "meter",   cmd_meter,   "live peak meter (-i SECONDS, -n COUNT)" },
    { "save",    cmd_save,    "save mixer state to JSON" },
    { "load",    cmd_load,    "load mixer state from JSON" },
    { NULL, NULL, NULL }
};

static void usage_main(void) {
    fprintf(stderr,
        "fcp-mix — user-friendly CLI for Focusrite Scarlett mixer controls\n"
        "\n"
        "Usage: fcp-mix [-c CARD] [--json] COMMAND [ARGS...]\n"
        "\n"
        "Commands:\n");
    for (int i = 0; CMDS[i].name; i++)
        fprintf(stderr, "  %-10s  %s\n", CMDS[i].name, CMDS[i].desc);
    fprintf(stderr,
        "\n"
        "Names support shell-glob: e.g. \"Line In * Gain*\", \"Mix A Input *\"\n"
        "Values: bool=on/off/toggle ; int=N or N.NdB ; enum=index or label\n");
}

int main(int argc, char **argv) {
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-c") && argi + 1 < argc) {
            g_card = atoi(argv[argi + 1]); argi += 2;
        } else if (!strcmp(argv[argi], "--json")) {
            g_json = 1; argi++;
        } else if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
            usage_main(); return 0;
        } else break;
    }
    if (g_card < 0) g_card = find_scarlett_card();
    if (g_card < 0) die("no Scarlett card found; use -c CARD");

    if (argi >= argc) { usage_main(); return 1; }
    const char *cmd = argv[argi++];
    for (int i = 0; CMDS[i].name; i++)
        if (!strcmp(CMDS[i].name, cmd))
            return CMDS[i].fn(argc - argi, argv + argi);
    fprintf(stderr, "unknown command: %s\n", cmd);
    usage_main();
    return 1;
}
