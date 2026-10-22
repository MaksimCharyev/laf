// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_DOWNLOADS_H_INCLUDED
#define OS_ANDROID_DOWNLOADS_H_INCLUDED
#pragma once

#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>

namespace os {

// Public "Download" folder through MediaStore.Downloads (ADR 0010):
// the app writes files to a folder in its cache that mirrors
// "Download/" and then each file is published (copied) to the
// MediaStore entry with the same relative path and name, owned by the
// app. No storage permission is needed for the app's own entries.
//
// All the member functions can be called from any thread.
class DownloadsAndroid {
public:
  static DownloadsAndroid* instance();

  std::string cachePath() const;
  bool publish(const std::string& path, std::string& error);

private:
  DownloadsAndroid();
  bool initJni(JNIEnv* env);

  // JNI cache, see os/android/jni.h.
  std::mutex m_jniMutex;
  std::atomic<bool> m_jniReady = false;
  jobject m_activity = nullptr; // GameActivity's global reference (not owned)
  jmethodID m_publish = nullptr;
};

} // namespace os

#endif
