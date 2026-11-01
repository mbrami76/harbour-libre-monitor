# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_translations
QT = core testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)

# The translation files are read from the source tree: they are the thing under test.
DEFINES += LM_TRANSLATIONS_DIR=\\\"$$PWD/../../app/translations\\\"

SOURCES += tst_translations.cpp
