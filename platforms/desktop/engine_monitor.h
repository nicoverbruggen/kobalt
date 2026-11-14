#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>

namespace kobalt::desktop {
// Desktop-only consumer of diagnostic events. Producers never wait for this window.
class EngineMonitor final : public QWidget
{
public:
    explicit EngineMonitor(QWidget *parent = nullptr);
    ~EngineMonitor() override;
    std::function<void(const QJsonObject &)> observer() const;
    void refresh();

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    struct State;
    std::unique_ptr<State> state;
};
} // namespace kobalt::desktop
