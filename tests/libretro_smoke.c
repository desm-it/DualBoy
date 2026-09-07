/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <libretro.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned (*api_version_fn)(void);
typedef void (*get_system_info_fn)(struct retro_system_info *);

static const char *const required_symbols[] = {
    "retro_set_environment",
    "retro_set_video_refresh",
    "retro_set_audio_sample",
    "retro_set_audio_sample_batch",
    "retro_set_input_poll",
    "retro_set_input_state",
    "retro_init",
    "retro_deinit",
    "retro_api_version",
    "retro_get_system_info",
    "retro_get_system_av_info",
    "retro_set_controller_port_device",
    "retro_reset",
    "retro_run",
    "retro_serialize_size",
    "retro_serialize",
    "retro_unserialize",
    "retro_cheat_reset",
    "retro_cheat_set",
    "retro_load_game",
    "retro_load_game_special",
    "retro_unload_game",
    "retro_get_region",
    "retro_get_memory_data",
    "retro_get_memory_size",
};

static void *load_symbol(void *handle, const char *name)
{
    void *symbol;
    const char *error;

    (void)dlerror();
    symbol = dlsym(handle, name);
    error = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "missing Libretro symbol %s: %s\n", name,
                error != NULL ? error : "unknown error");
        exit(EXIT_FAILURE);
    }
    return symbol;
}

int main(int argc, char **argv)
{
    void *handle;
    api_version_fn api_version;
    get_system_info_fn get_system_info;
    struct retro_system_info info;
    size_t index;

    if (argc != 2) {
        fprintf(stderr, "usage: %s CORE\n", argv[0]);
        return EXIT_FAILURE;
    }

    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "unable to load %s: %s\n", argv[1], dlerror());
        return EXIT_FAILURE;
    }

    for (index = 0U;
         index < sizeof(required_symbols) / sizeof(required_symbols[0]);
         ++index) {
        (void)load_symbol(handle, required_symbols[index]);
    }

    *(void **)(&api_version) = load_symbol(handle, "retro_api_version");
    *(void **)(&get_system_info) = load_symbol(handle, "retro_get_system_info");
    if (api_version() != RETRO_API_VERSION) {
        fprintf(stderr, "unexpected Libretro API version\n");
        (void)dlclose(handle);
        return EXIT_FAILURE;
    }

    memset(&info, 0, sizeof(info));
    get_system_info(&info);
    if (info.library_name == NULL || strcmp(info.library_name, "DualBoy") != 0 ||
        info.valid_extensions == NULL || strstr(info.valid_extensions, "gba") == NULL) {
        fprintf(stderr, "unexpected core metadata\n");
        (void)dlclose(handle);
        return EXIT_FAILURE;
    }

    if (dlclose(handle) != 0) {
        fprintf(stderr, "unable to unload core: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    puts("DualBoy Libretro ABI smoke test passed");
    return EXIT_SUCCESS;
}
