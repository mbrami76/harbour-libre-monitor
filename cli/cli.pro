# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Command-line program of milestone M1 (REQ-ARC-04): host builds only, never installed in
# any package.
TEMPLATE = app
TARGET = lm-cli
QT = core
CONFIG += console
CONFIG -= app_bundle
!hostbuild: error("lm-cli builds only with CONFIG+=hostbuild")

include(../common.pri)
useCore()
include(cli.pri)

SOURCES += main.cpp
