/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/* Declarations shared between this port's translation units. Not installed
 * for sketches: nothing here is API. */

#ifndef UA_ARDUINO_INTERNAL_H
#define UA_ARDUINO_INTERNAL_H

#include "open62541_arduino.h"

/** Service one ConnectionManager: drain readable clients, reap closed ones.
 *  Called by the EventLoop's run() for each registered connection manager, so
 *  the CM needs no timer of its own. */
void ua_arduino_cm_poll(UA_ConnectionManager* cm);

/** Point open62541's allocator singletons at our arena. Idempotent.
 *
 *  A static constructor in arena_arduino.cpp calls this so it is in place
 *  before setup(), but static initialisation order across translation units is
 *  unspecified, so every entry point that could be the first to allocate calls
 *  it too. Cheap: four pointer stores and a bool. */
void ua_arduino_install_allocator(void);

#endif /* UA_ARDUINO_INTERNAL_H */
