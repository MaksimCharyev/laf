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

#include <chrono>
#include <string>

class SkPixmap;

namespace os {

class GLContextEGL;

// Rendering backends of the Android window:
//
// - GPU (default): Skia Ganesh over OpenGL ES (GLContextEGL). The
//   EGLContext and the GrDirectContext live while GPU acceleration is
//   on; only the EGLSurface follows the ANativeWindow
//   (APP_CMD_INIT_WINDOW/TERM_WINDOW), and a resize only creates a
//   new render target (keepGpuContextOnResize()).
// - Raster (GPU acceleration off, or EGL/Skia GL failed): the laf
//   surface is copied to ANativeWindow_lock() buffers.
//
// Once ANativeWindow_lock() is used, the ANativeWindow stays
// connected to the CPU and eglCreateWindowSurface() fails for it, so
// switching from raster to GPU is applied with the next ANativeWindow
// (e.g. Home and back). Switching from GPU to raster is immediate
// (destroying the EGLSurface disconnects EGL from the window).
//
// The "debug.laf.gpu" system property overrides the app:
//
//   adb shell setprop debug.laf.gpu 1   (GPU, 0 = raster, "" = app decides)
//
class SkiaWindowAndroid : public SkiaWindowBase<WindowAndroid> {
public:
  SkiaWindowAndroid(const WindowSpec& spec);

  std::string getLayout() override { return ""; }
  void setLayout(const std::string& layout) override {}

  void setGpuAcceleration(bool state) override;
  void swapBuffers() override;

private:
  void onPaint(const gfx::Rect& rc) override;
  void onNativeWindowAttached() override;
  void onNativeWindowDetached() override;
  void onResize(const gfx::Size& sz) override;
#if SK_SUPPORT_GPU
  bool keepGpuContextOnResize() const override { return true; }
#endif

  void resetRasterState();
  void logColorProbe(const SkPixmap& dst);

  // GPU path
  void useWindowForRaster();
  void recreateLostGpuContext();
  void checkGpuContext();
  void logGpuState(const char* reason);
  void resetFrameStats();

  GLContextEGL* m_eglCtx = nullptr; // Owned by m_glCtx
  bool m_gpuRequested = false;      // Last setGpuAcceleration() value
  // The current ANativeWindow was locked by the raster path, EGL
  // cannot use it.
  bool m_rasterOwnsWindow = false;
  const void* m_lastGrCtx = nullptr;
  int m_grCtxCount = 0;
  int m_lastSurfaceCount = 0;
  bool m_firstGpuFrame = true;

  // Frame statistics of the GPU path (verbose mode)
  std::chrono::steady_clock::time_point m_statsStart;
  int m_statsFrames = 0;
  double m_statsSwapTotal = 0.0;
  double m_statsSwapMax = 0.0;

  // State of the raster path for the current ANativeWindow.
  bool m_buffersReady = false; // ANativeWindow_setBuffersGeometry() done
  bool m_buffersFailed = false;
  bool m_lockFailed = false;
  bool m_firstPaint = true;

  DISABLE_COPYING(SkiaWindowAndroid);
};

} // namespace os

#endif
