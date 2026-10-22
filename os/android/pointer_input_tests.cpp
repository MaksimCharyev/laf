// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <gtest/gtest.h>

#include "os/android/pointer_input.h"

#include <string>
#include <vector>

using namespace os;
using M = MotionEventAndroid;

namespace {

MotionPointerAndroid ptr(int32_t id, int32_t tool, float x, float y, float pressure = 0.0f)
{
  MotionPointerAndroid p;
  p.id = id;
  p.toolType = tool;
  p.x = x;
  p.y = y;
  p.pressure = pressure;
  return p;
}

M motion(int32_t action, std::initializer_list<MotionPointerAndroid> pointers, int index = 0)
{
  M ev;
  ev.action = action | (index << M::kActionPointerIndexShift);
  for (const auto& p : pointers)
    ev.pointers[ev.pointerCount++] = p;
  return ev;
}

M pen(int32_t action, float x, float y, float pressure = 0.0f)
{
  M ev = motion(action, { ptr(0, M::kToolStylus, x, y, pressure) });
  ev.source = 0x5002; // Stylus device on the MatePad (NOTES.md section 9)
  return ev;
}

M finger(int32_t action, float x, float y)
{
  M ev = motion(action, { ptr(0, M::kToolFinger, x, y, 5.0f) });
  ev.source = 0x1002;
  return ev;
}

const char* key_name(KeyScancode scancode)
{
  switch (scancode) {
    case kKeyPenDoubleTap:   return "PenDoubleTap";
    case kKeyTwoFingerTap:   return "TwoFingerTap";
    case kKeyThreeFingerTap: return "ThreeFingerTap";
    case kKeyPointerCancel:  return "PointerCancel";
    default:                 return "?";
  }
}

std::string event_name(const Event& ev)
{
  switch (ev.type()) {
    case Event::KeyDown:      return std::string("KeyDown:") + key_name(ev.scancode());
    case Event::KeyUp:        return std::string("KeyUp:") + key_name(ev.scancode());
    case Event::TouchMagnify: return "Magnify";
    default:                  break;
  }
  switch (ev.type()) {
    case Event::MouseEnter:       return "Enter";
    case Event::MouseLeave:       return "Leave";
    case Event::MouseMove:        return "Move";
    case Event::MouseDown:        return "Down";
    case Event::MouseUp:          return "Up";
    case Event::MouseWheel:       return "Wheel";
    case Event::MouseDoubleClick: return "DoubleClick";
    default:                      return "?";
  }
}

M fingers(int32_t action, std::initializer_list<MotionPointerAndroid> pointers, int index = 0)
{
  M ev = motion(action, pointers, index);
  ev.source = 0x1002;
  return ev;
}

MotionPointerAndroid f(int32_t id, float x, float y)
{
  return ptr(id, M::kToolFinger, x, y, 5.0f);
}

// Events of the given type.
std::vector<Event> only(const std::vector<Event>& evs, Event::Type type)
{
  std::vector<Event> result;
  for (const auto& ev : evs) {
    if (ev.type() == type)
      result.push_back(ev);
  }
  return result;
}

// Event types as a string, e.g. "Enter Move Down".
std::string types(const std::vector<Event>& evs)
{
  std::string s;
  for (const auto& ev : evs) {
    if (!s.empty())
      s += ' ';
    s += event_name(ev);
  }
  return s;
}

} // anonymous namespace

TEST(PointerInputAndroid, PenTapWithPressure)
{
  PointerInputAndroid input;
  input.setScale(2);
  std::vector<Event> out;

  input.processMotion(pen(M::kActionDown, 101, 51, 0.5f), 1000, out);
  EXPECT_EQ("Enter Move Down", types(out));
  EXPECT_EQ(PointerType::Pen, out[2].pointerType());
  EXPECT_EQ(Event::LeftButton, out[2].button());
  EXPECT_FLOAT_EQ(0.5f, out[2].pressure());
  EXPECT_EQ(gfx::Point(50, 25), out[2].position());
  EXPECT_TRUE(input.isPenInRange());

  out.clear();
  input.processMotion(pen(M::kActionMove, 121, 51, 0.857f), 1010, out);
  EXPECT_EQ("Move", types(out));
  EXPECT_FLOAT_EQ(0.857f, out[0].pressure());
  EXPECT_EQ(gfx::Point(60, 25), out[0].position());

  // The UP sample (pressure 0, other position) doesn't add a move.
  out.clear();
  input.processMotion(pen(M::kActionUp, 141, 51, 0.0f), 1020, out);
  EXPECT_EQ("Up", types(out));
  EXPECT_EQ(gfx::Point(60, 25), out[0].position());
  EXPECT_EQ(Event::LeftButton, out[0].button());

  // No hover after the UP: the pen leaves after kLeaveAfterUpDelay.
  out.clear();
  input.processTimers(1020 + PointerInputAndroid::kLeaveAfterUpDelay - 1, out);
  EXPECT_EQ("", types(out));
  EXPECT_EQ(1, input.nextTimeout(1020 + PointerInputAndroid::kLeaveAfterUpDelay - 1));
  input.processTimers(1020 + PointerInputAndroid::kLeaveAfterUpDelay, out);
  EXPECT_EQ("Leave", types(out));
  EXPECT_FALSE(input.isPenInRange());
  EXPECT_EQ(-1, input.nextTimeout(2000));
}

TEST(PointerInputAndroid, HistoricalSamples)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 10, 10, 0.1f), 0, out);

  M ev = pen(M::kActionMove, 50, 10, 0.5f);
  ev.historySize = 3;
  ev.history = { ptr(0, M::kToolStylus, 20, 10, 0.2f),
                 ptr(0, M::kToolStylus, 30, 10, 0.3f),
                 ptr(0, M::kToolStylus, 40, 10, 0.4f) };
  out.clear();
  input.processMotion(ev, 5, out);
  ASSERT_EQ("Move Move Move Move", types(out));
  EXPECT_EQ(20, out[0].position().x);
  EXPECT_FLOAT_EQ(0.2f, out[0].pressure());
  EXPECT_EQ(40, out[2].position().x);
  EXPECT_EQ(50, out[3].position().x);
  EXPECT_FLOAT_EQ(0.5f, out[3].pressure());
}

TEST(PointerInputAndroid, SamplesInTheSameScaledPixelAreMerged)
{
  PointerInputAndroid input;
  input.setScale(3);
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 30, 30, 0.1f), 0, out);
  out.clear();
  input.processMotion(pen(M::kActionMove, 31, 32, 0.2f), 1, out);
  input.processMotion(pen(M::kActionMove, 32, 31, 0.3f), 2, out);
  EXPECT_EQ("", types(out));
  input.processMotion(pen(M::kActionMove, 33, 31, 0.3f), 3, out);
  EXPECT_EQ("Move", types(out));
  EXPECT_EQ(gfx::Point(11, 10), out[0].position());
}

TEST(PointerInputAndroid, HoverExitBeforeDownDoesNotLeave)
{
  PointerInputAndroid input;
  std::vector<Event> out;

  input.processMotion(pen(M::kActionHoverEnter, 10, 10), 0, out);
  input.processMotion(pen(M::kActionHoverMove, 20, 10), 8, out);
  EXPECT_EQ("Enter Move Move", types(out));
  EXPECT_EQ(Event::NoneButton, out[2].button());
  EXPECT_FLOAT_EQ(0.0f, out[2].pressure());

  // Android order: HOVER_EXIT, DOWN (a few ms later), MOVE, UP,
  // HOVER_ENTER.
  out.clear();
  input.processMotion(pen(M::kActionHoverExit, 20, 10), 16, out);
  EXPECT_EQ("", types(out));
  EXPECT_EQ(PointerInputAndroid::kLeaveDelay, input.nextTimeout(16));
  input.processMotion(pen(M::kActionDown, 20, 10, 0.3f), 20, out);
  EXPECT_EQ("Move Down", types(out));
  EXPECT_EQ(-1, input.nextTimeout(20));

  out.clear();
  input.processMotion(pen(M::kActionUp, 20, 10), 100, out);
  input.processMotion(pen(M::kActionHoverEnter, 20, 10), 105, out);
  input.processMotion(pen(M::kActionHoverMove, 30, 10), 110, out);
  EXPECT_EQ("Up Move", types(out));
  EXPECT_EQ(-1, input.nextTimeout(110));

  // The pen goes away.
  out.clear();
  input.processMotion(pen(M::kActionHoverExit, 30, 10), 200, out);
  input.processTimers(200 + PointerInputAndroid::kLeaveDelay, out);
  EXPECT_EQ("Leave", types(out));
}

TEST(PointerInputAndroid, FingerTap)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  EXPECT_EQ("Enter Move Down", types(out));
  EXPECT_EQ(PointerType::Touch, out[2].pointerType());
  EXPECT_FLOAT_EQ(0.0f, out[2].pressure()); // Finger pressure is not forwarded

  out.clear();
  input.processMotion(finger(M::kActionMove, 110, 100), 10, out);
  input.processMotion(finger(M::kActionUp, 120, 100), 20, out);
  EXPECT_EQ("Move Move Up", types(out));
  EXPECT_EQ(gfx::Point(120, 100), out[2].position());

  // No MouseLeave after a finger.
  out.clear();
  input.processTimers(10000, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, FingersIgnoredWhilePenInRange)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionHoverEnter, 10, 10), 0, out);

  out.clear();
  input.processMotion(finger(M::kActionDown, 500, 500), 10, out);
  input.processMotion(finger(M::kActionMove, 510, 500), 20, out);
  input.processMotion(finger(M::kActionUp, 510, 500), 30, out);
  EXPECT_EQ("", types(out));

  // Still ignored while the deferred MouseLeave is pending.
  input.processMotion(pen(M::kActionHoverExit, 10, 10), 40, out);
  input.processMotion(finger(M::kActionDown, 500, 500), 45, out);
  EXPECT_EQ("", types(out));
  input.processMotion(finger(M::kActionUp, 500, 500), 46, out);

  // The pen left: fingers are still ignored during the rejection
  // timeout, then they work again.
  const base::tick_t left = 40 + PointerInputAndroid::kLeaveDelay;
  input.processTimers(left, out);
  EXPECT_EQ("Leave", types(out));
  EXPECT_FALSE(input.isPenInRange());
  out.clear();
  input.processMotion(finger(M::kActionDown, 500, 500), left + 100, out);
  input.processMotion(finger(M::kActionUp, 500, 500), left + 110, out);
  EXPECT_EQ("", types(out));
  input.processMotion(finger(M::kActionDown, 500, 500),
                      left + PointerInputAndroid::kDefaultPenRejectionTimeout,
                      out);
  EXPECT_EQ("Enter Move Down", types(out));
}

TEST(PointerInputAndroid, PenRejectionTimeoutCanBeZero)
{
  PointerInputAndroid input;
  input.setPenRejectionTimeout(0);
  std::vector<Event> out;
  input.processMotion(pen(M::kActionHoverEnter, 10, 10), 0, out);
  input.processMotion(pen(M::kActionHoverExit, 10, 10), 10, out);
  input.processTimers(10 + PointerInputAndroid::kLeaveDelay, out);
  out.clear();
  input.processMotion(finger(M::kActionDown, 500, 500), 10 + PointerInputAndroid::kLeaveDelay, out);
  EXPECT_EQ("Enter Move Down", types(out));
}

TEST(PointerInputAndroid, PenArrivalEndsFingerStroke)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 500, 500), 0, out);
  out.clear();

  // The finger stroke is cancelled (the palm lands before the pen).
  input.processMotion(pen(M::kActionHoverEnter, 10, 10), 10, out);
  EXPECT_EQ("KeyUp:PointerCancel Up Move", types(out));
  EXPECT_EQ(PointerType::Touch, out[1].pointerType());
  EXPECT_EQ(PointerType::Pen, out[2].pointerType());

  // The rest of the finger stream is ignored.
  out.clear();
  input.processMotion(finger(M::kActionMove, 600, 500), 20, out);
  input.processMotion(finger(M::kActionUp, 600, 500), 30, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, CancelDiscardsTheStroke)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  input.processMotion(finger(M::kActionMove, 150, 100), 10, out);
  out.clear();

  // System palm rejection: KeyUp(PointerCancel) first, so the app
  // discards the stroke, then the MouseUp. Never a KeyDown (Esc
  // KeyDown would deselect).
  input.processMotion(finger(M::kActionCancel, 150, 100), 20, out);
  EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
  EXPECT_FALSE(input.isButtonPressed());

  // Cancel without a pressed button emits nothing.
  out.clear();
  input.processMotion(finger(M::kActionCancel, 150, 100), 30, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, PenCancelDiscardsTheStroke)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 100, 100, 0.5f), 0, out);
  input.processMotion(pen(M::kActionMove, 150, 100, 0.5f), 10, out);
  out.clear();
  input.processMotion(pen(M::kActionCancel, 150, 100), 20, out);
  EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
}

// Palm modelled on the M0.6 log: one finger first (draws/scrolls),
// more fingers, then ACTION_CANCEL 0.1-2 s later.
TEST(PointerInputAndroid, PalmFirstWithLateCancel)
{
  for (const base::tick_t cancelTime : { 100, 2000 }) {
    PointerInputAndroid input;
    std::vector<Event> out;
    input.processMotion(fingers(M::kActionDown, { f(0, 100, 900) }), 0, out);
    EXPECT_EQ("Enter Move Down", types(out));
    out.clear();

    // More contacts of the palm: the one-finger stroke is cancelled
    // at the second one, the fourth one marks the stream as palm.
    input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 900), f(1, 140, 920) }, 1),
                        20,
                        out);
    EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
    out.clear();
    input.processMotion(
      fingers(M::kActionPointerDown, { f(0, 100, 900), f(1, 140, 920), f(2, 180, 930) }, 2),
      30,
      out);
    input.processMotion(fingers(M::kActionPointerDown,
                                { f(0, 100, 900), f(1, 140, 920), f(2, 180, 930), f(3, 220, 950) },
                                3),
                        40,
                        out);
    input.processMotion(
      fingers(M::kActionMove, { f(0, 300, 900), f(1, 340, 920), f(2, 380, 930), f(3, 420, 950) }),
      60,
      out);
    input.processMotion(
      fingers(M::kActionCancel, { f(0, 300, 900), f(1, 340, 920), f(2, 380, 930), f(3, 420, 950) }),
      cancelTime,
      out);
    EXPECT_EQ("", types(out)) << "cancel at " << cancelTime;
  }
}

// The palm rests and the pen arrives (hover) before the system
// CANCEL: the finger stroke is discarded.
TEST(PointerInputAndroid, PalmThenPen)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 900) }), 0, out);
  input.processMotion(fingers(M::kActionMove, { f(0, 200, 900) }), 50, out);
  out.clear();
  input.processMotion(pen(M::kActionHoverEnter, 500, 500), 100, out);
  EXPECT_EQ("KeyUp:PointerCancel Up Move", types(out));

  out.clear();
  input.processMotion(fingers(M::kActionMove, { f(0, 300, 900) }), 150, out);
  input.processMotion(fingers(M::kActionCancel, { f(0, 300, 900) }), 1000, out);
  EXPECT_EQ("", types(out));
}

// The pen draws first, the palm lands while it draws and after it
// lifts (within the rejection timeout): nothing from the fingers.
TEST(PointerInputAndroid, PenFirstThenPalm)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 500, 500, 0.5f), 0, out);
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 900) }), 10, out);
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 900), f(1, 140, 920) }, 1),
                      20,
                      out);
  input.processMotion(pen(M::kActionMove, 520, 500, 0.5f), 30, out);
  input.processMotion(pen(M::kActionUp, 520, 500), 40, out);
  input.processMotion(fingers(M::kActionCancel, { f(0, 100, 900), f(1, 140, 920) }), 150, out);
  EXPECT_EQ("Enter Move Down Move Up", types(out));

  // Palm right after lifting the pen (pen still in range or within
  // the timeout).
  out.clear();
  input.processTimers(40 + PointerInputAndroid::kLeaveAfterUpDelay, out);
  EXPECT_EQ("Leave", types(out));
  out.clear();
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 900) }), 400, out);
  input.processMotion(fingers(M::kActionMove, { f(0, 200, 900) }), 450, out);
  input.processMotion(fingers(M::kActionUp, { f(0, 200, 900) }), 500, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, TwoFingerTap)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
  out.clear();
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 100), f(1, 300, 100) }, 1),
                      40,
                      out);
  EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
  out.clear();
  input.processMotion(fingers(M::kActionMove, { f(0, 105, 103), f(1, 296, 100) }), 80, out);
  input.processMotion(fingers(M::kActionPointerUp, { f(0, 105, 103), f(1, 296, 100) }, 1),
                      150,
                      out);
  input.processMotion(fingers(M::kActionUp, { f(0, 105, 103) }), 170, out);
  EXPECT_EQ("KeyDown:TwoFingerTap KeyUp:TwoFingerTap", types(out));
}

TEST(PointerInputAndroid, ThreeFingerTap)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 100), f(1, 300, 100) }, 1),
                      30,
                      out);
  input.processMotion(
    fingers(M::kActionPointerDown, { f(0, 100, 100), f(1, 300, 100), f(2, 500, 100) }, 2),
    60,
    out);
  input.processMotion(
    fingers(M::kActionPointerUp, { f(0, 100, 100), f(1, 300, 100), f(2, 500, 100) }, 0),
    200,
    out);
  input.processMotion(fingers(M::kActionPointerUp, { f(1, 300, 100), f(2, 500, 100) }, 0),
                      210,
                      out);
  out.clear();
  input.processMotion(fingers(M::kActionUp, { f(2, 500, 100) }), 220, out);
  EXPECT_EQ("KeyDown:ThreeFingerTap KeyUp:ThreeFingerTap", types(out));
}

TEST(PointerInputAndroid, NotATap)
{
  struct Case {
    const char* name;
    base::tick_t secondDown;
    float moveX;
    base::tick_t up;
    bool cancel;
  };
  for (const Case& c : {
         Case{ "late second finger", PointerInputAndroid::kTapDownWindow + 1, 0,  250,                                      false },
         Case{ "moved",              30,                                      40, 250,                                      false },
         Case{ "too long",           30,                                      0,  PointerInputAndroid::kTapMaxDuration + 1, false },
         Case{ "cancelled",          30,                                      0,  250,                                      true  },
  }) {
    PointerInputAndroid input;
    std::vector<Event> out;
    input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
    input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 100), f(1, 300, 100) }, 1),
                        c.secondDown,
                        out);
    input.processMotion(fingers(M::kActionMove, { f(0, 100 + c.moveX, 100), f(1, 300, 100) }),
                        c.secondDown + 10,
                        out);
    out.clear();
    if (c.cancel) {
      input.processMotion(fingers(M::kActionCancel, { f(0, 100, 100), f(1, 300, 100) }), c.up, out);
    }
    else {
      input.processMotion(
        fingers(M::kActionPointerUp, { f(0, 100 + c.moveX, 100), f(1, 300, 100) }, 1),
        c.up - 5,
        out);
      input.processMotion(fingers(M::kActionUp, { f(0, 100 + c.moveX, 100) }), c.up, out);
    }
    EXPECT_TRUE(only(out, Event::KeyDown).empty()) << c.name;
  }

  // A one-finger tap is a click, not a gesture key.
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
  input.processMotion(fingers(M::kActionUp, { f(0, 100, 100) }), 50, out);
  EXPECT_EQ("Enter Move Down Up", types(out));
}

TEST(PointerInputAndroid, TwoFingerPan)
{
  PointerInputAndroid input;
  input.setScale(2);
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 100, 100), f(1, 300, 100) }, 1),
                      20,
                      out);
  out.clear();

  // Movement inside the tap slop: nothing yet.
  input.processMotion(fingers(M::kActionMove, { f(0, 110, 100), f(1, 310, 100) }), 40, out);
  EXPECT_EQ("", types(out));

  // Beyond the slop: a MouseMove at the center (target widget), then
  // the precise wheel with the whole movement since the second finger
  // landed (+40 px right = -20 scaled px of scroll).
  input.processMotion(fingers(M::kActionMove, { f(0, 140, 100), f(1, 340, 100) }), 60, out);
  ASSERT_EQ("Move Wheel", types(out));
  EXPECT_EQ(gfx::Point(100, 50), out[0].position());
  EXPECT_EQ(PointerType::Touch, out[0].pointerType());
  EXPECT_EQ(gfx::Point(120, 50), out[1].position());
  EXPECT_EQ(gfx::Point(-20, 0), out[1].wheelDelta());
  EXPECT_TRUE(out[1].preciseWheel());
  EXPECT_EQ(PointerType::Touchpad, out[1].pointerType());

  // Fractions are accumulated (1 px at scale 2 = half a step).
  out.clear();
  input.processMotion(fingers(M::kActionMove, { f(0, 140, 101), f(1, 340, 101) }), 70, out);
  EXPECT_EQ("", types(out));
  input.processMotion(fingers(M::kActionMove, { f(0, 140, 102), f(1, 340, 102) }), 80, out);
  ASSERT_EQ("Wheel", types(out));
  EXPECT_EQ(gfx::Point(0, -1), out[0].wheelDelta());

  // Lifting a finger ends the gesture; the other finger does nothing.
  out.clear();
  input.processMotion(fingers(M::kActionPointerUp, { f(0, 140, 102), f(1, 340, 102) }, 1), 90, out);
  input.processMotion(fingers(M::kActionMove, { f(0, 300, 300) }), 100, out);
  input.processMotion(fingers(M::kActionUp, { f(0, 300, 300) }), 110, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, PinchZoom)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 400, 500) }), 0, out);
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 400, 500), f(1, 600, 500) }, 1),
                      20,
                      out);
  out.clear();

  // Fingers apart around the same center: zoom in, no pan.
  input.processMotion(fingers(M::kActionMove, { f(0, 350, 500), f(1, 650, 500) }), 40, out);
  ASSERT_EQ("Move Magnify", types(out));
  EXPECT_EQ(gfx::Point(500, 500), out[1].position());
  EXPECT_FLOAT_EQ(0.5f, out[1].magnification());
  EXPECT_EQ(PointerType::Touchpad, out[1].pointerType());

  // Together: zoom out.
  out.clear();
  input.processMotion(fingers(M::kActionMove, { f(0, 425, 500), f(1, 575, 500) }), 60, out);
  ASSERT_EQ("Magnify", types(out));
  EXPECT_FLOAT_EQ(-0.5f, out[0].magnification());

  // A third finger ends the gesture.
  out.clear();
  input.processMotion(
    fingers(M::kActionPointerDown, { f(0, 425, 500), f(1, 575, 500), f(2, 800, 800) }, 2),
    80,
    out);
  input.processMotion(fingers(M::kActionMove, { f(0, 300, 500), f(1, 700, 500), f(2, 800, 800) }),
                      100,
                      out);
  EXPECT_EQ("", types(out));
}

// With finger drawing, a two-finger gesture must not leave a dot:
// the first finger's MouseDown is cancelled when the second lands.
TEST(PointerInputAndroid, GestureCancelsOneFingerStroke)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(fingers(M::kActionDown, { f(0, 100, 100) }), 0, out);
  input.processMotion(fingers(M::kActionMove, { f(0, 110, 100) }), 10, out);
  out.clear();
  input.processMotion(fingers(M::kActionPointerDown, { f(0, 110, 100), f(1, 300, 100) }, 1),
                      500,
                      out);
  EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
  out.clear();
  input.processMotion(fingers(M::kActionMove, { f(0, 110, 200), f(1, 300, 200) }), 520, out);
  EXPECT_EQ("Move Wheel", types(out));
}

TEST(PointerInputAndroid, ActionIndexOutOfRange)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  out.clear();

  // 9th pointer (not copied by GameActivity): a palm.
  M ev;
  ev.action = M::kActionPointerDown | (8 << M::kActionPointerIndexShift);
  for (int i = 0; i < M::kMaxPointers; ++i)
    ev.pointers[ev.pointerCount++] = ptr(i, M::kToolFinger, 100.0f + i, 100);
  input.processMotion(ev, 10, out);
  EXPECT_EQ("KeyUp:PointerCancel Up", types(out));
  EXPECT_FALSE(input.isButtonPressed());
  out.clear();
  ev.action = M::kActionPointerUp | (8 << M::kActionPointerIndexShift);
  input.processMotion(ev, 20, out);
  EXPECT_EQ("", types(out));

  // Invalid pointer counts are ignored.
  ev.pointerCount = 0;
  input.processMotion(ev, 30, out);
  ev.pointerCount = M::kMaxPointers + 1;
  input.processMotion(ev, 30, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, PenDoubleTapKey)
{
  const int32_t kDown = 0;
  const int32_t kUp = 1;
  const int32_t kKey = PointerInputAndroid::kPenDoubleTapKeyCode;

  // Two DOWN/UP pairs 6 ms and 16 ms apart = one gesture each time.
  for (const base::tick_t gap : { 6, 16 }) {
    PointerInputAndroid input;
    std::vector<Event> out;
    EXPECT_TRUE(input.processKey(kDown, kKey, 0, 1000, out));
    EXPECT_TRUE(input.processKey(kUp, kKey, 0, 1002, out));
    EXPECT_TRUE(input.processKey(kDown, kKey, 0, 1000 + gap, out));
    EXPECT_TRUE(input.processKey(kUp, kKey, 0, 1002 + gap, out));
    EXPECT_EQ("KeyDown:PenDoubleTap KeyUp:PenDoubleTap", types(out)) << "gap " << gap;
  }

  // Two double-taps 400 ms apart = two gestures; repeats dropped.
  PointerInputAndroid input;
  std::vector<Event> out;
  for (const base::tick_t t : { 1000, 1400 }) {
    input.processKey(kDown, kKey, 0, t, out);
    input.processKey(kUp, kKey, 0, t + 2, out);
    input.processKey(kDown, kKey, 0, t + 10, out);
    input.processKey(kDown, kKey, 1, t + 11, out);
    input.processKey(kUp, kKey, 0, t + 12, out);
  }
  EXPECT_EQ("KeyDown:PenDoubleTap KeyUp:PenDoubleTap KeyDown:PenDoubleTap KeyUp:PenDoubleTap",
            types(out));

  // Other keys are not handled here.
  out.clear();
  EXPECT_FALSE(PointerInputAndroid::isGestureKey(4)); // AKEYCODE_BACK
  EXPECT_FALSE(input.processKey(kDown, 4, 0, 3000, out));
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, DoubleTap)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 100, 100, 0.5f), 0, out);
  input.processMotion(pen(M::kActionUp, 100, 100), 50, out);
  out.clear();
  input.processMotion(pen(M::kActionDown, 105, 102, 0.5f), 150, out);
  EXPECT_EQ("Move DoubleClick", types(out));
  input.processMotion(pen(M::kActionUp, 105, 102), 200, out);

  // Too late for a double click (the pen also left the range).
  out.clear();
  input.processMotion(pen(M::kActionDown, 105, 102, 0.5f),
                      200 + PointerInputAndroid::kDefaultDoubleClickTime + 1,
                      out);
  EXPECT_EQ("Leave Enter Move Down", types(out));
}

TEST(PointerInputAndroid, StylusButtonAndEraser)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  M ev = pen(M::kActionDown, 10, 10, 0.5f);
  ev.buttonState = M::kButtonStylusPrimary;
  input.processMotion(ev, 0, out);
  EXPECT_EQ(Event::RightButton, out.back().button());
  input.processMotion(pen(M::kActionUp, 10, 10), 10, out);

  out.clear();
  ev = motion(M::kActionDown, { ptr(0, M::kToolEraser, 400, 10, 0.5f) });
  input.processMotion(ev, 1000, out);
  EXPECT_EQ(PointerType::Eraser, out.back().pointerType());
  EXPECT_EQ(Event::LeftButton, out.back().button());
}

TEST(PointerInputAndroid, MouseWheelSign)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  M ev = motion(M::kActionScroll, { ptr(0, M::kToolMouse, 10, 10) });
  ev.source = M::kSourceMouse;
  ev.vscroll = 1.0f; // Wheel up
  input.processMotion(ev, 0, out);
  ASSERT_EQ("Enter Wheel", types(out));
  EXPECT_EQ(gfx::Point(0, -1), out[1].wheelDelta());
  EXPECT_EQ(PointerType::Mouse, out[1].pointerType());

  out.clear();
  ev.vscroll = 0.0f;
  ev.hscroll = 0.5f;
  input.processMotion(ev, 10, out);
  EXPECT_EQ("", types(out));
  input.processMotion(ev, 20, out);
  ASSERT_EQ("Wheel", types(out));
  EXPECT_EQ(gfx::Point(1, 0), out[0].wheelDelta());
}

TEST(PointerInputAndroid, MouseButtons)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  M ev = motion(M::kActionDown, { ptr(0, M::kToolMouse, 10, 10) });
  ev.buttonState = M::kButtonSecondary;
  input.processMotion(ev, 0, out);
  EXPECT_EQ("Enter Move Down", types(out));
  EXPECT_EQ(Event::RightButton, out.back().button());
  EXPECT_EQ(PointerType::Mouse, out.back().pointerType());
  out.clear();
  ev.action = M::kActionUp;
  ev.buttonState = 0;
  input.processMotion(ev, 10, out);
  EXPECT_EQ("Up", types(out));
  EXPECT_EQ(Event::RightButton, out.back().button());
}

TEST(PointerInputAndroid, Reset)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(pen(M::kActionDown, 10, 10, 0.5f), 0, out);
  out.clear();
  input.reset(out);
  EXPECT_EQ("Up Leave", types(out));
  EXPECT_FALSE(input.isPenInRange());
  EXPECT_EQ(-1, input.nextTimeout(0));
}

int app_main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
