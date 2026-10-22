// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/screen.h"

#include "gfx/color_space.h"
#include "os/android/app.h"
#include "os/system.h"

namespace os {

ScreenAndroid::ScreenAndroid()
{
  if (AppAndroid* app = AppAndroid::instance()) {
    m_bounds = gfx::Rect(gfx::Point(0, 0), app->screenSize());
    m_density = app->density();
  }
}

os::ColorSpaceRef ScreenAndroid::colorSpace() const
{
  // TODO get the display color space (wide color gamut displays)
  return System::instance()->makeColorSpace(gfx::ColorSpace::MakeSRGB());
}

} // namespace os
