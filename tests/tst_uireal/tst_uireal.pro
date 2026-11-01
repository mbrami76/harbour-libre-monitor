# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_uireal
QT = core sql dbus network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useUiReal()
useDaemon()
useLibre()
useUi()
useCore()

SOURCES += tst_uireal.cpp
