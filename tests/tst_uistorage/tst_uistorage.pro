# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# The storage and maintenance page's logic and the diagnostic report dialog's controller, against the scripted MaintenanceBackend.
TARGET = tst_uistorage
QT = core testlib
CONFIG += console testcase
CONFIG -= app_bundle

include(../../common.pri)
useUi()
useCore()

SOURCES += tst_uistorage.cpp
