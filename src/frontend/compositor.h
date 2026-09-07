/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_COMPOSITOR_H
#define DUALBOY_COMPOSITOR_H

#include "frontend/engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum dualboy_display_mode {
    DUALBOY_MODE_DUAL = 0,
    DUALBOY_MODE_PLAYER1,
    DUALBOY_MODE_PLAYER2,
};

enum dualboy_layout {
    DUALBOY_LAYOUT_SIDE_BY_SIDE = 0,
    DUALBOY_LAYOUT_TOP_BOTTOM,
};

struct dualboy_compositor_config {
    enum dualboy_display_mode mode;
    enum dualboy_layout layout;
    bool swap_players;
};

struct dualboy_geometry {
    unsigned width;
    unsigned height;
    float aspect_ratio;
};

unsigned dualboy_machine_for_port(unsigned port, bool swap_players);

bool dualboy_compositor_geometry(const struct dualboy_video_frame frames[2],
                                 const struct dualboy_compositor_config *config,
                                 struct dualboy_geometry *geometry);

bool dualboy_compose_frame(uint32_t *output,
                           size_t output_pixels,
                           const struct dualboy_video_frame frames[2],
                           const struct dualboy_compositor_config *config,
                           struct dualboy_video_frame *composite);

#endif
