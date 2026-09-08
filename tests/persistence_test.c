/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _POSIX_C_SOURCE 200809L

#include "frontend/persistence.h"
#include "frontend/playlist.h"

#include <dirent.h>
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

static bool make_path(char *output,
                      size_t capacity,
                      const char *directory,
                      const char *name)
{
    const int length = snprintf(output, capacity, "%s/%s", directory, name);

    return length >= 0 && (size_t)length < capacity;
}

static bool make_temp_directory(char *path_template)
{
    const int descriptor = mkstemp(path_template);

    if (descriptor < 0) {
        return false;
    }
    if (close(descriptor) != 0) {
        (void)unlink(path_template);
        return false;
    }
    if (unlink(path_template) != 0) {
        return false;
    }
    return mkdir(path_template, (mode_t)0700) == 0;
}

static bool test_stems_and_save_paths(void)
{
    struct dualboy_save_paths paths;
    char output[DUALBOY_PATH_CAPACITY];
    char long_directory[DUALBOY_PATH_CAPACITY];
    char tiny[5];

    CHECK(dualboy_content_stem("/roms/Pokemon Red.gb", output,
                               sizeof(output)) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "Pokemon Red") == 0);
    CHECK(dualboy_content_stem("archive.game.gba", output,
                               sizeof(output)) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "archive.game") == 0);
    CHECK(dualboy_content_stem(".hidden", output,
                               sizeof(output)) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, ".hidden") == 0);
    CHECK(dualboy_content_stem("folder\\yellow.gbc", output,
                               sizeof(output)) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "yellow") == 0);
    CHECK(dualboy_content_stem("game.", output,
                               sizeof(output)) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "game") == 0);

    CHECK(dualboy_content_stem("/roms/", output, sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    CHECK(dualboy_content_stem(".", output, sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    CHECK(dualboy_content_stem("..", output, sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    CHECK(dualboy_content_stem("abcdef.gb", tiny, sizeof(tiny)) ==
          DUALBOY_PERSISTENCE_PATH_TOO_LONG);

    CHECK(dualboy_join_save_path("/save///", "red", ".srm", output,
                                 sizeof(output)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "/save/red.srm") == 0);
    CHECK(dualboy_join_save_path("/", "red", ".rtc", output,
                                 sizeof(output)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "/red.rtc") == 0);
    CHECK(dualboy_join_save_path("/save", "../red", ".srm", output,
                                 sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    CHECK(dualboy_join_save_path("/save", "red", "srm", output,
                                 sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    CHECK(dualboy_join_save_path("/save", "red", ".srm", tiny,
                                 sizeof(tiny)) ==
          DUALBOY_PERSISTENCE_PATH_TOO_LONG);

    CHECK(dualboy_build_save_paths("/saves", "/one/red.gb", "/two/blue.gbc",
                                   true, &paths) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(paths.sram[0], "/saves/red.srm") == 0);
    CHECK(strcmp(paths.sram[1], "/saves/blue.srm") == 0);
    CHECK(strcmp(paths.rtc[0], "/saves/red.rtc") == 0);
    CHECK(strcmp(paths.rtc[1], "/saves/blue.rtc") == 0);
    CHECK(strcmp(paths.firmware[0], "/saves/red.firmware.bin") == 0);
    CHECK(strcmp(paths.firmware[1], "/saves/blue.firmware.bin") == 0);
    CHECK(!paths.second_uses_collision_suffix);

    /* Different source paths still collide after save-directory derivation. */
    CHECK(dualboy_build_save_paths("/saves", "/one/game.gb", "/two/game.gbc",
                                   true, &paths) == DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(paths.sram[0], "/saves/game.srm") == 0);
    CHECK(strcmp(paths.sram[1], "/saves/game.srm.2") == 0);
    CHECK(strcmp(paths.rtc[0], "/saves/game.rtc") == 0);
    CHECK(strcmp(paths.rtc[1], "/saves/game.rtc.2") == 0);
    CHECK(strcmp(paths.firmware[0], "/saves/game.firmware.bin") == 0);
    /* The machine suffix follows the complete firmware extension, matching
     * the established .srm.2 and .rtc.2 collision convention. */
    CHECK(strcmp(paths.firmware[1], "/saves/game.firmware.bin.2") == 0);
    CHECK(paths.second_uses_collision_suffix);

    /* An unused NDS-only suffix must not reject a valid non-NDS save path. */
    memset(long_directory, 'd', DUALBOY_PATH_CAPACITY - 15U);
    long_directory[DUALBOY_PATH_CAPACITY - 15U] = '\0';
    CHECK(dualboy_build_save_paths(long_directory, "a.gb", "b.gb", false,
                                   &paths) == DUALBOY_PERSISTENCE_OK);
    CHECK(paths.firmware[0][0] == '\0' && paths.firmware[1][0] == '\0');
    CHECK(dualboy_build_save_paths(long_directory, "a.nds", "b.nds", true,
                                   &paths) ==
          DUALBOY_PERSISTENCE_PATH_TOO_LONG);

    CHECK(dualboy_legacy_sav_path("/saves/game.srm", output,
                                  sizeof(output)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "/saves/game.sav") == 0);
    CHECK(dualboy_legacy_sav_path("/saves/game.srm.2", output,
                                  sizeof(output)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(strcmp(output, "/saves/game.sav.2") == 0);
    CHECK(dualboy_legacy_sav_path("/saves/game.sav", output,
                                  sizeof(output)) ==
          DUALBOY_PERSISTENCE_INVALID_ARGUMENT);
    return true;
}

static size_t count_prefixed_files(const char *directory, const char *prefix)
{
    DIR *stream = opendir(directory);
    struct dirent *entry;
    size_t count = 0U;
    const size_t prefix_length = strlen(prefix);

    if (stream == NULL) {
        return SIZE_MAX;
    }
    while ((entry = readdir(stream)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_length) == 0) {
            ++count;
        }
    }
    (void)closedir(stream);
    return count;
}

static bool test_file_io_and_imports(void)
{
    char directory[] = "/tmp/dualboy-persistence-XXXXXX";
    char target[DUALBOY_PATH_CAPACITY];
    char empty[DUALBOY_PATH_CAPACITY];
    char missing[DUALBOY_PATH_CAPACITY];
    char missing_parent_target[DUALBOY_PATH_CAPACITY];
    char legacy[DUALBOY_PATH_CAPACITY];
    char canonical[DUALBOY_PATH_CAPACITY];
    char blocked[DUALBOY_PATH_CAPACITY];
    char marker[DUALBOY_PATH_CAPACITY];
    char long_path[DUALBOY_PATH_CAPACITY + 1U];
    const uint8_t first[] = {1U, 2U, 3U};
    const uint8_t second[] = {9U, 8U, 7U, 6U};
    uint8_t buffer[8];
    size_t file_size = SIZE_MAX;
    struct stat status;
    enum dualboy_sav_import_choice choice;

    CHECK(make_temp_directory(directory));
    CHECK(make_path(target, sizeof(target), directory, "game.srm"));
    CHECK(make_path(empty, sizeof(empty), directory, "empty.srm"));
    CHECK(make_path(missing, sizeof(missing), directory, "missing.srm"));
    CHECK(make_path(missing_parent_target, sizeof(missing_parent_target),
                    directory, "no-such-parent/save.srm"));
    CHECK(make_path(legacy, sizeof(legacy), directory, "import.sav"));
    CHECK(make_path(canonical, sizeof(canonical), directory, "import.srm"));
    CHECK(make_path(blocked, sizeof(blocked), directory, "blocked"));
    CHECK(make_path(marker, sizeof(marker), blocked, "still-here"));

    CHECK(dualboy_write_file_atomic(target, first, sizeof(first)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(chmod(target, (mode_t)0640) == 0);
    memset(buffer, 0U, sizeof(buffer));
    CHECK(dualboy_read_file_filled(target, buffer, sizeof(buffer), UINT8_C(0xA5),
                                   &file_size) == DUALBOY_PERSISTENCE_OK);
    CHECK(file_size == sizeof(first));
    CHECK(memcmp(buffer, first, sizeof(first)) == 0);
    CHECK(buffer[3] == UINT8_C(0xA5) && buffer[7] == UINT8_C(0xA5));

    CHECK(dualboy_write_file_atomic(target, second, sizeof(second)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(stat(target, &status) == 0);
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0640);
    CHECK(dualboy_read_file_filled(target, buffer, sizeof(buffer), 0U,
                                   &file_size) == DUALBOY_PERSISTENCE_OK);
    CHECK(file_size == sizeof(second));
    CHECK(memcmp(buffer, second, sizeof(second)) == 0);
    CHECK(count_prefixed_files(directory, ".dualboy-save-") == 0U);

    memset(buffer, 0U, sizeof(buffer));
    CHECK(dualboy_read_file_filled(target, buffer, 2U, UINT8_C(0xCC),
                                   &file_size) ==
          DUALBOY_PERSISTENCE_TOO_LARGE);
    CHECK(file_size == 0U);
    CHECK(buffer[0] == UINT8_C(0xCC) && buffer[1] == UINT8_C(0xCC));

    memset(buffer, 0U, sizeof(buffer));
    CHECK(dualboy_read_file_filled(missing, buffer, sizeof(buffer),
                                   UINT8_C(0xEE), &file_size) ==
          DUALBOY_PERSISTENCE_NOT_FOUND);
    CHECK(file_size == 0U && buffer[0] == UINT8_C(0xEE) &&
          buffer[7] == UINT8_C(0xEE));

    CHECK(dualboy_write_file_atomic(empty, NULL, 0U) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_read_file_filled(empty, NULL, 0U, 0U, &file_size) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(file_size == 0U);

    CHECK(dualboy_choose_sav_import(canonical, legacy, &choice) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(choice == DUALBOY_SAV_IMPORT_NO_SOURCE);
    CHECK(dualboy_write_file_atomic(legacy, first, sizeof(first)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_choose_sav_import(canonical, legacy, &choice) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(choice == DUALBOY_SAV_IMPORT_COPY_LEGACY);
    CHECK(dualboy_write_file_atomic(canonical, second, sizeof(second)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_choose_sav_import(canonical, legacy, &choice) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(choice == DUALBOY_SAV_IMPORT_KEEP_CANONICAL);
    CHECK(stat(legacy, &status) == 0 && S_ISREG(status.st_mode));

    /* rename(2) cannot replace this non-empty directory. The old directory and
     * its child survive, and the same-directory temporary file is cleaned up. */
    CHECK(mkdir(blocked, (mode_t)0700) == 0);
    CHECK(dualboy_write_file_atomic(marker, first, sizeof(first)) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_write_file_atomic(blocked, second, sizeof(second)) ==
          DUALBOY_PERSISTENCE_IO_ERROR);
    CHECK(stat(marker, &status) == 0 && S_ISREG(status.st_mode));
    CHECK(count_prefixed_files(directory, ".dualboy-save-") == 0U);

    CHECK(dualboy_write_file_atomic(missing_parent_target, second,
                                    sizeof(second)) ==
          DUALBOY_PERSISTENCE_IO_ERROR);
    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    CHECK(dualboy_write_file_atomic(long_path, second, sizeof(second)) ==
          DUALBOY_PERSISTENCE_PATH_TOO_LONG);

    CHECK(unlink(marker) == 0);
    CHECK(rmdir(blocked) == 0);
    CHECK(unlink(canonical) == 0);
    CHECK(unlink(legacy) == 0);
    CHECK(unlink(empty) == 0);
    CHECK(unlink(target) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

static bool parse_text(const char *path,
                       const char *text,
                       struct dualboy_playlist *playlist)
{
    return dualboy_playlist_parse(path, (const uint8_t *)text, strlen(text),
                                  playlist) == DUALBOY_PLAYLIST_OK;
}

static bool test_playlist_memory(void)
{
    static const uint8_t bom_playlist[] = {
        0xEFU, 0xBBU, 0xBFU,
        '#', 'E', 'X', 'T', 'M', '3', 'U', '\r', '\n',
        ' ', '\t', '\r', '\n',
        '.', '.', '/', 'r', 'o', 'm', 's', '/', '.', '/', 'r', 'e', 'd', '.',
        'g', 'b', '\r', '\n',
        ' ', 's', 'u', 'b', '/', '.', '.', '/', 'b', 'l', 'u', 'e', '.', 'g',
        'b', 'c', ' ', '\n',
    };
    struct dualboy_playlist playlist;
    char long_line[DUALBOY_PLAYLIST_PATH_CAPACITY + 8U];
    char long_path[DUALBOY_PLAYLIST_PATH_CAPACITY + 1U];
    uint8_t oversized[DUALBOY_PLAYLIST_FILE_LIMIT + 1U];
    uint8_t nul_data[] = {'a', '.', 'g', 'b', 0U, '\n', 'b', '.', 'g', 'b'};

    CHECK(dualboy_playlist_parse("/tmp/dualboy/lists/pair.m3u", bom_playlist,
                                 sizeof(bom_playlist), &playlist) ==
          DUALBOY_PLAYLIST_OK);
    CHECK(strcmp(playlist.entries[0], "/tmp/dualboy/roms/red.gb") == 0);
    CHECK(strcmp(playlist.entries[1], "/tmp/dualboy/lists/blue.gbc") == 0);

    CHECK(parse_text("lists/./pair.m3u",
                     "../red.gb\nnested/../../blue.gb\n", &playlist));
    CHECK(strcmp(playlist.entries[0], "red.gb") == 0);
    CHECK(strcmp(playlist.entries[1], "blue.gb") == 0);

    /* Absolute local paths are accepted without checking their existence. */
    CHECK(parse_text("/tmp/list.m3u",
                     "/this/file/does/not/exist.gb\n/also/missing.gbc\n",
                     &playlist));
    CHECK(strcmp(playlist.entries[0],
                 "/this/file/does/not/exist.gb") == 0);

    strcpy(playlist.entries[0], "unchanged");
    CHECK(dualboy_playlist_parse("/tmp/list.m3u",
                                 (const uint8_t *)"only.gb\n",
                                 sizeof("only.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT);
    CHECK(strcmp(playlist.entries[0], "unchanged") == 0);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u",
                                 (const uint8_t *)"a.gb\nb.gb\nc.gb\n",
                                 sizeof("a.gb\nb.gb\nc.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_WRONG_ENTRY_COUNT);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u",
                                 (const uint8_t *)"https://host/a.gb\nb.gb\n",
                                 sizeof("https://host/a.gb\nb.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_UNSAFE_PATH);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u",
                                 (const uint8_t *)"file:///tmp/a.gb\nb.gb\n",
                                 sizeof("file:///tmp/a.gb\nb.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_UNSAFE_PATH);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u",
                                 (const uint8_t *)"//server/a.gb\nb.gb\n",
                                 sizeof("//server/a.gb\nb.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_UNSAFE_PATH);
    CHECK(dualboy_playlist_parse(
              "/tmp/list.m3u",
              (const uint8_t *)"\\\\server\\a.gb\nb.gb\n",
              sizeof("\\\\server\\a.gb\nb.gb\n") - 1U, &playlist) ==
          DUALBOY_PLAYLIST_UNSAFE_PATH);
    CHECK(dualboy_playlist_parse("/pair.m3u",
                                 (const uint8_t *)"../escape.gb\nb.gb\n",
                                 sizeof("../escape.gb\nb.gb\n") - 1U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_UNSAFE_PATH);
    CHECK(dualboy_playlist_parse(
              "/tmp/list.m3u", (const uint8_t *)"sub/..\nb.gb\n",
              sizeof("sub/..\nb.gb\n") - 1U, &playlist) ==
          DUALBOY_PLAYLIST_INVALID_ARGUMENT);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u", nul_data,
                                 sizeof(nul_data), &playlist) ==
          DUALBOY_PLAYLIST_INVALID_ARGUMENT);
    CHECK(dualboy_playlist_parse("/tmp/directory/", (const uint8_t *)"a\nb\n",
                                 4U, &playlist) ==
          DUALBOY_PLAYLIST_INVALID_ARGUMENT);

    memset(long_line, 'a', DUALBOY_PLAYLIST_PATH_CAPACITY);
    long_line[DUALBOY_PLAYLIST_PATH_CAPACITY] = '\n';
    long_line[DUALBOY_PLAYLIST_PATH_CAPACITY + 1U] = 'b';
    long_line[DUALBOY_PLAYLIST_PATH_CAPACITY + 2U] = '\n';
    CHECK(dualboy_playlist_parse(
              "/tmp/list.m3u", (const uint8_t *)long_line,
              DUALBOY_PLAYLIST_PATH_CAPACITY + 3U, &playlist) ==
          DUALBOY_PLAYLIST_PATH_TOO_LONG);
    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    CHECK(dualboy_playlist_parse(long_path, (const uint8_t *)"a\nb\n", 4U,
                                 &playlist) ==
          DUALBOY_PLAYLIST_PATH_TOO_LONG);
    CHECK(dualboy_playlist_parse("/tmp/list.m3u", oversized,
                                 sizeof(oversized), &playlist) ==
          DUALBOY_PLAYLIST_TOO_LARGE);
    return true;
}

static bool test_playlist_file(void)
{
    char directory[] = "/tmp/dualboy-playlist-XXXXXX";
    char path[DUALBOY_PLAYLIST_PATH_CAPACITY];
    char first_expected[DUALBOY_PLAYLIST_PATH_CAPACITY];
    char second_expected[DUALBOY_PLAYLIST_PATH_CAPACITY];
    const char contents[] = "# linked pair\nfirst.gb\nsub/../second.gba\n";
    struct dualboy_playlist playlist;

    CHECK(make_temp_directory(directory));
    CHECK(make_path(path, sizeof(path), directory, "pair.m3u"));
    CHECK(make_path(first_expected, sizeof(first_expected), directory,
                    "first.gb"));
    CHECK(make_path(second_expected, sizeof(second_expected), directory,
                    "second.gba"));
    CHECK(dualboy_write_file_atomic(path, contents, sizeof(contents) - 1U) ==
          DUALBOY_PERSISTENCE_OK);
    CHECK(dualboy_playlist_parse_file(path, &playlist) ==
          DUALBOY_PLAYLIST_OK);
    CHECK(strcmp(playlist.entries[0], first_expected) == 0);
    CHECK(strcmp(playlist.entries[1], second_expected) == 0);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    return true;
}

int main(void)
{
    if (!test_stems_and_save_paths() || !test_file_io_and_imports() ||
        !test_playlist_memory() || !test_playlist_file()) {
        return EXIT_FAILURE;
    }
    puts("persistence and playlist tests passed");
    return EXIT_SUCCESS;
}
