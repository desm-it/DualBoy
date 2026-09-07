/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/session.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static char *duplicate_string(const char *value)
{
    size_t size;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    size = strlen(value) + 1U;
    copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

static bool own_rom(struct dualboy_owned_rom *destination,
                    const struct dualboy_rom *source,
                    const struct dualboy_owned_rom *share_with,
                    char *error,
                    size_t error_size)
{
    if (destination == NULL || source == NULL || source->data == NULL ||
        source->size == 0U) {
        set_error(error, error_size, "cannot own empty cartridge content");
        return false;
    }

    memset(destination, 0, sizeof(*destination));
    destination->rom.platform = source->platform;
    destination->rom.size = source->size;
    if (source->path != NULL) {
        destination->owned_path = duplicate_string(source->path);
        if (destination->owned_path == NULL) {
            set_error(error, error_size, "out of memory while copying content path");
            return false;
        }
        destination->rom.path = destination->owned_path;
    }

    if (share_with != NULL && share_with->rom.size == source->size &&
        share_with->rom.platform == source->platform &&
        share_with->rom.data != NULL && source->data == share_with->rom.data) {
        destination->rom.data = share_with->rom.data;
        return true;
    }

    destination->owned_data = malloc(source->size);
    if (destination->owned_data == NULL) {
        free(destination->owned_path);
        memset(destination, 0, sizeof(*destination));
        set_error(error, error_size, "out of memory while copying cartridge content");
        return false;
    }
    memcpy(destination->owned_data, source->data, source->size);
    destination->rom.data = destination->owned_data;
    return true;
}

static bool cache_state_capacities(struct dualboy_session *session,
                                   char *error,
                                   size_t error_size)
{
    const struct dualboy_engine_ops *ops = session->engine;
    unsigned machine;

    memset(session->machine_state_capacity, 0,
           sizeof(session->machine_state_capacity));
    memset(session->content_path_missing, 0,
           sizeof(session->content_path_missing));
    session->link_state_capacity = 0U;
    if (ops->machine_state_size == NULL || ops->serialize_machine == NULL ||
        ops->unserialize_machine == NULL || ops->link_state_size == NULL ||
        ops->serialize_link == NULL || ops->unserialize_link == NULL) {
        return true;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        session->machine_state_capacity[machine] =
            ops->machine_state_size(session->pair, machine);
        if (session->machine_state_capacity[machine] == 0U) {
            set_error(error, error_size,
                      "%s reported no savestate capacity for machine %u",
                      ops->name != NULL ? ops->name : "engine", machine + 1U);
            return false;
        }
    }
    session->link_state_capacity = ops->link_state_size(session->pair);
    return true;
}

void dualboy_session_init(struct dualboy_session *session)
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->display.mode = DUALBOY_MODE_DUAL;
    session->display.layout = DUALBOY_LAYOUT_SIDE_BY_SIDE;
}

void dualboy_session_unload(struct dualboy_session *session)
{
    unsigned machine;

    if (session == NULL) {
        return;
    }
    if (session->engine != NULL && session->engine->destroy_pair != NULL &&
        session->pair != NULL) {
        session->engine->destroy_pair(session->pair);
    }
    session->pair = NULL;
    session->engine = NULL;

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        free(session->roms[machine].owned_data);
        free(session->roms[machine].owned_path);
        memset(&session->roms[machine], 0, sizeof(session->roms[machine]));
    }
    free(session->composite_pixels);
    session->composite_pixels = NULL;
    memset(&session->composite, 0, sizeof(session->composite));
    memset(session->machine_state_capacity, 0,
           sizeof(session->machine_state_capacity));
    session->link_state_capacity = 0U;
    session->loaded = false;
    session->link_enabled = false;
    session->load_kind = DUALBOY_LOAD_NORMAL;
}

bool dualboy_session_load(struct dualboy_session *session,
                          const struct dualboy_engine_ops *engine,
                          const struct dualboy_engine_config *engine_config,
                          const struct dualboy_rom roms[DUALBOY_MACHINE_COUNT],
                          enum dualboy_load_kind load_kind,
                          bool link_enabled,
                          char *error,
                          size_t error_size)
{
    unsigned machine;

    if (session == NULL || engine == NULL || engine_config == NULL || roms == NULL ||
        engine->create_pair == NULL || engine->load_rom == NULL ||
        engine->destroy_pair == NULL) {
        set_error(error, error_size, "invalid engine/session load arguments");
        return false;
    }
    dualboy_session_unload(session);
    session->engine = engine;
    session->load_kind = load_kind;
    session->composite_pixels =
        calloc(DUALBOY_MAX_COMPOSITE_PIXELS, sizeof(*session->composite_pixels));
    if (session->composite_pixels == NULL) {
        set_error(error, error_size, "unable to allocate composite framebuffer");
        goto failure;
    }

    if (!own_rom(&session->roms[0], &roms[0], NULL, error, error_size)) {
        goto failure;
    }
    if (roms[1].data == roms[0].data && roms[1].size == roms[0].size &&
        roms[1].platform == roms[0].platform) {
        struct dualboy_rom shared = roms[1];
        shared.data = session->roms[0].rom.data;
        if (!own_rom(&session->roms[1], &shared, &session->roms[0], error,
                     error_size)) {
            goto failure;
        }
    } else if (!own_rom(&session->roms[1], &roms[1], NULL, error, error_size)) {
        goto failure;
    }

    if (!engine->create_pair(&session->pair, engine_config, error, error_size) ||
        session->pair == NULL) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            set_error(error, error_size, "%s failed to create an engine pair",
                      engine->name != NULL ? engine->name : "engine");
        }
        goto failure;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (!engine->load_rom(session->pair,
                              machine,
                              &session->roms[machine].rom,
                              error,
                              error_size)) {
            goto failure;
        }
    }
    if (engine->set_link != NULL &&
        !engine->set_link(session->pair, link_enabled, error, error_size)) {
        goto failure;
    }
    if (!cache_state_capacities(session, error, error_size)) {
        goto failure;
    }

    session->link_enabled = link_enabled;
    session->loaded = true;
    return true;

failure:
    dualboy_session_unload(session);
    return false;
}

void dualboy_session_reset(struct dualboy_session *session)
{
    if (session != NULL && session->loaded && session->engine->reset != NULL) {
        session->engine->reset(session->pair);
    }
}

bool dualboy_session_set_link(struct dualboy_session *session,
                              bool enabled,
                              char *error,
                              size_t error_size)
{
    if (session == NULL || !session->loaded || session->engine == NULL ||
        session->engine->set_link == NULL) {
        set_error(error, error_size, "no active session supports link changes");
        return false;
    }
    if (!session->engine->set_link(session->pair, enabled, error, error_size)) {
        return false;
    }
    session->link_enabled = enabled;
    return true;
}

bool dualboy_session_run(struct dualboy_session *session,
                         const uint16_t port_buttons[DUALBOY_MACHINE_COUNT],
                         const struct dualboy_compositor_config *display,
                         char *error,
                         size_t error_size)
{
    struct dualboy_video_frame frames[DUALBOY_MACHINE_COUNT];
    unsigned port;

    if (session == NULL || !session->loaded || session->engine == NULL ||
        port_buttons == NULL || display == NULL ||
        session->engine->set_input == NULL || session->engine->run_frame == NULL ||
        session->engine->video_frame == NULL) {
        set_error(error, error_size, "cannot run an incomplete DualBoy session");
        return false;
    }

    for (port = 0U; port < DUALBOY_MACHINE_COUNT; ++port) {
        const unsigned machine =
            dualboy_machine_for_port(port, display->swap_players);
        session->engine->set_input(session->pair, machine, port_buttons[port]);
    }
    if (!session->engine->run_frame(session->pair, error, error_size)) {
        return false;
    }
    if (!session->engine->video_frame(session->pair, 0U, &frames[0]) ||
        !session->engine->video_frame(session->pair, 1U, &frames[1])) {
        set_error(error, error_size, "engine did not produce both video frames");
        return false;
    }
    if (!dualboy_compose_frame(session->composite_pixels,
                               DUALBOY_MAX_COMPOSITE_PIXELS,
                               frames,
                               display,
                               &session->composite)) {
        set_error(error, error_size, "unable to compose engine video frames");
        return false;
    }
    session->display = *display;
    return true;
}

bool dualboy_session_geometry(struct dualboy_session *session,
                              const struct dualboy_compositor_config *display,
                              struct dualboy_geometry *geometry)
{
    struct dualboy_video_frame frames[DUALBOY_MACHINE_COUNT];

    if (session == NULL || !session->loaded || display == NULL || geometry == NULL ||
        !session->engine->video_frame(session->pair, 0U, &frames[0]) ||
        !session->engine->video_frame(session->pair, 1U, &frames[1])) {
        return false;
    }
    return dualboy_compositor_geometry(frames, display, geometry);
}
