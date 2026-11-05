#pragma once
#include <QElapsedTimer>
#include <QHostInfo>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace kobalt::kosync {
// Qt5's asynchronous resolver serializes each hostname across all managers.
// A separate lookup can refresh its cache while an old lookup is still waiting.
class HostLookup : public QObject {
public:
    using Resolver = std::function<QHostInfo(const QString &)>;
    using Done = std::function<void(QHostInfo)>;
    explicit HostLookup(Resolver resolver = {});
    ~HostLookup() override;
    void start(const QString &host, int timeout, Done done);
    void cancel();
    bool busy() const { return bool(state_); }

private:
    struct State;
    struct Job;
    Resolver resolver_;
    std::shared_ptr<State> state_;
    Done done_;
    QTimer poll_;
    QElapsedTimer elapsed_;
    int timeout_ = 0;
};
}
