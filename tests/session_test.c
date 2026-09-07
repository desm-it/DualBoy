/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

struct fake_pair {
    const uint8_t *rom_data[2];
    uint16_t input[2];
    uint32_t pixels[2][4];
    bool fail_second_load;
};

static bool fake_fail_second;
static unsigned fake_destroy_count;

static bool fake_create(void **context,
                        const struct dualboy_engine_config *config,
                        char *error,
                        size_t error_size)
{
    struct fake_pair *pair = calloc(1U, sizeof(*pair));
    (void)config;
    (void)error;
    (void)error_size;
    if (pair == NULL) {
        return false;
    }
    pair->fail_second_load = fake_fail_second;
    *context = pair;
    return true;
}

static bool fake_load(void *context,
                      unsigned machine,
                      const struct dualboy_rom *rom,
                      char *error,
                      size_t error_size)
{
    struct fake_pair *pair = context;
    (void)error;
    (void)error_size;
    if (machine > 1U || (machine == 1U && pair->fail_second_load)) {
        return false;
    }
    pair->rom_data[machine] = rom->data;
    return true;
}

static bool fake_link(void *context,
                      bool enabled,
                      char *error,
                      size_t error_size)
{
    (void)context;
    (void)enabled;
    (void)error;
    (void)error_size;
    return true;
}

static void fake_input(void *context, unsigned machine, uint16_t buttons)
{
    struct fake_pair *pair = context;
    pair->input[machine] = buttons;
}

static bool fake_run(void *context, char *error, size_t error_size)
{
    struct fake_pair *pair = context;
    unsigned machine;
    unsigned pixel;
    (void)error;
    (void)error_size;
    for (machine = 0U; machine < 2U; ++machine) {
        for (pixel = 0U; pixel < 4U; ++pixel) {
            pair->pixels[machine][pixel] = (uint32_t)(machine + 1U);
        }
    }
    return true;
}

static bool fake_video(void *context,
                       unsigned machine,
                       struct dualboy_video_frame *frame)
{
    struct fake_pair *pair = context;
    if (machine > 1U || frame == NULL) {
        return false;
    }
    frame->pixels = pair->pixels[machine];
    frame->width = 2U;
    frame->height = 2U;
    frame->pitch = 2U * sizeof(uint32_t);
    return true;
}

static void fake_destroy(void *context)
{
    ++fake_destroy_count;
    free(context);
}

static const struct dualboy_engine_ops fake_ops = {
    .name = "fake",
    .family = DUALBOY_ENGINE_SAMEBOY,
    .create_pair = fake_create,
    .load_rom = fake_load,
    .set_link = fake_link,
    .set_input = fake_input,
    .run_frame = fake_run,
    .video_frame = fake_video,
    .destroy_pair = fake_destroy,
};

static bool test_same_content_and_swap(void)
{
    uint8_t source[4] = {1U, 2U, 3U, 4U};
    const struct dualboy_rom roms[2] = {
        {DUALBOY_PLATFORM_GB, source, sizeof(source), "first.gb"},
        {DUALBOY_PLATFORM_GB, source, sizeof(source), "first.gb"},
    };
    const struct dualboy_engine_config engine_config = {0};
    struct dualboy_compositor_config display = {
        DUALBOY_MODE_DUAL,
        DUALBOY_LAYOUT_SIDE_BY_SIDE,
        true,
    };
    const uint16_t input[2] = {0x11U, 0x22U};
    struct dualboy_session session;
    struct fake_pair *pair;
    char error[128] = {0};

    dualboy_session_init(&session);
    CHECK(dualboy_session_load(&session, &fake_ops, &engine_config, roms,
                               DUALBOY_LOAD_NORMAL, true, error, sizeof(error)));
    pair = session.pair;
    CHECK(pair->rom_data[0] == pair->rom_data[1]);
    CHECK(pair->rom_data[0] != source);
    source[0] = 0xFFU;
    CHECK(pair->rom_data[0][0] == 1U);

    CHECK(dualboy_session_run(&session, input, &display, error, sizeof(error)));
    CHECK(pair->input[0] == input[1]);
    CHECK(pair->input[1] == input[0]);
    CHECK(session.composite.width == 4U && session.composite.height == 2U);
    CHECK(session.composite.pixels[0] == 2U);
    CHECK(session.composite.pixels[2] == 1U);

    dualboy_session_unload(&session);
    dualboy_session_unload(&session);
    return true;
}

static bool test_partial_failure_cleanup(void)
{
    uint8_t first[1] = {1U};
    uint8_t second[1] = {2U};
    const struct dualboy_rom roms[2] = {
        {DUALBOY_PLATFORM_GB, first, sizeof(first), "first.gb"},
        {DUALBOY_PLATFORM_GB, second, sizeof(second), "second.gb"},
    };
    const struct dualboy_engine_config engine_config = {0};
    struct dualboy_session session;
    char error[128] = {0};
    unsigned before;

    dualboy_session_init(&session);
    fake_fail_second = true;
    before = fake_destroy_count;
    CHECK(!dualboy_session_load(&session, &fake_ops, &engine_config, roms,
                                DUALBOY_LOAD_SUBSYSTEM, true, error,
                                sizeof(error)));
    CHECK(fake_destroy_count == before + 1U);
    CHECK(session.pair == NULL && !session.loaded);
    dualboy_session_unload(&session);
    CHECK(fake_destroy_count == before + 1U);
    fake_fail_second = false;
    return true;
}

int main(void)
{
    if (!test_same_content_and_swap() || !test_partial_failure_cleanup()) {
        return EXIT_FAILURE;
    }
    puts("session tests passed");
    return EXIT_SUCCESS;
}
