// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/pointer_input.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace os {

namespace {

using M = MotionEventAndroid;

bool is_pen_tool(const int32_t toolType)
{
  return (toolType == M::kToolStylus || toolType == M::kToolEraser);
}

Event::MouseButton stylus_button(const int32_t buttonState)
{
  if (buttonState & M::kButtonStylusPrimary)
    return Event::RightButton;
  if (buttonState & M::kButtonStylusSecondary)
    return Event::MiddleButton;
  return Event::LeftButton;
}

Event::MouseButton mouse_button(const int32_t buttonState)
{
  if (buttonState & M::kButtonSecondary)
    return Event::RightButton;
  if (buttonState & M::kButtonTertiary)
    return Event::MiddleButton;
  if (buttonState & M::kButtonBack)
    return Event::X1Button;
  if (buttonState & M::kButtonForward)
    return Event::X2Button;
  return Event::LeftButton;
}

// Index of the first pointer with a stylus/eraser tool, or -1.
int find_pen_pointer(const MotionEventAndroid& ev)
{
  for (int i = 0; i < ev.pointerCount; ++i) {
    if (is_pen_tool(ev.pointers[i].toolType))
      return i;
  }
  return -1;
}

int find_pointer_id(const MotionEventAndroid& ev, const int32_t id)
{
  for (int i = 0; i < ev.pointerCount; ++i) {
    if (ev.pointers[i].id == id)
      return i;
  }
  return -1;
}

} // anonymous namespace

PointerInputAndroid::PointerInputAndroid()
{
}

void PointerInputAndroid::processMotion(const MotionEventAndroid& ev,
                                        const base::tick_t now,
                                        std::vector<Event>& out)
{
  if (ev.pointerCount <= 0 || ev.pointerCount > M::kMaxPointers)
    return;

  // A deferred MouseLeave that is already due goes before this event.
  processTimers(now, out);

  if (find_pen_pointer(ev) >= 0)
    processPen(ev, now, out);
  else if (ev.pointers[0].toolType == M::kToolMouse ||
           (ev.pointers[0].toolType == M::kToolUnknown &&
            (ev.source & M::kSourceMouse) == M::kSourceMouse))
    processMouse(ev, now, out);
  else
    processTouch(ev, now, out);
}

void PointerInputAndroid::processTimers(const base::tick_t now, std::vector<Event>& out)
{
  if (!m_leaveTime || now < m_leaveTime)
    return;

  m_leaveTime = 0;
  if (m_owner == Owner::Pen)
    m_penInRange = false;

  if ((m_owner == Owner::Pen || m_owner == Owner::Mouse) && m_button == Event::NoneButton) {
    if (m_inside) {
      out.push_back(
        makeEvent(Event::MouseLeave, float(m_lastPx.x), float(m_lastPx.y), m_pointerType, 0.0f));
      m_inside = false;
      m_hasLastPos = false;
    }
    log(0, "input: pointer left (deferred MouseLeave emitted)");
    m_owner = Owner::None;
  }
}

int PointerInputAndroid::nextTimeout(const base::tick_t now) const
{
  if (!m_leaveTime)
    return -1;
  if (now >= m_leaveTime)
    return 0;
  return int(m_leaveTime - now);
}

void PointerInputAndroid::reset(std::vector<Event>& out)
{
  if (m_button != Event::NoneButton)
    release(float(m_lastPx.x), float(m_lastPx.y), out);
  if (m_inside) {
    out.push_back(
      makeEvent(Event::MouseLeave, float(m_lastPx.x), float(m_lastPx.y), m_pointerType, 0.0f));
    m_inside = false;
  }
  m_owner = Owner::None;
  m_penInRange = false;
  m_leaveTime = 0;
  m_touchStream = false;
  m_touchIgnored = false;
  m_touchPointerId = -1;
  m_hasLastPos = false;
  m_clickButton = Event::NoneButton;
  m_scrollX = m_scrollY = 0.0f;
}

void PointerInputAndroid::processPen(const MotionEventAndroid& ev,
                                     const base::tick_t now,
                                     std::vector<Event>& out)
{
  const int i = find_pen_pointer(ev);
  const MotionPointerAndroid& p = ev.pointers[i];
  const PointerType pointerType = (p.toolType == M::kToolEraser ? PointerType::Eraser :
                                                                  PointerType::Pen);

  switch (ev.actionMasked()) {
    case M::kActionHoverEnter:
    case M::kActionHoverMove:
      cancelLeave(now, "HOVER_ENTER/MOVE");
      takeOwnership(Owner::Pen, out);
      // A hover event means the tip is not touching: release a button
      // if we missed the UP.
      if (m_button != Event::NoneButton)
        release(p.x, p.y, out);
      m_penInRange = true;
      ensureInside(p.x, p.y, pointerType, out);
      moveWithHistory(ev, i, pointerType, false, out);
      break;

    case M::kActionHoverExit:
      if (m_owner == Owner::Pen && m_button == Event::NoneButton) {
        m_hoverExitTime = now;
        scheduleLeave(now, kLeaveDelay);
      }
      break;

    case M::kActionDown:
    case M::kActionPointerDown:
      if (m_owner == Owner::Pen && m_button != Event::NoneButton)
        break; // Already down
      if (m_leaveTime && !m_hoverOrderLogged && m_hoverExitTime) {
        log(1,
            "input: pen order: HOVER_EXIT then DOWN after %d ms (MouseLeave dropped)",
            int(now - m_hoverExitTime));
        m_hoverOrderLogged = true;
      }
      cancelLeave(now, "DOWN");
      takeOwnership(Owner::Pen, out);
      m_penInRange = true;
      ensureInside(p.x, p.y, pointerType, out);
      press(p, stylus_button(ev.buttonState), pointerType, true, now, out);
      break;

    case M::kActionMove:
      if (m_owner == Owner::Pen && m_button != Event::NoneButton)
        moveWithHistory(ev, i, pointerType, true, out);
      break;

    case M::kActionUp:
    case M::kActionPointerUp:
      if (m_owner == Owner::Pen && m_button != Event::NoneButton) {
        // Only the batched samples: the pressure of the UP sample
        // itself can be 0, which would add a thin end to the stroke.
        // The MouseUp goes at the last position.
        for (int h = 0;
             h < ev.historySize && int(ev.history.size()) >= ev.historySize * ev.pointerCount;
             ++h) {
          move(ev.historical(h, i), pointerType, true, false, out);
        }
        release(float(m_lastPx.x), float(m_lastPx.y), out);
        scheduleLeave(now, kLeaveAfterUpDelay);
      }
      break;

    case M::kActionCancel:
      if (m_owner == Owner::Pen) {
        // TODO M3.1 cancel the stroke instead of committing it
        if (m_button != Event::NoneButton) {
          log(1, "input: pen ACTION_CANCEL, MouseUp sent (stroke is kept until M3.1)");
          release(float(m_lastPx.x), float(m_lastPx.y), out);
        }
        scheduleLeave(now, kLeaveAfterUpDelay);
      }
      break;

    default:
      // ACTION_BUTTON_PRESS/RELEASE, ACTION_OUTSIDE, ACTION_SCROLL
      log(0, "input: pen action %d ignored", ev.actionMasked());
      break;
  }
}

void PointerInputAndroid::processTouch(const MotionEventAndroid& ev,
                                       const base::tick_t now,
                                       std::vector<Event>& out)
{
  const int action = ev.actionMasked();
  const int index = ev.actionIndex();

  // GameActivity copies 8 pointers at most: the pointer that went
  // down/up might not be in the event (ADR 0006 section 10).
  if ((action == M::kActionDown || action == M::kActionUp || action == M::kActionPointerDown ||
       action == M::kActionPointerUp) &&
      index >= ev.pointerCount) {
    log(0, "input: action pointer index %d >= pointer count %d, skipped", index, ev.pointerCount);
    if (action == M::kActionUp) {
      cancelTouchStream(out, "lost ACTION_UP pointer");
      m_touchStream = false;
      m_touchIgnored = false;
    }
    return;
  }

  switch (action) {
    case M::kActionDown: {
      // A new stream. If the previous one didn't end (lost UP),
      // release its button first.
      if (m_touchStream)
        cancelTouchStream(out, "new ACTION_DOWN");
      m_touchStream = true;
      m_touchIgnored = false;
      m_touchPointerId = -1;

      const MotionPointerAndroid& p = ev.pointers[index];
      const char* reason = nullptr;
      if (p.toolType == M::kToolPalm)
        reason = "palm";
      else if (m_penInRange)
        reason = "pen in range";
      else if (m_owner == Owner::Pen && m_button != Event::NoneButton)
        reason = "pen touching";
      else if (m_owner == Owner::Mouse && m_button != Event::NoneButton)
        reason = "mouse button pressed";

      if (reason) {
        m_touchIgnored = true;
        if (!m_touchIgnoredLogged) {
          log(1, "input: finger stream ignored (%s), logged once", reason);
          m_touchIgnoredLogged = true;
        }
        else {
          log(0, "input: finger stream ignored (%s)", reason);
        }
        break;
      }

      takeOwnership(Owner::Touch, out);
      m_touchPointerId = p.id;
      ensureInside(p.x, p.y, PointerType::Touch, out);
      press(p, Event::LeftButton, PointerType::Touch, false, now, out);
      break;
    }

    case M::kActionPointerDown:
      // TODO M3 two-finger gestures, palm (more than 3 pointers)
      if (ev.pointers[index].toolType == M::kToolPalm)
        cancelTouchStream(out, "palm pointer");
      break;

    case M::kActionMove: {
      if (m_touchIgnored || m_owner != Owner::Touch || m_touchPointerId < 0)
        break;
      const int i = find_pointer_id(ev, m_touchPointerId);
      if (i >= 0)
        moveWithHistory(ev, i, PointerType::Touch, false, out);
      break;
    }

    case M::kActionPointerUp:
      // The finger that drives the mouse is lifted while others stay:
      // release the button and ignore the rest of the stream (to
      // avoid jumps to the other finger).
      if (!m_touchIgnored && m_owner == Owner::Touch && ev.pointers[index].id == m_touchPointerId) {
        const MotionPointerAndroid& p = ev.pointers[index];
        moveWithHistory(ev, index, PointerType::Touch, false, out);
        release(p.x, p.y, out);
        m_touchPointerId = -1;
        m_touchIgnored = true;
      }
      break;

    case M::kActionUp:
      if (!m_touchIgnored && m_owner == Owner::Touch && m_touchPointerId >= 0) {
        const int i = find_pointer_id(ev, m_touchPointerId);
        const MotionPointerAndroid& p = ev.pointers[i >= 0 ? i : index];
        moveWithHistory(ev, i >= 0 ? i : index, PointerType::Touch, false, out);
        release(p.x, p.y, out);
      }
      m_touchStream = false;
      m_touchIgnored = false;
      m_touchPointerId = -1;
      break;

    case M::kActionCancel:
      // TODO M3.1 cancel the stroke instead of committing it
      cancelTouchStream(out, "ACTION_CANCEL");
      m_touchStream = false;
      m_touchIgnored = false;
      break;

    default:
      // Fingers don't hover; ACTION_OUTSIDE, ACTION_BUTTON_*
      log(0, "input: finger action %d ignored", action);
      break;
  }
}

void PointerInputAndroid::processMouse(const MotionEventAndroid& ev,
                                       const base::tick_t now,
                                       std::vector<Event>& out)
{
  const MotionPointerAndroid& p = ev.pointers[0];

  switch (ev.actionMasked()) {
    case M::kActionHoverEnter:
    case M::kActionHoverMove:
      cancelLeave(now, "mouse HOVER_ENTER/MOVE");
      takeOwnership(Owner::Mouse, out);
      if (m_button != Event::NoneButton)
        release(p.x, p.y, out);
      ensureInside(p.x, p.y, PointerType::Mouse, out);
      moveWithHistory(ev, 0, PointerType::Mouse, false, out);
      break;

    case M::kActionHoverExit:
      if (m_owner == Owner::Mouse && m_button == Event::NoneButton)
        scheduleLeave(now, kLeaveDelay);
      break;

    case M::kActionDown:
      if (m_owner == Owner::Mouse && m_button != Event::NoneButton)
        break;
      cancelLeave(now, "mouse DOWN");
      takeOwnership(Owner::Mouse, out);
      ensureInside(p.x, p.y, PointerType::Mouse, out);
      press(p, mouse_button(ev.buttonState), PointerType::Mouse, false, now, out);
      break;

    case M::kActionMove:
      if (m_owner == Owner::Mouse && m_button != Event::NoneButton)
        moveWithHistory(ev, 0, PointerType::Mouse, false, out);
      break;

    case M::kActionUp:
    case M::kActionCancel:
      if (m_owner == Owner::Mouse && m_button != Event::NoneButton) {
        moveWithHistory(ev, 0, PointerType::Mouse, false, out);
        release(p.x, p.y, out);
      }
      break;

    case M::kActionScroll: {
      // Sign contract (NOTES.md): wheel up = negative y, wheel right =
      // positive x, as in os/x11 and os/win. Fractions (touchpads) are
      // accumulated until they reach a whole step.
      m_scrollY -= ev.vscroll;
      m_scrollX += ev.hscroll;
      const int dx = int(m_scrollX);
      const int dy = int(m_scrollY);
      if (dx == 0 && dy == 0)
        break;
      m_scrollX -= float(dx);
      m_scrollY -= float(dy);

      takeOwnership(Owner::Mouse, out);
      ensureInside(p.x, p.y, PointerType::Mouse, out);
      Event wheel = makeEvent(Event::MouseWheel, p.x, p.y, PointerType::Mouse, 0.0f);
      wheel.setWheelDelta(gfx::Point(dx, dy));
      out.push_back(wheel);
      break;
    }

    default:
      // ACTION_BUTTON_PRESS/RELEASE (the button is taken from
      // buttonState at ACTION_DOWN), ACTION_OUTSIDE
      log(0, "input: mouse action %d ignored", ev.actionMasked());
      break;
  }
}

void PointerInputAndroid::cancelTouchStream(std::vector<Event>& out, const char* reason)
{
  if (m_owner == Owner::Touch && m_button != Event::NoneButton) {
    // TODO M3.1 KeyUp(kKeyPointerCancel) before the MouseUp so the
    //      stroke is discarded
    log(1, "input: finger stream ended (%s), MouseUp sent", reason);
    release(float(m_lastPx.x), float(m_lastPx.y), out);
  }
  else if (m_touchStream && !m_touchIgnored) {
    log(0, "input: finger stream ended (%s)", reason);
  }
  m_touchIgnored = true;
  m_touchPointerId = -1;
}

void PointerInputAndroid::takeOwnership(const Owner owner, std::vector<Event>& out)
{
  if (m_owner == owner)
    return;

  // The pen or the mouse take over a finger stream in progress (the
  // palm usually lands before the pen, ADR 0006 section 4.2).
  if (m_owner == Owner::Touch)
    cancelTouchStream(out, (owner == Owner::Pen ? "pen arrived" : "mouse arrived"));
  else if (m_button != Event::NoneButton)
    release(float(m_lastPx.x), float(m_lastPx.y), out);

  if (m_owner == Owner::Pen)
    m_penInRange = false;

  m_owner = owner;
  m_hasLastPos = false;
}

void PointerInputAndroid::cancelLeave(const base::tick_t now, const char* reason)
{
  if (m_leaveTime) {
    log(0,
        "input: deferred MouseLeave dropped (%s after %d ms)",
        reason,
        int(now - m_hoverExitTime));
    m_leaveTime = 0;
  }
}

void PointerInputAndroid::scheduleLeave(const base::tick_t now, const base::tick_t delay)
{
  m_leaveTime = std::max<base::tick_t>(now + delay, 1);
}

gfx::Point PointerInputAndroid::toWindow(const float x, const float y) const
{
  // Like the other backends, integer division of the pixel position.
  return gfx::Point(int(std::floor(x)) / m_scale, int(std::floor(y)) / m_scale);
}

Event PointerInputAndroid::makeEvent(const Event::Type type,
                                     const float x,
                                     const float y,
                                     const PointerType pointerType,
                                     const float pressure) const
{
  Event ev;
  ev.setType(type);
  ev.setPosition(toWindow(x, y));
  ev.setPointerType(pointerType);
  ev.setPressure(std::clamp(pressure, 0.0f, 1.0f));
  return ev;
}

void PointerInputAndroid::ensureInside(const float x,
                                       const float y,
                                       const PointerType pointerType,
                                       std::vector<Event>& out)
{
  m_pointerType = pointerType;
  if (m_inside)
    return;
  out.push_back(makeEvent(Event::MouseEnter, x, y, pointerType, 0.0f));
  m_inside = true;
  m_hasLastPos = false;
}

void PointerInputAndroid::move(const MotionPointerAndroid& p,
                               const PointerType pointerType,
                               const bool withPressure,
                               const bool force,
                               std::vector<Event>& out)
{
  const gfx::Point pos = toWindow(p.x, p.y);
  m_lastPx = gfx::Point(int(std::floor(p.x)), int(std::floor(p.y)));

  // Many samples fall in the same scaled pixel (scale 3, 144 Hz).
  if (!force && m_hasLastPos && pos == m_lastPos)
    return;

  out.push_back(
    makeEvent(Event::MouseMove, p.x, p.y, pointerType, withPressure ? p.pressure : 0.0f));
  m_lastPos = pos;
  m_hasLastPos = true;

  // Movement beyond the slop ends a double click candidate.
  if (m_clickButton != Event::NoneButton &&
      (std::abs(m_lastPx.x - m_clickPx.x) > m_doubleClickSlop ||
       std::abs(m_lastPx.y - m_clickPx.y) > m_doubleClickSlop)) {
    m_clickButton = Event::NoneButton;
  }
}

void PointerInputAndroid::moveWithHistory(const MotionEventAndroid& ev,
                                          const int pointerIndex,
                                          const PointerType pointerType,
                                          const bool withPressure,
                                          std::vector<Event>& out)
{
  if (pointerIndex < 0 || pointerIndex >= ev.pointerCount)
    return;

  if (ev.historySize > 0 && int(ev.history.size()) >= ev.historySize * ev.pointerCount) {
    for (int h = 0; h < ev.historySize; ++h)
      move(ev.historical(h, pointerIndex), pointerType, withPressure, false, out);
  }
  move(ev.pointers[pointerIndex], pointerType, withPressure, false, out);
}

void PointerInputAndroid::press(const MotionPointerAndroid& p,
                                const Event::MouseButton button,
                                const PointerType pointerType,
                                const bool withPressure,
                                const base::tick_t now,
                                std::vector<Event>& out)
{
  // ui::Manager sends the MouseDown to the widget under the last
  // MouseMove, so there is always a move at the same position first.
  move(p, pointerType, withPressure, true, out);

  Event ev = makeEvent(Event::MouseDown, p.x, p.y, pointerType, withPressure ? p.pressure : 0.0f);
  ev.setButton(button);

  const gfx::Point px(int(std::floor(p.x)), int(std::floor(p.y)));
  if (m_clickButton == button && m_clickPointerType == pointerType &&
      now - m_clickTime <= m_doubleClickTime && std::abs(px.x - m_clickPx.x) <= m_doubleClickSlop &&
      std::abs(px.y - m_clickPx.y) <= m_doubleClickSlop) {
    ev.setType(Event::MouseDoubleClick);
    m_clickButton = Event::NoneButton;
  }
  else {
    m_clickButton = button;
    m_clickPointerType = pointerType;
    m_clickTime = now;
    m_clickPx = px;
  }

  out.push_back(ev);
  m_button = button;
  m_pointerType = pointerType;
}

void PointerInputAndroid::release(const float x, const float y, std::vector<Event>& out)
{
  if (m_button == Event::NoneButton)
    return;

  MotionPointerAndroid p;
  p.x = x;
  p.y = y;
  move(p, m_pointerType, false, false, out);

  Event ev = makeEvent(Event::MouseUp, x, y, m_pointerType, 0.0f);
  ev.setButton(m_button);
  out.push_back(ev);
  m_button = Event::NoneButton;
}

void PointerInputAndroid::log(const int level, const char* fmt, ...) const
{
  if (!m_log)
    return;

  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  m_log(level, buf);
}

} // namespace os
