/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_SAVE_MANAGER_H
#define DUALBOY_SAVE_MANAGER_H

#include "frontend/persistence.h"
#include "frontend/session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUALBOY_SAVE_FLUSH_INTERVAL_FRAMES 300U
#define DUALBOY_MAX_SAVE_LOCKS \
    (DUALBOY_MACHINE_COUNT * DUALBOY_MEMORY_KIND_COUNT)

struct dualboy_save_region_tracking {
    uint8_t *baseline;
    size_t baseline_size;
    uint64_t hash;
    size_t size;
    size_t loaded_size;
    size_t preserved_size;
    bool extent_known;
    bool valid;
};

struct dualboy_save_manager {
    struct dualboy_save_paths paths;
    bool core_managed[DUALBOY_MACHINE_COUNT][DUALBOY_MEMORY_KIND_COUNT];
    struct dualboy_save_region_tracking
        tracked[DUALBOY_MACHINE_COUNT][DUALBOY_MEMORY_KIND_COUNT];
    uint64_t frames_since_flush;
    int write_lock_fds[DUALBOY_MAX_SAVE_LOCKS];
    size_t write_lock_count;
    bool write_owner;
    bool initialized;
};

/*
 * Selects safe persistence ownership and loads every core-managed region.
 * SameBoy uses frontend-managed memory where each content slot has a unique
 * identity. mGBA and M3U use core-managed files because their save storage is
 * not a stable one-file Libretro memory region. melonDS uses core-managed
 * cartridge SaveRAM and writable firmware; firmware is never frontend-managed.
 */
bool dualboy_save_manager_init(struct dualboy_save_manager *manager,
                               struct dualboy_session *session,
                               const char *save_directory,
                               char *error,
                               size_t error_size);

bool dualboy_save_manager_frontend_memory(
    const struct dualboy_save_manager *manager,
    struct dualboy_session *session,
    unsigned machine,
    enum dualboy_memory_kind kind,
    void **data,
    size_t *size);

/* Flushes changed core-managed regions every five seconds at 60 fps. */
bool dualboy_save_manager_tick(struct dualboy_save_manager *manager,
                               struct dualboy_session *session,
                               char *error,
                               size_t error_size);

/* A forced flush is intended for clean unload/deinitialization. */
bool dualboy_save_manager_flush(struct dualboy_save_manager *manager,
                                struct dualboy_session *session,
                                bool force,
                                char *error,
                                size_t error_size);

void dualboy_save_manager_deinit(struct dualboy_save_manager *manager);

#endif
