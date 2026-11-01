# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_tickrhythm
QT = core sql testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()
INCLUDEPATH += $$PWD/../support $$PWD/../tst_adapterlink

HEADERS += ../support/modtest.h ../support/schedtest.h ../tst_adapterlink/fakeadapter.h
SOURCES += tst_tickrhythm.cpp
