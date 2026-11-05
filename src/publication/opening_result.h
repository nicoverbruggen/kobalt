#pragma once
#include <QString>
#include <QByteArray>
#include <utility>

namespace kobalt {
enum class OpeningFailureKind {
    Cancelled,
    Protected,
    Unsupported,
    InvalidContent,
    Io,
    Revoked,
    HostUnavailable
};
enum class ContentDisposition { Retry, RememberRejection };

struct OpeningFailure
{
    OpeningFailureKind kind;
    // Stable identifiers are assigned at the failing operation, never inferred
    // from translated text. Detail temporarily carries legacy parser/resource
    // wording; it is for presentation only, not routing or diagnostics.
    QByteArray reason;
    QString detail;
    ContentDisposition content = ContentDisposition::Retry;
};

class OpeningResult
{
public:
    OpeningResult() = default;

    static OpeningResult failed(OpeningFailureKind kind, const char *reason, QString detail = {},
                                ContentDisposition content = ContentDisposition::Retry)
    {
        OpeningResult result;
        result.failure_ = OpeningFailure{ kind, QByteArray(reason), std::move(detail), content };
        result.failed_ = true;
        return result;
    }

    bool ready() const { return !failed_; }

    const OpeningFailure *failure() const { return failed_ ? &failure_ : nullptr; }

    bool rememberRejection() const
    {
        return failed_ && failure_.content == ContentDisposition::RememberRejection;
    }

    void rememberContentRejection()
    {
        if (!failed_)
            return;
        failure_.content = ContentDisposition::RememberRejection;
        // The transport reports completed XML/content validation separately
        // from an interrupted read. Only that evidence changes an I/O failure.
        if (failure_.kind == OpeningFailureKind::Io)
            failure_.kind = OpeningFailureKind::InvalidContent;
    }

private:
    // Qt5's GCC 4.9 standard library has no optional. The private tag keeps
    // inactive storage inaccessible without allocating a separate failure.
    bool failed_ = false;
    OpeningFailure failure_{ OpeningFailureKind::Cancelled, {}, {}, ContentDisposition::Retry };
};
} // namespace kobalt
