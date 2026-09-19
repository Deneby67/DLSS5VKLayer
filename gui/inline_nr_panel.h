#pragma once
#include <QWidget>
#include <QJsonObject>

class QCheckBox;
class QLabel;
class QPushButton;
class QProcess;
class QTimer;

class InlineNrPanel : public QWidget {
public:
    explicit InlineNrPanel(const QString& controller, QWidget* parent=nullptr, bool poll=true);
    void applyStatus(const QJsonObject& status);
private:
    void request(const QString& mode);
    QString controllerPath, logPath, token;
    QCheckBox* enabled;
    QLabel* state;
    QLabel* reason;
    QPushButton* openLog;
    QProcess* process;
    QTimer* timeout;
    bool canEnable=false, ready=false;
};
