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

  // TODO M2.3 hardware keyboard state
  bool isKeyPressed(KeyScancode scancode) override { return false; }
  void resetKeyPressed() override {}
  int getUnicodeFromScancode(KeyScancode scancode) override { return 0; }

  // TODO M2.1 last pointer position
  gfx::Point mousePosition() const override { return m_mousePosition; }

  // Android apps cannot move the pointer.
  void setMousePosition(const gfx::Point& screenPosition) override {}

  ScreenRef primaryScreen() override;
  void listScreens(ScreenList& list) override;

private:
  TabletOptions m_tabletOptions;
  gfx::Point m_mousePosition;
};

} // namespace os

#endif
