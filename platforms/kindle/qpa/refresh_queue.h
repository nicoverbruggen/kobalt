#pragma once

#include <QImage>
#include <QObject>
#include <QRegion>
#include <QString>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace kindle {
struct RefreshFrame {
    QImage pixels;
    QRegion damage;
    quint64 serial = 0;
    quint64 epoch = 0;
    bool full = false;
    int animationDirection = 0;
};
struct RefreshResult {
    bool ok = true;
    bool fatal = false;
    QString error;
};

// The driver owns the submitted pixels until its bounded completion wait ends.
// GUI submissions only replace the pending snapshot; they never wait for I/O.
class RefreshQueue final : public QObject {
    Q_OBJECT
public:
    using Present = std::function<RefreshResult(const RefreshFrame &)>;
    explicit RefreshQueue(Present present, QObject *parent = nullptr);
    ~RefreshQueue() override;
    bool enqueue(RefreshFrame frame);
    quint64 invalidate();
    void cancelAnimation();
    quint64 epoch() const;
    void stop();
signals:
    void completed(quint64 serial, quint64 epoch);
    void discarded(quint64 serial);
    void failed(QString error);
private:
    void run();
    Present present;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<RefreshFrame> pending;
    quint64 epoch_ = 1;
    bool stopping = false;
    bool needsFull = true;
    std::thread worker;
};
}
