/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/compositor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

unsigned dualboy_machine_for_port(unsigned port, bool swap_players)
{
    if (port >= DUALBOY_MACHINE_COUNT) {
        return DUALBOY_MACHINE_COUNT;
    }
    return swap_players ? (DUALBOY_MACHINE_COUNT - 1U - port) : port;
}

static bool valid_frame(const struct dualboy_video_frame *frame)
{
    return frame != NULL && frame->pixels != NULL && frame->width > 0U &&
           frame->height > 0U && frame->pitch >= frame->width * sizeof(uint32_t);
}

static unsigned selected_machine(const struct dualboy_compositor_config *config)
{
    unsigned logical_machine = 0U;
    if (config->mode == DUALBOY_MODE_PLAYER2) {
        logical_machine = 1U;
    }
    return dualboy_machine_for_port(logical_machine, config->swap_players);
}

bool dualboy_compositor_geometry(const struct dualboy_video_frame frames[2],
                                 const struct dualboy_compositor_config *config,
                                 struct dualboy_geometry *geometry)
{
    unsigned first;
    unsigned second;

    if (frames == NULL || config == NULL || geometry == NULL ||
        !valid_frame(&frames[0]) || !valid_frame(&frames[1])) {
        return false;
    }

    if (config->mode != DUALBOY_MODE_DUAL) {
        first = selected_machine(config);
        geometry->width = frames[first].width;
        geometry->height = frames[first].height;
    } else {
        first = dualboy_machine_for_port(0U, config->swap_players);
        second = dualboy_machine_for_port(1U, config->swap_players);
        if (frames[first].width != frames[second].width ||
            frames[first].height != frames[second].height) {
            return false;
        }
        if (config->layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
            geometry->width = frames[first].width;
            geometry->height = frames[first].height + frames[second].height;
        } else {
            geometry->width = frames[first].width + frames[second].width;
            geometry->height = frames[first].height;
        }
    }
    geometry->aspect_ratio = (float)geometry->width / (float)geometry->height;
    return true;
}

bool dualboy_compositor_map_point(
    const struct dualboy_video_frame frames[2],
    const struct dualboy_compositor_config *config,
    unsigned composite_x,
    unsigned composite_y,
    unsigned *port,
    unsigned *machine_x,
    unsigned *machine_y)
{
    struct dualboy_geometry geometry;
    unsigned logical_port;
    unsigned machine;
    unsigned local_x = composite_x;
    unsigned local_y = composite_y;

    if (port == NULL || machine_x == NULL || machine_y == NULL ||
        !dualboy_compositor_geometry(frames, config, &geometry) ||
        composite_x >= geometry.width || composite_y >= geometry.height) {
        return false;
    }

    if (config->mode == DUALBOY_MODE_PLAYER1) {
        logical_port = 0U;
    } else if (config->mode == DUALBOY_MODE_PLAYER2) {
        logical_port = 1U;
    } else if (config->layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
        logical_port = composite_y >= frames[dualboy_machine_for_port(
                                              0U, config->swap_players)].height
                           ? 1U
                           : 0U;
        if (logical_port == 1U) {
            local_y -= frames[dualboy_machine_for_port(
                                  0U, config->swap_players)].height;
        }
    } else {
        logical_port = composite_x >= frames[dualboy_machine_for_port(
                                              0U, config->swap_players)].width
                           ? 1U
                           : 0U;
        if (logical_port == 1U) {
            local_x -= frames[dualboy_machine_for_port(
                                  0U, config->swap_players)].width;
        }
    }

    machine = dualboy_machine_for_port(logical_port, config->swap_players);
    if (machine >= DUALBOY_MACHINE_COUNT || local_x >= frames[machine].width ||
        local_y >= frames[machine].height) {
        return false;
    }
    *port = logical_port;
    *machine_x = local_x;
    *machine_y = local_y;
    return true;
}

static void copy_row(uint32_t *destination,
                     const struct dualboy_video_frame *source,
                     unsigned row)
{
    const uint8_t *source_bytes = (const uint8_t *)source->pixels;
    memcpy(destination,
           source_bytes + (size_t)row * source->pitch,
           (size_t)source->width * sizeof(uint32_t));
}

bool dualboy_compose_frame(uint32_t *output,
                           size_t output_pixels,
                           const struct dualboy_video_frame frames[2],
                           const struct dualboy_compositor_config *config,
                           struct dualboy_video_frame *composite)
{
    struct dualboy_geometry geometry;
    unsigned first;
    unsigned second;
    unsigned row;

    if (output == NULL || composite == NULL ||
        !dualboy_compositor_geometry(frames, config, &geometry) ||
        output_pixels < (size_t)geometry.width * geometry.height) {
        return false;
    }

    if (config->mode != DUALBOY_MODE_DUAL) {
        first = selected_machine(config);
        for (row = 0U; row < frames[first].height; ++row) {
            copy_row(output + (size_t)row * geometry.width, &frames[first], row);
        }
    } else {
        first = dualboy_machine_for_port(0U, config->swap_players);
        second = dualboy_machine_for_port(1U, config->swap_players);
        if (config->layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
            for (row = 0U; row < frames[first].height; ++row) {
                copy_row(output + (size_t)row * geometry.width, &frames[first], row);
            }
            for (row = 0U; row < frames[second].height; ++row) {
                copy_row(output +
                             (size_t)(row + frames[first].height) * geometry.width,
                         &frames[second],
                         row);
            }
        } else {
            for (row = 0U; row < geometry.height; ++row) {
                uint32_t *destination = output + (size_t)row * geometry.width;
                copy_row(destination, &frames[first], row);
                copy_row(destination + frames[first].width, &frames[second], row);
            }
        }
    }

    composite->pixels = output;
    composite->width = geometry.width;
    composite->height = geometry.height;
    composite->pitch = (size_t)geometry.width * sizeof(uint32_t);
    return true;
}
