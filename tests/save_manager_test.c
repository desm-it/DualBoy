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

#define TEST_NDS_FIRMWARE_SIZE (128U * 1024U)

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

struct fake_pair {
    uint8_t sram[DUALBOY_MACHINE_COUNT][4];
    uint8_t rtc[DUALBOY_MACHINE_COUNT][16];
    bool extent_known[DUALBOY_MACHINE_COUNT][DUALBOY_MEMORY_KIND_COUNT];
    size_t extent[DUALBOY_MACHINE_COUNT][DUALBOY_MEMORY_KIND_COUNT];
    bool dirty[DUALBOY_MACHINE_COUNT];
};

struct fake_melonds_pair {
    uint8_t sram[DUALBOY_MACHINE_COUNT][4];
    uint8_t firmware[DUALBOY_MACHINE_COUNT][TEST_NDS_FIRMWARE_SIZE];
    unsigned persistent_load_calls;
};

struct large_fake_pair {
    uint8_t sram[DUALBOY_MACHINE_COUNT][32U * 1024U];
    uint8_t rtc[DUALBOY_MACHINE_COUNT][16];
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

static bool fake_melonds_memory(void *context,
                                unsigned machine,
                                enum dualboy_memory_kind kind,
                                void **data,
                                size_t *size)
{
    struct fake_melonds_pair *pair = context;

    if (pair == NULL || machine >= DUALBOY_MACHINE_COUNT || data == NULL ||
        size == NULL) {
        return false;
    }
    if (kind == DUALBOY_MEMORY_SAVE_RAM) {
        *data = pair->sram[machine];
        *size = sizeof(pair->sram[machine]);
        return true;
    }
    if (kind == DUALBOY_MEMORY_FIRMWARE) {
        *data = pair->firmware[machine];
        *size = sizeof(pair->firmware[machine]);
        return true;
    }
    return false;
}

static void fake_persistent_memory_loaded(void *context)
{
    struct fake_melonds_pair *pair = context;

    if (pair != NULL) {
        ++pair->persistent_load_calls;
    }
}

static bool fake_melonds_validate_persistent_memory(
    const void *context,
    unsigned machine,
    enum dualboy_memory_kind kind,
    const void *data,
    size_t size,
    char *error,
    size_t error_size)
{
    const uint8_t *bytes = data;

    if (context == NULL || machine >= DUALBOY_MACHINE_COUNT ||
        kind != DUALBOY_MEMORY_FIRMWARE || bytes == NULL ||
        size != TEST_NDS_FIRMWARE_SIZE) {
        return false;
    }
    if (bytes[0] == 0xeeU) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "test rejected malformed NDS firmware");
        }
        return false;
    }
    return true;
}

static const struct dualboy_engine_ops melonds_ops = {
    .name = "fake melonDS",
    .family = DUALBOY_ENGINE_MELONDS,
    .memory_info = fake_melonds_memory,
    .validate_persistent_memory = fake_melonds_validate_persistent_memory,
    .persistent_memory_loaded = fake_persistent_memory_loaded,
};

static bool fake_persistent_extent(const void *context,
                                   unsigned machine,
                                   enum dualboy_memory_kind kind,
                                   bool *known,
                                   size_t *size)
{
    const struct fake_pair *pair = context;

    if (known != NULL) {
        *known = false;
    }
    if (size != NULL) {
        *size = 0U;
    }
    if (pair == NULL || machine >= DUALBOY_MACHINE_COUNT ||
        (unsigned)kind >= DUALBOY_MEMORY_KIND_COUNT || known == NULL ||
        size == NULL) {
        return false;
    }
    *known = pair->extent_known[machine][kind];
    *size = pair->extent[machine][kind];
    return true;
}

static bool fake_memory_dirty(const void *context, unsigned machine)
{
    const struct fake_pair *pair = context;

    return pair != NULL && machine < DUALBOY_MACHINE_COUNT &&
           pair->dirty[machine];
}

static void fake_clear_memory_dirty(void *context, unsigned machine)
{
    struct fake_pair *pair = context;

    if (pair != NULL && machine < DUALBOY_MACHINE_COUNT) {
        pair->dirty[machine] = false;
    }
}

static const struct dualboy_engine_ops dynamic_mgba_ops = {
    .name = "dynamic fake mGBA",
    .family = DUALBOY_ENGINE_MGBA,
    .memory_info = fake_memory,
    .persistent_memory_extent = fake_persistent_extent,
    .memory_dirty = fake_memory_dirty,
    .clear_memory_dirty = fake_clear_memory_dirty,
};

static bool large_fake_memory(void *context,
                              unsigned machine,
                              enum dualboy_memory_kind kind,
                              void **data,
                              size_t *size)
{
    struct large_fake_pair *pair = context;

    if (pair == NULL || machine >= DUALBOY_MACHINE_COUNT || data == NULL ||
        size == NULL) {
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

static bool large_fake_extent(const void *context,
                              unsigned machine,
                              enum dualboy_memory_kind kind,
                              bool *known,
                              size_t *size)
{
    if (known != NULL) {
        *known = false;
    }
    if (size != NULL) {
        *size = 0U;
    }
    if (context == NULL || machine >= DUALBOY_MACHINE_COUNT || known == NULL ||
        size == NULL) {
        return false;
    }
    if (kind == DUALBOY_MEMORY_SAVE_RAM) {
        return true;
    }
    if (kind == DUALBOY_MEMORY_RTC) {
        *known = true;
        *size = 16U;
        return true;
    }
    return false;
}

static const struct dualboy_engine_ops unknown_mgba_ops = {
    .name = "unknown fake mGBA",
    .family = DUALBOY_ENGINE_MGBA,
    .memory_info = large_fake_memory,
    .persistent_memory_extent = large_fake_extent,
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
                         void *pair,
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

static bool file_size_is(const char *path, size_t expected)
{
    struct stat status;

    return stat(path, &status) == 0 && status.st_size >= 0 &&
           (uintmax_t)status.st_size == (uintmax_t)expected;
}

static bool memory_is_filled(const uint8_t *data, size_t size, uint8_t value)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (data[index] != value) {
            return false;
        }
    }
    return true;
}

static bool write_test_file_size(const char *path, size_t size)
{
    uint8_t *bytes;
    enum dualboy_persistence_result result;

    if (size == 0U) {
        FILE *file = fopen(path, "wb");

        return file != NULL && fclose(file) == 0;
    }
    bytes = (uint8_t *)malloc(size);
    if (bytes == NULL) {
        return false;
    }
    memset(bytes, 0x3c, size);
    result = dualboy_write_file_atomic(path, bytes, size);
    free(bytes);
    return result == DUALBOY_PERSISTENCE_OK;
}

static void remove_test_lock(const char *save_path)
{
    char lock_path[DUALBOY_PATH_CAPACITY];
    const int length = snprintf(lock_path, sizeof(lock_path),
                                "%s.dualboy.lock", save_path);

    if (length > 0 && (size_t)length < sizeof(lock_path)) {
        (void)unlink(lock_path);
    }
}

static void deinit_and_remove_test_locks(struct dualboy_save_manager *manager)
{
    char lock_paths[DUALBOY_MAX_SAVE_LOCKS][DUALBOY_PATH_CAPACITY];
    size_t count = 0U;
    unsigned machine;
    unsigned kind;

    if (manager->write_owner) {
        for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
            for (kind = 0U; kind < DUALBOY_MEMORY_KIND_COUNT; ++kind) {
                const char *save_path;
                int length;

                if (!manager->core_managed[machine][kind]) {
                    continue;
                }
                if (kind == DUALBOY_MEMORY_RTC) {
                    save_path = manager->paths.rtc[machine];
                } else if (kind == DUALBOY_MEMORY_FIRMWARE) {
                    save_path = manager->paths.firmware[machine];
                } else {
                    save_path = manager->paths.sram[machine];
                }
                length = snprintf(lock_paths[count], sizeof(lock_paths[count]),
                                  "%s.dualboy.lock", save_path);
                if (length > 0 &&
                    (size_t)length < sizeof(lock_paths[count])) {
                    ++count;
                }
            }
        }
    }
    dualboy_save_manager_deinit(manager);
    while (count > 0U) {
        --count;
        (void)unlink(lock_paths[count]);
    }
}

static bool test_same_rom_second_is_core_managed(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
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
                                   false, &manager.paths) ==
          DUALBOY_PERSISTENCE_OK);
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
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_distinct_subsystem_uses_frontend_memory(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
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
    CHECK(!manager.core_managed[0][DUALBOY_MEMORY_FIRMWARE]);
    CHECK(!manager.core_managed[1][DUALBOY_MEMORY_FIRMWARE]);
    CHECK(dualboy_save_manager_frontend_memory(
        &manager, &session, 0U, DUALBOY_MEMORY_RTC, &memory, &size));
    CHECK(memory == pair.rtc[0]);
    CHECK(dualboy_save_manager_frontend_memory(
        &manager, &session, 1U, DUALBOY_MEMORY_SAVE_RAM, &memory, &size));
    CHECK(memory == pair.sram[1]);
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_pathless_content_is_core_managed(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    void *memory = NULL;
    size_t size = 0U;
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &sameboy_ops, DUALBOY_LOAD_NORMAL,
                 "dualboy-a.gb", "dualboy-a.gb");
    session.content_path_missing[0] = true;
    session.content_path_missing[1] = true;
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(manager.write_owner);
    CHECK(manager.core_managed[0][DUALBOY_MEMORY_SAVE_RAM]);
    CHECK(manager.core_managed[1][DUALBOY_MEMORY_SAVE_RAM]);
    CHECK(!dualboy_save_manager_frontend_memory(
        &manager, &session, 0U, DUALBOY_MEMORY_SAVE_RAM, &memory, &size));
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_gba_legacy_copy_and_independent_flush(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
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
                                   "/roms/second.gba", false, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_legacy_sav_path(paths.sram[0], legacy, sizeof(legacy)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(legacy, imported, sizeof(imported)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(!manager.core_managed[0][DUALBOY_MEMORY_FIRMWARE]);
    CHECK(!manager.core_managed[1][DUALBOY_MEMORY_FIRMWARE]);
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
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_melonds_independent_firmware_persistence(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_melonds_pair first_pair = {0};
    struct fake_melonds_pair recreated_pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    struct dualboy_save_paths paths;
    uint8_t *initial_first;
    uint8_t *initial_second;
    uint8_t *observed;
    void *memory = NULL;
    size_t size = 0U;
    char error[256] = {0};

    initial_first = (uint8_t *)malloc(TEST_NDS_FIRMWARE_SIZE);
    initial_second = (uint8_t *)malloc(TEST_NDS_FIRMWARE_SIZE);
    observed = (uint8_t *)malloc(TEST_NDS_FIRMWARE_SIZE);
    CHECK(initial_first != NULL && initial_second != NULL && observed != NULL);
    memset(initial_first, 0x10, TEST_NDS_FIRMWARE_SIZE);
    memset(initial_second, 0x20, TEST_NDS_FIRMWARE_SIZE);
    initial_first[TEST_NDS_FIRMWARE_SIZE - 1U] = 0x1fU;
    initial_second[TEST_NDS_FIRMWARE_SIZE - 1U] = 0x2fU;
    CHECK(make_temp_directory(directory));
    fake_session(&session, &first_pair, &melonds_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/game.nds", "/roms/game.nds");
    CHECK(dualboy_build_save_paths(directory, session.roms[0].rom.path,
                                   session.roms[1].rom.path, true, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(paths.firmware[0], paths.firmware[1]) != 0);
    CHECK(strstr(paths.firmware[0], ".firmware.bin") != NULL);
    CHECK(strstr(paths.firmware[1], ".firmware.bin.2") != NULL);
    CHECK(dualboy_write_file_atomic(paths.firmware[0], initial_first,
                                    TEST_NDS_FIRMWARE_SIZE) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(paths.firmware[1], initial_second,
                                    TEST_NDS_FIRMWARE_SIZE) ==
          DUALBOY_PERSISTENCE_OK);

    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(first_pair.persistent_load_calls == 1U);
    CHECK(manager.core_managed[0][DUALBOY_MEMORY_SAVE_RAM]);
    CHECK(!manager.core_managed[0][DUALBOY_MEMORY_RTC]);
    CHECK(manager.core_managed[0][DUALBOY_MEMORY_FIRMWARE]);
    CHECK(manager.core_managed[1][DUALBOY_MEMORY_FIRMWARE]);
    CHECK(manager.write_lock_count == 4U);
    CHECK(memcmp(first_pair.firmware[0], initial_first,
                 TEST_NDS_FIRMWARE_SIZE) == 0);
    CHECK(memcmp(first_pair.firmware[1], initial_second,
                 TEST_NDS_FIRMWARE_SIZE) == 0);
    CHECK(!dualboy_save_manager_frontend_memory(
        &manager, &session, 0U, DUALBOY_MEMORY_FIRMWARE, &memory, &size));
    CHECK(memory == NULL && size == 0U);

    first_pair.sram[0][0] = 0xa1U;
    first_pair.sram[1][0] = 0xb2U;
    first_pair.firmware[0][0] = 0xc3U;
    first_pair.firmware[1][0] = 0xd4U;
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(read_exact(manager.paths.firmware[0], observed,
                     TEST_NDS_FIRMWARE_SIZE));
    CHECK(observed[0] == 0xc3U);
    CHECK(read_exact(manager.paths.firmware[1], observed,
                     TEST_NDS_FIRMWARE_SIZE));
    CHECK(observed[0] == 0xd4U);
    deinit_and_remove_test_locks(&manager);

    fake_session(&session, &recreated_pair, &melonds_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/game.nds", "/roms/game.nds");
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(recreated_pair.persistent_load_calls == 1U);
    CHECK(recreated_pair.sram[0][0] == 0xa1U);
    CHECK(recreated_pair.sram[1][0] == 0xb2U);
    CHECK(recreated_pair.firmware[0][0] == 0xc3U);
    CHECK(recreated_pair.firmware[1][0] == 0xd4U);

    CHECK(unlink(manager.paths.sram[0]) == 0);
    CHECK(unlink(manager.paths.sram[1]) == 0);
    CHECK(unlink(manager.paths.firmware[0]) == 0);
    CHECK(unlink(manager.paths.firmware[1]) == 0);
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    free(observed);
    free(initial_second);
    free(initial_first);
    return true;
}

static bool test_melonds_rejects_invalid_firmware_size(size_t invalid_size)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_melonds_pair pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    struct dualboy_save_paths paths;
    char error[256] = {0};
    char size_detail[64];

    memset(pair.firmware[0], 0x5a, sizeof(pair.firmware[0]));
    memset(pair.firmware[1], 0xa5, sizeof(pair.firmware[1]));
    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &melonds_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/invalid.nds", "/roms/invalid.nds");
    CHECK(dualboy_build_save_paths(directory, session.roms[0].rom.path,
                                   session.roms[1].rom.path, true, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(write_test_file_size(paths.firmware[0], invalid_size));

    CHECK(!dualboy_save_manager_init(&manager, &session, directory, error,
                                     sizeof(error)));
    CHECK(strstr(error, "must be exactly 128 KiB (131072 bytes)") != NULL);
    if (invalid_size > TEST_NDS_FIRMWARE_SIZE) {
        CHECK(strstr(error, "file is larger") != NULL);
    } else {
        const int length = snprintf(size_detail, sizeof(size_detail),
                                    "found %zu bytes", invalid_size);

        CHECK(length > 0 && (size_t)length < sizeof(size_detail));
        CHECK(strstr(error, size_detail) != NULL);
    }
    CHECK(!manager.initialized);
    CHECK(pair.persistent_load_calls == 0U);
    CHECK(memory_is_filled(pair.firmware[0], sizeof(pair.firmware[0]),
                           0x5aU));
    CHECK(memory_is_filled(pair.firmware[1], sizeof(pair.firmware[1]),
                           0xa5U));
    CHECK(file_size_is(paths.firmware[0], invalid_size));

    CHECK(unlink(paths.firmware[0]) == 0);
    remove_test_lock(paths.sram[0]);
    remove_test_lock(paths.sram[1]);
    remove_test_lock(paths.firmware[0]);
    remove_test_lock(paths.firmware[1]);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_melonds_rejects_invalid_firmware_contents(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_melonds_pair pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    struct dualboy_save_paths paths;
    uint8_t *malformed;
    char error[256] = {0};

    malformed = (uint8_t *)malloc(TEST_NDS_FIRMWARE_SIZE);
    CHECK(malformed != NULL);
    memset(malformed, 0xee, TEST_NDS_FIRMWARE_SIZE);
    memset(pair.firmware[0], 0x5a, sizeof(pair.firmware[0]));
    memset(pair.firmware[1], 0xa5, sizeof(pair.firmware[1]));
    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &melonds_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/invalid-contents.nds", "/roms/invalid-contents.nds");
    CHECK(dualboy_build_save_paths(directory, session.roms[0].rom.path,
                                   session.roms[1].rom.path, true, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(paths.firmware[0], malformed,
                                    TEST_NDS_FIRMWARE_SIZE) ==
          DUALBOY_PERSISTENCE_OK);

    CHECK(!dualboy_save_manager_init(&manager, &session, directory, error,
                                     sizeof(error)));
    CHECK(strstr(error, "rejected malformed NDS firmware") != NULL);
    CHECK(!manager.initialized);
    CHECK(pair.persistent_load_calls == 0U);
    CHECK(memory_is_filled(pair.firmware[0], sizeof(pair.firmware[0]),
                           0x5aU));
    CHECK(memory_is_filled(pair.firmware[1], sizeof(pair.firmware[1]),
                           0xa5U));

    CHECK(unlink(paths.firmware[0]) == 0);
    remove_test_lock(paths.sram[0]);
    remove_test_lock(paths.sram[1]);
    remove_test_lock(paths.firmware[0]);
    remove_test_lock(paths.firmware[1]);
    CHECK(rmdir(directory) == 0);
    free(malformed);
    return true;
}

static bool test_core_managed_writer_lock(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair first_pair = {0};
    struct fake_pair second_pair = {0};
    struct fake_pair unrelated_pair = {0};
    struct dualboy_session first_session;
    struct dualboy_session second_session;
    struct dualboy_session unrelated_session;
    struct dualboy_save_manager first = {0};
    struct dualboy_save_manager second = {0};
    struct dualboy_save_manager unrelated = {0};
    uint8_t observed[4] = {0};
    char written_path[DUALBOY_PATH_CAPACITY] = {0};
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    fake_session(&first_session, &first_pair, &mgba_ops,
                 DUALBOY_LOAD_NORMAL, "/roms/game.gba", "/roms/game.gba");
    fake_session(&second_session, &second_pair, &mgba_ops,
                 DUALBOY_LOAD_NORMAL, "/roms/game.gba", "/roms/game.gba");
    CHECK(dualboy_save_manager_init(&first, &first_session, directory, error,
                                    sizeof(error)));
    CHECK(first.write_owner);
    CHECK(dualboy_save_manager_init(&second, &second_session, directory, error,
                                    sizeof(error)));
    CHECK(!second.write_owner);

    fake_session(&unrelated_session, &unrelated_pair, &mgba_ops,
                 DUALBOY_LOAD_NORMAL, "/roms/other.gba", "/roms/other.gba");
    CHECK(dualboy_save_manager_init(&unrelated, &unrelated_session, directory,
                                    error, sizeof(error)));
    CHECK(unrelated.write_owner);

    second_pair.sram[0][0] = 0x22U;
    CHECK(dualboy_save_manager_flush(&second, &second_session, true, error,
                                     sizeof(error)));
    CHECK(access(second.paths.sram[0], F_OK) != 0);

    first_pair.sram[0][0] = 0x11U;
    CHECK(dualboy_save_manager_flush(&first, &first_session, true, error,
                                     sizeof(error)));
    CHECK(read_exact(first.paths.sram[0], observed, sizeof(observed)));
    CHECK(observed[0] == 0x11U);
    memcpy(written_path, first.paths.sram[0], sizeof(written_path));

    deinit_and_remove_test_locks(&second);
    deinit_and_remove_test_locks(&unrelated);
    deinit_and_remove_test_locks(&first);
    CHECK(unlink(written_path) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_lock_io_failure_rejects_load(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    char error[256] = {0};

    CHECK(make_temp_directory(directory));
    CHECK(rmdir(directory) == 0);
    fake_session(&session, &pair, &mgba_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/game.gba", "/roms/game.gba");
    CHECK(!dualboy_save_manager_init(&manager, &session, directory, error,
                                     sizeof(error)));
    CHECK(error[0] != '\0');
    dualboy_save_manager_deinit(&manager);
    return true;
}

static bool test_dynamic_extents_and_legacy_rtc_split(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct fake_pair pair = {0};
    struct dualboy_session session;
    struct dualboy_save_manager manager = {0};
    struct dualboy_save_paths paths;
    char legacy[DUALBOY_PATH_CAPACITY];
    uint8_t legacy_bytes[18];
    uint8_t observed[4] = {0};
    char error[256] = {0};
    unsigned machine;

    CHECK(make_temp_directory(directory));
    fake_session(&session, &pair, &dynamic_mgba_ops, DUALBOY_LOAD_NORMAL,
                 "/roms/dynamic.gba", "/roms/dynamic.gba");
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        pair.extent_known[machine][DUALBOY_MEMORY_RTC] = true;
        pair.extent[machine][DUALBOY_MEMORY_RTC] = 0U;
    }
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    pair.sram[0][0] = 0x41U;
    pair.sram[1][0] = 0x52U;
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        pair.extent_known[machine][DUALBOY_MEMORY_SAVE_RAM] = true;
        pair.extent[machine][DUALBOY_MEMORY_SAVE_RAM] = 2U;
        pair.dirty[machine] = true;
    }
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(file_size_is(manager.paths.sram[0], 2U));
    CHECK(file_size_is(manager.paths.sram[1], 2U));

    /* A save-state-only capacity promotion must not grow a clean canonical
     * file. Once the emulated program changes bytes in the new extent, the
     * normal flush makes that larger device durable. */
    pair.extent[0][DUALBOY_MEMORY_SAVE_RAM] = 4U;
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(file_size_is(manager.paths.sram[0], 2U));
    pair.sram[0][3] = 0x6EU;
    pair.dirty[0] = true;
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(file_size_is(manager.paths.sram[0], 4U));

    CHECK(dualboy_write_file_atomic(manager.paths.sram[0],
                                    (const uint8_t[]){1U, 2U, 0xffU, 0xffU},
                                    4U) == DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(manager.paths.sram[1],
                                    (const uint8_t[]){1U, 2U, 3U, 4U},
                                    4U) == DUALBOY_PERSISTENCE_OK);
    deinit_and_remove_test_locks(&manager);
    memset(&pair, 0, sizeof(pair));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        pair.extent_known[machine][DUALBOY_MEMORY_SAVE_RAM] = true;
        pair.extent[machine][DUALBOY_MEMORY_SAVE_RAM] = 2U;
        pair.extent_known[machine][DUALBOY_MEMORY_RTC] = true;
    }
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(file_size_is(manager.paths.sram[0], 2U));
    CHECK(file_size_is(manager.paths.sram[1], 4U));
    pair.sram[1][0] = 9U;
    pair.dirty[1] = true;
    CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                     sizeof(error)));
    CHECK(read_exact(manager.paths.sram[1], observed, sizeof(observed)));
    CHECK(observed[0] == 9U && observed[2] == 3U && observed[3] == 4U);
    CHECK(unlink(manager.paths.sram[0]) == 0);
    CHECK(unlink(manager.paths.sram[1]) == 0);
    deinit_and_remove_test_locks(&manager);

    memset(&pair, 0, sizeof(pair));
    fake_session(&session, &pair, &dynamic_mgba_ops,
                 DUALBOY_LOAD_SUBSYSTEM, "/roms/rtc-one.gba",
                 "/roms/rtc-two.gba");
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        pair.extent_known[machine][DUALBOY_MEMORY_SAVE_RAM] = true;
        pair.extent[machine][DUALBOY_MEMORY_SAVE_RAM] = 2U;
        pair.extent_known[machine][DUALBOY_MEMORY_RTC] = true;
        pair.extent[machine][DUALBOY_MEMORY_RTC] = 16U;
    }
    CHECK(dualboy_build_save_paths(directory, session.roms[0].rom.path,
                                   session.roms[1].rom.path, false, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_legacy_sav_path(paths.sram[0], legacy, sizeof(legacy)) ==
          DUALBOY_PERSISTENCE_OK);
    legacy_bytes[0] = 0xA1U;
    legacy_bytes[1] = 0xB2U;
    for (machine = 0U; machine < 16U; ++machine) {
        legacy_bytes[machine + 2U] = (uint8_t)(0x20U + machine);
    }
    CHECK(dualboy_write_file_atomic(legacy, legacy_bytes,
                                    sizeof(legacy_bytes)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(pair.sram[0][0] == 0xA1U && pair.sram[0][1] == 0xB2U);
    CHECK(memcmp(pair.rtc[0], legacy_bytes + 2U, 16U) == 0);
    CHECK(file_size_is(manager.paths.sram[0], 2U));
    CHECK(file_size_is(manager.paths.rtc[0], 16U));
    CHECK(unlink(legacy) == 0);
    CHECK(unlink(manager.paths.sram[0]) == 0);
    CHECK(unlink(manager.paths.rtc[0]) == 0);
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool test_unknown_extent_legacy_rtc_split(void)
{
    char directory[] = "/tmp/dualboy-manager-XXXXXX";
    struct large_fake_pair pair = {0};
    struct dualboy_session session = {0};
    struct dualboy_save_manager manager = {0};
    struct dualboy_save_paths paths;
    char legacy[DUALBOY_PATH_CAPACITY];
    uint8_t legacy_bytes[(32U * 1024U) + 16U];
    char error[256] = {0};
    size_t index;

    CHECK(make_temp_directory(directory));
    session.engine = &unknown_mgba_ops;
    session.pair = &pair;
    session.loaded = true;
    session.load_kind = DUALBOY_LOAD_SUBSYSTEM;
    session.roms[0].rom.path = "/roms/unknown-one.gba";
    session.roms[1].rom.path = "/roms/unknown-two.gba";
    CHECK(dualboy_build_save_paths(directory, session.roms[0].rom.path,
                                   session.roms[1].rom.path, false, &paths) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_legacy_sav_path(paths.sram[0], legacy, sizeof(legacy)) ==
          DUALBOY_PERSISTENCE_OK);
    memset(legacy_bytes, 0xff, sizeof(legacy_bytes));
    legacy_bytes[0] = 0x63U;
    for (index = 0U; index < 16U; ++index) {
        legacy_bytes[32U * 1024U + index] = (uint8_t)(0x80U + index);
    }
    CHECK(dualboy_write_file_atomic(legacy, legacy_bytes,
                                    sizeof(legacy_bytes)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                    sizeof(error)));
    CHECK(pair.sram[0][0] == 0x63U);
    CHECK(memcmp(pair.rtc[0], legacy_bytes + 32U * 1024U, 16U) == 0);
    CHECK(file_size_is(manager.paths.sram[0], 32U * 1024U));
    CHECK(file_size_is(manager.paths.rtc[0], 16U));
    CHECK(unlink(legacy) == 0);
    CHECK(unlink(manager.paths.sram[0]) == 0);
    CHECK(unlink(manager.paths.rtc[0]) == 0);
    deinit_and_remove_test_locks(&manager);
    CHECK(rmdir(directory) == 0);
    return true;
}

int main(void)
{
    if (!test_same_rom_second_is_core_managed() ||
        !test_distinct_subsystem_uses_frontend_memory() ||
        !test_pathless_content_is_core_managed() ||
        !test_gba_legacy_copy_and_independent_flush() ||
        !test_melonds_independent_firmware_persistence() ||
        !test_melonds_rejects_invalid_firmware_size(0U) ||
        !test_melonds_rejects_invalid_firmware_size(
            TEST_NDS_FIRMWARE_SIZE - 1U) ||
        !test_melonds_rejects_invalid_firmware_size(
            TEST_NDS_FIRMWARE_SIZE + 1U) ||
        !test_melonds_rejects_invalid_firmware_contents() ||
        !test_core_managed_writer_lock() ||
        !test_lock_io_failure_rejects_load() ||
        !test_dynamic_extents_and_legacy_rtc_split() ||
        !test_unknown_extent_legacy_rtc_split()) {
        return EXIT_FAILURE;
    }
    puts("save manager tests passed");
    return EXIT_SUCCESS;
}
