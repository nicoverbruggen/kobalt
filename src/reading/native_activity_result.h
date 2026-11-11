#pragma once

namespace kobalt {
// Completion of the host's activity obligation. NotRequired is an explicit
// adapter decision, not evidence that native storage was written.
enum class NativeActivityResult { NotRequired, Completed, Failed, Abandoned };

inline bool activityCompleted(NativeActivityResult result)
{
    return result == NativeActivityResult::NotRequired || result == NativeActivityResult::Completed;
}
} // namespace kobalt
