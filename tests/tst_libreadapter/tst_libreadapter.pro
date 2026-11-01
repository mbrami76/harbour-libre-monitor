# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_libreadapter
QT = core network testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useLibre()
useCore()

SOURCES += tst_libreadapter.cpp
