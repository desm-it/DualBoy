/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ROM_SIZE 0x8000U
#define TEST_AUDIO_CAPACITY 20000U
#define TEST_BOOT_FRAME_LIMIT 240U

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

/* The fixed cartridge-header logo required by Game Boy hardware. */
static const uint8_t test_gb_logo[48] = {
    0xCEU, 0xEDU, 0x66U, 0x66U, 0xCCU, 0x0DU, 0x00U, 0x0BU,
    0x03U, 0x73U, 0x00U, 0x83U, 0x00U, 0x0CU, 0x00U, 0x0DU,
    0x00U, 0x08U, 0x11U, 0x1FU, 0x88U, 0x89U, 0x00U, 0x0EU,
    0xDCU, 0xCCU, 0x6EU, 0xE6U, 0xDDU, 0xDDU, 0xD9U, 0x99U,
    0xBBU, 0xBBU, 0x67U, 0x63U, 0x6EU, 0x0EU, 0xECU, 0xCCU,
    0xDDU, 0xDCU, 0x99U, 0x9FU, 0xBBU, 0xB9U, 0x33U, 0x3EU,
};

static void make_test_rom(uint8_t *rom, bool color)
{
    uint8_t checksum = 0U;
    size_t index;

    memset(rom, 0, TEST_ROM_SIZE);
    rom[0x100U] = 0x00U; /* nop */
    rom[0x101U] = 0xC3U; /* jp $0150 */
    rom[0x102U] = 0x50U;
    rom[0x103U] = 0x01U;
    memcpy(rom + 0x104U, test_gb_logo, sizeof(test_gb_logo));
    memcpy(rom + 0x134U, "DUALBOY TEST", 12U);
    rom[0x143U] = color ? 0x80U : 0x00U;
    rom[0x146U] = 0x00U;
    rom[0x147U] = 0x10U; /* MBC3 + timer + RAM + battery */
    rom[0x148U] = 0x00U; /* 32 KiB ROM */
    rom[0x149U] = 0x03U; /* 32 KiB RAM */

    /* Enable cartridge RAM, write a marker, then initiate a serial transfer. */
    rom[0x150U] = 0x3EU; /* ld a, $0a */
    rom[0x151U] = 0x0AU;
    rom[0x152U] = 0xEAU; /* ld ($0000), a */
    rom[0x153U] = 0x00U;
    rom[0x154U] = 0x00U;
    rom[0x155U] = 0x3EU; /* ld a, $42 */
    rom[0x156U] = 0x42U;
    rom[0x157U] = 0xEAU; /* ld ($a000), a */
    rom[0x158U] = 0x00U;
    rom[0x159U] = 0xA0U;
    rom[0x15AU] = 0x3EU; /* ld a, $55 */
    rom[0x15BU] = 0x55U;
    rom[0x15CU] = 0xE0U; /* ldh ($01), a -- SB */
    rom[0x15DU] = 0x01U;
    rom[0x15EU] = 0x3EU; /* ld a, $81 */
    rom[0x15FU] = 0x81U;
    rom[0x160U] = 0xE0U; /* ldh ($02), a -- SC */
    rom[0x161U] = 0x02U;
    rom[0x162U] = 0x18U; /* jr $0162 */
    rom[0x163U] = 0xFEU;

    for (index = 0x134U; index <= 0x14CU; ++index) {
        checksum = (uint8_t)(checksum - rom[index] - 1U);
    }
    rom[0x14DU] = checksum;
}

static bool run_platform_case(enum dualboy_platform platform)
{
    const struct dualboy_engine_ops *operations = dualboy_sameboy_engine();
    const struct dualboy_engine_config config = {
        .audio_sample_rate = 48000U,
    };
    struct dualboy_rom content = {0};
    struct dualboy_video_frame frames[2];
    uint8_t rom[TEST_ROM_SIZE];
    void *pair = NULL;
    void *memory[2][2] = {{NULL, NULL}, {NULL, NULL}};
    void *memory_again = NULL;
    size_t memory_size[2][2] = {{0U, 0U}, {0U, 0U}};
    size_t memory_size_again = 0U;
    uint8_t *machine_state[2] = {NULL, NULL};
    size_t machine_state_size[2] = {0U, 0U};
    uint8_t *link_state = NULL;
    uint8_t *link_state_copy = NULL;
    size_t link_state_size;
    int16_t *audio = NULL;
    size_t used = 0U;
    size_t audio_frames;
    char error[256] = {0};
    unsigned index;
    bool marker_written = false;

    CHECK(operations != NULL);
    CHECK(operations->family == DUALBOY_ENGINE_SAMEBOY);
    make_test_rom(rom, platform == DUALBOY_PLATFORM_GBC);
    content.platform = platform;
    content.data = rom;
    content.size = sizeof(rom);
    content.path = platform == DUALBOY_PLATFORM_GBC ? "synthetic.gbc" :
                                                     "synthetic.gb";

    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    CHECK(pair != NULL);
    CHECK(operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    CHECK(operations->load_rom(pair, 1U, &content, error, sizeof(error)));
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    CHECK(operations->set_link(pair, true, error, sizeof(error)));

    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_SAVE_RAM,
                                      &memory[index][0],
                                      &memory_size[index][0]));
        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_RTC,
                                      &memory[index][1],
                                      &memory_size[index][1]));
        CHECK(memory[index][0] != NULL);
        CHECK(memory_size[index][0] == 32768U);
        CHECK(memory[index][1] != NULL);
        CHECK(memory_size[index][1] > 0U);
        CHECK(!operations->memory_dirty(pair, index));
    }
    CHECK(memory[0][0] != memory[1][0]);
    CHECK(memory[0][1] != memory[1][1]);
    CHECK(memory_size[0][1] == memory_size[1][1]);
    CHECK(operations->memory_info(pair,
                                  0U,
                                  DUALBOY_MEMORY_RTC,
                                  &memory_again,
                                  &memory_size_again));
    CHECK(memory_again == memory[0][1]);
    CHECK(memory_size_again == memory_size[0][1]);

    operations->set_input(pair,
                          0U,
                          (uint16_t)(DUALBOY_BUTTON_A |
                                     DUALBOY_BUTTON_RIGHT));
    operations->set_input(pair,
                          1U,
                          (uint16_t)(DUALBOY_BUTTON_B |
                                     DUALBOY_BUTTON_LEFT));
    for (index = 0U; index < TEST_BOOT_FRAME_LIMIT; ++index) {
        CHECK(operations->run_frame(pair, error, sizeof(error)));
        if (((const uint8_t *)memory[0][0])[0] == 0x42U &&
            ((const uint8_t *)memory[1][0])[0] == 0x42U) {
            marker_written = true;
            break;
        }
    }
    CHECK(marker_written);
    CHECK(operations->memory_dirty(pair, 0U));
    CHECK(operations->memory_dirty(pair, 1U));
    operations->clear_memory_dirty(pair, 0U);
    operations->clear_memory_dirty(pair, 1U);
    CHECK(!operations->memory_dirty(pair, 0U));
    CHECK(!operations->memory_dirty(pair, 1U));

    CHECK(operations->video_frame(pair, 0U, &frames[0]));
    CHECK(operations->video_frame(pair, 1U, &frames[1]));
    CHECK(frames[0].pixels != NULL && frames[1].pixels != NULL);
    CHECK(frames[0].pixels != frames[1].pixels);
    CHECK(frames[0].width == 160U && frames[0].height == 144U);
    CHECK(frames[1].width == 160U && frames[1].height == 144U);
    CHECK(frames[0].pitch == 160U * sizeof(uint32_t));
    CHECK(operations->audio_sample_rate(pair) == 48000U);

    /* Let unread audio exceed the adapter FIFO and verify its hard bound. */
    for (index = 0U; index < 30U; ++index) {
        CHECK(operations->run_frame(pair, error, sizeof(error)));
    }
    audio = (int16_t *)malloc((size_t)TEST_AUDIO_CAPACITY * (size_t)2U *
                              sizeof(*audio));
    CHECK(audio != NULL);
    audio_frames = operations->read_audio(pair, audio, TEST_AUDIO_CAPACITY);
    CHECK(audio_frames > 0U);
    CHECK(audio_frames <= 16384U);
    CHECK(operations->read_audio(pair, audio, TEST_AUDIO_CAPACITY) == 0U);

    for (index = 0U; index < 2U; ++index) {
        machine_state_size[index] =
            operations->machine_state_size(pair, index);
        CHECK(machine_state_size[index] > 0U);
        machine_state[index] =
            (uint8_t *)malloc(machine_state_size[index]);
        CHECK(machine_state[index] != NULL);
        CHECK(operations->serialize_machine(pair,
                                            index,
                                            machine_state[index],
                                            machine_state_size[index],
                                            &used));
        CHECK(used == machine_state_size[index]);
        CHECK(!operations->serialize_machine(pair,
                                             index,
                                             machine_state[index],
                                             machine_state_size[index] - 1U,
                                             &used));
    }
    link_state_size = operations->link_state_size(pair);
    CHECK(link_state_size > 0U);
    link_state = (uint8_t *)malloc(link_state_size);
    link_state_copy = (uint8_t *)malloc(link_state_size);
    CHECK(link_state != NULL && link_state_copy != NULL);
    CHECK(operations->serialize_link(pair,
                                     link_state,
                                     link_state_size,
                                     &used));
    CHECK(used == link_state_size);
    memcpy(link_state_copy, link_state, link_state_size);

    CHECK(operations->unserialize_machine(pair,
                                          0U,
                                          machine_state[0],
                                          machine_state_size[0]));
    CHECK(operations->unserialize_machine(pair,
                                          1U,
                                          machine_state[1],
                                          machine_state_size[1]));
    CHECK(operations->unserialize_link(pair, link_state, link_state_size));
    link_state[13U] = 1U;
    CHECK(!operations->unserialize_link(pair, link_state, link_state_size));
    CHECK(operations->serialize_link(pair,
                                     link_state,
                                     link_state_size,
                                     &used));
    CHECK(memcmp(link_state, link_state_copy, link_state_size) == 0);

    operations->reset(pair);
    CHECK(operations->run_frame(pair, error, sizeof(error)));
    CHECK(operations->set_link(pair, false, error, sizeof(error)));
    CHECK(operations->run_frame(pair, error, sizeof(error)));

    free(link_state_copy);
    free(link_state);
    free(machine_state[1]);
    free(machine_state[0]);
    free(audio);
    operations->destroy_pair(pair);
    return true;
}

static bool test_rejections_and_partial_teardown(void)
{
    const struct dualboy_engine_ops *operations = dualboy_sameboy_engine();
    const struct dualboy_engine_config config = {0};
    struct dualboy_rom content = {0};
    uint8_t rom[TEST_ROM_SIZE];
    void *pair = NULL;
    char error[128] = {0};

    make_test_rom(rom, false);
    content.platform = DUALBOY_PLATFORM_GB;
    content.data = rom;
    content.size = sizeof(rom);
    content.path = "partial.gb";

    CHECK(!operations->create_pair(NULL, &config, error, sizeof(error)));
    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    CHECK(!operations->set_link(pair, true, error, sizeof(error)));
    CHECK(operations->load_rom(pair, 1U, &content, error, sizeof(error)));
    CHECK(!operations->set_link(pair, true, error, sizeof(error)));
    operations->destroy_pair(pair);
    operations->destroy_pair(NULL);

    pair = NULL;
    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    content.platform = DUALBOY_PLATFORM_GBA;
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    content.platform = DUALBOY_PLATFORM_GB;
    content.size = 16U;
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    operations->destroy_pair(pair);
    return true;
}

int main(void)
{
    if (!test_rejections_and_partial_teardown() ||
        !run_platform_case(DUALBOY_PLATFORM_GB) ||
        !run_platform_case(DUALBOY_PLATFORM_GBC)) {
        return EXIT_FAILURE;
    }
    puts("SameBoy adapter tests passed");
    return EXIT_SUCCESS;
}
