// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_EVENT_QUEUE_INCLUDED
#define OS_ANDROID_EVENT_QUEUE_INCLUDED
#pragma once

#include "os/event.h"
#include "os/event_queue.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace os {

// Thread-safe queue of events. The Android event loop (which will
// live in the same thread that calls getEvent()) or other threads
// can queue events at any time.
//
// TODO when os/android lands, getEvent() must also poll the ALooper
//      (or be woken up with ALooper_wake()), otherwise blocking here
//      would stall the looper and the app would get an ANR.
class EventQueueAndroid : public EventQueue {
public:
  void queueEvent(const Event& ev) override;
  void getEvent(Event& ev, double timeout) override;
  void clearEvents() override;

private:
  std::deque<Event> m_events;
  std::mutex m_mutex;
  std::condition_variable m_cond;
};

using EventQueueImpl = EventQueueAndroid;

} // namespace os

#endif
