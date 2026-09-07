/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_PLAYLIST_H
#define DUALBOY_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>

#define DUALBOY_PLAYLIST_ENTRY_COUNT 2U
#define DUALBOY_PLAYLIST_PATH_CAPACITY 4096U
#define DUALBOY_PLAYLIST_FILE_LIMIT (64U * 1024U)

enum dualboy_playlist_result {
    DUALBOY_PLAYLIST_OK = 0,
    DUALBOY_PLAYLIST_INVALID_ARGUMENT,
    DUALBOY_PLAYLIST_IO_ERROR,
    DUALBOY_PLAYLIST_TOO_LARGE,
    DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT,
    DUALBOY_PLAYLIST_UNSAFE_PATH,
    DUALBOY_PLAYLIST_PATH_TOO_LONG,
};

struct dualboy_playlist {
    char entries[DUALBOY_PLAYLIST_ENTRY_COUNT]
                [DUALBOY_PLAYLIST_PATH_CAPACITY];
};

/* Parses UTF-8-compatible path bytes (a leading UTF-8 BOM is ignored), skips
 * blank/comment lines, and resolves exactly two local paths lexically against
 * playlist_path's directory. Referenced files do not need to exist. */
enum dualboy_playlist_result
dualboy_playlist_parse(const char *playlist_path,
                       const uint8_t *data,
                       size_t size,
                       struct dualboy_playlist *playlist);

/* Loads and parses a playlist, rejecting files larger than 64 KiB. */
enum dualboy_playlist_result
dualboy_playlist_parse_file(const char *playlist_path,
                            struct dualboy_playlist *playlist);

const char *dualboy_playlist_result_message(enum dualboy_playlist_result result);

#endif
