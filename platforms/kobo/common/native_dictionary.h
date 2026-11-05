#pragma once
#include <QObject>
#include "platform/api/dictionary_service.h"
#include <memory>
class QWidget;

namespace kobalt {
#ifndef KOBALT_READER_HARDWARE
extern void (*dictionaryTestNextReadingView)(void *, QWidget *);
extern void (*dictionaryTestNextWebView)(void *, QWidget *);
#endif
// GUI-thread adapter. Use only after firmwareSupported() accepts Nickel.
// No native view is needed, and no native objects leave this adapter.
class NativeDictionary : public QObject
{
    Q_OBJECT
public:
    NativeDictionary();
    ~NativeDictionary() override;
    static bool available();
    QVector<DictionaryChoice> choices() const;
    bool select(const QString &book, const QString &id, QString *error);
    DictionaryResult lookup(const QString &book, const QString &word);
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
#endif
private slots:
    void definition(const QString &word, const QString &html, const QUrl &url);

private:
    struct Data;
    std::unique_ptr<Data> data;
    DictionaryResult *pending = nullptr;
};
} // namespace kobalt
