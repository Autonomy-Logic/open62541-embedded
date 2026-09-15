/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * The _POSIX-named factories that plugins/ua_config_default.c references
 * unconditionally, even under UA_ARCHITECTURE=none.
 *
 * Upstream's porting guide offers two ways out: rename your implementations to
 * match, or assemble the config by hand instead of using
 * UA_ServerConfig_setMinimal. We take the first, because the second duplicates a
 * few hundred lines of upstream config setup that would need re-checking on
 * every version bump. The implementations are named *_Arduino and the aliases
 * are contained here.
 */

#include "arduino_internal.h"

extern "C" UA_EventLoop* UA_EventLoop_new_POSIX(const UA_Logger* logger)
{
    return UA_EventLoop_new_Arduino(logger);
}

extern "C" UA_ConnectionManager* UA_ConnectionManager_new_POSIX_TCP(const UA_String eventSourceName)
{
    return UA_ConnectionManager_new_Arduino_TCP(eventSourceName);
}

extern "C" UA_ConnectionManager* UA_ConnectionManager_new_POSIX_UDP(const UA_String eventSourceName)
{
    // No UDP transport. OPC-UA's mandatory profile is UA-TCP; UDP carries only
    // PubSub and multicast discovery, both compiled out of this build.
    // Returning null is honest -- a config that asks for UDP then fails at
    // setup rather than at the first datagram.
    (void)eventSourceName;
    return nullptr;
}

extern "C" UA_InterruptManager* UA_InterruptManager_new_POSIX(const UA_String eventSourceName)
{
    // Only PubSub's realtime paths register interrupts, and PubSub is off.
    (void)eventSourceName;
    return nullptr;
}
