/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/save_manager.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t memory_hash(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static const char *region_path(const struct dualboy_save_manager *manager,
                               unsigned machine,
                               enum dualboy_memory_kind kind)
{
    return kind == DUALBOY_MEMORY_RTC ? manager->paths.rtc[machine]
                                      : manager->paths.sram[machine];
}

static bool engine_memory(struct dualboy_session *session,
                          unsigned machine,
                          enum dualboy_memory_kind kind,
                          void **data,
                          size_t *size)
{
    if (data != NULL) {
        *data = NULL;
    }
    if (size != NULL) {
        *size = 0U;
    }
    return session != NULL && session->loaded && session->engine != NULL &&
           session->engine->memory_info != NULL && data != NULL && size != NULL &&
           session->engine->memory_info(session->pair, machine, kind, data, size) &&
           (*data != NULL || *size == 0U);
}

static bool read_region(const char *path,
                        void *destination,
                        size_t capacity,
                        uint8_t fill,
                        bool missing_is_ok,
                        char *error,
                        size_t error_size)
{
    uint8_t *temporary;
    size_t file_size = 0U;
    enum dualboy_persistence_result result;

    if (capacity == 0U) {
        return true;
    }
    temporary = malloc(capacity);
    if (temporary == NULL) {
        set_error(error, error_size, "out of memory while reading %s", path);
        return false;
    }
    result = dualboy_read_file_filled(path, temporary, capacity, fill, &file_size);
    if (result == DUALBOY_PERSISTENCE_OK) {
        memcpy(destination, temporary, capacity);
        free(temporary);
        return true;
    }
    free(temporary);
    if (missing_is_ok && result == DUALBOY_PERSISTENCE_NOT_FOUND) {
        return true;
    }
    set_error(error, error_size, "could not read save %s: %s", path,
              dualboy_persistence_result_message(result));
    return false;
}

static bool load_sram(struct dualboy_save_manager *manager,
                      struct dualboy_session *session,
                      unsigned machine,
                      void *data,
                      size_t size,
                      char *error,
                      size_t error_size)
{
    const char *canonical = manager->paths.sram[machine];

    if (session->engine->family != DUALBOY_ENGINE_MGBA) {
        return read_region(canonical, data, size, UINT8_C(0xff), true, error,
                           error_size);
    }
    else {
        char legacy[DUALBOY_PATH_CAPACITY];
        enum dualboy_sav_import_choice choice;
        enum dualboy_persistence_result result =
            dualboy_legacy_sav_path(canonical, legacy, sizeof(legacy));

        if (result != DUALBOY_PERSISTENCE_OK) {
            set_error(error, error_size, "could not derive legacy GBA save path: %s",
                      dualboy_persistence_result_message(result));
            return false;
        }
        result = dualboy_choose_sav_import(canonical, legacy, &choice);
        if (result != DUALBOY_PERSISTENCE_OK) {
            set_error(error, error_size, "could not inspect GBA save paths: %s",
                      dualboy_persistence_result_message(result));
            return false;
        }
        if (choice == DUALBOY_SAV_IMPORT_NO_SOURCE) {
            return true;
        }
        if (choice == DUALBOY_SAV_IMPORT_KEEP_CANONICAL) {
            return read_region(canonical, data, size, UINT8_C(0xff), false,
                               error, error_size);
        }
        if (!read_region(legacy, data, size, UINT8_C(0xff), false, error,
                         error_size)) {
            return false;
        }
        result = dualboy_write_file_atomic(canonical, data, size);
        if (result != DUALBOY_PERSISTENCE_OK) {
            set_error(error, error_size,
                      "loaded %s but could not copy it to canonical path %s: %s",
                      legacy, canonical,
                      dualboy_persistence_result_message(result));
            return false;
        }
        return true;
    }
}

static void choose_ownership(struct dualboy_save_manager *manager,
                             const struct dualboy_session *session)
{
    unsigned machine;
    unsigned kind;

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            manager->core_managed[machine][kind] = true;
        }
    }
    if (session->engine->family != DUALBOY_ENGINE_SAMEBOY ||
        session->load_kind == DUALBOY_LOAD_PLAYLIST) {
        return;
    }
    for (kind = 0U; kind < 2U; ++kind) {
        manager->core_managed[0][kind] = false;
    }
    if (session->load_kind == DUALBOY_LOAD_SUBSYSTEM &&
        !manager->paths.second_uses_collision_suffix) {
        for (kind = 0U; kind < 2U; ++kind) {
            manager->core_managed[1][kind] = false;
        }
    }
}

bool dualboy_save_manager_init(struct dualboy_save_manager *manager,
                               struct dualboy_session *session,
                               const char *save_directory,
                               char *error,
                               size_t error_size)
{
    enum dualboy_persistence_result result;
    unsigned machine;
    unsigned kind;

    if (manager == NULL || session == NULL || !session->loaded ||
        session->engine == NULL || session->engine->memory_info == NULL ||
        save_directory == NULL || save_directory[0] == '\0' ||
        session->roms[0].rom.path == NULL ||
        session->roms[1].rom.path == NULL) {
        set_error(error, error_size, "invalid save manager configuration");
        return false;
    }
    memset(manager, 0, sizeof(*manager));
    result = dualboy_build_save_paths(save_directory,
                                      session->roms[0].rom.path,
                                      session->roms[1].rom.path,
                                      &manager->paths);
    if (result != DUALBOY_PERSISTENCE_OK) {
        set_error(error, error_size, "could not derive save paths: %s",
                  dualboy_persistence_result_message(result));
        return false;
    }
    choose_ownership(manager, session);

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            struct dualboy_save_region_tracking *tracking =
                &manager->tracked[machine][kind];
            void *data = NULL;
            size_t size = 0U;

            if (!manager->core_managed[machine][kind]) {
                continue;
            }
            if (!engine_memory(session, machine,
                               (enum dualboy_memory_kind)kind, &data, &size)) {
                set_error(error, error_size,
                          "engine did not expose machine %u save region %u",
                          machine + 1U, kind);
                goto failure;
            }
            if (size > 0U) {
                const bool loaded = kind == DUALBOY_MEMORY_SAVE_RAM
                                        ? load_sram(manager, session, machine, data,
                                                    size, error, error_size)
                                        : read_region(region_path(
                                                          manager, machine,
                                                          DUALBOY_MEMORY_RTC),
                                                      data, size, 0U, true, error,
                                                      error_size);
                if (!loaded) {
                    goto failure;
                }
                tracking->hash = memory_hash(data, size);
                tracking->size = size;
                tracking->valid = true;
            }
        }
    }
    manager->initialized = true;
    return true;

failure:
    memset(manager, 0, sizeof(*manager));
    return false;
}

bool dualboy_save_manager_frontend_memory(
    const struct dualboy_save_manager *manager,
    struct dualboy_session *session,
    unsigned machine,
    enum dualboy_memory_kind kind,
    void **data,
    size_t *size)
{
    if (manager == NULL || !manager->initialized ||
        machine >= DUALBOY_MACHINE_COUNT || (unsigned)kind >= 2U ||
        manager->core_managed[machine][kind]) {
        if (data != NULL) {
            *data = NULL;
        }
        if (size != NULL) {
            *size = 0U;
        }
        return false;
    }
    return engine_memory(session, machine, kind, data, size);
}

bool dualboy_save_manager_flush(struct dualboy_save_manager *manager,
                                struct dualboy_session *session,
                                bool force,
                                char *error,
                                size_t error_size)
{
    unsigned machine;
    unsigned kind;

    if (manager == NULL || !manager->initialized || session == NULL ||
        !session->loaded) {
        return true;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            struct dualboy_save_region_tracking *tracking =
                &manager->tracked[machine][kind];
            enum dualboy_memory_kind memory_kind =
                (enum dualboy_memory_kind)kind;
            void *data = NULL;
            size_t size = 0U;
            uint64_t hash;
            enum dualboy_persistence_result result;

            if (!manager->core_managed[machine][kind]) {
                continue;
            }
            if (!engine_memory(session, machine, memory_kind, &data, &size)) {
                set_error(error, error_size,
                          "engine lost machine %u save region %u",
                          machine + 1U, kind);
                return false;
            }
            if (size == 0U) {
                continue;
            }
            hash = memory_hash(data, size);
            if (!force && tracking->valid && tracking->size == size &&
                tracking->hash == hash) {
                continue;
            }
            result = dualboy_write_file_atomic(
                region_path(manager, machine, memory_kind), data, size);
            if (result != DUALBOY_PERSISTENCE_OK) {
                set_error(error, error_size, "could not write save %s: %s",
                          region_path(manager, machine, memory_kind),
                          dualboy_persistence_result_message(result));
                return false;
            }
            tracking->hash = hash;
            tracking->size = size;
            tracking->valid = true;
        }
        if (session->engine->clear_memory_dirty != NULL) {
            session->engine->clear_memory_dirty(session->pair, machine);
        }
    }
    manager->frames_since_flush = 0U;
    return true;
}

bool dualboy_save_manager_tick(struct dualboy_save_manager *manager,
                               struct dualboy_session *session,
                               char *error,
                               size_t error_size)
{
    if (manager == NULL || !manager->initialized) {
        return true;
    }
    ++manager->frames_since_flush;
    if (manager->frames_since_flush < DUALBOY_SAVE_FLUSH_INTERVAL_FRAMES) {
        return true;
    }
    return dualboy_save_manager_flush(manager, session, false, error, error_size);
}

void dualboy_save_manager_deinit(struct dualboy_save_manager *manager)
{
    if (manager != NULL) {
        memset(manager, 0, sizeof(*manager));
    }
}
