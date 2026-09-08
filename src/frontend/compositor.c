/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/compositor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

unsigned dualboy_machine_for_screen(unsigned screen, bool swap_screens)
{
    if (screen >= DUALBOY_MACHINE_COUNT) {
        return DUALBOY_MACHINE_COUNT;
    }
    return swap_screens ? (DUALBOY_MACHINE_COUNT - 1U - screen) : screen;
}

static bool valid_frame(const struct dualboy_video_frame *frame)
{
    return frame != NULL && frame->pixels != NULL && frame->width > 0U &&
           frame->height > 0U && frame->pitch >= frame->width * sizeof(uint32_t);
}

static unsigned selected_machine(const struct dualboy_compositor_config *config)
{
    return config->mode == DUALBOY_MODE_PLAYER2 ? 1U : 0U;
}

static unsigned screen_for_machine(unsigned machine, bool swap_screens)
{
    if (machine >= DUALBOY_MACHINE_COUNT) {
        return DUALBOY_MACHINE_COUNT;
    }
    return swap_screens ? (DUALBOY_MACHINE_COUNT - 1U - machine) : machine;
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
        first = dualboy_machine_for_screen(0U, config->swap_screens);
        second = dualboy_machine_for_screen(1U, config->swap_screens);
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
    unsigned *mapped_machine,
    unsigned *machine_x,
    unsigned *machine_y)
{
    struct dualboy_geometry geometry;
    unsigned screen;
    unsigned machine;
    unsigned local_x = composite_x;
    unsigned local_y = composite_y;

    if (mapped_machine == NULL || machine_x == NULL || machine_y == NULL ||
        !dualboy_compositor_geometry(frames, config, &geometry) ||
        composite_x >= geometry.width || composite_y >= geometry.height) {
        return false;
    }

    if (config->mode == DUALBOY_MODE_PLAYER1) {
        machine = 0U;
    } else if (config->mode == DUALBOY_MODE_PLAYER2) {
        machine = 1U;
    } else if (config->layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
        const unsigned first =
            dualboy_machine_for_screen(0U, config->swap_screens);
        screen = composite_y >= frames[first].height ? 1U : 0U;
        if (screen == 1U) {
            local_y -= frames[first].height;
        }
        machine = dualboy_machine_for_screen(screen, config->swap_screens);
    } else {
        const unsigned first =
            dualboy_machine_for_screen(0U, config->swap_screens);
        screen = composite_x >= frames[first].width ? 1U : 0U;
        if (screen == 1U) {
            local_x -= frames[first].width;
        }
        machine = dualboy_machine_for_screen(screen, config->swap_screens);
    }

    if (machine >= DUALBOY_MACHINE_COUNT || local_x >= frames[machine].width ||
        local_y >= frames[machine].height) {
        return false;
    }
    *mapped_machine = machine;
    *machine_x = local_x;
    *machine_y = local_y;
    return true;
}

bool dualboy_compositor_project_point(
    const struct dualboy_video_frame frames[2],
    const struct dualboy_compositor_config *config,
    unsigned machine,
    unsigned machine_x,
    unsigned machine_y,
    unsigned *composite_x,
    unsigned *composite_y)
{
    struct dualboy_geometry geometry;
    unsigned screen;
    unsigned first;
    unsigned x = machine_x;
    unsigned y = machine_y;

    if (frames == NULL || config == NULL || composite_x == NULL ||
        composite_y == NULL || machine >= DUALBOY_MACHINE_COUNT ||
        !dualboy_compositor_geometry(frames, config, &geometry)) {
        return false;
    }
    if ((config->mode == DUALBOY_MODE_PLAYER1 && machine != 0U) ||
        (config->mode == DUALBOY_MODE_PLAYER2 && machine != 1U)) {
        return false;
    }

    if (machine_x >= frames[machine].width ||
        machine_y >= frames[machine].height) {
        return false;
    }

    screen = screen_for_machine(machine, config->swap_screens);
    if (config->mode == DUALBOY_MODE_DUAL && screen == 1U) {
        first = dualboy_machine_for_screen(0U, config->swap_screens);
        if (config->layout == DUALBOY_LAYOUT_TOP_BOTTOM) {
            y += frames[first].height;
        } else {
            x += frames[first].width;
        }
    }
    if (x >= geometry.width || y >= geometry.height) {
        return false;
    }
    *composite_x = x;
    *composite_y = y;
    return true;
}

bool dualboy_compositor_draw_cursor(uint32_t *output,
                                    unsigned width,
                                    unsigned height,
                                    size_t pitch,
                                    unsigned center_x,
                                    unsigned center_y,
                                    uint32_t accent)
{
    static const uint32_t black = UINT32_C(0x00000000);
    static const uint32_t white = UINT32_C(0x00ffffff);
    size_t row_bytes;
    int offset_y;

    if (output == NULL || width == 0U || height == 0U) {
        return false;
    }
    row_bytes = (size_t)width * sizeof(*output);
    if (row_bytes / sizeof(*output) != width || pitch < row_bytes ||
        center_x >= width || center_y >= height) {
        return false;
    }

    for (offset_y = -8; offset_y <= 8; ++offset_y) {
        const int64_t pixel_y = (int64_t)center_y + offset_y;
        int offset_x;

        if (pixel_y < 0 || pixel_y >= (int64_t)height) {
            continue;
        }
        for (offset_x = -8; offset_x <= 8; ++offset_x) {
            const int64_t pixel_x = (int64_t)center_x + offset_x;
            const int absolute_x = offset_x < 0 ? -offset_x : offset_x;
            const int absolute_y = offset_y < 0 ? -offset_y : offset_y;
            const int distance_squared =
                offset_x * offset_x + offset_y * offset_y;
            uint32_t color;
            bool draw = false;

            if (pixel_x < 0 || pixel_x >= (int64_t)width) {
                continue;
            }

            /* A white inner ring and black outer ring guarantee a contrasting
             * edge regardless of the game pixel beneath them. */
            if (distance_squared >= 46 && distance_squared <= 68) {
                color = black;
                draw = true;
            } else if (distance_squared >= 30 && distance_squared <= 45) {
                color = white;
                draw = true;
            }

            /* Fine crosshair ticks connect the ring to the exact target. */
            if (((absolute_x <= 1 && absolute_y <= 8) ||
                 (absolute_y <= 1 && absolute_x <= 8))) {
                color = black;
                draw = true;
                if ((offset_x == 0 || offset_y == 0) &&
                    (absolute_x >= 3 || absolute_y >= 3)) {
                    color = white;
                }
            }

            /* Keep the aim point precise and distinguish the two players. */
            if (absolute_x <= 2 && absolute_y <= 2) {
                color = black;
                draw = true;
                if (absolute_x <= 1 && absolute_y <= 1) {
                    color = accent & UINT32_C(0x00ffffff);
                }
            }

            if (draw) {
                uint8_t *row = (uint8_t *)(void *)output +
                               (size_t)pixel_y * pitch;
                memcpy(row + (size_t)pixel_x * sizeof(color), &color,
                       sizeof(color));
            }
        }
    }
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
        first = dualboy_machine_for_screen(0U, config->swap_screens);
        second = dualboy_machine_for_screen(1U, config->swap_screens);
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
