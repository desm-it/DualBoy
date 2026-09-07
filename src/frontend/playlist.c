/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _POSIX_C_SOURCE 200809L

#include "frontend/playlist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool is_space(uint8_t character)
{
    return character == (uint8_t)' ' || character == (uint8_t)'\t' ||
           character == (uint8_t)'\v' || character == (uint8_t)'\f';
}

static bool starts_network_path(const char *path)
{
    return (path[0] == '/' && path[1] == '/') ||
           (path[0] == '\\' && path[1] == '\\');
}

static bool starts_uri_scheme(const char *path)
{
    const unsigned char first = (unsigned char)path[0];
    const char *cursor;

    if (!((first >= (unsigned char)'A' && first <= (unsigned char)'Z') ||
          (first >= (unsigned char)'a' && first <= (unsigned char)'z'))) {
        return false;
    }
    for (cursor = path + 1; *cursor != '\0'; ++cursor) {
        const unsigned char character = (unsigned char)*cursor;
        if (*cursor == ':') {
            return true;
        }
        if (*cursor == '/' || *cursor == '\\') {
            return false;
        }
        if (!((character >= (unsigned char)'A' &&
               character <= (unsigned char)'Z') ||
              (character >= (unsigned char)'a' &&
               character <= (unsigned char)'z') ||
              (character >= (unsigned char)'0' &&
               character <= (unsigned char)'9') ||
              *cursor == '+' || *cursor == '-' || *cursor == '.')) {
            return false;
        }
    }
    return false;
}

static enum dualboy_playlist_result validate_local_path(const char *path)
{
    size_t length;

    if (path == NULL || path[0] == '\0') {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    length = strlen(path);
    if (length >= DUALBOY_PLAYLIST_PATH_CAPACITY) {
        return DUALBOY_PLAYLIST_PATH_TOO_LONG;
    }
    if (starts_network_path(path) || path[0] == '\\' ||
        starts_uri_scheme(path)) {
        return DUALBOY_PLAYLIST_UNSAFE_PATH;
    }
    return DUALBOY_PLAYLIST_OK;
}

static bool append_component(char *output,
                             size_t capacity,
                             size_t *output_length,
                             const char *component,
                             size_t component_length,
                             size_t *restore_length)
{
    size_t required = *output_length + component_length + 1U;
    const bool needs_separator =
        *output_length != 0U && output[*output_length - 1U] != '/';

    if (needs_separator) {
        ++required;
    }
    if (required > capacity) {
        return false;
    }
    *restore_length = *output_length;
    if (needs_separator) {
        output[(*output_length)++] = '/';
    }
    memcpy(output + *output_length, component, component_length);
    *output_length += component_length;
    output[*output_length] = '\0';
    return true;
}

static enum dualboy_playlist_result
normalize_path(const char *input, char *output, size_t capacity)
{
    const size_t input_length = strlen(input);
    const bool absolute = input[0] == '/';
    size_t *restore_lengths;
    bool *is_parent;
    size_t component_count = 0U;
    size_t output_length = 0U;
    size_t cursor = 0U;
    enum dualboy_playlist_result result = DUALBOY_PLAYLIST_OK;

    restore_lengths = malloc((input_length + 1U) * sizeof(*restore_lengths));
    is_parent = malloc((input_length + 1U) * sizeof(*is_parent));
    if (restore_lengths == NULL || is_parent == NULL) {
        free(restore_lengths);
        free(is_parent);
        return DUALBOY_PLAYLIST_IO_ERROR;
    }
    if (absolute) {
        if (capacity < 2U) {
            result = DUALBOY_PLAYLIST_PATH_TOO_LONG;
            goto cleanup;
        }
        output[output_length++] = '/';
        output[output_length] = '\0';
    }

    while (cursor < input_length) {
        size_t start;
        size_t length;
        bool parent;

        while (cursor < input_length && input[cursor] == '/') {
            ++cursor;
        }
        start = cursor;
        while (cursor < input_length && input[cursor] != '/') {
            ++cursor;
        }
        length = cursor - start;
        if (length == 0U ||
            (length == 1U && input[start] == '.')) {
            continue;
        }
        parent = length == 2U && input[start] == '.' &&
                 input[start + 1U] == '.';
        if (parent && component_count != 0U &&
            !is_parent[component_count - 1U]) {
            output_length = restore_lengths[--component_count];
            output[output_length] = '\0';
            continue;
        }
        if (parent && absolute) {
            result = DUALBOY_PLAYLIST_UNSAFE_PATH;
            goto cleanup;
        }
        if (!append_component(output, capacity, &output_length,
                              input + start, length,
                              &restore_lengths[component_count])) {
            result = DUALBOY_PLAYLIST_PATH_TOO_LONG;
            goto cleanup;
        }
        is_parent[component_count++] = parent;
    }

    if (output_length == 0U) {
        if (capacity < 2U) {
            result = DUALBOY_PLAYLIST_PATH_TOO_LONG;
            goto cleanup;
        }
        output[0] = '.';
        output[1] = '\0';
    }

cleanup:
    free(restore_lengths);
    free(is_parent);
    return result;
}

static enum dualboy_playlist_result
playlist_directory(const char *playlist_path,
                   char *directory,
                   size_t capacity)
{
    char normalized[DUALBOY_PLAYLIST_PATH_CAPACITY];
    char *slash;
    enum dualboy_playlist_result result;

    result = validate_local_path(playlist_path);
    if (result != DUALBOY_PLAYLIST_OK) {
        return result;
    }
    if (playlist_path[strlen(playlist_path) - 1U] == '/') {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    result = normalize_path(playlist_path, normalized, sizeof(normalized));
    if (result != DUALBOY_PLAYLIST_OK) {
        return result;
    }
    if (strcmp(normalized, "/") == 0 || strcmp(normalized, ".") == 0 ||
        strcmp(normalized, "..") == 0 || normalized[strlen(normalized) - 1U] == '/') {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }

    slash = strrchr(normalized, '/');
    if (slash == NULL) {
        if (capacity < 2U) {
            return DUALBOY_PLAYLIST_PATH_TOO_LONG;
        }
        directory[0] = '.';
        directory[1] = '\0';
        return DUALBOY_PLAYLIST_OK;
    }
    if (slash == normalized) {
        if (capacity < 2U) {
            return DUALBOY_PLAYLIST_PATH_TOO_LONG;
        }
        directory[0] = '/';
        directory[1] = '\0';
        return DUALBOY_PLAYLIST_OK;
    }
    *slash = '\0';
    if (strlen(normalized) >= capacity) {
        return DUALBOY_PLAYLIST_PATH_TOO_LONG;
    }
    memcpy(directory, normalized, strlen(normalized) + 1U);
    return DUALBOY_PLAYLIST_OK;
}

static enum dualboy_playlist_result
resolve_entry(const char *directory,
              const uint8_t *entry,
              size_t length,
              char *output)
{
    char raw[DUALBOY_PLAYLIST_PATH_CAPACITY];
    char combined[DUALBOY_PLAYLIST_PATH_CAPACITY];
    const char *last_component;
    size_t directory_length;
    enum dualboy_playlist_result result;

    if (length == 0U || length >= sizeof(raw)) {
        return length == 0U ? DUALBOY_PLAYLIST_INVALID_ARGUMENT
                            : DUALBOY_PLAYLIST_PATH_TOO_LONG;
    }
    memcpy(raw, entry, length);
    raw[length] = '\0';
    result = validate_local_path(raw);
    if (result != DUALBOY_PLAYLIST_OK) {
        return result;
    }
    if (raw[length - 1U] == '/') {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    last_component = strrchr(raw, '/');
    last_component = last_component == NULL ? raw : last_component + 1;
    if (strcmp(last_component, ".") == 0 || strcmp(last_component, "..") == 0) {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    if (raw[0] == '/') {
        return normalize_path(raw, output, DUALBOY_PLAYLIST_PATH_CAPACITY);
    }

    directory_length = strlen(directory);
    if (strcmp(directory, ".") == 0) {
        return normalize_path(raw, output, DUALBOY_PLAYLIST_PATH_CAPACITY);
    }
    if (directory_length + length + 2U > sizeof(combined)) {
        return DUALBOY_PLAYLIST_PATH_TOO_LONG;
    }
    memcpy(combined, directory, directory_length);
    if (directory_length == 0U || combined[directory_length - 1U] != '/') {
        combined[directory_length++] = '/';
    }
    memcpy(combined + directory_length, raw, length + 1U);
    return normalize_path(combined, output, DUALBOY_PLAYLIST_PATH_CAPACITY);
}

enum dualboy_playlist_result
dualboy_playlist_parse(const char *playlist_path,
                       const uint8_t *data,
                       size_t size,
                       struct dualboy_playlist *playlist)
{
    struct dualboy_playlist candidate = {{{0}}};
    char directory[DUALBOY_PLAYLIST_PATH_CAPACITY];
    size_t position = 0U;
    size_t entry_count = 0U;
    enum dualboy_playlist_result result;

    if (playlist == NULL || (data == NULL && size != 0U)) {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    result = playlist_directory(playlist_path, directory, sizeof(directory));
    if (result != DUALBOY_PLAYLIST_OK) {
        return result;
    }
    if (size > DUALBOY_PLAYLIST_FILE_LIMIT) {
        return DUALBOY_PLAYLIST_TOO_LARGE;
    }
    if (size >= 3U && data[0] == UINT8_C(0xEF) &&
        data[1] == UINT8_C(0xBB) && data[2] == UINT8_C(0xBF)) {
        position = 3U;
    }

    while (position < size) {
        size_t start = position;
        size_t end;

        while (position < size && data[position] != (uint8_t)'\n' &&
               data[position] != (uint8_t)'\r') {
            if (data[position] == 0U) {
                return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
            }
            ++position;
        }
        end = position;
        if (position < size && data[position] == (uint8_t)'\r') {
            ++position;
            if (position < size && data[position] == (uint8_t)'\n') {
                ++position;
            }
        } else if (position < size) {
            ++position;
        }

        while (start < end && is_space(data[start])) {
            ++start;
        }
        while (end > start && is_space(data[end - 1U])) {
            --end;
        }
        if (start == end || data[start] == (uint8_t)'#') {
            continue;
        }
        if (entry_count >= DUALBOY_PLAYLIST_ENTRY_COUNT) {
            return DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT;
        }
        result = resolve_entry(directory, data + start, end - start,
                               candidate.entries[entry_count]);
        if (result != DUALBOY_PLAYLIST_OK) {
            return result;
        }
        ++entry_count;
    }

    if (entry_count != DUALBOY_PLAYLIST_ENTRY_COUNT) {
        return DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT;
    }
    *playlist = candidate;
    return DUALBOY_PLAYLIST_OK;
}

enum dualboy_playlist_result
dualboy_playlist_parse_file(const char *playlist_path,
                            struct dualboy_playlist *playlist)
{
    uint8_t *data;
    struct stat status;
    size_t size = 0U;
    int descriptor;
    ssize_t amount;
    enum dualboy_playlist_result result;

    if (playlist == NULL) {
        return DUALBOY_PLAYLIST_INVALID_ARGUMENT;
    }
    result = validate_local_path(playlist_path);
    if (result != DUALBOY_PLAYLIST_OK) {
        return result;
    }
    data = malloc(DUALBOY_PLAYLIST_FILE_LIMIT + 1U);
    if (data == NULL) {
        return DUALBOY_PLAYLIST_IO_ERROR;
    }
    descriptor = open(playlist_path, O_RDONLY);
    if (descriptor < 0) {
        free(data);
        return DUALBOY_PLAYLIST_IO_ERROR;
    }
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
        fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        (void)close(descriptor);
        free(data);
        return DUALBOY_PLAYLIST_IO_ERROR;
    }

    while (size <= DUALBOY_PLAYLIST_FILE_LIMIT) {
        amount = read(descriptor, data + size,
                      DUALBOY_PLAYLIST_FILE_LIMIT + 1U - size);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount < 0) {
            (void)close(descriptor);
            free(data);
            return DUALBOY_PLAYLIST_IO_ERROR;
        }
        if (amount == 0) {
            break;
        }
        size += (size_t)amount;
    }
    if (close(descriptor) != 0) {
        free(data);
        return DUALBOY_PLAYLIST_IO_ERROR;
    }
    if (size > DUALBOY_PLAYLIST_FILE_LIMIT) {
        free(data);
        return DUALBOY_PLAYLIST_TOO_LARGE;
    }
    result = dualboy_playlist_parse(playlist_path, data, size, playlist);
    free(data);
    return result;
}

const char *dualboy_playlist_result_message(enum dualboy_playlist_result result)
{
    switch (result) {
    case DUALBOY_PLAYLIST_OK:
        return "success";
    case DUALBOY_PLAYLIST_INVALID_ARGUMENT:
        return "playlist contains invalid data or has no filename";
    case DUALBOY_PLAYLIST_IO_ERROR:
        return "playlist could not be read";
    case DUALBOY_PLAYLIST_TOO_LARGE:
        return "playlist exceeds the 64 KiB size limit";
    case DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT:
        return "playlist must contain exactly two ROM paths";
    case DUALBOY_PLAYLIST_UNSAFE_PATH:
        return "playlist contains a URL, network path, or root traversal";
    case DUALBOY_PLAYLIST_PATH_TOO_LONG:
        return "playlist path exceeds DualBoy's path limit";
    default:
        return "unknown playlist error";
    }
}
