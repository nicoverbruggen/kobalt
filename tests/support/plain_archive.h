#pragma once
#include "publication/native_archive.h"
#include <QString>
#include <memory>

namespace kobalt::native {
// A ResourceArchive over an unencrypted EPUB. Checks use it in place of
// Nickel's archive to exercise the native resource path. Empty when the file
// cannot be opened.
std::unique_ptr<ResourceArchive> openPlainArchive(const QString &path);
} // namespace kobalt::native
