#pragma once
#include <QWidget>
#include <QJsonObject>
class QComboBox;
class QLabel;
class QPushButton;
class QProcess;
class QTimer;
class DlssSrPanel : public QWidget {
public:
    explicit DlssSrPanel(const QString& controller,QWidget* parent=nullptr,bool poll=true);
    void applyStatus(const QJsonObject& status);
private:
    void presets();
    void request(bool save);
    QString controller;
    QComboBox *model,*preset;
    QLabel *version,*state;
    QPushButton* apply;
    QProcess* process;
    QTimer* timeout;
    bool dirty=false,installed=false;
};
