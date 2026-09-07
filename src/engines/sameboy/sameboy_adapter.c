/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * SameBoy's public API intentionally makes GB_gameboy_t opaque. This adapter
 * owns two pointer-allocated instances and uses public entry points everywhere
 * except for the stable live RTC memory region required by Libretro. The RTC
 * access below matches SameBoy's own pinned libretro implementation.
 */
#ifndef GB_INTERNAL
#define GB_INTERNAL
#endif
#ifndef GB_DISABLE_TIMEKEEPING
#define GB_DISABLE_TIMEKEEPING
#endif
#ifndef GB_DISABLE_REWIND
#define GB_DISABLE_REWIND
#endif
#ifndef GB_DISABLE_DEBUGGER
#define GB_DISABLE_DEBUGGER
#endif
#ifndef GB_DISABLE_CHEATS
#define GB_DISABLE_CHEATS
#endif

#include "../../frontend/engine.h"
#include "embedded_boot_roms.h"
#include "sameboy_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMEBOY_VIDEO_WIDTH 160U
#define SAMEBOY_VIDEO_HEIGHT 144U
#define SAMEBOY_VIDEO_PIXELS \
    ((size_t)SAMEBOY_VIDEO_WIDTH * (size_t)SAMEBOY_VIDEO_HEIGHT)
#define SAMEBOY_AUDIO_RATE 48000U
#define SAMEBOY_AUDIO_FIFO_FRAMES 16384U
#define SAMEBOY_FRAME_TICK_WATCHDOG 16777216ULL
#define SAMEBOY_FRAME_CALL_WATCHDOG 2000000ULL
#define SAMEBOY_LINK_STATE_SIZE 32U
#define SAMEBOY_LINK_STATE_VERSION 1U
#define SAMEBOY_LINK_FLAG_ENABLED 0x01U
#define SAMEBOY_MAX_SERIALIZED_SKEW 1048576ULL

struct sameboy_pair;

struct sameboy_machine {
    struct sameboy_pair *pair;
    GB_gameboy_t *core;
    enum dualboy_platform platform;
    unsigned index;
    bool loaded;
    bool vblank_seen;
    uint32_t pixels[SAMEBOY_VIDEO_PIXELS];
    uint32_t retained_pixels[SAMEBOY_VIDEO_PIXELS];
};

struct sameboy_audio_fifo {
    int16_t samples[(size_t)SAMEBOY_AUDIO_FIFO_FRAMES * (size_t)2U];
    size_t read_index;
    size_t frame_count;
};

struct sameboy_pair {
    struct sameboy_machine machine[DUALBOY_MACHINE_COUNT];
    struct sameboy_audio_fifo audio;
    dualboy_log_fn log;
    void *log_context;
    int64_t tick_skew;
    bool serial_output[DUALBOY_MACHINE_COUNT];
    bool infrared_output[DUALBOY_MACHINE_COUNT];
    bool link_enabled;
    uint8_t dmg_boot[DUALBOY_SAMEBOY_DMG_BOOT_SIZE];
    uint8_t cgb_boot[DUALBOY_SAMEBOY_CGB_BOOT_SIZE];
};

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

static void clear_error(char *error, size_t error_size)
{
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
}

static void log_message(struct sameboy_pair *pair,
                        enum dualboy_log_level level,
                        const char *message)
{
    if (pair != NULL && pair->log != NULL && message != NULL) {
        pair->log(pair->log_context, level, message);
    }
}

static struct sameboy_machine *machine_from_core(GB_gameboy_t *core)
{
    if (core == NULL) {
        return NULL;
    }
    return (struct sameboy_machine *)GB_get_user_data(core);
}

static bool pair_is_ready(const struct sameboy_pair *pair)
{
    return pair != NULL && pair->machine[0].loaded &&
           pair->machine[1].loaded;
}

static void audio_fifo_clear(struct sameboy_audio_fifo *fifo)
{
    if (fifo != NULL) {
        fifo->read_index = 0U;
        fifo->frame_count = 0U;
    }
}

static void audio_fifo_push(struct sameboy_audio_fifo *fifo,
                            int16_t left,
                            int16_t right)
{
    size_t write_index;

    if (fifo->frame_count == (size_t)SAMEBOY_AUDIO_FIFO_FRAMES) {
        fifo->read_index =
            (fifo->read_index + (size_t)1U) %
            (size_t)SAMEBOY_AUDIO_FIFO_FRAMES;
        --fifo->frame_count;
    }
    write_index =
        (fifo->read_index + fifo->frame_count) %
        (size_t)SAMEBOY_AUDIO_FIFO_FRAMES;
    fifo->samples[write_index * (size_t)2U] = left;
    fifo->samples[write_index * (size_t)2U + (size_t)1U] = right;
    ++fifo->frame_count;
}

static void sameboy_audio_sample(GB_gameboy_t *core, GB_sample_t *sample)
{
    struct sameboy_machine *machine = machine_from_core(core);

    if (machine == NULL || machine->pair == NULL || machine->index != 0U ||
        sample == NULL) {
        return;
    }
    audio_fifo_push(&machine->pair->audio, sample->left, sample->right);
}

static uint32_t sameboy_rgb_encode(GB_gameboy_t *core,
                                   uint8_t red,
                                   uint8_t green,
                                   uint8_t blue)
{
    (void)core;
    return ((uint32_t)red << 16U) | ((uint32_t)green << 8U) |
           (uint32_t)blue;
}

static void sameboy_vblank(GB_gameboy_t *core, GB_vblank_type_t type)
{
    struct sameboy_machine *machine = machine_from_core(core);

    if (machine == NULL) {
        return;
    }
    if (type == GB_VBLANK_TYPE_REPEAT) {
        memcpy(machine->pixels,
               machine->retained_pixels,
               sizeof(machine->pixels));
    }
    else if (type != GB_VBLANK_TYPE_LCD_OFF) {
        memcpy(machine->retained_pixels,
               machine->pixels,
               sizeof(machine->retained_pixels));
    }
    machine->vblank_seen = true;
}

static void sameboy_lcd_status(GB_gameboy_t *core, bool on)
{
    struct sameboy_machine *machine = machine_from_core(core);

    if (machine != NULL && !on) {
        memcpy(machine->retained_pixels,
               machine->pixels,
               sizeof(machine->retained_pixels));
    }
}

static void sameboy_log(GB_gameboy_t *core,
                        const char *message,
                        GB_log_attributes_t attributes)
{
    struct sameboy_machine *machine = machine_from_core(core);

    (void)attributes;
    if (machine != NULL) {
        log_message(machine->pair, DUALBOY_LOG_INFO, message);
    }
}

static void sameboy_boot_rom_load(GB_gameboy_t *core, GB_boot_rom_t type)
{
    struct sameboy_machine *machine = machine_from_core(core);
    struct sameboy_pair *pair;

    if (machine == NULL || machine->pair == NULL) {
        return;
    }
    pair = machine->pair;
    switch (type) {
        case GB_BOOT_ROM_CGB_0:
        case GB_BOOT_ROM_CGB:
        case GB_BOOT_ROM_CGB_E:
        case GB_BOOT_ROM_AGB_0:
        case GB_BOOT_ROM_AGB:
            GB_load_boot_rom_from_buffer(core,
                                         pair->cgb_boot,
                                         sizeof(pair->cgb_boot));
            break;
        case GB_BOOT_ROM_DMG_0:
        case GB_BOOT_ROM_DMG:
        case GB_BOOT_ROM_MGB:
        case GB_BOOT_ROM_SGB:
        case GB_BOOT_ROM_SGB2:
        default:
            GB_load_boot_rom_from_buffer(core,
                                         pair->dmg_boot,
                                         sizeof(pair->dmg_boot));
            break;
    }
}

static struct sameboy_machine *peer_machine(struct sameboy_machine *machine)
{
    if (machine == NULL || machine->pair == NULL || machine->index > 1U) {
        return NULL;
    }
    return &machine->pair->machine[machine->index ^ 1U];
}

static void sameboy_serial_start(GB_gameboy_t *core, bool output)
{
    struct sameboy_machine *machine = machine_from_core(core);

    if (machine != NULL && machine->pair != NULL && machine->index < 2U) {
        machine->pair->serial_output[machine->index] = output;
    }
}

static bool sameboy_serial_end(GB_gameboy_t *core)
{
    struct sameboy_machine *machine = machine_from_core(core);
    struct sameboy_machine *peer = peer_machine(machine);
    bool peer_output;

    if (machine == NULL || peer == NULL || !machine->pair->link_enabled ||
        !peer->loaded || peer->core == NULL) {
        return true;
    }
    peer_output = GB_serial_get_data_bit(peer->core);
    GB_serial_set_data_bit(peer->core,
                           machine->pair->serial_output[machine->index]);
    return peer_output;
}

static void sameboy_infrared_output(GB_gameboy_t *core, bool output)
{
    struct sameboy_machine *machine = machine_from_core(core);
    struct sameboy_machine *peer = peer_machine(machine);

    if (machine == NULL || peer == NULL || machine->pair == NULL) {
        return;
    }
    machine->pair->infrared_output[machine->index] = output;
    if (machine->pair->link_enabled && peer->loaded && peer->core != NULL) {
        GB_set_infrared_input(peer->core, output);
    }
}

static void apply_link_callbacks(struct sameboy_pair *pair)
{
    unsigned index;

    if (pair == NULL) {
        return;
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        struct sameboy_machine *machine = &pair->machine[index];

        if (!machine->loaded || machine->core == NULL) {
            continue;
        }
        if (pair->link_enabled && pair_is_ready(pair)) {
            unsigned peer = index ^ 1U;

            GB_set_serial_transfer_bit_start_callback(machine->core,
                                                       sameboy_serial_start);
            GB_set_serial_transfer_bit_end_callback(machine->core,
                                                     sameboy_serial_end);
            GB_set_infrared_callback(machine->core,
                                     sameboy_infrared_output);
            GB_set_infrared_input(machine->core,
                                  pair->infrared_output[peer]);
        }
        else {
            GB_set_serial_transfer_bit_start_callback(machine->core, NULL);
            GB_set_serial_transfer_bit_end_callback(machine->core, NULL);
            GB_set_infrared_callback(machine->core, NULL);
            GB_set_infrared_input(machine->core, false);
        }
    }
}

static void configure_machine_callbacks(struct sameboy_machine *machine)
{
    GB_gameboy_t *core = machine->core;

    GB_set_user_data(core, machine);
    GB_set_log_callback(core, sameboy_log);
    GB_set_boot_rom_load_callback(core, sameboy_boot_rom_load);
    GB_set_border_mode(core, GB_BORDER_NEVER);
    GB_set_pixels_output(core, machine->pixels);
    GB_set_rgb_encode_callback(core, sameboy_rgb_encode);
    GB_set_vblank_callback(core, sameboy_vblank);
    GB_set_lcd_status_callback(core, sameboy_lcd_status);
    GB_set_rtc_mode(core, GB_RTC_MODE_ACCURATE);
    if (machine->index == 0U) {
        GB_apu_set_sample_callback(core, sameboy_audio_sample);
        GB_set_sample_rate(core, SAMEBOY_AUDIO_RATE);
    }
    else {
        GB_set_sample_rate(core, 0U);
        GB_apu_set_sample_callback(core, NULL);
    }
}

static void destroy_machine(struct sameboy_machine *machine)
{
    if (machine == NULL || machine->core == NULL) {
        if (machine != NULL) {
            machine->loaded = false;
            machine->vblank_seen = false;
        }
        return;
    }
    if (GB_is_inited(machine->core)) {
        GB_set_serial_transfer_bit_start_callback(machine->core, NULL);
        GB_set_serial_transfer_bit_end_callback(machine->core, NULL);
        GB_set_infrared_callback(machine->core, NULL);
        GB_set_sample_rate(machine->core, 0U);
        GB_apu_set_sample_callback(machine->core, NULL);
        GB_set_vblank_callback(machine->core, NULL);
        GB_set_lcd_status_callback(machine->core, NULL);
        GB_set_log_callback(machine->core, NULL);
        GB_set_boot_rom_load_callback(machine->core, NULL);
        GB_set_user_data(machine->core, NULL);
        GB_disconnect_serial(machine->core);
    }
    GB_dealloc(machine->core);
    machine->core = NULL;
    machine->loaded = false;
    machine->vblank_seen = false;
    machine->platform = DUALBOY_PLATFORM_INVALID;
}

static bool sameboy_create_pair(void **output_pair,
                                const struct dualboy_engine_config *config,
                                char *error,
                                size_t error_size)
{
    struct sameboy_pair *pair;
    unsigned index;

    clear_error(error, error_size);
    if (output_pair == NULL) {
        set_error(error, error_size, "SameBoy pair output is null");
        return false;
    }
    *output_pair = NULL;
    pair = (struct sameboy_pair *)calloc(1U, sizeof(*pair));
    if (pair == NULL) {
        set_error(error, error_size, "Could not allocate SameBoy pair");
        return false;
    }
    if (config != NULL) {
        pair->log = config->log;
        pair->log_context = config->log_context;
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        pair->machine[index].pair = pair;
        pair->machine[index].index = index;
        pair->serial_output[index] = true;
    }
    if (!dualboy_sameboy_decode_boot_roms(pair->dmg_boot, pair->cgb_boot)) {
        set_error(error, error_size, "Embedded SameBoy boot ROM data is invalid");
        free(pair);
        return false;
    }
    if (config != NULL && config->audio_sample_rate != 0U &&
        config->audio_sample_rate != SAMEBOY_AUDIO_RATE) {
        log_message(pair,
                    DUALBOY_LOG_WARN,
                    "SameBoy adapter uses its fixed 48000 Hz output rate");
    }
    *output_pair = pair;
    return true;
}

static bool sameboy_load_rom(void *opaque_pair,
                             unsigned machine_index,
                             const struct dualboy_rom *rom,
                             char *error,
                             size_t error_size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_machine *machine;
    GB_model_t model;

    clear_error(error, error_size);
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        set_error(error, error_size, "Invalid SameBoy pair or machine index");
        return false;
    }
    if (rom == NULL || rom->data == NULL || rom->size < 0x150U ||
        rom->size > (size_t)(32U * 1024U * 1024U)) {
        set_error(error, error_size, "Invalid GB/GBC ROM buffer");
        return false;
    }
    if (rom->platform == DUALBOY_PLATFORM_GB) {
        model = GB_MODEL_DMG_B;
    }
    else if (rom->platform == DUALBOY_PLATFORM_GBC) {
        model = GB_MODEL_CGB_E;
    }
    else {
        set_error(error,
                  error_size,
                  "SameBoy only accepts detected GB or GBC content");
        return false;
    }

    machine = &pair->machine[machine_index];
    if (machine->loaded || machine->core != NULL) {
        set_error(error,
                  error_size,
                  "SameBoy machine %u is already loaded",
                  machine_index);
        return false;
    }

    memset(machine->pixels, 0, sizeof(machine->pixels));
    memset(machine->retained_pixels, 0, sizeof(machine->retained_pixels));
    machine->platform = rom->platform;
    machine->core = (GB_gameboy_t *)calloc(1U, GB_allocation_size());
    if (machine->core == NULL) {
        machine->platform = DUALBOY_PLATFORM_INVALID;
        set_error(error,
                  error_size,
                  "Could not allocate SameBoy machine %u",
                  machine_index);
        return false;
    }

    (void)GB_init(machine->core, model);
    configure_machine_callbacks(machine);
    GB_load_rom_from_buffer(machine->core, rom->data, rom->size);
    if (GB_get_screen_width(machine->core) != SAMEBOY_VIDEO_WIDTH ||
        GB_get_screen_height(machine->core) != SAMEBOY_VIDEO_HEIGHT) {
        set_error(error,
                  error_size,
                  "SameBoy machine %u did not configure a 160x144 display",
                  machine_index);
        destroy_machine(machine);
        return false;
    }
    machine->loaded = true;
    machine->vblank_seen = false;
    GB_clear_battery_dirty(machine->core);
    return true;
}

static bool sameboy_set_link(void *opaque_pair,
                             bool enabled,
                             char *error,
                             size_t error_size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;

    clear_error(error, error_size);
    if (pair == NULL) {
        set_error(error, error_size, "SameBoy pair is null");
        return false;
    }
    if (enabled && !pair_is_ready(pair)) {
        set_error(error,
                  error_size,
                  "Both SameBoy machines must be loaded before enabling link");
        return false;
    }
    if (enabled && !pair->link_enabled) {
        pair->serial_output[0] = true;
        pair->serial_output[1] = true;
        pair->infrared_output[0] = false;
        pair->infrared_output[1] = false;
    }
    pair->link_enabled = enabled;
    apply_link_callbacks(pair);
    return true;
}

static void sameboy_reset(void *opaque_pair)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    unsigned index;

    if (pair == NULL) {
        return;
    }
    apply_link_callbacks(pair);
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        if (pair->machine[index].loaded && pair->machine[index].core != NULL) {
            GB_reset(pair->machine[index].core);
            pair->machine[index].vblank_seen = false;
        }
        pair->serial_output[index] = true;
        pair->infrared_output[index] = false;
    }
    pair->tick_skew = 0;
    audio_fifo_clear(&pair->audio);
    apply_link_callbacks(pair);
}

static void sameboy_set_input(void *opaque_pair,
                              unsigned machine_index,
                              uint16_t buttons)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_machine *machine;
    unsigned mask = 0U;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        return;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_RIGHT) != 0U) {
        mask |= (unsigned)GB_KEY_RIGHT_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_LEFT) != 0U) {
        mask |= (unsigned)GB_KEY_LEFT_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_UP) != 0U) {
        mask |= (unsigned)GB_KEY_UP_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_DOWN) != 0U) {
        mask |= (unsigned)GB_KEY_DOWN_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_A) != 0U) {
        mask |= (unsigned)GB_KEY_A_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_B) != 0U) {
        mask |= (unsigned)GB_KEY_B_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_SELECT) != 0U) {
        mask |= (unsigned)GB_KEY_SELECT_MASK;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_START) != 0U) {
        mask |= (unsigned)GB_KEY_START_MASK;
    }
    GB_set_key_mask(machine->core, (GB_key_mask_t)mask);
}

static bool sameboy_run_frame(void *opaque_pair,
                              char *error,
                              size_t error_size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    uint64_t machine_ticks[DUALBOY_MACHINE_COUNT] = {0U, 0U};
    uint64_t calls = 0U;

    clear_error(error, error_size);
    if (!pair_is_ready(pair)) {
        set_error(error, error_size, "SameBoy pair is not fully loaded");
        return false;
    }
    pair->machine[0].vblank_seen = false;
    pair->machine[1].vblank_seen = false;

    while (!pair->machine[0].vblank_seen || !pair->machine[1].vblank_seen) {
        unsigned index = pair->tick_skew >= 0 ? 0U : 1U;
        unsigned elapsed = GB_run(pair->machine[index].core);

        if (elapsed == 0U) {
            set_error(error,
                      error_size,
                      "SameBoy machine %u made no timing progress",
                      index);
            return false;
        }
        machine_ticks[index] += (uint64_t)elapsed;
        if (index == 0U) {
            pair->tick_skew -= (int64_t)elapsed;
        }
        else {
            pair->tick_skew += (int64_t)elapsed;
        }
        ++calls;
        if (calls > SAMEBOY_FRAME_CALL_WATCHDOG ||
            machine_ticks[index] > SAMEBOY_FRAME_TICK_WATCHDOG) {
            set_error(error,
                      error_size,
                      "SameBoy frame scheduler watchdog expired");
            return false;
        }
    }
    return true;
}

static bool sameboy_video_frame(void *opaque_pair,
                                unsigned machine_index,
                                struct dualboy_video_frame *frame)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_machine *machine;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        frame == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded) {
        return false;
    }
    frame->pixels = machine->pixels;
    frame->width = SAMEBOY_VIDEO_WIDTH;
    frame->height = SAMEBOY_VIDEO_HEIGHT;
    frame->pitch = (size_t)SAMEBOY_VIDEO_WIDTH * sizeof(uint32_t);
    return true;
}

static unsigned sameboy_audio_sample_rate(const void *opaque_pair)
{
    (void)opaque_pair;
    return SAMEBOY_AUDIO_RATE;
}

static size_t sameboy_read_audio(void *opaque_pair,
                                 int16_t *interleaved_stereo,
                                 size_t max_frames)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_audio_fifo *fifo;
    size_t frames;
    size_t index;

    if (pair == NULL || (interleaved_stereo == NULL && max_frames != 0U)) {
        return 0U;
    }
    fifo = &pair->audio;
    frames = fifo->frame_count < max_frames ? fifo->frame_count : max_frames;
    for (index = 0U; index < frames; ++index) {
        size_t source =
            (fifo->read_index + index) % (size_t)SAMEBOY_AUDIO_FIFO_FRAMES;

        interleaved_stereo[index * (size_t)2U] =
            fifo->samples[source * (size_t)2U];
        interleaved_stereo[index * (size_t)2U + (size_t)1U] =
            fifo->samples[source * (size_t)2U + (size_t)1U];
    }
    fifo->read_index =
        (fifo->read_index + frames) % (size_t)SAMEBOY_AUDIO_FIFO_FRAMES;
    fifo->frame_count -= frames;
    return frames;
}

static bool sameboy_memory_info(void *opaque_pair,
                                unsigned machine_index,
                                enum dualboy_memory_kind kind,
                                void **data,
                                size_t *size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_machine *machine;

    if (data == NULL || size == NULL) {
        return false;
    }
    *data = NULL;
    *size = 0U;
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }
    switch (kind) {
        case DUALBOY_MEMORY_SAVE_RAM:
            if (machine->core->cartridge_type->has_battery &&
                machine->core->mbc_ram_size != 0U) {
                *data = GB_get_direct_access(machine->core,
                                             GB_DIRECT_ACCESS_CART_RAM,
                                             size,
                                             NULL);
            }
            return true;
        case DUALBOY_MEMORY_RTC:
            if (machine->core->cartridge_type->has_rtc) {
                *data = GB_GET_SECTION(machine->core, rtc);
                *size = GB_SECTION_SIZE(rtc);
            }
            return true;
        default:
            return false;
    }
}

static bool sameboy_memory_dirty(const void *opaque_pair,
                                 unsigned machine_index)
{
    const struct sameboy_pair *pair =
        (const struct sameboy_pair *)opaque_pair;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        !pair->machine[machine_index].loaded ||
        pair->machine[machine_index].core == NULL) {
        return false;
    }
    return GB_get_battery_dirty(pair->machine[machine_index].core);
}

static void sameboy_clear_memory_dirty(void *opaque_pair,
                                       unsigned machine_index)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;

    if (pair != NULL && machine_index < DUALBOY_MACHINE_COUNT &&
        pair->machine[machine_index].loaded &&
        pair->machine[machine_index].core != NULL) {
        GB_clear_battery_dirty(pair->machine[machine_index].core);
    }
}

static size_t sameboy_machine_state_size(const void *opaque_pair,
                                         unsigned machine_index)
{
    const struct sameboy_pair *pair =
        (const struct sameboy_pair *)opaque_pair;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        !pair->machine[machine_index].loaded ||
        pair->machine[machine_index].core == NULL) {
        return 0U;
    }
    return GB_get_save_state_size(pair->machine[machine_index].core);
}

static bool sameboy_serialize_machine(void *opaque_pair,
                                      unsigned machine_index,
                                      void *data,
                                      size_t capacity,
                                      size_t *used)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    size_t required;

    if (used != NULL) {
        *used = 0U;
    }
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        !pair->machine[machine_index].loaded ||
        pair->machine[machine_index].core == NULL || data == NULL ||
        used == NULL) {
        return false;
    }
    required = GB_get_save_state_size(pair->machine[machine_index].core);
    if (capacity < required) {
        return false;
    }
    GB_save_state_to_buffer(pair->machine[machine_index].core,
                            (uint8_t *)data);
    *used = required;
    return true;
}

static bool sameboy_unserialize_machine(void *opaque_pair,
                                        unsigned machine_index,
                                        const void *data,
                                        size_t size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    struct sameboy_machine *machine;
    GB_model_t state_model;
    size_t expected_size;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        data == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }
    expected_size = GB_get_save_state_size(machine->core);
    if (size != expected_size ||
        GB_get_state_model_from_buffer((const uint8_t *)data,
                                       size,
                                       &state_model) != 0 ||
        state_model != GB_get_model(machine->core)) {
        return false;
    }

    /*
     * SameBoy may mutate before reporting a malformed late section. The common
     * frontend snapshots both machines first and rolls both back if this call
     * fails; this primitive deliberately exposes that failure to its caller.
     */
    if (GB_load_state_from_buffer(machine->core,
                                  (const uint8_t *)data,
                                  size) != 0) {
        return false;
    }
    configure_machine_callbacks(machine);
    machine->vblank_seen = false;
    if (machine_index == 0U) {
        audio_fifo_clear(&pair->audio);
    }
    apply_link_callbacks(pair);
    return true;
}

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & UINT16_C(0xff));
    destination[1] = (uint8_t)(value >> 8U);
}

static uint16_t get_u16_le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
                      (uint16_t)((uint16_t)source[1] << 8U));
}

static void put_u64_le(uint8_t *destination, uint64_t value)
{
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint64_t get_u64_le(const uint8_t *source)
{
    uint64_t value = 0U;
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)source[index] << (index * 8U);
    }
    return value;
}

static size_t sameboy_link_state_size(const void *opaque_pair)
{
    (void)opaque_pair;
    return SAMEBOY_LINK_STATE_SIZE;
}

static bool sameboy_serialize_link(const void *opaque_pair,
                                   void *data,
                                   size_t capacity,
                                   size_t *used)
{
    const struct sameboy_pair *pair =
        (const struct sameboy_pair *)opaque_pair;
    uint8_t *bytes = (uint8_t *)data;

    if (used != NULL) {
        *used = 0U;
    }
    if (pair == NULL || data == NULL || used == NULL ||
        capacity < SAMEBOY_LINK_STATE_SIZE) {
        return false;
    }
    memset(bytes, 0, SAMEBOY_LINK_STATE_SIZE);
    bytes[0] = (uint8_t)'D';
    bytes[1] = (uint8_t)'B';
    bytes[2] = (uint8_t)'S';
    bytes[3] = (uint8_t)'L';
    put_u16_le(bytes + 4U, SAMEBOY_LINK_STATE_VERSION);
    put_u16_le(bytes + 6U, SAMEBOY_LINK_STATE_SIZE);
    bytes[8] = pair->link_enabled ? SAMEBOY_LINK_FLAG_ENABLED : 0U;
    bytes[9] = pair->serial_output[0] ? 1U : 0U;
    bytes[10] = pair->serial_output[1] ? 1U : 0U;
    bytes[11] = pair->infrared_output[0] ? 1U : 0U;
    bytes[12] = pair->infrared_output[1] ? 1U : 0U;
    put_u64_le(bytes + 16U, (uint64_t)pair->tick_skew);
    *used = SAMEBOY_LINK_STATE_SIZE;
    return true;
}

static bool decode_serialized_skew(uint64_t encoded, int64_t *skew)
{
    if (skew == NULL) {
        return false;
    }
    if (encoded <= SAMEBOY_MAX_SERIALIZED_SKEW) {
        *skew = (int64_t)encoded;
        return true;
    }
    if (encoded >= UINT64_MAX - SAMEBOY_MAX_SERIALIZED_SKEW + UINT64_C(1)) {
        uint64_t magnitude = (~encoded) + UINT64_C(1);

        *skew = -(int64_t)magnitude;
        return true;
    }
    return false;
}

static bool sameboy_unserialize_link(void *opaque_pair,
                                     const void *data,
                                     size_t size)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;
    const uint8_t *bytes = (const uint8_t *)data;
    int64_t skew;
    bool enabled;
    unsigned index;

    if (pair == NULL || data == NULL || size != SAMEBOY_LINK_STATE_SIZE ||
        bytes[0] != (uint8_t)'D' || bytes[1] != (uint8_t)'B' ||
        bytes[2] != (uint8_t)'S' || bytes[3] != (uint8_t)'L' ||
        get_u16_le(bytes + 4U) != SAMEBOY_LINK_STATE_VERSION ||
        get_u16_le(bytes + 6U) != SAMEBOY_LINK_STATE_SIZE ||
        (bytes[8] & (uint8_t)~SAMEBOY_LINK_FLAG_ENABLED) != 0U ||
        bytes[9] > 1U || bytes[10] > 1U || bytes[11] > 1U ||
        bytes[12] > 1U || bytes[13] != 0U || bytes[14] != 0U ||
        bytes[15] != 0U) {
        return false;
    }
    for (index = 24U; index < SAMEBOY_LINK_STATE_SIZE; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    if (!decode_serialized_skew(get_u64_le(bytes + 16U), &skew)) {
        return false;
    }
    enabled = (bytes[8] & SAMEBOY_LINK_FLAG_ENABLED) != 0U;
    if (enabled && !pair_is_ready(pair)) {
        return false;
    }

    pair->tick_skew = skew;
    pair->serial_output[0] = bytes[9] != 0U;
    pair->serial_output[1] = bytes[10] != 0U;
    pair->infrared_output[0] = bytes[11] != 0U;
    pair->infrared_output[1] = bytes[12] != 0U;
    pair->link_enabled = enabled;
    apply_link_callbacks(pair);
    return true;
}

static void sameboy_destroy_pair(void *opaque_pair)
{
    struct sameboy_pair *pair = (struct sameboy_pair *)opaque_pair;

    if (pair == NULL) {
        return;
    }
    pair->link_enabled = false;
    apply_link_callbacks(pair);
    destroy_machine(&pair->machine[1]);
    destroy_machine(&pair->machine[0]);
    memset(pair->dmg_boot, 0, sizeof(pair->dmg_boot));
    memset(pair->cgb_boot, 0, sizeof(pair->cgb_boot));
    free(pair);
}

const struct dualboy_engine_ops *dualboy_sameboy_engine(void)
{
    static const struct dualboy_engine_ops operations = {
        .name = "SameBoy",
        .family = DUALBOY_ENGINE_SAMEBOY,
        .create_pair = sameboy_create_pair,
        .load_rom = sameboy_load_rom,
        .set_link = sameboy_set_link,
        .reset = sameboy_reset,
        .set_input = sameboy_set_input,
        .run_frame = sameboy_run_frame,
        .video_frame = sameboy_video_frame,
        .audio_sample_rate = sameboy_audio_sample_rate,
        .read_audio = sameboy_read_audio,
        .memory_info = sameboy_memory_info,
        .memory_dirty = sameboy_memory_dirty,
        .clear_memory_dirty = sameboy_clear_memory_dirty,
        .machine_state_size = sameboy_machine_state_size,
        .serialize_machine = sameboy_serialize_machine,
        .unserialize_machine = sameboy_unserialize_machine,
        .link_state_size = sameboy_link_state_size,
        .serialize_link = sameboy_serialize_link,
        .unserialize_link = sameboy_unserialize_link,
        .destroy_pair = sameboy_destroy_pair,
    };

    return &operations;
}
