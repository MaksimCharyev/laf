// LAF OS Library
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "os/android/documents.h"

#include "base/fs.h"
#include "base/time.h"
#include "os/android/app.h"
#include "os/android/jni.h"

#include <android/looper.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace os {

namespace {

// Picker types, same values as AsepriteActivity.PICKER_*.
enum { kPickerOpen = 0, kPickerOpenMultiple = 1, kPickerSave = 2 };

// Result codes of AsepriteActivity.nativeOnDocumentsPicked().
enum { kPickError = -1, kPickCancel = 0, kPickOK = 1 };

// Maximum number of picked documents remembered (and persistable URI
// grants kept, the system limit is 128 before API 30 and 512 after).
constexpr size_t kMaxEntries = 64;

// After the picker returns, time to wait for APP_CMD_RESUME and
// APP_CMD_INIT_WINDOW before giving control back to the app.
constexpr base::tick_t kWaitResumeMsecs = 1500;

const char* kRegistryFileName = "saf-documents.txt";

std::unique_ptr<DocumentsAndroid> g_documents;

// 64-bit FNV-1a, stable between runs (std::hash is not guaranteed to
// be), so the same URI is always copied to the same path.
uint64_t fnv1a(const std::string& s)
{
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

std::string sanitize_file_name(const std::string& name)
{
  std::string result;
  result.reserve(name.size());
  for (char c : name) {
    if (c == '/' || c == '\0')
      result.push_back('_');
    else
      result.push_back(c);
  }
  if (result.empty() || result == "." || result == "..")
    result = "document";
  return result;
}

const char* picker_name(const AndroidDocumentPicker type)
{
  switch (type) {
    case AndroidDocumentPicker::Open:         return "open";
    case AndroidDocumentPicker::OpenMultiple: return "open-multiple";
    case AndroidDocumentPicker::Save:         return "save";
  }
  return "?";
}

void JNICALL native_on_documents_picked(JNIEnv* env,
                                        jobject,
                                        jint requestId,
                                        jint result,
                                        jobjectArray uris,
                                        jobjectArray names)
{
  if (DocumentsAndroid* docs = DocumentsAndroid::instance())
    docs->onPicked(env, requestId, result, uris, names);
}

std::vector<std::string> string_array(JNIEnv* env, jobjectArray array)
{
  std::vector<std::string> result;
  if (!array)
    return result;

  const jsize n = env->GetArrayLength(array);
  if (jni_exception_check(env, "GetArrayLength"))
    return result;
  for (jsize i = 0; i < n; ++i) {
    auto str = (jstring)env->GetObjectArrayElement(array, i);
    if (jni_exception_check(env, "GetObjectArrayElement"))
      break;
    result.push_back(jni_to_string(env, str));
    if (str)
      env->DeleteLocalRef(str);
  }
  return result;
}

} // anonymous namespace

// static
DocumentsAndroid* DocumentsAndroid::instance()
{
  // Created by the glue thread on the first use (pick() or the app
  // hooks). It lives until the process ends.
  static std::once_flag once;
  std::call_once(once, [] { g_documents.reset(new DocumentsAndroid); });
  return g_documents.get();
}

DocumentsAndroid::DocumentsAndroid()
{
}

int DocumentsAndroid::pick(const AndroidDocumentPicker type,
                           const std::string& initialPath,
                           const std::string& defaultExtension,
                           base::paths& output)
{
  AppAndroid* app = AppAndroid::instance();
  if (!app || !app->jniEnv())
    return kPickError;

  JNIEnv* env = app->jniEnv();
  if (!initJni(env)) {
    LAF_ALOGE("documents: JNI not available, using the app file selector");
    return kPickError;
  }

  // Suggested name and initial folder.
  int requestId;
  std::string suggestedName;
  std::string initialUri;
  {
    const std::lock_guard lock(m_mutex);
    loadRegistry();
    if (!initialPath.empty()) {
      if (Entry* entry = findByPath(initialPath))
        initialUri = entry->uri;
    }
    suggestedName = base::get_file_name(initialPath);
    if (type == AndroidDocumentPicker::Save) {
      if (suggestedName.empty())
        suggestedName = "Untitled";
      if (base::get_file_extension(suggestedName).empty() && !defaultExtension.empty())
        suggestedName = base::replace_extension(suggestedName, defaultExtension);
    }

    requestId = ++m_requestId;
    m_done = false;
    m_result = kPickCancel;
    m_uris.clear();
    m_names.clear();
  }

  // We always ask for application/octet-stream when creating a
  // document: providers keep the typed name as it is with that type,
  // while e.g. image/png turns "foo.gif" into "foo.gif.png" (ADR 0009).
  const int pickerType = (type == AndroidDocumentPicker::Open         ? kPickerOpen :
                          type == AndroidDocumentPicker::OpenMultiple ? kPickerOpenMultiple :
                                                                        kPickerSave);
  LAF_ALOGI("documents: request %d: showing the %s picker (name=\"%s\" initial uri=%s)",
            requestId,
            picker_name(type),
            suggestedName.c_str(),
            initialUri.empty() ? "-" : initialUri.c_str());

  jstring jmime = jni_new_string(env, "application/octet-stream");
  jstring jname = jni_new_string(env, suggestedName);
  jstring juri = (initialUri.empty() ? nullptr : jni_new_string(env, initialUri));
  env->CallVoidMethod(m_activity,
                      m_startPicker,
                      jint(requestId),
                      jint(pickerType),
                      jmime,
                      jname,
                      juri);
  const bool failed = jni_exception_check(env, "AsepriteActivity.startDocumentPicker()");
  if (jmime)
    env->DeleteLocalRef(jmime);
  if (jname)
    env->DeleteLocalRef(jname);
  if (juri)
    env->DeleteLocalRef(juri);
  if (failed)
    return kPickError;

  // Modal wait: process lifecycle commands (PAUSE, TERM_WINDOW, ...)
  // and input until the result arrives. onPicked() wakes up the
  // looper.
  const base::tick_t start = base::current_tick();
  while (true) {
    {
      const std::lock_guard lock(m_mutex);
      if (m_done)
        break;
    }
    if (app->isDestroyRequested()) {
      LAF_ALOGW("documents: request %d: activity destroyed while the picker was open", requestId);
      const std::lock_guard lock(m_mutex);
      m_result = kPickCancel;
      m_done = true;
      break;
    }
    app->pollLooper(-1);
  }

  int result;
  std::vector<std::string> uris, names;
  {
    const std::lock_guard lock(m_mutex);
    result = m_result;
    uris = std::move(m_uris);
    names = std::move(m_names);
    // Results arriving later for this request are ignored.
    ++m_requestId;
  }
  LAF_ALOGI("documents: request %d: result %d with %d document(s) after %d ms",
            requestId,
            result,
            int(uris.size()),
            int(base::current_tick() - start));

  if (result == kPickOK)
    waitForResumedWindow();
  if (result != kPickOK || uris.empty())
    return (result == kPickError ? kPickError : kPickCancel);

  for (size_t i = 0; i < uris.size(); ++i) {
    std::string uri = uris[i];
    std::string name = sanitize_file_name(i < names.size() ? names[i] : std::string());

    if (type == AndroidDocumentPicker::Save) {
      // The format is chosen by the extension, so a document created
      // without one is renamed (best effort) and the path always has
      // it.
      if (base::get_file_extension(name).empty() && !defaultExtension.empty()) {
        name = base::replace_extension(name, defaultExtension);
        const std::string newUri = renameDocument(env, uri, name);
        if (!newUri.empty()) {
          LAF_ALOGI("documents: renamed to \"%s\" (%s)", name.c_str(), newUri.c_str());
          uri = newUri;
        }
      }
    }

    const std::string path = pathForUri(uri, name);
    try {
      base::make_all_directories(base::get_file_path(path));
    }
    catch (const std::exception& ex) {
      LAF_ALOGE("documents: cannot create the folder of %s: %s", path.c_str(), ex.what());
      continue;
    }

    // When saving, the app writes the file and then calls
    // toProvider(); when opening, the document is copied now.
    if (type != AndroidDocumentPicker::Save) {
      const std::string error = copyUriToFile(env, uri, path);
      if (!error.empty()) {
        LAF_ALOGE("documents: cannot read %s: %s", uri.c_str(), error.c_str());
        continue;
      }
    }

    {
      const std::lock_guard lock(m_mutex);
      Entry& entry = addEntry(uri, path);
      entry.synced = true;
      saveRegistry();
    }
    LAF_ALOGI("documents: %s -> %s", uri.c_str(), path.c_str());
    output.push_back(path);
  }

  return (output.empty() ? kPickCancel : kPickOK);
}

bool DocumentsAndroid::toProvider(const std::string& path, std::string& error)
{
  std::string uri;
  {
    const std::lock_guard lock(m_mutex);
    loadRegistry();
    Entry* entry = findByPath(path);
    if (!entry)
      return true;
    uri = entry->uri;
  }

  AppAndroid* app = AppAndroid::instance();
  if (!app) {
    error = "The document cannot be written (no activity)";
    return false;
  }

  // This is the save worker thread of the app, usually.
  JniThreadEnv jni(app->app()->activity->vm);
  if (!jni.env() || !initJni(jni.env())) {
    error = "The document cannot be written (JNI is not available)";
    return false;
  }

  const base::tick_t start = base::current_tick();
  error = copyFileToUri(jni.env(), path, uri);
  if (!error.empty()) {
    LAF_ALOGE("documents: cannot write %s to %s: %s", path.c_str(), uri.c_str(), error.c_str());
    error = "Cannot write \"" + base::get_file_name(path) + "\" to the selected location (" + uri +
            "): " + error;
    return false;
  }

  LAF_ALOGI("documents: saved %s -> %s in %d ms",
            path.c_str(),
            uri.c_str(),
            int(base::current_tick() - start));

  const std::lock_guard lock(m_mutex);
  if (Entry* found = findByPath(path)) {
    // Most recently used first (with the registered form of the path).
    const std::string registeredPath = found->path;
    Entry& entry = addEntry(uri, registeredPath);
    entry.synced = true;
    saveRegistry();
  }
  return true;
}

void DocumentsAndroid::fromProvider(const std::string& path)
{
  std::string uri;
  {
    const std::lock_guard lock(m_mutex);
    loadRegistry();
    Entry* entry = findByPath(path);
    if (!entry || entry->synced)
      return;
    uri = entry->uri;
  }

  AppAndroid* app = AppAndroid::instance();
  if (!app)
    return;
  JniThreadEnv jni(app->app()->activity->vm);
  if (!jni.env() || !initJni(jni.env()))
    return;

  try {
    base::make_all_directories(base::get_file_path(path));
  }
  catch (const std::exception& ex) {
    LAF_ALOGE("documents: cannot create the folder of %s: %s", path.c_str(), ex.what());
    return;
  }

  const std::string error = copyUriToFile(jni.env(), uri, path);
  if (!error.empty()) {
    LAF_ALOGW("documents: cannot refresh %s from %s (%s), %s",
              path.c_str(),
              uri.c_str(),
              error.c_str(),
              base::is_file(path) ? "using the old copy" : "there is no copy");
    return;
  }
  LAF_ALOGI("documents: refreshed %s <- %s", path.c_str(), uri.c_str());

  const std::lock_guard lock(m_mutex);
  if (Entry* entry = findByPath(path))
    entry->synced = true;
}

void DocumentsAndroid::onPicked(JNIEnv* env,
                                const int requestId,
                                const int result,
                                jobjectArray uris,
                                jobjectArray names)
{
  std::vector<std::string> uriList = string_array(env, uris);
  std::vector<std::string> nameList = string_array(env, names);
  {
    const std::lock_guard lock(m_mutex);
    if (requestId != m_requestId || m_done) {
      LAF_ALOGW("documents: result of request %d ignored (waiting %d)", requestId, m_requestId);
      return;
    }
    m_result = result;
    m_uris = std::move(uriList);
    m_names = std::move(nameList);
    m_done = true;
  }

  // Wake up the glue thread (waiting in pick()).
  if (AppAndroid* app = AppAndroid::instance())
    ALooper_wake(app->app()->looper);
}

bool DocumentsAndroid::initJni(JNIEnv* env)
{
  // It can be called from the glue thread (pick(), fromProvider()) or
  // from a worker thread (toProvider()), the ids are valid in any
  // thread.
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

  bool ok = true;
  auto method = [env, cls, &ok](const char* name, const char* sig) -> jmethodID {
    jmethodID id = env->GetMethodID(cls, name, sig);
    if (jni_exception_check(env, name) || !id) {
      ok = false;
      return nullptr;
    }
    return id;
  };
  const char* kStr2Str = "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;";
  m_startPicker = method("startDocumentPicker",
                         "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  m_copyToFile = method("copyDocumentToFile", kStr2Str);
  m_copyToUri = method("copyFileToDocument", kStr2Str);
  m_rename = method("renameDocument", kStr2Str);
  m_release = method("releaseDocumentPermission", "(Ljava/lang/String;)V");

  if (ok) {
    const JNINativeMethod natives[] = {
      { "nativeOnDocumentsPicked",
       "(II[Ljava/lang/String;[Ljava/lang/String;)V", (void*)&native_on_documents_picked },
    };
    const jint res = env->RegisterNatives(cls, natives, 1);
    if (jni_exception_check(env, "RegisterNatives(nativeOnDocumentsPicked)") || res != JNI_OK)
      ok = false;
  }
  env->DeleteLocalRef(cls);

  if (!ok)
    return false;

  m_activity = activity;
  m_jniReady = true;
  return true;
}

void DocumentsAndroid::loadRegistry()
{
  if (m_registryLoaded)
    return;
  m_registryLoaded = true;

  AppAndroid* app = AppAndroid::instance();
  if (!app || !app->app()->activity->internalDataPath)
    return;

  std::ifstream f(base::join_path(app->app()->activity->internalDataPath, kRegistryFileName));
  std::string line;
  while (std::getline(f, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
      continue;
    Entry entry;
    entry.uri = line.substr(0, tab);
    entry.path = line.substr(tab + 1);
    m_entries.push_back(entry);
    if (m_entries.size() >= kMaxEntries)
      break;
  }
  LAF_ALOGV("documents: %d picked document(s) in the registry", int(m_entries.size()));
}

void DocumentsAndroid::saveRegistry()
{
  AppAndroid* app = AppAndroid::instance();
  if (!app || !app->app()->activity->internalDataPath)
    return;

  // Written at once (the process can be killed at any time, ADR
  // 0007), through a temporary file so it's never half written.
  const std::string fn = base::join_path(app->app()->activity->internalDataPath, kRegistryFileName);
  const std::string tmp = fn + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    for (const Entry& entry : m_entries)
      f << entry.uri << '\t' << entry.path << '\n';
    if (!f) {
      LAF_ALOGE("documents: cannot write %s", tmp.c_str());
      return;
    }
  }
  if (std::rename(tmp.c_str(), fn.c_str()) != 0)
    LAF_ALOGE("documents: cannot rename %s", tmp.c_str());
}

std::string DocumentsAndroid::pathForUri(const std::string& uri,
                                         const std::string& displayName) const
{
  std::string base;
  if (const char* tmp = std::getenv("TMPDIR"))
    base = tmp;
  if (base.empty()) {
    if (AppAndroid* app = AppAndroid::instance())
      base = base::get_file_path(app->app()->activity->internalDataPath);
    base = base::join_path(base, "cache");
  }

  char hash[17];
  std::snprintf(hash, sizeof(hash), "%016" PRIx64, fnv1a(uri));
  return base::join_path(base::join_path(base::join_path(base, "documents"), hash), displayName);
}

DocumentsAndroid::Entry& DocumentsAndroid::addEntry(const std::string& uri, const std::string& path)
{
  // Keep the "synced" flag of the existing entry.
  bool synced = false;
  auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) {
    return e.uri == uri || e.path == path;
  });
  while (it != m_entries.end()) {
    synced |= (it->uri == uri && it->path == path && it->synced);
    m_entries.erase(it);
    it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) {
      return e.uri == uri || e.path == path;
    });
  }

  Entry entry;
  entry.uri = uri;
  entry.path = path;
  entry.synced = synced;
  m_entries.insert(m_entries.begin(), entry);

  // Forget the least recently used documents (and their URI grants).
  while (m_entries.size() > kMaxEntries) {
    const std::string oldUri = m_entries.back().uri;
    m_entries.pop_back();
    if (AppAndroid* app = AppAndroid::instance()) {
      JniThreadEnv jni(app->app()->activity->vm);
      if (jni.env())
        releasePermission(jni.env(), oldUri);
    }
  }
  return m_entries.front();
}

DocumentsAndroid::Entry* DocumentsAndroid::findByPath(const std::string& path)
{
  for (Entry& entry : m_entries) {
    if (entry.path == path)
      return &entry;
  }
  // The app can give us the same path in other form, e.g. the Export
  // window joins the sprite folder with a relative path
  // ("<cache>/documents/A/../B/name.png").
  const std::string normalized = base::normalize_path(path);
  if (normalized != path) {
    for (Entry& entry : m_entries) {
      if (entry.path == normalized)
        return &entry;
    }
  }
  return nullptr;
}

std::string DocumentsAndroid::copyUriToFile(JNIEnv* env,
                                            const std::string& uri,
                                            const std::string& path)
{
  // Java returns null on success or the error message.
  return callStringMethod(env, m_copyToFile, "AsepriteActivity.copyDocumentToFile()", uri, path);
}

std::string DocumentsAndroid::copyFileToUri(JNIEnv* env,
                                            const std::string& path,
                                            const std::string& uri)
{
  return callStringMethod(env, m_copyToUri, "AsepriteActivity.copyFileToDocument()", path, uri);
}

std::string DocumentsAndroid::renameDocument(JNIEnv* env,
                                             const std::string& uri,
                                             const std::string& name)
{
  // Java returns the new URI or null.
  std::string result =
    callStringMethod(env, m_rename, "AsepriteActivity.renameDocument()", uri, name);
  if (result.compare(0, 10, "content://") != 0)
    result.clear();
  return result;
}

void DocumentsAndroid::releasePermission(JNIEnv* env, const std::string& uri)
{
  if (!m_jniReady)
    return;
  jstring juri = jni_new_string(env, uri);
  if (!juri)
    return;
  env->CallVoidMethod(m_activity, m_release, juri);
  jni_exception_check(env, "AsepriteActivity.releaseDocumentPermission()");
  env->DeleteLocalRef(juri);
}

std::string DocumentsAndroid::callStringMethod(JNIEnv* env,
                                               jmethodID method,
                                               const char* what,
                                               const std::string& a,
                                               const std::string& b)
{
  if (!m_jniReady || !method)
    return "(no JNI)";

  jstring ja = jni_new_string(env, a);
  jstring jb = jni_new_string(env, b);
  std::string result;
  if (ja && jb) {
    auto jresult = (jstring)env->CallObjectMethod(m_activity, method, ja, jb);
    if (jni_exception_check(env, what))
      result = "(exception)";
    else if (jresult) {
      result = jni_to_string(env, jresult);
      env->DeleteLocalRef(jresult);
    }
  }
  else
    result = "(cannot create Java strings)";
  if (ja)
    env->DeleteLocalRef(ja);
  if (jb)
    env->DeleteLocalRef(jb);
  return result;
}

void DocumentsAndroid::waitForResumedWindow()
{
  // onActivityResult() is called before onResume(), and the surface
  // comes after that: wait a bit so the app paints the document on a
  // live window. Any command/input wakes up the loop.
  AppAndroid* app = AppAndroid::instance();
  const base::tick_t deadline = base::current_tick() + kWaitResumeMsecs;
  while (!app->isDestroyRequested() && !(app->isResumed() && app->app()->window)) {
    const base::tick_t now = base::current_tick();
    if (now >= deadline) {
      LAF_ALOGW("documents: the activity was not resumed %d ms after the picker",
                int(kWaitResumeMsecs));
      break;
    }
    app->pollLooper(int(deadline - now));
  }
}

// Public API (os/android/android_app.h)

int android_pick_documents(const AndroidDocumentPicker type,
                           const std::string& initialPath,
                           const std::string& defaultExtension,
                           base::paths& output)
{
  return DocumentsAndroid::instance()->pick(type, initialPath, defaultExtension, output);
}

bool android_document_to_provider(const std::string& path, std::string& error)
{
  return DocumentsAndroid::instance()->toProvider(path, error);
}

void android_document_from_provider(const std::string& path)
{
  DocumentsAndroid::instance()->fromProvider(path);
}

} // namespace os
