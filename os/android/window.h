// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_WINDOW_INCLUDED
#define OS_ANDROID_WINDOW_INCLUDED
#pragma once

#include "gfx/size.h"
#include "os/native_cursor.h"
#include "os/screen.h"
#include "os/window.h"

#include <string>

struct ANativeWindow;

namespace os {

class WindowSpec;

// The only window of the app, full-screen, bound to the
// ANativeWindow of the GameActivity (app->window). The os::Window
// object lives as long as the app wants (e.g. the whole app_main()),
// while the ANativeWindow comes and goes with
// APP_CMD_INIT_WINDOW/TERM_WINDOW (Home, Recents, power button). Only
// one WindowAndroid is attached at a time; extra windows (the app
// should not create them, there is no Capabilities::MultipleWindows)
// are never visible.
//
// Plan for the GPU path (M1.6, see NOTES.md): EGLDisplay, EGLContext
// and the GrDirectContext are created once and survive TERM_WINDOW.
// onNativeWindowDetached() only flushes Skia, releases the SkSurface
// wrapping the framebuffer, makes the context current without a
// surface (EGL_KHR_surfaceless_context) and destroys the EGLSurface;
// onNativeWindowAttached() creates a new EGLSurface for the new
// ANativeWindow and wraps it again. Full recreation only on
// EGL_CONTEXT_LOST.
class WindowAndroid : public Window {
public:
  WindowAndroid(const WindowSpec& spec);
  ~WindowAndroid();

  os::ScreenRef screen() const override;
  os::ColorSpaceRef colorSpace() const override;

  int scale() const override { return m_scale; }
  void setScale(int scale) override;

  bool isVisible() const override { return m_visible; }
  void setVisible(bool visible) override { m_visible = visible; }

  void activate() override {}
  void maximize() override {}
  void minimize() override {}
  bool isMaximized() const override { return true; }
  // There is nothing on the screen while the app is in background.
  bool isMinimized() const override { return !m_nativeWindow; }
  bool isTransparent() const override { return false; }

  bool isFullscreen() const override { return true; }
  void setFullscreen(bool state) override {}

  std::string title() const override { return m_title; }
  void setTitle(const std::string& title) override { m_title = title; }

  gfx::Rect frame() const override { return gfx::Rect(gfx::Point(0, 0), m_size); }
  void setFrame(const gfx::Rect& bounds) override {}
  gfx::Rect contentRect() const override { return frame(); }
  gfx::Rect restoredFrame() const override { return frame(); }

  gfx::Size clientSize() const { return m_size; }

  void captureMouse() override {}
  void releaseMouse() override {}
  void setMousePosition(const gfx::Point& position) override {}
  void invalidateRegion(const gfx::Region& rgn) override;

  // There is no mouse cursor on the touch screen (a hardware mouse
  // shows the system pointer).
  bool setCursor(NativeCursor cursor) override { return true; }
  bool setCursor(const CursorRef& cursor) override { return false; }

  void performWindowAction(WindowAction action, const Event* event) override {}

  NativeHandle nativeHandle() const override { return (NativeHandle)m_nativeWindow; }

  // TODO M2.4 show/hide the soft keyboard (GameTextInput)
  void setTextInput(bool state, const gfx::Point& screenCaretPos = {}) {}

  ANativeWindow* nativeWindow() const { return m_nativeWindow; }

  // Called by AppAndroid from the glue thread when the commands
  // arrive (APP_CMD_INIT_WINDOW, APP_CMD_TERM_WINDOW,
  // APP_CMD_WINDOW_RESIZED/CONTENT_RECT_CHANGED/CONFIG_CHANGED,
  // APP_CMD_WINDOW_REDRAW_NEEDED).
  void attachNativeWindow(ANativeWindow* nativeWindow);
  void detachNativeWindow();
  void updateNativeWindowSize();
  void requestRedraw();

protected:
  virtual void onPaint(const gfx::Rect& rc) = 0;
  virtual void onResize(const gfx::Size& sz) = 0;

  // Hooks for the rendering backend, called after the new
  // ANativeWindow is set (and its size updated), and before the
  // current ANativeWindow is released. They are not called from the
  // constructor/destructor (a derived class must check nativeWindow()
  // in its own constructor).
  virtual void onNativeWindowAttached() {}
  virtual void onNativeWindowDetached() {}

private:
  bool isAttached() const;
  void setNativeWindow(ANativeWindow* nativeWindow);
  bool readNativeWindowSize();

  ANativeWindow* m_nativeWindow = nullptr;
  gfx::Size m_size;
  int m_scale;
  bool m_visible = false;
  std::string m_title;
};

} // namespace os

#endif
