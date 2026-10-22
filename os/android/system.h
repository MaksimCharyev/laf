// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_SYSTEM_H
#define OS_ANDROID_SYSTEM_H
#pragma once

#include "os/common/system.h"

namespace os {

// Base of SkiaSystem on Android (os/skia/skia_system.h). There is
// one full-screen window (see WindowAndroid) and one screen.
class SystemAndroid : public CommonSystem {
public:
  static Ref<SystemAndroid> instance() { return Ref<SystemAndroid>(System::instance()); }

  ~SystemAndroid();

  void setTabletOptions(const TabletOptions& options) override { m_tabletOptions = options; }
  TabletOptions tabletOptions() const override { return m_tabletOptions; }

  // Hardware keyboard state (os/android/keys.h).
  bool isKeyPressed(KeyScancode scancode) override;
  void resetKeyPressed() override;
  int getUnicodeFromScancode(KeyScancode scancode) override;

  // Soft keyboard (M2.4).
  void setTextInput(bool state, const gfx::Point& screenCaretPos) override;

  // Last position of the pen/finger/mouse (window pixels, the window
  // is at the screen origin).
  gfx::Point mousePosition() const override;

  // Android apps cannot move the pointer.
  void setMousePosition(const gfx::Point& screenPosition) override {}

  ScreenRef primaryScreen() override;
  void listScreens(ScreenList& list) override;

private:
  TabletOptions m_tabletOptions;
};

} // namespace os

#endif
