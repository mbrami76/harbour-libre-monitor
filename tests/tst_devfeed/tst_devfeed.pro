# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_devfeed
QT = core sql dbus network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useDaemon()
useLibre()
useCore()
INCLUDEPATH += $$PWD/../support

DEFINES += LM_TRANSLATIONS_DIR=\\\"$$PWD/../../app/translations\\\"

HEADERS += ../support/notifiertest.h
SOURCES += tst_devfeed.cpp
