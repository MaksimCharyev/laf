// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <gtest/gtest.h>

#include <vector>

#include "os/android/keys.h"

using namespace os;
using K = KeyEventAndroid;

namespace {

// Android key codes used in the tests (android.view.KeyEvent).
enum {
  BACK = 4,
  A = 29,
  B = 30,
  H = 36,
  J = 38,
  Z = 54,
  ALT_LEFT = 57,
  SHIFT_LEFT = 59,
  SPACE = 62,
  DEL = 67,
  CTRL_LEFT = 113,
  ENTER = 66,
  ESCAPE = 111,
  NUMPAD_5 = 149,
  F12 = 142,
  PLUS = 81, // No scancode
  UNKNOWN = 0,
};

K key(int action, int keyCode, int meta = 0, int unicode = 0, int repeat = 0)
{
  K ev;
  ev.action = action;
  ev.keyCode = keyCode;
  ev.metaState = meta;
  ev.unicodeChar = unicode;
  ev.repeatCount = repeat;
  return ev;
}

// Device ids from the Huawei MatePad log (NOTES.md section 6).
constexpr int kHw = 131; // Huawei Magnetic Keyboard
constexpr int kIme = -1; // KeyCharacterMap.VIRTUAL_KEYBOARD
constexpr int kHwMeta = 0x200000;

// Key event of a device at a time in milliseconds.
K dev(int action, int keyCode, int device, int64_t ms, int repeat = 0, int unicode = 0)
{
  K ev = key(action, keyCode, (device == kHw ? kHwMeta : 0), unicode, repeat);
  ev.deviceId = device;
  ev.scanCode = (device == kHw ? 14 : 0);
  ev.eventTime = ms * 1000000;
  return ev;
}

// Feeds the events and returns the queued os::Events.
std::vector<Event> feed(KeyInputAndroid& input, const std::vector<K>& evs)
{
  std::vector<Event> out;
  for (const K& k : evs) {
    Event ev;
    if (input.processKey(k, ev))
      out.push_back(ev);
  }
  return out;
}

} // anonymous namespace

TEST(AndroidKeys, Table)
{
  EXPECT_EQ(kKeyA, android_keycode_to_scancode(A));
  EXPECT_EQ(kKeyZ, android_keycode_to_scancode(Z));
  EXPECT_EQ(kKey0, android_keycode_to_scancode(7));
  EXPECT_EQ(kKey9, android_keycode_to_scancode(16));
  EXPECT_EQ(kKeyEsc, android_keycode_to_scancode(BACK));
  EXPECT_EQ(kKeyEsc, android_keycode_to_scancode(ESCAPE));
  EXPECT_EQ(kKeyBackspace, android_keycode_to_scancode(DEL));
  EXPECT_EQ(kKeyDel, android_keycode_to_scancode(112));
  EXPECT_EQ(kKeyEnter, android_keycode_to_scancode(ENTER));
  EXPECT_EQ(kKeySpace, android_keycode_to_scancode(SPACE));
  EXPECT_EQ(kKeyLShift, android_keycode_to_scancode(SHIFT_LEFT));
  EXPECT_EQ(kKeyLControl, android_keycode_to_scancode(CTRL_LEFT));
  EXPECT_EQ(kKeyAlt, android_keycode_to_scancode(ALT_LEFT));
  EXPECT_EQ(kKey5Pad, android_keycode_to_scancode(NUMPAD_5));
  EXPECT_EQ(kKeyF12, android_keycode_to_scancode(F12));
  EXPECT_EQ(kKeyNil, android_keycode_to_scancode(PLUS));
  EXPECT_EQ(kKeyNil, android_keycode_to_scancode(UNKNOWN));
  EXPECT_EQ(kKeyNil, android_keycode_to_scancode(K::kKeyCodePenDoubleTap));
}

TEST(AndroidKeys, Modifiers)
{
  EXPECT_EQ(kKeyNoneModifier, android_meta_to_modifiers(0));
  EXPECT_EQ(kKeyShiftModifier, android_meta_to_modifiers(K::kMetaShiftOn | K::kMetaShiftLeftOn));
  EXPECT_EQ(kKeyCtrlModifier | kKeyAltModifier,
            android_meta_to_modifiers(K::kMetaCtrlOn | K::kMetaAltOn));
  EXPECT_EQ(kKeyWinModifier, android_meta_to_modifiers(K::kMetaMetaOn));
}

TEST(AndroidKeys, TypeLetter)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, B, 0, 'b'), ev));
  EXPECT_EQ(Event::KeyDown, ev.type());
  EXPECT_EQ(kKeyB, ev.scancode());
  EXPECT_EQ('b', ev.unicodeChar());
  EXPECT_EQ(kKeyNoneModifier, ev.modifiers());
  EXPECT_EQ(0, ev.repeat());
  EXPECT_TRUE(input.isKeyPressed(kKeyB));

  ASSERT_TRUE(input.processKey(key(K::kActionDown, B, 0, 'b', 1), ev));
  EXPECT_EQ(1, ev.repeat());

  ASSERT_TRUE(input.processKey(key(K::kActionUp, B, 0, 'b'), ev));
  EXPECT_EQ(Event::KeyUp, ev.type());
  EXPECT_EQ(kKeyB, ev.scancode());
  EXPECT_FALSE(input.isKeyPressed(kKeyB));
}

TEST(AndroidKeys, CtrlZ)
{
  KeyInputAndroid input;
  Event ev;
  const int ctrl = K::kMetaCtrlOn | K::kMetaCtrlLeftOn;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, CTRL_LEFT, ctrl), ev));
  EXPECT_EQ(kKeyLControl, ev.scancode());
  EXPECT_EQ(kKeyCtrlModifier, ev.modifiers());
  EXPECT_TRUE(input.isKeyPressed(kKeyLControl));

  // getUnicodeChar() ignores Ctrl.
  ASSERT_TRUE(input.processKey(key(K::kActionDown, Z, ctrl, 'z'), ev));
  EXPECT_EQ(kKeyZ, ev.scancode());
  EXPECT_EQ(kKeyCtrlModifier, ev.modifiers());
  EXPECT_EQ(kKeyCtrlModifier, input.modifiers());

  ASSERT_TRUE(input.processKey(key(K::kActionUp, Z, ctrl, 'z'), ev));
  ASSERT_TRUE(input.processKey(key(K::kActionUp, CTRL_LEFT, 0), ev));
  EXPECT_EQ(kKeyNoneModifier, ev.modifiers());
  EXPECT_FALSE(input.isKeyPressed(kKeyLControl));
  EXPECT_EQ(kKeyNoneModifier, input.modifiers());
}

TEST(AndroidKeys, ShiftedCharacter)
{
  KeyInputAndroid input;
  Event ev;
  const int shift = K::kMetaShiftOn | K::kMetaShiftLeftOn;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, SHIFT_LEFT, shift), ev));
  ASSERT_TRUE(input.processKey(key(K::kActionDown, A, shift, 'A'), ev));
  EXPECT_EQ(kKeyA, ev.scancode());
  EXPECT_EQ('A', ev.unicodeChar());
  EXPECT_EQ(kKeyShiftModifier, ev.modifiers());

  // The shifted character is not remembered for shortcuts.
  EXPECT_EQ('a', input.unicodeFromScancode(kKeyA));
}

TEST(AndroidKeys, SpaceModifier)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, SPACE, 0, ' '), ev));
  EXPECT_EQ(kKeySpace, ev.scancode());
  EXPECT_EQ(kKeySpaceModifier, ev.modifiers());
  EXPECT_TRUE(input.isKeyPressed(kKeySpace));

  ASSERT_TRUE(input.processKey(key(K::kActionUp, SPACE, 0, ' '), ev));
  EXPECT_EQ(kKeyNoneModifier, ev.modifiers());
}

TEST(AndroidKeys, BackIsEsc)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, BACK), ev));
  EXPECT_EQ(Event::KeyDown, ev.type());
  EXPECT_EQ(kKeyEsc, ev.scancode());
  EXPECT_EQ(0, ev.unicodeChar());
  ASSERT_TRUE(input.processKey(key(K::kActionUp, BACK), ev));
  EXPECT_EQ(Event::KeyUp, ev.type());
  EXPECT_EQ(kKeyEsc, ev.scancode());
}

TEST(AndroidKeys, IgnoredKeys)
{
  KeyInputAndroid input;
  Event ev;

  // Pen double-tap is handled elsewhere (ADR 0006 section 6).
  EXPECT_FALSE(input.processKey(key(K::kActionDown, K::kKeyCodePenDoubleTap), ev));
  EXPECT_FALSE(input.processKey(key(K::kActionUp, K::kKeyCodePenDoubleTap), ev));

  // Unknown key without character.
  EXPECT_FALSE(input.processKey(key(K::kActionDown, UNKNOWN), ev));

  // ACTION_MULTIPLE.
  EXPECT_FALSE(input.processKey(key(2, A, 0, 'a'), ev));
}

TEST(AndroidKeys, CharacterWithoutScancode)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, PLUS, 0, '+'), ev));
  EXPECT_EQ(kKeyNil, ev.scancode());
  EXPECT_EQ('+', ev.unicodeChar());

  // Cyrillic layout: the scancode is the Latin key, the character is
  // the Cyrillic one.
  ASSERT_TRUE(input.processKey(key(K::kActionDown, A, 0, 0x0444), ev));
  EXPECT_EQ(kKeyA, ev.scancode());
  EXPECT_EQ(0x0444, ev.unicodeChar());
  EXPECT_EQ(0x0444, input.unicodeFromScancode(kKeyA));
}

TEST(AndroidKeys, DeadKey)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, 68, 0, int(K::kCombiningAccent | 0x300)), ev));
  EXPECT_EQ(kKeyTilde, ev.scancode());
  EXPECT_EQ(0, ev.unicodeChar());
}

TEST(AndroidKeys, StaleModifierAndReset)
{
  KeyInputAndroid input;
  Event ev;
  const int ctrl = K::kMetaCtrlOn | K::kMetaCtrlLeftOn;

  ASSERT_TRUE(input.processKey(key(K::kActionDown, CTRL_LEFT, ctrl), ev));
  EXPECT_TRUE(input.isKeyPressed(kKeyLControl));

  // The UP of Ctrl was lost (e.g. released in another app): the next
  // key without the Ctrl bit releases it.
  ASSERT_TRUE(input.processKey(key(K::kActionDown, A, 0, 'a'), ev));
  EXPECT_FALSE(input.isKeyPressed(kKeyLControl));
  EXPECT_EQ(kKeyNoneModifier, ev.modifiers());

  input.reset();
  EXPECT_FALSE(input.isKeyPressed(kKeyA));
  EXPECT_EQ(kKeyNoneModifier, input.modifiers());
}

TEST(AndroidKeys, UnicodeFromScancode)
{
  KeyInputAndroid input;
  EXPECT_EQ('b', input.unicodeFromScancode(kKeyB));
  EXPECT_EQ('1', input.unicodeFromScancode(kKey1));
  EXPECT_EQ('/', input.unicodeFromScancode(kKeySlash));
  EXPECT_EQ(0, input.unicodeFromScancode(kKeyF1));
  EXPECT_EQ(0, input.unicodeFromScancode(kKeyNil));
}

// 19:51:53.719 kc=67 dev=-1, 19:51:53.734 kc=67 dev=131: the IME
// copy first.
TEST(AndroidKeys, DuplicateImeFirst)
{
  KeyInputAndroid input;
  Event ev;

  ASSERT_TRUE(input.processKey(dev(K::kActionDown, DEL, kIme, 53719), ev));
  EXPECT_EQ(kKeyBackspace, ev.scancode());
  EXPECT_FALSE(input.lastWasDuplicate());
  EXPECT_FALSE(input.processKey(dev(K::kActionDown, DEL, kHw, 53734), ev));
  EXPECT_TRUE(input.lastWasDuplicate());
  EXPECT_TRUE(input.isKeyPressed(kKeyBackspace));

  // UP from the hardware first: still accepted (the key is pressed),
  // the IME one is the duplicate.
  ASSERT_TRUE(input.processKey(dev(K::kActionUp, DEL, kHw, 53820), ev));
  EXPECT_EQ(Event::KeyUp, ev.type());
  EXPECT_FALSE(input.isKeyPressed(kKeyBackspace));
  EXPECT_FALSE(input.processKey(dev(K::kActionUp, DEL, kIme, 53830), ev));
  EXPECT_TRUE(input.lastWasDuplicate());
}

// 19:51:54.424 kc=38 dev=-1, 19:51:54.439 kc=38 dev=131, typing "j".
TEST(AndroidKeys, DuplicateLetter)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, J, kIme, 54424, 0, 'j'),
                          dev(K::kActionDown, J, kHw, 54439, 0, 'j'),
                          dev(K::kActionUp, J, kIme, 54510, 0, 'j'),
                          dev(K::kActionUp, J, kHw, 54522, 0, 'j'),
                        });
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ(Event::KeyDown, out[0].type());
  EXPECT_EQ('j', out[0].unicodeChar());
  EXPECT_EQ(Event::KeyUp, out[1].type());
  EXPECT_FALSE(input.isKeyPressed(kKeyJ));
}

TEST(AndroidKeys, DuplicateHardwareFirst)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, H, kHw, 1000, 0, 'h'),
                          dev(K::kActionDown, H, kIme, 1008, 0, 'h'),
                          dev(K::kActionUp, H, kIme, 1090, 0, 'h'),
                          dev(K::kActionUp, H, kHw, 1100, 0, 'h'),
                          // Space doubled too.
                          dev(K::kActionDown, SPACE, kHw, 1200, 0, ' '),
                          dev(K::kActionDown, SPACE, kIme, 1215, 0, ' '),
                          dev(K::kActionUp, SPACE, kHw, 1290, 0, ' '),
                          dev(K::kActionUp, SPACE, kIme, 1300, 0, ' '),
                        });
  ASSERT_EQ(4u, out.size());
  EXPECT_EQ(Event::KeyDown, out[0].type());
  EXPECT_EQ(kKeyH, out[0].scancode());
  EXPECT_EQ(Event::KeyUp, out[1].type());
  EXPECT_EQ(Event::KeyDown, out[2].type());
  EXPECT_EQ(kKeySpace, out[2].scancode());
  EXPECT_EQ(Event::KeyUp, out[3].type());
  EXPECT_FALSE(input.isKeyPressed(kKeyH));
  EXPECT_FALSE(input.isKeyPressed(kKeySpace));
  EXPECT_EQ(kKeyNoneModifier, input.modifiers());
}

// Backspace held: hardware auto-repeat every ~50 ms, each repeat also
// re-injected by the IME.
TEST(AndroidKeys, DuplicateAutoRepeat)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, DEL, kHw, 0),
                          dev(K::kActionDown, DEL, kIme, 12),
                          dev(K::kActionDown, DEL, kHw, 400, 1),
                          dev(K::kActionDown, DEL, kIme, 410, 1),
                          // IME first, but 45 ms after the accepted hardware repeat 1:
                          // dropped, the hardware stays the source.
                          dev(K::kActionDown, DEL, kIme, 445, 2),
                          dev(K::kActionDown, DEL, kHw, 450, 2),
                          dev(K::kActionDown, DEL, kHw, 500, 3),
                          dev(K::kActionDown, DEL, kIme, 512, 3),
                          dev(K::kActionUp, DEL, kHw, 560),
                          dev(K::kActionUp, DEL, kIme, 571),
                        });
  ASSERT_EQ(5u, out.size());
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(Event::KeyDown, out[i].type());
    EXPECT_EQ(kKeyBackspace, out[i].scancode());
    EXPECT_EQ(i, out[i].repeat());
  }
  EXPECT_EQ(Event::KeyUp, out[4].type());
  EXPECT_FALSE(input.isKeyPressed(kKeyBackspace));
}

// Auto-repeat from the hardware only (the IME doesn't re-inject it),
// after the first DOWN was taken from the IME.
TEST(AndroidKeys, AutoRepeatFromOtherDeviceKept)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, DEL, kIme, 0),
                          dev(K::kActionDown, DEL, kHw, 10),
                          dev(K::kActionDown, DEL, kHw, 400, 1),
                          dev(K::kActionDown, DEL, kHw, 450, 2),
                          dev(K::kActionDown, DEL, kHw, 500, 3),
                          dev(K::kActionUp, DEL, kIme, 540),
                          dev(K::kActionUp, DEL, kHw, 552),
                        });
  ASSERT_EQ(5u, out.size());
  EXPECT_EQ(0, out[0].repeat());
  EXPECT_EQ(1, out[1].repeat());
  EXPECT_EQ(3, out[3].repeat());
  EXPECT_EQ(Event::KeyUp, out[4].type());
  EXPECT_FALSE(input.isKeyPressed(kKeyBackspace));
}

// Early in the session only the IME copy arrives: nothing is dropped,
// also fast typing of the same letter.
TEST(AndroidKeys, OnlyImeCopy)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, A, kIme, 0, 0, 'a'),
                          dev(K::kActionUp, A, kIme, 30, 0, 'a'),
                          dev(K::kActionDown, A, kIme, 50, 0, 'a'),
                          dev(K::kActionUp, A, kIme, 80, 0, 'a'),
                          dev(K::kActionDown, B, kIme, 90, 0, 'b'),
                          dev(K::kActionUp, B, kIme, 120, 0, 'b'),
                        });
  ASSERT_EQ(6u, out.size());
  EXPECT_EQ('a', out[0].unicodeChar());
  EXPECT_EQ('a', out[2].unicodeChar());
  EXPECT_EQ('b', out[4].unicodeChar());
}

// Two quick presses of the same key from the hardware are kept.
TEST(AndroidKeys, QuickPressesSameDevice)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, A, kHw, 0, 0, 'a'),
                          dev(K::kActionUp, A, kHw, 20, 0, 'a'),
                          dev(K::kActionDown, A, kHw, 40, 0, 'a'),
                          dev(K::kActionUp, A, kHw, 55, 0, 'a'),
                        });
  EXPECT_EQ(4u, out.size());
}

// The same key from another device after the window is a new press.
TEST(AndroidKeys, OtherDeviceAfterWindow)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, A, kHw, 0, 0, 'a'),
                          dev(K::kActionUp, A, kHw, 50, 0, 'a'),
                          dev(K::kActionDown, A, kIme, 200, 0, 'a'),
                          dev(K::kActionUp, A, kIme, 250, 0, 'a'),
                        });
  EXPECT_EQ(4u, out.size());
}

// The UP of a dropped DOWN is dropped even late, once the key was
// released by the other device.
TEST(AndroidKeys, DroppedDownLateUp)
{
  KeyInputAndroid input;
  const auto out = feed(input,
                        {
                          dev(K::kActionDown, H, kIme, 0, 0, 'h'),
                          dev(K::kActionDown, H, kHw, 10, 0, 'h'),
                          dev(K::kActionUp, H, kIme, 100, 0, 'h'),
                          dev(K::kActionUp, H, kHw, 300, 0, 'h'),
                        });
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ(Event::KeyDown, out[0].type());
  EXPECT_EQ(Event::KeyUp, out[1].type());
}

int app_main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
