# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_daemon
QT = core sql dbus network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useDaemon()
useLibre()
useCore()
INCLUDEPATH += $$PWD/../support
DEFINES += LM_TRANSLATIONS_DIR=\\\"$$PWD/../../app/translations\\\"

HEADERS += ../support/scriptedadapter.h ../support/notifiertest.h ../support/silenttest.h
SOURCES += tst_daemon.cpp
