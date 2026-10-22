// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/jni.h"

#include "base/string.h"
#include "os/android/app.h"

#include <vector>

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

  // GetStringUTFChars() returns "modified UTF-8" (characters outside
  // the BMP as two 3-byte surrogates), so we convert from UTF-16.
  const jsize len = env->GetStringLength(str);
  const jchar* chars = env->GetStringChars(str, nullptr);
  if (chars) {
    std::wstring wide;
    wide.reserve(len);
    for (jsize i = 0; i < len; ++i) {
      const jchar c = chars[i];
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && chars[i + 1] >= 0xDC00 &&
          chars[i + 1] <= 0xDFFF) {
        wide.push_back(wchar_t(base::utf16_to_codepoint(chars[i + 1], c)));
        ++i;
      }
      else
        wide.push_back(wchar_t(c));
    }
    env->ReleaseStringChars(str, chars);
    result = base::to_utf8(wide);
  }
  jni_exception_check(env, "GetStringChars");
  return result;
}

jstring jni_new_string(JNIEnv* env, const std::string& str)
{
  if (!env)
    return nullptr;

  // NewStringUTF() expects "modified UTF-8" and CheckJNI aborts on
  // 4-byte sequences (e.g. an emoji in a file name), so we create the
  // string from UTF-16.
  std::vector<jchar> utf16;
  utf16.reserve(str.size());
  for (const wchar_t chr : base::from_utf8(str)) {
    const uint32_t cp = uint32_t(chr);
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      utf16.push_back(jchar(0xD800 + ((cp - 0x10000) >> 10)));
      utf16.push_back(jchar(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    }
    else
      utf16.push_back(jchar(cp));
  }

  jstring result = env->NewString(utf16.data(), jsize(utf16.size()));
  if (jni_exception_check(env, "NewString"))
    return nullptr;
  return result;
}

JniThreadEnv::JniThreadEnv(JavaVM* vm) : m_vm(vm)
{
  if (!m_vm)
    return;

  void* env = nullptr;
  const jint res = m_vm->GetEnv(&env, JNI_VERSION_1_6);
  if (res == JNI_OK) {
    m_env = (JNIEnv*)env;
  }
  else if (res == JNI_EDETACHED) {
    if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK)
      m_attached = true;
    else {
      LAF_ALOGE("JNI: AttachCurrentThread failed");
      m_env = nullptr;
    }
  }
}

JniThreadEnv::~JniThreadEnv()
{
  if (m_attached)
    m_vm->DetachCurrentThread();
}

} // namespace os
