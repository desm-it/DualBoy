/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_SESSION_H
#define DUALBOY_SESSION_H

#include "frontend/compositor.h"
#include "frontend/engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUALBOY_MAX_COMPOSITE_WIDTH 480U
#define DUALBOY_MAX_COMPOSITE_HEIGHT 320U
#define DUALBOY_MAX_COMPOSITE_PIXELS                                           \
    ((size_t)DUALBOY_MAX_COMPOSITE_WIDTH * DUALBOY_MAX_COMPOSITE_HEIGHT)

enum dualboy_load_kind {
    DUALBOY_LOAD_NORMAL = 0,
    DUALBOY_LOAD_SUBSYSTEM,
    DUALBOY_LOAD_PLAYLIST,
};

struct dualboy_owned_rom {
    struct dualboy_rom rom;
    uint8_t *owned_data;
    char *owned_path;
};

struct dualboy_session {
    const struct dualboy_engine_ops *engine;
    void *pair;
    struct dualboy_owned_rom roms[DUALBOY_MACHINE_COUNT];
    uint32_t *composite_pixels;
    struct dualboy_video_frame composite;
    struct dualboy_compositor_config display;
    enum dualboy_load_kind load_kind;
    bool link_enabled;
    bool loaded;
};

void dualboy_session_init(struct dualboy_session *session);

bool dualboy_session_load(struct dualboy_session *session,
                          const struct dualboy_engine_ops *engine,
                          const struct dualboy_engine_config *engine_config,
                          const struct dualboy_rom roms[DUALBOY_MACHINE_COUNT],
                          enum dualboy_load_kind load_kind,
                          bool link_enabled,
                          char *error,
                          size_t error_size);

void dualboy_session_unload(struct dualboy_session *session);

void dualboy_session_reset(struct dualboy_session *session);

bool dualboy_session_set_link(struct dualboy_session *session,
                              bool enabled,
                              char *error,
                              size_t error_size);

bool dualboy_session_run(struct dualboy_session *session,
                         const uint16_t port_buttons[DUALBOY_MACHINE_COUNT],
                         const struct dualboy_compositor_config *display,
                         char *error,
                         size_t error_size);

bool dualboy_session_geometry(struct dualboy_session *session,
                              const struct dualboy_compositor_config *display,
                              struct dualboy_geometry *geometry);

#endif
