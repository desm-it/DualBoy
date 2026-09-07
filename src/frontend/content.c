/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/content.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GB_HEADER_END 0x150U
#define GBA_HEADER_END 0xC0U

static const uint8_t gb_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

static const uint8_t gba_logo[156] = {
    0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21,
    0x3D, 0x84, 0x82, 0x0A, 0x84, 0xE4, 0x09, 0xAD,
    0x11, 0x24, 0x8B, 0x98, 0xC0, 0x81, 0x7F, 0x21,
    0xA3, 0x52, 0xBE, 0x19, 0x93, 0x09, 0xCE, 0x20,
    0x10, 0x46, 0x4A, 0x4A, 0xF8, 0x27, 0x31, 0xEC,
    0x58, 0xC7, 0xE8, 0x33, 0x82, 0xE3, 0xCE, 0xBF,
    0x85, 0xF4, 0xDF, 0x94, 0xCE, 0x4B, 0x09, 0xC1,
    0x94, 0x56, 0x8A, 0xC0, 0x13, 0x72, 0xA7, 0xFC,
    0x9F, 0x84, 0x4D, 0x73, 0xA3, 0xCA, 0x9A, 0x61,
    0x58, 0x97, 0xA3, 0x27, 0xFC, 0x03, 0x98, 0x76,
    0x23, 0x1D, 0xC7, 0x61, 0x03, 0x04, 0xAE, 0x56,
    0xBF, 0x38, 0x84, 0x00, 0x40, 0xA7, 0x0E, 0xFD,
    0xFF, 0x52, 0xFE, 0x03, 0x6F, 0x95, 0x30, 0xF1,
    0x97, 0xFB, 0xC0, 0x85, 0x60, 0xD6, 0x80, 0x25,
    0xA9, 0x63, 0xBE, 0x03, 0x01, 0x4E, 0x38, 0xE2,
    0xF9, 0xA2, 0x34, 0xFF, 0xBB, 0x3E, 0x03, 0x44,
    0x78, 0x00, 0x90, 0xCB, 0x88, 0x11, 0x3A, 0x94,
    0x65, 0xC0, 0x7C, 0x63, 0x87, 0xF0, 0x3C, 0xAF,
    0xD6, 0x25, 0xE4, 0x8B, 0x38, 0x0A, 0xAC, 0x72,
    0x21, 0xD4, 0xF8, 0x07,
};

static bool has_valid_gb_header(const uint8_t *data, size_t size)
{
    uint8_t checksum = 0U;
    size_t index;

    if (size < GB_HEADER_END || memcmp(data + 0x104U, gb_logo, sizeof(gb_logo)) != 0) {
        return false;
    }
    for (index = 0x134U; index <= 0x14CU; ++index) {
        checksum = (uint8_t)(checksum - data[index] - 1U);
    }
    return checksum == data[0x14DU];
}

static bool has_valid_gba_header(const uint8_t *data, size_t size)
{
    uint8_t checksum = 0U;
    size_t index;

    if (size < GBA_HEADER_END || data[0xB2U] != 0x96U ||
        memcmp(data + 0x04U, gba_logo, sizeof(gba_logo)) != 0) {
        return false;
    }
    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - data[index]);
    }
    checksum = (uint8_t)(checksum - 0x19U);
    return checksum == data[0xBDU];
}

struct dualboy_detection dualboy_detect_content(const uint8_t *data, size_t size)
{
    struct dualboy_detection result = {
        DUALBOY_PLATFORM_INVALID,
        DUALBOY_DETECT_UNKNOWN_HEADER,
        "content does not contain a valid GB/GBC or GBA cartridge header",
    };

    if (data == NULL || size == 0U) {
        result.error = DUALBOY_DETECT_EMPTY;
        result.message = "content is empty";
        return result;
    }
    if (size < GBA_HEADER_END) {
        result.error = DUALBOY_DETECT_TOO_SMALL;
        result.message = "content is too small to contain a supported cartridge header";
        return result;
    }
    if (has_valid_gba_header(data, size)) {
        result.platform = DUALBOY_PLATFORM_GBA;
        result.error = DUALBOY_DETECT_OK;
        result.message = "Game Boy Advance";
        return result;
    }
    if (has_valid_gb_header(data, size)) {
        const uint8_t cgb_flag = data[0x143U];
        result.platform = (cgb_flag == 0x80U || cgb_flag == 0xC0U)
                              ? DUALBOY_PLATFORM_GBC
                              : DUALBOY_PLATFORM_GB;
        result.error = DUALBOY_DETECT_OK;
        result.message = result.platform == DUALBOY_PLATFORM_GBC
                             ? "Game Boy Color"
                             : "Game Boy";
        return result;
    }
    return result;
}

enum dualboy_engine_family
dualboy_engine_family_for_platform(enum dualboy_platform platform)
{
    switch (platform) {
    case DUALBOY_PLATFORM_GB:
    case DUALBOY_PLATFORM_GBC:
        return DUALBOY_ENGINE_SAMEBOY;
    case DUALBOY_PLATFORM_GBA:
        return DUALBOY_ENGINE_MGBA;
    case DUALBOY_PLATFORM_INVALID:
    default:
        return DUALBOY_ENGINE_NONE;
    }
}

bool dualboy_platforms_compatible(enum dualboy_platform first,
                                  enum dualboy_platform second)
{
    const enum dualboy_engine_family first_family =
        dualboy_engine_family_for_platform(first);
    return first_family != DUALBOY_ENGINE_NONE &&
           first_family == dualboy_engine_family_for_platform(second);
}

const char *dualboy_platform_name(enum dualboy_platform platform)
{
    switch (platform) {
    case DUALBOY_PLATFORM_GB:
        return "GB";
    case DUALBOY_PLATFORM_GBC:
        return "GBC";
    case DUALBOY_PLATFORM_GBA:
        return "GBA";
    case DUALBOY_PLATFORM_INVALID:
    default:
        return "invalid";
    }
}
