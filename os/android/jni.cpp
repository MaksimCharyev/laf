// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/jni.h"

#include "os/android/app.h"

namespace os {

bool jni_exception_check(JNIEnv* env, const char* what)
{
  if (!env || !env->ExceptionCheck())
    return false;

  // ExceptionDescribe() prints the Java stack trace to logcat.
  env->ExceptionDescribe();
  env->ExceptionClear();
  LAF_ALOGE("JNI: exception in %s", what);
  return true;
}

std::string jni_to_string(JNIEnv* env, jstring str)
{
  std::string result;
  if (!env || !str)
    return result;

  const char* utf = env->GetStringUTFChars(str, nullptr);
  if (utf) {
    result = utf;
    env->ReleaseStringUTFChars(str, utf);
  }
  jni_exception_check(env, "GetStringUTFChars");
  return result;
}

} // namespace os
