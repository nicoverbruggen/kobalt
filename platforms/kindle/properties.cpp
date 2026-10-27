#include "properties.h"
#include <QDeadlineTimer>
#include <QPointer>
#include <utility>

namespace kobalt::kindle {
Properties::Properties(Commands commands, QObject *parent) : QObject(parent), commands(std::move(commands)) {
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
        expired = true;
        process.kill();
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        complete(!expired && code == 0 && status == QProcess::NormalExit);
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false);
    });
}
Properties::~Properties() { stop(); }
QStringList Properties::arguments(const Property &property) {
    return {property.integer ? QStringLiteral("-i") : QStringLiteral("-s"), QStringLiteral("-q"),
            property.publisher, property.name};
}
void Properties::read(Property property, Finished finished) {
    if (stopped) {
        finished({});
        return;
    }
    queued.enqueue({std::move(property), std::move(finished)});
    if (!active)
        startNext();
}
void Properties::startNext() {
    if (stopped || active || queued.isEmpty())
        return;
    current = queued.dequeue();
    active = true;
    expired = false;
    process.setProgram(commands.get);
    process.setArguments(arguments(current.property));
    deadline.start(commands.timeoutMilliseconds);
    process.start();
}
void Properties::complete(bool success) {
    if (stopped || !active)
        return;
    deadline.stop();
    const auto bytes = process.readAllStandardOutput();
    process.readAllStandardError();
    auto finished = std::move(current.finished);
    current = {};
    active = false;
    QPointer<Properties> guard(this);
    finished({success && bytes.size() <= 4096, bytes.size() <= 4096 ? bytes.trimmed() : QByteArray()});
    if (guard)
        QTimer::singleShot(0, this, [this] { startNext(); });
}
PropertyReply Properties::run(const QString &program, const QStringList &arguments, int milliseconds) {
    if (milliseconds <= 0)
        return {};
    QProcess child;
    QDeadlineTimer limit(milliseconds);
    child.start(program, arguments);
    if (!child.waitForStarted(int(limit.remainingTime())) ||
        !child.waitForFinished(int(limit.remainingTime()))) {
        child.kill();
        child.waitForFinished(200);
        return {};
    }
    const auto bytes = child.readAllStandardOutput();
    return {child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 && bytes.size() <= 4096,
            bytes.size() <= 4096 ? bytes.trimmed() : QByteArray()};
}
PropertyReply Properties::readNow(const Property &property, int milliseconds) {
    return stopped ? PropertyReply{} : run(commands.get, arguments(property), milliseconds);
}
bool Properties::write(const Property &property, int value, int milliseconds) {
    if (stopped || !property.integer)
        return false;
    auto args = arguments(property);
    args.append(QString::number(value));
    return run(commands.set, args, milliseconds).ok;
}
void Properties::stop() {
    if (std::exchange(stopped, true))
        return;
    deadline.stop();
    queued.clear();
    current = {};
    active = false;
    process.disconnect(this);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(200);
    }
}
}
