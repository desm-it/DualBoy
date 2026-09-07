/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _POSIX_C_SOURCE 200809L

#include "frontend/save_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

struct fake_pair {
    uint8_t sram[2][4];
    uint8_t rtc[2][2];
};

static bool fake_memory(void *context,
                        unsigned machine,
                        enum dualboy_memory_kind kind,
                        void **data,
                        size_t *size)
{
    struct fake_pair *pair = context;
    if (pair == NULL || machine > 1U || data == NULL || size == NULL) {
        return false;
    }
    if (kind == DUALBOY_MEMORY_SAVE_RAM) {
        *data = pair->sram[machine];
        *size = sizeof(pair->sram[machine]);
        return true;
    }
    if (kind == DUALBOY_MEMORY_RTC) {
        *data = pair->rtc[machine];
        *size = sizeof(pair->rtc[machine]);
        return true;
    }
    return false;
}

static const struct dualboy_engine_ops sameboy_ops = {
    .name = "fake SameBoy",
    .family = DUALBOY_ENGINE_SAMEBOY,
    .memory_info = fake_memory,
};

static const struct dualboy_engine_ops mgba_ops = {
    .name = "fake mGBA",
    .family = DUALBOY_ENGINE_MGBA,
    .memory_info = fake_memory,
};

static bool make_temp_directory(char path_template[])
{
    int descriptor = mkstemp(path_template);
    if (descriptor < 0 || close(descriptor) != 0 ||
        unlink(path_template) != 0) {
        return false;
    }
    return mkdir(path_template, (mode_t)0700) == 0;
}

static void fake_session(struct dualboy_session *session,
                         struct fake_pair *pair,
                         const struct dualboy_engine_ops *ops,
                         enum dualboy_load_kind load_kind,
                         const char *first,
                         const char *second)
{
    memset(session, 0, sizeof(*session));
    session->engine = ops;
    session->pair = pair;
    session->loaded = true;
    session->load_kind = load_kind;
    session->roms[0].rom.path = first;
    session->roms[1].rom.path = second;
}

static bool read_exact(const char *path, uint8_t *data, size_t size)
{
    size_t actual = 0U;
    return dualboy_read_file_filled(path, data, size, 0U, &actual) ==
               DUALBOY_PERSISTENCE_OK &&
           actual == size;
}

static bool test_same_rom_second_is_core_managed(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {{{0}}, {{0}}};
    struct dualboy_session session;
    struct dualboy_save_manager manager;
    uint8_t second_save[4] = {4U, 3U, 2U, 1U};
    void *memory = NULL;
    size_t size = 0U;
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &sameboy_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/game.gb", "/roms/game.gb");
    CHECK(dualboy_build_save_paths(directory, "/roms/game.gb", "/roms/game.gb",
                                   &manager.paths) == DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(manager.paths.sram[1], second_save,
                                    sizeof(second_save)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(memcmp(pair.sram[1], second_save, sizeof(second_save)) == 0);
    CHECK(pair.sram[0][0] == 0U);
    CHECK(dualboy_save_manager_frontend_memory(
        &manager, &session, 0U, DUALBOY_MEMORY_SAVE_RAM, &memory, &size));
    CHECK(memory == pair.sram[0] && size == sizeof(pair.sram[0]));
    CHECK(!dualboy_save_manager_frontend_memory(
        &manager, &session, 1U, DUALBOY_MEMORY_SAVE_RAM, &memory, &size));

    CHECK(unlink(manager.paths.sram[1]) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_distinct_subsystem_uses_frontend_memory(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {{{0}}, {{0}}};
    struct dualboy_session session;
    struct dualboy_save_manager manager;
    void *memory = NULL;
    size_t size = 0U;
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &sameboy_ops, DUALBOY_LOAD_SUBSYSTEM,
                 "/roms/red.gb", "/roms/blue.gbc");
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(!manager.paths.second_uses_collision_suffix);
    CHECK(dualboy_save_manager_frontend_memory(
        &manager, &session, 0U, DUALBOY_MEMORY_RTC, &memory, &size));
    CHECK(memory == pair.rtc[0]);
    CHECK(dualboy_save_manager_frontend_memory(
        &manager, &session, 1U, DUALBOY_MEMORY_SAVE_RAM, &memory, &size));
    CHECK(memory == pair.sram[1]);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_gba_legacy_copy_and_independent_flush(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {{{0}}, {{0}}};
    struct dualboy_session session;
    struct dualboy_save_manager manager;
    struct dualboy_save_paths paths;
    char legacy[DUALBOY_PATH_CAPACITY];
    uint8_t imported[4] = {1U, 2U, 3U, 4U};
    uint8_t observed[4];
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &mgba_ops, DUALBOY_LOAD_SUBSYSTEM,
                 "/roms/first.gba", "/roms/second.gba");
    CHECK(dualboy_build_save_paths(directory, "/roms/first.gba",
                                   "/roms/second.gba", &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_legacy_sav_path(paths.sram[0], legacy, sizeof(legacy)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(legacy, imported, sizeof(imported)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(memcmp(pair.sram[0], imported, sizeof(imported)) == 0);
    CHECK(read_exact(manager.paths.sram[0], observed, sizeof(observed)));
    CHECK(memcmp(observed, imported, sizeof(imported)) == 0);

    pair.sram[0][0] = 0x11U;
    pair.sram[1][0] = 0x22U;
    pair.rtc[0][0] = 0x33U;
    pair.rtc[1][0] = 0x44U;
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(read_exact(manager.paths.sram[0], observed, sizeof(observed)));
    CHECK(observed[0] == 0x11U);
    CHECK(read_exact(manager.paths.sram[1], observed, sizeof(observed)));
    CHECK(observed[0] == 0x22U);

    CHECK(unlink(legacy) == 0);
    CHECK(unlink(manager.paths.sram[0]) == 0);
    CHECK(unlink(manager.paths.sram[1]) == 0);
    CHECK(unlink(manager.paths.rtc[0]) == 0);
    CHECK(unlink(manager.paths.rtc[1]) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

int main(void)
{
    if (!test_same_rom_second_is_core_managed() ||
        !test_distinct_subsystem_uses_frontend_memory() ||
        !test_gba_legacy_copy_and_independent_flush()) {
        return EXIT_FAILURE;
    }
    puts("save manager tests passed");
    return EXIT_SUCCESS;
}
