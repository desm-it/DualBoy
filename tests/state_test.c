/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/state.h"

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
    uint8_t value[2];
    uint8_t battery[2];
    uint8_t link_value;
    bool link_enabled;
    int fail_machine;
};

static int next_failure = -1;

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
    pair->fail_machine = next_failure;
    next_failure = -1;
    *context = pair;
    return true;
}

static bool fake_load(void *context,
                      unsigned machine,
                      const struct dualboy_rom *rom,
                      char *error,
                      size_t error_size)
{
    (void)context;
    (void)machine;
    (void)rom;
    (void)error;
    (void)error_size;
    return true;
}

static bool fake_set_link(void *context,
                          bool enabled,
                          char *error,
                          size_t error_size)
{
    struct fake_pair *pair = context;
    (void)error;
    (void)error_size;
    pair->link_enabled = enabled;
    return true;
}

static bool fake_memory(void *context,
                        unsigned machine,
                        enum dualboy_memory_kind kind,
                        void **data,
                        size_t *size)
{
    struct fake_pair *pair = context;
    if (machine > 1U || kind != DUALBOY_MEMORY_SAVE_RAM) {
        return false;
    }
    *data = &pair->battery[machine];
    *size = 1U;
    return true;
}

static size_t fake_machine_size(const void *context, unsigned machine)
{
    (void)context;
    return machine < 2U ? 2U : 0U;
}

static bool fake_serialize_machine(void *context,
                                   unsigned machine,
                                   void *data,
                                   size_t capacity,
                                   size_t *used)
{
    struct fake_pair *pair = context;
    uint8_t *bytes = data;
    if (machine > 1U || capacity < 2U || data == NULL || used == NULL) {
        return false;
    }
    bytes[0] = pair->value[machine];
    bytes[1] = pair->battery[machine];
    *used = 2U;
    return true;
}

static bool fake_unserialize_machine(void *context,
                                     unsigned machine,
                                     const void *data,
                                     size_t size)
{
    struct fake_pair *pair = context;
    const uint8_t *bytes = data;
    if (machine > 1U || size != 2U || data == NULL) {
        return false;
    }
    pair->value[machine] = bytes[0];
    pair->battery[machine] = bytes[1];
    if ((int)machine == pair->fail_machine) {
        pair->fail_machine = -1;
        return false;
    }
    return true;
}

static size_t fake_link_size(const void *context)
{
    (void)context;
    return 1U;
}

static bool fake_serialize_link(const void *context,
                                void *data,
                                size_t capacity,
                                size_t *used)
{
    const struct fake_pair *pair = context;
    if (data == NULL || capacity < 1U || used == NULL) {
        return false;
    }
    *(uint8_t *)data = pair->link_value;
    *used = 1U;
    return true;
}

static bool fake_unserialize_link(void *context, const void *data, size_t size)
{
    struct fake_pair *pair = context;
    if (data == NULL || size != 1U) {
        return false;
    }
    pair->link_value = *(const uint8_t *)data;
    return true;
}

static void fake_destroy(void *context)
{
    free(context);
}

static const struct dualboy_engine_ops fake_ops = {
    .name = "state fake",
    .family = DUALBOY_ENGINE_SAMEBOY,
    .create_pair = fake_create,
    .load_rom = fake_load,
    .set_link = fake_set_link,
    .memory_info = fake_memory,
    .machine_state_size = fake_machine_size,
    .serialize_machine = fake_serialize_machine,
    .unserialize_machine = fake_unserialize_machine,
    .link_state_size = fake_link_size,
    .serialize_link = fake_serialize_link,
    .unserialize_link = fake_unserialize_link,
    .destroy_pair = fake_destroy,
};

static bool make_session(struct dualboy_session *session)
{
    static const uint8_t rom_data[1] = {0U};
    const struct dualboy_rom roms[2] = {
        {DUALBOY_PLATFORM_GB, rom_data, sizeof(rom_data), "first.gb"},
        {DUALBOY_PLATFORM_GB, rom_data, sizeof(rom_data), "first.gb"},
    };
    const struct dualboy_engine_config config = {0};
    char error[128] = {0};

    dualboy_session_init(session);
    return dualboy_session_load(session, &fake_ops, &config, roms,
                                DUALBOY_LOAD_NORMAL, true, error, sizeof(error));
}

static bool test_round_trip_and_battery_protection(void)
{
    struct dualboy_session session;
    struct fake_pair *pair;
    uint8_t *state;
    size_t state_size;
    char error[128] = {0};

    CHECK(make_session(&session));
    pair = session.pair;
    pair->value[0] = 11U;
    pair->value[1] = 22U;
    pair->battery[0] = 33U;
    pair->battery[1] = 44U;
    pair->link_value = 55U;
    state_size = dualboy_state_size(&session);
    CHECK(state_size == 117U);
    state = malloc(state_size);
    CHECK(state != NULL);
    CHECK(dualboy_state_serialize(&session, state, state_size, error,
                                  sizeof(error)));

    pair->value[0] = 1U;
    pair->value[1] = 2U;
    pair->battery[0] = 3U;
    pair->battery[1] = 4U;
    pair->link_value = 5U;
    pair->link_enabled = false;
    session.link_enabled = false;
    CHECK(dualboy_state_unserialize(&session, state, state_size, error,
                                    sizeof(error)));
    CHECK(pair->value[0] == 11U && pair->value[1] == 22U);
    CHECK(pair->battery[0] == 3U && pair->battery[1] == 4U);
    CHECK(pair->link_value == 55U);
    CHECK(pair->link_enabled && session.link_enabled);

    free(state);
    dualboy_session_unload(&session);
    return true;
}

static bool test_corruption_rejected_before_mutation(void)
{
    struct dualboy_session session;
    struct fake_pair *pair;
    uint8_t *state;
    size_t state_size;
    char error[128] = {0};

    CHECK(make_session(&session));
    pair = session.pair;
    pair->value[0] = 10U;
    pair->value[1] = 20U;
    state_size = dualboy_state_size(&session);
    state = malloc(state_size);
    CHECK(state != NULL);
    CHECK(dualboy_state_serialize(&session, state, state_size, error,
                                  sizeof(error)));
    state[DUALBOY_STATE_HEADER_BYTES + 1U] ^= 1U;
    pair->value[0] = 30U;
    pair->value[1] = 40U;
    CHECK(!dualboy_state_unserialize(&session, state, state_size, error,
                                     sizeof(error)));
    CHECK(pair->value[0] == 30U && pair->value[1] == 40U);

    free(state);
    dualboy_session_unload(&session);
    return true;
}

static bool test_second_machine_failure_rolls_back_pair(void)
{
    struct dualboy_session session;
    struct fake_pair *pair;
    uint8_t *state;
    size_t state_size;
    char error[128] = {0};

    CHECK(make_session(&session));
    pair = session.pair;
    pair->value[0] = 9U;
    pair->value[1] = 8U;
    pair->battery[0] = 7U;
    pair->battery[1] = 6U;
    state_size = dualboy_state_size(&session);
    state = malloc(state_size);
    CHECK(state != NULL);
    CHECK(dualboy_state_serialize(&session, state, state_size, error,
                                  sizeof(error)));

    pair->value[0] = 1U;
    pair->value[1] = 2U;
    pair->battery[0] = 3U;
    pair->battery[1] = 4U;
    pair->fail_machine = 1;
    CHECK(!dualboy_state_unserialize(&session, state, state_size, error,
                                     sizeof(error)));
    CHECK(pair->value[0] == 1U && pair->value[1] == 2U);
    CHECK(pair->battery[0] == 3U && pair->battery[1] == 4U);

    free(state);
    dualboy_session_unload(&session);
    return true;
}

int main(void)
{
    if (!test_round_trip_and_battery_protection() ||
        !test_corruption_rejected_before_mutation() ||
        !test_second_machine_failure_rolls_back_pair()) {
        return EXIT_FAILURE;
    }
    puts("savestate tests passed");
    return EXIT_SUCCESS;
}
