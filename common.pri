# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Shared build settings for the core library and everything that links it.
# Development flag (REQ-SIM-06): host builds imply devsource; devsource defines
# LIBREMON_DEVSOURCE for development-only code. Release and Harbour builds define neither.

hostbuild: CONFIG += devsource
devsource: DEFINES += LIBREMON_DEVSOURCE

CONFIG += c++11

INCLUDEPATH += $$PWD/core

# The application's version, visible to the code as two defines:
#   LM_APP_VERSION  "<Version>-<Release>" of the RPM spec, for example "1.0.0-1"
# The version is read from the spec itself (the Version and Release lines), so the number is written
# in one place only and a package build, an IDE build and a host build all say the same thing. The
# spec is found by pattern because the development package script renames it (tools/devpackage);
# its release then follows the spec too. The Sailfish %qmake5 macro passes no version to qmake, which
# is why the spec is read here instead.
LM_SPEC_FILE = $$files($$PWD/rpm/*.spec)
LM_SPEC_FILE = $$first(LM_SPEC_FILE)
LM_VERSION = 0
LM_RELEASE = 0
LM_CONTACT =
!isEmpty(LM_SPEC_FILE) {
    LM_SPEC_LINES = $$cat($$LM_SPEC_FILE, lines)
    for(lm_line, LM_SPEC_LINES) {
        contains(lm_line, "^Version:.*"): LM_VERSION = $$replace(lm_line, "^Version:[ \\t]*", "")
        contains(lm_line, "^Release:.*"): LM_RELEASE = $$replace(lm_line, "^Release:[ \\t]*", "")
        # "Packager: Michele Brami <info@michelebrami.it>": the address between the angle brackets.
        contains(lm_line, "^Packager:.*<.*>.*"): LM_CONTACT = $$replace(lm_line, "^[^<]*<([^>]*)>.*$", "\\1")
    }
}
DEFINES += LM_APP_VERSION=\\\"$${LM_VERSION}-$${LM_RELEASE}\\\"

# The developer's contact (REQ-GRD-15 "read from the packaging metadata"):
#   LM_DEVELOPER_CONTACT  the e-mail address of the spec's Packager line, for example "info@michelebrami.it"
# One source, the spec, read here like the version; the About page and the diagnostic report dialog
# both get it through the report controller (app/src/main.cpp). The address is public by decision.
# A spec without a Packager address gives an empty define, and the pages then say that no address is known.
DEFINES += LM_DEVELOPER_CONTACT=\\\"$${LM_CONTACT}\\\"

LM_CORE_BUILD_DIR = $$shadowed($$PWD/core)

# Link the static core library (include from a sub-project that needs it).
defineTest(useCore) {
    LIBS += -L$$LM_CORE_BUILD_DIR -llmcore
    PRE_TARGETDEPS += $$LM_CORE_BUILD_DIR/liblmcore.a
    export(LIBS)
    export(PRE_TARGETDEPS)
}

# Link the Libre adapter library (module adapter-libre) and QtNetwork. Call it BEFORE
# useCore(): the adapter library depends on the core library, so it comes first on the
# link line.
LM_LIBRE_SRC_DIR = $$PWD/adapters/libre
LM_LIBRE_BUILD_DIR = $$shadowed($$PWD/adapters/libre)
defineTest(useLibre) {
    QT += network
    INCLUDEPATH += $$LM_LIBRE_SRC_DIR
    LIBS += -L$$LM_LIBRE_BUILD_DIR -llmlibre
    PRE_TARGETDEPS += $$LM_LIBRE_BUILD_DIR/liblmlibre.a
    export(QT)
    export(INCLUDEPATH)
    export(LIBS)
    export(PRE_TARGETDEPS)
}

# Headers of the Libre adapter, for code that only calls its entry point (the daemon library).
defineTest(useLibreHeaders) {
    INCLUDEPATH += $$LM_LIBRE_SRC_DIR
    export(INCLUDEPATH)
}

# Link the service's process library. Call it BEFORE useLibre() and useCore(): the
# daemon library depends on both, so it comes first on the link line. QtDBus and QtSql are
# added here because the static library needs them at the final link.
LM_DAEMON_BUILD_DIR = $$shadowed($$PWD/daemon)
LM_ROOT_DIR = $$PWD
defineTest(useDaemon) {
    QT += dbus sql
    INCLUDEPATH += $$LM_ROOT_DIR
    LIBS += -L$$LM_DAEMON_BUILD_DIR -llmdaemon
    PRE_TARGETDEPS += $$LM_DAEMON_BUILD_DIR/liblmdaemon.a
    export(QT)
    export(INCLUDEPATH)
    export(LIBS)
    export(PRE_TARGETDEPS)
}

# Link the user-interface logic library (app/ui). Call it BEFORE useCore(): it uses
# the unit conversion of the core, so it comes first on the link line.
LM_UI_BUILD_DIR = $$shadowed($$PWD/app/ui)
LM_UI_ROOT_DIR = $$PWD/app
defineTest(useUi) {
    INCLUDEPATH += $$LM_UI_ROOT_DIR
    LIBS += -L$$LM_UI_BUILD_DIR -llmui
    PRE_TARGETDEPS += $$LM_UI_BUILD_DIR/liblmui.a
    export(INCLUDEPATH)
    export(LIBS)
    export(PRE_TARGETDEPS)
}

# Link the real data source of the interface (app/real): D-Bus, store and journal reading, account
# saving. Call it BEFORE useUi() and useCore(): it depends on both.
LM_UIREAL_BUILD_DIR = $$shadowed($$PWD/app/real)
LM_UIREAL_ROOT_DIR = $$PWD/app
defineTest(useUiReal) {
    QT += dbus sql
    INCLUDEPATH += $$LM_UIREAL_ROOT_DIR
    LIBS += -L$$LM_UIREAL_BUILD_DIR -llmuireal
    PRE_TARGETDEPS += $$LM_UIREAL_BUILD_DIR/liblmuireal.a
    export(QT)
    export(INCLUDEPATH)
    export(LIBS)
    export(PRE_TARGETDEPS)
}
