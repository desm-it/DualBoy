/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DUALBOY_MGBA_ADAPTER_INTERNAL_H
#define DUALBOY_MGBA_ADAPTER_INTERNAL_H

#include <stdbool.h>

#define DUALBOY_MGBA_LOCKSTEP_QUEUE_CAPACITY 64

struct dualboy_mgba_lockstep_diagnostics {
    unsigned max_queue_depth;
    unsigned dropped_events;
};

/* Test/diagnostic surface; not part of the Libretro ABI. */
bool dualboy_mgba_get_lockstep_diagnostics(
    const void *pair,
    struct dualboy_mgba_lockstep_diagnostics *diagnostics);

#endif
