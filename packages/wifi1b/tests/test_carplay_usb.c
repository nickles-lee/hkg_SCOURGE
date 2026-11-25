/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_usb.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    bool present;

    if (argc != 3) return 2;
    present = cp_usb_apple_present(argv[1]);
    printf("apple_present=%d\n", present ? 1 : 0);
    return present == (argv[2][0] == '1') ? 0 : 1;
}
