/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _POSIX_C_SOURCE 200809L

#include <libretro.h>

#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_GB_ROM_SIZE 0x8000U
#define TEST_GBA_ROM_SIZE 0x8000U
#define TEST_GBA_CODE_OFFSET 0xC0U
#define TEST_GBA_IO_LITERAL 0x100U
#define TEST_GBA_SRAM_LITERAL 0x104U
#define TEST_GBA_VRAM_LITERAL 0x108U
#define TEST_GBA_SIGNATURE_OFFSET 0x140U
#define TEST_GBA_FRAME_COUNT 30U
#define TEST_BOOT_FRAME_LIMIT 360U
#define TEST_PATH_CAPACITY 1024U
#define TEST_LOG_CAPACITY 512U
#define TEST_SLOT1_SRAM_ID 0x100U
#define TEST_SLOT1_RTC_ID 0x101U
#define TEST_SLOT2_SRAM_ID 0x200U
#define TEST_SLOT2_RTC_ID 0x201U

#define REQUIRE(expression)                                                     \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "REQUIRE failed at %s:%d: %s\n", __FILE__,        \
                    __LINE__, #expression);                                     \
            return false;                                                       \
        }                                                                       \
    } while (false)

typedef void (*set_environment_fn)(retro_environment_t);
typedef void (*set_video_refresh_fn)(retro_video_refresh_t);
typedef void (*set_audio_sample_fn)(retro_audio_sample_t);
typedef void (*set_audio_sample_batch_fn)(retro_audio_sample_batch_t);
typedef void (*set_input_poll_fn)(retro_input_poll_t);
typedef void (*set_input_state_fn)(retro_input_state_t);
typedef void (*init_fn)(void);
typedef void (*deinit_fn)(void);
typedef unsigned (*api_version_fn)(void);
typedef void (*get_system_info_fn)(struct retro_system_info *);
typedef void (*get_system_av_info_fn)(struct retro_system_av_info *);
typedef void (*set_controller_port_device_fn)(unsigned, unsigned);
typedef void (*reset_fn)(void);
typedef void (*run_fn)(void);
typedef size_t (*serialize_size_fn)(void);
typedef bool (*serialize_fn)(void *, size_t);
typedef bool (*unserialize_fn)(const void *, size_t);
typedef void (*cheat_reset_fn)(void);
typedef void (*cheat_set_fn)(unsigned, bool, const char *);
typedef bool (*load_game_fn)(const struct retro_game_info *);
typedef bool (*load_game_special_fn)(unsigned,
                                     const struct retro_game_info *,
                                     size_t);
typedef void (*unload_game_fn)(void);
typedef unsigned (*get_region_fn)(void);
typedef void *(*get_memory_data_fn)(unsigned);
typedef size_t (*get_memory_size_fn)(unsigned);

struct core_api {
    set_environment_fn set_environment;
    set_video_refresh_fn set_video_refresh;
    set_audio_sample_fn set_audio_sample;
    set_audio_sample_batch_fn set_audio_sample_batch;
    set_input_poll_fn set_input_poll;
    set_input_state_fn set_input_state;
    init_fn init;
    deinit_fn deinit;
    api_version_fn api_version;
    get_system_info_fn get_system_info;
    get_system_av_info_fn get_system_av_info;
    set_controller_port_device_fn set_controller_port_device;
    reset_fn reset;
    run_fn run;
    serialize_size_fn serialize_size;
    serialize_fn serialize;
    unserialize_fn unserialize;
    cheat_reset_fn cheat_reset;
    cheat_set_fn cheat_set;
    load_game_fn load_game;
    load_game_special_fn load_game_special;
    unload_game_fn unload_game;
    get_region_fn get_region;
    get_memory_data_fn get_memory_data;
    get_memory_size_fn get_memory_size;
};

struct frontend_fixture {
    char directory[TEST_PATH_CAPACITY];
    const struct retro_subsystem_info *subsystems;
    const struct retro_system_content_info_override *content_overrides;
    const struct retro_controller_info *controllers;
    const struct retro_input_descriptor *input_descriptors;
    const struct retro_core_options_v2 *options_v2;
    const char *mode;
    const char *layout;
    const char *link;
    const char *swap;
    const char *audio_source;
    uint16_t input_masks[2];
    struct retro_game_geometry geometry;
    unsigned subsystem_calls;
    unsigned content_override_calls;
    unsigned controller_calls;
    unsigned input_descriptor_calls;
    unsigned option_registration_calls;
    unsigned pixel_format_calls;
    unsigned geometry_calls;
    unsigned video_calls;
    unsigned video_width;
    unsigned video_height;
    size_t video_pitch;
    uint64_t video_hash;
    uint64_t first_screen_hash;
    uint64_t second_screen_hash;
    unsigned audio_sample_calls;
    unsigned audio_batch_calls;
    size_t audio_frames;
    unsigned input_poll_calls;
    unsigned input_state_calls[2];
    unsigned log_calls;
    unsigned error_log_calls;
    char last_log[TEST_LOG_CAPACITY];
    bool support_no_game_seen;
    bool support_no_game;
    bool option_updated;
    bool video_contract_ok;
    bool audio_contract_ok;
    bool input_contract_ok;
};

static struct frontend_fixture frontend;

static const uint8_t test_gb_logo[48] = {
    0xCEU, 0xEDU, 0x66U, 0x66U, 0xCCU, 0x0DU, 0x00U, 0x0BU,
    0x03U, 0x73U, 0x00U, 0x83U, 0x00U, 0x0CU, 0x00U, 0x0DU,
    0x00U, 0x08U, 0x11U, 0x1FU, 0x88U, 0x89U, 0x00U, 0x0EU,
    0xDCU, 0xCCU, 0x6EU, 0xE6U, 0xDDU, 0xDDU, 0xD9U, 0x99U,
    0xBBU, 0xBBU, 0x67U, 0x63U, 0x6EU, 0x0EU, 0xECU, 0xCCU,
    0xDDU, 0xDCU, 0x99U, 0x9FU, 0xBBU, 0xB9U, 0x33U, 0x3EU,
};

static const uint8_t test_gba_logo[156] = {
    0x24U, 0xFFU, 0xAEU, 0x51U, 0x69U, 0x9AU, 0xA2U, 0x21U,
    0x3DU, 0x84U, 0x82U, 0x0AU, 0x84U, 0xE4U, 0x09U, 0xADU,
    0x11U, 0x24U, 0x8BU, 0x98U, 0xC0U, 0x81U, 0x7FU, 0x21U,
    0xA3U, 0x52U, 0xBEU, 0x19U, 0x93U, 0x09U, 0xCEU, 0x20U,
    0x10U, 0x46U, 0x4AU, 0x4AU, 0xF8U, 0x27U, 0x31U, 0xECU,
    0x58U, 0xC7U, 0xE8U, 0x33U, 0x82U, 0xE3U, 0xCEU, 0xBFU,
    0x85U, 0xF4U, 0xDFU, 0x94U, 0xCEU, 0x4BU, 0x09U, 0xC1U,
    0x94U, 0x56U, 0x8AU, 0xC0U, 0x13U, 0x72U, 0xA7U, 0xFCU,
    0x9FU, 0x84U, 0x4DU, 0x73U, 0xA3U, 0xCAU, 0x9AU, 0x61U,
    0x58U, 0x97U, 0xA3U, 0x27U, 0xFCU, 0x03U, 0x98U, 0x76U,
    0x23U, 0x1DU, 0xC7U, 0x61U, 0x03U, 0x04U, 0xAEU, 0x56U,
    0xBFU, 0x38U, 0x84U, 0x00U, 0x40U, 0xA7U, 0x0EU, 0xFDU,
    0xFFU, 0x52U, 0xFEU, 0x03U, 0x6FU, 0x95U, 0x30U, 0xF1U,
    0x97U, 0xFBU, 0xC0U, 0x85U, 0x60U, 0xD6U, 0x80U, 0x25U,
    0xA9U, 0x63U, 0xBEU, 0x03U, 0x01U, 0x4EU, 0x38U, 0xE2U,
    0xF9U, 0xA2U, 0x34U, 0xFFU, 0xBBU, 0x3EU, 0x03U, 0x44U,
    0x78U, 0x00U, 0x90U, 0xCBU, 0x88U, 0x11U, 0x3AU, 0x94U,
    0x65U, 0xC0U, 0x7CU, 0x63U, 0x87U, 0xF0U, 0x3CU, 0xAFU,
    0xD6U, 0x25U, 0xE4U, 0x8BU, 0x38U, 0x0AU, 0xACU, 0x72U,
    0x21U, 0xD4U, 0xF8U, 0x07U,
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

static void bind_function(void *destination,
                          size_t destination_size,
                          void *symbol,
                          const char *name)
{
    if (destination_size != sizeof(symbol)) {
        fprintf(stderr, "cannot represent Libretro function %s\n", name);
        exit(EXIT_FAILURE);
    }
    memcpy(destination, &symbol, sizeof(symbol));
}

#define BIND(api_, member_)                                                     \
    bind_function(&(api_)->member_, sizeof((api_)->member_),                   \
                  load_symbol(handle, "retro_" #member_),                     \
                  "retro_" #member_)

static void load_core_api(void *handle, struct core_api *api)
{
    memset(api, 0, sizeof(*api));
    BIND(api, set_environment);
    BIND(api, set_video_refresh);
    BIND(api, set_audio_sample);
    BIND(api, set_audio_sample_batch);
    BIND(api, set_input_poll);
    BIND(api, set_input_state);
    BIND(api, init);
    BIND(api, deinit);
    BIND(api, api_version);
    BIND(api, get_system_info);
    BIND(api, get_system_av_info);
    BIND(api, set_controller_port_device);
    BIND(api, reset);
    BIND(api, run);
    BIND(api, serialize_size);
    BIND(api, serialize);
    BIND(api, unserialize);
    BIND(api, cheat_reset);
    BIND(api, cheat_set);
    BIND(api, load_game);
    BIND(api, load_game_special);
    BIND(api, unload_game);
    BIND(api, get_region);
    BIND(api, get_memory_data);
    BIND(api, get_memory_size);
}

#undef BIND

static const char *option_value(const char *key)
{
    if (strcmp(key, "dualboy_mode") == 0) {
        return frontend.mode;
    }
    if (strcmp(key, "dualboy_layout") == 0) {
        return frontend.layout;
    }
    if (strcmp(key, "dualboy_link") == 0) {
        return frontend.link;
    }
    if (strcmp(key, "dualboy_swap_players") == 0) {
        return frontend.swap;
    }
    if (strcmp(key, "dualboy_audio_source") == 0) {
        return frontend.audio_source;
    }
    return NULL;
}

static void RETRO_CALLCONV frontend_log(enum retro_log_level level,
                                        const char *format,
                                        ...)
{
    va_list arguments;

    ++frontend.log_calls;
    if (level == RETRO_LOG_ERROR) {
        ++frontend.error_log_calls;
    }
    va_start(arguments, format);
    (void)vsnprintf(frontend.last_log, sizeof(frontend.last_log), format,
                    arguments);
    va_end(arguments);
}

static bool RETRO_CALLCONV environment_callback(unsigned command, void *data)
{
    switch (command) {
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            if (data == NULL) {
                return false;
            }
            frontend.support_no_game_seen = true;
            frontend.support_no_game = *(const bool *)data;
            return true;
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
            frontend.subsystems =
                (const struct retro_subsystem_info *)data;
            ++frontend.subsystem_calls;
            return data != NULL;
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
            frontend.content_overrides =
                (const struct retro_system_content_info_override *)data;
            ++frontend.content_override_calls;
            return data != NULL;
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            frontend.controllers = (const struct retro_controller_info *)data;
            ++frontend.controller_calls;
            return data != NULL;
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            frontend.input_descriptors =
                (const struct retro_input_descriptor *)data;
            ++frontend.input_descriptor_calls;
            return data != NULL;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            if (data == NULL) {
                return false;
            }
            *(unsigned *)data = 2U;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            frontend.options_v2 = (const struct retro_core_options_v2 *)data;
            ++frontend.option_registration_calls;
            return data != NULL;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            if (data == NULL) {
                return false;
            }
            {
                struct retro_variable *variable = (struct retro_variable *)data;
                variable->value = variable->key != NULL
                                      ? option_value(variable->key)
                                      : NULL;
                return variable->value != NULL;
            }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            if (data == NULL) {
                return false;
            }
            *(bool *)data = frontend.option_updated;
            frontend.option_updated = false;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            if (data == NULL) {
                return false;
            }
            ((struct retro_log_callback *)data)->log = frontend_log;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            if (data == NULL) {
                return false;
            }
            *(const char **)data = frontend.directory;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            ++frontend.pixel_format_calls;
            return data != NULL &&
                   *(const enum retro_pixel_format *)data ==
                       RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            if (data == NULL) {
                return false;
            }
            frontend.geometry = *(const struct retro_game_geometry *)data;
            ++frontend.geometry_calls;
            return true;
        default:
            return false;
    }
}

static uint64_t hash_rectangle(const uint8_t *pixels,
                               size_t pitch,
                               unsigned x,
                               unsigned y,
                               unsigned width,
                               unsigned height)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned row;

    for (row = 0U; row < height; ++row) {
        const uint32_t *source =
            (const uint32_t *)(const void *)(pixels + (size_t)(y + row) * pitch);
        unsigned column;

        for (column = 0U; column < width; ++column) {
            hash ^= source[x + column];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
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

static void RETRO_CALLCONV video_callback(const void *data,
                                          unsigned width,
                                          unsigned height,
                                          size_t pitch)
{
    ++frontend.video_calls;
    frontend.video_width = width;
    frontend.video_height = height;
    frontend.video_pitch = pitch;
    if (data == NULL || width == 0U || height == 0U ||
        pitch < (size_t)width * sizeof(uint32_t)) {
        frontend.video_contract_ok = false;
        return;
    }
    frontend.video_hash = hash_rectangle((const uint8_t *)data, pitch, 0U, 0U,
                                         width, height);
    if (width == 320U && height == 144U) {
        frontend.first_screen_hash = hash_rectangle(
            (const uint8_t *)data, pitch, 0U, 0U, 160U, 144U);
        frontend.second_screen_hash = hash_rectangle(
            (const uint8_t *)data, pitch, 160U, 0U, 160U, 144U);
    }
}

static void RETRO_CALLCONV audio_sample_callback(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
    ++frontend.audio_sample_calls;
    ++frontend.audio_frames;
}

static size_t RETRO_CALLCONV audio_batch_callback(const int16_t *data,
                                                  size_t frames)
{
    ++frontend.audio_batch_calls;
    if (frames > 0U && data == NULL) {
        frontend.audio_contract_ok = false;
    }
    frontend.audio_frames += frames;
    return frames;
}

static void RETRO_CALLCONV input_poll_callback(void)
{
    ++frontend.input_poll_calls;
}

static int16_t RETRO_CALLCONV input_state_callback(unsigned port,
                                                   unsigned device,
                                                   unsigned index,
                                                   unsigned id)
{
    if (port >= 2U || device != RETRO_DEVICE_JOYPAD || index != 0U ||
        id != RETRO_DEVICE_ID_JOYPAD_MASK) {
        frontend.input_contract_ok = false;
        return 0;
    }
    ++frontend.input_state_calls[port];
    return (int16_t)frontend.input_masks[port];
}

static void reset_observations(void)
{
    frontend.geometry_calls = 0U;
    frontend.video_calls = 0U;
    frontend.video_width = 0U;
    frontend.video_height = 0U;
    frontend.video_pitch = 0U;
    frontend.video_hash = 0U;
    frontend.first_screen_hash = 0U;
    frontend.second_screen_hash = 0U;
    frontend.audio_sample_calls = 0U;
    frontend.audio_batch_calls = 0U;
    frontend.audio_frames = 0U;
    frontend.input_poll_calls = 0U;
    frontend.input_state_calls[0] = 0U;
    frontend.input_state_calls[1] = 0U;
    frontend.video_contract_ok = true;
    frontend.audio_contract_ok = true;
    frontend.input_contract_ok = true;
}

static void set_default_options(void)
{
    frontend.mode = "dual";
    frontend.layout = "side_by_side";
    frontend.link = "enabled";
    frontend.swap = "disabled";
    frontend.audio_source = "player1";
    frontend.option_updated = true;
}

static void emit_byte(uint8_t *rom, size_t *cursor, uint8_t value)
{
    if (*cursor < TEST_GB_ROM_SIZE) {
        rom[*cursor] = value;
    }
    ++*cursor;
}

/* This original LR35902 program enables battery RAM, exposes a per-ROM marker
 * and the current directional-key nibble in SRAM, and paints a static screen.
 * No copyrighted cartridge code or data is used beyond the hardware-mandated
 * Nintendo header logo. */
static bool make_test_gb_rom(uint8_t *rom,
                             bool color,
                             uint8_t marker,
                             uint8_t background_palette)
{
    size_t cursor = 0x150U;
    uint8_t checksum = 0U;
    size_t index;

    memset(rom, 0, TEST_GB_ROM_SIZE);
    rom[0x100U] = 0x00U;
    rom[0x101U] = 0xC3U;
    rom[0x102U] = 0x50U;
    rom[0x103U] = 0x01U;
    memcpy(rom + 0x104U, test_gb_logo, sizeof(test_gb_logo));
    memcpy(rom + 0x134U, "DUALBOY TEST", 12U);
    rom[0x143U] = color ? 0x80U : 0x00U;
    rom[0x146U] = 0x00U;
    rom[0x147U] = 0x10U;
    rom[0x148U] = 0x00U;
    rom[0x149U] = 0x03U;

    emit_byte(rom, &cursor, 0xF3U); /* di */
    emit_byte(rom, &cursor, 0x3EU); /* ld a, $0a */
    emit_byte(rom, &cursor, 0x0AU);
    emit_byte(rom, &cursor, 0xEAU); /* ld ($0000), a */
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0x3EU); /* ld a, marker */
    emit_byte(rom, &cursor, marker);
    emit_byte(rom, &cursor, 0xEAU); /* ld ($a000), a */
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0xA0U);
    emit_byte(rom, &cursor, 0xAFU); /* xor a */
    emit_byte(rom, &cursor, 0xE0U); /* ldh ($40), a: LCD off */
    emit_byte(rom, &cursor, 0x40U);

    emit_byte(rom, &cursor, 0x21U); /* ld hl, $8000 */
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0x80U);
    emit_byte(rom, &cursor, 0x06U); /* ld b, 16 */
    emit_byte(rom, &cursor, 0x10U);
    emit_byte(rom, &cursor, 0xAFU); /* clear tile zero */
    emit_byte(rom, &cursor, 0x22U); /* ld (hl+), a */
    emit_byte(rom, &cursor, 0x05U); /* dec b */
    emit_byte(rom, &cursor, 0x20U); /* jr nz, -4 */
    emit_byte(rom, &cursor, 0xFCU);

    emit_byte(rom, &cursor, 0x21U); /* ld hl, $9800 */
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0x98U);
    emit_byte(rom, &cursor, 0x01U); /* ld bc, $0400 */
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0x04U);
    emit_byte(rom, &cursor, 0xAFU); /* clear background map */
    emit_byte(rom, &cursor, 0x22U);
    emit_byte(rom, &cursor, 0x0BU); /* dec bc */
    emit_byte(rom, &cursor, 0x78U); /* ld a, b */
    emit_byte(rom, &cursor, 0xB1U); /* or c */
    emit_byte(rom, &cursor, 0x20U); /* jr nz, -7 */
    emit_byte(rom, &cursor, 0xF9U);

    emit_byte(rom, &cursor, 0x3EU); /* ld a, DMG palette */
    emit_byte(rom, &cursor, background_palette);
    emit_byte(rom, &cursor, 0xE0U);
    emit_byte(rom, &cursor, 0x47U);
    if (color) {
        emit_byte(rom, &cursor, 0x3EU); /* CGB BG palette index + increment */
        emit_byte(rom, &cursor, 0x80U);
        emit_byte(rom, &cursor, 0xE0U);
        emit_byte(rom, &cursor, 0x68U);
        emit_byte(rom, &cursor, 0x3EU); /* color zero = green (0x03e0) */
        emit_byte(rom, &cursor, 0xE0U);
        emit_byte(rom, &cursor, 0xE0U);
        emit_byte(rom, &cursor, 0x69U);
        emit_byte(rom, &cursor, 0x3EU);
        emit_byte(rom, &cursor, 0x03U);
        emit_byte(rom, &cursor, 0xE0U);
        emit_byte(rom, &cursor, 0x69U);
    }
    emit_byte(rom, &cursor, 0x3EU); /* ld a, $91 */
    emit_byte(rom, &cursor, 0x91U);
    emit_byte(rom, &cursor, 0xE0U); /* ldh ($40), a: LCD on */
    emit_byte(rom, &cursor, 0x40U);

    emit_byte(rom, &cursor, 0x3EU); /* ld a, $20: directions */
    emit_byte(rom, &cursor, 0x20U);
    emit_byte(rom, &cursor, 0xE0U);
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0xF0U);
    emit_byte(rom, &cursor, 0x00U);
    emit_byte(rom, &cursor, 0xEAU); /* ld ($a001), a */
    emit_byte(rom, &cursor, 0x01U);
    emit_byte(rom, &cursor, 0xA0U);
    emit_byte(rom, &cursor, 0x18U); /* jr -11 */
    emit_byte(rom, &cursor, 0xF5U);

    if (cursor >= TEST_GB_ROM_SIZE) {
        return false;
    }
    for (index = 0x134U; index <= 0x14CU; ++index) {
        checksum = (uint8_t)(checksum - rom[index] - 1U);
    }
    rom[0x14DU] = checksum;
    return true;
}

static void put_u32le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

/* An original ARM-state program that selects mode 3, writes a marker to SRAM,
 * paints one pixel, and then remains live. The SRAM signature is the public
 * cartridge convention mGBA uses to select the save device. */
static bool make_test_gba_rom(uint8_t *rom)
{
    uint8_t checksum = 0U;
    size_t index;

    memset(rom, 0, TEST_GBA_ROM_SIZE);
    put_u32le(rom, UINT32_C(0xEA00002E)); /* b 0x080000c0 */
    memcpy(rom + 0x04U, test_gba_logo, sizeof(test_gba_logo));
    memcpy(rom + 0xA0U, "DUALBOY ABI ", 12U);
    memcpy(rom + 0xACU, "DBAB", 4U);
    memcpy(rom + 0xB0U, "00", 2U);
    rom[0xB2U] = 0x96U;

    put_u32le(rom + TEST_GBA_CODE_OFFSET + 0U,
              UINT32_C(0xE59F0038)); /* ldr r0, =0x04000000 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 4U,
              UINT32_C(0xE59F1038)); /* ldr r1, =0x0e000000 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 8U,
              UINT32_C(0xE59F2038)); /* ldr r2, =0x06000000 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 12U,
              UINT32_C(0xE3A03003)); /* mov r3, #3 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 16U,
              UINT32_C(0xE3833B01)); /* orr r3, r3, #0x400 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 20U,
              UINT32_C(0xE1C030B0)); /* strh r3, [r0] */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 24U,
              UINT32_C(0xE3A03074)); /* mov r3, #0x74 */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 28U,
              UINT32_C(0xE5C13000)); /* strb r3, [r1] */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 32U,
              UINT32_C(0xE3A0301F)); /* mov r3, #0x1f */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 36U,
              UINT32_C(0xE1C230B0)); /* strh r3, [r2] */
    put_u32le(rom + TEST_GBA_CODE_OFFSET + 40U,
              UINT32_C(0xEAFFFFFE)); /* b . */
    put_u32le(rom + TEST_GBA_IO_LITERAL, UINT32_C(0x04000000));
    put_u32le(rom + TEST_GBA_SRAM_LITERAL, UINT32_C(0x0E000000));
    put_u32le(rom + TEST_GBA_VRAM_LITERAL, UINT32_C(0x06000000));
    memcpy(rom + TEST_GBA_SIGNATURE_OFFSET, "SRAM_V110", 9U);

    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
    return true;
}

static bool validate_registration(const struct core_api *api)
{
    static const char *const option_keys[] = {
        "dualboy_mode", "dualboy_layout", "dualboy_link",
        "dualboy_swap_players", "dualboy_audio_source",
    };
    static const char *const option_defaults[] = {
        "dual", "side_by_side", "enabled", "disabled", "player1",
    };
    struct retro_system_info info;
    struct retro_system_av_info av;
    const struct retro_subsystem_info *subsystem;
    size_t index;
    size_t descriptor_count = 0U;
    size_t category_count = 0U;

    REQUIRE(api->api_version() == RETRO_API_VERSION);
    memset(&info, 0, sizeof(info));
    api->get_system_info(&info);
    REQUIRE(info.library_name != NULL);
    REQUIRE(strcmp(info.library_name, "DualBoy") == 0);
    REQUIRE(info.library_version != NULL && info.library_version[0] != '\0');
    REQUIRE(info.valid_extensions != NULL);
    REQUIRE(strcmp(info.valid_extensions, "gb|gbc|gba|m3u") == 0);
    REQUIRE(!info.need_fullpath && !info.block_extract);

    memset(&av, 0, sizeof(av));
    api->get_system_av_info(&av);
    REQUIRE(av.geometry.base_width == 320U);
    REQUIRE(av.geometry.base_height == 144U);
    REQUIRE(av.geometry.max_width == 480U);
    REQUIRE(av.geometry.max_height == 320U);
    REQUIRE(av.timing.fps > 59.0 && av.timing.fps < 60.0);
    REQUIRE(av.timing.sample_rate == 48000.0);

    REQUIRE(frontend.support_no_game_seen);
    REQUIRE(!frontend.support_no_game);
    REQUIRE(frontend.subsystem_calls == 1U);
    REQUIRE(frontend.content_override_calls == 1U);
    REQUIRE(frontend.controller_calls == 1U);
    REQUIRE(frontend.input_descriptor_calls == 1U);
    REQUIRE(frontend.option_registration_calls == 1U);

    REQUIRE(frontend.subsystems != NULL);
    subsystem = &frontend.subsystems[0];
    REQUIRE(subsystem->desc != NULL);
    REQUIRE(strcmp(subsystem->ident, "dualboylink") == 0);
    REQUIRE(subsystem->num_roms == 2U);
    REQUIRE(subsystem->id != 0U);
    REQUIRE(subsystem->roms != NULL);
    REQUIRE(frontend.subsystems[1].desc == NULL);
    for (index = 0U; index < 2U; ++index) {
        const struct retro_subsystem_rom_info *rom = &subsystem->roms[index];

        REQUIRE(rom->required);
        REQUIRE(!rom->need_fullpath && !rom->block_extract);
        REQUIRE(strcmp(rom->valid_extensions, "gb|gbc|gba") == 0);
        REQUIRE(rom->memory != NULL && rom->num_memory == 2U);
        REQUIRE(strcmp(rom->memory[0].extension, "srm") == 0);
        REQUIRE(strcmp(rom->memory[1].extension, "rtc") == 0);
    }
    REQUIRE(subsystem->roms[0].memory[0].type == TEST_SLOT1_SRAM_ID);
    REQUIRE(subsystem->roms[0].memory[1].type == TEST_SLOT1_RTC_ID);
    REQUIRE(subsystem->roms[1].memory[0].type == TEST_SLOT2_SRAM_ID);
    REQUIRE(subsystem->roms[1].memory[1].type == TEST_SLOT2_RTC_ID);

    REQUIRE(frontend.content_overrides != NULL);
    REQUIRE(strcmp(frontend.content_overrides[0].extensions, "m3u") == 0);
    REQUIRE(frontend.content_overrides[0].need_fullpath);
    REQUIRE(!frontend.content_overrides[0].persistent_data);
    REQUIRE(frontend.content_overrides[1].extensions == NULL);

    REQUIRE(frontend.controllers != NULL);
    REQUIRE(frontend.controllers[0].num_types == 1U);
    REQUIRE(frontend.controllers[1].num_types == 1U);
    REQUIRE(frontend.controllers[0].types[0].id == RETRO_DEVICE_JOYPAD);
    REQUIRE(frontend.controllers[1].types[0].id == RETRO_DEVICE_JOYPAD);
    REQUIRE(frontend.controllers[2].types == NULL);
    REQUIRE(frontend.input_descriptors != NULL);
    while (descriptor_count < 64U &&
           frontend.input_descriptors[descriptor_count].description != NULL) {
        ++descriptor_count;
    }
    REQUIRE(descriptor_count == 20U);

    REQUIRE(frontend.options_v2 != NULL);
    REQUIRE(frontend.options_v2->categories != NULL);
    REQUIRE(frontend.options_v2->definitions != NULL);
    while (category_count < 16U &&
           frontend.options_v2->categories[category_count].key != NULL) {
        ++category_count;
    }
    REQUIRE(category_count == 3U);
    for (index = 0U; index < 5U; ++index) {
        const struct retro_core_option_v2_definition *definition =
            &frontend.options_v2->definitions[index];

        REQUIRE(definition->key != NULL);
        REQUIRE(strcmp(definition->key, option_keys[index]) == 0);
        REQUIRE(definition->default_value != NULL);
        REQUIRE(strcmp(definition->default_value, option_defaults[index]) == 0);
        REQUIRE(definition->values[0].value != NULL);
    }
    REQUIRE(frontend.options_v2->definitions[5].key == NULL);
    REQUIRE(api->get_region() == RETRO_REGION_NTSC);
    return true;
}

static bool validate_loaded_av(const struct core_api *api,
                               unsigned width,
                               unsigned height)
{
    struct retro_system_av_info av;

    memset(&av, 0, sizeof(av));
    api->get_system_av_info(&av);
    REQUIRE(av.geometry.base_width == width);
    REQUIRE(av.geometry.base_height == height);
    REQUIRE(av.geometry.max_width == 480U);
    REQUIRE(av.geometry.max_height == 320U);
    REQUIRE(av.geometry.aspect_ratio > 0.0F);
    REQUIRE(av.timing.sample_rate == 48000.0);
    return true;
}

static bool build_test_path(char *output,
                            size_t capacity,
                            const char *filename)
{
    const int length = snprintf(output, capacity, "%s/%s", frontend.directory,
                                filename);

    return length > 0 && (size_t)length < capacity;
}

static bool write_test_file(const char *path, const void *data, size_t size)
{
    FILE *file;
    bool success;

    if (path == NULL || data == NULL || size == 0U) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    success = fwrite(data, 1U, size, file) == size;
    if (fclose(file) != 0) {
        success = false;
    }
    if (!success) {
        (void)unlink(path);
    }
    return success;
}

static bool file_starts_with(const char *path, uint8_t expected)
{
    FILE *file = fopen(path, "rb");
    int value;

    if (file == NULL) {
        return false;
    }
    value = fgetc(file);
    if (fclose(file) != 0) {
        return false;
    }
    return value != EOF && (uint8_t)value == expected;
}

static bool file_size_is(const char *path, size_t expected)
{
    struct stat status;

    return stat(path, &status) == 0 && status.st_size >= 0 &&
           (uintmax_t)status.st_size == (uintmax_t)expected;
}

static bool test_normal_load(struct core_api *api, const uint8_t *rom)
{
    const struct retro_game_info game = {
        "/virtual/normal.gb", rom, TEST_GB_ROM_SIZE, NULL,
    };
    void *standard_sram;
    void *standard_rtc;
    size_t state_size;
    size_t reload_state_size;
    uint8_t *state;
    uint8_t *sram;
    unsigned frame;
    unsigned geometry_calls;
    size_t audio_frames;
    bool marker_seen = false;

    set_default_options();
    reset_observations();
    REQUIRE(!api->load_game(NULL));
    REQUIRE(api->load_game(&game));
    REQUIRE(frontend.pixel_format_calls == 1U);
    REQUIRE(validate_loaded_av(api, 320U, 144U));

    standard_sram = api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    standard_rtc = api->get_memory_data(RETRO_MEMORY_RTC);
    REQUIRE(standard_sram != NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_SAVE_RAM) == 32768U);
    REQUIRE(standard_rtc != NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_RTC) > 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == standard_sram);
    REQUIRE(api->get_memory_data(TEST_SLOT1_RTC_ID) == standard_rtc);
    REQUIRE(api->get_memory_data(TEST_SLOT2_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT2_SRAM_ID) == 0U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SYSTEM_RAM) == NULL);
    sram = (uint8_t *)standard_sram;

    for (frame = 0U; frame < TEST_BOOT_FRAME_LIMIT; ++frame) {
        api->run();
        if (sram[0] == 0x41U) {
            marker_seen = true;
            break;
        }
    }
    REQUIRE(marker_seen);
    REQUIRE(frontend.video_calls > 0U);
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    REQUIRE(frontend.video_pitch == 320U * sizeof(uint32_t));
    REQUIRE(frontend.audio_frames > 0U);
    REQUIRE(frontend.audio_batch_calls > 0U);
    REQUIRE(frontend.input_poll_calls > 0U);
    REQUIRE(frontend.input_state_calls[0] > 0U);
    REQUIRE(frontend.input_state_calls[1] > 0U);
    REQUIRE(frontend.video_contract_ok && frontend.audio_contract_ok &&
            frontend.input_contract_ok);

    state_size = api->serialize_size();
    REQUIRE(state_size > 1U);
    state = (uint8_t *)malloc(state_size);
    REQUIRE(state != NULL);
    REQUIRE(api->serialize(state, state_size));
    REQUIRE(api->serialize_size() == state_size);
    REQUIRE(!api->serialize(state, state_size - 1U));

    sram[7] = 0x4AU;
    REQUIRE(api->serialize(state, state_size));
    sram[7] = 0x93U;
    REQUIRE(api->unserialize(state, state_size));
    REQUIRE(sram[7] == 0x93U);
    state[0] ^= 0xFFU;
    REQUIRE(!api->unserialize(state, state_size));
    REQUIRE(sram[7] == 0x93U);
    state[0] ^= 0xFFU;
    REQUIRE(api->unserialize(state, state_size));
    REQUIRE(api->serialize_size() == state_size);

    frontend.layout = "top_bottom";
    frontend.option_updated = true;
    geometry_calls = frontend.geometry_calls;
    api->run();
    REQUIRE(frontend.video_width == 160U && frontend.video_height == 288U);
    REQUIRE(frontend.geometry_calls > geometry_calls);
    REQUIRE(frontend.geometry.base_width == 160U);
    REQUIRE(frontend.geometry.base_height == 288U);
    REQUIRE(validate_loaded_av(api, 160U, 288U));

    frontend.mode = "player2";
    frontend.option_updated = true;
    api->run();
    REQUIRE(frontend.video_width == 160U && frontend.video_height == 144U);
    REQUIRE(validate_loaded_av(api, 160U, 144U));

    frontend.audio_source = "disabled";
    frontend.option_updated = true;
    audio_frames = frontend.audio_frames;
    api->run();
    REQUIRE(frontend.audio_frames == audio_frames);

    api->cheat_reset();
    api->cheat_set(0U, false, NULL);
    api->reset();
    REQUIRE(api->serialize_size() == state_size);
    free(state);

    api->unload_game();
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0U);
    REQUIRE(api->serialize_size() == 0U);

    set_default_options();
    REQUIRE(api->load_game(&game));
    reload_state_size = api->serialize_size();
    REQUIRE(reload_state_size == state_size);
    frame = frontend.video_calls;
    api->run();
    REQUIRE(frontend.video_calls > frame);
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    api->unload_game();
    return true;
}

static bool test_pathless_normal_load(struct core_api *api,
                                      const uint8_t *rom)
{
    const struct retro_game_info game = {
        NULL, rom, TEST_GB_ROM_SIZE, NULL,
    };
    const uint64_t identity = content_identity(rom, TEST_GB_ROM_SIZE);
    char first_name[96];
    char second_name[96];
    char first_save[TEST_PATH_CAPACITY];
    char second_save[TEST_PATH_CAPACITY];
    size_t state_size;
    unsigned frame;
    int length;

    length = snprintf(first_name, sizeof(first_name),
                      "dualboy-%016llx.srm", (unsigned long long)identity);
    REQUIRE(length > 0 && (size_t)length < sizeof(first_name));
    length = snprintf(second_name, sizeof(second_name),
                      "dualboy-%016llx.srm.2",
                      (unsigned long long)identity);
    REQUIRE(length > 0 && (size_t)length < sizeof(second_name));
    REQUIRE(build_test_path(first_save, sizeof(first_save), first_name));
    REQUIRE(build_test_path(second_save, sizeof(second_save), second_name));

    set_default_options();
    reset_observations();
    REQUIRE(api->load_game(&game));
    REQUIRE(validate_loaded_av(api, 320U, 144U));
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_RTC) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_RTC) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT1_SRAM_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_RTC_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT1_RTC_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT2_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT2_SRAM_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT2_RTC_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT2_RTC_ID) == 0U);
    state_size = api->serialize_size();
    REQUIRE(state_size > 0U);

    for (frame = 0U; frame < TEST_BOOT_FRAME_LIMIT; ++frame) {
        api->run();
    }
    REQUIRE(frontend.video_calls == TEST_BOOT_FRAME_LIMIT);
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    api->unload_game();
    REQUIRE(file_starts_with(first_save, 0x41U));
    REQUIRE(file_starts_with(second_save, 0x41U));
    REQUIRE(file_size_is(first_save, 32768U));
    REQUIRE(file_size_is(second_save, 32768U));

    /* The same bytes must deterministically select the same files and import
     * them into a completely new pair without ever exposing raw pointers. */
    REQUIRE(api->load_game(&game));
    REQUIRE(api->serialize_size() == state_size);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    api->run();
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    api->unload_game();
    REQUIRE(file_starts_with(first_save, 0x41U));
    REQUIRE(file_starts_with(second_save, 0x41U));
    return true;
}

static bool test_cgb_load(struct core_api *api, const uint8_t *rom)
{
    const struct retro_game_info game = {
        "/virtual/color.gbc", rom, TEST_GB_ROM_SIZE, NULL,
    };

    unsigned video_calls;

    set_default_options();
    REQUIRE(api->load_game(&game));
    REQUIRE(validate_loaded_av(api, 320U, 144U));
    REQUIRE(api->serialize_size() > 0U);
    video_calls = frontend.video_calls;
    api->run();
    REQUIRE(frontend.video_calls > video_calls);
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    api->unload_game();
    return true;
}

static bool test_gba_load(struct core_api *api, const uint8_t *rom)
{
    const struct retro_game_info linked_game = {
        "/virtual/advance-default-link.gba", rom, TEST_GBA_ROM_SIZE, NULL,
    };
    const struct retro_game_info game = {
        "/virtual/advance.gba", rom, TEST_GBA_ROM_SIZE, NULL,
    };
    char first_save[TEST_PATH_CAPACITY];
    char second_save[TEST_PATH_CAPACITY];
    size_t state_size;
    uint8_t *state;
    unsigned frame;

    /* Normal GBA content must be viable with the shipped link-enabled default,
     * even when the cartridge never touches SIO. */
    set_default_options();
    reset_observations();
    REQUIRE(api->load_game(&linked_game));
    REQUIRE(validate_loaded_av(api, 480U, 160U));
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    api->run();
    REQUIRE(frontend.video_calls == 1U);
    REQUIRE(frontend.video_width == 480U && frontend.video_height == 160U);
    api->unload_game();

    set_default_options();
    frontend.link = "disabled";
    reset_observations();
    REQUIRE(api->load_game(&game));
    REQUIRE(validate_loaded_av(api, 480U, 160U));

    /* mGBA save and RTC buffers are intentionally core-managed: exposing
     * either through Libretro would allow the frontend to overwrite the
     * adapter's fixed shadow storage with a device-dependent save size. */
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_RTC) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_RTC) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_data(TEST_SLOT2_SRAM_ID) == NULL);

    state_size = api->serialize_size();
    REQUIRE(state_size > 1U);
    state = (uint8_t *)malloc(state_size);
    REQUIRE(state != NULL);
    for (frame = 0U; frame < TEST_GBA_FRAME_COUNT; ++frame) {
        api->run();
    }
    REQUIRE(frontend.video_calls == TEST_GBA_FRAME_COUNT);
    REQUIRE(frontend.video_width == 480U && frontend.video_height == 160U);
    REQUIRE(frontend.video_pitch == 480U * sizeof(uint32_t));
    REQUIRE(frontend.audio_batch_calls > 0U);
    REQUIRE(frontend.audio_frames > 0U);
    REQUIRE(frontend.video_contract_ok && frontend.audio_contract_ok &&
            frontend.input_contract_ok);
    REQUIRE(api->serialize(state, state_size));
    REQUIRE(api->serialize_size() == state_size);
    REQUIRE(api->unserialize(state, state_size));
    REQUIRE(api->serialize_size() == state_size);
    free(state);
    api->unload_game();

    REQUIRE(build_test_path(first_save, sizeof(first_save), "advance.srm"));
    REQUIRE(build_test_path(second_save, sizeof(second_save),
                            "advance.srm.2"));
    REQUIRE(file_starts_with(first_save, 0x74U));
    REQUIRE(file_starts_with(second_save, 0x74U));
    REQUIRE(file_size_is(first_save, 0x8000U));
    REQUIRE(file_size_is(second_save, 0x8000U));

    /* A second complete load proves the persisted fixed-size shadow saves can
     * be imported into newly created mGBA instances through the public ABI. */
    REQUIRE(api->load_game(&game));
    REQUIRE(api->serialize_size() == state_size);
    api->run();
    REQUIRE(frontend.video_width == 480U && frontend.video_height == 160U);
    api->unload_game();
    return true;
}

static bool test_m3u_load(struct core_api *api,
                          const uint8_t *first_rom,
                          const uint8_t *second_rom)
{
    char first_path[TEST_PATH_CAPACITY];
    char second_path[TEST_PATH_CAPACITY];
    char playlist_path[TEST_PATH_CAPACITY];
    char playlist[TEST_PATH_CAPACITY * 2U + 4U];
    struct retro_game_info game;
    size_t state_size;
    uint8_t *state;
    int playlist_length;
    unsigned video_calls;

    REQUIRE(build_test_path(first_path, sizeof(first_path),
                            "playlist-one.gb"));
    REQUIRE(build_test_path(second_path, sizeof(second_path),
                            "playlist-two.gb"));
    REQUIRE(build_test_path(playlist_path, sizeof(playlist_path),
                            "pair.m3u"));
    REQUIRE(write_test_file(first_path, first_rom, TEST_GB_ROM_SIZE));
    REQUIRE(write_test_file(second_path, second_rom, TEST_GB_ROM_SIZE));
    playlist_length = snprintf(playlist, sizeof(playlist), "%s\n%s\n",
                               first_path, second_path);
    REQUIRE(playlist_length > 0 &&
            (size_t)playlist_length < sizeof(playlist));
    REQUIRE(write_test_file(playlist_path, playlist,
                            (size_t)playlist_length));

    memset(&game, 0, sizeof(game));
    game.path = playlist_path;
    set_default_options();
    frontend.link = "disabled";
    reset_observations();
    REQUIRE(api->load_game(&game));
    REQUIRE(validate_loaded_av(api, 320U, 144U));
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_data(TEST_SLOT2_SRAM_ID) == NULL);

    state_size = api->serialize_size();
    REQUIRE(state_size > 0U);
    state = (uint8_t *)malloc(state_size);
    REQUIRE(state != NULL);
    video_calls = frontend.video_calls;
    api->run();
    REQUIRE(frontend.video_calls > video_calls);
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    REQUIRE(api->serialize(state, state_size));
    REQUIRE(api->unserialize(state, state_size));
    free(state);
    api->unload_game();
    return true;
}

static bool run_until_subsystem_program(struct core_api *api,
                                        const uint8_t *first_sram,
                                        const uint8_t *second_sram,
                                        uint8_t first_marker,
                                        uint8_t second_marker,
                                        uint8_t first_input,
                                        uint8_t second_input)
{
    unsigned frame;

    for (frame = 0U; frame < TEST_BOOT_FRAME_LIMIT; ++frame) {
        api->run();
        if (first_sram[0] == first_marker &&
            second_sram[0] == second_marker &&
            (first_sram[1] & 0x0FU) == first_input &&
            (second_sram[1] & 0x0FU) == second_input) {
            return true;
        }
    }
    return false;
}

static bool test_gba_two_rom_subsystem(struct core_api *api,
                                       const uint8_t *first_rom,
                                       const uint8_t *second_rom)
{
    const unsigned subsystem_id = frontend.subsystems[0].id;
    const struct retro_game_info games[2] = {
        {"/virtual/gba-player-one.gba", first_rom, TEST_GBA_ROM_SIZE, NULL},
        {"/virtual/gba-player-two.gba", second_rom, TEST_GBA_ROM_SIZE, NULL},
    };
    char first_save[TEST_PATH_CAPACITY];
    char second_save[TEST_PATH_CAPACITY];
    size_t state_size;
    uint8_t *state;

    REQUIRE(memcmp(first_rom, second_rom, TEST_GBA_ROM_SIZE) != 0);
    set_default_options();
    reset_observations();
    REQUIRE(api->load_game_special(subsystem_id, games, 2U));
    REQUIRE(validate_loaded_av(api, 480U, 160U));

    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_RTC) == NULL);
    REQUIRE(api->get_memory_size(RETRO_MEMORY_RTC) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT1_SRAM_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_RTC_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT1_RTC_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT2_SRAM_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT2_SRAM_ID) == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT2_RTC_ID) == NULL);
    REQUIRE(api->get_memory_size(TEST_SLOT2_RTC_ID) == 0U);

    api->run();
    REQUIRE(frontend.video_calls == 1U);
    REQUIRE(frontend.video_width == 480U && frontend.video_height == 160U);
    REQUIRE(frontend.video_pitch == 480U * sizeof(uint32_t));
    REQUIRE(frontend.video_contract_ok && frontend.audio_contract_ok &&
            frontend.input_contract_ok);
    state_size = api->serialize_size();
    REQUIRE(state_size > 1U);
    state = (uint8_t *)malloc(state_size);
    REQUIRE(state != NULL);
    REQUIRE(api->serialize(state, state_size));
    REQUIRE(api->unserialize(state, state_size));
    REQUIRE(api->serialize_size() == state_size);
    free(state);
    api->unload_game();

    REQUIRE(build_test_path(first_save, sizeof(first_save),
                            "gba-player-one.srm"));
    REQUIRE(build_test_path(second_save, sizeof(second_save),
                            "gba-player-two.srm"));
    REQUIRE(file_starts_with(first_save, 0x74U));
    REQUIRE(file_starts_with(second_save, 0x74U));
    REQUIRE(file_size_is(first_save, 0x8000U));
    REQUIRE(file_size_is(second_save, 0x8000U));
    return true;
}

static bool test_two_rom_subsystem(struct core_api *api,
                                   const uint8_t *first_rom,
                                   const uint8_t *second_rom,
                                   const uint8_t *gba_header)
{
    const unsigned subsystem_id = frontend.subsystems[0].id;
    struct retro_game_info games[2] = {
        {"/virtual/player-one.gb", first_rom, TEST_GB_ROM_SIZE, NULL},
        {"/virtual/player-two.gbc", second_rom, TEST_GB_ROM_SIZE, NULL},
    };
    struct retro_game_info mixed[2] = {
        {"/virtual/player-one.gb", first_rom, TEST_GB_ROM_SIZE, NULL},
        {"/virtual/player-two.gba", gba_header, TEST_GBA_ROM_SIZE, NULL},
    };
    uint8_t *first_sram;
    uint8_t *second_sram;
    void *first_rtc;
    void *second_rtc;
    uint64_t first_hash;
    uint64_t second_hash;
    unsigned geometry_calls;
    size_t state_size;
    uint8_t *state;

    set_default_options();
    reset_observations();
    REQUIRE(!api->load_game_special(subsystem_id, games, 1U));
    REQUIRE(!api->load_game_special(subsystem_id + 1U, games, 2U));
    REQUIRE(api->load_game_special(subsystem_id, games, 2U));
    REQUIRE(validate_loaded_av(api, 320U, 144U));

    first_sram = (uint8_t *)api->get_memory_data(TEST_SLOT1_SRAM_ID);
    second_sram = (uint8_t *)api->get_memory_data(TEST_SLOT2_SRAM_ID);
    first_rtc = api->get_memory_data(TEST_SLOT1_RTC_ID);
    second_rtc = api->get_memory_data(TEST_SLOT2_RTC_ID);
    REQUIRE(first_sram != NULL && second_sram != NULL);
    REQUIRE(first_sram != second_sram);
    REQUIRE(api->get_memory_size(TEST_SLOT1_SRAM_ID) == 32768U);
    REQUIRE(api->get_memory_size(TEST_SLOT2_SRAM_ID) == 32768U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == first_sram);
    REQUIRE(first_rtc != NULL && second_rtc != NULL && first_rtc != second_rtc);
    REQUIRE(api->get_memory_size(TEST_SLOT1_RTC_ID) > 0U);
    REQUIRE(api->get_memory_size(TEST_SLOT2_RTC_ID) > 0U);
    REQUIRE(api->get_memory_data(RETRO_MEMORY_RTC) == first_rtc);

    frontend.input_masks[0] =
        (uint16_t)(1U << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    frontend.input_masks[1] =
        (uint16_t)(1U << RETRO_DEVICE_ID_JOYPAD_LEFT);
    REQUIRE(run_until_subsystem_program(api, first_sram, second_sram,
                                        0x51U, 0x62U, 0x0EU, 0x0DU));
    api->run();
    api->run();
    REQUIRE(frontend.video_width == 320U && frontend.video_height == 144U);
    REQUIRE(frontend.first_screen_hash != frontend.second_screen_hash);
    first_hash = frontend.first_screen_hash;
    second_hash = frontend.second_screen_hash;

    frontend.swap = "enabled";
    frontend.option_updated = true;
    api->run();
    REQUIRE((first_sram[1] & 0x0FU) == 0x0DU);
    REQUIRE((second_sram[1] & 0x0FU) == 0x0EU);
    REQUIRE(frontend.first_screen_hash == second_hash);
    REQUIRE(frontend.second_screen_hash == first_hash);

    frontend.layout = "top_bottom";
    frontend.option_updated = true;
    geometry_calls = frontend.geometry_calls;
    api->run();
    REQUIRE(frontend.video_width == 160U && frontend.video_height == 288U);
    REQUIRE(frontend.geometry_calls > geometry_calls);

    state_size = api->serialize_size();
    REQUIRE(state_size > 0U);
    state = (uint8_t *)malloc(state_size);
    REQUIRE(state != NULL);
    REQUIRE(api->serialize(state, state_size));
    REQUIRE(api->unserialize(state, state_size));
    free(state);
    api->unload_game();

    set_default_options();
    REQUIRE(!api->load_game_special(subsystem_id, mixed, 2U));
    REQUIRE(api->serialize_size() == 0U);
    REQUIRE(api->get_memory_data(TEST_SLOT1_SRAM_ID) == NULL);
    REQUIRE(frontend.video_contract_ok && frontend.audio_contract_ok &&
            frontend.input_contract_ok);
    frontend.input_masks[0] = 0U;
    frontend.input_masks[1] = 0U;
    return true;
}

static void cleanup_temporary_directory(const char *directory)
{
    DIR *stream;
    struct dirent *entry;
    char path[TEST_PATH_CAPACITY];

    stream = opendir(directory);
    if (stream == NULL) {
        return;
    }
    while ((entry = readdir(stream)) != NULL) {
        int length;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        length = snprintf(path, sizeof(path), "%s/%s", directory,
                          entry->d_name);
        if (length > 0 && (size_t)length < sizeof(path)) {
            (void)unlink(path);
        }
    }
    (void)closedir(stream);
    (void)rmdir(directory);
}

int main(int argc, char **argv)
{
    struct core_api api;
    uint8_t *normal_rom = NULL;
    uint8_t *color_rom = NULL;
    uint8_t *first_rom = NULL;
    uint8_t *second_rom = NULL;
    uint8_t gba_rom[TEST_GBA_ROM_SIZE];
    uint8_t gba_second_rom[TEST_GBA_ROM_SIZE];
    char directory_template[] = "/tmp/dualboy-libretro-smoke-XXXXXX";
    char *directory;
    int temporary_fd;
    void *handle;
    bool initialized = false;
    bool success = false;

    if (argc != 2) {
        fprintf(stderr, "usage: %s CORE\n", argv[0]);
        return EXIT_FAILURE;
    }
    temporary_fd = mkstemp(directory_template);
    if (temporary_fd < 0) {
        perror("temporary directory creation");
        return EXIT_FAILURE;
    }
    if (close(temporary_fd) != 0 || unlink(directory_template) != 0 ||
        mkdir(directory_template, 0700) != 0) {
        perror("temporary directory creation");
        (void)unlink(directory_template);
        return EXIT_FAILURE;
    }
    directory = directory_template;
    if (strlen(directory) >= sizeof(frontend.directory)) {
        fprintf(stderr, "temporary directory path is too long\n");
        cleanup_temporary_directory(directory);
        return EXIT_FAILURE;
    }
    memset(&frontend, 0, sizeof(frontend));
    memcpy(frontend.directory, directory, strlen(directory) + 1U);
    set_default_options();
    reset_observations();

    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "unable to load %s: %s\n", argv[1], dlerror());
        cleanup_temporary_directory(directory);
        return EXIT_FAILURE;
    }
    load_core_api(handle, &api);
    api.set_environment(environment_callback);
    api.set_video_refresh(video_callback);
    api.set_audio_sample(audio_sample_callback);
    api.set_audio_sample_batch(audio_batch_callback);
    api.set_input_poll(input_poll_callback);
    api.set_input_state(input_state_callback);
    api.init();
    initialized = true;

    if (!validate_registration(&api)) {
        goto cleanup;
    }
    normal_rom = (uint8_t *)malloc(TEST_GB_ROM_SIZE);
    color_rom = (uint8_t *)malloc(TEST_GB_ROM_SIZE);
    first_rom = (uint8_t *)malloc(TEST_GB_ROM_SIZE);
    second_rom = (uint8_t *)malloc(TEST_GB_ROM_SIZE);
    if (normal_rom == NULL || color_rom == NULL || first_rom == NULL ||
        second_rom == NULL) {
        fprintf(stderr, "unable to allocate synthetic test ROMs\n");
        goto cleanup;
    }
    if (!make_test_gb_rom(normal_rom, false, 0x41U, 0xE4U) ||
        !make_test_gb_rom(color_rom, true, 0x43U, 0xE4U) ||
        !make_test_gb_rom(first_rom, false, 0x51U, 0xE4U) ||
        !make_test_gb_rom(second_rom, true, 0x62U, 0xE7U)) {
        fprintf(stderr, "unable to generate synthetic test ROMs\n");
        goto cleanup;
    }
    if (!make_test_gba_rom(gba_rom)) {
        fprintf(stderr, "unable to generate synthetic GBA test ROM\n");
        goto cleanup;
    }
    memcpy(gba_second_rom, gba_rom, sizeof(gba_second_rom));
    gba_second_rom[0x200U] ^= 0xA5U;

    api.set_controller_port_device(0U, RETRO_DEVICE_JOYPAD);
    api.set_controller_port_device(1U, RETRO_DEVICE_JOYPAD);
    api.set_controller_port_device(2U, RETRO_DEVICE_ANALOG);
    if (!test_normal_load(&api, normal_rom) ||
        !test_pathless_normal_load(&api, normal_rom) ||
        !test_cgb_load(&api, color_rom) ||
        !test_two_rom_subsystem(&api, first_rom, second_rom, gba_rom) ||
        !test_m3u_load(&api, normal_rom, first_rom) ||
        !test_gba_two_rom_subsystem(&api, gba_rom, gba_second_rom) ||
        !test_gba_load(&api, gba_rom)) {
        goto cleanup;
    }
    success = true;

cleanup:
    free(second_rom);
    free(first_rom);
    free(color_rom);
    free(normal_rom);
    if (initialized) {
        api.deinit();
    }
    if (dlclose(handle) != 0) {
        fprintf(stderr, "unable to unload core: %s\n", dlerror());
        success = false;
    }
    cleanup_temporary_directory(directory);
    if (!success) {
        return EXIT_FAILURE;
    }
    puts("DualBoy Libretro runtime integration test passed");
    return EXIT_SUCCESS;
}
