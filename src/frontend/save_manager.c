/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/save_manager.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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

static bool replace_baseline(struct dualboy_save_region_tracking *tracking,
                             const void *data,
                             size_t size)
{
    uint8_t *replacement = NULL;

    if (size == tracking->baseline_size &&
        (size == 0U || tracking->baseline != NULL)) {
        if (size != 0U) {
            memcpy(tracking->baseline, data, size);
        }
        return true;
    }
    if (size != 0U) {
        replacement = (uint8_t *)malloc(size);
        if (replacement == NULL) {
            return false;
        }
        memcpy(replacement, data, size);
    }
    free(tracking->baseline);
    tracking->baseline = replacement;
    tracking->baseline_size = size;
    return true;
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
                        size_t *loaded_size,
                        char *error,
                        size_t error_size)
{
    uint8_t *temporary;
    size_t file_size = 0U;
    enum dualboy_persistence_result result;

    if (loaded_size != NULL) {
        *loaded_size = 0U;
    }
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
        if (loaded_size != NULL) {
            *loaded_size = file_size;
        }
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

static bool is_supported_gba_battery_size(size_t size)
{
    return size == 512U || size == 8U * 1024U || size == 32U * 1024U ||
           size == 64U * 1024U || size == 128U * 1024U;
}

static bool load_sram(struct dualboy_save_manager *manager,
                      struct dualboy_session *session,
                      unsigned machine,
                      void *data,
                      size_t size,
                      size_t *loaded_size,
                      char *error,
                      size_t error_size)
{
    const char *canonical = manager->paths.sram[machine];

    if (session->engine->family != DUALBOY_ENGINE_MGBA) {
        return read_region(canonical, data, size, UINT8_C(0xff), true,
                           loaded_size, error, error_size);
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
                               loaded_size, error, error_size);
        }
        {
            void *rtc_data = NULL;
            size_t rtc_size = 0U;
            size_t allocation_size;
            size_t legacy_size = 0U;
            size_t battery_size;
            uint8_t *temporary;
            bool extent_known = true;
            size_t extent = size;

            if (size > SIZE_MAX - 16U) {
                set_error(error, error_size, "GBA save capacity is too large");
                return false;
            }
            allocation_size = size + 16U;
            temporary = (uint8_t *)malloc(allocation_size);
            if (temporary == NULL) {
                set_error(error, error_size,
                          "out of memory while importing %s", legacy);
                return false;
            }
            result = dualboy_read_file_filled(legacy, temporary,
                                               allocation_size, UINT8_C(0xff),
                                               &legacy_size);
            if (result != DUALBOY_PERSISTENCE_OK) {
                free(temporary);
                set_error(error, error_size, "could not read save %s: %s",
                          legacy, dualboy_persistence_result_message(result));
                return false;
            }
            if (session->engine->persistent_memory_extent != NULL &&
                !session->engine->persistent_memory_extent(
                    session->pair, machine, DUALBOY_MEMORY_SAVE_RAM,
                    &extent_known, &extent)) {
                free(temporary);
                set_error(error, error_size,
                          "engine could not report GBA save extent");
                return false;
            }
            if (extent_known && extent > size) {
                free(temporary);
                set_error(error, error_size,
                          "engine GBA save extent exceeds its stable capacity");
                return false;
            }
            (void)engine_memory(session, machine, DUALBOY_MEMORY_RTC,
                                &rtc_data, &rtc_size);
            battery_size = legacy_size;
            if (rtc_data != NULL && rtc_size == 16U &&
                ((extent_known && legacy_size == extent + rtc_size) ||
                 (!extent_known && legacy_size >= rtc_size &&
                  is_supported_gba_battery_size(legacy_size - rtc_size)))) {
                battery_size = extent_known ? extent : legacy_size - rtc_size;
                memcpy(rtc_data, temporary + battery_size, rtc_size);
            } else if (legacy_size > size) {
                free(temporary);
                set_error(error, error_size,
                          "legacy GBA save %s has an unsupported size", legacy);
                return false;
            }
            memset(data, 0xff, size);
            if (battery_size != 0U) {
                memcpy(data, temporary, battery_size);
            }
            free(temporary);
            if (loaded_size != NULL) {
                *loaded_size = battery_size;
            }
            if (!manager->write_owner) {
                return true;
            }
            result = DUALBOY_PERSISTENCE_OK;
            if (rtc_data != NULL && rtc_size == 16U &&
                legacy_size == battery_size + rtc_size) {
                result = dualboy_write_file_atomic(manager->paths.rtc[machine],
                                                   rtc_data, rtc_size);
            }
            if (result == DUALBOY_PERSISTENCE_OK) {
                result = dualboy_write_file_atomic(canonical, data,
                                                   battery_size);
            }
            if (result != DUALBOY_PERSISTENCE_OK) {
                set_error(error, error_size,
                          "loaded %s but could not copy it to canonical paths: %s",
                          legacy, dualboy_persistence_result_message(result));
                return false;
            }
            return true;
        }
    }
}

static bool persistent_extent(struct dualboy_session *session,
                              unsigned machine,
                              enum dualboy_memory_kind kind,
                              size_t capacity,
                              bool *known,
                              size_t *size,
                              char *error,
                              size_t error_size)
{
    *known = true;
    *size = capacity;
    if (session->engine->persistent_memory_extent != NULL &&
        !session->engine->persistent_memory_extent(session->pair, machine,
                                                   kind, known, size)) {
        set_error(error, error_size,
                  "engine could not report machine %u save extent %u",
                  machine + 1U, (unsigned)kind);
        return false;
    }
    if (*known && *size > capacity) {
        set_error(error, error_size,
                  "engine save extent exceeds machine %u region capacity",
                  machine + 1U);
        return false;
    }
    return true;
}

static bool bytes_are_fill(const uint8_t *data,
                           size_t begin,
                           size_t end,
                           uint8_t fill)
{
    size_t index;

    for (index = begin; index < end; ++index) {
        if (data[index] != fill) {
            return false;
        }
    }
    return true;
}

static void initialize_extent_tracking(
    struct dualboy_save_region_tracking *tracking,
    const uint8_t *data,
    size_t extent)
{
    size_t tracked_size = extent;

    if (extent == 0U && tracking->loaded_size != 0U) {
        tracking->preserved_size = tracking->loaded_size;
    } else if (tracking->loaded_size > extent &&
               !bytes_are_fill(data, extent, tracking->loaded_size,
                               UINT8_C(0xff))) {
        tracking->preserved_size = tracking->loaded_size;
    }
    if (tracking->preserved_size > tracked_size) {
        tracked_size = tracking->preserved_size;
    }

    /* Do not expand a short existing file or create a blank one merely because
     * its detected capacity is larger. A padded, all-FF oversized file is left
     * at its old tracked length so the next safe flush normalizes it. */
    if (tracking->loaded_size <= tracked_size) {
        tracking->size = tracked_size;
        tracking->hash = memory_hash(data, tracked_size);
    }
    tracking->extent_known = true;
    tracking->valid = true;
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
    if (!session->content_path_missing[0]) {
        for (kind = 0U; kind < 2U; ++kind) {
            manager->core_managed[0][kind] = false;
        }
    }
    if (session->load_kind == DUALBOY_LOAD_SUBSYSTEM &&
        !manager->paths.second_uses_collision_suffix &&
        !session->content_path_missing[1]) {
        for (kind = 0U; kind < 2U; ++kind) {
            manager->core_managed[1][kind] = false;
        }
    }
}

enum write_ownership_result {
    WRITE_OWNERSHIP_ACQUIRED = 0,
    WRITE_OWNERSHIP_BUSY,
    WRITE_OWNERSHIP_ERROR,
};

static bool has_core_managed_regions(
    const struct dualboy_save_manager *manager)
{
    unsigned machine;
    unsigned kind;

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            if (manager->core_managed[machine][kind]) {
                return true;
            }
        }
    }
    return false;
}

static void set_lock_close_on_exec(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFD);

    if (flags >= 0) {
        (void)fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
}

static int compare_paths(const void *left, const void *right)
{
    const char *const *left_path = (const char *const *)left;
    const char *const *right_path = (const char *const *)right;

    return strcmp(*left_path, *right_path);
}

static void release_write_ownership(struct dualboy_save_manager *manager);

static enum write_ownership_result acquire_write_ownership(
    struct dualboy_save_manager *manager,
    char *error,
    size_t error_size)
{
    const char *paths[DUALBOY_MAX_SAVE_LOCKS];
    char lock_path[DUALBOY_PATH_CAPACITY];
    size_t path_count = 0U;
    size_t index;
    unsigned machine;
    unsigned kind;

    manager->write_lock_count = 0U;
    manager->write_owner = false;
    if (!has_core_managed_regions(manager)) {
        manager->write_owner = true;
        return WRITE_OWNERSHIP_ACQUIRED;
    }

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            const char *path;
            size_t previous;
            bool duplicate = false;

            if (!manager->core_managed[machine][kind]) {
                continue;
            }
            path = region_path(manager, machine,
                               (enum dualboy_memory_kind)kind);
            for (previous = 0U; previous < path_count; ++previous) {
                if (strcmp(paths[previous], path) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                paths[path_count++] = path;
            }
        }
    }
    qsort(paths, path_count, sizeof(paths[0]), compare_paths);

    for (index = 0U; index < path_count; ++index) {
        int descriptor;
        int lock_result;
        int saved_errno;
        const int length = snprintf(lock_path, sizeof(lock_path),
                                    "%s.dualboy.lock", paths[index]);

        if (length <= 0 || (size_t)length >= sizeof(lock_path)) {
            set_error(error, error_size,
                      "save lock path is too long for %s", paths[index]);
            release_write_ownership(manager);
            return WRITE_OWNERSHIP_ERROR;
        }
        descriptor = open(lock_path, O_RDWR | O_CREAT, 0600);
        if (descriptor < 0) {
            saved_errno = errno;
            set_error(error, error_size, "could not open save lock %s: %s",
                      lock_path, strerror(saved_errno));
            release_write_ownership(manager);
            return WRITE_OWNERSHIP_ERROR;
        }
        set_lock_close_on_exec(descriptor);
        do {
            lock_result = flock(descriptor, LOCK_EX | LOCK_NB);
        } while (lock_result != 0 && errno == EINTR);
        if (lock_result != 0) {
            saved_errno = errno;
            (void)close(descriptor);
            release_write_ownership(manager);
            if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
                return WRITE_OWNERSHIP_BUSY;
            }
            set_error(error, error_size, "could not lock save %s: %s",
                      paths[index], strerror(saved_errno));
            return WRITE_OWNERSHIP_ERROR;
        }
        manager->write_lock_fds[manager->write_lock_count++] = descriptor;
    }
    manager->write_owner = true;
    return WRITE_OWNERSHIP_ACQUIRED;
}

static void release_write_ownership(struct dualboy_save_manager *manager)
{
    size_t index;

    for (index = 0U; index < manager->write_lock_count; ++index) {
        (void)flock(manager->write_lock_fds[index], LOCK_UN);
        (void)close(manager->write_lock_fds[index]);
    }
    manager->write_lock_count = 0U;
    manager->write_owner = false;
}

static void release_tracking_baselines(struct dualboy_save_manager *manager)
{
    unsigned machine;
    unsigned kind;

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            free(manager->tracked[machine][kind].baseline);
            manager->tracked[machine][kind].baseline = NULL;
            manager->tracked[machine][kind].baseline_size = 0U;
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
    {
        const enum write_ownership_result ownership =
            acquire_write_ownership(manager, error, error_size);

        if (ownership == WRITE_OWNERSHIP_ERROR) {
            goto failure;
        }
    }

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            struct dualboy_save_region_tracking *tracking =
                &manager->tracked[machine][kind];
            void *data = NULL;
            size_t size = 0U;
            size_t loaded_size = 0U;
            bool extent_known;
            size_t extent;

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
                                                    size, &loaded_size, error,
                                                    error_size)
                                        : read_region(region_path(
                                                          manager, machine,
                                                          DUALBOY_MEMORY_RTC),
                                                      data, size, 0U, true,
                                                      &loaded_size, error,
                                                      error_size);
                if (!loaded) {
                    goto failure;
                }
                tracking->loaded_size = loaded_size;
                tracking->size = loaded_size;
                tracking->hash = memory_hash(data, loaded_size);
                tracking->valid = true;
                if (!persistent_extent(session, machine,
                                       (enum dualboy_memory_kind)kind, size,
                                       &extent_known, &extent, error,
                                       error_size)) {
                    goto failure;
                }
                if (extent_known) {
                    initialize_extent_tracking(tracking, data, extent);
                }
                if (!replace_baseline(tracking, data, size)) {
                    set_error(error, error_size,
                              "out of memory while tracking machine %u save baseline",
                              machine + 1U);
                    goto failure;
                }
            }
        }
    }
    manager->initialized = true;
    return true;

failure:
    release_write_ownership(manager);
    release_tracking_baselines(manager);
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

    (void)force;

    if (manager == NULL || !manager->initialized || !manager->write_owner ||
        session == NULL ||
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
            size_t extent;
            uint64_t hash;
            enum dualboy_persistence_result result;
            bool extent_known;

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
            if (!persistent_extent(session, machine, memory_kind, size,
                                   &extent_known, &extent, error,
                                   error_size)) {
                return false;
            }
            if (!extent_known) {
                continue;
            }
            if (!tracking->extent_known) {
                const bool dirty = session->engine->memory_dirty != NULL &&
                                   session->engine->memory_dirty(session->pair,
                                                                 machine);
                bool changed_from_baseline = false;

                initialize_extent_tracking(tracking, data, extent);
                if (tracking->baseline != NULL) {
                    size_t compare_size = extent;

                    if (tracking->preserved_size > compare_size) {
                        compare_size = tracking->preserved_size;
                    }
                    if (compare_size <= tracking->baseline_size) {
                        changed_from_baseline =
                            memcmp(data, tracking->baseline, compare_size) != 0;
                    } else {
                        changed_from_baseline = true;
                    }
                }
                if (dirty || changed_from_baseline) {
                    tracking->valid = false;
                }
            }
            if (tracking->preserved_size > extent) {
                extent = tracking->preserved_size;
            }
            /* Loading a state may legitimately promote a live save device
             * (for example FLASH512 to FLASH1M), but the promotion alone is
             * not a battery write. Defer growing the canonical file until its
             * bytes differ from the last disk-authoritative baseline. */
            if (tracking->valid && extent > tracking->size &&
                tracking->baseline != NULL &&
                extent <= tracking->baseline_size &&
                memcmp(data, tracking->baseline, extent) == 0) {
                continue;
            }
            hash = memory_hash(data, extent);
            if (tracking->valid && tracking->size == extent &&
                tracking->hash == hash) {
                continue;
            }
            result = dualboy_write_file_atomic(
                region_path(manager, machine, memory_kind), data, extent);
            if (result != DUALBOY_PERSISTENCE_OK) {
                set_error(error, error_size, "could not write save %s: %s",
                          region_path(manager, machine, memory_kind),
                          dualboy_persistence_result_message(result));
                return false;
            }
            tracking->hash = hash;
            tracking->size = extent;
            tracking->loaded_size = extent;
            tracking->valid = true;
            if (!replace_baseline(tracking, data, size)) {
                set_error(error, error_size,
                          "save was written but its tracking baseline could not be updated");
                return false;
            }
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
        if (manager->initialized) {
            release_write_ownership(manager);
            release_tracking_baselines(manager);
        }
        memset(manager, 0, sizeof(*manager));
    }
}
