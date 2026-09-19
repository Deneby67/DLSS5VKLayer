#include "inline_nr_panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QJsonObject>
#include <cstdio>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    InlineNrPanel panel("",nullptr,false);panel.resize(580,390);
    auto* toggle=panel.findChild<QCheckBox*>("inlineNrEnabled");
    auto* state=panel.findChild<QLabel*>("inlineNrState");
    int failures=0;
    auto check=[&](bool ok){if(!ok)++failures;};
    panel.applyStatus({{"status","off"},{"ready",true},{"can_enable",true},{"requested_on",false},{"reason","Ready. NR is off for this session."}});
    check(toggle->isEnabled() && !toggle->isChecked());
    panel.applyStatus({{"status","pending"},{"ready",true},{"can_enable",true},{"requested_on",true}});
    check(toggle->isChecked() && state->text().contains("Enabling"));
    panel.applyStatus({{"status","blocked"},{"ready",true},{"can_enable",false},{"requested_on",true}});
    check(toggle->isEnabled()); // Off remains available after an NR failure.
    panel.applyStatus({{"status","restart"},{"ready",true},{"can_enable",false},{"requested_on",false},{"reason","Integration updated. Save and restart RDR2 to load the installed DLLs."}});
    check(!toggle->isEnabled() && state->text().contains("restart"));
    panel.show();app.processEvents();
    if(argc>1)check(panel.grab().save(argv[1]));
    // Exercise the actual asynchronous toggle transport without touching a game.
    QTemporaryDir tmp;
    const QString script=tmp.path()+"/control.py";
    QFile file(script);check(file.open(QIODevice::WriteOnly));
    file.write("import json,sys\nfrom pathlib import Path\nPath(__file__).with_suffix('.args').write_text(json.dumps(sys.argv[1:]))\nprint(json.dumps(dict(status='pending',ready=True,can_enable=True,requested_on=sys.argv[2]=='on',token='77',log='/test/adapter.log')))\n");file.close();
    InlineNrPanel transport(script,nullptr,false);
    transport.applyStatus({{"status","off"},{"ready",true},{"can_enable",true},{"requested_on",false},{"token","77"},{"log","/test/adapter.log"}});
    auto* button=transport.findChild<QCheckBox*>("inlineNrEnabled");button->click();
    QElapsedTimer deadline;deadline.start();
    while(!button->isEnabled() && deadline.elapsed()<5000){app.processEvents();QThread::msleep(5);}
    check(button->isEnabled() && button->isChecked());
    QFile arguments(tmp.path()+"/control.args");check(arguments.open(QIODevice::ReadOnly));
    check(arguments.readAll()=="[\"/test/adapter.log\", \"on\", \"--token\", \"77\"]");
    std::printf("inline panel: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
