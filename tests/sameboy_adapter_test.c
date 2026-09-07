/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "frontend/engine.h"
#include "frontend/save_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define PERSIST_CHECK(expression)                                               \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            goto cleanup;                                                       \
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

static void make_test_rom(uint8_t *rom,
                          bool color,
                          uint8_t serial_byte,
                          bool internal_clock)
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
    rom[0x15AU] = 0x3EU; /* ld a, serial byte */
    rom[0x15BU] = serial_byte;
    rom[0x15CU] = 0xE0U; /* ldh ($01), a -- SB */
    rom[0x15DU] = 0x01U;
    rom[0x15EU] = 0x3EU; /* ld a, transfer start + clock source */
    rom[0x15FU] = internal_clock ? 0x81U : 0x80U;
    rom[0x160U] = 0xE0U; /* ldh ($02), a -- SC */
    rom[0x161U] = 0x02U;
    rom[0x162U] = 0xF0U; /* ldh a, ($02) -- wait for transfer complete */
    rom[0x163U] = 0x02U;
    rom[0x164U] = 0xE6U; /* and $80 */
    rom[0x165U] = 0x80U;
    rom[0x166U] = 0x20U; /* jr nz, $0162 */
    rom[0x167U] = 0xFAU;
    rom[0x168U] = 0xF0U; /* ldh a, ($01) -- received peer byte */
    rom[0x169U] = 0x01U;
    rom[0x16AU] = 0xEAU; /* ld ($a001), a */
    rom[0x16BU] = 0x01U;
    rom[0x16CU] = 0xA0U;
    rom[0x16DU] = 0x18U; /* jr $016d */
    rom[0x16EU] = 0xFEU;

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
    struct dualboy_rom content[2] = {{0}};
    struct dualboy_video_frame frames[2];
    uint8_t rom[2][TEST_ROM_SIZE];
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
    /* A cable transfer has one clock source; the peer accepts external edges. */
    make_test_rom(rom[0], platform == DUALBOY_PLATFORM_GBC, 0x55U, true);
    make_test_rom(rom[1], platform == DUALBOY_PLATFORM_GBC, 0xA3U, false);
    content[0].platform = platform;
    content[0].data = rom[0];
    content[0].size = sizeof(rom[0]);
    content[0].path = platform == DUALBOY_PLATFORM_GBC ?
                          "synthetic-master.gbc" :
                          "synthetic-master.gb";
    content[1] = content[0];
    content[1].data = rom[1];
    content[1].size = sizeof(rom[1]);
    content[1].path = platform == DUALBOY_PLATFORM_GBC ?
                          "synthetic-slave.gbc" :
                          "synthetic-slave.gb";

    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    CHECK(pair != NULL);
    CHECK(operations->load_rom(pair, 0U, &content[0], error, sizeof(error)));
    CHECK(operations->load_rom(pair, 1U, &content[1], error, sizeof(error)));
    CHECK(!operations->load_rom(pair, 0U, &content[0], error, sizeof(error)));
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
    CHECK(((const uint8_t *)memory[0][0])[1] == 0xA3U);
    CHECK(((const uint8_t *)memory[1][0])[1] == 0x55U);
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

    make_test_rom(rom, false, 0x55U, true);
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

static void wrap_live_pair(struct dualboy_session *session,
                           const struct dualboy_engine_ops *operations,
                           void *pair,
                           const struct dualboy_rom content[2])
{
    unsigned machine;

    memset(session, 0, sizeof(*session));
    session->engine = operations;
    session->pair = pair;
    session->load_kind = DUALBOY_LOAD_PLAYLIST;
    session->loaded = true;
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        session->roms[machine].rom = content[machine];
    }
}

static bool test_disk_persistence_round_trip(void)
{
    const struct dualboy_engine_ops *operations = dualboy_sameboy_engine();
    const struct dualboy_engine_config config = {
        .audio_sample_rate = 48000U,
    };
    const uint8_t expected_guest = 0x42U;
    const uint8_t expected_sentinel[2] = {0x51U, 0xA2U};
    const uint8_t expected_rtc[2] = {0x19U, 0xE7U};
    struct dualboy_rom content[2] = {{0}};
    struct dualboy_session session = {0};
    struct dualboy_save_manager manager = {0};
    uint8_t rom[TEST_ROM_SIZE];
    void *pair = NULL;
    void *sram[2] = {NULL, NULL};
    void *rtc[2] = {NULL, NULL};
    size_t sram_size[2] = {0U, 0U};
    size_t rtc_size[2] = {0U, 0U};
    char cleanup_paths[4][DUALBOY_PATH_CAPACITY] = {{0}};
    char temporary_directory[] = "/tmp/dualboy-sameboy-save-XXXXXX";
    char error[256] = {0};
    bool directory_created = false;
    bool guest_wrote_both = false;
    bool success = false;
    unsigned machine;
    unsigned frame;

    make_test_rom(rom, false, 0x55U, true);
    content[0].platform = DUALBOY_PLATFORM_GB;
    content[0].data = rom;
    content[0].size = sizeof(rom);
    content[0].path = "/virtual/left/collision.gb";
    content[1] = content[0];
    content[1].path = "/virtual/right/collision.gb";

    PERSIST_CHECK(mkdtemp(temporary_directory) != NULL);
    directory_created = true;
    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 0U, &content[0], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 1U, &content[1], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->set_link(pair, true, error, sizeof(error)));
    wrap_live_pair(&session, operations, pair, content);

    /* Playlist loads are deliberately core-managed, even for SameBoy. */
    PERSIST_CHECK(dualboy_save_manager_init(&manager, &session,
                                            temporary_directory, error,
                                            sizeof(error)));
    PERSIST_CHECK(manager.paths.second_uses_collision_suffix);
    PERSIST_CHECK(strcmp(manager.paths.sram[0], manager.paths.sram[1]) != 0);
    PERSIST_CHECK(strcmp(manager.paths.rtc[0], manager.paths.rtc[1]) != 0);
    PERSIST_CHECK(strstr(manager.paths.sram[1], ".srm.2") != NULL);
    PERSIST_CHECK(strstr(manager.paths.rtc[1], ".rtc.2") != NULL);
    memcpy(cleanup_paths[0], manager.paths.sram[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[1], manager.paths.sram[1], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[2], manager.paths.rtc[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[3], manager.paths.rtc[1], DUALBOY_PATH_CAPACITY);

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[machine],
                                              &sram_size[machine]));
        PERSIST_CHECK(sram[machine] != NULL && sram_size[machine] == 32768U);
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_RTC,
                                              &rtc[machine],
                                              &rtc_size[machine]));
        PERSIST_CHECK(rtc[machine] != NULL && rtc_size[machine] != 0U);
    }
    PERSIST_CHECK(sram[0] != sram[1]);
    PERSIST_CHECK(rtc[0] != rtc[1]);

    for (frame = 0U; frame < TEST_BOOT_FRAME_LIMIT; ++frame) {
        PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
        if (((const uint8_t *)sram[0])[0] == expected_guest &&
            ((const uint8_t *)sram[1])[0] == expected_guest) {
            guest_wrote_both = true;
            break;
        }
    }
    PERSIST_CHECK(guest_wrote_both);
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        ((uint8_t *)sram[machine])[0x100U] = expected_sentinel[machine];
        ((uint8_t *)rtc[machine])[rtc_size[machine] - 1U] =
            expected_rtc[machine];
    }
    PERSIST_CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                             sizeof(error)));

    dualboy_save_manager_deinit(&manager);
    operations->destroy_pair(pair);
    pair = NULL;
    session.pair = NULL;
    session.loaded = false;

    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 0U, &content[0], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 1U, &content[1], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->set_link(pair, true, error, sizeof(error)));
    wrap_live_pair(&session, operations, pair, content);

    /* Poison fresh engine storage so success cannot come from reset defaults. */
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[machine],
                                              &sram_size[machine]));
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_RTC,
                                              &rtc[machine],
                                              &rtc_size[machine]));
        PERSIST_CHECK(sram[machine] != NULL && sram_size[machine] != 0U);
        PERSIST_CHECK(rtc[machine] != NULL && rtc_size[machine] != 0U);
        ((uint8_t *)sram[machine])[0] = 0U;
        ((uint8_t *)sram[machine])[0x100U] = 0U;
        ((uint8_t *)rtc[machine])[rtc_size[machine] - 1U] = 0U;
    }
    PERSIST_CHECK(dualboy_save_manager_init(&manager, &session,
                                            temporary_directory, error,
                                            sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(((const uint8_t *)sram[machine])[0] == expected_guest);
        PERSIST_CHECK(((const uint8_t *)sram[machine])[0x100U] ==
                      expected_sentinel[machine]);
        PERSIST_CHECK(((const uint8_t *)rtc[machine])
                          [rtc_size[machine] - 1U] == expected_rtc[machine]);
    }
    success = true;

cleanup:
    dualboy_save_manager_deinit(&manager);
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    if (directory_created) {
        for (machine = 0U; machine < 4U; ++machine) {
            if (cleanup_paths[machine][0] != '\0') {
                (void)unlink(cleanup_paths[machine]);
            }
        }
        (void)rmdir(temporary_directory);
    }
    return success;
}

int main(void)
{
    if (!test_rejections_and_partial_teardown() ||
        !run_platform_case(DUALBOY_PLATFORM_GB) ||
        !run_platform_case(DUALBOY_PLATFORM_GBC) ||
        !test_disk_persistence_round_trip()) {
        return EXIT_FAILURE;
    }
    puts("SameBoy adapter tests passed (disk save/RTC reload verified)");
    return EXIT_SUCCESS;
}
