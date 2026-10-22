// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/app.h"

#include "base/debug.h"
#include "base/time.h"
#include "os/android/android_app.h"
#include "os/android/event_queue.h"
#include "os/android/jni.h"
#include "os/android/window.h"
#include "os/event.h"

#include <android/configuration.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

namespace os {

namespace {

AppAndroid* g_instance = nullptr;

// Timeout to wait APP_CMD_DESTROY after GameActivity_finish().
constexpr base::tick_t kFinishTimeoutMsecs = 2000;

// Time after APP_CMD_DESTROY until the process is ended if it's still
// alive. The Java thread is blocked in onDestroy() until
// android_main() returns, so this must stay below the ANR timeout
// (5 s).
constexpr int kDestroyWatchdogMsecs = 2500;

std::atomic<bool> g_appMainReturned(false);
std::atomic<bool> g_destroyWatchdogStarted(false);

bool read_verbose_property()
{
  char value[PROP_VALUE_MAX] = { 0 };
  if (__system_property_get("debug.laf.verbose", value) <= 0)
    return false;
  return (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0);
}

int read_debug_scale_property()
{
  char value[PROP_VALUE_MAX] = { 0 };
  if (__system_property_get("debug.laf.scale", value) <= 0)
    return 0;
  const int scale = std::atoi(value);
  return (scale >= 1 && scale <= 9 ? scale : 0);
}

// Keys that must reach the system (volume UI, camera, media
// sessions, power) instead of the app (ADR 0006 section 11).
// Everything else, including Back and the M-Pencil double-tap
// (keyCode 718), is delivered to native code.
bool key_event_filter(const GameActivityKeyEvent* ev)
{
  switch (ev->keyCode) {
    case AKEYCODE_VOLUME_UP:
    case AKEYCODE_VOLUME_DOWN:
    case AKEYCODE_VOLUME_MUTE:
    case AKEYCODE_MUTE:
    case AKEYCODE_HEADSETHOOK:
    case AKEYCODE_CAMERA:
    case AKEYCODE_ZOOM_IN:
    case AKEYCODE_ZOOM_OUT:
    case AKEYCODE_FOCUS:
    case AKEYCODE_MEDIA_PLAY_PAUSE:
    case AKEYCODE_MEDIA_STOP:
    case AKEYCODE_MEDIA_NEXT:
    case AKEYCODE_MEDIA_PREVIOUS:
    case AKEYCODE_MEDIA_REWIND:
    case AKEYCODE_MEDIA_FAST_FORWARD:
    case AKEYCODE_MEDIA_PLAY:
    case AKEYCODE_MEDIA_PAUSE:
    case AKEYCODE_MEDIA_CLOSE:
    case AKEYCODE_MEDIA_EJECT:
    case AKEYCODE_MEDIA_RECORD:
    case AKEYCODE_MEDIA_AUDIO_TRACK:
    case AKEYCODE_MEDIA_TOP_MENU:
    case AKEYCODE_MEDIA_SKIP_FORWARD:
    case AKEYCODE_MEDIA_SKIP_BACKWARD:
    case AKEYCODE_MEDIA_STEP_FORWARD:
    case AKEYCODE_MEDIA_STEP_BACKWARD:
    case AKEYCODE_POWER:
    case AKEYCODE_SLEEP:
    case AKEYCODE_WAKEUP:
    case AKEYCODE_BRIGHTNESS_DOWN:
    case AKEYCODE_BRIGHTNESS_UP:       return false;
  }
  return true;
}

// The default glue filter accepts only SOURCE_TOUCHSCREEN; we want
// stylus, mouse and hover events too.
bool motion_event_filter(const GameActivityMotionEvent*)
{
  return true;
}

const char* app_cmd_name(const int32_t cmd)
{
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:             return "INIT_WINDOW";
    case APP_CMD_TERM_WINDOW:             return "TERM_WINDOW";
    case APP_CMD_WINDOW_RESIZED:          return "WINDOW_RESIZED";
    case APP_CMD_WINDOW_REDRAW_NEEDED:    return "WINDOW_REDRAW_NEEDED";
    case APP_CMD_CONTENT_RECT_CHANGED:    return "CONTENT_RECT_CHANGED";
    case APP_CMD_SOFTWARE_KB_VIS_CHANGED: return "SOFTWARE_KB_VIS_CHANGED";
    case APP_CMD_GAINED_FOCUS:            return "GAINED_FOCUS";
    case APP_CMD_LOST_FOCUS:              return "LOST_FOCUS";
    case APP_CMD_CONFIG_CHANGED:          return "CONFIG_CHANGED";
    case APP_CMD_LOW_MEMORY:              return "LOW_MEMORY";
    case APP_CMD_START:                   return "START";
    case APP_CMD_RESUME:                  return "RESUME";
    case APP_CMD_SAVE_STATE:              return "SAVE_STATE";
    case APP_CMD_PAUSE:                   return "PAUSE";
    case APP_CMD_STOP:                    return "STOP";
    case APP_CMD_DESTROY:                 return "DESTROY";
    case APP_CMD_WINDOW_INSETS_CHANGED:   return "WINDOW_INSETS_CHANGED";
    case APP_CMD_EDITOR_ACTION:           return "EDITOR_ACTION";
    case APP_CMD_KEY_EVENT:               return "KEY_EVENT";
    case APP_CMD_TOUCH_EVENT:             return "TOUCH_EVENT";
  }
  return "?";
}

} // anonymous namespace

bool AppAndroid::s_verbose = false;
int AppAndroid::s_debugScale = 0;

// static
AppAndroid* AppAndroid::instance()
{
  return g_instance;
}

AppAndroid::AppAndroid(::android_app* app) : m_app(app)
{
  ASSERT(!g_instance);
  g_instance = this;
  s_verbose = read_verbose_property();
  s_debugScale = read_debug_scale_property();
  if (s_debugScale)
    LAF_ALOGI("android_main: debug.laf.scale=%d (window scale forced)", s_debugScale);

  m_app->onAppCmd = &AppAndroid::onAppCmd;

  // The glue thread stays attached to the JVM until android_main()
  // returns (see ~AppAndroid).
  if (m_app->activity->vm->AttachCurrentThread(&m_env, nullptr) != JNI_OK) {
    LAF_ALOGE("JNI: AttachCurrentThread failed");
    m_env = nullptr;
  }
  initJni();

  refreshConfiguration("startup", true);

  // From now on, events queued from other threads wake up this
  // thread's looper.
  static_cast<EventQueueAndroid*>(EventQueue::instance())->setLooper(m_app->looper);
}

AppAndroid::~AppAndroid()
{
  static_cast<EventQueueAndroid*>(EventQueue::instance())->setLooper(nullptr);

  if (m_env) {
    if (m_activityClass)
      m_env->DeleteGlobalRef(m_activityClass);
    if (m_fileClass)
      m_env->DeleteGlobalRef(m_fileClass);
    m_app->activity->vm->DetachCurrentThread();
    m_env = nullptr;
  }

  m_app->onAppCmd = nullptr;
  g_instance = nullptr;
}

void AppAndroid::setupEnvironment()
{
  const char* filesDir = m_app->activity->internalDataPath;
  if (!filesDir || !*filesDir) {
    LAF_ALOGE("android_main: no internalDataPath, HOME is not set");
    return;
  }

  std::string tmpDir = cacheDir();
  if (tmpDir.empty()) {
    // Context.getCacheDir() is a sibling of getFilesDir() on every
    // Android version (/data/user/0/<package>/{files,cache}).
    tmpDir = filesDir;
    const auto pos = tmpDir.rfind('/');
    if (pos != std::string::npos && pos > 0)
      tmpDir = tmpDir.substr(0, pos) + "/cache";
    LAF_ALOGW("android_main: getCacheDir() failed, using %s", tmpDir.c_str());

    // getCacheDir() creates the directory, here we have to do it.
    if (mkdir(tmpDir.c_str(), 0700) != 0 && errno != EEXIST)
      LAF_ALOGE("android_main: cannot create %s (errno %d)", tmpDir.c_str(), errno);
  }

  setenv("HOME", filesDir, 1);
  setenv("XDG_CONFIG_HOME", filesDir, 1);
  setenv("TMPDIR", tmpDir.c_str(), 1);

  LAF_ALOGI("android_main: HOME=XDG_CONFIG_HOME=%s TMPDIR=%s", filesDir, tmpDir.c_str());
}

void AppAndroid::setupInput()
{
  android_app_set_motion_event_filter(m_app, motion_event_filter);
  android_app_set_key_event_filter(m_app, key_event_filter);

  // GameActivity reports only AXIS_X/Y unless other axes are enabled
  // (NOTES.md section 4, ADR 0006 section 1).
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_PRESSURE);
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_TILT);
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_ORIENTATION);
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_TOUCH_MAJOR);
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_TOUCH_MINOR);

  LAF_ALOGI("android_main: input filters installed (all motion sources, system keys excluded)");
}

gfx::Size AppAndroid::screenSize() const
{
  // The activity is full-screen, so the window size is the most
  // precise value we have (and it's always up to date).
  if (m_app->window) {
    const int w = ANativeWindow_getWidth(m_app->window);
    const int h = ANativeWindow_getHeight(m_app->window);
    if (w > 0 && h > 0) {
      m_lastWindowSize = gfx::Size(w, h);
      return m_lastWindowSize;
    }
  }

  if (m_lastWindowSize.w > 0 && m_lastWindowSize.h > 0)
    return m_lastWindowSize;

  // Only before the first window (e.g. the app creates its window
  // before APP_CMD_INIT_WINDOW).
  if (m_density > 0 && m_screenSizeDp.w > 0 && m_screenSizeDp.h > 0) {
    return gfx::Size((m_screenSizeDp.w * m_density + 80) / 160,
                     (m_screenSizeDp.h * m_density + 80) / 160);
  }
  return gfx::Size(0, 0);
}

void AppAndroid::setWindow(WindowAndroid* window)
{
  m_window = window;
}

void AppAndroid::pollLooper(int timeoutMs)
{
  for (int ms = timeoutMs;; ms = 0) {
    int events = 0;
    android_poll_source* source = nullptr;
    const int id = ALooper_pollOnce(ms, nullptr, &events, (void**)&source);
    if (id >= 0) {
      // LOOPER_ID_MAIN: an APP_CMD_* command. The glue runs
      // pre_exec_cmd(), our onAppCmd(), and post_exec_cmd(), so
      // e.g. TERM_WINDOW is fully handled before the Java thread
      // continues.
      if (source)
        source->process(m_app, source);
      continue;
    }
    if (id == ALOOPER_POLL_CALLBACK)
      continue;

    // ALOOPER_POLL_WAKE (ALooper_wake() from queueEvent()),
    // ALOOPER_POLL_TIMEOUT, or ALOOPER_POLL_ERROR.
    if (id == ALOOPER_POLL_ERROR)
      LAF_ALOGE("ALooper_pollOnce() error");
    break;
  }

  processInput();
}

void AppAndroid::finish()
{
  LAF_ALOGI("android_main: app_main() returned, finishing the activity");
  GameActivity_finish(m_app->activity);

  const base::tick_t deadline = base::current_tick() + kFinishTimeoutMsecs;
  while (!isDestroyRequested()) {
    const base::tick_t now = base::current_tick();
    if (now >= deadline) {
      LAF_ALOGW("android_main: APP_CMD_DESTROY not received in %d ms", int(kFinishTimeoutMsecs));
      break;
    }
    pollLooper(int(deadline - now));
  }
}

// static
void AppAndroid::onAppCmd(::android_app* app, int32_t cmd)
{
  if (g_instance && g_instance->m_app == app)
    g_instance->handleAppCmd(cmd);
}

void AppAndroid::handleAppCmd(const int32_t cmd)
{
  switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
      const gfx::Size sz = screenSize();
      LAF_ALOGI("APP_CMD_INIT_WINDOW: window %dx%d", sz.w, sz.h);
      if (m_window && m_app->window)
        m_window->attachNativeWindow(m_app->window);
      break;
    }

    case APP_CMD_TERM_WINDOW:
      LAF_ALOGI("APP_CMD_TERM_WINDOW");
      // Synchronous: the Java UI thread waits until we return, and
      // the ANativeWindow is invalid after that (ADR 0001).
      if (m_window)
        m_window->detachNativeWindow();
      break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
      LAF_ALOGV("APP_CMD_%s: contentRect=(%d %d %d %d)",
                app_cmd_name(cmd),
                m_app->contentRect.left,
                m_app->contentRect.top,
                m_app->contentRect.right,
                m_app->contentRect.bottom);
      if (m_window)
        m_window->updateNativeWindowSize();
      break;

    case APP_CMD_WINDOW_REDRAW_NEEDED:
      LAF_ALOGV("APP_CMD_WINDOW_REDRAW_NEEDED");
      if (m_window)
        m_window->requestRedraw();
      break;

    case APP_CMD_CONFIG_CHANGED:
      refreshConfiguration("APP_CMD_CONFIG_CHANGED", false);
      if (m_window)
        m_window->updateNativeWindowSize();
      break;

    case APP_CMD_RESUME: {
      LAF_ALOGI("APP_CMD_RESUME");
      m_resumed = true;
      Event ev;
      ev.setType(Event::AppEnter);
      queue_event(ev);
      break;
    }

    case APP_CMD_PAUSE: {
      LAF_ALOGI("APP_CMD_PAUSE");
      m_resumed = false;
      Event ev;
      ev.setType(Event::AppLeave);
      queue_event(ev);
      break;
    }

    case APP_CMD_START:
    case APP_CMD_STOP:
    case APP_CMD_SAVE_STATE: LAF_ALOGI("APP_CMD_%s", app_cmd_name(cmd)); break;

    case APP_CMD_LOW_MEMORY:
      // HarmonyOS sends it after every SAVE_STATE (NOTES.md section
      // 9), it's not real memory pressure (ADR 0007).
      LAF_ALOGV("APP_CMD_LOW_MEMORY (ignored)");
      break;

    case APP_CMD_DESTROY: {
      LAF_ALOGI("APP_CMD_DESTROY");
      startDestroyWatchdog();
      // Usually it's the answer to GameActivity_finish() (see
      // finish()), but if the system destroys the activity while
      // app_main() is running we ask the app to quit.
      Event ev;
      ev.setType(Event::CloseApp);
      queue_event(ev);
      break;
    }

    case APP_CMD_GAINED_FOCUS:
    case APP_CMD_LOST_FOCUS:
    case APP_CMD_SOFTWARE_KB_VIS_CHANGED:
    case APP_CMD_WINDOW_INSETS_CHANGED:
    case APP_CMD_EDITOR_ACTION:           LAF_ALOGV("APP_CMD_%s", app_cmd_name(cmd)); break;

    case APP_CMD_KEY_EVENT:
    case APP_CMD_TOUCH_EVENT:
      // Only wake-ups, input is read by processInput() after each
      // pollLooper().
      break;

    default: LAF_ALOGV("APP_CMD %d (unknown)", int(cmd)); break;
  }
}

void AppAndroid::processInput()
{
  android_input_buffer* ib = android_app_swap_input_buffers(m_app);
  if (!ib)
    return;

  // TODO M2.1-M2.3: translate motion/key events into os::Event (ADR
  //      0006). Until then they are only logged in verbose mode and
  //      dropped. Wheel/scroll deltas must use the sign convention of
  //      the X11/Windows backends (wheel up = negative y, wheel right =
  //      positive x, see get_mouse_wheel_delta() in os/x11/window.cpp),
  //      i.e. delta.y = -AXIS_VSCROLL and delta.x = +AXIS_HSCROLL; the
  //      app relies on it (state_with_wheel_behavior.cpp under
  //      LAF_ANDROID).
  if (verbose()) {
    for (uint64_t i = 0; i < ib->motionEventsCount; ++i) {
      const GameActivityMotionEvent& ev = ib->motionEvents[i];
      LAF_ALOGV("motion: action=0x%x source=0x%x pointers=%u tool=%d history=%d",
                ev.action,
                ev.source,
                ev.pointerCount,
                (ev.pointerCount > 0 ? ev.pointers[0].toolType : -1),
                ev.historySize);
    }
    for (uint64_t i = 0; i < ib->keyEventsCount; ++i) {
      const GameActivityKeyEvent& ev = ib->keyEvents[i];
      LAF_ALOGV("key: action=%d keyCode=%d scanCode=%d repeat=%d",
                ev.action,
                ev.keyCode,
                ev.scanCode,
                ev.repeatCount);
    }
  }

  android_app_clear_motion_events(ib);
  android_app_clear_key_events(ib);
}

void AppAndroid::refreshConfiguration(const char* reason, const bool startup)
{
  AConfiguration* cfg = m_app->config;

  // The glue reads the configuration once at startup; refresh it
  // so the density is not stale after a configuration change.
  AConfiguration_fromAssetManager(cfg, m_app->activity->assetManager);

  m_density = AConfiguration_getDensity(cfg);
  if (m_density == ACONFIGURATION_DENSITY_DEFAULT)
    m_density = ACONFIGURATION_DENSITY_MEDIUM;
  else if (m_density == ACONFIGURATION_DENSITY_ANY || m_density == ACONFIGURATION_DENSITY_NONE)
    m_density = 0;

  // Orientation and screen size in dp don't change after a rotation
  // on HarmonyOS (NOTES.md section 9, M1.4), the screen dp size is
  // used only before the first window, and both are logged as
  // information only.
  const gfx::Size screenDp(AConfiguration_getScreenWidthDp(cfg),
                           AConfiguration_getScreenHeightDp(cfg));
  if (startup)
    m_screenSizeDp = screenDp;

  const gfx::Size sz = screenSize();
  LAF_ALOGI("%s: density=%d (GameActivity %d) window=%dx%d px sdk=%d "
            "(AConfiguration, may be stale: orientation=%d screen=%dx%ddp)",
            reason,
            m_density,
            GameActivity_getDensityDpi(m_app->activity),
            sz.w,
            sz.h,
            AConfiguration_getSdkVersion(cfg),
            AConfiguration_getOrientation(cfg),
            screenDp.w,
            screenDp.h);
}

void AppAndroid::startDestroyWatchdog()
{
  if (g_destroyWatchdogStarted.exchange(true))
    return;

  // The thread doesn't use "this" (AppAndroid is destroyed before
  // android_main() ends the process).
  try {
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(kDestroyWatchdogMsecs));
      if (g_appMainReturned)
        LAF_ALOGW("APP_CMD_DESTROY watchdog: process still alive %d ms after APP_CMD_DESTROY, "
                  "calling _exit(0)",
                  kDestroyWatchdogMsecs);
      else
        LAF_ALOGW("APP_CMD_DESTROY watchdog: app_main() did not return %d ms after "
                  "APP_CMD_DESTROY, calling _exit(0)",
                  kDestroyWatchdogMsecs);
      _exit(0);
    }).detach();
  }
  catch (const std::exception& ex) {
    LAF_ALOGE("APP_CMD_DESTROY watchdog: cannot start thread (%s)", ex.what());
  }
}

// static
void AppAndroid::setAppMainReturned()
{
  g_appMainReturned = true;
}

void AppAndroid::initJni()
{
  if (!m_env)
    return;

  jclass cls = m_env->GetObjectClass(m_app->activity->javaGameActivity);
  if (!jni_exception_check(m_env, "GetObjectClass(activity)") && cls) {
    m_activityClass = (jclass)m_env->NewGlobalRef(cls);
    m_env->DeleteLocalRef(cls);

    m_getCacheDir = m_env->GetMethodID(m_activityClass, "getCacheDir", "()Ljava/io/File;");
    if (jni_exception_check(m_env, "GetMethodID(getCacheDir)"))
      m_getCacheDir = nullptr;
  }

  cls = m_env->FindClass("java/io/File");
  if (!jni_exception_check(m_env, "FindClass(java/io/File)") && cls) {
    m_fileClass = (jclass)m_env->NewGlobalRef(cls);
    m_env->DeleteLocalRef(cls);

    m_getAbsolutePath = m_env->GetMethodID(m_fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    if (jni_exception_check(m_env, "GetMethodID(getAbsolutePath)"))
      m_getAbsolutePath = nullptr;
  }
}

std::string AppAndroid::cacheDir()
{
  if (!m_env || !m_getCacheDir || !m_getAbsolutePath)
    return std::string();

  std::string result;
  jobject file = m_env->CallObjectMethod(m_app->activity->javaGameActivity, m_getCacheDir);
  if (!jni_exception_check(m_env, "getCacheDir()") && file) {
    auto path = (jstring)m_env->CallObjectMethod(file, m_getAbsolutePath);
    if (!jni_exception_check(m_env, "File.getAbsolutePath()") && path) {
      result = jni_to_string(m_env, path);
      m_env->DeleteLocalRef(path);
    }
    m_env->DeleteLocalRef(file);
  }
  return result;
}

// Public API (os/android/android_app.h)

::android_app* android_app()
{
  if (g_instance)
    return g_instance->app();
  return nullptr;
}

int android_screen_density()
{
  if (g_instance)
    return g_instance->density();
  return 0;
}

} // namespace os
