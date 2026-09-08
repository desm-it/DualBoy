/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/touch_cursor.h"

#include <stddef.h>
#include <string.h>

static unsigned coordinate_distance(uint16_t first, uint16_t second)
{
    return first >= second ? (unsigned)(first - second)
                           : (unsigned)(second - first);
}

void dualboy_touch_cursor_reset(struct dualboy_touch_cursor *cursor)
{
    if (cursor != NULL) {
        memset(cursor, 0, sizeof(*cursor));
    }
}

void dualboy_touch_cursor_update(struct dualboy_touch_cursor *cursor,
                                 uint16_t x,
                                 uint16_t y,
                                 bool initially_deflected,
                                 bool pressed,
                                 bool time_available,
                                 uint64_t now_usec)
{
    bool activity;

    if (cursor == NULL) {
        return;
    }

    if (!cursor->sampled) {
        cursor->sampled = true;
        cursor->x = x;
        cursor->y = y;
        cursor->activity_x = x;
        cursor->activity_y = y;
        activity = initially_deflected || pressed;
    } else {
        activity =
            coordinate_distance(x, cursor->activity_x) >=
                DUALBOY_TOUCH_CURSOR_MOTION_PIXELS ||
            coordinate_distance(y, cursor->activity_y) >=
                DUALBOY_TOUCH_CURSOR_MOTION_PIXELS ||
            (pressed && !cursor->pressed);
        cursor->x = x;
        cursor->y = y;
    }
    cursor->pressed = pressed;

    if (activity) {
        cursor->activity_x = x;
        cursor->activity_y = y;
        cursor->last_activity_usec = now_usec;
        cursor->inactive_frames = 0U;
        cursor->using_clock = time_available;
        cursor->visible = true;
        return;
    }
    if (!cursor->visible) {
        return;
    }

    if (time_available) {
        if (!cursor->using_clock || now_usec < cursor->last_activity_usec) {
            cursor->last_activity_usec = now_usec;
            cursor->inactive_frames = 0U;
            cursor->using_clock = true;
        } else if (now_usec - cursor->last_activity_usec >=
                   DUALBOY_TOUCH_CURSOR_TIMEOUT_USEC) {
            cursor->visible = false;
        }
    } else {
        if (cursor->using_clock) {
            cursor->inactive_frames = 0U;
            cursor->using_clock = false;
        } else if (++cursor->inactive_frames >=
                   DUALBOY_TOUCH_CURSOR_FALLBACK_FRAMES) {
            cursor->visible = false;
        }
    }
}

bool dualboy_touch_cursor_visible(const struct dualboy_touch_cursor *cursor)
{
    return cursor != NULL && cursor->visible;
}
