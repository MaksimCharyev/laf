// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/event_queue.h"

#include <chrono>

namespace os {

void EventQueueAndroid::queueEvent(const Event& ev)
{
  {
    const std::lock_guard lock(m_mutex);
    m_events.push_back(ev);
  }
  m_cond.notify_one();
}

void EventQueueAndroid::getEvent(Event& ev, double timeout)
{
  ev.setWindow(nullptr);

  std::unique_lock lock(m_mutex);
  if (m_events.empty()) {
    if (timeout == kWithoutTimeout) {
      m_cond.wait(lock, [this] { return !m_events.empty(); });
    }
    else if (timeout > 0.0) {
      m_cond.wait_for(lock, std::chrono::duration<double>(timeout), [this] {
        return !m_events.empty();
      });
    }
  }

  if (m_events.empty()) {
    ev.setType(Event::None);
    return;
  }

  ev = m_events.front();
  m_events.pop_front();
}

void EventQueueAndroid::clearEvents()
{
  const std::lock_guard lock(m_mutex);
  m_events.clear();
}

} // namespace os
