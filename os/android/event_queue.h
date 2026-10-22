// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_EVENT_QUEUE_INCLUDED
#define OS_ANDROID_EVENT_QUEUE_INCLUDED
#pragma once

#include "base/concurrent_queue.h"
#include "os/event.h"
#include "os/event_queue.h"

#include <atomic>

struct ALooper;

namespace os {

// Event queue driven by the ALooper of the GameActivity glue thread
// (the thread that runs android_main() and app_main()).
//
// getEvent() returns queued events first; if there are none, it waits
// in ALooper_pollOnce() up to the given timeout and processes the
// APP_CMD_* commands and input buffers of the glue (see
// AppAndroid::pollLooper()), which can queue new events.
//
// queueEvent() can be called from any thread: if the glue thread is
// waiting in the looper, it's woken up with ALooper_wake().
class EventQueueAndroid : public EventQueue {
public:
  EventQueueAndroid();

  void queueEvent(const Event& ev) override;
  void getEvent(Event& ev, double timeout) override;
  void clearEvents() override;

  bool isEmpty() const { return m_events.empty(); }

  // Looper of the glue thread, set by AppAndroid (nullptr when
  // android_main() finishes).
  void setLooper(ALooper* looper);

private:
  base::concurrent_queue<Event> m_events;
  std::atomic<ALooper*> m_looper;
  std::atomic<bool> m_sleeping;
};

using EventQueueImpl = EventQueueAndroid;

} // namespace os

#endif
