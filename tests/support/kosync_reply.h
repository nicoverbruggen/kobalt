#pragma once
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <cstring>
#include <utility>

namespace kobalt::test {
// A KOSync HTTP answer without a server. It finishes on the next event loop
// pass, or, when `held`, only when finish() is called, so a test can keep a
// request in flight.
class KosyncReply final : public QNetworkReply
{
public:
    KosyncReply(const QNetworkRequest &request, QByteArray bytes, int status,
                QNetworkReply::NetworkError error, QObject *parent, bool held = false)
        : QNetworkReply(parent), bytes_(std::move(bytes))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute,
                     error == QNetworkReply::NoError ? status : 0);
        if (error != QNetworkReply::NoError)
            setError(error, "fixture transport failure");
        open(QIODevice::ReadOnly);
        if (!held)
            QTimer::singleShot(0, this, [this] { finish(); });
    }

    void finish()
    {
        if (isFinished())
            return;
        emit readyRead();
        if (isFinished())
            return;
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        if (isFinished())
            return;
        setError(OperationCanceledError, "cancelled");
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return bytes_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *output, qint64 maximum) override
    {
        const auto count = qMin<qint64>(maximum, bytes_.size() - offset_);
        if (count <= 0)
            return -1;
        std::memcpy(output, bytes_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    QByteArray bytes_;
    qint64 offset_ = 0;
};
} // namespace kobalt::test
