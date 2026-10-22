// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/window.h"

#include "gfx/color_space.h"
#include "gfx/region.h"
#include "os/android/app.h"
#include "os/system.h"
#include "os/window_spec.h"

#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cstdlib>

namespace os {

namespace {

// Refresh rate requested for the window surface. HarmonyOS drops the
// panel to 60 Hz when the app renders on demand; this vote tells
// SurfaceFlinger that the content wants a high rate while it's
// updating (the system ignores it when the app is in background or
// idle). "debug.laf.frame_rate" overrides it (0 = don't ask):
//
//   adb shell setprop debug.laf.frame_rate 120   (then restart the app)
//
constexpr float kDefaultFrameRate = 144.0f;

float frame_rate_property()
{
  static const float value = []() -> float {
    char value[PROP_VALUE_MAX] = { 0 };
    if (__system_property_get("debug.laf.frame_rate", value) <= 0)
      return kDefaultFrameRate;
    const float fps = std::strtof(value, nullptr);
    return (fps >= 0.0f && fps <= 1000.0f ? fps : kDefaultFrameRate);
  }();
  return value;
}

// ANativeWindow_setFrameRate() is API 30+ and minSdk is 29, so it's
// loaded at runtime.
using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);

SetFrameRateFn set_frame_rate_fn()
{
  static const SetFrameRateFn fn = []() -> SetFrameRateFn {
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return nullptr;
    return reinterpret_cast<SetFrameRateFn>(dlsym(lib, "ANativeWindow_setFrameRate"));
  }();
  return fn;
}

// Called for each new ANativeWindow (the vote belongs to the surface).
void request_frame_rate(ANativeWindow* window)
{
  const float fps = frame_rate_property();
  if (fps <= 0.0f) {
    LAF_ALOGI("Frame rate: not requested (debug.laf.frame_rate=0)");
    return;
  }

  SetFrameRateFn fn = set_frame_rate_fn();
  if (!fn) {
    LAF_ALOGW("Frame rate: ANativeWindow_setFrameRate() not available (API < 30)");
    return;
  }

  const int32_t res = fn(window, fps, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
  if (res == 0)
    LAF_ALOGI("Frame rate: ANativeWindow_setFrameRate(%.0f) requested", fps);
  else
    LAF_ALOGW("Frame rate: ANativeWindow_setFrameRate(%.0f) failed (%d)", fps, int(res));
}

} // anonymous namespace

WindowAndroid::WindowAndroid(const WindowSpec& spec)
  : m_scale(AppAndroid::debugScale() ? AppAndroid::debugScale() : std::max(1, spec.scale()))
{
  AppAndroid* app = AppAndroid::instance();
  if (!app)
    throw WindowCreationException("Cannot create a window outside android_main()");

  m_size = app->screenSize();
  if (m_size.w <= 0 || m_size.h <= 0)
    m_size = spec.contentRect().size();

  if (!app->window()) {
    app->setWindow(this);
    // If APP_CMD_INIT_WINDOW was already processed we can use the
    // ANativeWindow right now, if not, attachNativeWindow() will be
    // called later.
    if (app->app()->window) {
      setNativeWindow(app->app()->window);
      readNativeWindowSize();
    }
  }
  else {
    LAF_ALOGW("Only one window is supported on Android, the new window will not be visible");
  }

  LAF_ALOGI("Window created: %dx%d scale=%d (%s)",
            m_size.w,
            m_size.h,
            m_scale,
            m_nativeWindow ? "ANativeWindow attached" : "waiting for APP_CMD_INIT_WINDOW");
}

WindowAndroid::~WindowAndroid()
{
  AppAndroid* app = AppAndroid::instance();
  if (app && app->window() == this)
    app->setWindow(nullptr);

  setNativeWindow(nullptr);
}

os::ScreenRef WindowAndroid::screen() const
{
  if (auto system = System::instance())
    return system->primaryScreen();
  return nullptr;
}

os::ColorSpaceRef WindowAndroid::colorSpace() const
{
  if (auto cs = Window::colorSpace())
    return cs;

  // TODO get the display color space (wide color gamut displays)
  if (auto system = System::instance())
    return system->makeColorSpace(gfx::ColorSpace::MakeSRGB());

  return nullptr;
}

void WindowAndroid::setScale(const int scale)
{
  m_scale = std::max(1, scale);

  // Testing aid: debug.laf.scale replaces the scale requested by the
  // app (see AppAndroid::debugScale()).
  if (const int debugScale = AppAndroid::debugScale()) {
    if (debugScale != m_scale)
      LAF_ALOGI("Window scale %d requested, using debug.laf.scale=%d", m_scale, debugScale);
    m_scale = debugScale;
  }

  onResize(m_size);
}

void WindowAndroid::invalidateRegion(const gfx::Region& rgn)
{
  if (!m_nativeWindow)
    return;

  const gfx::Rect bounds = rgn.bounds();
  onPaint(
    gfx::Rect(bounds.x * m_scale, bounds.y * m_scale, bounds.w * m_scale, bounds.h * m_scale));
}

void WindowAndroid::attachNativeWindow(ANativeWindow* nativeWindow)
{
  if (!isAttached())
    return;

  setNativeWindow(nativeWindow);
  readNativeWindowSize();
  LAF_ALOGI("Window attached: %dx%d", m_size.w, m_size.h);

  onNativeWindowAttached();

  // The content of a new ANativeWindow is undefined, so we always ask
  // for a full repaint (even if the size didn't change).
  onResize(m_size);
}

void WindowAndroid::detachNativeWindow()
{
  if (!m_nativeWindow)
    return;

  onNativeWindowDetached();
  setNativeWindow(nullptr);
  LAF_ALOGI("Window detached");
}

void WindowAndroid::updateNativeWindowSize()
{
  if (m_nativeWindow && readNativeWindowSize()) {
    LAF_ALOGI("Window resized: %dx%d", m_size.w, m_size.h);
    onResize(m_size);
  }
}

void WindowAndroid::requestRedraw()
{
  if (m_nativeWindow)
    onResize(m_size);
}

bool WindowAndroid::isAttached() const
{
  AppAndroid* app = AppAndroid::instance();
  return (app && app->window() == this);
}

void WindowAndroid::setNativeWindow(ANativeWindow* nativeWindow)
{
  if (m_nativeWindow == nativeWindow)
    return;

  // We hold a reference so the object stays valid while we use it,
  // but after APP_CMD_TERM_WINDOW its surface is gone anyway.
  if (m_nativeWindow)
    ANativeWindow_release(m_nativeWindow);

  m_nativeWindow = nativeWindow;

  if (m_nativeWindow) {
    ANativeWindow_acquire(m_nativeWindow);
    request_frame_rate(m_nativeWindow);
  }
}

bool WindowAndroid::readNativeWindowSize()
{
  if (!m_nativeWindow)
    return false;

  const int w = ANativeWindow_getWidth(m_nativeWindow);
  const int h = ANativeWindow_getHeight(m_nativeWindow);
  if (w <= 0 || h <= 0 || (w == m_size.w && h == m_size.h))
    return false;

  m_size = gfx::Size(w, h);
  return true;
}

} // namespace os
