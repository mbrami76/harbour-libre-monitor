# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Static library with the logic of the user interface: everything that
# decides what the screens say, with no QML and no Silica, so the host build can test it
#. The QML pages only present what these classes return
# (REQ-UI-07). Not installed; the application links it into its one binary.
TEMPLATE = lib
TARGET = lmui
CONFIG += staticlib
QT = core

include(../../common.pri)

HEADERS += \
    accounterrors.h \
    accountbackend.h \
    alertsbackend.h \
    alertview.h \
    alertmodel.h \
    fakealertsbackend.h \
    accountsetup.h \
    fakeaccountbackend.h \
    appsettings.h \
    backend.h \
    band.h \
    fakebackend.h \
    plotcontroller.h \
    subjectmodel.h \
    uiconstants.h \
    direction.h \
    glucoseformat.h \
    subjectstate.h \
    configstore.h \
    configsettings.h \
    thresholdview.h \
    thresholdeditor.h \
    monitoringinfo.h \
    coverrank.h \
    settingsstore.h \
    plotgeometry.h \
    plotreduce.h \
    plotview.h \
    sensorview.h \
    maintenancebackend.h \
    fakemaintenancebackend.h \
    storageview.h \
    storagecontroller.h \
    purgeview.h \
    purgecontroller.h \
    reportcontroller.h

SOURCES += \
    accounterrors.cpp \
    alertview.cpp \
    alertmodel.cpp \
    fakealertsbackend.cpp \
    accountsetup.cpp \
    fakeaccountbackend.cpp \
    fakebackend.cpp \
    plotcontroller.cpp \
    subjectmodel.cpp \
    direction.cpp \
    glucoseformat.cpp \
    subjectstate.cpp \
    configstore.cpp \
    configsettings.cpp \
    thresholdview.cpp \
    thresholdeditor.cpp \
    monitoringinfo.cpp \
    coverrank.cpp \
    settingsstore.cpp \
    plotgeometry.cpp \
    plotreduce.cpp \
    plotview.cpp \
    sensorview.cpp \
    fakemaintenancebackend.cpp \
    storageview.cpp \
    storagecontroller.cpp \
    purgeview.cpp \
    purgecontroller.cpp \
    reportcontroller.cpp
