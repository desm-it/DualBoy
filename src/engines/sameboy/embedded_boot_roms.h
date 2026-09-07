/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_SAMEBOY_EMBEDDED_BOOT_ROMS_H
#define DUALBOY_SAMEBOY_EMBEDDED_BOOT_ROMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUALBOY_SAMEBOY_DMG_BOOT_SIZE 256U
#define DUALBOY_SAMEBOY_CGB_BOOT_SIZE 2304U

/*
 * Decode the exact Expat-licensed SameBoy boot ROM artifacts documented in
 * BOOT_ROMS.md. Keeping the decoded bytes pair-owned avoids mutable globals.
 */
bool dualboy_sameboy_decode_boot_roms(
    uint8_t dmg[DUALBOY_SAMEBOY_DMG_BOOT_SIZE],
    uint8_t cgb[DUALBOY_SAMEBOY_CGB_BOOT_SIZE]);

#endif
