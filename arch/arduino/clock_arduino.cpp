/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * The three clock functions open62541/types.h declares and UA_ARCHITECTURE=none
 * leaves for the integrator.
 */

#include "open62541_arduino.h"

namespace {

/** 100 ns ticks between 1601-01-01 (the OPC-UA epoch) and the Unix epoch. */
constexpr int64_t kUnixEpochIn1601Ticks = 116444736000000000LL;

/** Wall-clock base, as Unix seconds, and the uptime at which it was set.
 *  UA_Arduino_setTime() moves the pair; nothing else touches it. */
int64_t  g_epoch_unix      = UA_ARDUINO_EPOCH_UNIX;
uint64_t g_epoch_at_micros = 0;

/** Monotonic microseconds, immune to the 32-bit micros() wrap.
 *
 *  micros() wraps every ~71.6 minutes. A server that has been up for longer
 *  than that -- which is every server -- would otherwise see time jump
 *  backwards, and open62541 schedules its housekeeping off this clock: a
 *  backwards jump stops SecureChannel reaping until the wrap is undone.
 *
 *  Not thread-safe, and does not need to be: everything here runs from the
 *  sketch's single thread. It does need to be called more often than once per
 *  wrap period, which the EventLoop's run() guarantees. */
uint64_t monotonic_micros()
{
    static uint32_t last  = 0;
    static uint64_t high  = 0;
    const uint32_t now = micros();
    if (now < last)
        high += 0x100000000ULL;
    last = now;
    return high + now;
}

} // namespace

extern "C" UA_DateTime UA_DateTime_nowMonotonic(void)
{
    return (UA_DateTime)(monotonic_micros() * 10);   // us -> 100 ns ticks
}

extern "C" UA_DateTime UA_DateTime_now(void)
{
    const uint64_t since = monotonic_micros() - g_epoch_at_micros;
    return (UA_DateTime)(kUnixEpochIn1601Ticks + g_epoch_unix * 10000000LL
                         + (int64_t)(since * 10));
}

/** Zero, always.
 *
 *  This is the offset of local time from UTC, and a board with no timezone
 *  database has no honest answer other than "I am UTC". Reporting a guess
 *  would put a wrong offset on every timestamp; reporting zero puts the
 *  server in UTC, which is what OPC-UA wants anyway. */
extern "C" UA_Int64 UA_DateTime_localTimeUtcOffset(void)
{
    return 0;
}

extern "C" void UA_Arduino_setTime(int64_t unixSeconds)
{
    g_epoch_unix      = unixSeconds;
    g_epoch_at_micros = monotonic_micros();
}
