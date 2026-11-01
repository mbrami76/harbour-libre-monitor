# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_service
QT = core sql testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()
INCLUDEPATH += $$PWD/../support

HEADERS += ../support/servicetest.h
SOURCES += tst_service.cpp
