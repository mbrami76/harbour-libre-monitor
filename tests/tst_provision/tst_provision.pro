# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_provision
QT = core sql dbus network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useDaemon()
useLibre()
useCore()

SOURCES += tst_provision.cpp
