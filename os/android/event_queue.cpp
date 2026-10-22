// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/event_queue.h"

#include "base/time.h"
#include "os/android/app.h"

#include <android/looper.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace os {

namespace {

// Maximum wait in getEvent() after APP_CMD_DESTROY.
constexpr base::tick_t kDestroyedWaitMsecs = 100;

} // anonymous namespace

EventQueueAndroid::EventQueueAndroid() : m_looper(nullptr), m_sleeping(false)
{
}

void EventQueueAndroid::queueEvent(const Event& ev)
{
  m_events.push(ev);

  if (m_sleeping) {
    if (ALooper* looper = m_looper)
      ALooper_wake(looper);
  }
}

void EventQueueAndroid::getEvent(Event& ev, double timeout)
{
  ev.setWindow(nullptr);

  AppAndroid* app = AppAndroid::instance();
  if (app) {
    // Always process pending commands/input first (without waiting),
    // so lifecycle commands are not delayed while the app keeps the
    // queue busy.
    app->pollLooper(0);

    if (m_events.empty() && timeout != 0.0) {
      bool withoutTimeout = (timeout < 0.0); // kWithoutTimeout
      base::tick_t deadline =
        (withoutTimeout ? 0 : base::current_tick() + base::tick_t(std::ceil(timeout * 1000.0)));

      // After APP_CMD_DESTROY the app got CloseApp and should return
      // from app_main() soon, but it can still be running (e.g. a
      // "save changes?" alert without window). Never wait more than
      // kDestroyedWaitMsecs then (the looper doesn't receive new
      // commands, it could block forever), but don't return at once
      // either (the app would spin). AppAndroid has a watchdog that
      // ends the process if app_main() doesn't return.
      if (app->isDestroyRequested()) {
        const base::tick_t destroyedDeadline = base::current_tick() + kDestroyedWaitMsecs;
        if (withoutTimeout || destroyedDeadline < deadline)
          deadline = destroyedDeadline;
        withoutTimeout = false;
      }

      while (m_events.empty()) {
        int waitMsecs = -1;
        if (!withoutTimeout && (!app->canWaitWithoutTimeout() || app->isDestroyRequested())) {
          const base::tick_t now = base::current_tick();
          if (now >= deadline)
            break;
          waitMsecs = int(std::min<base::tick_t>(deadline - now, std::numeric_limits<int>::max()));
        }

        // Set the sleeping flag before checking the queue again, so a
        // queueEvent() from other thread between the check and the
        // ALooper_pollOnce() wakes up the looper.
        m_sleeping = true;
        if (m_events.empty())
          app->pollLooper(waitMsecs);
        m_sleeping = false;

        // APP_CMD_DESTROY arrived while waiting: it queues CloseApp,
        // so the loop ends anyway.
      }
    }
  }

  if (!m_events.try_pop(ev))
    ev.setType(Event::None);
}

void EventQueueAndroid::clearEvents()
{
  m_events.clear();
}

void EventQueueAndroid::setLooper(ALooper* looper)
{
  m_looper = looper;
}

} // namespace os
