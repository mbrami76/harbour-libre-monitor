# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Static library with the process side of the service: the modes of the
# application binary, the D-Bus interface, the wiring of the pipeline and the real ports.
# Not installed. The application links it into its one binary (no second executable).
# QtDBus is on the Harbour list.
TEMPLATE = lib
TARGET = lmdaemon
CONFIG += staticlib
QT = core sql dbus network

include(../common.pri)
INCLUDEPATH += $$PWD/..
useLibreHeaders()

HEADERS += \
    adaptermode.h \
    applicationprovision.h \
    applicationservice.h \
    commonports.h \
    dbusservice.h \
    devfeed.h \
    devlog.h \
    discovery.h \
    hostports.h \
    maintenancejobs.h \
    processmode.h \
    provisioning.h \
    provisionmode.h \
    servicecore.h \
    serviceconfig.h \
    serviceidentity.h \
    serviceplatform.h \
    serviceprocess.h \
    serviceruntime.h \
    terminalprompt.h \
    uiguard.h \
    unixsignals.h

SOURCES += \
    adaptermode.cpp \
    applicationprovision.cpp \
    applicationservice.cpp \
    commonports.cpp \
    dbusservice.cpp \
    devfeed.cpp \
    discovery.cpp \
    hostports.cpp \
    maintenancejobs.cpp \
    processmode.cpp \
    provisioning.cpp \
    provisionmode.cpp \
    servicecore.cpp \
    serviceconfig.cpp \
    serviceidentity.cpp \
    serviceprocess.cpp \
    serviceruntime.cpp \
    terminalprompt.cpp \
    uiguard.cpp \
    unixsignals.cpp

# The ports that exist only on the device (keepalive, the notification library, Sailfish
# Secrets): device kits only. The PC build uses the stand-ins of hostports.h.
!hostbuild {
    DEFINES += LM_PHONE_PORTS
    CONFIG += link_pkgconfig
    # QML for the one headless engine that watches Do not disturb (phoneports.h).
    QT += qml
    PKGCONFIG += keepalive nemonotifications-qt5 sailfishsecrets
    HEADERS += phoneports.h
    SOURCES += phoneports.cpp
}
