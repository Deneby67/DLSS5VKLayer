#include "inline_nr_panel.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

InlineNrPanel::InlineNrPanel(const QString& controller,QWidget* parent,bool poll)
    : QWidget(parent),controllerPath(controller) {
    setObjectName("inlineNrPanel");
    auto* layout=new QVBoxLayout(this);
    auto* title=new QLabel("DLSS 5 NR → DLSS · RDR2 Vulkan",this);
    title->setWordWrap(true);
    QFont font=title->font();font.setBold(true);title->setFont(font);
    layout->addWidget(title);
    auto* description=new QLabel("Uses the game's motion vectors and applies NR before native DLSS upscaling. Experimental integration.",this);
    description->setWordWrap(true);layout->addWidget(description);
    enabled=new QCheckBox("Enable NR before DLSS",this);
    enabled->setObjectName("inlineNrEnabled");enabled->setEnabled(false);
    enabled->setToolTip("Applies only to the connected game session. Every new game starts with NR off.");
    layout->addWidget(enabled);
    state=new QLabel("Checking game connection…",this);state->setObjectName("inlineNrState");state->setWordWrap(true);
    reason=new QLabel(this);reason->setObjectName("inlineNrReason");reason->setWordWrap(true);
    reason->setTextFormat(Qt::PlainText);reason->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(state);layout->addWidget(reason);
    openLog=new QPushButton("Open game NR log",this);openLog->setEnabled(false);layout->addWidget(openLog,0,Qt::AlignLeft);
    auto* note=new QLabel("Load a scene with native DLSS, then enable NR here.\n\nThis mode runs inside the game. Runner, Start helper and the other tabs configure the separate external helper.\n\nClosing this window leaves the current session unchanged. A new game starts with NR off.",this);
    note->setWordWrap(true);layout->addWidget(note);layout->addStretch();
    process=new QProcess(this);
    timeout=new QTimer(this);timeout->setSingleShot(true);timeout->setInterval(5000);
    connect(timeout,&QTimer::timeout,this,[this]{process->kill();});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus exit){
        timeout->stop();
        auto doc=QJsonDocument::fromJson(process->readAllStandardOutput());
        if(!doc.isObject() || exit!=QProcess::NormalExit) {
            applyStatus({{"status","error"},{"reason","Game controller did not respond. Connection will be checked again."}});
        } else {
            auto status=doc.object();
            if(code!=0)status["status"]="error";
            applyStatus(status);
        }
    });
    connect(process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart) {
            timeout->stop();applyStatus({{"status","error"},{"reason","Game controller is unavailable. Reinstall the GUI integration."}});
        }
    });
    connect(enabled,&QCheckBox::clicked,this,[this](bool on){request(on?"on":"off");});
    connect(openLog,&QPushButton::clicked,this,[this]{if(!logPath.isEmpty())QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));});
    if(poll) {
        auto* timer=new QTimer(this);timer->setInterval(1500);
        connect(timer,&QTimer::timeout,this,[this]{request("status");});timer->start();
        QTimer::singleShot(0,this,[this]{request("status");});
    }
}

void InlineNrPanel::request(const QString& mode) {
    if(process->state()!=QProcess::NotRunning)return;
    if(controllerPath.isEmpty()) {
        applyStatus({{"status","not_installed"},{"reason","Install the NR-before-DLSS GUI controller."}});return;
    }
    if(mode!="status" && (!ready || (mode=="on" && !canEnable)))return;
    QStringList args{controllerPath,mode=="status"?"auto":logPath,mode};
    if(mode!="status")args<<"--token"<<token;
    enabled->setEnabled(false);
    process->start("python3",args);timeout->start();
}

void InlineNrPanel::applyStatus(const QJsonObject& s) {
    const QString status=s.value("status").toString();
    const bool requested=s.value("requested_on").toBool();
    ready=s.value("ready").toBool();canEnable=s.value("can_enable").toBool();
    logPath=s.value("log").toString();token=s.value("token").toString();
    QSignalBlocker blocked(enabled);enabled->setChecked(requested);
    enabled->setEnabled(ready && (canEnable || requested));
    QString label="NR unavailable";
    if(status=="off")label="Connected · NR off";
    else if(status=="pending")label=requested?"Enabling NR…":"Disabling NR…";
    else if(status=="starting")label="NR requested · waiting for processing";
    else if(status=="recording")label="NR → DLSS commands recorded";
    else if(status=="bypassed")label="NR bypassed · original DLSS continues";
    else if(status=="restart")label="Game restart required";
    else if(status=="waiting" || status=="waiting_sr")label="Waiting for RDR2 / native DLSS";
    else if(status=="not_installed")label="Game integration not installed";
    else if(status=="error")label="Connection error";
    state->setText(label);reason->setText(s.value("reason").toString());
    openLog->setEnabled(!logPath.isEmpty());
}
