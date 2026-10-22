// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_POINTER_INPUT_H_INCLUDED
#define OS_ANDROID_POINTER_INPUT_H_INCLUDED
#pragma once

#include "base/time.h"
#include "gfx/point.h"
#include "os/event.h"
#include "os/pointer_type.h"

#include <cstdint>
#include <vector>

namespace os {

// Pointer of a MotionEventAndroid (a copy of the values we use from
// GameActivityPointerAxes, coordinates in window pixels).
struct MotionPointerAndroid {
  int32_t id = 0;
  int32_t toolType = 0;
  float x = 0.0f;
  float y = 0.0f;
  float pressure = 0.0f;    // AXIS_PRESSURE (0..1 for a stylus)
  float tilt = 0.0f;        // AXIS_TILT in radians (not in os::Event yet)
  float orientation = 0.0f; // AXIS_ORIENTATION in radians (not in os::Event yet)
};

// The part of a GameActivityMotionEvent (i.e. a Java MotionEvent)
// that PointerInputAndroid needs. It doesn't depend on the NDK or
// GameActivity headers so the translation can be tested on the host
// (os/android/pointer_input_tests.cpp). The constants have the same
// values as the AMOTION_EVENT_* and AINPUT_* constants of
// <android/input.h> (checked with static_assert in app.cpp).
struct MotionEventAndroid {
  enum : int32_t {
    kActionMask = 0xff,
    kActionPointerIndexMask = 0xff00,
    kActionPointerIndexShift = 8,

    kActionDown = 0,
    kActionUp = 1,
    kActionMove = 2,
    kActionCancel = 3,
    kActionOutside = 4,
    kActionPointerDown = 5,
    kActionPointerUp = 6,
    kActionHoverMove = 7,
    kActionScroll = 8,
    kActionHoverEnter = 9,
    kActionHoverExit = 10,
    kActionButtonPress = 11,
    kActionButtonRelease = 12,
  };

  enum : int32_t {
    kToolUnknown = 0,
    kToolFinger = 1,
    kToolStylus = 2,
    kToolMouse = 3,
    kToolEraser = 4,
    kToolPalm = 5,
  };

  enum : int32_t {
    kButtonPrimary = 1 << 0,
    kButtonSecondary = 1 << 1,
    kButtonTertiary = 1 << 2,
    kButtonBack = 1 << 3,
    kButtonForward = 1 << 4,
    kButtonStylusPrimary = 1 << 5,
    kButtonStylusSecondary = 1 << 6,
  };

  enum : int32_t {
    kSourceClassPointer = 0x00000002,
    kSourceMouse = 0x00002002,
  };

  // GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT (GameActivity copies
  // at most 8 pointers, ADR 0006 section 10).
  static constexpr int kMaxPointers = 8;

  int32_t action = 0; // Masked action + pointer index
  int32_t source = 0;
  int32_t buttonState = 0;
  int pointerCount = 0;
  MotionPointerAndroid pointers[kMaxPointers];

  // Batched samples before the current one (oldest first), for each
  // pointer: history[pos * pointerCount + pointerIndex].
  int historySize = 0;
  std::vector<MotionPointerAndroid> history;

  // AXIS_VSCROLL/AXIS_HSCROLL of the first pointer (ACTION_SCROLL).
  float vscroll = 0.0f;
  float hscroll = 0.0f;

  int actionMasked() const { return action & kActionMask; }
  int actionIndex() const { return (action & kActionPointerIndexMask) >> kActionPointerIndexShift; }
  const MotionPointerAndroid& historical(int pos, int pointerIndex) const
  {
    return history[pos * pointerCount + pointerIndex];
  }
};

// Translates Android motion events into os::Event mouse events (ADR
// 0006 sections 2, 3 and 10, ROADMAP M2.1/M2.2). Pure C++: the caller
// (AppAndroid::processInput()) converts each GameActivityMotionEvent
// into a MotionEventAndroid, passes the current time, and queues the
// returned events.
//
// What is emitted:
// - Only one pointer drives the mouse at a time: the stylus, the
//   first finger of a touch stream, or a mouse. Extra fingers are
//   ignored (multi-touch gestures and palm logic are M3).
// - Stylus/eraser: MouseDown/MouseMove/MouseUp with LeftButton,
//   PointerType::Pen (Eraser for TOOL_TYPE_ERASER) and the pressure.
//   BUTTON_STYLUS_PRIMARY/SECONDARY at touch down select the
//   RightButton/MiddleButton. Every historical sample becomes its own
//   MouseMove.
// - Hover (stylus or mouse): MouseEnter + MouseMove (no button,
//   pressure 0). HOVER_EXIT (and stylus UP/CANCEL) schedules a
//   MouseLeave that is dropped if the pointer comes back (DOWN or
//   HOVER_ENTER/MOVE) before the delay ends: Android sends HOVER_EXIT
//   just before the stylus DOWN and HOVER_ENTER just after the UP.
// - The pen is "in range" from the first hover/down until its
//   MouseLeave is emitted; a finger stream that starts while the pen
//   is in range is ignored completely (stylus priority), and the pen
//   entering the range ends the current finger stream.
// - Finger (TOOL_TYPE_FINGER/UNKNOWN): MouseDown/MouseMove/MouseUp
//   with LeftButton, PointerType::Touch, pressure 0. No MouseLeave
//   after the finger lifts (like os/win), so the last position stays
//   as the mouse position. TOOL_TYPE_PALM ends the finger stream.
// - Mouse: buttons from buttonState, PointerType::Mouse, ACTION_SCROLL
//   as MouseWheel with delta.y = -AXIS_VSCROLL and delta.x =
//   +AXIS_HSCROLL (the sign contract of NOTES.md, like os/x11).
// - ACTION_CANCEL: MouseUp of the held button (placeholder until M3.1,
//   which cancels the stroke instead).
// - A MouseMove is always emitted at the position of a MouseDown
//   before it (ui::Manager picks the target widget from the last
//   move), and before MouseUp if the position changed.
// - A second press of the same pointer type and button in less than
//   doubleClickTime and doubleClickSlop pixels is a MouseDoubleClick
//   instead of a MouseDown (like os/x11; ui::Widget turns unused
//   double clicks into a MouseDown).
// - Positions are window pixels divided by the window scale.
//   Modifiers are not set (kKeyUninitializedModifier), so ui::Message
//   asks os::System::keyModifiers().
class PointerInputAndroid {
public:
  // Optional log function (level 0 = verbose, 1 = info).
  using LogFunc = void (*)(int level, const char* msg);

  // Time after HOVER_EXIT before the MouseLeave is emitted.
  static constexpr base::tick_t kLeaveDelay = 50;
  // Time after a stylus UP/CANCEL before the MouseLeave is emitted
  // if no hover event follows.
  static constexpr base::tick_t kLeaveAfterUpDelay = 150;
  static constexpr base::tick_t kDefaultDoubleClickTime = 300;
  static constexpr int kDefaultDoubleClickSlop = 24;

  PointerInputAndroid();

  void setScale(int scale) { m_scale = (scale > 0 ? scale : 1); }
  int scale() const { return m_scale; }

  // Maximum distance in window pixels between two presses of a
  // double click.
  void setDoubleClickSlop(int px) { m_doubleClickSlop = px; }
  void setDoubleClickTime(base::tick_t msecs) { m_doubleClickTime = msecs; }

  void setLogFunc(LogFunc func) { m_log = func; }

  // Translates one motion event, appending the os::Event(s) to "out".
  void processMotion(const MotionEventAndroid& ev, base::tick_t now, std::vector<Event>& out);

  // Emits the deferred MouseLeave if its time has come.
  void processTimers(base::tick_t now, std::vector<Event>& out);

  // Milliseconds until the next processTimers() has something to do,
  // or -1 if there is nothing pending.
  int nextTimeout(base::tick_t now) const;

  // Ends everything (e.g. the app is paused): MouseUp if a button is
  // held, MouseLeave if the mouse is inside.
  void reset(std::vector<Event>& out);

  // Last position in window pixels (not scaled).
  gfx::Point lastPosition() const { return m_lastPx; }

  bool isPenInRange() const { return m_penInRange; }
  bool isButtonPressed() const { return m_button != Event::NoneButton; }

private:
  enum class Owner { None, Touch, Pen, Mouse };

  void processPen(const MotionEventAndroid& ev, base::tick_t now, std::vector<Event>& out);
  void processTouch(const MotionEventAndroid& ev, base::tick_t now, std::vector<Event>& out);
  void processMouse(const MotionEventAndroid& ev, base::tick_t now, std::vector<Event>& out);

  // Ends the current finger stream (if it drives the mouse, releases
  // the button) and ignores it until the next ACTION_DOWN.
  void cancelTouchStream(std::vector<Event>& out, const char* reason);
  void takeOwnership(Owner owner, std::vector<Event>& out);
  void cancelLeave(base::tick_t now, const char* reason);
  void scheduleLeave(base::tick_t now, base::tick_t delay);

  gfx::Point toWindow(float x, float y) const;
  Event makeEvent(Event::Type type, float x, float y, PointerType pointerType, float pressure) const;
  void ensureInside(float x, float y, PointerType pointerType, std::vector<Event>& out);
  void move(const MotionPointerAndroid& p,
            PointerType pointerType,
            bool withPressure,
            bool force,
            std::vector<Event>& out);
  void moveWithHistory(const MotionEventAndroid& ev,
                       int pointerIndex,
                       PointerType pointerType,
                       bool withPressure,
                       std::vector<Event>& out);
  void press(const MotionPointerAndroid& p,
             Event::MouseButton button,
             PointerType pointerType,
             bool withPressure,
             base::tick_t now,
             std::vector<Event>& out);
  void release(float x, float y, std::vector<Event>& out);

  void log(int level, const char* fmt, ...) const;

  int m_scale = 1;
  int m_doubleClickSlop = kDefaultDoubleClickSlop;
  base::tick_t m_doubleClickTime = kDefaultDoubleClickTime;
  LogFunc m_log = nullptr;

  Owner m_owner = Owner::None;
  bool m_inside = false;                           // MouseEnter sent without MouseLeave
  Event::MouseButton m_button = Event::NoneButton; // Button held (MouseDown sent)
  PointerType m_pointerType = PointerType::Unknown;

  // Stylus
  bool m_penInRange = false;
  base::tick_t m_leaveTime = 0;     // When the deferred MouseLeave is emitted (0 = none)
  base::tick_t m_hoverExitTime = 0; // Last HOVER_EXIT (for the order log)
  bool m_hoverOrderLogged = false;

  // Finger stream (ACTION_DOWN..ACTION_UP/CANCEL)
  bool m_touchStream = false;
  bool m_touchIgnored = false;
  int32_t m_touchPointerId = -1; // Finger that drives the mouse (-1 = none)
  bool m_touchIgnoredLogged = false;

  // Last emitted position
  gfx::Point m_lastPx;
  gfx::Point m_lastPos; // Scaled
  bool m_hasLastPos = false;

  // Double click
  Event::MouseButton m_clickButton = Event::NoneButton;
  PointerType m_clickPointerType = PointerType::Unknown;
  base::tick_t m_clickTime = 0;
  gfx::Point m_clickPx;

  // Mouse wheel fractions
  float m_scrollX = 0.0f;
  float m_scrollY = 0.0f;
};

} // namespace os

#endif
