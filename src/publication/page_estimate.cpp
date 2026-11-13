#include "page_estimate.h"
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>
#include "support/qt_compat.h"
#include <climits>
#include <cmath>

namespace kobalt {
namespace {
// The advance of one character in units of the font size. These are typical
// values for a book face, not measurements of the reading font. Real glyph
// advances were measured and were not more precise once the model weights
// were fitted; see doc/engines/migration.md.
float advance(char32_t c)
{
    if (c == U' ')
        return 0.26f;
    if (c < 0x80) {
        switch (c) {
        case 'i':
        case 'j':
        case 'l':
        case 'I':
        case 'f':
        case 't':
        case 'r':
        case '.':
        case ',':
        case ';':
        case ':':
        case '\'':
        case '!':
        case '|':
        case '`':
        case '(':
        case ')':
        case '[':
        case ']':
            return 0.3f;
        case 'm':
        case 'w':
        case 'M':
        case 'W':
        case '@':
            return 0.85f;
        default:
            break;
        }
        if (c >= 'A' && c <= 'Z')
            return 0.68f;
        if (c >= '0' && c <= '9')
            return 0.52f;
        return 0.5f;
    }
    if (c == 0x2014)
        return 1.0f;
    if (c == 0x2018 || c == 0x2019)
        return 0.3f;
    // CJK and other full-width scripts.
    if (c >= 0x2E80 && c <= 0xD7FF)
        return 1.0f;
    if (c >= 0xF900 && c <= 0xFAFF)
        return 1.0f;
    if (c >= 0xFF00 && c <= 0xFFEF)
        return 1.0f;
    if (c >= 0x20000)
        return 1.0f;
    return 0.55f;
}

// The text of a line takes this much more room than the advance table says:
// book faces are wider than the table, and a line is not filled to its end.
constexpr double textRoom = 1.15 / 0.97;

bool whitespace(QChar c)
{
    const auto unit = c.unicode();
    return unit == ' ' || unit == '\t' || unit == '\n' || unit == '\r' || unit == '\f';
}

bool heading(const QString &name)
{
    return name.size() == 2 && name[0] == QLatin1Char('h') && name[1] >= QLatin1Char('1')
            && name[1] <= QLatin1Char('6');
}

float headingScale(const QString &name)
{
    if (!heading(name))
        return 1;
    switch (name[1].unicode()) {
    case '1':
        return 1.6f;
    case '2':
        return 1.4f;
    case '3':
        return 1.2f;
    default:
        return 1;
    }
}

const QSet<QString> &blockNames()
{
    static const QSet<QString> names{
        "p",       "div",     "h1",     "h2",     "h3",      "h4",   "h5",     "h6",
        "li",      "ul",      "ol",     "dl",     "dt",      "dd",   "pre",    "blockquote",
        "section", "article", "header", "footer", "aside",   "nav",  "figure", "figcaption",
        "table",   "tr",      "td",     "th",     "caption", "body", "hgroup", "address"
    };
    return names;
}

bool indents(const QString &name)
{
    return name == QLatin1String("blockquote") || name == QLatin1String("ul")
            || name == QLatin1String("ol") || name == QLatin1String("dd");
}

bool cell(const QString &name)
{
    return name == QLatin1String("td") || name == QLatin1String("th");
}

bool listItem(const QString &name)
{
    return name == QLatin1String("li") || name == QLatin1String("dt")
            || name == QLatin1String("dd");
}

bool skipped(const QString &name)
{
    return name == QLatin1String("head") || name == QLatin1String("script")
            || name == QLatin1String("style") || name == QLatin1String("title");
}

quint8 add(quint8 count)
{
    return count < 255 ? quint8(count + 1) : count;
}

// The lines of evidence that the reference weights count as. A weight of
// lines is drawn towards its reference as if 1000 lines of the open book
// confirmed it. The weights of block counts and of pages need fewer.
const double referenceStrength[PageEstimateFeatures::Count] = { 1000, 1000, 1000, 30, 1000,
                                                                30,   30,   30,   5,  50 };
} // namespace

PageEstimateMeasurer::PageEstimateMeasurer() : parser_(new QXmlStreamReader) { }

PageEstimateMeasurer::~PageEstimateMeasurer() = default;

void PageEstimateMeasurer::flush(PageEstimateBlock::Kind kind)
{
    if (content_ || kind != PageEstimateBlock::Text) {
        PageEstimateBlock block;
        block.width = width_;
        block.scale = scales_.last();
        block.depth = quint8(qBound(0, depth_, 255));
        block.end = characters_;
        block.quotes = quotes_;
        block.listItems = listItems_;
        block.rows = rows_;
        quotes_ = listItems_ = rows_ = 0;
        block.kind = kind;
        if (kind == PageEstimateBlock::Text || kind == PageEstimateBlock::Line) {
            if (cells_ > 0)
                block.kind = PageEstimateBlock::Cell;
            else if (headings_ > 0)
                block.kind = PageEstimateBlock::Heading;
            else if (broken_ || kind == PageEstimateBlock::Line)
                block.kind = PageEstimateBlock::Line;
        }
        item_.blocks.append(block);
    }
    // Text after a line break in the same block is the next line of verse.
    broken_ = kind == PageEstimateBlock::Line;
    width_ = 0;
    content_ = pendingSpace_ = false;
}

void PageEstimateMeasurer::start(const QString &name)
{
    if (skip_ || skipped(name)) {
        ++skip_;
        return;
    }
    if (wordSkip_ || name == QLatin1String("rt") || name == QLatin1String("rp"))
        ++wordSkip_;
    if (!wordSkip_
        && (blockNames().contains(name) || name == QLatin1String("br")
            || name == QLatin1String("hr")))
        wordCounter_.boundary();
    if (name == QLatin1String("br")) {
        flush(PageEstimateBlock::Line);
    } else if (name == QLatin1String("hr")) {
        flush(PageEstimateBlock::Text);
        flush(PageEstimateBlock::Rule);
    } else if (name == QLatin1String("img") || name == QLatin1String("svg")
               || name == QLatin1String("image")) {
        flush(PageEstimateBlock::Text);
        flush(PageEstimateBlock::Image);
        // An image inside an SVG is the same picture.
        if (name == QLatin1String("svg"))
            ++skip_;
    } else if (blockNames().contains(name)) {
        flush(PageEstimateBlock::Text);
        scales_.append(scales_.last() * headingScale(name));
        depth_ += indents(name);
        cells_ += cell(name);
        headings_ += heading(name);
        if (name == QLatin1String("blockquote"))
            quotes_ = add(quotes_);
        else if (listItem(name))
            listItems_ = add(listItems_);
        else if (name == QLatin1String("tr"))
            rows_ = add(rows_);
    }
}

void PageEstimateMeasurer::end(const QString &name)
{
    if (skip_) {
        --skip_;
        return;
    }
    if (wordSkip_)
        --wordSkip_;
    if (!wordSkip_ && blockNames().contains(name))
        wordCounter_.boundary();
    if (blockNames().contains(name)) {
        flush(PageEstimateBlock::Text);
        if (scales_.size() > 1)
            scales_.removeLast();
        depth_ -= indents(name);
        cells_ -= cell(name);
        headings_ -= heading(name);
    }
}

void PageEstimateMeasurer::text(const QString &text)
{
    const float scale = scales_.last();
    for (int i = 0; i < text.size(); ++i) {
        const QChar unit = text[i];
        if (whitespace(unit)) {
            if (!wordSkip_)
                wordCounter_.append(unit.unicode());
            pendingSpace_ = content_;
            continue;
        }
        char32_t c = unit.unicode();
        if (unit.isHighSurrogate() && i + 1 < text.size())
            c = QChar::surrogateToUcs4(unit, text[++i]);
        if (!wordSkip_)
            wordCounter_.append(c);
        if (pendingSpace_) {
            width_ += advance(U' ') * scale;
            ++characters_;
        }
        pendingSpace_ = false;
        width_ += advance(c) * scale;
        content_ = true;
        ++characters_;
    }
}

void PageEstimateMeasurer::feed(const QByteArray &chunk, bool last)
{
    if (done_)
        return;
    parser_->addData(chunk);
    while (!parser_->atEnd()) {
        parser_->readNext();
        if (parser_->isStartElement())
            start(parser_->name().toString());
        else if (parser_->isEndElement())
            end(parser_->name().toString());
        else if (parser_->isCharacters() && !skip_)
            text(parser_->text().toString());
    }
    const bool waiting = parser_->error() == QXmlStreamReader::PrematureEndOfDocumentError;
    if (waiting && !last)
        return;
    flush(PageEstimateBlock::Text);
    item_.characters = characters_;
    item_.words = wordCounter_.words();
    item_.parsed = !parser_->hasError();
    done_ = true;
    parser_.reset();
}

PageEstimateItem measurePageEstimate(const QByteArray &xhtml)
{
    PageEstimateMeasurer measurer;
    measurer.feed(xhtml, true);
    return measurer.item();
}

double pageEstimateTextFraction(const QByteArray &xhtml, const QString &path, bool previous,
                                qint64 *wordsBefore, const std::function<bool()> &current)
{
    // The steps of the path: an element name or "text()", and its index
    // among the siblings of the same kind, counted from one.
    struct Step
    {
        QString name;
        int index = 1;
    };
    QVector<Step> steps;
    int offset = 0;
    QString rest = path;
    const int dot = rest.lastIndexOf('.');
    if (dot > rest.lastIndexOf('/')) {
        offset = qMax(0, rest.mid(dot + 1).toInt());
        rest.truncate(dot);
    }
    for (const auto &part : rest.split('/', qt::skipEmptyParts)) {
        const int open = part.indexOf('[');
        Step step;
        step.name = open < 0 ? part : part.left(open);
        if (open >= 0)
            step.index = qMax(1, part.mid(open + 1, part.size() - open - 2).toInt());
        steps.append(step);
    }
    const bool text = !steps.isEmpty() && steps.last().name == QLatin1String("text()");
    const int elements = steps.size() - (text ? 1 : 0);
    // The characters as the measurer counts them: one space between words.
    qint64 characters = 0, target = -1, fallback = -1;
    LogicalWordCounter wordCounter;
    qint64 targetWords = -1, fallbackWords = -1;
    int wordSkip = 0;
    bool interrupted = false;
    unsigned work = 0;
    bool space = false;
    const auto count = [&](const QString &value, int limit) {
        for (int i = 0; i < value.size() && i < limit; ++i) {
            if (++work % 4096 == 0 && current && !current()) {
                interrupted = true;
                return;
            }
            if (!wordSkip && !value.at(i).isLowSurrogate()) {
                const auto c = value.at(i).isHighSurrogate() && i + 1 < value.size()
                        ? QChar::surrogateToUcs4(value.at(i), value.at(i + 1))
                        : uint(value.at(i).unicode());
                wordCounter.append(c);
            }
            if (value.at(i).isSpace()) {
                space = characters > 0;
            } else {
                characters += space ? 2 : 1;
                space = false;
            }
        }
    };
    // For each open element below <html>: the children seen by name, and
    // the text nodes.
    QVector<QHash<QString, int>> children{ {} };
    QVector<int> texts{ 0 };
    // How many steps the open elements match, and how deep they are.
    int matched = 0, depth = 0;
    bool body = false;
    QXmlStreamReader reader(xhtml);
    while (!reader.atEnd()) {
        if (interrupted || (++work % 128 == 0 && current && !current()))
            return -1;
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto name = reader.name().toString().toLower();
            if (depth == 0 && name == QLatin1String("html")) {
                // The root has no step.
                continue;
            }
            if (wordSkip || skipped(name) || name == QLatin1String("svg")
                || name == QLatin1String("rt") || name == QLatin1String("rp"))
                ++wordSkip;
            if (!wordSkip
                && (blockNames().contains(name) || name == QLatin1String("br")
                    || name == QLatin1String("hr")))
                wordCounter.boundary();
            const int index = ++children.last()[name];
            ++depth;
            children.append(QHash<QString, int>());
            texts.append(0);
            body = body || (depth == 1 && name == QLatin1String("body"));
            if (target < 0 && matched == depth - 1 && matched < elements
                && steps[matched].name == name && steps[matched].index == index) {
                ++matched;
                fallback = characters;
                fallbackWords = wordCounter.words();
                if (matched == elements && !text) {
                    target = characters;
                    targetWords = wordCounter.words();
                }
            }
        } else if (token == QXmlStreamReader::EndElement && depth > 0) {
            // A step that the element does not have: the position is at the
            // start of the deepest element found.
            if (target < 0 && matched == depth && matched < steps.size()) {
                target = fallback;
                targetWords = fallbackWords;
            }
            if (!wordSkip && blockNames().contains(reader.name().toString().toLower()))
                wordCounter.boundary();
            if (wordSkip)
                --wordSkip;
            matched = qMin(matched, depth - 1);
            --depth;
            children.removeLast();
            texts.removeLast();
        } else if (token == QXmlStreamReader::Characters && body && depth > 0) {
            const auto value = reader.text().toString();
            if (reader.isWhitespace()) {
                count(value, INT_MAX);
                continue;
            }
            const int index = ++texts.last();
            if (target < 0 && text && matched == depth && matched == elements
                && steps.last().index == index) {
                count(value, offset);
                target = characters;
                targetWords = wordCounter.words();
                count(value.mid(qMin(offset, int(value.size()))), INT_MAX);
                continue;
            }
            count(value, INT_MAX);
        }
    }
    if (interrupted || (current && !current()) || !body || characters <= 0)
        return -1;
    if (target < 0) {
        target = fallback < 0 ? 0 : fallback;
        targetWords = fallbackWords;
    }
    if (wordsBefore)
        *wordsBefore = qMax<qint64>(0, targetWords);
    if (previous && target > 0)
        --target;
    return qBound(0.0, double(target) / characters, 1.0);
}

double PageEstimateLayout::linesPerPage() const
{
    return valid() ? qMax(1.0, std::floor(height / lineHeight)) : 1;
}

bool PageEstimateLayout::operator==(const PageEstimateLayout &other) const
{
    return fontSize == other.fontSize && lineHeight == other.lineHeight && width == other.width
            && height == other.height && indent == other.indent;
}

PageEstimateFeatures &PageEstimateFeatures::operator+=(const PageEstimateFeatures &other)
{
    for (int i = 0; i < Count; ++i)
        value[i] += other.value[i];
    return *this;
}

PageEstimateFeatures pageEstimateFeatures(const PageEstimateItem &item,
                                          const PageEstimateLayout &layout, int first, int last)
{
    PageEstimateFeatures features;
    if (!layout.valid())
        return features;
    using F = PageEstimateFeatures;
    auto &value = features.value;
    const double page = layout.linesPerPage();
    const int end = last < 0 ? item.blocks.size() : qMin(last, int(item.blocks.size()));
    for (int i = qMax(0, first); i < end; ++i) {
        const auto &block = item.blocks[i];
        value[F::Quotes] += block.quotes;
        value[F::ListItems] += block.listItems;
        value[F::Rows] += block.rows;
        switch (block.kind) {
        case PageEstimateBlock::Image:
            value[F::Images] += page;
            continue;
        case PageEstimateBlock::Rule:
            value[F::ProseLines] += 1;
            continue;
        default:
            break;
        }
        const double available =
                qMax(layout.fontSize * 4, layout.width - block.depth * 2 * layout.fontSize);
        double text = block.width * textRoom * layout.fontSize;
        if (block.kind != PageEstimateBlock::Line)
            text += layout.indent;
        const double lines = qMax(1.0, std::ceil(text / available)) * block.scale;
        switch (block.kind) {
        case PageEstimateBlock::Text:
            value[F::ProseLines] += lines;
            value[F::Paragraphs] += 1;
            break;
        case PageEstimateBlock::Line:
            value[F::VerseLines] += lines;
            break;
        case PageEstimateBlock::Cell:
            value[F::CellLines] += lines;
            break;
        case PageEstimateBlock::Heading:
            value[F::HeadingLines] += lines;
            break;
        default:
            break;
        }
    }
    return features;
}

double PageEstimateModel::lines(const PageEstimateFeatures &features) const
{
    double sum = 0;
    for (int i = 0; i < PageEstimateFeatures::Count; ++i)
        sum += weight[i] * features.value[i];
    return sum;
}

PageEstimateModel PageEstimateModel::reference()
{
    // Fitted with the survey in tests/model/page_estimate.cpp (--fit) on
    // 22 Standard Ebooks. In order: lines of prose, verse, table cells and
    // headings, then for each paragraph, quotation, list item and table row,
    // and in pages for each image and each start on a new page.
    PageEstimateModel model;
    const double weights[PageEstimateFeatures::Count] = { 1.035, 1.061, 0.644, 4.966, -0.227,
                                                          3.937, 0.821, 2.572, 0.423, 0.425 };
    for (int i = 0; i < PageEstimateFeatures::Count; ++i)
        model.weight[i] = weights[i];
    return model;
}

const double *PageEstimateModel::strength()
{
    return referenceStrength;
}

void PageEstimateFit::add(const PageEstimateFeatures &features, double lines)
{
    const double weight = 1 / qMax(1.0, lines);
    for (int i = 0; i < PageEstimateFeatures::Count; ++i) {
        b_[i] += weight * features.value[i] * lines;
        for (int j = 0; j < PageEstimateFeatures::Count; ++j)
            a_[i][j] += weight * features.value[i] * features.value[j];
    }
    ++count_;
}

PageEstimateModel PageEstimateFit::solve(const PageEstimateModel &prior,
                                         const double *strength) const
{
    if (!count_)
        return prior;
    constexpr int n = PageEstimateFeatures::Count;
    double m[n][n + 1];
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j)
            m[i][j] = a_[i][j];
        m[i][i] += strength[i];
        m[i][n] = b_[i] + strength[i] * prior.weight[i];
    }
    // Gaussian elimination with partial pivoting. The prior keeps the
    // matrix positive definite.
    for (int i = 0; i < n; ++i) {
        int pivot = i;
        for (int r = i + 1; r < n; ++r) {
            if (std::fabs(m[r][i]) > std::fabs(m[pivot][i]))
                pivot = r;
        }
        if (pivot != i) {
            for (int c = 0; c <= n; ++c)
                std::swap(m[i][c], m[pivot][c]);
        }
        if (std::fabs(m[i][i]) < 1e-12)
            return prior;
        for (int r = 0; r < n; ++r) {
            if (r == i)
                continue;
            const double factor = m[r][i] / m[i][i];
            for (int c = i; c <= n; ++c)
                m[r][c] -= factor * m[i][c];
        }
    }
    PageEstimateModel model;
    for (int i = 0; i < n; ++i)
        model.weight[i] = m[i][n] / m[i][i];
    return model;
}

void PageMap::reset(const QVector<qint64> &bytes)
{
    items_ = QVector<Item>(bytes.size());
    for (int i = 0; i < bytes.size(); ++i)
        items_[i].bytes = qMax<qint64>(0, bytes[i]);
    observed_.clear();
    measuredCount_ = 0;
    model_ = prior_;
    kept_ = false;
    adopted_ = 0;
    update();
}

int PageMap::nextUnmeasured(int first) const
{
    if (first >= 0 && first < items_.size() && !items_[first].measured)
        return first;
    for (int i = 0; i < items_.size(); ++i) {
        if (!items_[i].measured)
            return i;
    }
    return -1;
}

qint64 PageMap::itemWords(int item) const
{
    return item >= 0 && item < items_.size() && items_[item].measured
                    && items_[item].measurement.parsed
            ? items_[item].measurement.words
            : -1;
}

bool PageMap::measured(int item) const
{
    return item >= 0 && item < items_.size() && items_[item].measured;
}

bool PageMap::complete() const
{
    if (items_.isEmpty())
        return false;
    if (allMeasured())
        return true;
    const auto counted = completeItems();
    for (int i = 0; i < items_.size(); ++i) {
        if (!items_[i].measured && !counted.contains(i))
            return false;
    }
    return true;
}

void PageMap::setMeasured(int item, const PageEstimateItem &measurement)
{
    if (item < 0 || item >= items_.size())
        return;
    auto &entry = items_[item];
    measuredCount_ += !entry.measured;
    entry.measurement = measurement;
    entry.measured = true;
    entry.features = pageEstimateFeatures(measurement, layout_);
    // Observations of the item before its measurement have no counts yet.
    const auto key = qint64(item) << 32;
    for (auto it = observed_.lowerBound(key); it != observed_.end() && it.key() >> 32 == item; ++it)
        it->ready = false;
    refit();
}

bool PageMap::setLayout(const PageEstimateLayout &layout, int sliceCharacters)
{
    sliceCharacters = qMax(0, sliceCharacters);
    if (layout == layout_ && sliceCharacters == sliceCharacters_)
        return false;
    layout_ = layout;
    sliceCharacters_ = sliceCharacters;
    observed_.clear();
    kept_ = false;
    adopted_ = 0;
    for (auto &item : items_) {
        if (item.measured)
            item.features = pageEstimateFeatures(item.measurement, layout_);
    }
    refit();
    return true;
}

void PageMap::setPrior(const PageEstimateModel &prior)
{
    prior_ = prior;
    kept_ = false;
    adopted_ = 0;
    refit();
}

void PageMap::observe(int item, int first, int last, int characters, int pages)
{
    if (item < 0 || item >= items_.size() || pages < 0)
        return;
    if (characters <= 0)
        first = 0, last = characters = 1;
    Observed observed;
    observed.first = qBound(0, first, characters);
    observed.last = qBound(observed.first, last, characters);
    observed.characters = characters;
    observed.pages = pages;
    // A part without pages, such as an empty spine item, has no last page
    // whose empty end the weights could learn.
    observed.fit = pages > 0;
    // A slice laid out after a change of the slices can overlap older ones.
    const double from = double(observed.first) / characters;
    const double to = double(observed.last) / characters;
    const auto key = qint64(item) << 32;
    for (auto it = observed_.lowerBound(key); it != observed_.end() && it.key() >> 32 == item;) {
        const double otherFrom = double(it->first) / it->characters;
        const double otherTo = double(it->last) / it->characters;
        if (otherFrom < to && from < otherTo)
            it = observed_.erase(it);
        else
            ++it;
    }
    observed_.insert((qint64(item) << 32) | quint32(observed.first), observed);
    refit();
}

QMap<int, int> PageMap::completeItems() const
{
    QMap<int, int> result;
    int item = -1, pages = 0;
    // The part of the item that the parts so far cover without a gap, or
    // negative after a gap.
    double covered = 0;
    const auto finish = [&] {
        if (item >= 0 && covered >= 1 - 1e-9)
            result.insert(item, pages);
    };
    for (auto it = observed_.cbegin(); it != observed_.cend(); ++it) {
        const int index = int(it.key() >> 32);
        if (index != item) {
            finish();
            item = index;
            pages = 0;
            covered = 0;
        }
        if (covered < 0)
            continue;
        if (double(it->first) / it->characters > covered + 1e-9) {
            covered = -1;
            continue;
        }
        covered = qMax(covered, double(it->last) / it->characters);
        pages += it->pages;
    }
    finish();
    return result;
}

bool PageMap::adoptCounted(int through)
{
    if (!keeping() || adopted_ > 0 || through < 0 || through >= items_.size())
        return false;
    const auto counted = completeItems();
    for (int i = 0; i <= through; ++i) {
        if (!counted.contains(i))
            return false;
    }
    for (int i = 0; i <= through; ++i) {
        keptPages_[i] = counted.value(i);
        keptStarts_[i + 1] = keptStarts_[i] + keptPages_[i];
    }
    for (int i = through + 1; i < items_.size(); ++i)
        keptStarts_[i + 1] = keptStarts_[i] + keptPages_[i];
    adopted_ = through + 1;
    return true;
}

void PageMap::restoreKept(const QVector<double> &pages)
{
    if (pages.isEmpty() || pages.size() != items_.size())
        return;
    adopted_ = 0;
    keptPages_ = pages;
    keptStarts_.resize(pages.size() + 1);
    keptStarts_[0] = 0;
    for (int i = 0; i < pages.size(); ++i)
        keptStarts_[i + 1] = keptStarts_[i] + qMax(0.0, pages[i]);
    kept_ = true;
}

void PageMap::restore(const QMap<int, int> &pages)
{
    for (auto it = pages.cbegin(); it != pages.cend(); ++it) {
        if (it.key() < 0 || it.key() >= items_.size() || it.value() < 0)
            continue;
        Observed observed;
        observed.last = observed.characters = 1;
        observed.pages = it.value();
        observed.fit = false;
        observed_.insert(qint64(it.key()) << 32, observed);
    }
    refit();
}

QVector<PageEstimateRecord::Part> PageMap::completeParts() const
{
    QVector<PageEstimateRecord::Part> result;
    const auto complete = completeItems();
    for (auto it = observed_.cbegin(); it != observed_.cend(); ++it) {
        const int item = int(it.key() >> 32);
        // A spine item with one part is whole: its pages are enough.
        if (!complete.contains(item) || (it->first == 0 && it->last == it->characters))
            continue;
        result.append({ item, it->first, it->last, it->characters, it->pages });
    }
    return result;
}

void PageMap::restoreParts(const QVector<PageEstimateRecord::Part> &parts)
{
    QSet<int> replaced;
    for (const auto &part : parts) {
        if (part.item < 0 || part.item >= items_.size() || part.characters <= 0 || part.first < 0
            || part.last <= part.first || part.last > part.characters || part.pages < 0)
            continue;
        const auto key = qint64(part.item) << 32;
        if (!replaced.contains(part.item)) {
            replaced.insert(part.item);
            for (auto it = observed_.lowerBound(key);
                 it != observed_.end() && it.key() >> 32 == part.item;)
                it = observed_.erase(it);
        }
        Observed observed;
        observed.first = part.first;
        observed.last = part.last;
        observed.characters = part.characters;
        observed.pages = part.pages;
        observed.fit = false;
        observed_.insert(key | quint32(part.first), observed);
    }
    if (!replaced.isEmpty())
        refit();
}

int PageMap::blockAt(const Item &item, double fraction) const
{
    // The first block that starts at or after the fraction. A block starts
    // where the block before it ends.
    const auto &blocks = item.measurement.blocks;
    if (fraction <= 0)
        return 0;
    if (fraction >= 1)
        return blocks.size();
    const double target = fraction * item.measurement.characters;
    int low = 0, high = blocks.size();
    while (low < high) {
        const int middle = (low + high) / 2;
        const double start = middle > 0 ? blocks[middle - 1].end : 0;
        if (start < target)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

double PageMap::gapLines(const Item &item, double from, double to) const
{
    if (to <= from + 1e-9)
        return 0;
    // The parts that start in the stretch, each on a new page: the spine
    // item, or its slices. The stretch starts at the start of the item or
    // where an observed part ends, which is a slice start too. The slices
    // are counted in engine characters; the measured ones are close.
    int starts = from <= 0 ? 1 : 0;
    if (sliceCharacters_) {
        const double characters = qMax<qint32>(1, item.measurement.characters);
        starts = qMax(1, int(std::ceil((to - from) * characters / sliceCharacters_ - 1e-9)));
    }
    const double start = model_.weight[PageEstimateFeatures::Starts] * layout_.linesPerPage();
    // The counts of the whole item are ready.
    const auto features = from <= 0 && to >= 1
            ? item.features
            : pageEstimateFeatures(item.measurement, layout_, blockAt(item, from),
                                   blockAt(item, to));
    return model_.lines(features) + starts * start;
}

double PageMap::estimate(int index, double fraction) const
{
    const auto &item = items_[index];
    const auto key = qint64(index) << 32;
    auto it = observed_.lowerBound(key);
    // The observed parts count with their real pages, and only the stretches
    // between them count with the model. Where the observed parts reach the
    // result is exact: a stretch without a gap adds nothing.
    double pages = 0, lines = 0, cursor = 0;
    // True when a part before `fraction` has no observation.
    bool gap = false;
    for (; it != observed_.end() && it.key() >> 32 == index; ++it) {
        const double first = double(it->first) / it->characters;
        const double last = double(it->last) / it->characters;
        if (first >= fraction)
            break;
        if (first > cursor + 1e-9) {
            gap = true;
            lines += gapLines(item, cursor, first);
        }
        if (last <= fraction + 1e-9)
            pages += it->pages;
        else
            pages += it->pages * (fraction - first) / qMax(1e-9, last - first);
        cursor = qMax(cursor, qMin(last, fraction));
    }
    if (fraction > cursor + 1e-9) {
        gap = true;
        lines += gapLines(item, cursor, fraction);
    }
    pages += qMax(0.0, lines) / layout_.linesPerPage();
    // An item takes at least one page, unless its every part was observed:
    // an empty spine item has none.
    return fraction >= 1 && gap ? qMax(1.0, pages) : pages;
}

void PageMap::refit()
{
    if (!layout_.valid()) {
        model_ = prior_;
        update();
        return;
    }
    const double page = layout_.linesPerPage();
    PageEstimateFit fit;
    for (auto it = observed_.begin(); it != observed_.end(); ++it) {
        const auto &item = items_[int(it.key() >> 32)];
        if (!item.measured || !it->fit)
            continue;
        if (!it->ready) {
            const double first = double(it->first) / it->characters;
            const double last = double(it->last) / it->characters;
            it->features = pageEstimateFeatures(item.measurement, layout_, blockAt(item, first),
                                                blockAt(item, last));
            it->features.value[PageEstimateFeatures::Starts] = page;
            it->ready = true;
        }
        fit.add(it->features, it->pages * page);
    }
    // The pull of a weight in pages is the same at every font size.
    double strength[PageEstimateFeatures::Count];
    for (int i = 0; i < PageEstimateFeatures::Count; ++i)
        strength[i] = PageEstimateModel::strength()[i];
    strength[PageEstimateFeatures::Images] *= page * page;
    strength[PageEstimateFeatures::Starts] *= page * page;
    model_ = fit.solve(prior_, strength);
    update();
}

void PageMap::update()
{
    // The items without a measurement take the pages per byte of the
    // measured ones, or else of the observed parts.
    double pages = 0, bytes = 0;
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].measured && layout_.valid()) {
            items_[i].pages = estimate(i, 1);
            pages += items_[i].pages;
            bytes += items_[i].bytes;
        }
    }
    if (bytes <= 0) {
        for (auto it = observed_.cbegin(); it != observed_.cend(); ++it) {
            const double part = double(it->last - it->first) / it->characters;
            pages += it->pages;
            bytes += items_[int(it.key() >> 32)].bytes * part;
        }
    }
    pagesPerByte_ = bytes > 0 ? pages / bytes : 0;
    starts_.resize(items_.size() + 1);
    starts_[0] = 0;
    for (int i = 0; i < items_.size(); ++i) {
        if (!items_[i].measured || !layout_.valid())
            items_[i].pages = byteEstimate(i, 1);
        starts_[i + 1] = starts_[i] + items_[i].pages;
    }
    counted_ = !items_.isEmpty() && completeItems().size() == items_.size();
    if (!kept_ && complete() && layout_.valid()) {
        kept_ = true;
        keptStarts_ = starts_;
        keptPages_.resize(items_.size());
        for (int i = 0; i < items_.size(); ++i)
            keptPages_[i] = items_[i].pages;
    }
}

double PageMap::byteEstimate(int index, double fraction) const
{
    // The observed parts count with their real pages, also an empty item
    // with none. The parts between them count with their bytes.
    const double itemPages = items_[index].bytes * pagesPerByte_;
    const auto key = qint64(index) << 32;
    double pages = 0, cursor = 0;
    // True when a part before `fraction` has no observation.
    bool gap = false;
    for (auto it = observed_.lowerBound(key); it != observed_.end() && it.key() >> 32 == index;
         ++it) {
        const double first = double(it->first) / it->characters;
        const double last = double(it->last) / it->characters;
        if (first >= fraction)
            break;
        if (first > cursor + 1e-9) {
            gap = true;
            pages += (first - cursor) * itemPages;
        }
        if (last <= fraction + 1e-9)
            pages += it->pages;
        else
            pages += it->pages * (fraction - first) / qMax(1e-9, last - first);
        cursor = qMax(cursor, qMin(last, fraction));
    }
    if (fraction > cursor + 1e-9) {
        gap = true;
        pages += (fraction - cursor) * itemPages;
    }
    // An item takes at least one page, unless its every part was observed.
    return fraction >= 1 && gap && pagesPerByte_ > 0 ? qMax(1.0, pages) : pages;
}

double PageMap::itemPages(int item) const
{
    if (item < 0 || item >= items_.size())
        return 0;
    return keeping() ? keptPages_[item] : items_[item].pages;
}

double PageMap::pagesBefore(int item) const
{
    if (item < 0 || item >= items_.size())
        return 0;
    return keeping() ? keptStarts_[item] : starts_[item];
}

double PageMap::pagesBefore(int item, int before, int characters) const
{
    if (item < 0 || item >= items_.size() || characters <= 0 || before <= 0)
        return 0;
    const double fraction = qBound(0.0, double(before) / characters, 1.0);
    // An adopted spine item has the real pages of its parts.
    if (keeping() && item >= adopted_)
        return keptPages_[item] * fraction;
    if (!items_[item].measured || !layout_.valid())
        return byteEstimate(item, fraction);
    return estimate(item, fraction);
}

double PageMap::total() const
{
    if (keeping())
        return keptStarts_.last();
    return starts_.isEmpty() ? 0 : starts_.last();
}

namespace {
bool integer(const QJsonValue &value, int low, int high, int *result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) || number < low || number > high)
        return false;
    *result = int(number);
    return true;
}
} // namespace

QJsonObject PageEstimateRecord::toJson() const
{
    QJsonArray weights;
    for (const double weight : model.weight)
        weights.append(std::round(weight * 10000) / 10000);
    // Runs of spine items that follow each other: the first item, then the
    // pages of each.
    QJsonArray runs, run;
    int next = -1;
    for (auto it = read.cbegin(); it != read.cend(); ++it) {
        if (it.key() != next) {
            if (!run.isEmpty())
                runs.append(run);
            run = QJsonArray();
            run.append(it.key());
        }
        run.append(it.value());
        next = it.key() + 1;
    }
    if (!run.isEmpty())
        runs.append(run);
    QJsonObject object;
    object["version"] = 1;
    object["weights"] = weights;
    object["layout"] = layout;
    object["spine_items"] = spineItems;
    if (item >= 0 && shown > 0 && itemShown > 0) {
        QJsonArray place;
        place.append(item);
        place.append(first);
        place.append(page);
        place.append(partPages);
        object["place"] = place;
        object["page"] = shown;
        object["item_page"] = itemShown;
    }
    if (!runs.isEmpty())
        object["read"] = runs;
    if (!parts.isEmpty()) {
        // One run for each spine item: the item, its characters, then the
        // first character, the last character and the pages of each part.
        QJsonArray runs, run;
        int item = -1;
        for (const auto &part : parts) {
            if (part.item != item) {
                if (!run.isEmpty())
                    runs.append(run);
                run = QJsonArray();
                run.append(part.item);
                run.append(part.characters);
                item = part.item;
            }
            run.append(part.first);
            run.append(part.last);
            run.append(part.pages);
        }
        if (!run.isEmpty())
            runs.append(run);
        object["parts"] = runs;
    }
    if (!kept.isEmpty() && kept.size() <= maxRead) {
        QJsonArray pages;
        for (const double value : kept)
            pages.append(std::round(value * 100) / 100);
        object["kept"] = pages;
    }
    return object;
}

bool PageEstimateRecord::fromJson(const QJsonValue &value, PageEstimateRecord *record)
{
    const auto object = value.toObject();
    int version = 0;
    if (!integer(object.value("version"), 1, 1, &version))
        return false;
    const auto weights = object.value("weights").toArray();
    if (weights.size() != PageEstimateFeatures::Count)
        return false;
    PageEstimateRecord result;
    for (int i = 0; i < PageEstimateFeatures::Count; ++i) {
        const double weight = weights[i].toDouble(NAN);
        if (!weights[i].isDouble() || !std::isfinite(weight) || std::fabs(weight) > 1000)
            return false;
        result.model.weight[i] = weight;
    }
    result.layout = object.value("layout").toString().left(64);
    if (!integer(object.value("spine_items"), 0, 1000000, &result.spineItems))
        result.spineItems = 0;
    const auto place = object.value("place").toArray();
    int item = -1, first = 0, page = 0, partPages = 0, shown = 0, itemShown = 0;
    if (place.size() == 4 && integer(place[0], 0, 1000000, &item)
        && integer(place[1], 0, INT_MAX, &first) && integer(place[2], 0, 1000000, &page)
        && integer(place[3], 1, 1000000, &partPages) && page < partPages
        && integer(object.value("page"), 1, 10000000, &shown)
        && integer(object.value("item_page"), 1, 10000000, &itemShown)) {
        result.item = item;
        result.first = first;
        result.page = page;
        result.partPages = partPages;
        result.shown = shown;
        result.itemShown = itemShown;
    }
    for (const auto &entry : object.value("read").toArray()) {
        const auto run = entry.toArray();
        int start = 0;
        if (run.size() < 2 || !integer(run[0], 0, 1000000, &start))
            continue;
        for (int i = 1; i < run.size() && result.read.size() < maxRead; ++i) {
            int pages = 0;
            if (!integer(run[i], 0, 1000000, &pages))
                break;
            result.read.insert(start + i - 1, pages);
        }
    }
    for (const auto &entry : object.value("parts").toArray()) {
        const auto run = entry.toArray();
        int item = 0, characters = 0;
        if (run.size() < 5 || run.size() % 3 != 2 || !integer(run[0], 0, 1000000, &item)
            || !integer(run[1], 1, INT_MAX, &characters))
            continue;
        QVector<Part> parts;
        for (int i = 2; i + 2 < run.size(); i += 3) {
            Part part;
            part.item = item;
            part.characters = characters;
            if (!integer(run[i], 0, characters, &part.first)
                || !integer(run[i + 1], part.first + 1, characters, &part.last)
                || !integer(run[i + 2], 0, 1000000, &part.pages)) {
                parts.clear();
                break;
            }
            parts.append(part);
        }
        if (result.parts.size() + parts.size() <= maxParts)
            result.parts += parts;
    }
    const auto kept = object.value("kept").toArray();
    if (kept.size() <= maxRead) {
        for (const auto &entry : kept) {
            const double pages = entry.toDouble(NAN);
            if (!entry.isDouble() || !std::isfinite(pages) || pages < 0 || pages > 1000000) {
                result.kept.clear();
                break;
            }
            result.kept.append(pages);
        }
    }
    *record = result;
    return true;
}

int PageAnchor::place(int estimate, Step step, bool itemStart)
{
    estimate = qMax(1, estimate);
    if (shown_ <= 0 || step == Jump) {
        shown_ = estimate;
    } else if (step == Forward) {
        shown_ = itemStart ? qMax(shown_ + 1, estimate) : shown_ + 1;
    } else if (step == Backward) {
        shown_ = qMax(1, shown_ - 1);
    }
    return shown_;
}
} // namespace kobalt
