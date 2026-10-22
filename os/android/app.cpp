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

#include <android/api-level.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
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

// Returns the integer value of a system property, or defValue if
// it's not set.
int read_int_property(const char* name, const int defValue)
{
  char value[PROP_VALUE_MAX] = { 0 };
  if (__system_property_get(name, value) <= 0)
    return defValue;
  return std::atoi(value);
}

// Positions of android.view.RoundedCorner (API 31).
enum { kTopLeft = 0, kTopRight = 1, kBottomRight = 2, kBottomLeft = 3 };

// Inset needed on both sides of a rounded corner of the given radius
// so a rectangle doesn't go out of it: with a margin "m" on both
// sides, the corner of the rectangle is at (m, m) from the corner of
// the display, which is on the arc when m = r * (1 - 1/sqrt(2)).
int corner_inset(const int radius)
{
  if (radius <= 0)
    return 0;
  return int(std::ceil(radius * (1.0 - 1.0 / std::sqrt(2.0))));
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

// MotionEventAndroid (os/android/pointer_input.h) doesn't include the
// NDK headers, so its constants are checked here.
using M = MotionEventAndroid;
static_assert(M::kActionMask == AMOTION_EVENT_ACTION_MASK &&
                M::kActionPointerIndexMask == AMOTION_EVENT_ACTION_POINTER_INDEX_MASK &&
                M::kActionPointerIndexShift == AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT,
              "Action masks");
static_assert(M::kActionDown == AMOTION_EVENT_ACTION_DOWN &&
                M::kActionUp == AMOTION_EVENT_ACTION_UP &&
                M::kActionMove == AMOTION_EVENT_ACTION_MOVE &&
                M::kActionCancel == AMOTION_EVENT_ACTION_CANCEL &&
                M::kActionOutside == AMOTION_EVENT_ACTION_OUTSIDE &&
                M::kActionPointerDown == AMOTION_EVENT_ACTION_POINTER_DOWN &&
                M::kActionPointerUp == AMOTION_EVENT_ACTION_POINTER_UP &&
                M::kActionHoverMove == AMOTION_EVENT_ACTION_HOVER_MOVE &&
                M::kActionScroll == AMOTION_EVENT_ACTION_SCROLL &&
                M::kActionHoverEnter == AMOTION_EVENT_ACTION_HOVER_ENTER &&
                M::kActionHoverExit == AMOTION_EVENT_ACTION_HOVER_EXIT &&
                M::kActionButtonPress == AMOTION_EVENT_ACTION_BUTTON_PRESS &&
                M::kActionButtonRelease == AMOTION_EVENT_ACTION_BUTTON_RELEASE,
              "Actions");
static_assert(M::kToolUnknown == AMOTION_EVENT_TOOL_TYPE_UNKNOWN &&
                M::kToolFinger == AMOTION_EVENT_TOOL_TYPE_FINGER &&
                M::kToolStylus == AMOTION_EVENT_TOOL_TYPE_STYLUS &&
                M::kToolMouse == AMOTION_EVENT_TOOL_TYPE_MOUSE &&
                M::kToolEraser == AMOTION_EVENT_TOOL_TYPE_ERASER &&
                M::kToolPalm == AMOTION_EVENT_TOOL_TYPE_PALM,
              "Tool types");
static_assert(M::kButtonPrimary == AMOTION_EVENT_BUTTON_PRIMARY &&
                M::kButtonSecondary == AMOTION_EVENT_BUTTON_SECONDARY &&
                M::kButtonTertiary == AMOTION_EVENT_BUTTON_TERTIARY &&
                M::kButtonBack == AMOTION_EVENT_BUTTON_BACK &&
                M::kButtonForward == AMOTION_EVENT_BUTTON_FORWARD &&
                M::kButtonStylusPrimary == AMOTION_EVENT_BUTTON_STYLUS_PRIMARY &&
                M::kButtonStylusSecondary == AMOTION_EVENT_BUTTON_STYLUS_SECONDARY,
              "Buttons");
static_assert(M::kSourceClassPointer == AINPUT_SOURCE_CLASS_POINTER &&
                M::kSourceMouse == AINPUT_SOURCE_MOUSE,
              "Sources");
static_assert(M::kMaxPointers == GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT,
              "GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT must not be overridden");

// Original GameActivity callback installed by the glue: it only sets
// android_app::textInputState, without waking up the looper.
void (*g_glueTextInputEvent)(GameActivity*, const GameTextInputState*) = nullptr;

void on_text_input_event(GameActivity* activity, const GameTextInputState* state)
{
  if (g_glueTextInputEvent)
    g_glueTextInputEvent(activity, state);

  // Called on the Java UI thread: wake up the glue thread so the text
  // is processed now (see AppAndroid::processTextInput()).
  if (AppAndroid* app = AppAndroid::instance())
    ALooper_wake(app->app()->looper);
}

// Maximum distance between the two taps of a double click, in dp.
constexpr int kDoubleClickSlopDp = 16;

void pointer_input_log(const int level, const char* msg)
{
  if (level > 0)
    LAF_ALOGI("%s", msg);
  else
    LAF_ALOGV("%s", msg);
}

const char* motion_action_name(const int action)
{
  switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:           return "DOWN";
    case AMOTION_EVENT_ACTION_UP:             return "UP";
    case AMOTION_EVENT_ACTION_MOVE:           return "MOVE";
    case AMOTION_EVENT_ACTION_CANCEL:         return "CANCEL";
    case AMOTION_EVENT_ACTION_OUTSIDE:        return "OUTSIDE";
    case AMOTION_EVENT_ACTION_POINTER_DOWN:   return "POINTER_DOWN";
    case AMOTION_EVENT_ACTION_POINTER_UP:     return "POINTER_UP";
    case AMOTION_EVENT_ACTION_HOVER_MOVE:     return "HOVER_MOVE";
    case AMOTION_EVENT_ACTION_SCROLL:         return "SCROLL";
    case AMOTION_EVENT_ACTION_HOVER_ENTER:    return "HOVER_ENTER";
    case AMOTION_EVENT_ACTION_HOVER_EXIT:     return "HOVER_EXIT";
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:   return "BUTTON_PRESS";
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: return "BUTTON_RELEASE";
  }
  return "?";
}

const char* tool_type_name(const int32_t toolType)
{
  switch (toolType) {
    case AMOTION_EVENT_TOOL_TYPE_FINGER: return "finger";
    case AMOTION_EVENT_TOOL_TYPE_STYLUS: return "stylus";
    case AMOTION_EVENT_TOOL_TYPE_MOUSE:  return "mouse";
    case AMOTION_EVENT_TOOL_TYPE_ERASER: return "eraser";
    case AMOTION_EVENT_TOOL_TYPE_PALM:   return "palm";
  }
  return "unknown";
}

const char* event_type_name(const Event::Type type)
{
  switch (type) {
    case Event::MouseEnter:       return "MouseEnter";
    case Event::MouseLeave:       return "MouseLeave";
    case Event::MouseMove:        return "MouseMove";
    case Event::MouseDown:        return "MouseDown";
    case Event::MouseUp:          return "MouseUp";
    case Event::MouseWheel:       return "MouseWheel";
    case Event::MouseDoubleClick: return "MouseDoubleClick";
    default:                      return "?";
  }
}

const char* pointer_type_name(const PointerType pointerType)
{
  switch (pointerType) {
    case PointerType::Mouse:  return "Mouse";
    case PointerType::Touch:  return "Touch";
    case PointerType::Pen:    return "Pen";
    case PointerType::Eraser: return "Eraser";
    default:                  return "Unknown";
  }
}

MotionPointerAndroid pointer_from_axes(const GameActivityPointerAxes& axes)
{
  MotionPointerAndroid p;
  p.id = axes.id;
  p.toolType = axes.toolType;
  p.x = GameActivityPointerAxes_getX(&axes);
  p.y = GameActivityPointerAxes_getY(&axes);
  p.pressure = GameActivityPointerAxes_getPressure(&axes);
  p.tilt = GameActivityPointerAxes_getAxisValue(&axes, AMOTION_EVENT_AXIS_TILT);
  p.orientation = GameActivityPointerAxes_getOrientation(&axes);
  return p;
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
  refreshSafeArea("startup");

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
    if (m_display)
      m_env->DeleteGlobalRef(m_display);
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
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_VSCROLL);
  GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_HSCROLL);

  m_pointerInput.setLogFunc(pointer_input_log);

  // Soft keyboard (M2.4): one-line text, "Done" action, no full-screen
  // editor, no suggestions (fewer composing rewrites). The glue
  // doesn't wake up the looper on text changes, so the callback is
  // wrapped.
  GameActivity_setImeEditorInfo(
    m_app->activity,
    GameTextInputType(TYPE_CLASS_TEXT | TYPE_TEXT_FLAG_NO_SUGGESTIONS),
    IME_ACTION_DONE,
    GameTextInputImeOptions(IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI));
  if (m_app->activity->callbacks->onTextInputEvent != on_text_input_event) {
    g_glueTextInputEvent = m_app->activity->callbacks->onTextInputEvent;
    m_app->activity->callbacks->onTextInputEvent = on_text_input_event;
  }

  LAF_ALOGI("android_main: input filters installed (all motion sources, system keys excluded), "
            "pen/finger/mouse -> mouse events (ADR 0006)");
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
  for (int ms = waitTimeout(timeoutMs);; ms = 0) {
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
      refreshSafeArea("APP_CMD_INIT_WINDOW");
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
      updateWindowSizeAndSafeArea(app_cmd_name(cmd));
      break;

    case APP_CMD_WINDOW_REDRAW_NEEDED:
      LAF_ALOGV("APP_CMD_WINDOW_REDRAW_NEEDED");
      if (m_window)
        m_window->requestRedraw();
      break;

    case APP_CMD_CONFIG_CHANGED:
      refreshConfiguration("APP_CMD_CONFIG_CHANGED", false);
      updateWindowSizeAndSafeArea("CONFIG_CHANGED");
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
      // The input stream ends here (no UP will arrive), release the
      // button/mouse so the UI doesn't keep a capture.
      resetInput("APP_CMD_PAUSE");
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

    case APP_CMD_LOST_FOCUS:
      LAF_ALOGV("APP_CMD_LOST_FOCUS");
      // Keys released while another window has the focus (e.g. the
      // notification shade) don't send their UP to us.
      m_keyInput.reset();
      break;

    case APP_CMD_EDITOR_ACTION:
      LAF_ALOGV("APP_CMD_EDITOR_ACTION: action=%d", m_app->editorAction);
      // "Done" key of the soft keyboard.
      if (m_textInputActive) {
        std::vector<Event> events;
        TextInputAndroid::editorAction(events);
        queueKeyEvents(events);
      }
      break;

    case APP_CMD_SOFTWARE_KB_VIS_CHANGED:
      LAF_ALOGV("APP_CMD_SOFTWARE_KB_VIS_CHANGED: visible=%d", int(m_app->softwareKeyboardVisible));
      // Hidden by the user (Back, keyboard button): the next focus on a
      // text field shows it again.
      if (!m_app->softwareKeyboardVisible && m_textInputActive) {
        m_textInputActive = false;
        LAF_ALOGI("input: soft keyboard hidden by the user");
      }
      break;

    case APP_CMD_GAINED_FOCUS: LAF_ALOGV("APP_CMD_%s", app_cmd_name(cmd)); break;

    case APP_CMD_WINDOW_INSETS_CHANGED:
      LAF_ALOGV("APP_CMD_WINDOW_INSETS_CHANGED");
      updateWindowSizeAndSafeArea("WINDOW_INSETS_CHANGED");
      break;

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
  const base::tick_t now = base::current_tick();

  android_input_buffer* ib = android_app_swap_input_buffers(m_app);
  if (ib) {
    // Window scale and density can change at any time.
    m_pointerInput.setScale(m_window ? m_window->scale() : 1);
    m_pointerInput.setDoubleClickSlop(m_density > 0 ? (kDoubleClickSlopDp * m_density + 80) / 160 :
                                                      PointerInputAndroid::kDefaultDoubleClickSlop);

    for (uint64_t i = 0; i < ib->motionEventsCount; ++i)
      processMotionEvent(ib->motionEvents[i], now);

    for (uint64_t i = 0; i < ib->keyEventsCount; ++i)
      processKeyEvent(ib->keyEvents[i]);

    android_app_clear_motion_events(ib);
    android_app_clear_key_events(ib);
  }

  if (m_app->textInputState)
    processTextInput();

  // Deferred MouseLeave (pen hover exit, ADR 0006 section 3).
  m_pointerInput.processTimers(now, m_inputEvents);
  queueInputEvents();
}

void AppAndroid::processMotionEvent(const GameActivityMotionEvent& ev, const base::tick_t now)
{
  // GameActivity copies at most 8 pointers (ADR 0006 section 10).
  const int pointerCount = std::min<int>(int(ev.pointerCount),
                                         GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT);
  const int historySize = (ev.historicalAxisValues ? std::max(0, ev.historySize) : 0);

  MotionEventAndroid& m = m_motion;
  m.action = ev.action;
  m.source = ev.source;
  m.buttonState = ev.buttonState;
  m.pointerCount = pointerCount;
  for (int p = 0; p < pointerCount; ++p)
    m.pointers[p] = pointer_from_axes(ev.pointers[p]);

  m.historySize = historySize;
  m.history.resize(size_t(historySize) * pointerCount);
  for (int h = 0; h < historySize; ++h) {
    for (int p = 0; p < pointerCount; ++p) {
      MotionPointerAndroid& hp = m.history[h * pointerCount + p];
      hp = m.pointers[p];
      hp.x = GameActivityMotionEvent_getHistoricalX(&ev, p, h);
      hp.y = GameActivityMotionEvent_getHistoricalY(&ev, p, h);
      hp.pressure = GameActivityMotionEvent_getHistoricalPressure(&ev, p, h);
      hp.tilt = GameActivityMotionEvent_getHistoricalAxisValue(&ev, AMOTION_EVENT_AXIS_TILT, p, h);
      hp.orientation = GameActivityMotionEvent_getHistoricalOrientation(&ev, p, h);
    }
  }

  m.vscroll = m.hscroll = 0.0f;
  if (pointerCount > 0) {
    m.vscroll = GameActivityPointerAxes_getAxisValue(&ev.pointers[0], AMOTION_EVENT_AXIS_VSCROLL);
    m.hscroll = GameActivityPointerAxes_getAxisValue(&ev.pointers[0], AMOTION_EVENT_AXIS_HSCROLL);
  }

  if (verbose()) {
    const int action = ev.action & AMOTION_EVENT_ACTION_MASK;
    const MotionPointerAndroid* p = (pointerCount > 0 ? &m.pointers[0] : nullptr);
    LAF_ALOGV("input: motion %s index=%d source=0x%x pointers=%u history=%d buttons=0x%x "
              "[0] id=%d %s x=%.1f y=%.1f pressure=%.3f tilt=%.2f orientation=%.2f",
              motion_action_name(action),
              int(m.actionIndex()),
              ev.source,
              ev.pointerCount,
              ev.historySize,
              ev.buttonState,
              (p ? p->id : -1),
              (p ? tool_type_name(p->toolType) : "-"),
              (p ? p->x : 0.0f),
              (p ? p->y : 0.0f),
              (p ? p->pressure : 0.0f),
              (p ? p->tilt : 0.0f),
              (p ? p->orientation : 0.0f));
  }

  m_pointerInput.processMotion(m, now, m_inputEvents);
}

void AppAndroid::processKeyEvent(const GameActivityKeyEvent& ev)
{
  KeyEventAndroid k;
  k.action = ev.action;
  k.keyCode = ev.keyCode;
  k.scanCode = ev.scanCode;
  k.metaState = ev.metaState;
  k.repeatCount = ev.repeatCount;
  k.unicodeChar = ev.unicodeChar;

  Event out;
  const bool queued = m_keyInput.processKey(k, out);
  LAF_ALOGV("input: key %s keyCode=%d scanCode=%d meta=0x%x repeat=%d unicode=0x%x device=%d "
            "source=0x%x -> %s scancode=%d unicode=0x%x modifiers=0x%x",
            (ev.action == AKEY_EVENT_ACTION_DOWN ? "DOWN" :
             ev.action == AKEY_EVENT_ACTION_UP   ? "UP" :
                                                   "?"),
            ev.keyCode,
            ev.scanCode,
            ev.metaState,
            ev.repeatCount,
            unsigned(ev.unicodeChar),
            ev.deviceId,
            ev.source,
            (queued ? (out.type() == Event::KeyDown ? "KeyDown" : "KeyUp") : "ignored"),
            (queued ? int(out.scancode()) : 0),
            (queued ? unsigned(out.unicodeChar()) : 0u),
            (queued ? unsigned(out.modifiers()) : 0u));
  if (!queued)
    return;

  std::vector<Event> events{ out };
  queueKeyEvents(events);
}

void AppAndroid::processTextInput()
{
  m_app->textInputState = 0;

  std::string text;
  GameActivity_getTextInputState(
    m_app->activity,
    [](void* context, const GameTextInputState* state) {
      if (state && state->text_UTF8)
        static_cast<std::string*>(context)->assign(state->text_UTF8, state->text_length);
    },
    &text);

  if (!m_textInputActive) {
    LAF_ALOGV("input: soft keyboard text ignored (text input is off)");
    return;
  }

  std::vector<Event> events;
  m_textInput.update(text, events);
  LAF_ALOGV("input: soft keyboard text \"%s\" -> %d key event(s)",
            text.c_str(),
            int(events.size()));
  queueKeyEvents(events);
}

void AppAndroid::queueKeyEvents(std::vector<Event>& events)
{
  for (Event& ev : events) {
    if (m_window)
      m_window->queueEvent(ev);
    else
      queue_event(ev);
  }
  events.clear();
}

void AppAndroid::setTextInput(const bool state)
{
  if (state == m_textInputActive)
    return;

  if (state && hasHardwareKeyboard()) {
    LAF_ALOGV("input: text input on, hardware keyboard attached (soft keyboard not shown)");
    return;
  }

  m_textInputActive = state;
  if (state) {
    // Start with an empty IME text, the app's field gets the typed
    // characters as key events (TextInputAndroid).
    m_textInput.reset();
    GameTextInputState empty = {};
    empty.text_UTF8 = "";
    empty.text_length = 0;
    empty.selection = { 0, 0 };
    empty.composingRegion = { SPAN_UNDEFINED, SPAN_UNDEFINED };
    GameActivity_setTextInputState(m_app->activity, &empty);
    GameActivity_showSoftInput(m_app->activity, 0);
    LAF_ALOGI("input: text input on, showing the soft keyboard");
  }
  else {
    GameActivity_hideSoftInput(m_app->activity, 0);
    LAF_ALOGI("input: text input off, hiding the soft keyboard");
  }
}

bool AppAndroid::hasHardwareKeyboard() const
{
  // Updated on APP_CMD_CONFIG_CHANGED (attaching a keyboard changes
  // the "keyboard" and "keyboardHidden" configuration).
  return (AConfiguration_getKeyboard(m_app->config) == ACONFIGURATION_KEYBOARD_QWERTY &&
          AConfiguration_getKeysHidden(m_app->config) != ACONFIGURATION_KEYSHIDDEN_YES);
}

void AppAndroid::queueInputEvents()
{
  for (Event& ev : m_inputEvents) {
    if (verbose()) {
      LAF_ALOGV("input: -> %s pos=%d,%d %s button=%d pressure=%.3f wheel=%d,%d",
                event_type_name(ev.type()),
                ev.position().x,
                ev.position().y,
                pointer_type_name(ev.pointerType()),
                int(ev.button()),
                ev.pressure(),
                ev.wheelDelta().x,
                ev.wheelDelta().y);
    }
    if (m_window)
      m_window->queueEvent(ev);
    else
      queue_event(ev);
  }
  m_inputEvents.clear();
}

void AppAndroid::resetInput(const char* reason)
{
  // No key UP will arrive for the keys held now.
  m_keyInput.reset();
  m_pointerInput.reset(m_inputEvents);
  if (!m_inputEvents.empty())
    LAF_ALOGV("input: reset (%s)", reason);
  queueInputEvents();
}

int AppAndroid::waitTimeout(const int timeoutMs) const
{
  // Wake up for the deferred MouseLeave (processTimers()).
  const int timer = m_pointerInput.nextTimeout(base::current_tick());
  if (timer >= 0 && (timeoutMs < 0 || timer < timeoutMs))
    return timer;
  return timeoutMs;
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

  LAF_ALOGI("%s: keyboard=%d keysHidden=%d (hardware keyboard %s)",
            reason,
            AConfiguration_getKeyboard(cfg),
            AConfiguration_getKeysHidden(cfg),
            hasHardwareKeyboard() ? "attached" : "not attached");

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

bool AppAndroid::refreshSafeArea(const char* reason)
{
  int radius[4];
  bool anyRadius = false;
  for (int i = 0; i < 4; ++i) {
    radius[i] = roundedCornerRadius(i);
    anyRadius |= (radius[i] > 0);
  }
  const char* source = "Display.getRoundedCorner()";

  // HarmonyOS 4 reports radius 0 for all corners (NOTES.md section
  // 9) but has the radius in dp in a Huawei property.
  if (!anyRadius) {
    const int dp = read_int_property("ro.config.fillet_radius_size", 0);
    if (dp > 0 && m_density > 0) {
      std::fill(radius, radius + 4, (dp * m_density + 80) / 160);
      source = "ro.config.fillet_radius_size";
    }
  }

  // Testing aid to try other radii (in pixels, 0 = no corners):
  //
  //   adb shell setprop debug.laf.corner_radius 40
  //
  const int forced = read_int_property("debug.laf.corner_radius", -1);
  if (forced >= 0) {
    std::fill(radius, radius + 4, forced);
    source = "debug.laf.corner_radius";
  }

  // The cutout and waterfall insets are kept by GameActivity (updated
  // before APP_CMD_WINDOW_INSETS_CHANGED), they are 0 if there are
  // none.
  ARect cutout = {}, waterfall = {};
  GameActivity_getWindowInsets(m_app->activity, GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT, &cutout);
  GameActivity_getWindowInsets(m_app->activity, GAMECOMMON_INSETS_TYPE_WATERFALL, &waterfall);

  const gfx::Border safeArea(std::max({ corner_inset(radius[kTopLeft]),
                                        corner_inset(radius[kBottomLeft]),
                                        int(cutout.left),
                                        int(waterfall.left) }),
                             std::max({ corner_inset(radius[kTopLeft]),
                                        corner_inset(radius[kTopRight]),
                                        int(cutout.top),
                                        int(waterfall.top) }),
                             std::max({ corner_inset(radius[kTopRight]),
                                        corner_inset(radius[kBottomRight]),
                                        int(cutout.right),
                                        int(waterfall.right) }),
                             std::max({ corner_inset(radius[kBottomLeft]),
                                        corner_inset(radius[kBottomRight]),
                                        int(cutout.bottom),
                                        int(waterfall.bottom) }));

  if (safeArea == m_safeArea)
    return false;

  m_safeArea = safeArea;
  LAF_ALOGI("%s: safe area insets=(%d %d %d %d) px, corner radii=(%d %d %d %d) px from %s, "
            "cutout=(%d %d %d %d) waterfall=(%d %d %d %d)",
            reason,
            safeArea.left(),
            safeArea.top(),
            safeArea.right(),
            safeArea.bottom(),
            radius[kTopLeft],
            radius[kTopRight],
            radius[kBottomRight],
            radius[kBottomLeft],
            source,
            cutout.left,
            cutout.top,
            cutout.right,
            cutout.bottom,
            waterfall.left,
            waterfall.top,
            waterfall.right,
            waterfall.bottom);
  return true;
}

void AppAndroid::updateWindowSizeAndSafeArea(const char* reason)
{
  const bool safeAreaChanged = refreshSafeArea(reason);
  if (!m_window)
    return;

  m_window->updateNativeWindowSize();

  // The app reads the safe area when the window is resized, so we
  // send a resize event if only the insets changed.
  if (safeAreaChanged)
    m_window->requestRedraw();
}

int AppAndroid::roundedCornerRadius(const int position)
{
  if (!m_env || !m_display || !m_getRoundedCorner || !m_getRadius)
    return 0;

  // Display.getRoundedCorner() returns the corner for the current
  // rotation, or null if the corner is not rounded.
  int radius = 0;
  jobject corner = m_env->CallObjectMethod(m_display, m_getRoundedCorner, jint(position));
  if (!jni_exception_check(m_env, "Display.getRoundedCorner()") && corner) {
    radius = m_env->CallIntMethod(corner, m_getRadius);
    if (jni_exception_check(m_env, "RoundedCorner.getRadius()"))
      radius = 0;
    m_env->DeleteLocalRef(corner);
  }
  return std::max(0, radius);
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

  // Rounded display corners: Activity.getDisplay() (API 30) and
  // Display.getRoundedCorner() (API 31).
  if (m_activityClass && android_get_device_api_level() >= 31)
    initRoundedCornersJni();
}

void AppAndroid::initRoundedCornersJni()
{
  jmethodID getDisplay =
    m_env->GetMethodID(m_activityClass, "getDisplay", "()Landroid/view/Display;");
  if (jni_exception_check(m_env, "GetMethodID(getDisplay)") || !getDisplay)
    return;

  jobject display = m_env->CallObjectMethod(m_app->activity->javaGameActivity, getDisplay);
  if (jni_exception_check(m_env, "Activity.getDisplay()") || !display)
    return;

  jclass displayClass = m_env->GetObjectClass(display);
  if (!jni_exception_check(m_env, "GetObjectClass(display)") && displayClass) {
    m_getRoundedCorner =
      m_env->GetMethodID(displayClass, "getRoundedCorner", "(I)Landroid/view/RoundedCorner;");
    if (jni_exception_check(m_env, "GetMethodID(getRoundedCorner)"))
      m_getRoundedCorner = nullptr;
    m_env->DeleteLocalRef(displayClass);
  }

  jclass cornerClass = m_env->FindClass("android/view/RoundedCorner");
  if (!jni_exception_check(m_env, "FindClass(android/view/RoundedCorner)") && cornerClass) {
    m_getRadius = m_env->GetMethodID(cornerClass, "getRadius", "()I");
    if (jni_exception_check(m_env, "GetMethodID(getRadius)"))
      m_getRadius = nullptr;
    m_env->DeleteLocalRef(cornerClass);
  }

  if (m_getRoundedCorner && m_getRadius)
    m_display = m_env->NewGlobalRef(display);
  m_env->DeleteLocalRef(display);
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

gfx::Border android_safe_area_insets()
{
  if (g_instance)
    return g_instance->safeArea();
  return gfx::Border();
}

} // namespace os
