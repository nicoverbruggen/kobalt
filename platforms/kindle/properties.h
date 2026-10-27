#pragma once

#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <functional>

namespace kobalt::kindle {
struct Property {
    QString publisher, name;
    bool integer = true;
};
struct PropertyReply {
    bool ok = false;
    QByteArray value;
};

// Status queries never wait on the GUI thread. Light writes need an
// acknowledgement, so their subprocess lifetime has a separate hard deadline.
class Properties final : public QObject {
    Q_OBJECT
public:
    struct Commands {
        QString get = QStringLiteral("/usr/bin/lipc-get-prop");
        QString set = QStringLiteral("/usr/bin/lipc-set-prop");
        int timeoutMilliseconds = 1000;
    };
    explicit Properties(Commands commands, QObject *parent = nullptr);
    ~Properties() override;
    using Finished = std::function<void(PropertyReply)>;
    void read(Property property, Finished finished);
    PropertyReply readNow(const Property &property, int milliseconds);
    bool write(const Property &property, int value, int milliseconds);
    void stop();
    int timeout() const { return commands.timeoutMilliseconds; }
private:
    struct Request { Property property; Finished finished; };
    Commands commands;
    QProcess process;
    QTimer deadline;
    QQueue<Request> queued;
    Request current;
    bool active = false, stopped = false, expired = false;
    void startNext();
    void complete(bool success);
    static QStringList arguments(const Property &property);
    static PropertyReply run(const QString &program, const QStringList &arguments, int milliseconds);
};
}
