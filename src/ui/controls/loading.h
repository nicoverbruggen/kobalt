#pragma once
#include <QWidget>

class QTimer;
namespace kobalt::ui {
// An opt-in status control. Hidden controls do no timer work; showing one
// restarts the requested delay. The caller owns when loading starts and stops.
class LoadingIndicator final : public QWidget {
public:
    enum class Style { Dots, Squares, Logo };
    explicit LoadingIndicator(Style style, QWidget *parent = nullptr);
    void start(int delayMilliseconds = 1000);
    void stop();
    bool isRunning() const {
        return running;
    }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    const Style style;
    QTimer *timer;
    int delay = 1000, filled = 0;
    bool running = false, revealed = false;
    int count() const;
    int interval() const;
    void restart();
    void advance();
};
}
