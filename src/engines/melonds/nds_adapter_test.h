/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DUALBOY_MELONDS_ADAPTER_TEST_H
#define DUALBOY_MELONDS_ADAPTER_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostics and deterministic controls used by generated-content tests. */
unsigned dualboy_melonds_debug_live_instances(const void *pair);
const void *dualboy_melonds_debug_userdata(const void *pair, unsigned machine);
const void *dualboy_melonds_debug_nds_object(const void *pair,
                                             unsigned machine);
uint8_t dualboy_melonds_debug_registration_mask(const void *pair);
uint64_t dualboy_melonds_debug_completed_frames(const void *pair,
                                                unsigned machine);
bool dualboy_melonds_debug_last_touch(const void *pair,
                                     unsigned machine,
                                     bool *active,
                                     uint16_t *x,
                                     uint16_t *y);
bool dualboy_melonds_debug_tsc_touch(void *pair,
                                    unsigned machine,
                                    uint16_t *x,
                                    uint16_t *y);
bool dualboy_melonds_debug_last_buttons(const void *pair,
                                       unsigned machine,
                                       uint16_t *buttons);
bool dualboy_melonds_debug_firmware_mac(const void *pair,
                                       unsigned machine,
                                       uint8_t mac[6]);
bool dualboy_melonds_debug_set_firmware_mac(void *pair,
                                           unsigned machine,
                                           const uint8_t mac[6]);
bool dualboy_melonds_debug_stop_machine(void *pair, unsigned machine);
bool dualboy_melonds_debug_write_cart_save(void *pair,
                                          unsigned machine,
                                          uint32_t address,
                                          uint8_t value);
bool dualboy_melonds_debug_set_frame_deadline_ms(void *pair,
                                                uint32_t milliseconds);
bool dualboy_melonds_debug_fail_next_gl_renderer(void *pair,
                                                unsigned machine);
bool dualboy_melonds_debug_uses_opengl(const void *pair);
bool dualboy_melonds_debug_link_enabled(const void *pair);
bool dualboy_melonds_debug_hold_next_frame(void *pair, unsigned machine);
bool dualboy_melonds_debug_wait_for_frame_timeout(void *pair,
                                                 uint32_t timeout_ms);
bool dualboy_melonds_debug_release_frame_hold(void *pair, unsigned machine);
bool dualboy_melonds_debug_is_frame_poisoned(const void *pair);
bool dualboy_melonds_debug_set_mp_requested(void *pair,
                                           unsigned machine,
                                           bool requested);
bool dualboy_melonds_debug_mp_round_trip(void *pair,
                                        uint64_t timestamp,
                                        const uint8_t *sent,
                                        size_t length,
                                        uint8_t *received,
                                        uint64_t *received_timestamp);

#ifdef __cplusplus
}
#endif

#endif
