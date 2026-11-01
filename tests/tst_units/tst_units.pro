# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_units
QT = core testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()

SOURCES += tst_units.cpp
