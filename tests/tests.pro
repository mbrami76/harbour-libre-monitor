# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami

# Host builds only (CONFIG+=hostbuild), never installed (REQ-ARC-04).
TEMPLATE = subdirs
!hostbuild: error("tests build only with CONFIG+=hostbuild")

SUBDIRS = tst_jsonl tst_envelope tst_utctime tst_record tst_units tst_store tst_storemigration tst_journal tst_compaction tst_purge tst_storagecheck \
    tst_libretime tst_libreunit tst_libreadapter tst_adapterlink tst_diagnosticreport tst_diagnosticcollector tst_cli \
    tst_guard tst_schedulemath tst_scheduler tst_tickrhythm tst_storeadapters \
    tst_service tst_versionwatch tst_maintenance tst_daemon tst_discovery tst_provision \
    tst_alertthresholds tst_alertengine tst_alertjournal tst_alertrhythm tst_alertcoordinator tst_alertsilent tst_alertnotification tst_notificationcatalog tst_notificationtexts tst_notifierarbiter tst_notifierreminders tst_notifiersilent tst_notifiernotices tst_residentnotice tst_donotdisturb tst_devfeed \
    tst_uilogic tst_uiplot tst_uistore tst_uimodel tst_uiaccount tst_uialerts tst_uithresholds tst_uistorage tst_uireal tst_translations
