/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _POSIX_C_SOURCE 200809L

#include "frontend/persistence.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool is_separator(char character)
{
    return character == '/' || character == '\\';
}

static bool is_safe_component(const char *component)
{
    const char *cursor;

    if (component == NULL || component[0] == '\0' ||
        strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
        return false;
    }
    for (cursor = component; *cursor != '\0'; ++cursor) {
        if (is_separator(*cursor)) {
            return false;
        }
    }
    return true;
}

static bool copy_string(char *output, size_t capacity,
                        const char *source, size_t length)
{
    if (output == NULL || capacity == 0U || length >= capacity) {
        return false;
    }
    if (length != 0U) {
        memcpy(output, source, length);
    }
    output[length] = '\0';
    return true;
}

enum dualboy_persistence_result
dualboy_content_stem(const char *content_path, char *output, size_t capacity)
{
    const char *basename;
    const char *cursor;
    const char *last_dot = NULL;
    size_t basename_length;
    size_t stem_length;

    if (content_path == NULL || output == NULL || capacity == 0U ||
        content_path[0] == '\0') {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }

    basename = content_path;
    for (cursor = content_path; *cursor != '\0'; ++cursor) {
        if (is_separator(*cursor)) {
            basename = cursor + 1;
        }
    }
    if (!is_safe_component(basename)) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }

    basename_length = strlen(basename);
    for (cursor = basename + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            last_dot = cursor;
        }
    }
    stem_length = last_dot != NULL ? (size_t)(last_dot - basename)
                                   : basename_length;
    if (stem_length == 0U ||
        !copy_string(output, capacity, basename, stem_length)) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }
    if (!is_safe_component(output)) {
        output[0] = '\0';
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    return DUALBOY_PERSISTENCE_OK;
}

enum dualboy_persistence_result
dualboy_join_save_path(const char *save_directory,
                       const char *stem,
                       const char *suffix,
                       char *output,
                       size_t capacity)
{
    size_t directory_length;
    size_t stem_length;
    size_t suffix_length;
    size_t required;
    bool needs_separator;

    if (save_directory == NULL || save_directory[0] == '\0' ||
        !is_safe_component(stem) || !is_safe_component(suffix) ||
        suffix[0] != '.' || output == NULL || capacity == 0U) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }

    directory_length = strlen(save_directory);
    while (directory_length > 1U &&
           is_separator(save_directory[directory_length - 1U])) {
        --directory_length;
    }
    needs_separator = !is_separator(save_directory[directory_length - 1U]);
    stem_length = strlen(stem);
    suffix_length = strlen(suffix);

    if (directory_length > SIZE_MAX - stem_length ||
        directory_length + stem_length > SIZE_MAX - suffix_length - 2U) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }
    required = directory_length + (needs_separator ? 1U : 0U) +
               stem_length + suffix_length + 1U;
    if (required > capacity || required > DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }

    memcpy(output, save_directory, directory_length);
    required = directory_length;
    if (needs_separator) {
        output[required++] = '/';
    }
    memcpy(output + required, stem, stem_length);
    required += stem_length;
    memcpy(output + required, suffix, suffix_length);
    required += suffix_length;
    output[required] = '\0';
    return DUALBOY_PERSISTENCE_OK;
}

static enum dualboy_persistence_result
build_one_set(const char *save_directory,
              const char *stem,
              const char *sram_suffix,
              const char *rtc_suffix,
              const char *firmware_suffix,
              bool include_firmware,
              char *sram,
              char *rtc,
              char *firmware)
{
    enum dualboy_persistence_result result;

    result = dualboy_join_save_path(save_directory, stem, sram_suffix,
                                    sram, DUALBOY_PATH_CAPACITY);
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }
    result = dualboy_join_save_path(save_directory, stem, rtc_suffix,
                                    rtc, DUALBOY_PATH_CAPACITY);
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }
    if (!include_firmware) {
        firmware[0] = '\0';
        return DUALBOY_PERSISTENCE_OK;
    }
    return dualboy_join_save_path(save_directory, stem, firmware_suffix,
                                  firmware, DUALBOY_PATH_CAPACITY);
}

enum dualboy_persistence_result
dualboy_build_save_paths(const char *save_directory,
                         const char *first_content_path,
                         const char *second_content_path,
                         bool include_firmware,
                         struct dualboy_save_paths *paths)
{
    struct dualboy_save_paths candidate = {0};
    char stems[DUALBOY_MACHINE_COUNT][DUALBOY_PATH_CAPACITY];
    char second_unsuffixed[DUALBOY_PATH_CAPACITY];
    enum dualboy_persistence_result result;

    if (paths == NULL) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    result = dualboy_content_stem(first_content_path, stems[0],
                                  sizeof(stems[0]));
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }
    result = dualboy_content_stem(second_content_path, stems[1],
                                  sizeof(stems[1]));
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }
    result = build_one_set(save_directory, stems[0], ".srm", ".rtc",
                           ".firmware.bin", include_firmware, candidate.sram[0],
                           candidate.rtc[0], candidate.firmware[0]);
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }
    result = dualboy_join_save_path(save_directory, stems[1], ".srm",
                                    second_unsuffixed,
                                    sizeof(second_unsuffixed));
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }

    candidate.second_uses_collision_suffix =
        strcmp(candidate.sram[0], second_unsuffixed) == 0;
    result = build_one_set(save_directory, stems[1],
                           candidate.second_uses_collision_suffix
                               ? ".srm.2" : ".srm",
                           candidate.second_uses_collision_suffix
                               ? ".rtc.2" : ".rtc",
                           candidate.second_uses_collision_suffix
                               ? ".firmware.bin.2" : ".firmware.bin",
                           include_firmware,
                           candidate.sram[1], candidate.rtc[1],
                           candidate.firmware[1]);
    if (result != DUALBOY_PERSISTENCE_OK) {
        return result;
    }

    *paths = candidate;
    return DUALBOY_PERSISTENCE_OK;
}

static bool has_suffix(const char *value, const char *suffix)
{
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);

    return value_length >= suffix_length &&
           memcmp(value + value_length - suffix_length,
                  suffix, suffix_length) == 0;
}

enum dualboy_persistence_result
dualboy_legacy_sav_path(const char *canonical_srm,
                        char *output,
                        size_t capacity)
{
    const char *old_suffix;
    const char *new_suffix;
    size_t old_length;
    size_t new_length;
    size_t prefix_length;

    if (canonical_srm == NULL || canonical_srm[0] == '\0' ||
        output == NULL || capacity == 0U) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    if (has_suffix(canonical_srm, ".srm.2")) {
        old_suffix = ".srm.2";
        new_suffix = ".sav.2";
    } else if (has_suffix(canonical_srm, ".srm")) {
        old_suffix = ".srm";
        new_suffix = ".sav";
    } else {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }

    old_length = strlen(old_suffix);
    new_length = strlen(new_suffix);
    prefix_length = strlen(canonical_srm) - old_length;
    if (prefix_length > SIZE_MAX - new_length ||
        prefix_length + new_length + 1U > capacity ||
        prefix_length + new_length + 1U > DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }
    memcpy(output, canonical_srm, prefix_length);
    memcpy(output + prefix_length, new_suffix, new_length + 1U);
    return DUALBOY_PERSISTENCE_OK;
}

/* Returns 1 for a regular file, 0 for absence, and -1 for an unusable path. */
static int regular_file_state(const char *path)
{
    struct stat status;

    if (stat(path, &status) == 0) {
        return S_ISREG(status.st_mode) ? 1 : -1;
    }
    return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}

enum dualboy_persistence_result
dualboy_choose_sav_import(const char *canonical_srm,
                          const char *legacy_sav,
                          enum dualboy_sav_import_choice *choice)
{
    int canonical_state;
    int legacy_state;

    if (canonical_srm == NULL || canonical_srm[0] == '\0' ||
        legacy_sav == NULL || legacy_sav[0] == '\0' || choice == NULL) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    if (strlen(canonical_srm) >= DUALBOY_PATH_CAPACITY ||
        strlen(legacy_sav) >= DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }
    canonical_state = regular_file_state(canonical_srm);
    if (canonical_state < 0) {
        return DUALBOY_PERSISTENCE_IO_ERROR;
    }
    if (canonical_state > 0) {
        *choice = DUALBOY_SAV_IMPORT_KEEP_CANONICAL;
        return DUALBOY_PERSISTENCE_OK;
    }

    legacy_state = regular_file_state(legacy_sav);
    if (legacy_state < 0) {
        return DUALBOY_PERSISTENCE_IO_ERROR;
    }
    *choice = legacy_state > 0 ? DUALBOY_SAV_IMPORT_COPY_LEGACY
                               : DUALBOY_SAV_IMPORT_NO_SOURCE;
    return DUALBOY_PERSISTENCE_OK;
}

static void fill_buffer(void *buffer, size_t capacity, uint8_t fill)
{
    if (capacity != 0U) {
        memset(buffer, (int)fill, capacity);
    }
}

static bool set_close_on_exec(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFD);

    return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

enum dualboy_persistence_result
dualboy_read_file_filled(const char *path,
                         void *buffer,
                         size_t capacity,
                         uint8_t fill,
                         size_t *file_size)
{
    struct stat status;
    uint8_t *temporary = NULL;
    size_t expected_size;
    size_t offset = 0U;
    int descriptor;
    uint8_t probe;
    ssize_t amount;
    enum dualboy_persistence_result result = DUALBOY_PERSISTENCE_IO_ERROR;

    if (file_size != NULL) {
        *file_size = 0U;
    }
    if (path == NULL || path[0] == '\0' || file_size == NULL ||
        (buffer == NULL && capacity != 0U)) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    fill_buffer(buffer, capacity, fill);
    if (strlen(path) >= DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }

    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return errno == ENOENT || errno == ENOTDIR
                   ? DUALBOY_PERSISTENCE_NOT_FOUND
                   : DUALBOY_PERSISTENCE_IO_ERROR;
    }
    (void)set_close_on_exec(descriptor);

    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0) {
        goto cleanup;
    }
    if ((uintmax_t)status.st_size > (uintmax_t)capacity) {
        result = DUALBOY_PERSISTENCE_TOO_LARGE;
        goto cleanup;
    }
    expected_size = (size_t)status.st_size;
    temporary = malloc(expected_size == 0U ? 1U : expected_size);
    if (temporary == NULL) {
        goto cleanup;
    }

    while (offset < expected_size) {
        size_t request = expected_size - offset;
        if (request > (size_t)SSIZE_MAX) {
            request = (size_t)SSIZE_MAX;
        }
        amount = read(descriptor, temporary + offset, request);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            goto cleanup;
        }
        offset += (size_t)amount;
    }

    do {
        amount = read(descriptor, &probe, 1U);
    } while (amount < 0 && errno == EINTR);
    if (amount > 0) {
        result = DUALBOY_PERSISTENCE_TOO_LARGE;
        goto cleanup;
    }
    if (amount < 0) {
        goto cleanup;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto cleanup;
    }
    descriptor = -1;

    if (expected_size != 0U) {
        memcpy(buffer, temporary, expected_size);
    }
    *file_size = expected_size;
    result = DUALBOY_PERSISTENCE_OK;

cleanup:
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    free(temporary);
    return result;
}

static bool valid_target_path(const char *path, size_t length)
{
    const char *basename;
    const char *slash;

    if (path[length - 1U] == '/') {
        return false;
    }
    slash = strrchr(path, '/');
    basename = slash == NULL ? path : slash + 1;
    return basename[0] != '\0' && strcmp(basename, ".") != 0 &&
           strcmp(basename, "..") != 0;
}

static bool sync_descriptor(int descriptor)
{
    int result;

    do {
        result = fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static void sync_parent_directory_best_effort(const char *path)
{
    char parent[DUALBOY_PATH_CAPACITY];
    const char *slash = strrchr(path, '/');
    size_t length;
    int descriptor;

    if (slash == NULL) {
        parent[0] = '.';
        parent[1] = '\0';
    } else {
        length = slash == path ? 1U : (size_t)(slash - path);
        if (!copy_string(parent, sizeof(parent), path, length)) {
            return;
        }
    }
    descriptor = open(parent, O_RDONLY);
    if (descriptor >= 0) {
        (void)set_close_on_exec(descriptor);
        (void)sync_descriptor(descriptor);
        (void)close(descriptor);
    }
}

enum dualboy_persistence_result
dualboy_write_file_atomic(const char *path, const void *data, size_t size)
{
    static const char temporary_name[] = ".dualboy-save-XXXXXX";
    char *temporary_path;
    const char *slash;
    struct stat existing_status;
    size_t path_length;
    size_t directory_length;
    size_t temporary_length;
    size_t offset = 0U;
    int descriptor;
    ssize_t amount;
    bool renamed = false;
    enum dualboy_persistence_result result = DUALBOY_PERSISTENCE_IO_ERROR;

    if (path == NULL || path[0] == '\0' || (data == NULL && size != 0U)) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    path_length = strlen(path);
    if (path_length >= DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }
    if (!valid_target_path(path, path_length)) {
        return DUALBOY_PERSISTENCE_INVALID_ARGUMENT;
    }
    slash = strrchr(path, '/');
    directory_length = slash == NULL ? 0U : (size_t)(slash - path) + 1U;
    if (directory_length > SIZE_MAX - sizeof(temporary_name) ||
        directory_length + sizeof(temporary_name) > DUALBOY_PATH_CAPACITY) {
        return DUALBOY_PERSISTENCE_PATH_TOO_LONG;
    }

    temporary_length = directory_length + sizeof(temporary_name);
    temporary_path = malloc(temporary_length);
    if (temporary_path == NULL) {
        return DUALBOY_PERSISTENCE_IO_ERROR;
    }
    if (directory_length != 0U) {
        memcpy(temporary_path, path, directory_length);
    }
    memcpy(temporary_path + directory_length, temporary_name,
           sizeof(temporary_name));

    descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        free(temporary_path);
        return DUALBOY_PERSISTENCE_IO_ERROR;
    }
    (void)set_close_on_exec(descriptor);
    if (stat(path, &existing_status) == 0 &&
        S_ISREG(existing_status.st_mode)) {
        (void)fchmod(descriptor, existing_status.st_mode & (mode_t)0777);
    }

    while (offset < size) {
        size_t request = size - offset;
        if (request > (size_t)SSIZE_MAX) {
            request = (size_t)SSIZE_MAX;
        }
        amount = write(descriptor, (const uint8_t *)data + offset, request);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            goto cleanup;
        }
        offset += (size_t)amount;
    }
    if (!sync_descriptor(descriptor)) {
        goto cleanup;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto cleanup;
    }
    descriptor = -1;

    if (rename(temporary_path, path) != 0) {
        goto cleanup;
    }
    renamed = true;
    sync_parent_directory_best_effort(path);
    result = DUALBOY_PERSISTENCE_OK;

cleanup:
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    if (!renamed) {
        (void)unlink(temporary_path);
    }
    free(temporary_path);
    return result;
}

const char *
dualboy_persistence_result_message(enum dualboy_persistence_result result)
{
    switch (result) {
    case DUALBOY_PERSISTENCE_OK:
        return "success";
    case DUALBOY_PERSISTENCE_INVALID_ARGUMENT:
        return "invalid persistence path or argument";
    case DUALBOY_PERSISTENCE_PATH_TOO_LONG:
        return "persistence path exceeds DualBoy's path limit";
    case DUALBOY_PERSISTENCE_NOT_FOUND:
        return "persistence file does not exist";
    case DUALBOY_PERSISTENCE_TOO_LARGE:
        return "persistence file is larger than its destination memory";
    case DUALBOY_PERSISTENCE_IO_ERROR:
    default:
        return "persistence file I/O failed";
    }
}
