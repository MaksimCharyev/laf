// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_GL_CONTEXT_EGL_INCLUDED
#define OS_GL_CONTEXT_EGL_INCLUDED
#pragma once

#include "os/gl/gl_context.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>

#if LAF_ANDROID
  #include <android/log.h>
  #define LAF_EGL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "laf", __VA_ARGS__)
  #define LAF_EGL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "laf", __VA_ARGS__)
#else
  #include "base/log.h"
  #define LAF_EGL_LOGI(...) LOG(INFO, __VA_ARGS__)
  #define LAF_EGL_LOGE(...) LOG(ERROR, __VA_ARGS__)
#endif

namespace os {

// OpenGL ES context over EGL (Android). Unlike the other GLContext
// implementations, the context and the window surface have separate
// lifetimes:
//
// - The EGLDisplay and the EGLContext are created by the first
//   createGLContext() call that has a native window, and live until
//   destroyGLContext() (GPU acceleration turned off, context lost,
//   window destroyed).
// - The EGLSurface follows the native window given in
//   setNativeWindow(): it's created by createGLContext() and
//   destroyed when the native window changes or is removed
//   (APP_CMD_TERM_WINDOW). Meanwhile the context stays current
//   without a surface (EGL_KHR_surfaceless_context, or a 1x1 pbuffer
//   if the extension is missing), so the GPU resources of Skia
//   survive.
//
// createGLContext() can be called again at any time: it only creates
// what is missing and returns true if there is a surface to draw
// into. That's what SkiaWindowBase expects when
// keepGpuContextOnResize() is true.
class GLContextEGL : public GLContext {
public:
  GLContextEGL() {}

  ~GLContextEGL()
  {
    destroyGLContext();
    if (m_display != EGL_NO_DISPLAY) {
      eglTerminate(m_display);
      m_display = EGL_NO_DISPLAY;
    }
  }

  // True if the EGLContext exists (with or without surface).
  bool isValid() override { return m_context != EGL_NO_CONTEXT; }

  bool createGLContext() override
  {
    if (!m_window)
      return false;

    if (m_context == EGL_NO_CONTEXT && !createContext())
      return false;

    if (m_surface == EGL_NO_SURFACE && !createSurface())
      return false;

    return true;
  }

  void destroyGLContext() override
  {
    destroySurface();

    if (m_display != EGL_NO_DISPLAY) {
      eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (m_pbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(m_display, m_pbuffer);
        m_pbuffer = EGL_NO_SURFACE;
      }
      if (m_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_CONTEXT;
        LAF_EGL_LOGI("GL: EGLContext destroyed");
      }
    }
    m_contextLost = false;
    m_contextFailed = false;
  }

  void makeCurrent() override
  {
    if (m_context == EGL_NO_CONTEXT)
      return;

    if (m_surface != EGL_NO_SURFACE) {
      if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
        checkError("eglMakeCurrent");
    }
    else {
      makeCurrentWithoutSurface();
    }
  }

  void swapBuffers() override
  {
    if (m_surface == EGL_NO_SURFACE)
      return;

    if (eglSwapBuffers(m_display, m_surface)) {
      m_swapFailed = false;
    }
    else if (!m_swapFailed) {
      // Log only the first error of a sequence (e.g. the surface is
      // being destroyed).
      checkError("eglSwapBuffers");
      m_swapFailed = true;
    }
  }

  // Sets the native window for the EGLSurface. If it's a different
  // window (or nullptr), the current EGLSurface is destroyed and
  // the context stays current without surface. The window must stay
  // valid until it's replaced with setNativeWindow(nullptr).
  void setNativeWindow(EGLNativeWindowType window)
  {
    if (m_window == window)
      return;

    destroySurface();
    m_window = window;
    // Try again with the new window (a failure is retried only once
    // per window).
    m_surfaceFailed = false;
    if (m_context == EGL_NO_CONTEXT)
      m_contextFailed = false;
  }

  EGLNativeWindowType nativeWindow() const { return m_window; }
  bool hasSurface() const { return m_surface != EGL_NO_SURFACE; }

  // EGL_CONTEXT_LOST was returned by an EGL call (e.g. GPU reset).
  // The owner must abandon its Skia GrDirectContext and call
  // destroyGLContext() before creating everything again.
  bool isContextLost() const { return m_contextLost; }

  // Number of EGLSurfaces created by this object (for logs).
  int surfaceCount() const { return m_surfaceCount; }

  // Size of the current EGLSurface (0x0 if there is no surface).
  void surfaceSize(int& w, int& h) const
  {
    w = h = 0;
    if (m_surface != EGL_NO_SURFACE) {
      eglQuerySurface(m_display, m_surface, EGL_WIDTH, &w);
      eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &h);
    }
  }

private:
  bool initDisplay()
  {
    if (m_display != EGL_NO_DISPLAY)
      return true;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
      LAF_EGL_LOGE("GL: eglGetDisplay() failed");
      return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
      checkError("eglInitialize");
      return false;
    }

    m_display = display;

    const char* exts = eglQueryString(m_display, EGL_EXTENSIONS);
    m_surfaceless = has_extension(exts, "EGL_KHR_surfaceless_context");

    LAF_EGL_LOGI("GL: EGL %d.%d (%s), surfaceless_context=%d",
                 int(major),
                 int(minor),
                 eglQueryString(m_display, EGL_VENDOR),
                 int(m_surfaceless));
    return true;
  }

  bool chooseConfig(const EGLint renderableType)
  {
    // RGBA8888 window surfaces, without depth/stencil buffers: laf
    // draws with Skia in 2D (Skia allocates its own stencil buffers
    // for offscreen targets when it needs them). The pbuffer bit is
    // only needed when there is no EGL_KHR_surfaceless_context.
    const EGLint surfaceType = EGL_WINDOW_BIT | (m_surfaceless ? 0 : EGL_PBUFFER_BIT);
    const EGLint attribs[] = { EGL_RENDERABLE_TYPE,
                               renderableType,
                               EGL_SURFACE_TYPE,
                               surfaceType,
                               EGL_RED_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_BLUE_SIZE,
                               8,
                               EGL_ALPHA_SIZE,
                               8,
                               EGL_DEPTH_SIZE,
                               0,
                               EGL_STENCIL_SIZE,
                               0,
                               EGL_NONE };

    EGLConfig configs[64];
    EGLint n = 0;
    if (!eglChooseConfig(m_display, attribs, configs, 64, &n) || n <= 0)
      return false;

    // eglChooseConfig() sorts bigger buffers first, look for the
    // exact RGBA8888 config without depth/stencil.
    m_config = configs[0];
    for (EGLint i = 0; i < n; ++i) {
      EGLint r, g, b, a, d, s;
      eglGetConfigAttrib(m_display, configs[i], EGL_RED_SIZE, &r);
      eglGetConfigAttrib(m_display, configs[i], EGL_GREEN_SIZE, &g);
      eglGetConfigAttrib(m_display, configs[i], EGL_BLUE_SIZE, &b);
      eglGetConfigAttrib(m_display, configs[i], EGL_ALPHA_SIZE, &a);
      eglGetConfigAttrib(m_display, configs[i], EGL_DEPTH_SIZE, &d);
      eglGetConfigAttrib(m_display, configs[i], EGL_STENCIL_SIZE, &s);
      if (r == 8 && g == 8 && b == 8 && a == 8 && d == 0 && s == 0) {
        m_config = configs[i];
        break;
      }
    }
    return true;
  }

  bool createContext()
  {
    // Don't try again and again (on each resize) if EGL doesn't work,
    // the window uses the raster backend.
    if (m_contextFailed)
      return false;
    m_contextFailed = true;

    if (!initDisplay())
      return false;

    // OpenGL ES 3, or 2 as a fallback (Skia works with both).
    int version = 3;
    if (!chooseConfig(EGL_OPENGL_ES3_BIT_KHR)) {
      version = 2;
      if (!chooseConfig(EGL_OPENGL_ES2_BIT)) {
        LAF_EGL_LOGE("GL: no RGBA8888 EGLConfig for OpenGL ES");
        return false;
      }
    }

    for (; version >= 2; --version) {
      const EGLint attribs[] = { EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE };
      m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, attribs);
      if (m_context != EGL_NO_CONTEXT)
        break;
      checkError("eglCreateContext");
    }
    if (m_context == EGL_NO_CONTEXT)
      return false;

    if (!makeCurrentWithoutSurface()) {
      eglDestroyContext(m_display, m_context);
      m_context = EGL_NO_CONTEXT;
      return false;
    }

    m_contextFailed = false;
    LAF_EGL_LOGI("GL: EGLContext created (OpenGL ES %d)", version);
    return true;
  }

  bool createSurface()
  {
    // A failure is reported only once per native window (e.g. the
    // window is already connected to the CPU renderer, see
    // SkiaWindowAndroid).
    if (m_surfaceFailed)
      return false;

    m_surface = eglCreateWindowSurface(m_display, m_config, m_window, nullptr);
    if (m_surface == EGL_NO_SURFACE) {
      checkError("eglCreateWindowSurface");
      m_surfaceFailed = true;
      return false;
    }

    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
      checkError("eglMakeCurrent");
      eglDestroySurface(m_display, m_surface);
      m_surface = EGL_NO_SURFACE;
      m_surfaceFailed = true;
      makeCurrentWithoutSurface();
      return false;
    }

    // Wait the vertical sync (one frame per refresh).
    eglSwapInterval(m_display, 1);

    ++m_surfaceCount;
    m_swapFailed = false;

    int w, h;
    surfaceSize(w, h);
    LAF_EGL_LOGI("GL: EGLSurface #%d created (%dx%d)", m_surfaceCount, w, h);
    return true;
  }

  void destroySurface()
  {
    if (m_surface == EGL_NO_SURFACE)
      return;

    // The surface must not be current to be destroyed right now (and
    // to disconnect it from the native window).
    makeCurrentWithoutSurface();
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
    LAF_EGL_LOGI("GL: EGLSurface #%d destroyed", m_surfaceCount);
  }

  bool makeCurrentWithoutSurface()
  {
    if (m_context == EGL_NO_CONTEXT)
      return false;

    if (m_surfaceless) {
      if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context))
        return true;
      checkError("eglMakeCurrent(EGL_NO_SURFACE)");
      return false;
    }

    if (m_pbuffer == EGL_NO_SURFACE) {
      const EGLint attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
      m_pbuffer = eglCreatePbufferSurface(m_display, m_config, attribs);
      if (m_pbuffer == EGL_NO_SURFACE) {
        checkError("eglCreatePbufferSurface");
        return false;
      }
    }
    if (eglMakeCurrent(m_display, m_pbuffer, m_pbuffer, m_context))
      return true;
    checkError("eglMakeCurrent(pbuffer)");
    return false;
  }

  void checkError(const char* function)
  {
    const EGLint err = eglGetError();
    if (err == EGL_CONTEXT_LOST)
      m_contextLost = true;
    LAF_EGL_LOGE("GL: %s() failed (EGL error 0x%04x)", function, int(err));
  }

  static bool has_extension(const char* exts, const char* name)
  {
    if (!exts)
      return false;
    const size_t len = std::strlen(name);
    for (const char* p = exts; (p = std::strstr(p, name)) != nullptr; p += len) {
      if ((p == exts || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
        return true;
    }
    return false;
  }

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLContext m_context = EGL_NO_CONTEXT;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLSurface m_pbuffer = EGL_NO_SURFACE;
  EGLNativeWindowType m_window = nullptr;
  bool m_surfaceless = false;
  bool m_contextLost = false;
  bool m_contextFailed = false;
  bool m_surfaceFailed = false;
  bool m_swapFailed = false;
  int m_surfaceCount = 0;
};

} // namespace os

#endif
