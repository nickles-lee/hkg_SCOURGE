/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Projection+ settings parser and distance rounding. See settings.h. Offline-tested only.
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>

void hn_settings_default(hn_settings *s)
{
    s->aa_steps = s->cp_steps = HN_STEPS_BALANCED;
    s->aa_units = s->cp_units = HN_UNITS_CAR;
}

static bool parse_steps(const char *v, hn_steps *out)
{
    if (!strcmp(v, "balanced")) { *out = HN_STEPS_BALANCED; return true; }
    if (!strcmp(v, "precise"))  { *out = HN_STEPS_PRECISE;  return true; }
    if (!strcmp(v, "calm"))     { *out = HN_STEPS_CALM;     return true; }
    return false;
}

static bool parse_units(const char *v, hn_units *out)
{
    if (!strcmp(v, "car"))      { *out = HN_UNITS_CAR;      return true; }
    if (!strcmp(v, "metric"))   { *out = HN_UNITS_METRIC;   return true; }
    if (!strcmp(v, "imperial")) { *out = HN_UNITS_IMPERIAL; return true; }
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

int hn_settings_load(hn_settings *s, const char *path)
{
    hn_settings_default(s);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return -1;
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof line, f) && lines++ < 256) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!strcmp(key, "aa_steps"))      parse_steps(val, &s->aa_steps);
        else if (!strcmp(key, "cp_steps")) parse_steps(val, &s->cp_steps);
        else if (!strcmp(key, "aa_units")) parse_units(val, &s->aa_units);
        else if (!strcmp(key, "cp_units")) parse_units(val, &s->cp_units);
    }
    fclose(f);
    return 0;
}

const char *hn_steps_name(hn_steps s)
{
    return s == HN_STEPS_PRECISE ? "precise" : s == HN_STEPS_CALM ? "calm" : "balanced";
}

const char *hn_units_name(hn_units u)
{
    return u == HN_UNITS_METRIC ? "metric" : u == HN_UNITS_IMPERIAL ? "imperial" : "car";
}

static int round_to(int v, int step) { return ((v + step / 2) / step) * step; }

hn_distance hn_round_distance(int m, hn_steps steps, hn_units units)
{
    hn_distance r;
    if (m < 0) m = 0;
    if (m > 0xffff) m = 0xffff;

    /* Imperial (car-UNVERIFIED): only the ft unit code (3) is known, so feet are used up to
     * 0.1 mi (161 m); beyond that, until a mile code is found on the car, fall through to metric. */
    if (units == HN_UNITS_IMPERIAL && m < 161) {
        int ft = (m * 328084 + 50000) / 100000;
        ft = round_to(ft, steps == HN_STEPS_CALM ? 50 : 10);
        r.list_b0 = 0x03;
        r.list_val = ft;
        r.turn_m = (ft * 100000 + 164042) / 328084;
        return r;
    }

    /* "car" is metric for now (the AU car's unit setting is not read yet). */
    if (m >= 1000) {
        /* HUD/cluster carry tenths of a km only, so every preset uses 0.1 km here
         * (byte-identical to the car-proven 2026-11-27 build). */
        int tenths_total = (m + 50) / 100;
        r.list_b0 = (unsigned char)(((tenths_total % 10) << 4) | 0x01);
        r.list_val = tenths_total / 10;
        r.turn_m = tenths_total * 100;
        return r;
    }
    int d = m;
    if (steps == HN_STEPS_PRECISE) {
        /* 1 m steps all the way to 999 m */
    } else if (d >= 100 || steps == HN_STEPS_CALM) {
        d = round_to(d, 10);
        if (d > 990) d = 990;
    }
    r.list_b0 = 0x00;
    r.list_val = d;
    r.turn_m = d;
    return r;
}
