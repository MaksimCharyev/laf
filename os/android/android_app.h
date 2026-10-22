// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_ANDROID_APP_H_INCLUDED
#define OS_ANDROID_ANDROID_APP_H_INCLUDED
#pragma once

// Android-only API for app code (include it under #if LAF_ANDROID).
// It doesn't include the GameActivity headers, link with laf-os to
// get them if you need the android_app fields.

struct android_app;

namespace os {

// Returns the android_app received in android_main() (it lives in
// laf's os/android/main.cpp), or nullptr outside android_main().
::android_app* android_app();

// Screen density in dpi (AConfiguration_getDensity(), e.g. 400 on a
// 2.5x screen), refreshed on each APP_CMD_CONFIG_CHANGED.  Returns 0
// if it's unknown.
int android_screen_density();

// Optional hook defined by the app. If it's defined, android_main()
// calls it once before app_main(), after HOME/XDG_CONFIG_HOME/TMPDIR
// are set, from the same thread that will run app_main() (e.g. to
// extract the APK assets).
//
// It's a weak symbol: the definition must be linked directly into the
// final shared library (an object file of the target itself, or with
// -Wl,-u,<mangled name>), because an undefined weak reference doesn't
// pull a member from a static library and the hook would be silently
// skipped.
void android_pre_main(::android_app* app) __attribute__((weak));

} // namespace os

#endif
