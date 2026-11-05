#pragma once

#include "private_text.h"
#include <QPalette>
#include <QRectF>
#include <memory>

class QPainter;

namespace kobalt::ui {
// Qt parses HTML and styles. FreeType, HarfBuzz and FriBidi own text metrics,
// line breaks, pagination and glyphs, including fonts unavailable to Qt.
class PrivateRichText {
public:
    PrivateRichText();
    ~PrivateRichText();
    void setHtml(const QString &html);
    void setPlainText(const QString &text);
    void setFont(const QFont &font);
    QFont font() const;
    void layout(int width, int pageHeight = 0, int minimumLineHeight = 0);
    int pages() const;
    int height() const;
    int naturalWidth() const;
    QVector<QRectF> lineRectangles() const;
    void draw(QPainter &painter, const QRectF &clip, const QPalette &palette) const;

private:
    struct Data;
    std::unique_ptr<Data> data;
};
}
