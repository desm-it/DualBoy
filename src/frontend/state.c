/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/state.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUALBOY_STATE_HEADER_SIZE DUALBOY_STATE_HEADER_BYTES
#define DUALBOY_STATE_VERSION 1U
#define DUALBOY_STATE_FLAG_LINK_ENABLED (1U << 0)

static const uint8_t state_magic[8] = {'D', 'U', 'A', 'L', 'B', 'S', 'T', '\0'};

struct state_parts {
    uint8_t *machines[DUALBOY_MACHINE_COUNT];
    size_t machine_capacity[DUALBOY_MACHINE_COUNT];
    size_t machine_used[DUALBOY_MACHINE_COUNT];
    uint8_t *link;
    size_t link_capacity;
    size_t link_used;
};

struct battery_copy {
    void *destination;
    uint8_t *bytes;
    size_t size;
};

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

static void put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t *destination, uint64_t value)
{
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
}

static uint64_t get_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)source[index] << (index * 8U);
    }
    return value;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned bit;

        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static bool checked_add(size_t first, size_t second, size_t *result)
{
    if (result == NULL || first > SIZE_MAX - second) {
        return false;
    }
    *result = first + second;
    return true;
}

static bool state_ops_valid(const struct dualboy_session *session)
{
    const struct dualboy_engine_ops *ops;

    if (session == NULL || !session->loaded || session->pair == NULL ||
        session->engine == NULL) {
        return false;
    }
    ops = session->engine;
    return ops->machine_state_size != NULL && ops->serialize_machine != NULL &&
           ops->unserialize_machine != NULL && ops->link_state_size != NULL &&
           ops->serialize_link != NULL && ops->unserialize_link != NULL &&
           session->machine_state_capacity[0] > 0U &&
           session->machine_state_capacity[1] > 0U;
}

size_t dualboy_state_size(const struct dualboy_session *session)
{
    size_t total = DUALBOY_STATE_HEADER_SIZE;
    unsigned machine;

    if (!state_ops_valid(session)) {
        return 0U;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        const size_t size = session->machine_state_capacity[machine];
        if (size == 0U || !checked_add(total, size, &total)) {
            return 0U;
        }
    }
    if (!checked_add(total,
                     session->link_state_capacity,
                     &total)) {
        return 0U;
    }
    return total;
}

static void free_state_parts(struct state_parts *parts)
{
    unsigned machine;

    if (parts == NULL) {
        return;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        free(parts->machines[machine]);
    }
    free(parts->link);
    memset(parts, 0, sizeof(*parts));
}

static bool snapshot_state(struct dualboy_session *session,
                           struct state_parts *parts,
                           char *error,
                           size_t error_size)
{
    unsigned machine;

    memset(parts, 0, sizeof(*parts));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        parts->machine_capacity[machine] =
            session->machine_state_capacity[machine];
        if (parts->machine_capacity[machine] == 0U) {
            set_error(error, error_size, "engine reported an empty machine state");
            goto failure;
        }
        parts->machines[machine] = malloc(parts->machine_capacity[machine]);
        if (parts->machines[machine] == NULL ||
            !session->engine->serialize_machine(
                session->pair,
                machine,
                parts->machines[machine],
                parts->machine_capacity[machine],
                &parts->machine_used[machine]) ||
            parts->machine_used[machine] > parts->machine_capacity[machine]) {
            set_error(error, error_size, "failed to snapshot machine %u", machine + 1U);
            goto failure;
        }
    }

    parts->link_capacity = session->link_state_capacity;
    if (parts->link_capacity > 0U) {
        parts->link = malloc(parts->link_capacity);
        if (parts->link == NULL) {
            set_error(error, error_size, "out of memory while snapshotting link state");
            goto failure;
        }
    }
    if (!session->engine->serialize_link(session->pair,
                                         parts->link,
                                         parts->link_capacity,
                                         &parts->link_used) ||
        parts->link_used > parts->link_capacity) {
        set_error(error, error_size, "failed to snapshot link state");
        goto failure;
    }
    return true;

failure:
    free_state_parts(parts);
    return false;
}

static bool copy_battery(struct dualboy_session *session,
                         struct battery_copy copies[DUALBOY_MACHINE_COUNT * 2U],
                         char *error,
                         size_t error_size)
{
    unsigned machine;
    unsigned kind;

    memset(copies, 0, sizeof(*copies) * DUALBOY_MACHINE_COUNT * 2U);
    if (session->engine->memory_info == NULL) {
        return true;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        for (kind = 0U; kind < 2U; ++kind) {
            struct battery_copy *copy = &copies[machine * 2U + kind];
            void *data = NULL;
            size_t size = 0U;

            if (!session->engine->memory_info(session->pair,
                                              machine,
                                              (enum dualboy_memory_kind)kind,
                                              &data,
                                              &size) ||
                size == 0U) {
                continue;
            }
            if (data == NULL) {
                set_error(error, error_size,
                          "engine exposed null battery memory for machine %u",
                          machine + 1U);
                return false;
            }
            copy->bytes = malloc(size);
            if (copy->bytes == NULL) {
                set_error(error, error_size,
                          "out of memory while protecting battery state");
                return false;
            }
            memcpy(copy->bytes, data, size);
            copy->destination = data;
            copy->size = size;
        }
    }
    return true;
}

static void restore_and_free_battery(
    struct battery_copy copies[DUALBOY_MACHINE_COUNT * 2U])
{
    unsigned index;

    for (index = 0U; index < DUALBOY_MACHINE_COUNT * 2U; ++index) {
        if (copies[index].destination != NULL && copies[index].bytes != NULL) {
            memcpy(copies[index].destination, copies[index].bytes, copies[index].size);
        }
        free(copies[index].bytes);
        memset(&copies[index], 0, sizeof(copies[index]));
    }
}

bool dualboy_state_serialize(struct dualboy_session *session,
                             void *data,
                             size_t size,
                             char *error,
                             size_t error_size)
{
    uint8_t *bytes = data;
    size_t capacities[DUALBOY_MACHINE_COUNT];
    size_t used[DUALBOY_MACHINE_COUNT];
    size_t link_capacity;
    size_t link_used;
    size_t offset = DUALBOY_STATE_HEADER_SIZE;
    size_t total_used;
    unsigned machine;

    if (!state_ops_valid(session) || data == NULL ||
        size < dualboy_state_size(session)) {
        set_error(error, error_size, "savestate buffer is too small or no game is loaded");
        return false;
    }
    memset(bytes, 0, size);

    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        capacities[machine] = session->machine_state_capacity[machine];
        if (!session->engine->serialize_machine(session->pair,
                                                machine,
                                                bytes + offset,
                                                capacities[machine],
                                                &used[machine]) ||
            used[machine] > capacities[machine]) {
            set_error(error, error_size, "failed to serialize machine %u", machine + 1U);
            return false;
        }
        offset += used[machine];
    }
    link_capacity = session->link_state_capacity;
    if (!session->engine->serialize_link(session->pair,
                                         bytes + offset,
                                         link_capacity,
                                         &link_used) ||
        link_used > link_capacity) {
        set_error(error, error_size, "failed to serialize link state");
        return false;
    }
    if (!checked_add(offset, link_used, &total_used)) {
        set_error(error, error_size, "savestate size overflow");
        return false;
    }

    memcpy(bytes, state_magic, sizeof(state_magic));
    put_u32(bytes + 8U, DUALBOY_STATE_VERSION);
    put_u32(bytes + 12U, DUALBOY_STATE_HEADER_SIZE);
    put_u64(bytes + 16U, (uint64_t)total_used);
    put_u32(bytes + 24U, (uint32_t)session->engine->family);
    put_u32(bytes + 28U, (uint32_t)session->roms[0].rom.platform);
    put_u32(bytes + 32U, (uint32_t)session->roms[1].rom.platform);
    put_u32(bytes + 36U,
            session->link_enabled ? DUALBOY_STATE_FLAG_LINK_ENABLED : 0U);
    put_u64(bytes + 40U, (uint64_t)session->roms[0].rom.size);
    put_u64(bytes + 48U, (uint64_t)session->roms[1].rom.size);
    put_u32(bytes + 56U,
            crc32_bytes(session->roms[0].rom.data, session->roms[0].rom.size));
    put_u32(bytes + 60U,
            crc32_bytes(session->roms[1].rom.data, session->roms[1].rom.size));
    put_u64(bytes + 64U, (uint64_t)used[0]);
    put_u64(bytes + 72U, (uint64_t)used[1]);
    put_u64(bytes + 80U, (uint64_t)link_used);
    put_u32(bytes + 88U,
            crc32_bytes(bytes + DUALBOY_STATE_HEADER_SIZE, used[0]));
    put_u32(bytes + 92U,
            crc32_bytes(bytes + DUALBOY_STATE_HEADER_SIZE + used[0], used[1]));
    put_u32(bytes + 96U,
            crc32_bytes(bytes + DUALBOY_STATE_HEADER_SIZE + used[0] + used[1],
                        link_used));
    put_u32(bytes + 108U, crc32_bytes(bytes, 108U));
    return true;
}

static bool lengths_valid(uint64_t first,
                          uint64_t second,
                          uint64_t link,
                          size_t input_size,
                          size_t *total)
{
    size_t result = DUALBOY_STATE_HEADER_SIZE;

    if (first > SIZE_MAX || second > SIZE_MAX || link > SIZE_MAX ||
        !checked_add(result, (size_t)first, &result) ||
        !checked_add(result, (size_t)second, &result) ||
        !checked_add(result, (size_t)link, &result) || result > input_size) {
        return false;
    }
    *total = result;
    return true;
}

static bool rollback_state(struct dualboy_session *session,
                           const struct state_parts *snapshot,
                           bool link_enabled)
{
    bool success = true;
    unsigned machine;
    char ignored[1] = {0};

    if (!session->engine->set_link(session->pair, link_enabled, ignored,
                                   sizeof(ignored))) {
        success = false;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (!session->engine->unserialize_machine(
                session->pair,
                machine,
                snapshot->machines[machine],
                snapshot->machine_used[machine])) {
            success = false;
        }
    }
    if (!session->engine->unserialize_link(session->pair,
                                           snapshot->link,
                                           snapshot->link_used)) {
        success = false;
    }
    session->link_enabled = link_enabled;
    return success;
}

enum dualboy_state_restore_result
dualboy_state_unserialize_ex(struct dualboy_session *session,
                             const void *data,
                             size_t size,
                             char *error,
                             size_t error_size)
{
    const uint8_t *bytes = data;
    const uint8_t *payload;
    uint64_t lengths64[3];
    size_t lengths[3];
    size_t total;
    size_t offset;
    uint32_t flags;
    bool saved_link_enabled;
    bool old_link_enabled;
    bool applied = false;
    bool rollback_ok = true;
    struct state_parts snapshot;
    struct battery_copy battery[DUALBOY_MACHINE_COUNT * 2U];
    unsigned machine;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(battery, 0, sizeof(battery));
    if (!state_ops_valid(session) || data == NULL ||
        size < DUALBOY_STATE_HEADER_SIZE) {
        set_error(error, error_size, "savestate is absent, truncated, or no game is loaded");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    if (memcmp(bytes, state_magic, sizeof(state_magic)) != 0 ||
        get_u32(bytes + 8U) != DUALBOY_STATE_VERSION ||
        get_u32(bytes + 12U) != DUALBOY_STATE_HEADER_SIZE ||
        get_u32(bytes + 108U) != crc32_bytes(bytes, 108U)) {
        set_error(error, error_size, "savestate header is corrupt or unsupported");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    if (get_u32(bytes + 24U) != (uint32_t)session->engine->family ||
        get_u32(bytes + 28U) != (uint32_t)session->roms[0].rom.platform ||
        get_u32(bytes + 32U) != (uint32_t)session->roms[1].rom.platform) {
        set_error(error, error_size, "savestate engine or platform does not match");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    if (get_u64(bytes + 40U) != (uint64_t)session->roms[0].rom.size ||
        get_u64(bytes + 48U) != (uint64_t)session->roms[1].rom.size ||
        get_u32(bytes + 56U) !=
            crc32_bytes(session->roms[0].rom.data, session->roms[0].rom.size) ||
        get_u32(bytes + 60U) !=
            crc32_bytes(session->roms[1].rom.data, session->roms[1].rom.size)) {
        set_error(error, error_size, "savestate cartridges do not match this session");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    flags = get_u32(bytes + 36U);
    if ((flags & ~DUALBOY_STATE_FLAG_LINK_ENABLED) != 0U) {
        set_error(error, error_size, "savestate uses unknown feature flags");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    lengths64[0] = get_u64(bytes + 64U);
    lengths64[1] = get_u64(bytes + 72U);
    lengths64[2] = get_u64(bytes + 80U);
    if (!lengths_valid(lengths64[0], lengths64[1], lengths64[2], size, &total) ||
        get_u64(bytes + 16U) != (uint64_t)total) {
        set_error(error, error_size, "savestate lengths are invalid");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        lengths[machine] = (size_t)lengths64[machine];
        if (lengths[machine] == 0U ||
            lengths[machine] > session->machine_state_capacity[machine]) {
            set_error(error, error_size, "savestate machine payload is invalid");
            return DUALBOY_STATE_RESTORE_REJECTED;
        }
    }
    lengths[2] = (size_t)lengths64[2];
    if (lengths[2] > session->link_state_capacity) {
        set_error(error, error_size, "savestate link payload is invalid");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }
    payload = bytes + DUALBOY_STATE_HEADER_SIZE;
    if (get_u32(bytes + 88U) != crc32_bytes(payload, lengths[0]) ||
        get_u32(bytes + 92U) !=
            crc32_bytes(payload + lengths[0], lengths[1]) ||
        get_u32(bytes + 96U) !=
            crc32_bytes(payload + lengths[0] + lengths[1], lengths[2])) {
        set_error(error, error_size, "savestate payload checksum failed");
        return DUALBOY_STATE_RESTORE_REJECTED;
    }

    if (!snapshot_state(session, &snapshot, error, error_size) ||
        !copy_battery(session, battery, error, error_size)) {
        restore_and_free_battery(battery);
        free_state_parts(&snapshot);
        return DUALBOY_STATE_RESTORE_REJECTED;
    }

    old_link_enabled = session->link_enabled;
    saved_link_enabled = (flags & DUALBOY_STATE_FLAG_LINK_ENABLED) != 0U;
    if (!session->engine->set_link(session->pair, saved_link_enabled, error,
                                   error_size)) {
        goto failure;
    }
    offset = 0U;
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        if (!session->engine->unserialize_machine(session->pair,
                                                  machine,
                                                  payload + offset,
                                                  lengths[machine])) {
            set_error(error, error_size, "failed to restore machine %u", machine + 1U);
            goto failure;
        }
        offset += lengths[machine];
    }
    if (!session->engine->unserialize_link(session->pair,
                                           payload + offset,
                                           lengths[2])) {
        set_error(error, error_size, "failed to restore link state");
        goto failure;
    }
    session->link_enabled = saved_link_enabled;
    applied = true;

failure:
    if (!applied) {
        rollback_ok = rollback_state(session, &snapshot, old_link_enabled);
    }
    restore_and_free_battery(battery);
    free_state_parts(&snapshot);
    if (!applied && !rollback_ok) {
        set_error(error, error_size,
                  "savestate restore failed and engine rollback was incomplete");
        return DUALBOY_STATE_RESTORE_FATAL;
    }
    return applied ? DUALBOY_STATE_RESTORE_OK
                   : DUALBOY_STATE_RESTORE_REJECTED;
}

bool dualboy_state_unserialize(struct dualboy_session *session,
                               const void *data,
                               size_t size,
                               char *error,
                               size_t error_size)
{
    return dualboy_state_unserialize_ex(session, data, size, error,
                                        error_size) ==
           DUALBOY_STATE_RESTORE_OK;
}
