// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/android_app.h"
#include "os/android/app.h"

#include <cstdlib>

extern int app_main(int argc, char* argv[]);

// Entry point called by the GameActivity glue on its own thread
// (ADR 0001). This is the Android version of os/common/main.cpp:
// app_main() runs here, and this thread becomes the main/UI thread of
// the app.
extern "C" void android_main(struct android_app* app)
{
  LAF_ALOGI("android_main: start (sdk %d)", app->activity->sdkVersion);

  {
    os::AppAndroid appAndroid(app);
    appAndroid.setupEnvironment();
    appAndroid.setupInput();

    if (os::android_pre_main) {
      LAF_ALOGI("android_main: calling android_pre_main()");
      os::android_pre_main(app);
    }

    char arg0[] = "laf";
    char* argv[] = { arg0, nullptr };
    LAF_ALOGI("android_main: calling app_main()");
    const int result = app_main(1, argv);
    os::AppAndroid::setAppMainReturned();
    LAF_ALOGI("android_main: app_main() returned %d", result);

    appAndroid.finish();
  }

  // The process is never reused for a second app_main() (static state
  // of the app is not designed for it), see ADR 0001. The result was
  // already logged, nobody reads the exit code of an app process.
  std::exit(0);
}
