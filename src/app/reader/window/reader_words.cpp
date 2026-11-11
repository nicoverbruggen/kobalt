#include "reader_p.h"
#include "reading/features.h"
#include "storage/bookmark_store.h"
#include "support/strings.h"
#include <QDateTime>
#include <QUrl>

namespace kobalt::reader {
void Reader::buildWords()
{
    if (!savedWords)
        return;
    wordNotice = new ui::ButtonFeedback(this);
    wordNotice->setObjectName("rrWordNotice");
    savedWords->relocate(wordsBook(), wordsSource(), session->title(), session->author());
    wordList = new kobalt::WordList(bookmarkList, kobalt::WordList::Scope::Book);
    wordList->setSortOrder(preferences.value("marks_sort").toString("position"));
    wordList->selected = [this](const kobalt::SavedWord &word) { openWord(word); };
    wordList->removeRequested = [this](const kobalt::SavedWord &word) {
        savedWords->remove(word.book, word.word);
    };
    bookmarkList->setWordsView(wordList);
    bookmarkList->wordsRequested = [this] {
        if (features::enabled(preferences, features::Words))
            showWords();
    };
    bookmarkList->allWordsRequested = [this] { showAllWords(); };
    // Both cover the Bookmarks list and share its place between the reader's bars.
    allWords = new kobalt::WordList(bookmarkList, kobalt::WordList::Scope::Library);
    bookmarkList->addCoveringPanel(allWords);
    allWords->back = [this] { allWords->hide(); };
    allWords->selected = [this](const kobalt::SavedWord &word) { openWord(word); };
    allWords->removeRequested = [this](const kobalt::SavedWord &word) {
        savedWords->remove(word.book, word.word);
    };
    wordPage = new kobalt::WordPage(bookmarkList);
    bookmarkList->addCoveringPanel(wordPage);
    wordPage->back = [this] { wordPage->hide(); };
    wordPage->removeRequested = [this](const kobalt::SavedWord &word) {
        savedWords->remove(word.book, word.word);
        wordPage->hide();
    };
    wordPage->passageRequested = [this](const kobalt::SavedWord &word) {
        wordPage->hide();
        if (allWords)
            allWords->hide();
        bookmarkList->hide();
        goToPassage(word.position);
    };
    savedWords->changed = [this] { reloadWords(); };
    dictionaryPanel->saveToggled = [this](bool save) {
        if (lookupSnapshot.book.isEmpty() || !features::enabled(preferences, features::Words))
            return;
        const auto epoch = ++savedLookupEpoch;
        QPointer<Reader> guard(this);
        const auto finished = [guard, epoch, save](bool ok) {
            if (!guard || guard->savedLookupEpoch != epoch || !guard->dictionaryPanel->isVisible()
                || !features::enabled(guard->preferences, features::Words))
                return;
            if (!ok)
                guard->dictionaryPanel->setSaved(!save, true);
            guard->showWordNotice(strings::get(!ok            ? "words.update_failed"
                                                       : save ? "words.added"
                                                              : "words.removed"));
        };
        if (save)
            savedWords->record(lookupSnapshot, finished);
        else
            savedWords->remove(lookupSnapshot.book, lookupSnapshot.word, finished);
        dictionaryPanel->setSaved(save, true);
    };
}

void Reader::showWordNotice(const QString &text)
{
    if (!isVisible() || lifecycleHidden || closing || services.suspended() || !wordNotice)
        return;
    wordNotice->showFor(dictionaryPanel->saveActionButton(), text);
}

void Reader::placeWordNotice()
{
    if (!wordNotice)
        return;
    wordNotice->setGeometry(rect());
    wordNotice->reposition();
}

// The Library's key for this file: the host's source key, or a local path.
QString Reader::wordsSource() const
{
    return book->sourceKey().isEmpty() && book->identity().startsWith("file://")
            ? QUrl(book->identity()).toLocalFile()
            : book->sourceKey();
}

QString Reader::wordsBook() const
{
    return BookmarkStore::bookKey(book->identity(), book->fingerprint());
}

void Reader::recordLookup(const QString &query, const kobalt::DictionaryResult &result)
{
    const bool found = result.received && result.error.isEmpty()
            && !kobalt::DictionaryPanel::formatDefinition(result.html).trimmed().isEmpty();
    if (!savedWords || query.simplified().isEmpty() || wordsBook().isEmpty()
        || !features::enabled(preferences, features::Words)) {
        dictionaryPanel->setSaved(false, false);
        return;
    }
    kobalt::SavedWord word;
    word.book = wordsBook();
    word.word = query;
    word.identity = book->identity();
    word.fingerprint = QString::fromLatin1(book->fingerprint());
    word.sourceKey = wordsSource();
    word.title = session->title();
    word.author = session->author();
    word.chapter = session->chapterTitle();
    word.context = lookupContext;
    word.position = lookupPosition;
    // A word without an entry, such as a name from the book, is saved
    // without a definition, and only by hand.
    word.dictionary = found ? result.dictionary : QString();
    word.definition = found ? result.html : QString();
    word.created = word.lookedUp = QDateTime::currentMSecsSinceEpoch();
    lookupSnapshot = word;
    const auto epoch = ++savedLookupEpoch;
    if (!found || !preferences.value("save_looked_up_words").toBool(true)) {
        // The sheet shows whether an earlier lookup saved the word.
        dictionaryPanel->setSaved(false, true);
        QPointer<Reader> guard(this);
        savedWords->contains(wordsBook(), query, [guard, epoch](bool saved) {
            if (guard && guard->savedLookupEpoch == epoch && guard->dictionaryPanel->isVisible())
                guard->dictionaryPanel->setSaved(saved, true);
        });
        return;
    }
    savedWords->record(word);
    dictionaryPanel->setSaved(true, true);
}

void Reader::showWords()
{
    if (!savedWords)
        return;
    annotationPopover->hide();
    wordList->setExcerptFont(bookFace());
    wordList->setGlossFont(settingsDictionaryFontFace.isEmpty() ? bookFace()
                                                                : settingsDictionaryFontFace);
    wordList->setWords(wordList->words(), wordList->words().isEmpty());
    bookmarkList->showWords();
    reloadWords();
}

void Reader::showAllWords()
{
    if (!savedWords)
        return;
    allWords->setGeometry(bookmarkList->rect());
    allWords->setExcerptFont(bookFace());
    allWords->setGlossFont(settingsDictionaryFontFace.isEmpty() ? bookFace()
                                                                : settingsDictionaryFontFace);
    allWords->setWords(allWords->words(), allWords->words().isEmpty());
    allWords->show();
    allWords->raise();
    reloadWords();
}

// Lists are read when they open and after each change, on the writer thread.
void Reader::reloadWords()
{
    if (!savedWords)
        return;
    QPointer<Reader> guard(this);
    if (bookmarkList->isVisible() && bookmarkList->wordsShown())
        savedWords->load(wordsBook(), [guard](bool ok, QVector<kobalt::SavedWord> words) {
            if (!guard || !ok)
                return;
            QStringList positions;
            for (const auto &word : words)
                positions.append(word.position);
            guard->wordList->setWords(words);
            // Pages follow the current layout, as bookmark pages do.
            guard->session->bookmarkPages(positions, [guard, positions](QVector<int> found) {
                if (!guard || found.size() != positions.size())
                    return;
                QHash<QString, int> pages;
                for (int i = 0; i < positions.size(); ++i)
                    pages.insert(positions[i], found[i]);
                guard->wordList->setPages(std::move(pages));
            });
        });
    if (allWords->isVisible())
        savedWords->load({}, [guard](bool ok, QVector<kobalt::SavedWord> words) {
            if (guard && ok)
                guard->allWords->setWords(std::move(words));
        });
}

void Reader::openWord(const kobalt::SavedWord &word)
{
    wordPage->setFonts(bookFace(), settingsDictionaryFontFace, settingsDictionaryFontSize);
    wordPage->setGeometry(bookmarkList->rect());
    if (word.book != wordsBook()) {
        wordPage->open(word, {}, strings::get("words.open_book_first"));
        return;
    }
    QPointer<Reader> guard(this);
    session->bookmarkPages({ word.position }, [guard, word](QVector<int> found) {
        if (!guard)
            return;
        const int page = found.value(0, -1);
        guard->wordPage->open(
                word, page >= 0 ? strings::get("words.go_to_page").arg(page + 1) : QString(),
                page >= 0 ? QString() : strings::get("words.passage_missing"));
    });
}
} // namespace kobalt::reader
