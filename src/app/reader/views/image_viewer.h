#pragma once
#include "ui/controls/panels.h"
#include <QImage>
#include <QJsonObject>
#include <functional>

namespace kobalt {
class ImageCanvas;

class ImageViewer final : public ui::Panel
{
public:
    explicit ImageViewer(QWidget *parent);
    void open();
    bool hasImage() const;
    void setImage(QImage image, const QJsonObject &view = {});
    void closeImage(bool cleanPage = true);
    std::function<void(bool)> dismissed;
    std::function<void(QString, QJsonObject)> rememberView;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    ImageCanvas *canvas;
    QString imageId;
};
} // namespace kobalt
