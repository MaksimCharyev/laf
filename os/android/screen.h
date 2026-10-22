// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_SCREEN_H
#define OS_ANDROID_SCREEN_H
#pragma once

#include "os/screen.h"

namespace os {

// The device screen. As the activity is full-screen, bounds and
// workarea are the same: the window size if there is a window, or the
// AConfiguration screen size (dp * density / 160) if not.
class ScreenAndroid : public Screen {
public:
  ScreenAndroid();

  bool isPrimary() const override { return true; }
  gfx::Rect bounds() const override { return m_bounds; }
  gfx::Rect workarea() const override { return m_bounds; }

  os::ColorSpaceRef colorSpace() const override;

  void* nativeHandle() const override { return nullptr; }

  // Density in dpi (AConfiguration_getDensity()).
  int density() const { return m_density; }

private:
  gfx::Rect m_bounds;
  int m_density = 0;
};

} // namespace os

#endif
