// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <gtest/gtest.h>

#include "base/string.h"
#include "os/android/text_input.h"

#include <string>
#include <vector>

using namespace os;

namespace {

// Compact description of the KeyDown events: characters as they are,
// "<" for Backspace, "|" for Enter. KeyUps are checked to follow each
// KeyDown.
std::string keys(const std::vector<Event>& events)
{
  std::u32string result;
  for (size_t i = 0; i < events.size(); i += 2) {
    const Event& down = events[i];
    EXPECT_EQ(Event::KeyDown, down.type());
    EXPECT_LT(i + 1, events.size());
    if (i + 1 < events.size()) {
      EXPECT_EQ(Event::KeyUp, events[i + 1].type());
      EXPECT_EQ(down.scancode(), events[i + 1].scancode());
    }
    if (down.scancode() == kKeyBackspace)
      result.push_back('<');
    else if (down.scancode() == kKeyEnter)
      result.push_back('|');
    else
      result.push_back(char32_t(down.unicodeChar()));
  }
  std::string utf8;
  for (char32_t c : result)
    utf8 += base::codepoint_to_utf8(c);
  return utf8;
}

std::string update(TextInputAndroid& input, const std::string& text)
{
  std::vector<Event> events;
  input.update(text, events);
  return keys(events);
}

} // anonymous namespace

TEST(AndroidTextInput, Typing)
{
  TextInputAndroid input;
  EXPECT_EQ("1", update(input, "1"));
  EXPECT_EQ("2", update(input, "12"));
  EXPECT_EQ("8", update(input, "128"));
  EXPECT_EQ("<", update(input, "12"));
  EXPECT_EQ("", update(input, "12"));
}

TEST(AndroidTextInput, ComposingAndAutocorrect)
{
  TextInputAndroid input;
  EXPECT_EQ("t", update(input, "t"));
  EXPECT_EQ("e", update(input, "te"));
  EXPECT_EQ("h", update(input, "teh"));
  // "teh" -> "the "
  EXPECT_EQ("<<he ", update(input, "the "));
}

TEST(AndroidTextInput, Cyrillic)
{
  TextInputAndroid input;
  EXPECT_EQ("п", update(input, "п"));
  EXPECT_EQ("ри", update(input, "при"));
  EXPECT_EQ("<<<", update(input, ""));
}

TEST(AndroidTextInput, NewLineIsEnter)
{
  TextInputAndroid input;
  EXPECT_EQ("a|", update(input, "a\n"));

  std::vector<Event> events;
  TextInputAndroid::editorAction(events);
  EXPECT_EQ("|", keys(events));
}

TEST(AndroidTextInput, Reset)
{
  TextInputAndroid input;
  EXPECT_EQ("ab", update(input, "ab"));
  input.reset();
  EXPECT_EQ("c", update(input, "c"));
}

int app_main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
