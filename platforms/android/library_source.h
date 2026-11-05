#pragma once
#include "library/scanner.h"

namespace kobalt::android {
// Reads granted document trees through the activity. Folder and book keys
// are content URIs; files are read in place through file descriptors.
class LibrarySource final : public library::Source
{
public:
    bool list(const library::Folder &folder, QVector<library::Entry> &files,
              const std::atomic<bool> &cancelled) override;
    std::unique_ptr<QIODevice> open(const library::Entry &entry) override;
};
} // namespace kobalt::android
