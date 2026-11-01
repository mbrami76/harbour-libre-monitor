# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# The real data source of the user interface: the monitoring service over D-Bus, the
# store and the journal read-only, and the account page's test and saving. Separate from app/ui,
# which has no D-Bus and no SQL, so the interface logic stays testable without them. Not
# installed; the application links it into its one binary.
TEMPLATE = lib
TARGET = lmuireal
CONFIG += staticlib
QT = core sql dbus

include(../../common.pri)
INCLUDEPATH += $$PWD/.. $$PWD/../..

HEADERS += \
    realaccountbackend.h \
    realalertsbackend.h \
    realbackend.h \
    realmaintenancebackend.h \
    servicelink.h \
    snapshotbuilder.h \
    statusparser.h \
    storequeries.h \
    storeworker.h

SOURCES += \
    realaccountbackend.cpp \
    realalertsbackend.cpp \
    realbackend.cpp \
    realmaintenancebackend.cpp \
    servicelink.cpp \
    snapshotbuilder.cpp \
    statusparser.cpp \
    storequeries.cpp \
    storeworker.cpp
