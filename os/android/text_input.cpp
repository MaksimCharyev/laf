// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/text_input.h"

#include "base/string.h"

#include <algorithm>

namespace os {

void TextInputAndroid::update(const std::string& utf8, std::vector<Event>& out)
{
  std::u32string text;
  for (const wchar_t chr : base::from_utf8(utf8))
    text.push_back(char32_t(chr));

  // Common prefix: everything after it in the old text is deleted and
  // everything after it in the new text is inserted.
  const size_t n = std::min(text.size(), m_text.size());
  size_t prefix = 0;
  while (prefix < n && text[prefix] == m_text[prefix])
    ++prefix;

  for (size_t i = prefix; i < m_text.size(); ++i)
    addKey(out, kKeyBackspace, 0);

  for (size_t i = prefix; i < text.size(); ++i) {
    const char32_t chr = text[i];
    if (chr == '\n')
      addKey(out, kKeyEnter, 0);
    else if (chr >= 32)
      addKey(out, kKeyNil, int(chr));
  }

  m_text = std::move(text);
}

// static
void TextInputAndroid::editorAction(std::vector<Event>& out)
{
  addKey(out, kKeyEnter, 0);
}

// static
void TextInputAndroid::addKey(std::vector<Event>& out,
                              const KeyScancode scancode,
                              const int unicodeChar)
{
  Event ev;
  ev.setScancode(scancode);
  ev.setUnicodeChar(unicodeChar);
  ev.setModifiers(kKeyNoneModifier);
  ev.setRepeat(0);

  ev.setType(Event::KeyDown);
  out.push_back(ev);
  ev.setType(Event::KeyUp);
  out.push_back(ev);
}

} // namespace os
