#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/column_view.h"
#include "engine/crengine/engine_document.h"
#include "engine/crengine/reader_locations.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QXmlStreamReader>
#include <cstdio>
#include <lvdocview.h>

using namespace kobalt;

namespace kobalt {
// Reads CREngine's DOM to choose test positions and to compare it with the
// source XHTML. Linked only into this check.
struct ReaderLocationProbe
{
    struct Sample
    {
        QString position;
        // The element that holds the position, as source path and text.
        QString href;
        QVector<int> path;
        QString name, text;
    };

    static QString qstring(const lString32 &s)
    {
        return QString::fromUtf8(UnicodeToUtf8(s).c_str());
    }

    static void collect(ldomNode *node, QVector<ldomNode *> &texts)
    {
        for (int i = 0; i < node->getChildCount(); ++i) {
            auto *child = node->getChildNode(i);
            if (child->isText()) {
                if (child->getText().length())
                    texts.append(child);
            } else if (!enginelocation::generated(child)) {
                collect(child, texts);
            }
        }
    }

    // Up to `limit` text nodes for each spine item, spread evenly, with three
    // positions in each: the start, the middle and the end.
    static QVector<Sample> samples(const EngineDocument &model, int limit, int *fragments)
    {
        QVector<Sample> result;
        auto *root = model.engine_->getDocument()->nodeFromXPath(U"/body");
        *fragments = 0;
        for (int f = 0; root && f < root->getChildCount(); ++f) {
            auto *fragment = root->getChildNode(f);
            auto *body = fragment->isElement() ? enginelocation::body(fragment) : nullptr;
            if (!body)
                continue;
            ++*fragments;
            QVector<ldomNode *> texts;
            collect(body, texts);
            const int step = qMax(1, int(texts.size()) / limit);
            for (int i = 0; i < texts.size(); i += step) {
                auto *text = texts[i];
                auto *element = text->getParentNode();
                while (element && enginelocation::wrapper(element))
                    element = element->getParentNode();
                Sample sample;
                sample.href = qstring(fragment->getAttributeValue("Source"));
                if (element && enginelocation::elementPath(body, element, sample.path)) {
                    sample.name = qstring(element->getNodeName());
                    for (auto *node : enginelocation::textNodes(element))
                        sample.text += qstring(node->getText());
                }
                const int length = text->getText().length();
                for (const int offset : { 0, length / 2, length }) {
                    sample.position = qstring(ldomXPointer(text, offset).toString());
                    result.append(sample);
                }
            }
        }
        return result;
    }

    // The counted characters of the spine item before a position, or -1.
    static int counted(const EngineDocument &model, const QString &position)
    {
        const auto pointer = model.engine_->getDocument()->createXPointer(
                Utf8ToUnicode(position.toUtf8().constData()));
        auto *node = pointer.getNode();
        auto *fragment = node;
        while (fragment && !fragment->isNodeName("DocFragment"))
            fragment = fragment->getParentNode();
        auto *body = fragment ? enginelocation::body(fragment) : nullptr;
        if (!body || !node->isText())
            return -1;
        int count = 0;
        for (auto *text : enginelocation::textNodes(body)) {
            const auto content = text->getText();
            const int end = text == node ? pointer.getOffset() : content.length();
            for (int i = 0; i < end; ++i)
                count += !locationUncounted(content[i]);
            if (text == node)
                return count;
        }
        return -1;
    }

    static QByteArray source(const EngineDocument &model, const QString &href)
    {
        auto stream = model.engine_->getDocument()->getContainer()->OpenStream(
                Utf8ToUnicode(href.toUtf8().constData()).c_str(), LVOM_READ);
        if (stream.isNull() || stream->GetSize() > 64 * 1024 * 1024)
            return {};
        QByteArray bytes(int(stream->GetSize()), '\0');
        lvsize_t read = 0;
        if (stream->Read(bytes.data(), bytes.size(), &read) != LVERR_OK
            || read != lvsize_t(bytes.size()))
            return {};
        return bytes;
    }
};
} // namespace kobalt

namespace {
using Probe = ReaderLocationProbe;

// The elements of one source file, with the text below each.
struct Source
{
    struct Element
    {
        QString name, text;
        QVector<int> children;
        int parent = -1;
    };

    QVector<Element> elements;
    int body = -1;
    bool parsed = false;

    static Source read(const QByteArray &xml)
    {
        Source source;
        QVector<int> open;
        QXmlStreamReader parser(xml);
        while (!parser.atEnd()) {
            parser.readNext();
            if (parser.isStartElement()) {
                Element element;
                element.name = parser.name().toString();
                element.parent = open.isEmpty() ? -1 : open.last();
                const int index = source.elements.size();
                if (!open.isEmpty())
                    source.elements[open.last()].children.append(index);
                if (source.body < 0 && element.name == QLatin1String("body"))
                    source.body = index;
                source.elements.append(element);
                open.append(index);
            } else if (parser.isEndElement()) {
                open.removeLast();
            } else if (parser.isCharacters()) {
                for (const int index : open)
                    source.elements[index].text += parser.text();
            }
        }
        source.parsed = !parser.hasError() && source.body >= 0;
        return source;
    }

    int at(const QVector<int> &path) const
    {
        int node = body;
        for (const int step : path) {
            if (step >= elements[node].children.size())
                return -1;
            node = elements[node].children[step];
        }
        return node;
    }
};

QString counted(const QString &text)
{
    QString result;
    for (const auto c : text)
        if (!locationUncounted(c.unicode()))
            result += c;
    return result;
}

struct Totals
{
    int positions = 0, exact = 0, equivalent = 0, failed = 0;
    int elements = 0, unparsed = 0, missing = 0, renamed = 0, changed = 0, sameText = 0;
};

Totals check(const QString &label, const QByteArray &bytes, bool report)
{
    EngineDocument model(test::readerFonts());
    QString error;
    require(model.open(bytes, { 600, 800 },
                       { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error),
            qPrintable(label + ": " + error));
    Totals totals;
    int fragments = 0;
    const auto samples = Probe::samples(model, 400, &fragments);
    require(fragments > 0 && !samples.isEmpty(), qPrintable(label + ": no text to sample"));
    QHash<QString, Source> sources;
    QString firstFailure;
    for (int i = 0; i < samples.size(); ++i) {
        const auto &sample = samples[i];
        ++totals.positions;
        Location location;
        Restoration restoration = Restoration::Unresolved;
        const bool located = model.locationOf(sample.position, &location);
        const auto back = located ? model.positionOf(location, &restoration) : QString();
        if (back == sample.position && restoration == Restoration::Exact) {
            ++totals.exact;
        } else if (!back.isEmpty() && restoration == Restoration::Exact
                   && Probe::counted(model, back) >= 0
                   && Probe::counted(model, back) == Probe::counted(model, sample.position)) {
            // Another position at the same place in the text: only
            // whitespace or markup is between the two.
            ++totals.equivalent;
        } else {
            ++totals.failed;
            if (firstFailure.isEmpty())
                firstFailure = sample.position + " -> " + back;
        }
        // One source comparison for each sampled element.
        if (i % 3)
            continue;
        ++totals.elements;
        if (!sources.contains(sample.href))
            sources.insert(sample.href, Source::read(Probe::source(model, sample.href)));
        const auto &source = sources[sample.href];
        if (!source.parsed) {
            ++totals.unparsed;
            continue;
        }
        const int element = sample.name.isEmpty() ? -1 : source.at(sample.path);
        if (element < 0)
            ++totals.missing;
        else if (source.elements[element].name.compare(sample.name, Qt::CaseInsensitive) != 0)
            ++totals.renamed;
        else if (counted(source.elements[element].text) != counted(sample.text)) {
            if (report && totals.changed < 3) {
                const auto a = counted(source.elements[element].text), b = counted(sample.text);
                int at = 0;
                while (at < a.size() && at < b.size() && a[at] == b[at])
                    ++at;
                std::printf(
                        "  changed text in <%s> at %d: source [%s] U+%04X, engine [%s] U+%04X\n",
                        qPrintable(sample.name), at, qPrintable(a.mid(qMax(0, at - 12), 30)),
                        at < a.size() ? a[at].unicode() : 0,
                        qPrintable(b.mid(qMax(0, at - 12), 30)),
                        at < b.size() ? b[at].unicode() : 0);
            }
            ++totals.changed;
        } else if (source.elements[element].text == sample.text)
            ++totals.sameText;
    }
    if (report) {
        std::printf(
                "%s: %d positions, %d exact, %d equivalent, %d failed; %d elements, "
                "%d unparsed source, %d missing, %d renamed, %d changed text, %d identical text\n",
                qPrintable(label), totals.positions, totals.exact, totals.equivalent, totals.failed,
                totals.elements, totals.unparsed, totals.missing, totals.renamed, totals.changed,
                totals.sameText);
        if (!firstFailure.isEmpty())
            std::printf("  first failure: %s\n", qPrintable(firstFailure));
    }
    return totals;
}
} // namespace

// With a folder argument, reports each EPUB in it and requires nothing: this
// is the survey for books that are not in the repository.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc > 1) {
        const QDir folder(QString::fromLocal8Bit(argv[1]));
        for (const auto &name : folder.entryList({ "*.epub" }, QDir::Files, QDir::Name)) {
            QFile file(folder.filePath(name));
            if (file.open(QIODevice::ReadOnly))
                check(name, file.readAll(), true);
        }
        return 0;
    }
    for (const auto &name : { "trevelyans-type-tester.kepub.epub", "figure-caption.epub",
                              "perf-notes.epub", "kobalt-benchmark.kepub.epub" }) {
        QFile file(QStringLiteral(KOBALT_TEST_FIXTURES) + '/' + name);
        require(file.open(QIODevice::ReadOnly), name);
        const auto totals = check(name, file.readAll(), true);
        require(!totals.failed, "A position did not return to the same place in the text");
        require(!totals.missing && !totals.renamed && !totals.changed,
                "CREngine's elements differ from the source XHTML");
    }
    std::printf("PASS: engine positions convert to source locations and back\n");
    return 0;
}
