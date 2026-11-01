# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_discovery
QT = core sql dbus network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useDaemon()
useLibre()
useCore()
INCLUDEPATH += $$PWD/../support

HEADERS += ../support/scriptedadapter.h ../support/servicetest.h
SOURCES += tst_discovery.cpp
