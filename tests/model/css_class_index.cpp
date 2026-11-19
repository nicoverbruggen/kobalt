#include "check.h"
#include "reader_inputs.h"
#include "engine_fixtures.h"
#include "engine/blaze/adapter/engine_document.h"
#include <lvtinydom.h>
#include <fb2def.h>
#include <QGuiApplication>

namespace {
void checkClassSelectorIndex()
{
    ldomDocument document;
    auto *root = document.getRootNode();
    auto *ancestor = root->insertChildElement(document.getElementNameIndex("section"));
    ancestor->setAttributeValue(0, attr_class, U"chapter");
    auto *node = ancestor->insertChildElement(document.getElementNameIndex("p"));
    node->setAttributeValue(0, attr_class, U"item Aa accent accent ");
    auto *pseudo = node->insertChildElement(el_pseudoElem);
    pseudo->setAttributeValue(0, attr_Before, U"");
    auto *clone = root->insertChildElement(el_cloneNode);
    lString32 source;
    source << fmt::decimal(node->getDataIndex());
    clone->setAttributeValue(0, attr_SrcId, source.c_str());

    const char *one = ".item {", *two = ".other {", *three = "* {";
    LVCssSelector first;
    auto *other = new LVCssSelector;
    auto *universal = new LVCssSelector;
    first.setNext(other);
    other->setNext(universal);
    require(first.parse(one, &document) && other->parse(two, &document)
                    && universal->parse(three, &document),
            "Cannot parse class index candidates");
    LVCssClassIndex index;
    index.rebuild(&first);
    lUInt32 classes[] = { lString32(U"item").getHash(), lString32(U"item").getHash() };
    LVCssClassIndex::Cursor cursor(index, classes, 2);
    require(cursor.next() == &first && cursor.next() == universal && !cursor.next(),
            "Class index visited an irrelevant selector or visited a selector twice");

    LVStyleSheet scan(&document);
    auto &indexed = *document.getStyleSheet();
    // Aa and BB have the same class hash. The full matcher decides between them.
    const char *css = "* {color:#333} .Aa {color:#111} .BB {color:#999}"
                      ".item {background-color:#111} :is(.accent) {background-color:#222}"
                      ".accent {background-color:#333} .chapter > .item {text-align:right}"
                      "p.item {background-color:#444} .item::before {color:#123}";
    require(scan.parse(css) && indexed.parse(css), "Cannot parse the class selector fixture");
    auto compare = [&](LVStyleSheet &a, LVStyleSheet &b, ldomNode *target, int color) {
        css_style_rec_t expected, actual;
        a.apply(target, &expected);
        b.apply(target, &actual);
        require(actual == expected && actual.color.value == color,
                "Class index changed the cascade");
        std::unique_ptr<css_style_rec_t> beforeExpected(expected.pseudo_elem_before_style);
        std::unique_ptr<css_style_rec_t> beforeActual(actual.pseudo_elem_before_style);
        require(bool(beforeActual) == bool(beforeExpected)
                        && (!beforeActual || *beforeActual == *beforeExpected),
                "Class index changed the generated style");
    };
    compare(scan, indexed, node, 0x111111);
    compare(scan, indexed, clone, 0x111111);
    compare(scan, indexed, pseudo, 0x112233);
    compare(scan, indexed, ancestor, 0x333333);
    LVStyleSheet copied(indexed);
    compare(scan, copied, node, 0x111111);

    scan.push();
    indexed.push();
    LVStyleSheet imported(&document);
    require(imported.parse(".item {color:#456}"), "Cannot parse imported class rule");
    scan.merge(imported);
    indexed.merge(imported);
    compare(scan, indexed, node, 0x445566);
    require(scan.pop() && indexed.pop(), "Cannot restore CSS scope");
    compare(scan, indexed, node, 0x111111);
    scan.clear();
    indexed.clear();
    require(scan.parse(".accent {color:#789}") && indexed.parse(".accent {color:#789}"),
            "Cannot replace the cleared stylesheet");
    compare(scan, indexed, node, 0x778899);
    scan.clear();
    indexed.clear();
    const char *equal = ".item {-cr-hint:no-presentational;color:#123}"
                        "p {-cr-hint:no-presentational;color:#456}"
                        ".accent {-cr-hint:no-presentational;color:#789}";
    require(scan.parse(equal) && indexed.parse(equal), "Cannot parse zero-weight selector rules");
    compare(scan, indexed, node, 0x778899);
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    auto chapter = kobalt::test::publisherHtml(
            "<section class='chapter'><p class='lead accent'>A class selector test.</p>"
            "<p class='body'>The next line retains its publisher style.</p></section>");
    chapter.replace("</head>", "<link rel='stylesheet' href='book.css'/></head>");
    const QByteArray css = ".unused{display:none}.lead{font-size:160%}"
                           ".chapter>.accent{color:#912}.lead::before{content:'Initial '}"
                           "p.body{text-indent:2em}.body{font-style:italic}";
    QVector<QImage> pages;
    QStringList starts;
    for (bool indexed : { false, true }) {
        auto rules = css;
        // Attribute word matching has the same specificity as class matching
        // and goes through the ordinary selector chain.
        if (!indexed) {
            for (const char *name : { "unused", "lead", "chapter", "accent", "body" })
                rules.replace(QByteArray(".") + name, QByteArray("[class~='") + name + "']");
        }
        const auto files = kobalt::test::publisherPackage(
                "en", "Publisher class selectors",
                { { "first.xhtml", chapter }, { "second.xhtml", chapter }, { "book.css", rules } },
                "<itemref idref='first.xhtml'/><itemref idref='second.xhtml'/>");
        QMap<QString, qint64> sizes;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            sizes.insert(it.key(), it.value().size());
        QString error;
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        auto resources = std::make_shared<kobalt::ResourceCache>(
                files.keys(),
                [&files](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                    const auto &bytes = files[name];
                    return sink(bytes.constData(), bytes.size());
                },
                std::make_shared<std::atomic_bool>(false));
        require(model.openResources(resources, sizes, QSize(550, 800), {}, &error),
                qPrintable(error));
        if (indexed) {
            checkClassSelectorIndex();
            require(model.pages() == pages.size(), "Class index changed the publisher page count");
        }
        for (int page = 0; page < model.pages(); ++page) {
            require(model.go(page), "Cannot open publisher fixture page");
            if (indexed) {
                require(model.position() == starts[page] && model.image(page) == pages[page],
                        "Class index changed publisher page starts or pixels");
            } else {
                starts.append(model.position());
                pages.append(model.image(page));
            }
        }
    }
}
