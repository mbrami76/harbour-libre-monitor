# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_notificationtexts
QT = core sql testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()
INCLUDEPATH += $$PWD/../support

# The translation files and the source of the texts are read from the source tree: they are the
# thing under test (the .qm files are made from the .ts with lrelease, the way the package does).
DEFINES += LM_TRANSLATIONS_DIR=\\\"$$PWD/../../app/translations\\\"
DEFINES += LM_TEXTS_SOURCE=\\\"$$PWD/../../core/service/notificationtexts.cpp\\\"

DEFINES += LM_SOURCE_ROOT=\\\"$$PWD/../..\\\"

SOURCES += tst_notificationtexts.cpp
