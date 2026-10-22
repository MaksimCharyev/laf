// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/skia/skia_window_android.h"

#include "os/skia/skia_window.h"

namespace os {

SkiaWindowAndroid::SkiaWindowAndroid(const WindowSpec& spec) : Base(spec)
{
  // TODO M1.6 m_glCtx = GLContextEGL (see the plan in
  //      os/android/window.h). Without m_glCtx the window uses the
  //      raster backend.
  initColorSpace();
}

void SkiaWindowAndroid::onPaint(const gfx::Rect& rc)
{
#if SK_SUPPORT_GPU
  if (backend() == Backend::GL)
    return;
#endif

  // TODO M1.5 copy the raster surface to ANativeWindow_lock() buffers
  //      (WINDOW_FORMAT_RGBA_8888), scaling it as SkiaWindowX11 does.
}

} // namespace os
