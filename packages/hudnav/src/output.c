/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "output.h"

static bool log_turn(void *ctx, const hn_native *n)
{
    FILE *f = ctx;
    if (n->type == HN_TYPE_ICON)
        fprintf(f, "FRAME turn icon=%d dist=%d unit=%d bar=%d near=%d\n",
                n->icon, n->dist_m, n->unit, n->bar, n->near);
    else
        fprintf(f, "FRAME turn type=%d angle=%d side=%u dist=%d unit=%d bar=%d near=%d\n",
                n->type, n->angle, n->side_street, n->dist_m, n->unit, n->bar, n->near);
    return true;
}
static bool log_clear(void *ctx)
{
    fprintf(ctx, "FRAME clear\n");
    return true;
}
static bool log_guidance(void *ctx, bool active)
{
    fprintf(ctx, "FRAME guidance=%d\n", active ? 1 : 0);
    return true;
}
static void log_destroy(void *ctx) { (void)ctx; }

hn_output hn_output_log(FILE *f)
{
    hn_output o = { f, log_turn, log_clear, log_guidance, log_destroy };
    return o;
}
