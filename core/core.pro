# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Static library with the shared base (module core-foundation). Qt core only (REQ-ARC-05).
TEMPLATE = lib
TARGET = lmcore
CONFIG += staticlib
QT = core sql

include(../common.pri)

HEADERS += \
    alerts/alertcoordinator.h \
    alerts/alertengine.h \
    alerts/alertjournal.h \
    alerts/alertsettings.h \
    alerts/silentbook.h \
    alerts/alerttypes.h \
    alerts/band.h \
    alerts/thresholdbook.h \
    alerts/thresholds.h \
    service/donotdisturb.h \
    service/effectfacts.h \
    service/noticewatcher.h \
    service/notificationcatalog.h \
    service/notificationtexts.h \
    service/alertnotification.h \
    service/journallog.h \
    service/maintenancerunner.h \
    service/reportproposal.h \
    service/schedulerhost.h \
    service/residentnotice.h \
    service/serviceevents.h \
    service/silentcontrol.h \
    service/subjectlabel.h \
    service/diagnosticcollector.h \
    service/servicelifecycle.h \
    service/serviceports.h \
    service/servicesettings.h \
    service/startcause.h \
    service/versionwatch.h \
    service/wakepolicy.h \
    scheduler/adapterlinkport.h \
    scheduler/connectionsession.h \
    scheduler/storereadingstore.h \
    scheduler/schedulemath.h \
    scheduler/scheduler.h \
    scheduler/schedulerports.h \
    scheduler/schedulersettings.h \
    guard/guard.h \
    guard/guardsettings.h \
    guard/ingestlog.h \
    guard/journalstatelog.h \
    guard/ports.h \
    guard/statekeeper.h \
    guard/statelog.h \
    guard/violationscore.h \
    jsonl/jsonlcodec.h \
    protocol/envelope.h \
    time/utctime.h \
    record/record.h \
    record/registry.h \
    session/adapterlink.h \
    session/adapterregistry.h \
    session/diagnosticreport.h \
    session/linkchannel.h \
    session/linkpolicy.h \
    session/processchannel.h \
    session/processmemory.h \
    session/scrubber.h \
    store/compactor.h \
    store/dbutil.h \
    store/journal.h \
    store/migration.h \
    store/progress.h \
    store/purge.h \
    store/storagecheck.h \
    store/store.h \
    units/glucoseunits.h

SOURCES += \
    alerts/alertcoordinator.cpp \
    alerts/alertengine.cpp \
    alerts/alertjournal.cpp \
    alerts/alerttypes.cpp \
    alerts/band.cpp \
    alerts/silentbook.cpp \
    alerts/thresholdbook.cpp \
    alerts/thresholds.cpp \
    service/noticewatcher.cpp \
    service/notificationcatalog.cpp \
    service/notificationtexts.cpp \
    service/alertnotification.cpp \
    service/donotdisturb.cpp \
    service/journallog.cpp \
    service/maintenancerunner.cpp \
    service/reportproposal.cpp \
    service/schedulerhost.cpp \
    service/residentnotice.cpp \
    service/serviceevents.cpp \
    service/servicelifecycle.cpp \
    service/startcause.cpp \
    service/subjectlabel.cpp \
    service/diagnosticcollector.cpp \
    service/versionwatch.cpp \
    service/wakepolicy.cpp \
    scheduler/connectionsession.cpp \
    scheduler/storereadingstore.cpp \
    scheduler/schedulemath.cpp \
    scheduler/scheduler.cpp \
    guard/guard.cpp \
    guard/ingestlog.cpp \
    guard/journalstatelog.cpp \
    guard/statekeeper.cpp \
    guard/violationscore.cpp \
    jsonl/jsonlcodec.cpp \
    protocol/envelope.cpp \
    time/utctime.cpp \
    record/record.cpp \
    record/registry.cpp \
    session/adapterlink.cpp \
    session/adapterregistry.cpp \
    session/diagnosticreport.cpp \
    session/nullpolicy.cpp \
    session/processchannel.cpp \
    session/processmemory.cpp \
    session/scrubber.cpp \
    store/compactor.cpp \
    store/dbutil.cpp \
    store/journal.cpp \
    store/migration.cpp \
    store/progress.cpp \
    store/purge.cpp \
    store/storagecheck.cpp \
    store/store.cpp \
    units/glucoseunits.cpp
