#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>
#include <QJsonObject>
#include "annotation_database.h"

namespace kobalt {
struct Highlight
{
    QString id, start, end, text, chapter;
    QDateTime created;
    // Absent provenance in older files preserves their exact endpoint.
    QString chosenEnd = {};
    bool sentenceEndPolicy = false;
    QString chosenStart = {};
    bool sentenceStartPolicy = false;
    QString color = QStringLiteral("gray");
    QString style = QStringLiteral("shaded");
    QString note = {};
    // Compact database records omit text, chapter and note until requested.
    bool detailsLoaded = true;
    // Bookmark-only metadata carried by the shared annotation transaction.
    QString bookmarkName = {}, bookmarkIcon = {};
};

struct HighlightExcerpt
{
    QString id, text, chapter;
};

QJsonObject highlightJson(const Highlight &highlight);
bool validHighlight(const Highlight &highlight);
bool validHighlightMetadata(const Highlight &highlight);
// Unknown future stored keys display gray without losing their original value.
bool isHighlightColor(const QString &color);
QString highlightDisplayColor(const QString &color);
bool isHighlightStyle(const QString &style);
QString highlightDisplayStyle(const QString &style);

// Separate passages keep their IDs even when their ranges overlap or reflow.
class HighlightStore
{
public:
    static constexpr int maxHighlights = 2000;
    static constexpr int maxTextUnits = 256 * 1024;
    static constexpr int maxNoteUnits = 64 * 1024;
    static constexpr qint64 maxFileBytes = 8 * 1024 * 1024;
    // root contains legacy JSON; database selects the shared profile database.
    // An omitted database path uses kobalt.sqlite under root for standalone stores.
    HighlightStore(QString root, QString identity, QByteArray fingerprint, QString title,
                   QString database = {});

    bool available() const { return !key_.isEmpty(); }

    bool load(QString *error = nullptr);
    bool loadCompact(QString *error = nullptr);
    bool details(const QStringList &ids, QVector<Highlight> *rows, QString *error = nullptr) const;
    bool excerpts(const QStringList &ids, QVector<HighlightExcerpt> *rows,
                  QString *error = nullptr) const;
    bool save(QString *error = nullptr);
    QString folder() const;

    // Metadata files read while looking for a changed sideloaded book's highlights.
    int adoptionMetadataReads() const { return adoptionMetadataReads_; }

    const QVector<Highlight> &highlights() const { return highlights_; }

    void setHighlights(QVector<Highlight> highlights) { highlights_ = std::move(highlights); }

    // A compact store keeps only rendering metadata for saved passages.
    void dropDetails();

    // Takes over what `saver`, a copy of this store, last wrote. Database reads
    // check the list revision, so they then match storage.
    void adoptSaved(const HighlightStore &saver)
    {
        persistedHighlights_ = saver.persistedHighlights_;
        databaseState_ = saver.databaseState_;
    }

private:
    friend class AnnotationDatabase;
    bool readFolder(const QString &folder, QString *error);
    bool load(bool compact, QString *error);
    QString root_, identity_, title_, database_, key_;
    QByteArray fingerprint_;
    QVector<Highlight> highlights_;
    QVector<Highlight> persistedHighlights_;
    AnnotationDatabaseState databaseState_;
    bool sqlite_ = annotationDatabaseEnabled();
    bool compact_ = false;
    bool loadFailed_ = false;
    int adoptionMetadataReads_ = 0;
};
} // namespace kobalt
