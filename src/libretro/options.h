/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_LIBRETRO_OPTIONS_H
#define DUALBOY_LIBRETRO_OPTIONS_H

#include "frontend/compositor.h"

#include <libretro.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dualboy_options {
    enum dualboy_display_mode mode;
    enum dualboy_layout layout;
    bool link_enabled;
    unsigned controller_ports[DUALBOY_MACHINE_COUNT];
    bool swap_screens;
    bool audio_player1;
    enum dualboy_video_renderer nds_renderer;
};

#define DUALBOY_OPTION_LINK "dualboy_link"
#define DUALBOY_OPTION_NDS_RENDERER "dualboy_nds_renderer"

void dualboy_options_set_defaults(struct dualboy_options *options);

/* Register DualBoy options using the newest API advertised by the frontend. */
void dualboy_options_register(retro_environment_t environment);

/*
 * Read all available values, retaining the previous value for a missing or
 * unrecognised option. Returns true if any parsed setting changed.
 */
bool dualboy_options_read(retro_environment_t environment,
                          struct dualboy_options *options);

#ifdef __cplusplus
}
#endif

#endif
