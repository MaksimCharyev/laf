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
#include "os/skia/skia_surface.h"
#include "os/skia/skia_window.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"

#include <android/native_window.h>

#include <chrono>
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

} // anonymous namespace

SkiaWindowAndroid::SkiaWindowAndroid(const WindowSpec& spec) : Base(spec)
{
  // TODO M1.6 m_glCtx = GLContextEGL (see the plan in
  //      os/android/window.h). Without m_glCtx the window uses the
  //      raster backend.
  initColorSpace();
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
}

void SkiaWindowAndroid::onNativeWindowDetached()
{
  resetRasterState();
}

void SkiaWindowAndroid::resetRasterState()
{
  m_buffersReady = false;
  m_buffersFailed = false;
  m_lockFailed = false;
  m_firstPaint = true;
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
