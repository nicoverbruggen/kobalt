#pragma once
#include "engine_fixtures.h"

namespace kobalt::test {
inline BookFiles columnPairsBook(int before, int after)
{
    const auto columns = [](const QByteArray &name, int count) {
        QByteArray body;
        for (int column = 0; column < count; ++column)
            body += "<p style='page-break-before:always'>" + name + ' ' + QByteArray::number(column)
                    + ". " + QByteArray("A short column. ").repeated(3) + "</p>";
        return publisherHtml(body);
    };
    auto files = publisherPackage("en", "Column pairs",
                                  { { "first.xhtml", columns("First", before) },
                                    { "second.xhtml", columns("Second", after) } },
                                  "<itemref idref='first.xhtml'/><itemref idref='second.xhtml'/>");
    files["book.opf"].replace(
            "</manifest><spine>",
            "<item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
            "</manifest><spine toc='ncx'>");
    files["toc.ncx"] =
            "<ncx xmlns='http://www.daisy.org/z3986/2005/ncx/' version='2005-1'><head/>"
            "<docTitle><text>Column pairs</text></docTitle><navMap>"
            "<navPoint id='first' playOrder='1'><navLabel><text>First</text></navLabel>"
            "<content src='first.xhtml'/></navPoint>"
            "<navPoint id='second' playOrder='2'><navLabel><text>Second</text></navLabel>"
            "<content src='second.xhtml'/></navPoint></navMap></ncx>";
    return files;
}
} // namespace kobalt::test
