/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <libretro.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef DUALBOY_VERSION
#define DUALBOY_VERSION "0.0.0"
#endif

#define DUALBOY_BOOTSTRAP_WIDTH 320U
#define DUALBOY_BOOTSTRAP_HEIGHT 144U

static retro_environment_t environment_callback;
static retro_video_refresh_t video_callback;
static retro_audio_sample_batch_t audio_batch_callback;
static retro_input_poll_t input_poll_callback;
static retro_input_state_t input_state_callback;
static uint32_t bootstrap_frame[DUALBOY_BOOTSTRAP_WIDTH * DUALBOY_BOOTSTRAP_HEIGHT];
static bool content_loaded;

void retro_set_environment(retro_environment_t callback)
{
    bool support_no_game = false;

    environment_callback = callback;
    if (environment_callback != NULL) {
        (void)environment_callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME,
                                   &support_no_game);
    }
}

void retro_set_video_refresh(retro_video_refresh_t callback)
{
    video_callback = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback)
{
    (void)callback;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback)
{
    audio_batch_callback = callback;
}

void retro_set_input_poll(retro_input_poll_t callback)
{
    input_poll_callback = callback;
}

void retro_set_input_state(retro_input_state_t callback)
{
    input_state_callback = callback;
}

void retro_init(void)
{
    memset(bootstrap_frame, 0, sizeof(bootstrap_frame));
}

void retro_deinit(void)
{
    content_loaded = false;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->library_name = "DualBoy";
    info->library_version = DUALBOY_VERSION;
    info->valid_extensions = "gb|gbc|gba|m3u";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->geometry.base_width = DUALBOY_BOOTSTRAP_WIDTH;
    info->geometry.base_height = DUALBOY_BOOTSTRAP_HEIGHT;
    info->geometry.max_width = 480U;
    info->geometry.max_height = 320U;
    info->geometry.aspect_ratio =
        (float)DUALBOY_BOOTSTRAP_WIDTH / (float)DUALBOY_BOOTSTRAP_HEIGHT;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

void retro_reset(void)
{
}

void retro_run(void)
{
    if (input_poll_callback != NULL) {
        input_poll_callback();
    }
    (void)input_state_callback;

    if (content_loaded && video_callback != NULL) {
        video_callback(bootstrap_frame,
                       DUALBOY_BOOTSTRAP_WIDTH,
                       DUALBOY_BOOTSTRAP_HEIGHT,
                       DUALBOY_BOOTSTRAP_WIDTH * sizeof(uint32_t));
    }
    if (audio_batch_callback != NULL) {
        (void)audio_batch_callback(NULL, 0U);
    }
}

size_t retro_serialize_size(void)
{
    return 0U;
}

bool retro_serialize(void *data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}

bool retro_unserialize(const void *data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

bool retro_load_game(const struct retro_game_info *game)
{
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;

    if (game == NULL || (game->data == NULL && game->path == NULL)) {
        return false;
    }
    if (environment_callback == NULL ||
        !environment_callback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        return false;
    }
    content_loaded = true;
    return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info *info,
                             size_t num_info)
{
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void)
{
    content_loaded = false;
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    (void)id;
    return 0U;
}
