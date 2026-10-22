// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_SKIA_SKIA_WINDOW_ANDROID_INCLUDED
#define OS_SKIA_SKIA_WINDOW_ANDROID_INCLUDED
#pragma once

#include "base/disable_copying.h"
#include "os/android/window.h"
#include "os/skia/skia_window_base.h"

#include <string>

class SkPixmap;

namespace os {

class SkiaWindowAndroid : public SkiaWindowBase<WindowAndroid> {
public:
  SkiaWindowAndroid(const WindowSpec& spec);

  std::string getLayout() override { return ""; }
  void setLayout(const std::string& layout) override {}

private:
  void onPaint(const gfx::Rect& rc) override;
  void onNativeWindowAttached() override;
  void onNativeWindowDetached() override;

  void resetRasterState();
  void logColorProbe(const SkPixmap& dst);

  // State of the raster path for the current ANativeWindow.
  bool m_buffersReady = false; // ANativeWindow_setBuffersGeometry() done
  bool m_buffersFailed = false;
  bool m_lockFailed = false;
  bool m_firstPaint = true;

  DISABLE_COPYING(SkiaWindowAndroid);
};

} // namespace os

#endif
