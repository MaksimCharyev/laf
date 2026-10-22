// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <gtest/gtest.h>

#include "os/android/keys.h"

using namespace os;
using K = KeyEventAndroid;

namespace {

// Android key codes used in the tests (android.view.KeyEvent).
enum {
  BACK = 4,
  A = 29,
  B = 30,
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

int app_main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
