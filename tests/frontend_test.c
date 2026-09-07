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

static bool test_detection(void)
{
    uint8_t gb[0x8000U];
    uint8_t gbc[0x8000U];
    uint8_t gba[0x200U];
    struct dualboy_detection detection;

    make_gb_header(gb, sizeof(gb), false);
    make_gb_header(gbc, sizeof(gbc), true);
    make_gba_header(gba, sizeof(gba));

    detection = dualboy_detect_content(gb, sizeof(gb));
    CHECK(detection.error == DUALBOY_DETECT_OK);
    CHECK(detection.platform == DUALBOY_PLATFORM_GB);
    detection = dualboy_detect_content(gbc, sizeof(gbc));
    CHECK(detection.platform == DUALBOY_PLATFORM_GBC);
    detection = dualboy_detect_content(gba, sizeof(gba));
    CHECK(detection.platform == DUALBOY_PLATFORM_GBA);

    gb[0x14DU] ^= 1U;
    CHECK(dualboy_detect_content(gb, sizeof(gb)).platform ==
          DUALBOY_PLATFORM_INVALID);
    gba[0xB2U] = 0U;
    CHECK(dualboy_detect_content(gba, sizeof(gba)).platform ==
          DUALBOY_PLATFORM_INVALID);
    CHECK(dualboy_detect_content(NULL, 0U).error == DUALBOY_DETECT_EMPTY);

    CHECK(dualboy_platforms_compatible(DUALBOY_PLATFORM_GB,
                                       DUALBOY_PLATFORM_GBC));
    CHECK(!dualboy_platforms_compatible(DUALBOY_PLATFORM_GB,
                                        DUALBOY_PLATFORM_GBA));
    CHECK(dualboy_engine_family_for_platform(DUALBOY_PLATFORM_GBA) ==
          DUALBOY_ENGINE_MGBA);
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

int main(void)
{
    if (!test_detection() || !test_compositor()) {
        return EXIT_FAILURE;
    }
    puts("frontend tests passed");
    return EXIT_SUCCESS;
}
