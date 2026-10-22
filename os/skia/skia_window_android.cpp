// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/skia/skia_window_android.h"

#include "os/android/app.h"
#include "os/event.h"
#include "os/event_queue.h"
#include "os/gl/gl_context_egl.h"
#include "os/skia/skia_surface.h"
#include "os/skia/skia_window.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"

#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace os {

// Skia is RGBA on Android (ADR 0005). If laf was compiled with
// SK_R32_SHIFT=16 (BGRA, the desktop Linux setting) its view of
// SkPMColor would not match the Skia library.
static_assert(kN32_SkColorType == kRGBA_8888_SkColorType,
              "laf must use the RGBA Skia configuration on Android");

namespace {

// Pixel layout of the ANativeWindow buffer as a Skia color type
// (WINDOW_FORMAT_* values are the same as AHARDWAREBUFFER_FORMAT_*).
// 8888 buffers are written as kRGB_888x so the alpha byte is always
// 0xFF: the laf surface is opaque but its alpha channel can be 0
// (SkiaSurface::create() erases it to transparent).
SkColorType color_type_from_buffer_format(const int32_t format)
{
  switch (format) {
    case WINDOW_FORMAT_RGBA_8888:
    case WINDOW_FORMAT_RGBX_8888: return kRGB_888x_SkColorType;
    case WINDOW_FORMAT_RGB_565:   return kRGB_565_SkColorType;
  }
  return kUnknown_SkColorType;
}

// Value of "debug.laf.gpu": -1 (not set, the app decides), 0 (raster)
// or 1 (GPU). Read once per process.
int gpu_property()
{
  static const int value = []() -> int {
    char value[PROP_VALUE_MAX] = { 0 };
    if (__system_property_get("debug.laf.gpu", value) <= 0)
      return -1;
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0)
      return 1;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0)
      return 0;
    return -1;
  }();
  return value;
}

const char* gl_string(const GLenum name)
{
  const auto* str = reinterpret_cast<const char*>(glGetString(name));
  return (str ? str : "?");
}

// Interval of the frame statistics in verbose mode.
constexpr double kFrameStatsSecs = 2.0;

} // anonymous namespace

SkiaWindowAndroid::SkiaWindowAndroid(const WindowSpec& spec) : Base(spec)
{
#if SK_SUPPORT_GPU
  auto eglCtx = std::make_unique<GLContextEGL>();
  m_eglCtx = eglCtx.get();
  m_glCtx = std::move(eglCtx);

  // The ANativeWindow can be here already if APP_CMD_INIT_WINDOW
  // was processed before the window creation (see WindowAndroid).
  m_eglCtx->setNativeWindow(nativeWindow());

  // GPU acceleration is on by default on Android, the app can turn
  // it off with setGpuAcceleration(false) (it's applied before the
  // first paint if the app does it right after creating the window).
  if (gpu_property() >= 0)
    LAF_ALOGI("GL: debug.laf.gpu=%d (overrides the app)", gpu_property());
  setGpuAcceleration(true);
#endif
  initColorSpace();
}

void SkiaWindowAndroid::setGpuAcceleration(bool state)
{
  if (gpu_property() >= 0)
    state = (gpu_property() == 1);

  if (state != m_gpuRequested) {
    LAF_ALOGI("GL: GPU acceleration %s", state ? "on" : "off");
    if (state && m_rasterOwnsWindow && nativeWindow()) {
      LAF_ALOGI("GL: the current ANativeWindow is used by the raster backend, "
                "the GPU backend starts with the next window (e.g. Home and back)");
    }
  }
  m_gpuRequested = state;

  Base::setGpuAcceleration(state);
  logGpuState("setGpuAcceleration");
}

void SkiaWindowAndroid::swapBuffers()
{
#if SK_SUPPORT_GPU
  if (backend() != Backend::GL || !m_eglCtx)
    return;

  const auto t0 = std::chrono::steady_clock::now();
  Base::swapBuffers();
  const auto t1 = std::chrono::steady_clock::now();

  if (m_eglCtx->isContextLost()) {
    recreateLostGpuContext();
    return;
  }

  checkGpuContext();

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (m_firstGpuFrame) {
    m_firstGpuFrame = false;
    LAF_ALOGI("GL: first frame on EGLSurface #%d (flush+swap %.2f ms)",
              m_eglCtx->surfaceCount(),
              ms);
    resetFrameStats();
    return;
  }

  if (AppAndroid::verbose()) {
    ++m_statsFrames;
    m_statsSwapTotal += ms;
    m_statsSwapMax = std::max(m_statsSwapMax, ms);

    const double secs = std::chrono::duration<double>(t1 - m_statsStart).count();
    if (secs >= kFrameStatsSecs) {
      LAF_ALOGV("GL: %d frames in %.1f s (%.1f fps), flush+swap avg %.2f ms max %.2f ms",
                m_statsFrames,
                secs,
                m_statsFrames / secs,
                m_statsSwapTotal / m_statsFrames,
                m_statsSwapMax);
      resetFrameStats();
    }
  }
#endif
}

// CPU raster path (M1.5): copies the laf surface (scaled by scale())
// to the ANativeWindow buffer, like SkiaWindowX11::onPaint().
void SkiaWindowAndroid::onPaint(const gfx::Rect& rc)
{
#if SK_SUPPORT_GPU
  if (backend() == Backend::GL)
    return;
#endif

  ANativeWindow* window = nativeWindow();
  auto surface = static_cast<SkiaSurface*>(this->surface());
  if (!window || !surface || !surface->isValid() || rc.isEmpty())
    return;

  useWindowForRaster();

  const auto t0 = std::chrono::steady_clock::now();

  if (!m_buffersReady) {
    // A 0x0 size means "the window size", so the buffers follow the
    // window when it's resized (rotation, split screen).
    const int32_t res = ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    if (res != 0 && !m_buffersFailed) {
      LAF_ALOGE("Raster: ANativeWindow_setBuffersGeometry() failed (%d)", int(res));
      m_buffersFailed = true;
    }
    m_buffersReady = true;
  }

  ARect dirty;
  dirty.left = rc.x;
  dirty.top = rc.y;
  dirty.right = rc.x2();
  dirty.bottom = rc.y2();

  ANativeWindow_Buffer buffer;
  const int32_t res = ANativeWindow_lock(window, &buffer, &dirty);
  if (res != 0) {
    if (!m_lockFailed) {
      LAF_ALOGE("Raster: ANativeWindow_lock() failed (%d)", int(res));
      m_lockFailed = true;
    }
    return;
  }
  m_lockFailed = false;

  const SkBitmap& bitmap = surface->bitmap();
  const int scale = this->scale();
  const SkColorType colorType = color_type_from_buffer_format(buffer.format);

  if (m_firstPaint) {
    LAF_ALOGI("Raster: buffer %dx%d stride=%d format=%d, surface %dx%d colorType=%d scale=%d",
              buffer.width,
              buffer.height,
              buffer.stride,
              buffer.format,
              bitmap.width(),
              bitmap.height(),
              int(bitmap.colorType()),
              scale);
  }

  if (colorType == kUnknown_SkColorType) {
    if (m_firstPaint)
      LAF_ALOGE("Raster: unsupported buffer format %d", buffer.format);
    m_firstPaint = false;
    ANativeWindow_unlockAndPost(window);
    return;
  }

  // Same color space as the source: no color conversion, only the
  // pixel format conversion (RGBA -> RGBX/565).
  const SkImageInfo dstInfo = SkImageInfo::Make(buffer.width,
                                                buffer.height,
                                                colorType,
                                                kOpaque_SkAlphaType,
                                                bitmap.refColorSpace());
  const SkPixmap dst(dstInfo,
                     buffer.bits,
                     size_t(buffer.stride) * SkColorTypeBytesPerPixel(colorType));

  // ANativeWindow_lock() can return a bigger dirty area than the
  // requested one (the whole buffer when the previous frame cannot
  // be copied back), all of it must be painted.
  const SkIRect bufferBounds = SkIRect::MakeWH(buffer.width, buffer.height);
  SkIRect dirtyRc = SkIRect::MakeLTRB(dirty.left, dirty.top, dirty.right, dirty.bottom);

  if (dirtyRc.intersect(bufferBounds)) {
    // Area covered by the scaled surface. The buffer can be bigger
    // while a resize is in progress (the surface is resized later),
    // the rest is cleared.
    SkIRect covered = SkIRect::MakeWH(bitmap.width() * scale, bitmap.height() * scale);
    if (!covered.intersect(bufferBounds))
      covered.setEmpty();
    if (!covered.contains(dirtyRc))
      dst.erase(SK_ColorBLACK, dirtyRc);

    SkIRect paintRc = dirtyRc;
    if (paintRc.intersect(covered)) {
      if (scale == 1) {
        SkPixmap dstPixels;
        if (dst.extractSubset(&dstPixels, paintRc))
          bitmap.pixmap().readPixels(dstPixels, paintRc.x(), paintRc.y());
      }
      else {
        std::unique_ptr<SkCanvas> canvas =
          SkCanvas::MakeRasterDirect(dst.info(), dst.writable_addr(), dst.rowBytes());
        if (canvas) {
          SkPaint paint;
          paint.setBlendMode(SkBlendMode::kSrc);
          canvas->clipIRect(paintRc);
          canvas->scale(SkScalar(scale), SkScalar(scale));
          canvas->drawImage(SkImages::RasterFromPixmap(bitmap.pixmap(), nullptr, nullptr),
                            0,
                            0,
                            SkSamplingOptions(SkFilterMode::kNearest),
                            &paint);
        }
      }
    }

    if (m_firstPaint && AppAndroid::verbose())
      logColorProbe(dst);
  }

  ANativeWindow_unlockAndPost(window);
  m_firstPaint = false;

  if (AppAndroid::verbose()) {
    const auto t1 = std::chrono::steady_clock::now();
    LAF_ALOGV("Raster: paint rc=(%d %d %d %d) dirty=(%d %d %d %d) %.2f ms",
              rc.x,
              rc.y,
              rc.w,
              rc.h,
              dirtyRc.x(),
              dirtyRc.y(),
              dirtyRc.width(),
              dirtyRc.height(),
              std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
}

void SkiaWindowAndroid::onNativeWindowAttached()
{
  // New ANativeWindow: its buffer format must be set again.
  resetRasterState();

#if SK_SUPPORT_GPU
  if (!m_eglCtx)
    return;

  if (m_eglCtx->isContextLost())
    recreateLostGpuContext();

  // A new ANativeWindow is not connected to the CPU, so the GPU
  // backend can be used again (or for the first time).
  m_eglCtx->setNativeWindow(nativeWindow());
  if (m_gpuRequested) {
    // New EGLSurface + render target (the size can be the same).
    // WindowAndroid calls onResize() after this to repaint.
    resetSkiaSurface();
  }
  logGpuState("window attached");
#endif
}

void SkiaWindowAndroid::onNativeWindowDetached()
{
#if SK_SUPPORT_GPU
  if (m_eglCtx) {
    const bool hadSurface = m_eglCtx->hasSurface();
    if (hadSurface && backend() == Backend::GL && m_gl.grCtx())
      m_gl.grCtx()->flushAndSubmit();

    // Destroys the EGLSurface, the EGLContext and the GrDirectContext
    // stay alive (current without surface).
    m_eglCtx->setNativeWindow(nullptr);

    // Replace the SkSurface wrapping the destroyed framebuffer with a
    // raster one, so the app can keep drawing while it's in
    // background. The window is repainted when it's attached again.
    if (hadSurface)
      resetSkiaSurface();
    logGpuState("window detached");
  }
#endif

  resetRasterState();
}

void SkiaWindowAndroid::onResize(const gfx::Size& sz)
{
  Base::onResize(sz);
  logGpuState("resize");
}

void SkiaWindowAndroid::resetRasterState()
{
  m_buffersReady = false;
  m_buffersFailed = false;
  m_lockFailed = false;
  m_firstPaint = true;
  m_rasterOwnsWindow = false;
}

// Called before the first ANativeWindow_lock() of each window.
void SkiaWindowAndroid::useWindowForRaster()
{
  if (m_rasterOwnsWindow)
    return;
  m_rasterOwnsWindow = true;

#if SK_SUPPORT_GPU
  if (m_eglCtx) {
    // An EGLSurface would make ANativeWindow_lock() fail, and EGL
    // cannot use the window after it.
    if (m_eglCtx->hasSurface())
      LAF_ALOGW("Raster: destroying the unused EGLSurface");
    m_eglCtx->setNativeWindow(nullptr);
  }
#endif

  LAF_ALOGI("Raster: using the raster backend for this window (GPU acceleration %s)",
            m_gpuRequested ? "requested but not available" : "off");
}

void SkiaWindowAndroid::recreateLostGpuContext()
{
#if SK_SUPPORT_GPU
  LAF_ALOGE("GL: EGL context lost, creating the GL context and the GrDirectContext again");

  m_gl.detachGL(); // Abandons the GrDirectContext
  m_eglCtx->destroyGLContext();
  m_lastGrCtx = nullptr;
  resetSkiaSurface();
  logGpuState("context lost");

  // Repaint everything.
  Event ev;
  ev.setType(Event::ResizeWindow);
  ev.setWindow(AddRef(this));
  queue_event(ev);
#endif
}

// Logs the creation of a GrDirectContext (once per process unless
// GPU acceleration is turned off/on or the context is lost) and of a
// new EGLSurface.
void SkiaWindowAndroid::checkGpuContext()
{
#if SK_SUPPORT_GPU
  const GrDirectContext* grCtx = m_gl.grCtx();
  if (grCtx && grCtx != m_lastGrCtx) {
    ++m_grCtxCount;
    // The context is current on this thread (GrDirectContext alive).
    LAF_ALOGI("OS: Using OpenGL backend (GrDirectContext #%d, %s, %s)",
              m_grCtxCount,
              gl_string(GL_RENDERER),
              gl_string(GL_VERSION));
  }
  m_lastGrCtx = grCtx;

  if (m_eglCtx && m_eglCtx->surfaceCount() != m_lastSurfaceCount) {
    m_lastSurfaceCount = m_eglCtx->surfaceCount();
    m_firstGpuFrame = true;
  }
#endif
}

void SkiaWindowAndroid::logGpuState(const char* reason)
{
  checkGpuContext();

#if SK_SUPPORT_GPU
  if (AppAndroid::verbose()) {
    int sw = 0, sh = 0;
    if (m_eglCtx)
      m_eglCtx->surfaceSize(sw, sh);
    const gfx::Size sz = clientSize();
    LAF_ALOGV("GL: %s: backend=%s window=%dx%d scale=%d EGLSurface=%dx%d",
              reason,
              backend() == Backend::GL ? "GL" : "raster",
              sz.w,
              sz.h,
              scale(),
              sw,
              sh);
  }
#endif
}

void SkiaWindowAndroid::resetFrameStats()
{
  m_statsStart = std::chrono::steady_clock::now();
  m_statsFrames = 0;
  m_statsSwapTotal = 0.0;
  m_statsSwapMax = 0.0;
}

// Debug helper to check the channel order on the device (verbose
// mode, first paint of each ANativeWindow): logs the first pixel of
// the first row whose red and blue components differ, as the laf
// surface sees it (SkColor, independent of the memory layout) and as
// raw bytes in the window buffer (RGBX: R G B FF expected).
void SkiaWindowAndroid::logColorProbe(const SkPixmap& dst)
{
  auto surface = static_cast<SkiaSurface*>(this->surface());
  const SkBitmap& bitmap = surface->bitmap();
  const int scale = this->scale();

  for (int x = 0; x < bitmap.width(); ++x) {
    const SkColor c = bitmap.getColor(x, 0);
    if (SkColorGetR(c) == SkColorGetB(c))
      continue;

    const int dx = x * scale;
    if (dx >= dst.width() || dst.height() < 1 || dst.colorType() != kRGB_888x_SkColorType)
      break;

    const auto* p = static_cast<const uint8_t*>(dst.addr(dx, 0));
    LAF_ALOGV(
      "Raster: color probe surface(%d,0)=#%02x%02x%02x buffer(%d,0) bytes=%02x %02x %02x %02x",
      x,
      SkColorGetR(c),
      SkColorGetG(c),
      SkColorGetB(c),
      dx,
      p[0],
      p[1],
      p[2],
      p[3]);
    return;
  }
  LAF_ALOGV("Raster: color probe found no colored pixel in the first row");
}

} // namespace os
