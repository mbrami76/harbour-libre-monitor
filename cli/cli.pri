# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Sources of the command-line program except main.cpp, shared by the program and its
# tests. Host builds only (REQ-ARC-04).
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/credentials.h \
    $$PWD/options.h \
    $$PWD/output.h \
    $$PWD/runner.h

SOURCES += \
    $$PWD/credentials.cpp \
    $$PWD/options.cpp \
    $$PWD/output.cpp \
    $$PWD/runner.cpp
