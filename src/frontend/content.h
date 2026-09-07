/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_CONTENT_H
#define DUALBOY_CONTENT_H

#include "frontend/engine.h"

#include <stddef.h>
#include <stdint.h>

enum dualboy_detection_error {
    DUALBOY_DETECT_OK = 0,
    DUALBOY_DETECT_EMPTY,
    DUALBOY_DETECT_TOO_SMALL,
    DUALBOY_DETECT_UNKNOWN_HEADER,
};

struct dualboy_detection {
    enum dualboy_platform platform;
    enum dualboy_detection_error error;
    const char *message;
};

struct dualboy_detection dualboy_detect_content(const uint8_t *data, size_t size);

enum dualboy_engine_family
dualboy_engine_family_for_platform(enum dualboy_platform platform);

bool dualboy_platforms_compatible(enum dualboy_platform first,
                                  enum dualboy_platform second);

const char *dualboy_platform_name(enum dualboy_platform platform);

#endif
