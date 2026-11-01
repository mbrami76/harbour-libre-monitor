# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Top-level project: builds the components as sub-projects.
#
#   app/  the only installed executable, harbour-libre-monitor (UI, service and adapter modes)
#   daemon/  static library, not installed: process modes, D-Bus interface, service wiring;
#            its host-only executable daemon/host (lm-service) is built with hostbuild
#
#   core/   static library, not installed
#   tests/  QtTest, host builds only (CONFIG+=hostbuild), never installed (REQ-ARC-04)
#   cli/    lm-cli, the command-line program of milestone M1; host builds only, never installed
#
#   adapters/libre/       static library with the Libre adapter, not installed;
#                         its host-only executable adapters/libre/host is built with hostbuild
#
TEMPLATE = subdirs

SUBDIRS = core libre daemon uilogic uireal app

# The logic of the user interface: no QML, testable on the host.
uilogic.subdir = app/ui
uilogic.depends = core

# The real data source of the interface: D-Bus to the service, read-only store, account saving.
uireal.subdir = app/real
uireal.depends = core daemon uilogic

libre.subdir = adapters/libre
libre.depends = core

# The process side of the service: modes, D-Bus interface, wiring, real ports.
# Linked into the application's one binary by app/.
daemon.depends = libre core
app.depends = daemon libre core uilogic uireal

hostbuild {
    SUBDIRS += libre_host service_host cli tests
    service_host.subdir = daemon/host
    service_host.depends = daemon libre core
    cli.depends = core
    libre_host.subdir = adapters/libre/host
    libre_host.depends = libre core
    tests.depends = core libre daemon uilogic uireal
}

DISTFILES += \
    rpm/harbour-libre-monitor.changes.in \
    rpm/harbour-libre-monitor.changes.run.in \
    rpm/harbour-libre-monitor.spec
