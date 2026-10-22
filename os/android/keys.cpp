// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/keys.h"

#include <algorithm>

#ifdef __ANDROID__
  #include <android/input.h>
  #include <android/keycodes.h>
#endif

namespace os {

namespace {

// AKEYCODE_<name> = value -> KeyScancode. The values are spelled out
// so the table can be tested on the host; on Android they are checked
// against <android/keycodes.h> below.
#define LAF_ANDROID_KEYS(KEY)                                                                      \
  KEY(BACK, 4, kKeyEsc)                                                                            \
  KEY(0, 7, kKey0)                                                                                 \
  KEY(1, 8, kKey1)                                                                                 \
  KEY(2, 9, kKey2)                                                                                 \
  KEY(3, 10, kKey3)                                                                                \
  KEY(4, 11, kKey4)                                                                                \
  KEY(5, 12, kKey5)                                                                                \
  KEY(6, 13, kKey6)                                                                                \
  KEY(7, 14, kKey7)                                                                                \
  KEY(8, 15, kKey8)                                                                                \
  KEY(9, 16, kKey9)                                                                                \
  KEY(DPAD_UP, 19, kKeyUp)                                                                         \
  KEY(DPAD_DOWN, 20, kKeyDown)                                                                     \
  KEY(DPAD_LEFT, 21, kKeyLeft)                                                                     \
  KEY(DPAD_RIGHT, 22, kKeyRight)                                                                   \
  KEY(A, 29, kKeyA)                                                                                \
  KEY(B, 30, kKeyB)                                                                                \
  KEY(C, 31, kKeyC)                                                                                \
  KEY(D, 32, kKeyD)                                                                                \
  KEY(E, 33, kKeyE)                                                                                \
  KEY(F, 34, kKeyF)                                                                                \
  KEY(G, 35, kKeyG)                                                                                \
  KEY(H, 36, kKeyH)                                                                                \
  KEY(I, 37, kKeyI)                                                                                \
  KEY(J, 38, kKeyJ)                                                                                \
  KEY(K, 39, kKeyK)                                                                                \
  KEY(L, 40, kKeyL)                                                                                \
  KEY(M, 41, kKeyM)                                                                                \
  KEY(N, 42, kKeyN)                                                                                \
  KEY(O, 43, kKeyO)                                                                                \
  KEY(P, 44, kKeyP)                                                                                \
  KEY(Q, 45, kKeyQ)                                                                                \
  KEY(R, 46, kKeyR)                                                                                \
  KEY(S, 47, kKeyS)                                                                                \
  KEY(T, 48, kKeyT)                                                                                \
  KEY(U, 49, kKeyU)                                                                                \
  KEY(V, 50, kKeyV)                                                                                \
  KEY(W, 51, kKeyW)                                                                                \
  KEY(X, 52, kKeyX)                                                                                \
  KEY(Y, 53, kKeyY)                                                                                \
  KEY(Z, 54, kKeyZ)                                                                                \
  KEY(COMMA, 55, kKeyComma)                                                                        \
  KEY(PERIOD, 56, kKeyStop)                                                                        \
  KEY(ALT_LEFT, 57, kKeyAlt)                                                                       \
  KEY(ALT_RIGHT, 58, kKeyAlt)                                                                      \
  KEY(SHIFT_LEFT, 59, kKeyLShift)                                                                  \
  KEY(SHIFT_RIGHT, 60, kKeyRShift)                                                                 \
  KEY(TAB, 61, kKeyTab)                                                                            \
  KEY(SPACE, 62, kKeySpace)                                                                        \
  KEY(ENTER, 66, kKeyEnter)                                                                        \
  KEY(DEL, 67, kKeyBackspace)                                                                      \
  KEY(GRAVE, 68, kKeyTilde)                                                                        \
  KEY(MINUS, 69, kKeyMinus)                                                                        \
  KEY(EQUALS, 70, kKeyEquals)                                                                      \
  KEY(LEFT_BRACKET, 71, kKeyOpenbrace)                                                             \
  KEY(RIGHT_BRACKET, 72, kKeyClosebrace)                                                           \
  KEY(BACKSLASH, 73, kKeyBackslash)                                                                \
  KEY(SEMICOLON, 74, kKeyColon)                                                                    \
  KEY(APOSTROPHE, 75, kKeyQuote)                                                                   \
  KEY(SLASH, 76, kKeySlash)                                                                        \
  KEY(AT, 77, kKeyAt)                                                                              \
  KEY(MENU, 82, kKeyMenu)                                                                          \
  KEY(PAGE_UP, 92, kKeyPageUp)                                                                     \
  KEY(PAGE_DOWN, 93, kKeyPageDown)                                                                 \
  KEY(ESCAPE, 111, kKeyEsc)                                                                        \
  KEY(FORWARD_DEL, 112, kKeyDel)                                                                   \
  KEY(CTRL_LEFT, 113, kKeyLControl)                                                                \
  KEY(CTRL_RIGHT, 114, kKeyRControl)                                                               \
  KEY(CAPS_LOCK, 115, kKeyCapsLock)                                                                \
  KEY(SCROLL_LOCK, 116, kKeyScrLock)                                                               \
  KEY(META_LEFT, 117, kKeyLWin)                                                                    \
  KEY(META_RIGHT, 118, kKeyRWin)                                                                   \
  KEY(SYSRQ, 120, kKeyPrtscr)                                                                      \
  KEY(BREAK, 121, kKeyPause)                                                                       \
  KEY(MOVE_HOME, 122, kKeyHome)                                                                    \
  KEY(MOVE_END, 123, kKeyEnd)                                                                      \
  KEY(INSERT, 124, kKeyInsert)                                                                     \
  KEY(F1, 131, kKeyF1)                                                                             \
  KEY(F2, 132, kKeyF2)                                                                             \
  KEY(F3, 133, kKeyF3)                                                                             \
  KEY(F4, 134, kKeyF4)                                                                             \
  KEY(F5, 135, kKeyF5)                                                                             \
  KEY(F6, 136, kKeyF6)                                                                             \
  KEY(F7, 137, kKeyF7)                                                                             \
  KEY(F8, 138, kKeyF8)                                                                             \
  KEY(F9, 139, kKeyF9)                                                                             \
  KEY(F10, 140, kKeyF10)                                                                           \
  KEY(F11, 141, kKeyF11)                                                                           \
  KEY(F12, 142, kKeyF12)                                                                           \
  KEY(NUM_LOCK, 143, kKeyNumLock)                                                                  \
  KEY(NUMPAD_0, 144, kKey0Pad)                                                                     \
  KEY(NUMPAD_1, 145, kKey1Pad)                                                                     \
  KEY(NUMPAD_2, 146, kKey2Pad)                                                                     \
  KEY(NUMPAD_3, 147, kKey3Pad)                                                                     \
  KEY(NUMPAD_4, 148, kKey4Pad)                                                                     \
  KEY(NUMPAD_5, 149, kKey5Pad)                                                                     \
  KEY(NUMPAD_6, 150, kKey6Pad)                                                                     \
  KEY(NUMPAD_7, 151, kKey7Pad)                                                                     \
  KEY(NUMPAD_8, 152, kKey8Pad)                                                                     \
  KEY(NUMPAD_9, 153, kKey9Pad)                                                                     \
  KEY(NUMPAD_DIVIDE, 154, kKeySlashPad)                                                            \
  KEY(NUMPAD_MULTIPLY, 155, kKeyAsterisk)                                                          \
  KEY(NUMPAD_SUBTRACT, 156, kKeyMinusPad)                                                          \
  KEY(NUMPAD_ADD, 157, kKeyPlusPad)                                                                \
  KEY(NUMPAD_DOT, 158, kKeyDelPad)                                                                 \
  KEY(NUMPAD_ENTER, 160, kKeyEnterPad)                                                             \
  KEY(NUMPAD_EQUALS, 161, kKeyEqualsPad)                                                           \
  KEY(ZENKAKU_HANKAKU, 211, kKeyKanji)                                                             \
  KEY(MUHENKAN, 213, kKeyNoconvert)                                                                \
  KEY(HENKAN, 214, kKeyConvert)                                                                    \
  KEY(YEN, 216, kKeyYen)                                                                           \
  KEY(RO, 217, kKeyAbntC1)                                                                         \
  KEY(KANA, 218, kKeyKana)

#ifdef __ANDROID__
  #define LAF_CHECK_KEY(name, value, scancode)                                                     \
    static_assert(AKEYCODE_##name == value, "AKEYCODE_" #name);
LAF_ANDROID_KEYS(LAF_CHECK_KEY)
  #undef LAF_CHECK_KEY

using K = KeyEventAndroid;
static_assert(K::kActionDown == AKEY_EVENT_ACTION_DOWN && K::kActionUp == AKEY_EVENT_ACTION_UP,
              "Key actions");
static_assert(K::kMetaShiftOn == AMETA_SHIFT_ON && K::kMetaAltOn == AMETA_ALT_ON &&
                K::kMetaAltLeftOn == AMETA_ALT_LEFT_ON &&
                K::kMetaAltRightOn == AMETA_ALT_RIGHT_ON &&
                K::kMetaShiftLeftOn == AMETA_SHIFT_LEFT_ON &&
                K::kMetaShiftRightOn == AMETA_SHIFT_RIGHT_ON && K::kMetaCtrlOn == AMETA_CTRL_ON &&
                K::kMetaCtrlLeftOn == AMETA_CTRL_LEFT_ON &&
                K::kMetaCtrlRightOn == AMETA_CTRL_RIGHT_ON && K::kMetaMetaOn == AMETA_META_ON &&
                K::kMetaMetaLeftOn == AMETA_META_LEFT_ON &&
                K::kMetaMetaRightOn == AMETA_META_RIGHT_ON,
              "Meta state");
#endif

// Modifier keys, with the metaState bit that is on while each one is
// held.
struct ModifierKey {
  int keyCode;
  int metaBit;
};
constexpr ModifierKey kModifierKeys[] = {
  { 57,  KeyEventAndroid::kMetaAltLeftOn    }, // ALT_LEFT
  { 58,  KeyEventAndroid::kMetaAltRightOn   }, // ALT_RIGHT
  { 59,  KeyEventAndroid::kMetaShiftLeftOn  }, // SHIFT_LEFT
  { 60,  KeyEventAndroid::kMetaShiftRightOn }, // SHIFT_RIGHT
  { 113, KeyEventAndroid::kMetaCtrlLeftOn   }, // CTRL_LEFT
  { 114, KeyEventAndroid::kMetaCtrlRightOn  }, // CTRL_RIGHT
  { 117, KeyEventAndroid::kMetaMetaLeftOn   }, // META_LEFT
  { 118, KeyEventAndroid::kMetaMetaRightOn  }, // META_RIGHT
};

// Character of the US layout without modifiers, used until the key
// was typed once (see unicodeFromScancode()).
int us_unicode_from_scancode(const KeyScancode scancode)
{
  if (scancode >= kKeyA && scancode <= kKeyZ)
    return 'a' + (scancode - kKeyA);
  if (scancode >= kKey0 && scancode <= kKey9)
    return '0' + (scancode - kKey0);
  if (scancode >= kKey0Pad && scancode <= kKey9Pad)
    return '0' + (scancode - kKey0Pad);
  switch (scancode) {
    case kKeyTilde:      return '`';
    case kKeyMinus:      return '-';
    case kKeyEquals:     return '=';
    case kKeyOpenbrace:  return '[';
    case kKeyClosebrace: return ']';
    case kKeyColon:      return ';';
    case kKeyQuote:      return '\'';
    case kKeyBackslash:  return '\\';
    case kKeyComma:      return ',';
    case kKeyStop:       return '.';
    case kKeySlash:      return '/';
    case kKeySpace:      return ' ';
    case kKeyAt:         return '@';
    case kKeySlashPad:   return '/';
    case kKeyAsterisk:   return '*';
    case kKeyMinusPad:   return '-';
    case kKeyPlusPad:    return '+';
    case kKeyEqualsPad:  return '=';
    default:             return 0;
  }
}

} // anonymous namespace

KeyScancode android_keycode_to_scancode(const int keyCode)
{
  switch (keyCode) {
#define LAF_KEY_CASE(name, value, scancode)                                                        \
  case value: return scancode;
    LAF_ANDROID_KEYS(LAF_KEY_CASE)
#undef LAF_KEY_CASE
  }
  return kKeyNil;
}

KeyModifiers android_meta_to_modifiers(const int metaState)
{
  int modifiers = kKeyNoneModifier;
  if (metaState & KeyEventAndroid::kMetaShiftOn)
    modifiers |= kKeyShiftModifier;
  if (metaState & KeyEventAndroid::kMetaCtrlOn)
    modifiers |= kKeyCtrlModifier;
  if (metaState & KeyEventAndroid::kMetaAltOn)
    modifiers |= kKeyAltModifier;
  if (metaState & KeyEventAndroid::kMetaMetaOn)
    modifiers |= kKeyWinModifier;
  return KeyModifiers(modifiers);
}

bool KeyInputAndroid::processKey(const KeyEventAndroid& ev, Event& out)
{
  if (ev.action != KeyEventAndroid::kActionDown && ev.action != KeyEventAndroid::kActionUp)
    return false;
  // Handled as kKeyPenDoubleTap by the gesture code
  // (PointerInputAndroid::isGestureKey()), never as a keyboard key.
  // No Android key is mapped to kKeyUnknown1..4 (gesture scancodes).
  if (ev.keyCode == KeyEventAndroid::kKeyCodePenDoubleTap)
    return false;

  const bool down = (ev.action == KeyEventAndroid::kActionDown);
  const KeyScancode scancode = android_keycode_to_scancode(ev.keyCode);

  // Dead keys (KeyCharacterMap.COMBINING_ACCENT) are not composed,
  // they are sent without character.
  int unicode = 0;
  if (!(uint32_t(ev.unicodeChar) & KeyEventAndroid::kCombiningAccent) && ev.unicodeChar > 0)
    unicode = ev.unicodeChar;

  // Back has the character of no key (it's not Esc for the system).
  if (scancode == kKeyEsc)
    unicode = 0;

  if (scancode == kKeyNil && unicode == 0)
    return false;

  // A modifier released while the app had no focus never sends its UP.
  dropStaleModifiers(ev.metaState, ev.keyCode);
  m_metaState = ev.metaState;

  if (scancode != kKeyNil) {
    setPressed(ev.keyCode, down);

    // Remember the character without modifiers for shortcuts.
    if (down && unicode >= 32 &&
        !(ev.metaState & (KeyEventAndroid::kMetaShiftOn | KeyEventAndroid::kMetaAltOn |
                          KeyEventAndroid::kMetaCtrlOn | KeyEventAndroid::kMetaMetaOn))) {
      m_unicode[scancode] = unicode;
    }
  }

  out.setType(down ? Event::KeyDown : Event::KeyUp);
  out.setScancode(scancode);
  out.setUnicodeChar(unicode);
  out.setDeadKey(false);
  out.setRepeat(down ? std::max(0, ev.repeatCount) : 0);
  out.setModifiers(modifiers());
  return true;
}

bool KeyInputAndroid::isKeyPressed(const KeyScancode scancode) const
{
  if (scancode == kKeyNil)
    return false;
  for (int keyCode : m_pressed) {
    if (android_keycode_to_scancode(keyCode) == scancode)
      return true;
  }
  return false;
}

KeyModifiers KeyInputAndroid::modifiers() const
{
  int modifiers = android_meta_to_modifiers(m_metaState);
  if (isKeyPressed(kKeySpace))
    modifiers |= kKeySpaceModifier;
  return KeyModifiers(modifiers);
}

int KeyInputAndroid::unicodeFromScancode(const KeyScancode scancode) const
{
  if (scancode <= kKeyNil || scancode >= kKeyScancodes)
    return 0;
  if (m_unicode[scancode])
    return m_unicode[scancode];
  return us_unicode_from_scancode(scancode);
}

void KeyInputAndroid::reset()
{
  m_pressed.clear();
  m_metaState = 0;
}

void KeyInputAndroid::setPressed(const int keyCode, const bool pressed)
{
  auto it = std::find(m_pressed.begin(), m_pressed.end(), keyCode);
  if (pressed) {
    if (it == m_pressed.end())
      m_pressed.push_back(keyCode);
  }
  else if (it != m_pressed.end()) {
    m_pressed.erase(it);
  }
}

void KeyInputAndroid::dropStaleModifiers(const int metaState, const int keyCode)
{
  for (const ModifierKey& m : kModifierKeys) {
    if (m.keyCode != keyCode && !(metaState & m.metaBit))
      setPressed(m.keyCode, false);
  }
}

} // namespace os
