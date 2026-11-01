# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Host-only executable for development: runs the Libre adapter loop on the real standard
# streams. Built only with CONFIG+=hostbuild, never installed, never packaged (REQ-SIM-06).
TEMPLATE = app
TARGET = libre-adapter-host
QT = core network
CONFIG += console
CONFIG -= app_bundle
!hostbuild: error("libre-adapter-host builds only with CONFIG+=hostbuild")

include(../../../common.pri)
useLibre()
useCore()

SOURCES += main.cpp
