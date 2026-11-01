# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Static library with the Libre adapter (module adapter-libre). Qt core and network only,
# and QtNetwork only in this folder. Not installed; the service links it into the application
# binary (REQ-ADP-15).
TEMPLATE = lib
TARGET = lmlibre
CONFIG += staticlib
QT = core network

include(../../common.pri)

HEADERS += \
    httptransport.h \
    libreadapter.h \
    libreclient.h \
    libreerror.h \
    libremain.h \
    libresource.h \
    libretimestamp.h \
    libreunit.h \
    qthttptransport.h

SOURCES += \
    libreadapter.cpp \
    libreclient.cpp \
    libremain.cpp \
    libresource.cpp \
    libretimestamp.cpp \
    libreunit.cpp \
    qthttptransport.cpp
