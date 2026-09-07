/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_PERSISTENCE_H
#define DUALBOY_PERSISTENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Includes the terminating NUL. This matches Linux's usual PATH_MAX while
 * remaining independent of whether the host exposes PATH_MAX at compile time. */
#define DUALBOY_PATH_CAPACITY 4096U
#define DUALBOY_MACHINE_COUNT 2U

enum dualboy_persistence_result {
    DUALBOY_PERSISTENCE_OK = 0,
    DUALBOY_PERSISTENCE_INVALID_ARGUMENT,
    DUALBOY_PERSISTENCE_PATH_TOO_LONG,
    DUALBOY_PERSISTENCE_NOT_FOUND,
    DUALBOY_PERSISTENCE_TOO_LARGE,
    DUALBOY_PERSISTENCE_IO_ERROR,
};

struct dualboy_save_paths {
    char sram[DUALBOY_MACHINE_COUNT][DUALBOY_PATH_CAPACITY];
    char rtc[DUALBOY_MACHINE_COUNT][DUALBOY_PATH_CAPACITY];
    bool second_uses_collision_suffix;
};

enum dualboy_sav_import_choice {
    /* Neither a canonical save nor a legacy .sav file exists. */
    DUALBOY_SAV_IMPORT_NO_SOURCE = 0,
    /* The canonical .srm already exists and remains authoritative. */
    DUALBOY_SAV_IMPORT_KEEP_CANONICAL,
    /* A legacy .sav exists and may be copied to the absent canonical path. */
    DUALBOY_SAV_IMPORT_COPY_LEGACY,
};

/* Extracts a final path component and removes its last ordinary extension.
 * A leading dot by itself does not introduce an extension. */
enum dualboy_persistence_result
dualboy_content_stem(const char *content_path, char *output, size_t capacity);

/* Joins a trusted save directory, a single safe filename stem, and a suffix
 * such as ".srm". The stem and suffix may not contain path separators. */
enum dualboy_persistence_result
dualboy_join_save_path(const char *save_directory,
                       const char *stem,
                       const char *suffix,
                       char *output,
                       size_t capacity);

/* Produces canonical RetroArch paths for two machines. When the initially
 * derived full SRAM paths collide, machine 1 receives .srm.2 and .rtc.2. */
enum dualboy_persistence_result
dualboy_build_save_paths(const char *save_directory,
                         const char *first_content_path,
                         const char *second_content_path,
                         struct dualboy_save_paths *paths);

/* Converts a canonical .srm or .srm.2 name to the corresponding optional
 * standalone-emulator .sav or .sav.2 import candidate. */
enum dualboy_persistence_result
dualboy_legacy_sav_path(const char *canonical_srm,
                        char *output,
                        size_t capacity);

/* This is intentionally a decision-only operation: it never renames, removes,
 * or writes either file. A caller choosing COPY_LEGACY must copy the bytes. */
enum dualboy_persistence_result
dualboy_choose_sav_import(const char *canonical_srm,
                          const char *legacy_sav,
                          enum dualboy_sav_import_choice *choice);

/* Reads one complete regular file. The file may be shorter than capacity; its
 * bytes are copied to buffer and the remainder is set to fill. On any error,
 * buffer is left entirely filled and *file_size is zero. */
enum dualboy_persistence_result
dualboy_read_file_filled(const char *path,
                         void *buffer,
                         size_t capacity,
                         uint8_t fill,
                         size_t *file_size);

/* Writes through a same-directory temporary file, fsyncs it, and atomically
 * renames it over path. The old path is never removed before the rename. */
enum dualboy_persistence_result
dualboy_write_file_atomic(const char *path, const void *data, size_t size);

const char *
dualboy_persistence_result_message(enum dualboy_persistence_result result);

#endif
