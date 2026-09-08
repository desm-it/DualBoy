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
#define NDS_HEADER_END 0x1000U
#define NDS_UNIT_CODE_OFFSET 0x12U
#define NDS_UNIT_CODE_DS 0x00U
#define NDS_UNIT_CODE_DSI_ENHANCED 0x02U

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

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t get_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static uint16_t nds_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = UINT16_C(0xffff);
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned bit;

        crc = (uint16_t)(crc ^ data[index]);
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                      ? (uint16_t)((crc >> 1U) ^ UINT16_C(0xa001))
                      : (uint16_t)(crc >> 1U);
        }
    }
    return crc;
}

static bool nds_section_is_valid(size_t rom_size,
                                 uint32_t offset,
                                 uint32_t section_size)
{
    return offset >= UINT32_C(0x200) && section_size != 0U &&
           (size_t)offset <= rom_size &&
           (size_t)section_size <= rom_size - (size_t)offset;
}

static bool has_valid_nds_header(const uint8_t *data, size_t size)
{
    const uint32_t arm9_offset = get_u32_le(data + 0x20U);
    const uint32_t arm9_size = get_u32_le(data + 0x2cU);
    const uint32_t arm7_offset = get_u32_le(data + 0x30U);
    const uint32_t arm7_size = get_u32_le(data + 0x3cU);

    /* UnitCode 2 cartridges retain a DS-compatible partition and are accepted
     * in DS mode. UnitCode 3 is DSi-exclusive and outside DualBoy's scope. */
    return size >= NDS_HEADER_END &&
           (data[NDS_UNIT_CODE_OFFSET] == NDS_UNIT_CODE_DS ||
            data[NDS_UNIT_CODE_OFFSET] == NDS_UNIT_CODE_DSI_ENHANCED) &&
           memcmp(data + 0xc0U, gba_logo, sizeof(gba_logo)) == 0 &&
           get_u16_le(data + 0x15cU) == nds_crc16(data + 0xc0U,
                                                  sizeof(gba_logo)) &&
           get_u16_le(data + 0x15eU) == nds_crc16(data, 0x15eU) &&
           nds_section_is_valid(size, arm9_offset, arm9_size) &&
           nds_section_is_valid(size, arm7_offset, arm7_size);
}

struct dualboy_detection dualboy_detect_content(const uint8_t *data, size_t size)
{
    struct dualboy_detection result = {
        DUALBOY_PLATFORM_INVALID,
        DUALBOY_DETECT_UNKNOWN_HEADER,
        "content does not contain a valid GB/GBC, GBA, or NDS cartridge header",
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
    if (has_valid_nds_header(data, size)) {
        result.platform = DUALBOY_PLATFORM_NDS;
        result.error = DUALBOY_DETECT_OK;
        result.message = "Nintendo DS";
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
    case DUALBOY_PLATFORM_NDS:
        return DUALBOY_ENGINE_MELONDS;
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
    case DUALBOY_PLATFORM_NDS:
        return "NDS";
    case DUALBOY_PLATFORM_INVALID:
    default:
        return "invalid";
    }
}
