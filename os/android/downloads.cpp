// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/downloads.h"

#include "base/fs.h"
#include "base/split_string.h"
#include "base/time.h"
#include "os/android/app.h"
#include "os/android/jni.h"

#include <cstdlib>
#include <memory>
#include <vector>

namespace os {

namespace {

// Same value as Environment.DIRECTORY_DOWNLOADS.
const char* kDownloadDir = "Download";

std::unique_ptr<DownloadsAndroid> g_downloads;

} // anonymous namespace

// static
DownloadsAndroid* DownloadsAndroid::instance()
{
  static std::once_flag once;
  std::call_once(once, [] { g_downloads.reset(new DownloadsAndroid); });
  return g_downloads.get();
}

DownloadsAndroid::DownloadsAndroid()
{
}

std::string DownloadsAndroid::cachePath() const
{
  // Same base folder as the picked documents (documents.cpp): TMPDIR
  // is the app cache dir (set by android_main()).
  std::string base;
  if (const char* tmp = std::getenv("TMPDIR"))
    base = tmp;
  if (base.empty()) {
    if (AppAndroid* app = AppAndroid::instance())
      base = base::get_file_path(app->app()->activity->internalDataPath);
    base = base::join_path(base, "cache");
  }
  return base::join_path(base, "downloads");
}

bool DownloadsAndroid::publish(const std::string& path, std::string& error)
{
  // Only files inside the cache folder are published, "path" can be
  // written in any form (e.g. with ".." from a relative path).
  const std::string root = base::normalize_path(cachePath());
  const std::string fn = base::normalize_path(path);
  if (root.empty() || fn.size() <= root.size() + 1 || fn.compare(0, root.size(), root) != 0 ||
      fn[root.size()] != '/') {
    return true;
  }

  const std::string rel = fn.substr(root.size() + 1);
  const std::string name = base::get_file_name(rel);
  std::string relativePath = kDownloadDir;
  relativePath.push_back('/');
  if (!base::get_file_path(rel).empty()) {
    relativePath += base::get_file_path(rel);
    relativePath.push_back('/');
  }

  if (!base::is_file(fn)) {
    error = "The file \"" + fn + "\" doesn't exist";
    return false;
  }

  AppAndroid* app = AppAndroid::instance();
  if (!app) {
    error = "The file cannot be published to " + relativePath + " (no activity)";
    return false;
  }

  // This is the save worker thread of the app, usually.
  JniThreadEnv jni(app->app()->activity->vm);
  JNIEnv* env = jni.env();
  if (!env || !initJni(env)) {
    error = "The file cannot be published to " + relativePath + " (JNI is not available)";
    return false;
  }

  const base::tick_t start = base::current_tick();
  std::string result;
  jstring jpath = jni_new_string(env, fn);
  jstring jrel = jni_new_string(env, relativePath);
  jstring jname = jni_new_string(env, name);
  if (jpath && jrel && jname) {
    auto jresult = (jstring)env->CallObjectMethod(m_activity, m_publish, jpath, jrel, jname);
    if (jni_exception_check(env, "AsepriteActivity.publishDownload()"))
      result = "error\t(exception)";
    else if (jresult) {
      result = jni_to_string(env, jresult);
      env->DeleteLocalRef(jresult);
    }
  }
  else
    result = "error\t(cannot create Java strings)";
  if (jpath)
    env->DeleteLocalRef(jpath);
  if (jrel)
    env->DeleteLocalRef(jrel);
  if (jname)
    env->DeleteLocalRef(jname);

  // Java returns "ok<TAB>uri<TAB>display name<TAB>new|replaced" or
  // "error<TAB>message".
  std::vector<std::string> parts;
  base::split_string(result, parts, "\t");
  if (parts.size() < 4 || parts[0] != "ok") {
    const std::string msg = (parts.size() >= 2 ? parts[1] : result);
    LAF_ALOGE("downloads: cannot publish %s to %s%s: %s",
              fn.c_str(),
              relativePath.c_str(),
              name.c_str(),
              msg.c_str());
    error = "Cannot save \"" + name + "\" to " + relativePath + ": " + msg;
    return false;
  }

  LAF_ALOGI("downloads: published %s -> %s%s (%s, %s) in %d ms",
            fn.c_str(),
            relativePath.c_str(),
            parts[2].c_str(),
            parts[3].c_str(),
            parts[1].c_str(),
            int(base::current_tick() - start));

  // The public copy is the only one we need.
  try {
    base::delete_file(fn);
  }
  catch (const std::exception& ex) {
    LAF_ALOGW("downloads: cannot delete %s: %s", fn.c_str(), ex.what());
  }
  return true;
}

bool DownloadsAndroid::initJni(JNIEnv* env)
{
  const std::lock_guard lock(m_jniMutex);
  if (m_jniReady)
    return true;

  AppAndroid* app = AppAndroid::instance();
  if (!app || !env)
    return false;

  // javaGameActivity is a global reference owned by GameActivity.
  jobject activity = app->app()->activity->javaGameActivity;
  jclass cls = env->GetObjectClass(activity);
  if (jni_exception_check(env, "GetObjectClass(activity)") || !cls)
    return false;

  jmethodID publish = env->GetMethodID(
    cls,
    "publishDownload",
    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  const bool failed = (jni_exception_check(env, "publishDownload") || !publish);
  env->DeleteLocalRef(cls);
  if (failed)
    return false;

  m_activity = activity;
  m_publish = publish;
  m_jniReady = true;
  return true;
}

// Public API (os/android/android_app.h)

std::string android_downloads_cache_path()
{
  return DownloadsAndroid::instance()->cachePath();
}

bool android_publish_download(const std::string& path, std::string& error)
{
  return DownloadsAndroid::instance()->publish(path, error);
}

} // namespace os
