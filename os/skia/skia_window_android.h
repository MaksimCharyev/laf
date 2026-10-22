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

namespace os {

class SkiaWindowAndroid : public SkiaWindowBase<WindowAndroid> {
public:
  SkiaWindowAndroid(const WindowSpec& spec);

  std::string getLayout() override { return ""; }
  void setLayout(const std::string& layout) override {}

private:
  void onPaint(const gfx::Rect& rc) override;

  DISABLE_COPYING(SkiaWindowAndroid);
};

} // namespace os

#endif
