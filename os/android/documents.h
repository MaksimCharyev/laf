// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_DOCUMENTS_H_INCLUDED
#define OS_ANDROID_DOCUMENTS_H_INCLUDED
#pragma once

#include "base/paths.h"
#include "os/android/android_app.h"

#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace os {

// Storage Access Framework documents (ADR 0009): the system picker is
// shown by AsepriteActivity (Java), each picked content:// URI is
// mapped to a file in the app cache with the document's display name,
// and the app works with that file path. Files saved to a mapped path
// are copied back to their URI.
//
// pick() must be called from the glue thread (the one that runs
// app_main()). toProvider() and fromProvider() can be called from any
// thread.
class DocumentsAndroid {
public:
  static DocumentsAndroid* instance();

  // Result: -1 = error (no picker), 0 = cancelled, 1 = OK.
  int pick(AndroidDocumentPicker type,
           const std::string& initialPath,
           const std::string& defaultExtension,
           base::paths& output);

  bool toProvider(const std::string& path, std::string& error);
  void fromProvider(const std::string& path);

  // Called from the Java UI thread (AsepriteActivity.onActivityResult()).
  void onPicked(JNIEnv* env, int requestId, int result, jobjectArray uris, jobjectArray names);

private:
  struct Entry {
    std::string uri;
    std::string path;
    bool synced = false; // Copied from/to the URI in this process
  };

  DocumentsAndroid();
  bool initJni(JNIEnv* env);
  void loadRegistry();
  void saveRegistry();
  std::string pathForUri(const std::string& uri, const std::string& displayName) const;
  // Adds/moves the entry to the front of the registry (mutex locked).
  Entry& addEntry(const std::string& uri, const std::string& path);
  Entry* findByPath(const std::string& path);
  std::string copyUriToFile(JNIEnv* env, const std::string& uri, const std::string& path);
  std::string copyFileToUri(JNIEnv* env, const std::string& path, const std::string& uri);
  std::string renameDocument(JNIEnv* env, const std::string& uri, const std::string& name);
  void releasePermission(JNIEnv* env, const std::string& uri);
  std::string callStringMethod(JNIEnv* env,
                               jmethodID method,
                               const char* what,
                               const std::string& a,
                               const std::string& b);
  void waitForResumedWindow();

  // JNI cache (global references), see os/android/jni.h.
  std::mutex m_jniMutex;
  std::atomic<bool> m_jniReady = false;
  jobject m_activity = nullptr; // GameActivity's global reference (not owned)
  jmethodID m_startPicker = nullptr;
  jmethodID m_copyToFile = nullptr;
  jmethodID m_copyToUri = nullptr;
  jmethodID m_rename = nullptr;
  jmethodID m_release = nullptr;

  // Pending picker request (protected by m_mutex).
  int m_requestId = 0;
  bool m_done = false;
  int m_result = 0;
  std::vector<std::string> m_uris;
  std::vector<std::string> m_names;

  // Registry URI <-> path, most recently used first (protected by m_mutex).
  std::mutex m_mutex;
  bool m_registryLoaded = false;
  std::vector<Entry> m_entries;
};

} // namespace os

#endif
