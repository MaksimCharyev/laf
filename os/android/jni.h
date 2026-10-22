// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OS_ANDROID_JNI_H_INCLUDED
#define OS_ANDROID_JNI_H_INCLUDED
#pragma once

#include <jni.h>

#include <string>

// Rules for all JNI code in os/android (debuggable builds run with
// CheckJNI, which aborts the process on a pending exception):
//
// 1. Look up jclass/jmethodID once (at startup) and keep the classes
//    as global references. Use GetObjectClass() on a known object or
//    FindClass() only for framework classes: FindClass() on a native
//    thread uses the system class loader and cannot see app classes.
// 2. Call jni_exception_check() after every GetMethodID() and every
//    Call*() before any other JNI call.
// 3. Delete local references created in loops.

namespace os {

// If there is a pending Java exception, it's logged (with "what" as
// context) and cleared. Returns true if there was an exception.
bool jni_exception_check(JNIEnv* env, const char* what);

// Converts a java.lang.String to UTF-8 (empty string if str is null).
std::string jni_to_string(JNIEnv* env, jstring str);

// Returns a new local reference to a java.lang.String with the given
// UTF-8 text (nullptr on error, the exception is cleared).
jstring jni_new_string(JNIEnv* env, const std::string& str);

// JNIEnv for the current thread. If the thread is not attached to the
// JVM (e.g. a worker thread of the app), it's attached here and
// detached in the destructor.
class JniThreadEnv {
public:
  explicit JniThreadEnv(JavaVM* vm);
  ~JniThreadEnv();
  JniThreadEnv(const JniThreadEnv&) = delete;
  JniThreadEnv& operator=(const JniThreadEnv&) = delete;

  JNIEnv* env() const { return m_env; }

private:
  JavaVM* m_vm;
  JNIEnv* m_env = nullptr;
  bool m_attached = false;
};

} // namespace os

#endif
