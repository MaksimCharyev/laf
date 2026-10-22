// laf-dlgs
// Copyright (C) 2026  Maksim Charyev
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "dlgs/file_dialog.h"

#include "os/android/android_app.h"

namespace dlgs {

// System document picker (Storage Access Framework). The picked
// content:// documents are given to the app as files in its cache,
// see os/android/documents.cpp and ADR 0009.
class FileDialogAndroid : public FileDialog {
public:
  FileDialogAndroid(const Spec&) {}

  std::string fileName() override { return m_filename; }

  void getMultipleFileNames(base::paths& output) override { output = m_filenames; }

  void setFileName(const std::string& filename) override { m_filename = filename; }

  Result show(void*) override
  {
    os::AndroidDocumentPicker type;
    switch (m_type) {
      case Type::OpenFile:  type = os::AndroidDocumentPicker::Open; break;
      case Type::OpenFiles: type = os::AndroidDocumentPicker::OpenMultiple; break;
      case Type::SaveFile:  type = os::AndroidDocumentPicker::Save; break;
      default:
        // A folder picked with ACTION_OPEN_DOCUMENT_TREE has no path,
        // the app uses its own file selector.
        return Result::Error;
    }

    base::paths output;
    const int res = os::android_pick_documents(type, m_filename, m_defExtension, output);
    if (res < 0)
      return Result::Error;
    if (res == 0 || output.empty())
      return Result::Cancel;

    m_filenames = output;
    m_filename = output.front();
    return Result::OK;
  }

private:
  std::string m_filename;
  base::paths m_filenames;
};

FileDialogRef FileDialog::makeAndroid(const Spec& spec)
{
  return base::make_ref<FileDialogAndroid>(spec);
}

} // namespace dlgs
