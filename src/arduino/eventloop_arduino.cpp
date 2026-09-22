/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * A cooperative EventLoop: no threads, no select(), no sleeping. run() does
 * one pass of due timers, one poll of each connection manager, and returns.
 *
 * The sketch's loop() is the scheduler. That is the whole design, and it is
 * what lets an OPC-UA server share a microcontroller with code that has its
 * own timing to keep.
 */

#include "arduino_internal.h"

namespace {

struct Timer
{
    UA_UInt64      id;
    UA_Callback    cb;
    void*          application;
    void*          data;
    UA_DateTime    interval;   // 100 ns ticks
    UA_DateTime    next;
    UA_TimerPolicy policy;
    bool           active;
};

struct ArduinoEventLoop
{
    UA_EventLoop        base;      // MUST be first: open62541 casts between them
    Timer               timers[UA_ARDUINO_MAX_TIMERS];
    UA_UInt64           next_id;
    UA_DelayedCallback* delayed;
};

ArduinoEventLoop* self(UA_EventLoop* el) { return reinterpret_cast<ArduinoEventLoop*>(el); }

/** The state member is `const volatile` so open62541's callers cannot write
 *  it; the owner still has to. */
void set_state(UA_EventLoop* el, UA_EventLoopState s)
{
    *const_cast<UA_EventLoopState*>(&el->state) = s;
}

UA_StatusCode el_start(UA_EventLoop* el)
{
    if (el->state != UA_EVENTLOOPSTATE_FRESH && el->state != UA_EVENTLOOPSTATE_STOPPED)
        return UA_STATUSCODE_GOOD;
    for (UA_EventSource* es = el->eventSources; es != nullptr; es = es->next)
        es->start(es);
    set_state(el, UA_EVENTLOOPSTATE_STARTED);
    return UA_STATUSCODE_GOOD;
}

void el_stop(UA_EventLoop* el)
{
    for (UA_EventSource* es = el->eventSources; es != nullptr; es = es->next)
        es->stop(es);
    set_state(el, UA_EVENTLOOPSTATE_STOPPED);
}

UA_StatusCode el_free(UA_EventLoop* el)
{
    if (el->state != UA_EVENTLOOPSTATE_STOPPED && el->state != UA_EVENTLOOPSTATE_FRESH)
        return UA_STATUSCODE_BADINTERNALERROR;
    while (el->eventSources != nullptr)
    {
        UA_EventSource* es = el->eventSources;
        el->eventSources = es->next;
        es->free(es);
    }
    UA_free(el);
    return UA_STATUSCODE_GOOD;
}

void run_due_timers(ArduinoEventLoop* l)
{
    const UA_DateTime now = UA_DateTime_nowMonotonic();
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_TIMERS; i++)
    {
        Timer& t = l->timers[i];
        if (!t.active || t.next > now)
            continue;

        // Re-arm BEFORE the callback: open62541 callbacks can remove their own
        // timer, and writing t.next afterwards would resurrect a dead slot.
        if (t.policy == UA_TIMERPOLICY_CURRENTTIME)
            t.next = now + t.interval;
        else
        {
            // ONCE_PER_INTERVAL: keep the original phase. If we have fallen
            // more than one interval behind -- a long scan, a blocking
            // sketch -- skip the missed ticks rather than firing a burst to
            // catch up, which is never what housekeeping wants.
            t.next += t.interval;
            if (t.next <= now)
                t.next = now + t.interval;
        }

        UA_Callback cb = t.cb;
        void* app = t.application;
        void* dat = t.data;
        if (cb != nullptr)
            cb(app, dat);
    }
}

void drain_delayed(ArduinoEventLoop* l)
{
    // Snapshot first: a delayed callback may enqueue another, and draining a
    // list while it grows is an unbounded loop inside one iteration.
    UA_DelayedCallback* dc = l->delayed;
    l->delayed = nullptr;
    while (dc != nullptr)
    {
        UA_DelayedCallback* next = dc->next;
        if (dc->callback != nullptr)
            dc->callback(dc->application, dc->context);
        dc = next;
    }
}

/** Non-blocking, always -- `timeout` is ignored.
 *
 *  This is called from the sketch's loop(), where the sketch's own timing is
 *  the contract with whatever else it is doing. Honouring a timeout would mean
 *  sleeping with everything else stopped. Pending work is picked up on the
 *  next iteration; request/response over TCP tolerates that. */
UA_StatusCode el_run(UA_EventLoop* el, UA_UInt32 timeout)
{
    (void)timeout;
    if (el->state != UA_EVENTLOOPSTATE_STARTED)
        return UA_STATUSCODE_BADINTERNALERROR;
    ArduinoEventLoop* l = self(el);
    run_due_timers(l);
    for (UA_EventSource* es = el->eventSources; es != nullptr; es = es->next)
    {
        if (es->eventSourceType == UA_EVENTSOURCETYPE_CONNECTIONMANAGER)
        {
            UA_ConnectionManager* cm = reinterpret_cast<UA_ConnectionManager*>(es);
            if (cm->eventSource.state == UA_EVENTSOURCESTATE_STARTED)
                ua_arduino_cm_poll(cm);
        }
    }
    drain_delayed(l);
    return UA_STATUSCODE_GOOD;
}

void el_cancel(UA_EventLoop* el) { (void)el; }

UA_DateTime el_now(UA_EventLoop* el)           { (void)el; return UA_DateTime_now(); }
UA_DateTime el_now_monotonic(UA_EventLoop* el) { (void)el; return UA_DateTime_nowMonotonic(); }
UA_Int64    el_utc_offset(UA_EventLoop* el)    { (void)el; return 0; }

UA_DateTime el_next_timer(UA_EventLoop* el)
{
    ArduinoEventLoop* l = self(el);
    UA_DateTime soonest = UA_INT64_MAX;
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_TIMERS; i++)
    {
        if (l->timers[i].active && l->timers[i].next < soonest)
            soonest = l->timers[i].next;
    }
    return soonest;
}

UA_StatusCode el_add_timer(UA_EventLoop* el, UA_Callback cb, void* application, void* data,
                           UA_Double interval_ms, UA_DateTime* baseTime,
                           UA_TimerPolicy policy, UA_UInt64* timerId)
{
    ArduinoEventLoop* l = self(el);
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_TIMERS; i++)
    {
        Timer& t = l->timers[i];
        if (t.active)
            continue;
        t.id          = ++l->next_id;
        t.cb          = cb;
        t.application = application;
        t.data        = data;
        t.interval    = (UA_DateTime)(interval_ms * (UA_Double)UA_DATETIME_MSEC);
        t.policy      = policy;
        t.next        = (baseTime != nullptr ? *baseTime : UA_DateTime_nowMonotonic()) + t.interval;
        t.active      = true;
        if (timerId != nullptr)
            *timerId = t.id;
        return UA_STATUSCODE_GOOD;
    }
    // Bounded by design. Refusing loudly beats silently not running a callback
    // the server believes is scheduled -- raise UA_ARDUINO_MAX_TIMERS.
    UA_LOG_ERROR(el->logger, UA_LOGCATEGORY_EVENTLOOP,
                 "Arduino EventLoop: timer table full (%u), callback refused. "
                 "Raise UA_ARDUINO_MAX_TIMERS.", (unsigned)UA_ARDUINO_MAX_TIMERS);
    return UA_STATUSCODE_BADOUTOFMEMORY;
}

UA_StatusCode el_modify_timer(UA_EventLoop* el, UA_UInt64 timerId, UA_Double interval_ms,
                              UA_DateTime* baseTime, UA_TimerPolicy policy)
{
    ArduinoEventLoop* l = self(el);
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_TIMERS; i++)
    {
        Timer& t = l->timers[i];
        if (!t.active || t.id != timerId)
            continue;
        t.interval = (UA_DateTime)(interval_ms * (UA_Double)UA_DATETIME_MSEC);
        t.policy   = policy;
        t.next     = (baseTime != nullptr ? *baseTime : UA_DateTime_nowMonotonic()) + t.interval;
        return UA_STATUSCODE_GOOD;
    }
    return UA_STATUSCODE_BADNOTFOUND;
}

void el_remove_timer(UA_EventLoop* el, UA_UInt64 timerId)
{
    ArduinoEventLoop* l = self(el);
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_TIMERS; i++)
    {
        if (l->timers[i].active && l->timers[i].id == timerId)
        {
            l->timers[i].active = false;
            l->timers[i].cb     = nullptr;
            return;
        }
    }
}

void el_add_delayed(UA_EventLoop* el, UA_DelayedCallback* dc)
{
    ArduinoEventLoop* l = self(el);
    dc->next   = l->delayed;
    l->delayed = dc;
}

void el_remove_delayed(UA_EventLoop* el, UA_DelayedCallback* dc)
{
    ArduinoEventLoop* l = self(el);
    UA_DelayedCallback** p = &l->delayed;
    while (*p != nullptr)
    {
        if (*p == dc)
        {
            *p = dc->next;
            return;
        }
        p = &(*p)->next;
    }
}

UA_StatusCode el_register_es(UA_EventLoop* el, UA_EventSource* es)
{
    if (es->eventLoop != nullptr)
        return UA_STATUSCODE_BADINTERNALERROR;
    es->eventLoop = el;
    es->next      = el->eventSources;
    el->eventSources = es;
    if (el->state == UA_EVENTLOOPSTATE_STARTED)
        es->start(es);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode el_deregister_es(UA_EventLoop* el, UA_EventSource* es)
{
    UA_EventSource** p = &el->eventSources;
    while (*p != nullptr)
    {
        if (*p == es)
        {
            *p = es->next;
            es->next      = nullptr;
            es->eventLoop = nullptr;
            return UA_STATUSCODE_GOOD;
        }
        p = &(*p)->next;
    }
    return UA_STATUSCODE_BADNOTFOUND;
}

} // namespace

extern "C" UA_EventLoop* UA_EventLoop_new_Arduino(const UA_Logger* logger)
{
    ua_arduino_install_allocator();

    ArduinoEventLoop* l = (ArduinoEventLoop*)UA_calloc(1, sizeof(ArduinoEventLoop));
    if (l == nullptr)
        return nullptr;

    UA_EventLoop* el = &l->base;
    el->logger = logger;
    set_state(el, UA_EVENTLOOPSTATE_FRESH);

    el->start                    = el_start;
    el->stop                     = el_stop;
    el->run                      = el_run;
    el->cancel                   = el_cancel;
    el->free                     = el_free;
    el->dateTime_now             = el_now;
    el->dateTime_nowMonotonic    = el_now_monotonic;
    el->dateTime_localTimeUtcOffset = el_utc_offset;
    el->nextTimer                = el_next_timer;
    el->addTimer                 = el_add_timer;
    el->modifyTimer              = el_modify_timer;
    el->removeTimer              = el_remove_timer;
    el->addDelayedCallback       = el_add_delayed;
    el->removeDelayedCallback    = el_remove_delayed;
    el->registerEventSource      = el_register_es;
    el->deregisterEventSource    = el_deregister_es;

    return el;
}
