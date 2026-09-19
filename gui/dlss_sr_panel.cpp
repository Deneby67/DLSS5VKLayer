#include "dlss_sr_panel.h"
#include <QComboBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

DlssSrPanel::DlssSrPanel(const QString& path,QWidget* parent,bool poll):QWidget(parent),controller(path) {
    auto* layout=new QVBoxLayout(this);
    auto* title=new QLabel("DLSS Super Resolution · RDR2 Vulkan",this);
    auto font=title->font();font.setBold(true);title->setFont(font);layout->addWidget(title);
    version=new QLabel(this);version->setObjectName("srVersion");version->setWordWrap(true);layout->addWidget(version);
    auto* form=new QFormLayout;layout->addLayout(form);
    model=new QComboBox(this);model->setObjectName("srModel");
    model->addItem("Transformer","transformer");model->addItem("Legacy CNN","cnn");model->addItem("Game default","game");
    form->addRow("Model",model);
    preset=new QComboBox(this);preset->setObjectName("srPreset");form->addRow("Preset",preset);presets();
    apply=new QPushButton("Save for next game launch",this);apply->setObjectName("srApply");apply->setEnabled(false);layout->addWidget(apply);
    state=new QLabel("Checking DLSS installation…",this);state->setObjectName("srState");state->setWordWrap(true);state->setTextFormat(Qt::PlainText);layout->addWidget(state);
    auto* note=new QLabel("Uses the latest validated NVIDIA DLSS SR DLL for both model families. No DLL downgrade is needed for CNN E/F.\n\nK is the standard Transformer preset; J trades some ghosting for more flicker. L/M use newer Transformer models and can cost more GPU time. E/F are legacy CNN presets deprecated by NVIDIA.\n\nThe selected preset applies to all DLSS quality modes. Choose Quality, Balanced or Performance in the game's graphics settings: these modes also control the engine's render resolution.\n\nRestart RDR2 after saving. F2 controls NR before DLSS, independently of these settings. Start helper is not needed.",this);
    note->setWordWrap(true);layout->addWidget(note);layout->addStretch();
    process=new QProcess(this);timeout=new QTimer(this);timeout->setSingleShot(true);timeout->setInterval(5000);
    connect(timeout,&QTimer::timeout,process,&QProcess::kill);
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus exit){
        timeout->stop();auto data=QJsonDocument::fromJson(process->readAllStandardOutput());
        if(code || exit!=QProcess::NormalExit || !data.isObject()) {
            state->setText(data.object().value("error").toString("DLSS controller did not respond."));
            dirty=true;model->setEnabled(true);preset->setEnabled(true);apply->setEnabled(installed);return;
        }
        applyStatus(data.object());
    });
    connect(process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(e==QProcess::FailedToStart){timeout->stop();dirty=true;model->setEnabled(true);preset->setEnabled(true);apply->setEnabled(installed);state->setText("DLSS controller is unavailable.");}});
    connect(model,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{presets();dirty=true;apply->setEnabled(installed);state->setText("Unsaved model/preset selection.");});
    connect(preset,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{dirty=true;apply->setEnabled(installed);state->setText("Unsaved model/preset selection.");});
    connect(apply,&QPushButton::clicked,this,[this]{request(true);});
    if(poll) {
        auto* timer=new QTimer(this);timer->setInterval(3000);connect(timer,&QTimer::timeout,this,[this]{request(false);});timer->start();
        QTimer::singleShot(0,this,[this]{request(false);});
    }
}
void DlssSrPanel::presets() {
    QSignalBlocker block(preset);preset->clear();
    const auto family=model->currentData().toString();
    if(family=="transformer") {
        preset->addItem("K — Transformer","K");preset->addItem("J — Transformer","J");
        preset->addItem("M — Transformer 2","M");preset->addItem("L — Transformer 2","L");
    } else if(family=="cnn") {
        preset->addItem("E — legacy CNN","E");preset->addItem("F — legacy CNN","F");
    } else preset->addItem("Game default","auto");
}
void DlssSrPanel::request(bool save) {
    if(process->state()!=QProcess::NotRunning)return;
    QStringList args{controller,save?"save":"status"};
    if(save){args<<"--model"<<model->currentData().toString()<<"--preset"<<preset->currentData().toString();dirty=false;}
    apply->setEnabled(false);model->setEnabled(!save);preset->setEnabled(!save);
    process->start("python3",args);timeout->start();
}
void DlssSrPanel::applyStatus(const QJsonObject& s) {
    installed=s.value("installed").toBool();model->setEnabled(true);preset->setEnabled(true);
    version->setText(installed?"NVIDIA DLSS SR "+s.value("dll_version").toString()+" · latest validated release":"DLSS SR integration not installed");
    if(!dirty) {
        auto config=s.value("config").toObject();QSignalBlocker blockModel(model),blockPreset(preset);
        model->setCurrentIndex(qMax(0,model->findData(config.value("model").toString())));presets();
        preset->setCurrentIndex(qMax(0,preset->findData(config.value("preset").toString())));
        state->setText(s.value("message").toString());
    }
    apply->setEnabled(installed && dirty);
}
