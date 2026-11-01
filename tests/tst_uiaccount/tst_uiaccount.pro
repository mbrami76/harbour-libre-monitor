# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_uiaccount
QT = core testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useUi()
useCore()

# The texts are read from the source tree: they are what is checked.
DEFINES += LM_TRANSLATIONS_DIR=\\\"$$PWD/../../app/translations\\\"
DEFINES += LM_QML_COMPONENTS_DIR=\\\"$$PWD/../../app/qml/components\\\"

SOURCES += tst_uiaccount.cpp
