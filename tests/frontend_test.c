/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/compositor.h"
#include "frontend/content.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t test_gb_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

static const uint8_t test_gba_logo[156] = {
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

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

static void make_gb_header(uint8_t *rom, size_t size, bool color)
{
    uint8_t checksum = 0U;
    size_t index;
    memset(rom, 0, size);
    memcpy(rom + 0x104U, test_gb_logo, sizeof(test_gb_logo));
    memcpy(rom + 0x134U, "DUALBOY TEST", 12U);
    rom[0x143U] = color ? 0x80U : 0x00U;
    for (index = 0x134U; index <= 0x14CU; ++index) {
        checksum = (uint8_t)(checksum - rom[index] - 1U);
    }
    rom[0x14DU] = checksum;
}

static void make_gba_header(uint8_t *rom, size_t size)
{
    uint8_t checksum = 0U;
    size_t index;
    memset(rom, 0, size);
    memcpy(rom + 0x04U, test_gba_logo, sizeof(test_gba_logo));
    memcpy(rom + 0xA0U, "DUALBOYTEST", 11U);
    rom[0xB2U] = 0x96U;
    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
}

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
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

static void make_nds_header(uint8_t *rom, size_t size)
{
    memset(rom, 0, size);
    memcpy(rom, "DUALBOY NDS ", 12U);
    memcpy(rom + 0x0cU, "DBNE", 4U);
    memcpy(rom + 0x10U, "DB", 2U);
    put_u32_le(rom + 0x20U, UINT32_C(0x200));
    put_u32_le(rom + 0x24U, UINT32_C(0x02000000));
    put_u32_le(rom + 0x28U, UINT32_C(0x02000000));
    put_u32_le(rom + 0x2cU, UINT32_C(4));
    put_u32_le(rom + 0x30U, UINT32_C(0x204));
    put_u32_le(rom + 0x34U, UINT32_C(0x03800000));
    put_u32_le(rom + 0x38U, UINT32_C(0x03800000));
    put_u32_le(rom + 0x3cU, UINT32_C(4));
    put_u32_le(rom + 0x80U, (uint32_t)size);
    put_u32_le(rom + 0x84U, UINT32_C(0x200));
    memcpy(rom + 0xc0U, test_gba_logo, sizeof(test_gba_logo));
    put_u16_le(rom + 0x15cU,
               nds_crc16(rom + 0xc0U, sizeof(test_gba_logo)));
    put_u16_le(rom + 0x15eU, nds_crc16(rom, 0x15eU));
}

static bool test_detection(void)
{
    uint8_t gb[0x8000U];
    uint8_t gbc[0x8000U];
    uint8_t gba[0x200U];
    uint8_t nds[0x1000U];
    struct dualboy_detection detection;

    make_gb_header(gb, sizeof(gb), false);
    make_gb_header(gbc, sizeof(gbc), true);
    make_gba_header(gba, sizeof(gba));
    make_nds_header(nds, sizeof(nds));

    detection = dualboy_detect_content(gb, sizeof(gb));
    CHECK(detection.error == DUALBOY_DETECT_OK);
    CHECK(detection.platform == DUALBOY_PLATFORM_GB);
    detection = dualboy_detect_content(gbc, sizeof(gbc));
    CHECK(detection.platform == DUALBOY_PLATFORM_GBC);
    detection = dualboy_detect_content(gba, sizeof(gba));
    CHECK(detection.platform == DUALBOY_PLATFORM_GBA);
    detection = dualboy_detect_content(nds, sizeof(nds));
    CHECK(detection.error == DUALBOY_DETECT_OK);
    CHECK(detection.platform == DUALBOY_PLATFORM_NDS);
    nds[0x12U] = 2U;
    put_u16_le(nds + 0x15eU, nds_crc16(nds, 0x15eU));
    CHECK(dualboy_detect_content(nds, sizeof(nds)).platform ==
          DUALBOY_PLATFORM_NDS);
    nds[0x12U] = 3U;
    put_u16_le(nds + 0x15eU, nds_crc16(nds, 0x15eU));
    CHECK(dualboy_detect_content(nds, sizeof(nds)).platform ==
          DUALBOY_PLATFORM_INVALID);
    nds[0x12U] = 0U;
    put_u16_le(nds + 0x15eU, nds_crc16(nds, 0x15eU));

    gb[0x14DU] ^= 1U;
    CHECK(dualboy_detect_content(gb, sizeof(gb)).platform ==
          DUALBOY_PLATFORM_INVALID);
    gba[0xB2U] = 0U;
    CHECK(dualboy_detect_content(gba, sizeof(gba)).platform ==
          DUALBOY_PLATFORM_INVALID);
    CHECK(dualboy_detect_content(NULL, 0U).error == DUALBOY_DETECT_EMPTY);
    nds[0x15eU] ^= 1U;
    CHECK(dualboy_detect_content(nds, sizeof(nds)).platform ==
          DUALBOY_PLATFORM_INVALID);
    nds[0x15eU] ^= 1U;
    put_u32_le(nds + 0x30U, UINT32_C(0x1000));
    put_u16_le(nds + 0x15eU, nds_crc16(nds, 0x15eU));
    CHECK(dualboy_detect_content(nds, sizeof(nds)).platform ==
          DUALBOY_PLATFORM_INVALID);

    CHECK(dualboy_platforms_compatible(DUALBOY_PLATFORM_GB,
                                       DUALBOY_PLATFORM_GBC));
    CHECK(!dualboy_platforms_compatible(DUALBOY_PLATFORM_GB,
                                        DUALBOY_PLATFORM_GBA));
    CHECK(dualboy_platforms_compatible(DUALBOY_PLATFORM_NDS,
                                       DUALBOY_PLATFORM_NDS));
    CHECK(!dualboy_platforms_compatible(DUALBOY_PLATFORM_NDS,
                                        DUALBOY_PLATFORM_GBA));
    CHECK(dualboy_engine_family_for_platform(DUALBOY_PLATFORM_GBA) ==
          DUALBOY_ENGINE_MGBA);
    CHECK(dualboy_engine_family_for_platform(DUALBOY_PLATFORM_NDS) ==
          DUALBOY_ENGINE_MELONDS);
    CHECK(strcmp(dualboy_platform_name(DUALBOY_PLATFORM_NDS), "NDS") == 0);
    return true;
}

static bool check_pixels(const uint32_t *actual,
                         const uint32_t *expected,
                         size_t count)
{
    return memcmp(actual, expected, count * sizeof(*actual)) == 0;
}

static bool test_compositor(void)
{
    const uint32_t first[] = {1U, 2U, 3U, 4U};
    const uint32_t second[] = {5U, 6U, 7U, 8U};
    const struct dualboy_video_frame frames[2] = {
        {first, 2U, 2U, 2U * sizeof(uint32_t)},
        {second, 2U, 2U, 2U * sizeof(uint32_t)},
    };
    struct dualboy_compositor_config config = {
        DUALBOY_MODE_DUAL,
        DUALBOY_LAYOUT_SIDE_BY_SIDE,
        false,
    };
    struct dualboy_video_frame result;
    uint32_t output[8] = {0U};
    const uint32_t side_expected[] = {1U, 2U, 5U, 6U, 3U, 4U, 7U, 8U};
    const uint32_t swapped_expected[] = {5U, 6U, 1U, 2U, 7U, 8U, 3U, 4U};
    const uint32_t top_expected[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

    CHECK(dualboy_compose_frame(output, 8U, frames, &config, &result));
    CHECK(result.width == 4U && result.height == 2U);
    CHECK(check_pixels(output, side_expected, 8U));

    config.swap_players = true;
    CHECK(dualboy_compose_frame(output, 8U, frames, &config, &result));
    CHECK(check_pixels(output, swapped_expected, 8U));
    CHECK(dualboy_machine_for_port(0U, true) == 1U);
    CHECK(dualboy_machine_for_port(1U, true) == 0U);

    config.swap_players = false;
    config.layout = DUALBOY_LAYOUT_TOP_BOTTOM;
    CHECK(dualboy_compose_frame(output, 8U, frames, &config, &result));
    CHECK(result.width == 2U && result.height == 4U);
    CHECK(check_pixels(output, top_expected, 8U));

    config.mode = DUALBOY_MODE_PLAYER2;
    CHECK(dualboy_compose_frame(output, 8U, frames, &config, &result));
    CHECK(result.width == 2U && result.height == 2U);
    CHECK(check_pixels(output, second, 4U));
    config.swap_players = true;
    CHECK(dualboy_compose_frame(output, 8U, frames, &config, &result));
    CHECK(check_pixels(output, first, 4U));

    CHECK(!dualboy_compose_frame(output, 3U, frames, &config, &result));
    return true;
}

static bool test_nds_geometry_and_point_mapping(void)
{
    const uint32_t placeholder[1] = {0U};
    const struct dualboy_video_frame frames[2] = {
        {placeholder, 256U, 384U, 256U * sizeof(uint32_t)},
        {placeholder, 256U, 384U, 256U * sizeof(uint32_t)},
    };
    struct dualboy_compositor_config config = {
        DUALBOY_MODE_DUAL,
        DUALBOY_LAYOUT_SIDE_BY_SIDE,
        false,
    };
    struct dualboy_geometry geometry;
    unsigned port;
    unsigned x;
    unsigned y;

    CHECK(dualboy_compositor_geometry(frames, &config, &geometry));
    CHECK(geometry.width == 512U && geometry.height == 384U);
    CHECK(dualboy_compositor_map_point(frames, &config, 10U, 200U,
                                       &port, &x, &y));
    CHECK(port == 0U && x == 10U && y == 200U);
    CHECK(dualboy_compositor_map_point(frames, &config, 300U, 300U,
                                       &port, &x, &y));
    CHECK(port == 1U && x == 44U && y == 300U);

    config.swap_players = true;
    CHECK(dualboy_compositor_map_point(frames, &config, 10U, 200U,
                                       &port, &x, &y));
    CHECK(port == 0U && x == 10U && y == 200U);

    config.swap_players = false;
    config.layout = DUALBOY_LAYOUT_TOP_BOTTOM;
    CHECK(dualboy_compositor_geometry(frames, &config, &geometry));
    CHECK(geometry.width == 256U && geometry.height == 768U);
    CHECK(dualboy_compositor_map_point(frames, &config, 100U, 600U,
                                       &port, &x, &y));
    CHECK(port == 1U && x == 100U && y == 216U);

    config.mode = DUALBOY_MODE_PLAYER2;
    CHECK(dualboy_compositor_geometry(frames, &config, &geometry));
    CHECK(geometry.width == 256U && geometry.height == 384U);
    CHECK(dualboy_compositor_map_point(frames, &config, 255U, 383U,
                                       &port, &x, &y));
    CHECK(port == 1U && x == 255U && y == 383U);
    CHECK(!dualboy_compositor_map_point(frames, &config, 256U, 383U,
                                        &port, &x, &y));
    return true;
}

int main(void)
{
    if (!test_detection() || !test_compositor() ||
        !test_nds_geometry_and_point_mapping()) {
        return EXIT_FAILURE;
    }
    puts("frontend tests passed");
    return EXIT_SUCCESS;
}
