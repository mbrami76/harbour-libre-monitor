# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

TARGET = tst_notificationcatalog
QT = core gui sql testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useCore()
INCLUDEPATH += $$PWD/../support

DEFINES += LM_REPO_DIR=\\\"$$PWD/../..\\\"
SOURCES += tst_notificationcatalog.cpp
