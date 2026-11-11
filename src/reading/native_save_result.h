#pragma once
#include "reading_progress.h"
#include <utility>

namespace kobalt {
// Persistence evidence, independent of the caller's retry and lifecycle policy.
// Only Saved and ActivityOnly carry a verified native baseline. Unverified
// never promises that the operation left native storage unchanged.
class NativeSaveResult
{
public:
    enum class Status {
        NotAttempted,
        Saved,
        ActivityOnly,
        LocalOnly,
        Conflict,
        BusyBeforeWrite,
        ProgressChanged,
        Unavailable,
        Deferred,
        Rejected,
        Unverified
    };
    NativeSaveResult() = default;

    static NativeSaveResult verified(ReadingProgress baseline, bool activityOnly = false)
    {
        NativeSaveResult result(activityOnly ? Status::ActivityOnly : Status::Saved, {});
        result.baseline_ = std::move(baseline);
        return result;
    }

    static NativeSaveResult localOnly(QString message = {})
    {
        return NativeSaveResult(Status::LocalOnly, std::move(message));
    }

    static NativeSaveResult conflict(QString message = {})
    {
        return NativeSaveResult(Status::Conflict, std::move(message));
    }

    static NativeSaveResult busyBeforeWrite(QString message = {})
    {
        return NativeSaveResult(Status::BusyBeforeWrite, std::move(message));
    }

    static NativeSaveResult progressChanged(QString message = {})
    {
        return NativeSaveResult(Status::ProgressChanged, std::move(message));
    }

    static NativeSaveResult unavailable(QString message = {})
    {
        return NativeSaveResult(Status::Unavailable, std::move(message));
    }

    static NativeSaveResult deferred(QString message = {})
    {
        return NativeSaveResult(Status::Deferred, std::move(message));
    }

    static NativeSaveResult rejected(QString message = {})
    {
        return NativeSaveResult(Status::Rejected, std::move(message));
    }

    static NativeSaveResult unverified(QString message = {})
    {
        return NativeSaveResult(Status::Unverified, std::move(message));
    }

    Status status() const { return status_; }

    bool saved() const { return status_ == Status::Saved || status_ == Status::ActivityOnly; }

    bool failed() const
    {
        return !saved() && status_ != Status::LocalOnly && status_ != Status::Conflict;
    }

    const ReadingProgress &baseline() const { return baseline_; }

    const QString &message() const { return message_; }

    // Preserve the existing checkpoint and diagnostic vocabulary.
    const char *diagnosticName() const
    {
        switch (status_) {
        case Status::NotAttempted:
            return "not-attempted";
        case Status::Saved:
            return "verified";
        case Status::ActivityOnly:
            return "time-only";
        case Status::LocalOnly:
            return "local-only";
        default:
            return "failed";
        }
    }

private:
    NativeSaveResult(Status status, QString message) : status_(status), message_(std::move(message))
    {
    }

    Status status_ = Status::NotAttempted;
    ReadingProgress baseline_;
    QString message_;
};
} // namespace kobalt
