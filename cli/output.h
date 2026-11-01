// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LMCLI_OUTPUT_H
#define LMCLI_OUTPUT_H

#include "session/adapterlink.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace lmcli {

// What one Subject returned. Names are personal data: they are printed on the terminal
// and never written to a file or a log.
struct SubjectResult
{
    SubjectResult() : index(0), received(0), valid(0), quarantined(0) {}

    int index;
    lm::SubjectInfo info;
    QList<lm::Record> records;          // valid records, arrival order
    QJsonObject sensor;                 // as in contract 5.5; empty when absent
    QList<lm::FeedCoverage> coverage;
    lm::UtcTime watermark;
    qint64 received, valid, quarantined;
};

// The newest glucose sample, if any.
bool newestGlucose(const QList<lm::Record> &records, lm::Record *out);

qint64 ageMinutes(const lm::UtcTime &measured, const lm::UtcTime &now);

// "112 mg/dL", with the native resolution of the unit (REQ-DAT-12).
QString formatValue(double value, const QString &unit);

// Text for the terminal. One block per Subject, no secret ever (REQ-ADP-11).
QString formatDiscover(const QList<lm::SubjectInfo> &subjects);
QString formatCurrent(const QList<SubjectResult> &results, const lm::UtcTime &now);
QString formatRange(const QList<SubjectResult> &results);

// The `--json` shape (cli/README.md): one JSON object per run.
// `withRef` adds `subject_ref`, the source's identifier of the person (`discover --json
// --show-ref` only; a person's identifier is never printed unless asked for).
QJsonObject discoverJson(const QList<lm::SubjectInfo> &subjects, bool withRef = false);
QJsonObject currentJson(const QList<SubjectResult> &results, const lm::UtcTime &now);
QJsonObject rangeJson(const QList<SubjectResult> &results);

} // namespace lmcli

#endif
