// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/system.h"

#include "os/android/app.h"
#include "os/android/screen.h"

namespace os {

SystemAndroid::~SystemAndroid()
{
  destroyInstance();
}

bool SystemAndroid::isKeyPressed(const KeyScancode scancode)
{
  if (AppAndroid* app = AppAndroid::instance())
    return app->keyInput().isKeyPressed(scancode);
  return false;
}

void SystemAndroid::resetKeyPressed()
{
  if (AppAndroid* app = AppAndroid::instance())
    app->keyInput().reset();
}

int SystemAndroid::getUnicodeFromScancode(const KeyScancode scancode)
{
  if (AppAndroid* app = AppAndroid::instance())
    return app->keyInput().unicodeFromScancode(scancode);
  return 0;
}

void SystemAndroid::setTextInput(const bool state, const gfx::Point&)
{
  if (AppAndroid* app = AppAndroid::instance())
    app->setTextInput(state);
}

gfx::Point SystemAndroid::mousePosition() const
{
  if (AppAndroid* app = AppAndroid::instance())
    return app->mousePosition();
  return gfx::Point(0, 0);
}

ScreenRef SystemAndroid::primaryScreen()
{
  return make_ref<ScreenAndroid>();
}

void SystemAndroid::listScreens(ScreenList& list)
{
  list.push_back(make_ref<ScreenAndroid>());
}

} // namespace os
