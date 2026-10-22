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

// AKEY_EVENT_ACTION_DOWN/UP
constexpr int32_t kKeyActionDown = 0;

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
    setPenOutOfRange(now);

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

bool PointerInputAndroid::processKey(const int32_t action,
                                     const int32_t keyCode,
                                     const int32_t repeatCount,
                                     const base::tick_t now,
                                     std::vector<Event>& out)
{
  if (!isGestureKey(keyCode))
    return false;

  // Each double-tap arrives as two DOWN/UP pairs a few ms apart: only
  // the first DOWN of a burst is the gesture, UPs and repeats are
  // dropped (ADR 0006 section 6).
  if (action != kKeyActionDown || repeatCount > 0)
    return true;

  const bool burst = (m_penKeyTime && now - m_penKeyTime < kPenDoubleTapDebounce);
  m_penKeyTime = std::max<base::tick_t>(now, 1);
  if (burst) {
    log(0, "input: pen double-tap key %d repeated, dropped", int(keyCode));
    return true;
  }

  log(1, "input: pen double-tap");
  pushKey(kKeyPenDoubleTap, true, out);
  return true;
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
  m_touchMode = TouchMode::None;
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
      takeOwnership(Owner::Pen, now, out);
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
      takeOwnership(Owner::Pen, now, out);
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
        if (m_button != Event::NoneButton) {
          log(1, "input: pen ACTION_CANCEL, stroke cancelled");
          cancelButton(out);
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
  const bool active = (m_touchMode != TouchMode::None && m_touchMode != TouchMode::Ignored);

  // GameActivity copies 8 pointers at most: the pointer that went
  // down/up might not be in the event (ADR 0006 section 10). So many
  // pointers are a palm anyway.
  if ((action == M::kActionDown || action == M::kActionUp || action == M::kActionPointerDown ||
       action == M::kActionPointerUp) &&
      index >= ev.pointerCount) {
    log(0, "input: action pointer index %d >= pointer count %d, skipped", index, ev.pointerCount);
    if (active)
      cancelTouchStream(out, "palm (pointer limit)");
    if (action == M::kActionUp)
      m_touchMode = TouchMode::None;
    return;
  }

  // Palm: too many fingers at the same time (ADR 0006 section 4.5).
  if (active && ev.pointerCount > kMaxTouchPointers) {
    cancelTouchStream(out, "palm (too many fingers)");
    if (action == M::kActionUp || action == M::kActionCancel)
      m_touchMode = TouchMode::None;
    return;
  }

  switch (action) {
    case M::kActionDown: {
      // A new stream. If the previous one didn't end (lost UP),
      // cancel its stroke first.
      if (active)
        cancelTouchStream(out, "new ACTION_DOWN");
      m_touchMode = TouchMode::Ignored;
      m_touchPointerId = -1;

      const MotionPointerAndroid& p = ev.pointers[index];
      const char* reason = nullptr;
      if (rejectTouch(p, now, &reason)) {
        if (!m_touchIgnoredLogged) {
          log(1, "input: finger stream ignored (%s), logged once", reason);
          m_touchIgnoredLogged = true;
        }
        else {
          log(0, "input: finger stream ignored (%s)", reason);
        }
        break;
      }

      m_touchMode = TouchMode::Mouse;
      m_touchStartTime = now;
      m_touchMaxPointers = 1;
      m_tapCandidate = true;
      m_touchMoved = false;
      m_gestureAllowed = false;
      m_touchStartCount = 0;
      addTouchStart(p);

      takeOwnership(Owner::Touch, now, out);
      m_touchPointerId = p.id;
      ensureInside(p.x, p.y, PointerType::Touch, out);
      press(p, Event::LeftButton, PointerType::Touch, false, now, out);
      break;
    }

    case M::kActionPointerDown: {
      if (!active)
        break;
      const MotionPointerAndroid& p = ev.pointers[index];
      if (p.toolType == M::kToolPalm) {
        cancelTouchStream(out, "palm pointer");
        break;
      }

      addTouchStart(p);
      m_touchMaxPointers = std::max(m_touchMaxPointers, ev.pointerCount);
      if (now - m_touchStartTime > kTapDownWindow)
        m_tapCandidate = false;

      // A second finger: the one-finger stroke/scroll is discarded,
      // the stream is now a tap or a two-finger gesture.
      if (m_touchMode == TouchMode::Mouse) {
        if (m_button != Event::NoneButton) {
          log(0, "input: second finger, one-finger stroke cancelled");
          cancelButton(out);
        }
        m_touchPointerId = -1;
        m_touchMode = TouchMode::Multi;
      }

      if (ev.pointerCount == 2 && m_touchMaxPointers == 2) {
        startTwoFingerGesture(ev);
      }
      else {
        // A third finger ends the gesture (3-finger tap or palm).
        if (m_touchMode == TouchMode::Gesture)
          log(0, "input: two-finger gesture ended (third finger)");
        m_touchMode = TouchMode::Multi;
        m_gestureAllowed = false;
      }
      break;
    }

    case M::kActionMove:
      if (!active)
        break;
      checkTouchMovement(ev);

      if (m_touchMode == TouchMode::Mouse) {
        const int i = find_pointer_id(ev, m_touchPointerId);
        if (i >= 0)
          moveWithHistory(ev, i, PointerType::Touch, false, out);
      }
      else if (m_touchMode == TouchMode::Multi) {
        if (m_gestureAllowed && m_touchMoved && ev.pointerCount == 2) {
          log(0, "input: two-finger gesture started");
          m_touchMode = TouchMode::Gesture;
          m_tapCandidate = false;
          // The wheel/magnify events go to the widget under the
          // fingers.
          const float cx = m_gestureX;
          const float cy = m_gestureY;
          MotionPointerAndroid c;
          c.x = cx;
          c.y = cy;
          ensureInside(cx, cy, PointerType::Touch, out);
          move(c, PointerType::Touch, false, true, out);
          processTwoFingerGesture(ev, out);
        }
      }
      else if (m_touchMode == TouchMode::Gesture) {
        processTwoFingerGesture(ev, out);
      }
      break;

    case M::kActionPointerUp:
      if (!active)
        break;
      checkTouchMovement(ev);
      // Lifting one of two fingers ends the gesture; the remaining
      // finger doesn't draw or pan.
      if (m_touchMode == TouchMode::Gesture)
        log(0, "input: two-finger gesture ended (finger lifted)");
      if (m_touchMode == TouchMode::Gesture || m_touchMode == TouchMode::Multi) {
        m_touchMode = TouchMode::Multi;
        m_gestureAllowed = false;
      }
      break;

    case M::kActionUp:
      if (active) {
        checkTouchMovement(ev);
        if (m_touchMode == TouchMode::Mouse && m_touchPointerId >= 0) {
          const int i = find_pointer_id(ev, m_touchPointerId);
          const MotionPointerAndroid& p = ev.pointers[i >= 0 ? i : index];
          moveWithHistory(ev, i >= 0 ? i : index, PointerType::Touch, false, out);
          release(p.x, p.y, out);
        }
        endTouchStream(now, out);
      }
      m_touchMode = TouchMode::None;
      m_touchPointerId = -1;
      break;

    case M::kActionCancel:
      // The system rejected the stream (palm, ADR 0006 section 4.3).
      if (active)
        cancelTouchStream(out, "ACTION_CANCEL");
      m_touchMode = TouchMode::None;
      break;

    default:
      // Fingers don't hover; ACTION_OUTSIDE, ACTION_BUTTON_*
      log(0, "input: finger action %d ignored", action);
      break;
  }
}

bool PointerInputAndroid::rejectTouch(const MotionPointerAndroid& p,
                                      const base::tick_t now,
                                      const char** reason) const
{
  if (p.toolType == M::kToolPalm)
    *reason = "palm";
  else if (m_penInRange)
    *reason = "pen in range";
  else if (m_owner == Owner::Pen && m_button != Event::NoneButton)
    *reason = "pen touching";
  else if (m_penLeftTime && now - m_penLeftTime < m_penRejectionTimeout)
    *reason = "pen left the range recently";
  else if (m_owner == Owner::Mouse && m_button != Event::NoneButton)
    *reason = "mouse button pressed";
  else
    return false;
  return true;
}

void PointerInputAndroid::addTouchStart(const MotionPointerAndroid& p)
{
  for (int i = 0; i < m_touchStartCount; ++i) {
    if (m_touchStarts[i].id == p.id) {
      m_touchStarts[i].x = p.x;
      m_touchStarts[i].y = p.y;
      return;
    }
  }
  if (m_touchStartCount < int(m_touchStarts.size())) {
    TouchStart& t = m_touchStarts[m_touchStartCount++];
    t.id = p.id;
    t.x = p.x;
    t.y = p.y;
  }
}

void PointerInputAndroid::checkTouchMovement(const MotionEventAndroid& ev)
{
  if (m_touchMoved)
    return;

  for (int i = 0; i < ev.pointerCount; ++i) {
    const MotionPointerAndroid& p = ev.pointers[i];
    for (int j = 0; j < m_touchStartCount; ++j) {
      const TouchStart& t = m_touchStarts[j];
      if (t.id == p.id &&
          (std::abs(p.x - t.x) > float(m_tapSlop) || std::abs(p.y - t.y) > float(m_tapSlop))) {
        m_touchMoved = true;
        m_tapCandidate = false;
        return;
      }
    }
  }
}

void PointerInputAndroid::startTwoFingerGesture(const MotionEventAndroid& ev)
{
  // The center and distance when the second finger lands are the
  // reference, so the movement before the gesture starts (up to the
  // tap slop) is not lost.
  const MotionPointerAndroid& a = ev.pointers[0];
  const MotionPointerAndroid& b = ev.pointers[1];
  m_gestureIds[0] = a.id;
  m_gestureIds[1] = b.id;
  m_gestureX = (a.x + b.x) / 2.0f;
  m_gestureY = (a.y + b.y) / 2.0f;
  m_gestureDistance = std::hypot(b.x - a.x, b.y - a.y);
  m_panX = m_panY = 0.0f;
  m_gestureAllowed = true;
}

void PointerInputAndroid::processTwoFingerGesture(const MotionEventAndroid& ev,
                                                  std::vector<Event>& out)
{
  const int ia = find_pointer_id(ev, m_gestureIds[0]);
  const int ib = find_pointer_id(ev, m_gestureIds[1]);
  if (ia < 0 || ib < 0)
    return;

  const MotionPointerAndroid& a = ev.pointers[ia];
  const MotionPointerAndroid& b = ev.pointers[ib];
  const float cx = (a.x + b.x) / 2.0f;
  const float cy = (a.y + b.y) / 2.0f;
  const float distance = std::hypot(b.x - a.x, b.y - a.y);

  // Pinch: zoom around the center (StateWithWheelBehavior::
  // onTouchMagnify() multiplies the scale by 1 + magnification).
  // Tiny changes are accumulated until they are noticeable.
  if (m_gestureDistance >= 1.0f && distance >= 1.0f) {
    const float magnification = distance / m_gestureDistance - 1.0f;
    if (std::abs(magnification) >= 0.005f) {
      Event mag = makeEvent(Event::TouchMagnify, cx, cy, PointerType::Touchpad, 0.0f);
      mag.setMagnification(magnification);
      out.push_back(mag);
      m_gestureDistance = distance;
    }
  }
  else {
    m_gestureDistance = distance;
  }

  // Pan: the content follows the fingers, i.e. the scroll position
  // moves the other way (a precise wheel adds its delta to the
  // scroll, ui::View and the editor).
  m_panX -= (cx - m_gestureX) / float(m_scale);
  m_panY -= (cy - m_gestureY) / float(m_scale);
  m_gestureX = cx;
  m_gestureY = cy;

  const int dx = int(m_panX);
  const int dy = int(m_panY);
  if (dx != 0 || dy != 0) {
    m_panX -= float(dx);
    m_panY -= float(dy);
    Event wheel = makeEvent(Event::MouseWheel, cx, cy, PointerType::Touchpad, 0.0f);
    wheel.setWheelDelta(gfx::Point(dx, dy));
    wheel.setPreciseWheel(true);
    out.push_back(wheel);
  }
}

void PointerInputAndroid::endTouchStream(const base::tick_t now, std::vector<Event>& out)
{
  // Finger taps (ADR 0006 section 6).
  if (m_tapCandidate && now - m_touchStartTime <= kTapMaxDuration &&
      (m_touchMaxPointers == 2 || m_touchMaxPointers == 3)) {
    log(1, "input: %d-finger tap", m_touchMaxPointers);
    pushKey(m_touchMaxPointers == 2 ? kKeyTwoFingerTap : kKeyThreeFingerTap, true, out);
  }
  m_tapCandidate = false;
  m_gestureAllowed = false;
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
      takeOwnership(Owner::Mouse, now, out);
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
      takeOwnership(Owner::Mouse, now, out);
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

      takeOwnership(Owner::Mouse, now, out);
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
    log(1, "input: finger stream cancelled (%s), stroke cancelled", reason);
    cancelButton(out);
  }
  else if (m_touchMode != TouchMode::None && m_touchMode != TouchMode::Ignored) {
    log(m_touchMode == TouchMode::Gesture ? 1 : 0, "input: finger stream cancelled (%s)", reason);
  }
  m_touchMode = TouchMode::Ignored;
  m_touchPointerId = -1;
  m_tapCandidate = false;
  m_gestureAllowed = false;
}

void PointerInputAndroid::cancelButton(std::vector<Event>& out)
{
  if (m_button == Event::NoneButton)
    return;

  // The app discards the stroke on this KeyUp (DrawingState treats it
  // like Esc), so the MouseUp that follows doesn't commit it.
  pushKey(kKeyPointerCancel, false, out);
  release(float(m_lastPx.x), float(m_lastPx.y), out);
}

void PointerInputAndroid::setPenOutOfRange(const base::tick_t now)
{
  if (m_penInRange)
    m_penLeftTime = std::max<base::tick_t>(now, 1);
  m_penInRange = false;
}

void PointerInputAndroid::takeOwnership(const Owner owner,
                                        const base::tick_t now,
                                        std::vector<Event>& out)
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
    setPenOutOfRange(now);

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

void PointerInputAndroid::pushKey(const KeyScancode scancode,
                                  const bool withKeyDown,
                                  std::vector<Event>& out) const
{
  Event ev;
  ev.setScancode(scancode);
  if (withKeyDown) {
    ev.setType(Event::KeyDown);
    out.push_back(ev);
  }
  ev.setType(Event::KeyUp);
  out.push_back(ev);
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
