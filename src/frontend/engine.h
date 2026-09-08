/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_ENGINE_H
#define DUALBOY_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUALBOY_MACHINE_COUNT 2U

enum dualboy_platform {
    DUALBOY_PLATFORM_INVALID = 0,
    DUALBOY_PLATFORM_GB,
    DUALBOY_PLATFORM_GBC,
    DUALBOY_PLATFORM_GBA,
    DUALBOY_PLATFORM_NDS,
};

enum dualboy_engine_family {
    DUALBOY_ENGINE_NONE = 0,
    DUALBOY_ENGINE_SAMEBOY,
    DUALBOY_ENGINE_MGBA,
    DUALBOY_ENGINE_MELONDS,
};

enum dualboy_log_level {
    DUALBOY_LOG_DEBUG = 0,
    DUALBOY_LOG_INFO,
    DUALBOY_LOG_WARN,
    DUALBOY_LOG_ERROR,
};

enum dualboy_memory_kind {
    DUALBOY_MEMORY_SAVE_RAM = 0,
    DUALBOY_MEMORY_RTC,
    DUALBOY_MEMORY_FIRMWARE,
    DUALBOY_MEMORY_KIND_COUNT,
};

enum dualboy_button {
    DUALBOY_BUTTON_B = 1U << 0,
    DUALBOY_BUTTON_Y = 1U << 1,
    DUALBOY_BUTTON_SELECT = 1U << 2,
    DUALBOY_BUTTON_START = 1U << 3,
    DUALBOY_BUTTON_UP = 1U << 4,
    DUALBOY_BUTTON_DOWN = 1U << 5,
    DUALBOY_BUTTON_LEFT = 1U << 6,
    DUALBOY_BUTTON_RIGHT = 1U << 7,
    DUALBOY_BUTTON_A = 1U << 8,
    DUALBOY_BUTTON_X = 1U << 9,
    DUALBOY_BUTTON_L = 1U << 10,
    DUALBOY_BUTTON_R = 1U << 11,
};

typedef void (*dualboy_log_fn)(void *context,
                               enum dualboy_log_level level,
                               const char *message);

struct dualboy_engine_config {
    dualboy_log_fn log;
    void *log_context;
    const char *system_directory;
    unsigned audio_sample_rate;
};

struct dualboy_rom {
    enum dualboy_platform platform;
    const uint8_t *data;
    size_t size;
    const char *path;
};

struct dualboy_video_frame {
    const uint32_t *pixels;
    unsigned width;
    unsigned height;
    size_t pitch;
};

struct dualboy_machine_input {
    uint16_t buttons;
    bool touch_active;
    uint16_t touch_x;
    uint16_t touch_y;
};

/*
 * `pair` is adapter-private and owns every engine allocation. The frontend calls
 * load_rom exactly once for each machine after create_pair, then set_link. Every
 * operation must tolerate teardown after a failed partial load.
 */
struct dualboy_engine_ops {
    const char *name;
    enum dualboy_engine_family family;

    bool (*create_pair)(void **pair,
                        const struct dualboy_engine_config *config,
                        char *error,
                        size_t error_size);
    bool (*load_rom)(void *pair,
                     unsigned machine,
                     const struct dualboy_rom *rom,
                     char *error,
                     size_t error_size);
    bool (*set_link)(void *pair,
                     bool enabled,
                     char *error,
                     size_t error_size);
    void (*reset)(void *pair);
    /* Legacy button-only adapters may implement set_input. New adapters should
     * consume the complete per-machine value through set_machine_input. */
    void (*set_input)(void *pair, unsigned machine, uint16_t buttons);
    void (*set_machine_input)(void *pair,
                              unsigned machine,
                              const struct dualboy_machine_input *input);
    /* This is a synchronous ownership boundary even for adapters backed by
     * worker threads. On every return, including failure, workers launched for
     * the frame must have stopped accessing pair-owned video, audio,
     * persistence, and engine state. */
    bool (*run_frame)(void *pair, char *error, size_t error_size);
    bool (*video_frame)(void *pair,
                        unsigned machine,
                        struct dualboy_video_frame *frame);

    unsigned (*audio_sample_rate)(const void *pair);
    double (*frame_rate)(const void *pair);
    size_t (*read_audio)(void *pair,
                         int16_t *interleaved_stereo,
                         size_t max_frames);

    /* Non-empty memory regions remain at a stable address and size until the
     * pair is destroyed, including across machine-state restores. */
    bool (*memory_info)(void *pair,
                        unsigned machine,
                        enum dualboy_memory_kind kind,
                        void **data,
                        size_t *size);
    /* Optional disk extent for a stable memory region. `known == false`
     * means an auto-detecting engine has not selected a save device yet;
     * callers must preserve any existing file and defer writes. */
    bool (*persistent_memory_extent)(const void *pair,
                                     unsigned machine,
                                     enum dualboy_memory_kind kind,
                                     bool *known,
                                     size_t *size);
    /* Optional validation for untrusted bytes read from disk. This runs before
     * the persistence layer copies a region into live engine memory, so an
     * engine may reject malformed private formats without exposing them to
     * upstream code. */
    bool (*validate_persistent_memory)(const void *pair,
                                       unsigned machine,
                                       enum dualboy_memory_kind kind,
                                       const void *data,
                                       size_t size,
                                       char *error,
                                       size_t error_size);
    bool (*memory_dirty)(const void *pair, unsigned machine);
    void (*clear_memory_dirty)(void *pair, unsigned machine);

    size_t (*machine_state_size)(const void *pair, unsigned machine);
    bool (*serialize_machine)(void *pair,
                              unsigned machine,
                              void *data,
                              size_t capacity,
                              size_t *used);
    bool (*unserialize_machine)(void *pair,
                                unsigned machine,
                                const void *data,
                                size_t size);

    size_t (*link_state_size)(const void *pair);
    bool (*serialize_link)(const void *pair,
                           void *data,
                           size_t capacity,
                           size_t *used);
    bool (*unserialize_link)(void *pair,
                             const void *data,
                             size_t size);

    /* True only while both machines are actively joined to an engine transport
     * that requires real-time pacing. The Libretro layer owns frontend policy. */
    bool (*link_transport_active)(const void *pair);

    void (*destroy_pair)(void *pair);

    /* Optional notification after the persistence layer has populated every
     * stable memory region. Engines that cache firmware-derived runtime state
     * may rebuild it here before the first emulated frame. */
    void (*persistent_memory_loaded)(void *pair);
};

const struct dualboy_engine_ops *dualboy_sameboy_engine(void);
const struct dualboy_engine_ops *dualboy_mgba_engine(void);
const struct dualboy_engine_ops *dualboy_melonds_engine(void);

#ifdef __cplusplus
}
#endif

#endif
