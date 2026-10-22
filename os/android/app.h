// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_APP_H_INCLUDED
#define OS_ANDROID_APP_H_INCLUDED
#pragma once

#include "gfx/border.h"
#include "gfx/point.h"
#include "gfx/size.h"
#include "os/android/pointer_input.h"

#include <android/log.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <jni.h>

#include <string>
#include <vector>

// All os/android messages go to logcat with the "laf" tag (the same
// tag used by base/log.cpp), so "adb logcat -s laf:V" shows them.
// LAF_ALOGV() messages are printed only when the verbose mode is on:
//
//   adb shell setprop debug.laf.verbose 1   (then restart the app)
//
#define LAF_ALOG_TAG   "laf"
#define LAF_ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LAF_ALOG_TAG, __VA_ARGS__)
#define LAF_ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LAF_ALOG_TAG, __VA_ARGS__)
#define LAF_ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LAF_ALOG_TAG, __VA_ARGS__)
#define LAF_ALOGV(...)                                                                             \
  do {                                                                                             \
    if (os::AppAndroid::verbose())                                                                 \
      __android_log_print(ANDROID_LOG_VERBOSE, LAF_ALOG_TAG, __VA_ARGS__);                         \
  } while (0)

namespace os {

class WindowAndroid;

// State of the GameActivity native glue (android_app). There is one
// instance, created by android_main() (os/android/main.cpp) on the
// glue thread, which is also the thread that runs app_main(), owns
// the window/EGL resources, and calls EventQueue::getEvent(). Unless
// it's said otherwise, the member functions must be called from that
// thread only.
class AppAndroid {
public:
  static AppAndroid* instance();
  static bool verbose() { return s_verbose; }

  // Window scale forced for testing (0 = use the scale requested by
  // the app), see WindowAndroid::setScale():
  //
  //   adb shell setprop debug.laf.scale 2   (then restart the app)
  //
  static int debugScale() { return s_debugScale; }

  // Called by android_main() when app_main() returns (from the glue
  // thread). Only used by the APP_CMD_DESTROY watchdog log.
  static void setAppMainReturned();

  explicit AppAndroid(::android_app* app);
  ~AppAndroid();

  ::android_app* app() const { return m_app; }

  // JNIEnv attached to the glue thread (for the whole android_main()).
  JNIEnv* jniEnv() const { return m_env; }

  // Sets HOME, XDG_CONFIG_HOME and TMPDIR (ADR 0002).
  void setupEnvironment();

  // Installs the motion/key event filters and enables the pointer
  // axes that GameActivity doesn't report by default (ADR 0006).
  void setupInput();

  // Last position of the pointer that drives the mouse, in window
  // pixels (see System::mousePosition()).
  gfx::Point mousePosition() const { return m_pointerInput.lastPosition(); }

  // Density in dpi (from AConfiguration, refreshed on
  // APP_CMD_CONFIG_CHANGED).
  int density() const { return m_density; }

  // Size of the ANativeWindow in pixels, or the last known size if
  // there is no window now (paused). Before the first window it's
  // estimated from the screen size in dp read at startup.
  // AConfiguration orientation and screen size are stale after a
  // rotation on HarmonyOS (NOTES.md section 9), so they're not used
  // after startup.
  gfx::Size screenSize() const;

  // Insets in pixels from each window edge that must be kept free
  // of UI because the display corners are rounded or there is a
  // display cutout (see refreshSafeArea()).
  const gfx::Border& safeArea() const { return m_safeArea; }

  // True between APP_CMD_RESUME and APP_CMD_PAUSE.
  bool isResumed() const { return m_resumed; }

  // True if getEvent() can wait without timeout when there are no
  // events: the app is paused and there is no surface to paint (ADR
  // 0007 section 4). Events queued from other threads still wake up
  // the looper.
  bool canWaitWithoutTimeout() const { return !m_resumed && !m_app->window; }

  bool isDestroyRequested() const { return m_app->destroyRequested != 0; }

  // The only os::Window bound to app->window (nullptr if there is no
  // window yet). Called by WindowAndroid's ctor/dtor.
  WindowAndroid* window() const { return m_window; }
  void setWindow(WindowAndroid* window);

  // Waits up to timeoutMs (-1 = no timeout, 0 = don't wait) for the
  // ALooper, then processes all pending APP_CMD_* commands and input
  // events without waiting again. Returns earlier if ALooper_wake()
  // is called (see EventQueueAndroid::queueEvent()).
  void pollLooper(int timeoutMs);

  // Called when app_main() returns: asks the activity to finish and
  // processes commands until APP_CMD_DESTROY (or a timeout).
  void finish();

private:
  static void onAppCmd(::android_app* app, int32_t cmd);
  void handleAppCmd(int32_t cmd);
  void processInput();
  void processMotionEvent(const GameActivityMotionEvent& ev, base::tick_t now);
  void queueInputEvents();
  void resetInput(const char* reason);
  int waitTimeout(int timeoutMs) const;
  void refreshConfiguration(const char* reason, bool startup);
  bool refreshSafeArea(const char* reason);
  void updateWindowSizeAndSafeArea(const char* reason);
  int roundedCornerRadius(int position);
  void startDestroyWatchdog();
  void initJni();
  void initRoundedCornersJni();
  std::string cacheDir();

  static bool s_verbose;
  static int s_debugScale;

  ::android_app* m_app;
  JNIEnv* m_env = nullptr;
  WindowAndroid* m_window = nullptr;
  bool m_resumed = false;
  int m_density = 0;
  gfx::Size m_screenSizeDp; // At startup only
  mutable gfx::Size m_lastWindowSize;
  gfx::Border m_safeArea = gfx::Border(-1); // Invalid until the first refreshSafeArea()

  // Translation of motion events into mouse events (ADR 0006).
  PointerInputAndroid m_pointerInput;
  MotionEventAndroid m_motion;      // Reused for each motion event
  std::vector<Event> m_inputEvents; // Output of m_pointerInput

  // JNI cache (global references), see os/android/jni.h.
  jclass m_activityClass = nullptr;
  jmethodID m_getCacheDir = nullptr;
  jclass m_fileClass = nullptr;
  jmethodID m_getAbsolutePath = nullptr;
  jobject m_display = nullptr;            // android.view.Display of the activity
  jmethodID m_getRoundedCorner = nullptr; // API 31
  jmethodID m_getRadius = nullptr;
};

} // namespace os

#endif
