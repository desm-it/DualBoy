/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_STATE_H
#define DUALBOY_STATE_H

#include "frontend/session.h"

#include <stdbool.h>
#include <stddef.h>

#define DUALBOY_STATE_HEADER_BYTES 112U

/*
 * The returned size is a stable upper bound as long as the loaded session is
 * unchanged. Serialized data is padded to that bound so Libretro rewind and
 * runahead callers can retain one fixed allocation.
 */
size_t dualboy_state_size(const struct dualboy_session *session);

bool dualboy_state_serialize(struct dualboy_session *session,
                             void *data,
                             size_t size,
                             char *error,
                             size_t error_size);

/*
 * Restore is transactional across both machines and the link scheduler.
 * Cartridge RAM and RTC are restored to their pre-call contents after engine
 * state application, keeping battery persistence independent from savestates.
 */
bool dualboy_state_unserialize(struct dualboy_session *session,
                               const void *data,
                               size_t size,
                               char *error,
                               size_t error_size);

#endif
