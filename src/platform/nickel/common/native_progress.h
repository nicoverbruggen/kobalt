#pragma once
#include "reading/reading_progress.h"
#include <QObject>
#include <QVariantMap>

namespace kobalt {
struct ReadingEstimates;
class NativeProgress : public QObject {
    Q_OBJECT
public:
    explicit NativeProgress(QString book);
    ~NativeProgress() override;
    static bool available();
    ReadingProgress read(QString *error = nullptr) const;
    bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
               ReadingProgress *verified, QString *error, const ReadingEstimates *estimates = nullptr);
    static void disconnected();
    bool deviceTransition() const {
        return transitioning;
    }
    // True once after Nickel may have changed the saved progress: after sync,
    // an account update or USB.
    bool takeChange() {
        const bool value = changed;
        changed = false;
        return value;
    }
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
    static QJsonObject testAction(const QJsonObject &command);
#endif
private slots:
    void beginTransition() {
        transitioning = true;
    }
    void beginProcessing() {
        processing = transitioning = true;
    }
    void endTransition() {
        transitioning = processing = false;
        changed = true;
    }
    void markChanged() {
        changed = true;
    }

private:
    QString book;
    quint64 connectionId;
    bool transitioning = false, processing = false, changed = true;
    QVariantMap persisted(QString *error) const;
};
}
