# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_storemigration
QT = core sql testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()

# The fixture stores of earlier schema versions (REQ-STO-10), read from the source tree.
DEFINES += LM_STORE_FIXTURES_DIR=\\\"$$PWD/../fixtures/store\\\"

SOURCES += tst_storemigration.cpp
