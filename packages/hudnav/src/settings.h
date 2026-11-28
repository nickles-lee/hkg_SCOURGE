/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Projection+ settings (packages/projection/design/BACKEND_CONTRACT.md §2):
 * per-source distance steps and units read from <kit>/settings.conf, plus the distance rounding
 * used by the CAN backend. Zero values are the car-proven defaults (balanced, car units), so a
 * missing or broken settings file never changes behaviour. Offline-tested only.
 */
#ifndef HUDNAV_SETTINGS_H
#define HUDNAV_SETTINGS_H

#include <stdbool.h>

typedef enum { HN_STEPS_BALANCED = 0, HN_STEPS_PRECISE, HN_STEPS_CALM } hn_steps;
typedef enum { HN_UNITS_CAR = 0, HN_UNITS_METRIC, HN_UNITS_IMPERIAL } hn_units;

typedef struct {
    hn_steps aa_steps, cp_steps;
    hn_units aa_units, cp_units;
} hn_settings;

/* What the CAN backend sends for one maneuver distance. */
typedef struct {
    int           turn_m;   /* 0x4CC B4/B5: metres (rounded to what the list shows) */
    unsigned char list_b0;  /* 0x63E B0: (tenths << 4) | unit (0 m, 1 km, 3 ft) */
    int           list_val; /* 0x63E B2/B3: whole metres, km or feet */
} hn_distance;

void hn_settings_default(hn_settings *s);
/* Returns 0 when the file was read, -1 when it is missing/unreadable (defaults kept).
 * Unknown keys and invalid values are ignored (value stays at its default). */
int hn_settings_load(hn_settings *s, const char *path);
hn_distance hn_round_distance(int metres, hn_steps steps, hn_units units);
const char *hn_steps_name(hn_steps s);
const char *hn_units_name(hn_units u);

#endif
