/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/content.h"
#include "frontend/playlist.h"
#include "frontend/save_manager.h"
#include "frontend/session.h"
#include "frontend/state.h"
#include "frontend/touch_cursor.h"
#include "libretro/options.h"

#include <libretro.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DUALBOY_VERSION
#define DUALBOY_VERSION "0.0.0"
#endif

#define DUALBOY_SUBSYSTEM_ID UINT32_C(0x44554232)
#define DUALBOY_SLOT1_SRAM_ID UINT32_C(0x100)
#define DUALBOY_SLOT1_RTC_ID UINT32_C(0x101)
#define DUALBOY_SLOT2_SRAM_ID UINT32_C(0x200)
#define DUALBOY_SLOT2_RTC_ID UINT32_C(0x201)
#define DUALBOY_AUDIO_BUFFER_FRAMES 4096U
#define DUALBOY_AUDIO_DRAIN_LIMIT 16U
#define DUALBOY_ERROR_CAPACITY 512U
#define DUALBOY_ROM_FILE_LIMIT (512U * 1024U * 1024U)
#define DUALBOY_FRAME_RATE 59.7275
#define DUALBOY_DEFAULT_SAMPLE_RATE 48000U
#define DUALBOY_NDS_SCREEN_WIDTH 256U
#define DUALBOY_NDS_SCREEN_HEIGHT 192U
#define DUALBOY_POINTER_CONTACT_LIMIT 16U
#define DUALBOY_PLAYER1_CURSOR_COLOR UINT32_C(0x0000dfff)
#define DUALBOY_PLAYER2_CURSOR_COLOR UINT32_C(0x00ffb000)

struct dualboy_libretro_context {
    retro_environment_t environment;
    retro_video_refresh_t video;
    retro_audio_sample_t audio;
    retro_audio_sample_batch_t audio_batch;
    retro_input_poll_t input_poll;
    retro_input_state_t input_state;
    retro_log_printf_t frontend_log;
    struct dualboy_session session;
    struct dualboy_save_manager saves;
    struct dualboy_options options;
    struct dualboy_geometry last_geometry;
    struct dualboy_touch_cursor touch_cursors[DUALBOY_MACHINE_COUNT];
    unsigned port_devices[DUALBOY_MACHINE_COUNT];
    char system_directory[DUALBOY_PATH_CAPACITY];
    char save_directory[DUALBOY_PATH_CAPACITY];
    int16_t audio_buffer[DUALBOY_AUDIO_BUFFER_FRAMES * 2U];
    uint32_t presentation_buffer[DUALBOY_MAX_COMPOSITE_PIXELS];
    retro_perf_get_time_usec_t get_time_usec;
    bool frontend_link_value;
    bool frontend_link_known;
    bool pending_link_value;
    bool link_request_pending;
    bool link_failure_logged;
    bool input_bitmasks;
    bool geometry_valid;
    bool fastforward_override_known;
    bool fastforward_inhibited;
    bool fastforward_warning_logged;
    bool initialized;
    enum dualboy_video_renderer loaded_nds_renderer;
};

static struct dualboy_libretro_context core;

#if defined(DUALBOY_INTERNAL_TEST)
/* Test-only access to the real adapter pair. This source is compiled into a
 * separate executable for end-to-end input assertions and is absent from the
 * production shared object's ABI. */
void *dualboy_libretro_debug_engine_pair(void)
{
    return core.session.pair;
}
#endif

static struct retro_subsystem_memory_info slot1_memory[] = {
    {"srm", DUALBOY_SLOT1_SRAM_ID},
    {"rtc", DUALBOY_SLOT1_RTC_ID},
};

static struct retro_subsystem_memory_info slot2_memory[] = {
    {"srm", DUALBOY_SLOT2_SRAM_ID},
    {"rtc", DUALBOY_SLOT2_RTC_ID},
};

static struct retro_subsystem_rom_info subsystem_roms[] = {
    {
        "Player 1 cartridge",
        "gb|gbc|gba|nds",
        false,
        false,
        true,
        slot1_memory,
        2U,
    },
    {
        "Player 2 cartridge",
        "gb|gbc|gba|nds",
        false,
        false,
        true,
        slot2_memory,
        2U,
    },
};

static struct retro_subsystem_info subsystem_info[] = {
    {
        "DualBoy two-player link",
        "dualboylink",
        subsystem_roms,
        DUALBOY_MACHINE_COUNT,
        DUALBOY_SUBSYSTEM_ID,
    },
    {NULL, NULL, NULL, 0U, 0U},
};

static struct retro_system_content_info_override content_overrides[] = {
    {"m3u", true, false},
    {NULL, false, false},
};

static struct retro_controller_description retropad_types[] = {
    {"RetroPad", RETRO_DEVICE_JOYPAD},
    {"RetroPad with Analog", RETRO_DEVICE_ANALOG},
};

static struct retro_controller_info controller_info[] = {
    {retropad_types, 2U},
    {retropad_types, 2U},
    {NULL, 0U},
};

#define INPUT_DESC(port_, id_, text_)                                          \
    {(port_), RETRO_DEVICE_JOYPAD, 0U, (id_), (text_)}
#define ANALOG_DESC(port_, id_, text_)                                         \
    {(port_), RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, (id_),     \
     (text_)}

static struct retro_input_descriptor input_descriptors[] = {
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_UP, "Player 1 Up"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_DOWN, "Player 1 Down"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_LEFT, "Player 1 Left"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Player 1 Right"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_A, "Player 1 A"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_B, "Player 1 B"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_X, "Player 1 X"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_Y, "Player 1 Y"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_L, "Player 1 L"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_R, "Player 1 R"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_SELECT, "Player 1 Select"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_START, "Player 1 Start"),
    INPUT_DESC(0U, RETRO_DEVICE_ID_JOYPAD_R3, "Player 1 Touch Press"),
    ANALOG_DESC(0U, RETRO_DEVICE_ID_ANALOG_X, "Player 1 Touch X"),
    ANALOG_DESC(0U, RETRO_DEVICE_ID_ANALOG_Y, "Player 1 Touch Y"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_UP, "Player 2 Up"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_DOWN, "Player 2 Down"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_LEFT, "Player 2 Left"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Player 2 Right"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_A, "Player 2 A"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_B, "Player 2 B"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_X, "Player 2 X"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_Y, "Player 2 Y"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_L, "Player 2 L"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_R, "Player 2 R"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_SELECT, "Player 2 Select"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_START, "Player 2 Start"),
    INPUT_DESC(1U, RETRO_DEVICE_ID_JOYPAD_R3, "Player 2 Touch Press"),
    ANALOG_DESC(1U, RETRO_DEVICE_ID_ANALOG_X, "Player 2 Touch X"),
    ANALOG_DESC(1U, RETRO_DEVICE_ID_ANALOG_Y, "Player 2 Touch Y"),
    {0U, 0U, 0U, 0U, NULL},
};

#undef INPUT_DESC
#undef ANALOG_DESC

static enum retro_log_level frontend_log_level(enum dualboy_log_level level)
{
    switch (level) {
        case DUALBOY_LOG_DEBUG:
            return RETRO_LOG_DEBUG;
        case DUALBOY_LOG_INFO:
            return RETRO_LOG_INFO;
        case DUALBOY_LOG_WARN:
            return RETRO_LOG_WARN;
        case DUALBOY_LOG_ERROR:
        default:
            return RETRO_LOG_ERROR;
    }
}

static void core_log(enum retro_log_level level, const char *format, ...)
{
    char message[DUALBOY_ERROR_CAPACITY];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (core.frontend_log != NULL) {
        core.frontend_log(level, "[DualBoy] %s\n", message);
    } else {
        (void)fprintf(stderr, "[DualBoy] %s\n", message);
    }
}

static bool force_core_option(const char *key, const char *value)
{
    struct retro_variable variable = {key, value};

    return core.environment != NULL &&
           core.environment(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
}

static bool set_loaded_renderer(enum dualboy_video_renderer renderer,
                                char *error,
                                size_t error_size)
{
    if (!core.session.loaded || core.session.engine == NULL ||
        core.session.engine->family != DUALBOY_ENGINE_MELONDS) {
        return true;
    }
    if (core.session.engine->set_video_renderer == NULL) {
        (void)snprintf(error, error_size,
                       "Nintendo DS adapter has no renderer transition");
        return false;
    }
    return core.session.engine->set_video_renderer(
        core.session.pair, renderer, error, error_size);
}

static void force_software_renderer(const char *reason)
{
    core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
    core.options.nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
    (void)force_core_option(DUALBOY_OPTION_NDS_RENDERER, "software");
    core_log(RETRO_LOG_WARN,
             "melonDS OpenGL is unavailable; using software rendering: %s",
             reason != NULL && reason[0] != '\0' ? reason
                                                  : "unknown renderer error");
}

static bool activate_loaded_gl_renderer(void)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (!core.session.loaded || core.session.engine == NULL ||
        core.session.engine->family != DUALBOY_ENGINE_MELONDS ||
        core.options.nds_renderer != DUALBOY_VIDEO_RENDERER_OPENGL) {
        return true;
    }
    if (!set_loaded_renderer(DUALBOY_VIDEO_RENDERER_OPENGL, error,
                             sizeof(error))) {
        if (core.session.engine->video_renderer != NULL &&
            core.session.engine->video_renderer(core.session.pair) !=
                DUALBOY_VIDEO_RENDERER_SOFTWARE) {
            core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_OPENGL;
            core_log(RETRO_LOG_ERROR,
                     "melonDS OpenGL activation left the NDS pair unusable: %s",
                     error[0] != '\0' ? error : "unknown renderer error");
            return false;
        }
        force_software_renderer(error);
        return true;
    }

    core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_OPENGL;
    core_log(RETRO_LOG_INFO,
             "enabled melonDS OpenGL 3.2 on DualBoy's isolated EGL worker");
    return true;
}

static bool deactivate_loaded_gl_renderer(void)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (core.session.loaded && core.session.engine != NULL &&
        core.session.engine->video_renderer != NULL &&
        core.session.engine->video_renderer(core.session.pair) ==
            DUALBOY_VIDEO_RENDERER_OPENGL) {
        if (!set_loaded_renderer(DUALBOY_VIDEO_RENDERER_SOFTWARE, error,
                                 sizeof(error))) {
            core_log(RETRO_LOG_ERROR,
                     "could not release melonDS OpenGL resources on its worker: %s",
                     error[0] != '\0' ? error : "unknown renderer error");
            if (core.session.engine->video_renderer(core.session.pair) !=
                DUALBOY_VIDEO_RENDERER_SOFTWARE) {
                core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_OPENGL;
                return false;
            }
        }
    }
    core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
    return true;
}

static void engine_log(void *context,
                       enum dualboy_log_level level,
                       const char *message)
{
    (void)context;
    core_log(frontend_log_level(level), "%s",
             message != NULL ? message : "engine emitted an empty log message");
}

static void refresh_frontend_services(void)
{
    struct retro_log_callback log_callback = {NULL};
    struct retro_perf_callback perf_callback = {0};

    core.frontend_log = NULL;
    core.get_time_usec = NULL;
    core.input_bitmasks = false;
    if (core.environment == NULL) {
        return;
    }
    if (core.environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_callback)) {
        core.frontend_log = log_callback.log;
    }
    core.input_bitmasks =
        core.environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
    if (core.environment(RETRO_ENVIRONMENT_GET_PERF_INTERFACE,
                         &perf_callback)) {
        core.get_time_usec = perf_callback.get_time_usec;
    }
}

static void register_environment_interfaces(void)
{
    bool support_no_game = false;

    if (core.environment == NULL) {
        return;
    }
    (void)core.environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME,
                           &support_no_game);
    (void)core.environment(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO,
                           subsystem_info);
    (void)core.environment(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE,
                           content_overrides);
    (void)core.environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
                           controller_info);
    (void)core.environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                           input_descriptors);
    dualboy_options_register(core.environment);
}

static bool copy_directory(char *destination,
                           size_t capacity,
                           const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0U || source == NULL ||
        source[0] == '\0') {
        return false;
    }
    length = strlen(source);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool query_directories(char *error, size_t error_size)
{
    const char *system_directory = NULL;
    const char *save_directory = NULL;

    core.system_directory[0] = '\0';
    core.save_directory[0] = '\0';
    if (core.environment != NULL) {
        (void)core.environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
                               &system_directory);
        (void)core.environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,
                               &save_directory);
    }
    if (system_directory == NULL || system_directory[0] == '\0') {
        system_directory = ".";
    }
    if (!copy_directory(core.system_directory, sizeof(core.system_directory),
                        system_directory)) {
        (void)snprintf(error, error_size,
                       "frontend system directory is too long");
        return false;
    }
    if (save_directory == NULL || save_directory[0] == '\0') {
        save_directory = system_directory;
        core_log(RETRO_LOG_WARN,
                 "frontend did not provide a save directory; using %s",
                 save_directory);
    }
    if (!copy_directory(core.save_directory, sizeof(core.save_directory),
                        save_directory)) {
        (void)snprintf(error, error_size,
                       "frontend save directory is too long");
        return false;
    }
    return true;
}

static struct dualboy_compositor_config current_display(void)
{
    struct dualboy_compositor_config display;

    display.mode = core.options.mode;
    display.layout = core.options.layout;
    display.swap_players = core.options.swap_players;
    return display;
}

static void read_initial_options(void)
{
    if (core.frontend_link_known) {
        core.options.link_enabled = core.frontend_link_value;
    }
    if (core.environment != NULL) {
        (void)dualboy_options_read(core.environment, &core.options);
    }
    core.frontend_link_value = core.options.link_enabled;
    core.frontend_link_known = true;
    core.link_request_pending = false;
    core.link_failure_logged = false;
}

static void ensure_initialized(void)
{
    if (core.initialized) {
        return;
    }
    dualboy_session_init(&core.session);
    dualboy_options_set_defaults(&core.options);
    core.port_devices[0] = RETRO_DEVICE_JOYPAD;
    core.port_devices[1] = RETRO_DEVICE_JOYPAD;
    refresh_frontend_services();
    read_initial_options();
    core.initialized = true;
}

static bool transport_requires_realtime(void)
{
    return core.session.loaded && core.session.engine != NULL &&
           core.session.engine->link_transport_active != NULL &&
           core.session.engine->link_transport_active(core.session.pair);
}

static void set_fastforward_inhibition(bool inhibit)
{
    struct retro_fastforwarding_override override = {
        inhibit ? 1.0F : 0.0F,
        false,
        false,
        inhibit,
    };

    if (!core.fastforward_override_known && !inhibit) {
        return;
    }
    if (core.fastforward_override_known &&
        core.fastforward_inhibited == inhibit) {
        return;
    }
    core.fastforward_override_known = true;
    core.fastforward_inhibited = inhibit;
    if (core.environment != NULL &&
        !core.environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE,
                          &override) &&
        inhibit && !core.fastforward_warning_logged) {
        core_log(RETRO_LOG_WARN,
                 "frontend cannot inhibit fast-forward during active local wireless");
        core.fastforward_warning_logged = true;
    }
}

static void synchronize_fastforward_policy(void)
{
    const bool inhibit = transport_requires_realtime();

    if (inhibit || (core.fastforward_override_known &&
                    core.fastforward_inhibited)) {
        set_fastforward_inhibition(inhibit);
    }
}

static void reset_touch_cursors(void)
{
    unsigned port;

    for (port = 0U; port < DUALBOY_MACHINE_COUNT; ++port) {
        dualboy_touch_cursor_reset(&core.touch_cursors[port]);
    }
}

static void unload_current(void)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    set_fastforward_inhibition(false);
    if (core.saves.initialized && core.session.loaded &&
        !dualboy_save_manager_flush(&core.saves, &core.session, true, error,
                                    sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "save flush during unload failed: %s",
                 error[0] != '\0' ? error : "unknown persistence error");
    }
    (void)deactivate_loaded_gl_renderer();
    dualboy_save_manager_deinit(&core.saves);
    dualboy_session_unload(&core.session);
    reset_touch_cursors();
    core.geometry_valid = false;
    core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
}

/* A failed transactional rollback can leave engine memory untrustworthy.
 * Release ownership without asking the adapter to mirror or persist it. */
static void discard_current(void)
{
    set_fastforward_inhibition(false);
    (void)deactivate_loaded_gl_renderer();
    dualboy_save_manager_deinit(&core.saves);
    dualboy_session_unload(&core.session);
    reset_touch_cursors();
    core.geometry_valid = false;
}

static bool set_pixel_format(void)
{
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;

    if (core.environment == NULL ||
        !core.environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        core_log(RETRO_LOG_ERROR,
                 "frontend rejected the required XRGB8888 pixel format");
        return false;
    }
    return true;
}

static uint64_t content_identity(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool fallback_content_path(char *output,
                                  size_t capacity,
                                  const struct dualboy_rom *rom)
{
    const char *extension;
    int written;

    if (output == NULL || capacity == 0U || rom == NULL || rom->data == NULL) {
        return false;
    }
    if (rom->platform == DUALBOY_PLATFORM_NDS) {
        extension = "nds";
    } else if (rom->platform == DUALBOY_PLATFORM_GBA) {
        extension = "gba";
    } else {
        extension = rom->platform == DUALBOY_PLATFORM_GBC ? "gbc" : "gb";
    }
    written = snprintf(output, capacity, "dualboy-%016llx.%s",
                       (unsigned long long)content_identity(rom->data,
                                                           rom->size),
                       extension);
    return written > 0 && (size_t)written < capacity;
}

static const struct dualboy_engine_ops *engine_for_family(
    enum dualboy_engine_family family)
{
    if (family == DUALBOY_ENGINE_SAMEBOY) {
        return dualboy_sameboy_engine();
    }
    if (family == DUALBOY_ENGINE_MGBA) {
        return dualboy_mgba_engine();
    }
    if (family == DUALBOY_ENGINE_MELONDS) {
        return dualboy_melonds_engine();
    }
    return NULL;
}

static bool load_detected_pair(struct dualboy_rom roms[DUALBOY_MACHINE_COUNT],
                               enum dualboy_load_kind load_kind)
{
    struct dualboy_detection detection[DUALBOY_MACHINE_COUNT];
    struct dualboy_engine_config engine_config;
    const struct dualboy_engine_ops *engine;
    enum dualboy_engine_family family;
    char error[DUALBOY_ERROR_CAPACITY] = {0};
    char fallback_paths[DUALBOY_MACHINE_COUNT][64];
    bool content_path_missing[DUALBOY_MACHINE_COUNT];
    bool same_cartridge;
    unsigned machine;

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        content_path_missing[machine] =
            roms[machine].path == NULL || roms[machine].path[0] == '\0';
        if (roms[machine].size > DUALBOY_ROM_FILE_LIMIT) {
            core_log(RETRO_LOG_ERROR,
                     "player %u cartridge exceeds the %u MiB ROM limit",
                     machine + 1U,
                     DUALBOY_ROM_FILE_LIMIT / (1024U * 1024U));
            return false;
        }
        detection[machine] = dualboy_detect_content(roms[machine].data,
                                                    roms[machine].size);
        if (detection[machine].error != DUALBOY_DETECT_OK) {
            core_log(RETRO_LOG_ERROR, "player %u cartridge rejected: %s",
                     machine + 1U, detection[machine].message);
            return false;
        }
        roms[machine].platform = detection[machine].platform;
    }

    same_cartridge =
        roms[0].size == roms[1].size &&
        (roms[0].data == roms[1].data ||
         memcmp(roms[0].data, roms[1].data, roms[0].size) == 0);
    if (same_cartridge &&
        (roms[0].path == NULL || roms[0].path[0] == '\0') &&
        roms[1].path != NULL && roms[1].path[0] != '\0') {
        roms[0].path = roms[1].path;
    }
    if (same_cartridge &&
        (roms[1].path == NULL || roms[1].path[0] == '\0') &&
        roms[0].path != NULL && roms[0].path[0] != '\0') {
        roms[1].path = roms[0].path;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (roms[machine].path == NULL || roms[machine].path[0] == '\0') {
            if (!fallback_content_path(fallback_paths[machine],
                                       sizeof(fallback_paths[machine]),
                                       &roms[machine])) {
                core_log(RETRO_LOG_ERROR,
                         "could not derive a pathless content identity");
                return false;
            }
            roms[machine].path = fallback_paths[machine];
            core_log(RETRO_LOG_WARN,
                     "player %u content has no frontend path; core-managed save identity is %s",
                     machine + 1U, roms[machine].path);
        }
    }

    if (!dualboy_platforms_compatible(roms[0].platform, roms[1].platform)) {
        core_log(RETRO_LOG_ERROR,
                 "mixed cartridge families are unsupported: player 1 is %s, "
                 "player 2 is %s",
                 dualboy_platform_name(roms[0].platform),
                 dualboy_platform_name(roms[1].platform));
        return false;
    }
    family = dualboy_engine_family_for_platform(roms[0].platform);
    engine = engine_for_family(family);
    if (engine == NULL) {
        core_log(RETRO_LOG_ERROR, "no engine adapter is available for %s",
                 dualboy_platform_name(roms[0].platform));
        return false;
    }
    core.loaded_nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
    if (family == DUALBOY_ENGINE_MELONDS &&
        core.options.nds_renderer == DUALBOY_VIDEO_RENDERER_OPENGL) {
        if (core.options.link_enabled) {
            core.options.link_enabled = false;
            core.frontend_link_value = false;
            core.frontend_link_known = true;
            (void)force_core_option(DUALBOY_OPTION_LINK, "disabled");
            core_log(RETRO_LOG_INFO,
                     "disabled Local Link because the NDS OpenGL renderer was selected");
        }
    }
    if (!set_pixel_format() || !query_directories(error, sizeof(error))) {
        if (error[0] != '\0') {
            core_log(RETRO_LOG_ERROR, "%s", error);
        }
        return false;
    }

    engine_config.log = engine_log;
    engine_config.log_context = &core;
    engine_config.system_directory = core.system_directory;
    engine_config.audio_sample_rate = DUALBOY_DEFAULT_SAMPLE_RATE;
    if (!dualboy_session_load(&core.session, engine, &engine_config, roms,
                              load_kind, core.options.link_enabled, error,
                              sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "%s pair initialization failed: %s",
                 engine->name != NULL ? engine->name : "emulator",
                 error[0] != '\0' ? error : "unknown engine error");
        return false;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        core.session.content_path_missing[machine] =
            content_path_missing[machine];
    }
    if (!dualboy_save_manager_init(&core.saves, &core.session,
                                   core.save_directory, error,
                                   sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "save initialization failed: %s",
                 error[0] != '\0' ? error : "unknown persistence error");
        dualboy_session_unload(&core.session);
        dualboy_save_manager_deinit(&core.saves);
        return false;
    }
    if (!core.saves.write_owner) {
        core_log(RETRO_LOG_WARN,
                 "another DualBoy instance owns core-managed saves in %s; "
                 "this instance is read-only",
                 core.save_directory);
    }
    if (family == DUALBOY_ENGINE_MELONDS &&
        core.options.nds_renderer == DUALBOY_VIDEO_RENDERER_OPENGL) {
        /* Persistence initialization performs the final NDS reset. Install
         * the GL renderers afterwards, on their context-owning worker. A
         * rejected or unavailable EGL path intentionally leaves the loaded
         * pair on its already-valid software renderers. */
        if (!activate_loaded_gl_renderer()) {
            discard_current();
            return false;
        }
    }

    core.session.display = current_display();
    core.geometry_valid = false;
    core_log(RETRO_LOG_INFO, "loaded %s pair using %s (%s)",
             dualboy_platform_name(roms[0].platform),
             engine->name != NULL ? engine->name : "unknown engine",
             load_kind == DUALBOY_LOAD_NORMAL
                 ? "duplicated content"
                 : (load_kind == DUALBOY_LOAD_SUBSYSTEM ? "two-ROM subsystem"
                                                        : "M3U playlist"));
    return true;
}

static bool path_has_extension(const char *path, const char *extension)
{
    const char *basename;
    const char *slash;
    const char *backslash;
    const char *dot;

    if (path == NULL || extension == NULL) {
        return false;
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    basename = path;
    if (slash != NULL) {
        basename = slash + 1;
    }
    if (backslash != NULL && backslash + 1 > basename) {
        basename = backslash + 1;
    }
    dot = strrchr(basename, '.');
    if (dot == NULL) {
        return false;
    }
    ++dot;
    while (*dot != '\0' && *extension != '\0') {
        char left = *dot++;
        char right = *extension++;

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return *dot == '\0' && *extension == '\0';
}

static bool read_rom_file(const char *path,
                          uint8_t **output,
                          size_t *output_size,
                          char *error,
                          size_t error_size)
{
    FILE *file;
    uint8_t *bytes = NULL;
    long file_length;
    size_t size;
    bool success = false;

    if (output != NULL) {
        *output = NULL;
    }
    if (output_size != NULL) {
        *output_size = 0U;
    }
    if (path == NULL || path[0] == '\0' || output == NULL ||
        output_size == NULL) {
        (void)snprintf(error, error_size, "invalid playlist cartridge path");
        return false;
    }

    errno = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(error, error_size, "cannot open %s: %s", path,
                       strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_length = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)snprintf(error, error_size, "cannot determine size of %s", path);
        goto cleanup;
    }
    if (file_length == 0L ||
        (unsigned long)file_length > (unsigned long)DUALBOY_ROM_FILE_LIMIT) {
        (void)snprintf(error, error_size,
                       "%s is empty or exceeds the %u MiB ROM limit", path,
                       DUALBOY_ROM_FILE_LIMIT / (1024U * 1024U));
        goto cleanup;
    }
    size = (size_t)file_length;
    bytes = malloc(size);
    if (bytes == NULL) {
        (void)snprintf(error, error_size,
                       "out of memory while reading %s", path);
        goto cleanup;
    }
    if (fread(bytes, 1U, size, file) != size) {
        (void)snprintf(error, error_size, "could not read all of %s", path);
        goto cleanup;
    }
    *output = bytes;
    *output_size = size;
    bytes = NULL;
    success = true;

cleanup:
    free(bytes);
    if (fclose(file) != 0 && success) {
        free(*output);
        *output = NULL;
        *output_size = 0U;
        (void)snprintf(error, error_size, "could not close %s after reading",
                       path);
        success = false;
    }
    return success;
}

static bool load_playlist(const char *playlist_path)
{
    struct dualboy_playlist playlist;
    struct dualboy_rom roms[DUALBOY_MACHINE_COUNT];
    uint8_t *bytes[DUALBOY_MACHINE_COUNT] = {NULL, NULL};
    size_t sizes[DUALBOY_MACHINE_COUNT] = {0U, 0U};
    enum dualboy_playlist_result result;
    char error[DUALBOY_ERROR_CAPACITY] = {0};
    bool success = false;
    unsigned machine;

    result = dualboy_playlist_parse_file(playlist_path, &playlist);
    if (result != DUALBOY_PLAYLIST_OK) {
        core_log(RETRO_LOG_ERROR, "cannot load playlist %s: %s", playlist_path,
                 dualboy_playlist_result_message(result));
        return false;
    }
    memset(roms, 0, sizeof(roms));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (!read_rom_file(playlist.entries[machine], &bytes[machine],
                           &sizes[machine], error, sizeof(error))) {
            core_log(RETRO_LOG_ERROR, "playlist player %u load failed: %s",
                     machine + 1U, error);
            goto cleanup;
        }
        roms[machine].data = bytes[machine];
        roms[machine].size = sizes[machine];
        roms[machine].path = playlist.entries[machine];
    }
    success = load_detected_pair(roms, DUALBOY_LOAD_PLAYLIST);

cleanup:
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        free(bytes[machine]);
    }
    return success;
}

static bool port_has_controller(unsigned port)
{
    const unsigned device = port < DUALBOY_MACHINE_COUNT
                                ? core.port_devices[port] & RETRO_DEVICE_MASK
                                : RETRO_DEVICE_NONE;

    return device == RETRO_DEVICE_JOYPAD || device == RETRO_DEVICE_ANALOG;
}

static uint16_t read_port_button_mask(unsigned port)
{
    uint16_t buttons = 0U;
    unsigned id;

    if (core.input_state == NULL || !port_has_controller(port)) {
        return 0U;
    }
    if (core.input_bitmasks) {
        return (uint16_t)core.input_state(port, RETRO_DEVICE_JOYPAD, 0U,
                                          RETRO_DEVICE_ID_JOYPAD_MASK);
    }
    for (id = 0U; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id) {
        if (core.input_state(port, RETRO_DEVICE_JOYPAD, 0U, id) != 0) {
            buttons = (uint16_t)(buttons | (uint16_t)(1U << id));
        }
    }
    return buttons;
}

static unsigned normalized_coordinate(int16_t coordinate, unsigned extent)
{
    const uint32_t scaled =
        (uint32_t)((int32_t)coordinate + INT32_C(0x8000));

    if (extent <= 1U) {
        return 0U;
    }
    return (unsigned)(((uint64_t)scaled * extent) / UINT32_C(0x10000));
}

static bool nds_session_loaded(void)
{
    return core.session.loaded &&
           core.session.roms[0].rom.platform == DUALBOY_PLATFORM_NDS;
}

static bool touch_cursor_time(uint64_t *now_usec)
{
    retro_time_t current;

    if (now_usec == NULL || core.get_time_usec == NULL) {
        return false;
    }
    current = core.get_time_usec();
    if (current < 0) {
        return false;
    }
    *now_usec = (uint64_t)current;
    return true;
}

static struct dualboy_machine_input read_port_input(unsigned port,
                                                    bool time_available,
                                                    uint64_t now_usec)
{
    struct dualboy_machine_input input = {0U, false, 0U, 0U};
    const uint16_t button_mask = read_port_button_mask(port);

    input.buttons = button_mask & UINT16_C(0x0fff);
    if (nds_session_loaded() && core.input_state != NULL &&
        port_has_controller(port)) {
        const int16_t analog_x = core.input_state(
            port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_X);
        const int16_t analog_y = core.input_state(
            port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_Y);
        const bool pressed =
            (button_mask &
             (UINT16_C(1) << RETRO_DEVICE_ID_JOYPAD_R3)) != 0U;
        const uint16_t touch_x = (uint16_t)normalized_coordinate(
            analog_x, DUALBOY_NDS_SCREEN_WIDTH);
        const uint16_t touch_y = (uint16_t)normalized_coordinate(
            analog_y, DUALBOY_NDS_SCREEN_HEIGHT);
        const bool initially_deflected =
            touch_x + DUALBOY_TOUCH_CURSOR_MOTION_PIXELS <=
                DUALBOY_NDS_SCREEN_WIDTH / 2U ||
            touch_x >= DUALBOY_NDS_SCREEN_WIDTH / 2U +
                           DUALBOY_TOUCH_CURSOR_MOTION_PIXELS ||
            touch_y + DUALBOY_TOUCH_CURSOR_MOTION_PIXELS <=
                DUALBOY_NDS_SCREEN_HEIGHT / 2U ||
            touch_y >= DUALBOY_NDS_SCREEN_HEIGHT / 2U +
                           DUALBOY_TOUCH_CURSOR_MOTION_PIXELS;

        dualboy_touch_cursor_update(&core.touch_cursors[port], touch_x,
                                    touch_y, initially_deflected, pressed,
                                    time_available, now_usec);

        if (pressed) {
            input.touch_active = true;
            input.touch_x = touch_x;
            input.touch_y = touch_y;
        }
    } else if (port < DUALBOY_MACHINE_COUNT) {
        dualboy_touch_cursor_reset(&core.touch_cursors[port]);
    }
    return input;
}

static void apply_pointer_contacts(
    struct dualboy_machine_input inputs[DUALBOY_MACHINE_COUNT],
    const struct dualboy_compositor_config *display,
    bool assigned[DUALBOY_MACHINE_COUNT])
{
    struct dualboy_video_frame frames[DUALBOY_MACHINE_COUNT];
    struct dualboy_geometry geometry;
    unsigned index;

    if (assigned == NULL) {
        return;
    }
    assigned[0] = false;
    assigned[1] = false;
    if (!nds_session_loaded() || core.input_state == NULL || display == NULL ||
        core.session.engine == NULL || core.session.engine->video_frame == NULL ||
        !core.session.engine->video_frame(core.session.pair, 0U, &frames[0]) ||
        !core.session.engine->video_frame(core.session.pair, 1U, &frames[1]) ||
        !dualboy_compositor_geometry(frames, display, &geometry)) {
        return;
    }

    for (index = 0U; index < DUALBOY_POINTER_CONTACT_LIMIT; ++index) {
        int16_t pointer_x;
        int16_t pointer_y;
        unsigned composite_x;
        unsigned composite_y;
        unsigned port;
        unsigned machine_x;
        unsigned machine_y;

        if (core.input_state(0U, RETRO_DEVICE_POINTER, index,
                             RETRO_DEVICE_ID_POINTER_PRESSED) == 0) {
            break;
        }
        pointer_x = core.input_state(0U, RETRO_DEVICE_POINTER, index,
                                     RETRO_DEVICE_ID_POINTER_X);
        pointer_y = core.input_state(0U, RETRO_DEVICE_POINTER, index,
                                     RETRO_DEVICE_ID_POINTER_Y);
        composite_x = normalized_coordinate(pointer_x, geometry.width);
        composite_y = normalized_coordinate(pointer_y, geometry.height);
        if (!dualboy_compositor_map_point(frames, display, composite_x,
                                          composite_y, &port, &machine_x,
                                          &machine_y) ||
            port >= DUALBOY_MACHINE_COUNT || assigned[port] ||
            machine_x >= DUALBOY_NDS_SCREEN_WIDTH ||
            machine_y < DUALBOY_NDS_SCREEN_HEIGHT ||
            machine_y >= DUALBOY_NDS_SCREEN_HEIGHT * 2U) {
            continue;
        }
        inputs[port].touch_active = true;
        inputs[port].touch_x = (uint16_t)machine_x;
        inputs[port].touch_y =
            (uint16_t)(machine_y - DUALBOY_NDS_SCREEN_HEIGHT);
        assigned[port] = true;
    }
}

static void publish_video_with_touch_cursors(
    const bool pointer_assigned[DUALBOY_MACHINE_COUNT])
{
    static const uint32_t accent[DUALBOY_MACHINE_COUNT] = {
        DUALBOY_PLAYER1_CURSOR_COLOR,
        DUALBOY_PLAYER2_CURSOR_COLOR,
    };
    const struct dualboy_video_frame *composite = &core.session.composite;
    const uint32_t *pixels;
    size_t pitch;

    if (core.video == NULL || composite->pixels == NULL ||
        composite->width == 0U || composite->height == 0U ||
        composite->width > DUALBOY_MAX_COMPOSITE_WIDTH ||
        composite->height > DUALBOY_MAX_COMPOSITE_HEIGHT ||
        composite->pitch < (size_t)composite->width * sizeof(uint32_t)) {
        return;
    }

    pixels = composite->pixels;
    pitch = composite->pitch;
    if (nds_session_loaded()) {
        struct dualboy_video_frame frames[DUALBOY_MACHINE_COUNT];
        unsigned cursor_x[DUALBOY_MACHINE_COUNT] = {0U, 0U};
        unsigned cursor_y[DUALBOY_MACHINE_COUNT] = {0U, 0U};
        bool draw[DUALBOY_MACHINE_COUNT] = {false, false};
        bool any_cursor = false;
        unsigned port;

        if (core.session.engine != NULL &&
            core.session.engine->video_frame != NULL &&
            core.session.engine->video_frame(core.session.pair, 0U,
                                             &frames[0]) &&
            core.session.engine->video_frame(core.session.pair, 1U,
                                             &frames[1])) {
            for (port = 0U; port < DUALBOY_MACHINE_COUNT; ++port) {
                draw[port] =
                    (pointer_assigned == NULL || !pointer_assigned[port]) &&
                    dualboy_touch_cursor_visible(&core.touch_cursors[port]) &&
                    dualboy_compositor_project_point(
                        frames, &core.session.display, port,
                        core.touch_cursors[port].x,
                        DUALBOY_NDS_SCREEN_HEIGHT +
                            core.touch_cursors[port].y,
                        &cursor_x[port], &cursor_y[port]);
                any_cursor = any_cursor || draw[port];
            }
        }

        if (any_cursor) {
            const size_t row_bytes =
                (size_t)composite->width * sizeof(uint32_t);
            unsigned row;

            for (row = 0U; row < composite->height; ++row) {
                const uint8_t *source =
                    (const uint8_t *)(const void *)composite->pixels +
                    (size_t)row * composite->pitch;
                memcpy(core.presentation_buffer +
                           (size_t)row * composite->width,
                       source, row_bytes);
            }
            pitch = row_bytes;
            pixels = core.presentation_buffer;
            for (port = 0U; port < DUALBOY_MACHINE_COUNT; ++port) {
                if (draw[port]) {
                    (void)dualboy_compositor_draw_cursor(
                        core.presentation_buffer, composite->width,
                        composite->height, pitch, cursor_x[port],
                        cursor_y[port], accent[port]);
                }
            }
        }
    }

    core.video(pixels, composite->width, composite->height, pitch);
}

static void reflect_link_option(bool enabled)
{
    const bool frontend_change =
        !core.frontend_link_known || core.frontend_link_value != enabled;

    core.options.link_enabled = enabled;
    core.frontend_link_value = enabled;
    core.frontend_link_known = true;
    if (frontend_change) {
        (void)force_core_option(DUALBOY_OPTION_LINK,
                                enabled ? "enabled" : "disabled");
    }
}

static void reflect_renderer_option(enum dualboy_video_renderer renderer)
{
    core.options.nds_renderer = renderer;
    (void)force_core_option(
        DUALBOY_OPTION_NDS_RENDERER,
        renderer == DUALBOY_VIDEO_RENDERER_OPENGL ? "opengl" : "software");
}

static void retire_failed_renderer_session(const char *operation)
{
    core_log(RETRO_LOG_ERROR,
             "%s left the Nintendo DS renderer unusable; discarding the session",
             operation != NULL ? operation : "renderer transition");
    discard_current();
    if (core.environment != NULL) {
        (void)core.environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

static void update_options(void)
{
    struct dualboy_options candidate;
    bool updated = false;
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (core.environment != NULL &&
        core.environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) &&
        updated) {
        bool requested_link;
        bool frontend_link_changed;
        bool renderer_changed;
        bool old_link = core.options.link_enabled;
        enum dualboy_video_renderer old_renderer =
            core.options.nds_renderer;
        enum dualboy_video_renderer requested_renderer;

        candidate = core.options;
        if (core.frontend_link_known) {
            candidate.link_enabled = core.frontend_link_value;
        }
        (void)dualboy_options_read(core.environment, &candidate);
        requested_link = candidate.link_enabled;
        frontend_link_changed = !core.frontend_link_known ||
                                requested_link != core.frontend_link_value;
        renderer_changed =
            candidate.nds_renderer != core.options.nds_renderer;

        if (nds_session_loaded() && requested_link &&
            candidate.nds_renderer == DUALBOY_VIDEO_RENDERER_OPENGL) {
            /* A real frontend normally changes one option per update. Local
             * Link wins when it alone was just enabled; renderer selection
             * wins when OpenGL was selected (or both values arrived at once). */
            if (frontend_link_changed && !renderer_changed) {
                candidate.nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
                (void)force_core_option(DUALBOY_OPTION_NDS_RENDERER,
                                        "software");
            } else {
                requested_link = false;
                candidate.link_enabled = false;
                (void)force_core_option(DUALBOY_OPTION_LINK, "disabled");
            }
        }
        core.frontend_link_value = requested_link;
        core.frontend_link_known = true;
        requested_renderer = candidate.nds_renderer;
        candidate.link_enabled = old_link;
        candidate.nds_renderer = old_renderer;
        core.options = candidate;

        if (nds_session_loaded()) {
            core.link_request_pending = false;
            core.link_failure_logged = false;

            if (requested_renderer == DUALBOY_VIDEO_RENDERER_OPENGL) {
                const bool restore_link_on_failure = old_link;

                if (old_link && !dualboy_session_set_link(
                                    &core.session, false, error,
                                    sizeof(error))) {
                    core_log(RETRO_LOG_ERROR,
                             "could not disable Local Link for OpenGL: %s",
                             error[0] != '\0' ? error
                                               : "unknown link error");
                    reflect_link_option(true);
                    reflect_renderer_option(
                        DUALBOY_VIDEO_RENDERER_SOFTWARE);
                    return;
                }
                core.options.link_enabled = false;
                core.options.nds_renderer =
                    DUALBOY_VIDEO_RENDERER_OPENGL;

                if (core.loaded_nds_renderer !=
                        DUALBOY_VIDEO_RENDERER_OPENGL &&
                    !activate_loaded_gl_renderer()) {
                    retire_failed_renderer_session("OpenGL activation");
                    return;
                }
                if (core.loaded_nds_renderer ==
                    DUALBOY_VIDEO_RENDERER_OPENGL) {
                    core.options.nds_renderer =
                        DUALBOY_VIDEO_RENDERER_OPENGL;
                    reflect_link_option(false);
                    return;
                }

                /* Missing EGL/OpenGL is a safe software fallback. Restore the
                 * link state that was disabled transactionally for activation. */
                reflect_renderer_option(DUALBOY_VIDEO_RENDERER_SOFTWARE);
                if (restore_link_on_failure &&
                    dualboy_session_set_link(&core.session, true, error,
                                             sizeof(error))) {
                    reflect_link_option(true);
                } else {
                    if (restore_link_on_failure) {
                        core_log(RETRO_LOG_ERROR,
                                 "could not restore Local Link after OpenGL fallback: %s",
                                 error[0] != '\0' ? error
                                                   : "unknown link error");
                    }
                    reflect_link_option(false);
                }
                return;
            }

            {
                const bool rollback_to_gl =
                    core.loaded_nds_renderer ==
                    DUALBOY_VIDEO_RENDERER_OPENGL;
                if (rollback_to_gl && !deactivate_loaded_gl_renderer()) {
                    reflect_link_option(false);
                    reflect_renderer_option(DUALBOY_VIDEO_RENDERER_OPENGL);
                    retire_failed_renderer_session("OpenGL teardown");
                    return;
                }
                core.options.nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;

                if (requested_link != core.options.link_enabled &&
                    !dualboy_session_set_link(&core.session, requested_link,
                                              error, sizeof(error))) {
                    core_log(RETRO_LOG_ERROR, "link option change failed: %s",
                             error[0] != '\0' ? error
                                               : "unknown engine error");
                    if (rollback_to_gl && requested_link) {
                        core.options.nds_renderer =
                            DUALBOY_VIDEO_RENDERER_OPENGL;
                        if (!activate_loaded_gl_renderer()) {
                            retire_failed_renderer_session(
                                "OpenGL rollback");
                            return;
                        }
                        if (core.loaded_nds_renderer ==
                            DUALBOY_VIDEO_RENDERER_OPENGL) {
                            reflect_renderer_option(
                                DUALBOY_VIDEO_RENDERER_OPENGL);
                        } else {
                            reflect_renderer_option(
                                DUALBOY_VIDEO_RENDERER_SOFTWARE);
                        }
                    } else {
                        reflect_renderer_option(
                            DUALBOY_VIDEO_RENDERER_SOFTWARE);
                    }
                    reflect_link_option(core.session.link_enabled);
                    return;
                }
                core.options.link_enabled = requested_link;
                core.options.nds_renderer =
                    DUALBOY_VIDEO_RENDERER_SOFTWARE;
                core.frontend_link_value = requested_link;
                core.frontend_link_known = true;
                return;
            }
        }

        /* Other engines retain the existing asynchronous link retry boundary;
         * the NDS renderer preference is simply remembered for the next load. */
        core.options.nds_renderer = requested_renderer;
        if (frontend_link_changed) {
            core.pending_link_value = requested_link;
            core.link_request_pending = true;
            core.link_failure_logged = false;
        }
    }

    if (!core.link_request_pending) {
        return;
    }
    if (!core.session.loaded) {
        core.options.link_enabled = core.pending_link_value;
        core.link_request_pending = false;
    } else if (dualboy_session_set_link(&core.session,
                                        core.pending_link_value,
                                        error, sizeof(error))) {
        core.options.link_enabled = core.pending_link_value;
        core.link_request_pending = false;
        core.link_failure_logged = false;
    } else {
        if (!core.link_failure_logged) {
            core_log(RETRO_LOG_ERROR, "link option change failed; retrying: %s",
                     error[0] != '\0' ? error : "unknown engine error");
            core.link_failure_logged = true;
        }
    }
}

static bool geometry_equal(const struct dualboy_geometry *first,
                           const struct dualboy_geometry *second)
{
    return first->width == second->width && first->height == second->height &&
           first->aspect_ratio == second->aspect_ratio;
}

static void publish_geometry(const struct dualboy_geometry *geometry)
{
    struct retro_game_geometry frontend_geometry;

    if (geometry == NULL ||
        (core.geometry_valid && geometry_equal(geometry, &core.last_geometry))) {
        return;
    }
    frontend_geometry.base_width = geometry->width;
    frontend_geometry.base_height = geometry->height;
    frontend_geometry.max_width = DUALBOY_MAX_COMPOSITE_WIDTH;
    frontend_geometry.max_height = DUALBOY_MAX_COMPOSITE_HEIGHT;
    frontend_geometry.aspect_ratio = geometry->aspect_ratio;
    if (core.environment != NULL) {
        (void)core.environment(RETRO_ENVIRONMENT_SET_GEOMETRY,
                               &frontend_geometry);
    }
    core.last_geometry = *geometry;
    core.geometry_valid = true;
}

static struct dualboy_geometry fallback_geometry(void)
{
    struct dualboy_geometry geometry;
    unsigned native_width = 160U;
    unsigned native_height = 144U;

    if (core.session.loaded) {
        if (core.session.roms[0].rom.platform == DUALBOY_PLATFORM_NDS) {
            native_width = DUALBOY_NDS_SCREEN_WIDTH;
            native_height = DUALBOY_NDS_SCREEN_HEIGHT * 2U;
        } else if (core.session.roms[0].rom.platform == DUALBOY_PLATFORM_GBA) {
            native_width = 240U;
            native_height = 160U;
        }
    }
    if (core.options.mode != DUALBOY_MODE_DUAL) {
        geometry.width = native_width;
        geometry.height = native_height;
    } else if (core.options.layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
        geometry.width = native_width;
        geometry.height = native_height * 2U;
    } else {
        geometry.width = native_width * 2U;
        geometry.height = native_height;
    }
    geometry.aspect_ratio = (float)geometry.width / (float)geometry.height;
    return geometry;
}

static void drain_audio(void)
{
    const struct dualboy_engine_ops *engine = core.session.engine;
    unsigned pass;

    if (engine == NULL || engine->read_audio == NULL) {
        return;
    }
    for (pass = 0U; pass < DUALBOY_AUDIO_DRAIN_LIMIT; ++pass) {
        size_t frames = engine->read_audio(core.session.pair,
                                           core.audio_buffer,
                                           DUALBOY_AUDIO_BUFFER_FRAMES);
        size_t frame;

        if (frames == 0U) {
            break;
        }
        if (frames > DUALBOY_AUDIO_BUFFER_FRAMES) {
            core_log(RETRO_LOG_ERROR,
                     "engine returned an oversized audio batch");
            frames = DUALBOY_AUDIO_BUFFER_FRAMES;
        }
        if (core.options.audio_player1) {
            if (core.audio_batch != NULL) {
                (void)core.audio_batch(core.audio_buffer, frames);
            } else if (core.audio != NULL) {
                for (frame = 0U; frame < frames; ++frame) {
                    core.audio(core.audio_buffer[frame * 2U],
                               core.audio_buffer[frame * 2U + 1U]);
                }
            }
        }
        if (frames < DUALBOY_AUDIO_BUFFER_FRAMES) {
            break;
        }
    }
}

static void tick_saves(void)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (!dualboy_save_manager_tick(&core.saves, &core.session, error,
                                   sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "periodic save flush failed: %s",
                 error[0] != '\0' ? error : "unknown persistence error");
    }
}

static bool resolve_memory(unsigned id,
                           unsigned *machine,
                           enum dualboy_memory_kind *kind)
{
    if (machine == NULL || kind == NULL) {
        return false;
    }
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
        case DUALBOY_SLOT1_SRAM_ID:
            *machine = 0U;
            *kind = DUALBOY_MEMORY_SAVE_RAM;
            return true;
        case RETRO_MEMORY_RTC:
        case DUALBOY_SLOT1_RTC_ID:
            *machine = 0U;
            *kind = DUALBOY_MEMORY_RTC;
            return true;
        case DUALBOY_SLOT2_SRAM_ID:
            *machine = 1U;
            *kind = DUALBOY_MEMORY_SAVE_RAM;
            return true;
        case DUALBOY_SLOT2_RTC_ID:
            *machine = 1U;
            *kind = DUALBOY_MEMORY_RTC;
            return true;
        default:
            return false;
    }
}

void retro_set_environment(retro_environment_t callback)
{
    core.environment = callback;
    register_environment_interfaces();
    refresh_frontend_services();
}

void retro_set_video_refresh(retro_video_refresh_t callback)
{
    core.video = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback)
{
    core.audio = callback;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback)
{
    core.audio_batch = callback;
}

void retro_set_input_poll(retro_input_poll_t callback)
{
    core.input_poll = callback;
}

void retro_set_input_state(retro_input_state_t callback)
{
    core.input_state = callback;
}

void retro_init(void)
{
    retro_environment_t environment = core.environment;
    retro_video_refresh_t video = core.video;
    retro_audio_sample_t audio = core.audio;
    retro_audio_sample_batch_t audio_batch = core.audio_batch;
    retro_input_poll_t input_poll = core.input_poll;
    retro_input_state_t input_state = core.input_state;

    unload_current();
    memset(&core, 0, sizeof(core));
    core.environment = environment;
    core.video = video;
    core.audio = audio;
    core.audio_batch = audio_batch;
    core.input_poll = input_poll;
    core.input_state = input_state;
    core.port_devices[0] = RETRO_DEVICE_JOYPAD;
    core.port_devices[1] = RETRO_DEVICE_JOYPAD;
    dualboy_session_init(&core.session);
    dualboy_options_set_defaults(&core.options);
    refresh_frontend_services();
    read_initial_options();
    core.initialized = true;
}

void retro_deinit(void)
{
    unload_current();
    memset(&core, 0, sizeof(core));
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
    info->valid_extensions = "gb|gbc|gba|nds|m3u";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    struct dualboy_geometry geometry;
    struct dualboy_compositor_config display = current_display();
    unsigned sample_rate = DUALBOY_DEFAULT_SAMPLE_RATE;
    double frame_rate = DUALBOY_FRAME_RATE;

    if (info == NULL) {
        return;
    }
    if (!dualboy_session_geometry(&core.session, &display, &geometry)) {
        geometry = fallback_geometry();
    }
    if (core.session.loaded && core.session.engine != NULL &&
        core.session.engine->audio_sample_rate != NULL) {
        const unsigned engine_rate =
            core.session.engine->audio_sample_rate(core.session.pair);
        if (engine_rate != 0U) {
            sample_rate = engine_rate;
        }
    }
    if (core.session.loaded && core.session.engine != NULL &&
        core.session.engine->frame_rate != NULL) {
        const double engine_rate =
            core.session.engine->frame_rate(core.session.pair);
        if (engine_rate > 0.0) {
            frame_rate = engine_rate;
        }
    }
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = geometry.width;
    info->geometry.base_height = geometry.height;
    info->geometry.max_width = DUALBOY_MAX_COMPOSITE_WIDTH;
    info->geometry.max_height = DUALBOY_MAX_COMPOSITE_HEIGHT;
    info->geometry.aspect_ratio = geometry.aspect_ratio;
    info->timing.fps = frame_rate;
    info->timing.sample_rate = (double)sample_rate;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    const unsigned base_device = device & RETRO_DEVICE_MASK;

    if (port >= DUALBOY_MACHINE_COUNT) {
        return;
    }
    if (base_device == RETRO_DEVICE_JOYPAD ||
        base_device == RETRO_DEVICE_ANALOG ||
        base_device == RETRO_DEVICE_NONE) {
        core.port_devices[port] = base_device;
    } else {
        core.port_devices[port] = RETRO_DEVICE_NONE;
        core_log(RETRO_LOG_WARN,
                 "unsupported device %u on port %u; input disabled", device,
                 port + 1U);
    }
}

void retro_reset(void)
{
    reset_touch_cursors();
    dualboy_session_reset(&core.session);
    synchronize_fastforward_policy();
}

void retro_run(void)
{
    struct dualboy_machine_input inputs[DUALBOY_MACHINE_COUNT];
    struct dualboy_compositor_config display;
    struct dualboy_geometry geometry;
    bool pointer_assigned[DUALBOY_MACHINE_COUNT] = {false, false};
    uint64_t cursor_time_usec = 0U;
    const bool cursor_time_available = touch_cursor_time(&cursor_time_usec);
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (core.input_poll != NULL) {
        core.input_poll();
    }
    if (!core.session.loaded) {
        return;
    }
    update_options();
    if (!core.session.loaded) {
        return;
    }
    display = current_display();
    synchronize_fastforward_policy();
    inputs[0] = read_port_input(0U, cursor_time_available,
                                cursor_time_usec);
    inputs[1] = read_port_input(1U, cursor_time_available,
                                cursor_time_usec);
    apply_pointer_contacts(inputs, &display, pointer_assigned);
    if (!dualboy_session_run(&core.session, inputs, &display, error,
                             sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "emulation frame failed: %s",
                 error[0] != '\0' ? error : "unknown engine error");
        publish_video_with_touch_cursors(pointer_assigned);
        /* A failed frame is not a publication boundary. Keep the last complete
         * audio/save observation; normal unload still force-flushes once the
         * synchronous engine call has returned quiescent. */
        synchronize_fastforward_policy();
        return;
    }

    synchronize_fastforward_policy();

    geometry.width = core.session.composite.width;
    geometry.height = core.session.composite.height;
    geometry.aspect_ratio = (float)geometry.width / (float)geometry.height;
    publish_geometry(&geometry);
    publish_video_with_touch_cursors(pointer_assigned);
    drain_audio();
    tick_saves();
}

size_t retro_serialize_size(void)
{
    return dualboy_state_size(&core.session);
}

bool retro_serialize(void *data, size_t size)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};

    if (!dualboy_state_serialize(&core.session, data, size, error,
                                sizeof(error))) {
        core_log(RETRO_LOG_ERROR, "savestate creation failed: %s",
                 error[0] != '\0' ? error : "unknown state error");
        return false;
    }
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    char error[DUALBOY_ERROR_CAPACITY] = {0};
    enum dualboy_state_restore_result result;

    result = dualboy_state_unserialize_ex(&core.session, data, size, error,
                                          sizeof(error));
    if (result != DUALBOY_STATE_RESTORE_OK) {
        core_log(RETRO_LOG_ERROR, "savestate restore failed: %s",
                 error[0] != '\0' ? error : "unknown state error");
        if (result == DUALBOY_STATE_RESTORE_FATAL) {
            core_log(RETRO_LOG_ERROR,
                     "discarding session after incomplete state rollback");
            discard_current();
            if (core.environment != NULL) {
                (void)core.environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
            }
        }
        return false;
    }
    core.options.link_enabled = core.session.link_enabled;
    /* A state owns its encoded link/scheduler state. Do not let a stale failed
     * UI request immediately replace it; a subsequent explicit option change
     * will create a fresh pending request. */
    core.link_request_pending = false;
    core.link_failure_logged = false;
    core.geometry_valid = false;
    synchronize_fastforward_policy();
    return true;
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
    struct dualboy_rom roms[DUALBOY_MACHINE_COUNT];

    ensure_initialized();
    unload_current();
    read_initial_options();
    if (game == NULL) {
        core_log(RETRO_LOG_ERROR, "DualBoy requires cartridge content");
        return false;
    }
    if (path_has_extension(game->path, "m3u")) {
        if (game->path == NULL || game->path[0] == '\0') {
            core_log(RETRO_LOG_ERROR, "M3U content requires a local file path");
            return false;
        }
        return load_playlist(game->path);
    }
    if (game->data == NULL || game->size == 0U) {
        core_log(RETRO_LOG_ERROR,
                 "cartridge content is empty (non-M3U content must be memory-loaded)");
        return false;
    }

    memset(roms, 0, sizeof(roms));
    roms[0].data = game->data;
    roms[0].size = game->size;
    roms[0].path = game->path;
    roms[1] = roms[0];
    return load_detected_pair(roms, DUALBOY_LOAD_NORMAL);
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info *info,
                             size_t num_info)
{
    struct dualboy_rom roms[DUALBOY_MACHINE_COUNT];
    unsigned machine;

    ensure_initialized();
    unload_current();
    read_initial_options();
    if (game_type != DUALBOY_SUBSYSTEM_ID) {
        core_log(RETRO_LOG_ERROR, "unknown subsystem id 0x%x", game_type);
        return false;
    }
    if (info == NULL || num_info != DUALBOY_MACHINE_COUNT) {
        core_log(RETRO_LOG_ERROR,
                 "dualboylink requires exactly two cartridge files (received %zu)",
                 num_info);
        return false;
    }
    memset(roms, 0, sizeof(roms));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (info[machine].data == NULL || info[machine].size == 0U) {
            core_log(RETRO_LOG_ERROR,
                     "dualboylink player %u cartridge is missing or empty",
                     machine + 1U);
            return false;
        }
        roms[machine].data = info[machine].data;
        roms[machine].size = info[machine].size;
        roms[machine].path = info[machine].path;
    }
    return load_detected_pair(roms, DUALBOY_LOAD_SUBSYSTEM);
}

void retro_unload_game(void)
{
    unload_current();
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    unsigned machine;
    enum dualboy_memory_kind kind;
    void *data = NULL;
    size_t size = 0U;

    if (!resolve_memory(id, &machine, &kind) ||
        !dualboy_save_manager_frontend_memory(&core.saves, &core.session,
                                              machine, kind, &data, &size)) {
        return NULL;
    }
    return data;
}

size_t retro_get_memory_size(unsigned id)
{
    unsigned machine;
    enum dualboy_memory_kind kind;
    void *data = NULL;
    size_t size = 0U;

    if (!resolve_memory(id, &machine, &kind) ||
        !dualboy_save_manager_frontend_memory(&core.saves, &core.session,
                                              machine, kind, &data, &size)) {
        return 0U;
    }
    return size;
}
