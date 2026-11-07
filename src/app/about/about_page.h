#pragma once
#include <QWidget>
#include <QVector>

class QLabel;

class QPushButton;

namespace kobalt {

// The same build information and content in reader Settings and the Library.
class AboutPage final : public QWidget
{
public:
    explicit AboutPage(QWidget *parent = nullptr);
    int fitWidth(int width);
    int descriptionTop() const;
    QVector<int> pageStarts(int availableHeight) const;
    QPushButton *acknowledgmentsButton;

private:
    QLabel *description;
    QWidget *buildStart;
};

} // namespace kobalt
