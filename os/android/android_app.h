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

#include "base/paths.h"
#include "gfx/border.h"

#include <string>

struct android_app;

namespace os {

// Returns the android_app received in android_main() (it lives in
// laf's os/android/main.cpp), or nullptr outside android_main().
::android_app* android_app();

// Screen density in dpi (AConfiguration_getDensity(), e.g. 400 on a
// 2.5x screen), refreshed on each APP_CMD_CONFIG_CHANGED.  Returns 0
// if it's unknown.
int android_screen_density();

// Insets in pixels from each edge of the window where the app should
// not place UI: the part of the rounded display corners outside a
// rectangle (per corner side, ceil(radius * (1 - 1/sqrt(2)))), the
// display cutout and waterfall insets, whichever is bigger. Updated
// before the window resize event when the window is resized or
// rotated, or when the insets change (a resize event is sent then).
gfx::Border android_safe_area_insets();

// Storage Access Framework documents (ADR 0009). The system picker
// returns content:// URIs; each one is mapped to a file with the same
// display name in the app cache, and the app works with that path.

enum class AndroidDocumentPicker {
  Open,         // ACTION_OPEN_DOCUMENT
  OpenMultiple, // ACTION_OPEN_DOCUMENT with EXTRA_ALLOW_MULTIPLE
  Save,         // ACTION_CREATE_DOCUMENT
};

// Shows the system picker and waits for it (modal, from the thread
// that runs app_main(); lifecycle commands are processed meanwhile).
// "initialPath" is the current file name (its name is suggested when
// saving, its folder is opened if it's a picked document) and
// "defaultExtension" is added to a saved file without extension.
// Opened documents are copied to their paths before returning.
// Returns 1 if "output" has the paths, 0 if the user cancelled, or -1
// if the picker cannot be shown.
int android_pick_documents(AndroidDocumentPicker type,
                           const std::string& initialPath,
                           const std::string& defaultExtension,
                           base::paths& output);

// Must be called after the app writes a file: if "path" is mapped to
// a picked document, the file is copied to it. Returns false (with
// "error" set) if the copy fails; true if it was copied or the path
// is not a picked document. Can be called from any thread.
bool android_document_to_provider(const std::string& path, std::string& error);

// Should be called before the app reads a file: if "path" is mapped
// to a picked document that was not copied in this process yet (e.g.
// it comes from the recent files list), it's copied again from the
// document. Errors are logged (the old copy is kept). Can be called
// from any thread.
void android_document_from_provider(const std::string& path);

// Public "Download" folder (MediaStore.Downloads, ADR 0010). The
// app writes a file inside android_downloads_cache_path() (a folder
// in the app cache, e.g. "<cache>/downloads/Aseprite/foo.png") and
// then calls android_publish_download() with that path: the file is
// copied to "Download/<subfolder>/<name>" (e.g.
// "Download/Aseprite/foo.png") as an entry owned by the app (no
// permission needed), replacing the app's own entry with the same
// name, and the cache file is deleted.
std::string android_downloads_cache_path();

// Returns false (with "error" set) if publishing fails; true if the
// file was published or "path" is not inside
// android_downloads_cache_path(). Can be called from any thread.
bool android_publish_download(const std::string& path, std::string& error);

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
