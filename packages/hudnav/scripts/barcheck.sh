#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host check: our bar math vs the Numen worked example (right turn at 250 m -> 0x50 = 80%).
cd "$(dirname "$0")/.."
cat > /tmp/hn_barcheck.c <<'EOF'
#include "../src/hudnav.h"
#include <stdio.h>
int main(void) {
    hn_config c; hn_config_default(&c);
    printf("hn_bar(250,457)=%d (0x%02x)  [Numen IONIQ6 example: 0x50=80%%]\n",
           hn_bar(250,c.bar_scale_m), hn_bar(250,c.bar_scale_m));
    printf("hn_bar(250,300)=%d (0x%02x)  [~300m near-view scale]\n",
           hn_bar(250,300), hn_bar(250,300));
    return 0;
}
EOF
gcc -std=c99 -I src -o /tmp/hn_barcheck /tmp/hn_barcheck.c src/convert.c 2>/dev/null || gcc -std=c99 -o /tmp/hn_barcheck /tmp/hn_barcheck.c src/convert.c
/tmp/hn_barcheck
