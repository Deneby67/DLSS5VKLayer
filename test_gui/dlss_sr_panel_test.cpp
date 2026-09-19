#include "dlss_sr_panel.h"
#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
int main(int argc,char** argv) {
    QApplication app(argc,argv);int failures=0;
    auto check=[&](bool v){if(!v)++failures;};
    QTemporaryDir tmp;QString path=tmp.path()+"/control.py";
    QFile file(path);check(file.open(QIODevice::WriteOnly));
    file.write("import sys,json\nfrom pathlib import Path\nPath(__file__).with_suffix('.args').write_text(json.dumps(sys.argv[1:]))\nprint(json.dumps(dict(installed=True,dll_version='310.9.1.0',config=dict(model=sys.argv[3],preset=sys.argv[5]),message='Saved. Restart RDR2.')))\n");file.close();
    DlssSrPanel panel(path,nullptr,false);panel.resize(620,580);
    auto* model=panel.findChild<QComboBox*>("srModel");auto* preset=panel.findChild<QComboBox*>("srPreset");
    auto* apply=panel.findChild<QPushButton*>("srApply");auto* state=panel.findChild<QLabel*>("srState");
    QJsonObject saved{{"installed",true},{"dll_version","310.9.1.0"},{"config",QJsonObject{{"model","transformer"},{"preset","K"}}},{"message","Ready"}};
    panel.applyStatus(saved);check(model->currentData()=="transformer" && preset->currentData()=="K" && !apply->isEnabled());
    model->setCurrentIndex(model->findData("cnn"));check(preset->count()==2 && preset->currentData()=="E");
    preset->setCurrentIndex(preset->findData("F"));panel.applyStatus(saved);check(preset->currentData()=="F" && apply->isEnabled());
    apply->click();QElapsedTimer time;time.start();
    while(!model->isEnabled() && time.elapsed()<5000){app.processEvents();QThread::msleep(5);}
    check(model->isEnabled() && preset->currentData()=="F" && state->text().contains("Restart"));
    QFile args(tmp.path()+"/control.args");check(args.open(QIODevice::ReadOnly));check(args.readAll()=="[\"save\", \"--model\", \"cnn\", \"--preset\", \"F\"]");
    panel.show();app.processEvents();if(argc>1)check(panel.grab().save(argv[1]));
    printf("DLSS SR panel: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
