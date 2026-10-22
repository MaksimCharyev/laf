// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_KEYS_H_INCLUDED
#define OS_ANDROID_KEYS_H_INCLUDED
#pragma once

#include "os/event.h"
#include "os/keys.h"

#include <cstdint>
#include <vector>

namespace os {

// Fields of an Android key event (GameActivityKeyEvent / KeyEvent)
// used by KeyInputAndroid. Constants have the values of the NDK
// (<android/input.h>, <android/keycodes.h>), checked with
// static_assert in os/android/keys.cpp when it's compiled for Android.
// Nothing here includes NDK headers, so it's tested on the host.
struct KeyEventAndroid {
  static constexpr int kActionDown = 0;
  static constexpr int kActionUp = 1;

  // Meta state bits (AMETA_*).
  static constexpr int kMetaShiftOn = 0x01;
  static constexpr int kMetaAltOn = 0x02;
  static constexpr int kMetaAltLeftOn = 0x10;
  static constexpr int kMetaAltRightOn = 0x20;
  static constexpr int kMetaShiftLeftOn = 0x40;
  static constexpr int kMetaShiftRightOn = 0x80;
  static constexpr int kMetaCtrlOn = 0x1000;
  static constexpr int kMetaCtrlLeftOn = 0x2000;
  static constexpr int kMetaCtrlRightOn = 0x4000;
  static constexpr int kMetaMetaOn = 0x10000;
  static constexpr int kMetaMetaLeftOn = 0x20000;
  static constexpr int kMetaMetaRightOn = 0x40000;

  // KeyCharacterMap.COMBINING_ACCENT: unicodeChar of a dead key.
  static constexpr uint32_t kCombiningAccent = 0x80000000u;

  // Huawei M-Pencil double-tap (NOTES.md section 9). Not a keyboard
  // key: it's left to the pen gesture code (ADR 0006 section 6).
  static constexpr int kKeyCodePenDoubleTap = 718;

  int action = kActionDown;
  int keyCode = 0;
  int scanCode = 0;
  int metaState = 0;
  int repeatCount = 0;
  int unicodeChar = 0;
};

// Translates hardware keyboard events into KeyDown/KeyUp os::Events
// (ADR 0006 section 11, M2.3) and keeps the state of pressed keys for
// System::isKeyPressed()/keyModifiers().
//
//   AKEYCODE_* -> KeyScancode (android_keycode_to_scancode()),
//   KEYCODE_BACK (key or back gesture) -> kKeyEsc,
//   metaState -> KeyModifiers (+ kKeySpaceModifier while Space is down),
//   unicodeChar (KeyEvent.getUnicodeChar(metaState)) -> unicodeChar,
//   repeatCount -> repeat.
//
// Keys without scancode still produce events if they have a unicode
// character (e.g. symbols of other layouts), so they can be typed.
// Dead keys are sent without character (not composed).
class KeyInputAndroid {
public:
  // Returns false if the event must be ignored (no os::Event):
  // unknown keys without character, the pen double-tap key (718),
  // and actions other than DOWN/UP.
  bool processKey(const KeyEventAndroid& ev, Event& out);

  bool isKeyPressed(KeyScancode scancode) const;
  KeyModifiers modifiers() const;

  // Character produced by the scancode without modifiers (the last
  // one seen, or the US layout one), for shortcuts.
  int unicodeFromScancode(KeyScancode scancode) const;

  // Forgets all pressed keys (e.g. the app lost the focus, so no UP
  // will arrive).
  void reset();

private:
  void setPressed(int keyCode, bool pressed);
  void dropStaleModifiers(int metaState, int keyCode);

  std::vector<int> m_pressed; // Android key codes
  int m_metaState = 0;        // Of the last event
  int m_unicode[kKeyScancodes] = {};
};

// AKEYCODE_* -> KeyScancode, kKeyNil if there is no equivalent.
KeyScancode android_keycode_to_scancode(int keyCode);

// AMETA_* bits -> KeyModifiers (Meta = kKeyWinModifier).
KeyModifiers android_meta_to_modifiers(int metaState);

} // namespace os

#endif
