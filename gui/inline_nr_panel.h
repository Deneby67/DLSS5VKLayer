#pragma once
#include <QWidget>
#include <QJsonObject>

class QCheckBox;
class QLabel;
class QPushButton;
class QProcess;
class QTimer;
struct ShmHeader;

class InlineNrPanel : public QWidget {
public:
    explicit InlineNrPanel(const QString& controller, QWidget* parent=nullptr, bool poll=true, ShmHeader* settings=nullptr);
    void setRenderingEnabled(bool on);
    void applyStatus(const QJsonObject& status);
private:
    void request(const QString& mode);
    ShmHeader* settings;
    QString controllerPath, logPath, token, pendingMode, pendingToken, pendingLog;
    QCheckBox* enabled;
    QLabel* state;
    QLabel* reason;
    QPushButton* openLog;
    QProcess* process;
    QTimer* timeout;
    bool canEnable=false, ready=false;
};
