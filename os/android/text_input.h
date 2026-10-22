// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_TEXT_INPUT_H_INCLUDED
#define OS_ANDROID_TEXT_INPUT_H_INCLUDED
#pragma once

#include "os/event.h"

#include <string>
#include <vector>

namespace os {

// Turns the text of the soft keyboard (GameTextInput state, M2.4)
// into key events for the app: the IME edits its own copy of the text
// (composing regions, autocorrection), and each change is sent as
// Backspaces for the removed tail plus one KeyDown/KeyUp with
// unicodeChar per inserted character ('\n' is sent as Enter).
//
// The app's text field must have the caret at the end of its text for
// this to match (the usual case when typing in a new field: the app
// selects the whole text on focus, the first character replaces it).
// Pure C++, tested on the host.
class TextInputAndroid {
public:
  // Forgets the text (call it when the IME text is cleared).
  void reset() { m_text.clear(); }

  // New text of the IME (UTF-8). Events are appended to "out".
  void update(const std::string& utf8, std::vector<Event>& out);

  // IME action (e.g. "Done"): Enter.
  static void editorAction(std::vector<Event>& out);

  const std::u32string& text() const { return m_text; }

private:
  static void addKey(std::vector<Event>& out, KeyScancode scancode, int unicodeChar);

  std::u32string m_text;
};

} // namespace os

#endif
