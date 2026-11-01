# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_uilogic
QT = core testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useUi()
useCore()

SOURCES += tst_uilogic.cpp
