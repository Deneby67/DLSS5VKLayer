QT += widgets
CONFIG += c++17 release
TARGET = dlssnr_gui
TEMPLATE = app

INCLUDEPATH += $$PWD/../common

SOURCES += \
    ../layer_linux/src/hotkey.cpp \
    main.cpp \
    mainwindow.cpp \
    inline_nr_panel.cpp \
    dlss_sr_panel.cpp \
    passdialog.cpp \
    shm_binder.cpp \
    ../common/runner_discovery.cpp

HEADERS += \
    mainwindow.h \
    inline_nr_panel.h \
    dlss_sr_panel.h \
    passdialog.h \
    shm_binder.h \
    ../common/runner_discovery.h
