/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_TOUCH_CURSOR_H
#define DUALBOY_TOUCH_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#define DUALBOY_TOUCH_CURSOR_TIMEOUT_USEC UINT64_C(3000000)
#define DUALBOY_TOUCH_CURSOR_FALLBACK_FRAMES 180U
#define DUALBOY_TOUCH_CURSOR_MOTION_PIXELS 2U

struct dualboy_touch_cursor {
    uint16_t x;
    uint16_t y;
    uint16_t activity_x;
    uint16_t activity_y;
    uint64_t last_activity_usec;
    unsigned inactive_frames;
    bool sampled;
    bool pressed;
    bool visible;
    bool using_clock;
};

void dualboy_touch_cursor_reset(struct dualboy_touch_cursor *cursor);

/* Tracks an absolute analog-stick aim point. `initially_deflected` prevents a
 * neutral first sample from showing a cursor as soon as content loads. A press
 * edge is also activity so R3 can reveal a stationary aim point. */
void dualboy_touch_cursor_update(struct dualboy_touch_cursor *cursor,
                                 uint16_t x,
                                 uint16_t y,
                                 bool initially_deflected,
                                 bool pressed,
                                 bool time_available,
                                 uint64_t now_usec);

bool dualboy_touch_cursor_visible(const struct dualboy_touch_cursor *cursor);

#endif
