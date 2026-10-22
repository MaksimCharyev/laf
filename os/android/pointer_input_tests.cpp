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

const char* type_name(Event::Type type)
{
  switch (type) {
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

// Event types as a string, e.g. "Enter Move Down".
std::string types(const std::vector<Event>& evs)
{
  std::string s;
  for (const auto& ev : evs) {
    if (!s.empty())
      s += ' ';
    s += type_name(ev.type());
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

  // After the pen left, fingers work again.
  input.processTimers(40 + PointerInputAndroid::kLeaveDelay, out);
  EXPECT_EQ("Leave", types(out));
  out.clear();
  input.processMotion(finger(M::kActionDown, 500, 500), 200, out);
  EXPECT_EQ("Enter Move Down", types(out));
}

TEST(PointerInputAndroid, PenArrivalEndsFingerStroke)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 500, 500), 0, out);
  out.clear();

  input.processMotion(pen(M::kActionHoverEnter, 10, 10), 10, out);
  EXPECT_EQ("Up Move", types(out));
  EXPECT_EQ(PointerType::Touch, out[0].pointerType());
  EXPECT_EQ(PointerType::Pen, out[1].pointerType());

  // The rest of the finger stream is ignored.
  out.clear();
  input.processMotion(finger(M::kActionMove, 600, 500), 20, out);
  input.processMotion(finger(M::kActionUp, 600, 500), 30, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, CancelReleasesTheButton)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  input.processMotion(finger(M::kActionMove, 150, 100), 10, out);
  out.clear();
  input.processMotion(finger(M::kActionCancel, 150, 100), 20, out);
  EXPECT_EQ("Up", types(out));
  EXPECT_FALSE(input.isButtonPressed());

  // Cancel without a pressed button emits nothing.
  out.clear();
  input.processMotion(finger(M::kActionCancel, 150, 100), 30, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, OnlyTheFirstFingerDrivesTheMouse)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  out.clear();

  M ev = motion(M::kActionPointerDown,
                { ptr(0, M::kToolFinger, 100, 100), ptr(1, M::kToolFinger, 300, 300) },
                1);
  input.processMotion(ev, 10, out);
  EXPECT_EQ("", types(out));

  ev = motion(M::kActionMove,
              { ptr(0, M::kToolFinger, 110, 100), ptr(1, M::kToolFinger, 320, 300) });
  input.processMotion(ev, 20, out);
  ASSERT_EQ("Move", types(out));
  EXPECT_EQ(gfx::Point(110, 100), out[0].position());

  // The first finger is lifted: MouseUp, the second one is ignored.
  out.clear();
  ev = motion(M::kActionPointerUp,
              { ptr(0, M::kToolFinger, 110, 100), ptr(1, M::kToolFinger, 330, 300) },
              0);
  input.processMotion(ev, 30, out);
  EXPECT_EQ("Up", types(out));
  out.clear();
  input.processMotion(motion(M::kActionMove, { ptr(1, M::kToolFinger, 340, 300) }), 40, out);
  input.processMotion(motion(M::kActionUp, { ptr(1, M::kToolFinger, 340, 300) }), 50, out);
  EXPECT_EQ("", types(out));
}

TEST(PointerInputAndroid, ActionIndexOutOfRange)
{
  PointerInputAndroid input;
  std::vector<Event> out;
  input.processMotion(finger(M::kActionDown, 100, 100), 0, out);
  out.clear();

  // 9th pointer (not copied by GameActivity).
  M ev;
  ev.action = M::kActionPointerDown | (8 << M::kActionPointerIndexShift);
  for (int i = 0; i < M::kMaxPointers; ++i)
    ev.pointers[ev.pointerCount++] = ptr(i, M::kToolFinger, 100.0f + i, 100);
  input.processMotion(ev, 10, out);
  ev.action = M::kActionPointerUp | (8 << M::kActionPointerIndexShift);
  input.processMotion(ev, 20, out);
  EXPECT_EQ("", types(out));
  EXPECT_TRUE(input.isButtonPressed());

  // Invalid pointer counts are ignored.
  ev.pointerCount = 0;
  input.processMotion(ev, 30, out);
  ev.pointerCount = M::kMaxPointers + 1;
  input.processMotion(ev, 30, out);
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
