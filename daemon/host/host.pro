# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Host-only service executable for the PC: the same service code as the
# application's `--service` mode, with the stand-in ports of hostports.h and development
# options for the simulator. Built only with CONFIG+=hostbuild, never installed, never
# packaged (REQ-SIM-06).
TEMPLATE = app
TARGET = lm-service
QT = core sql dbus network
CONFIG += console
CONFIG -= app_bundle
!hostbuild: error("lm-service builds only with CONFIG+=hostbuild")

include(../../common.pri)
useDaemon()
useLibre()
useCore()

SOURCES += main.cpp
