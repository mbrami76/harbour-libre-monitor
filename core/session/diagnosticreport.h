// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DIAGNOSTICREPORT_H
#define LM_DIAGNOSTICREPORT_H

// Diagnostic report for the developer (REQ-GRD-15). When the guard ended an adapter in
// the middle of a batch, the batch is held in memory (REQ-GRD-13). If it happens twice in a
// row, the app offers to write an inspectable text file the user can read and send. This
// header is the pure part of that: from the raw protocol line of the held batch and a small
// context, it builds the text of the file. It reads no file, writes no file and looks at no
// clock, so the same input always gives the same bytes and tests need nothing else. The
// file, the consent and the developer's address belong to the app (modules 6, 8 and 9).
//
// Pattern: whitelist, not blacklist. The adapter is an untrusted program talking to an
// unofficial source, so we cannot list every field that might carry a name or an
// identifier. Instead each field of the batch is either known and safe (kept), known and
// sensitive (replaced by a marker), or unknown (reduced to its type). A text value that is
// not on the context's lists of declared feeds, metrics and units is never copied: it is
// replaced by a mask that keeps only its length and the kind of each character, which is
// enough to see that a format changed.
//
// What the report keeps: glucose values and units (the problem cannot be understood without
// them, and the user decides whether to send), the shape, the declared metric, feed and
// source names, and the key names of the batch. What it removes: names, Subject and account
// identifiers, source references (`subject_ref`), sensor ids, session and token strings,
// message and correlation ids, and every absolute time. Times become offsets from the first
// time of the batch ("t+0", "t+300 s"); a time that cannot be read keeps only a mask of its
// format, so a changed date format stays visible without revealing a date.

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace lm {

// Version of the report layout, written in the first line, so a reader of the file knows
// which layout to expect.
const int kDiagnosticReportFormat = 1;

// Upper bound on the records written from one batch (the protocol ceiling is 500 per batch,
//); more are only counted.
const int kDiagnosticMaxRecords = 500;

// One violation as the driver reported it: the wire name of the kind (violationKindName) and
// its short detail, which names a limit or a field and never carries content.
struct DiagnosticViolation
{
    QString kind;
    QString detail;
};

// Everything besides the batch itself. All of it is supplied by the caller from its own
// state and is trusted not to hold personal data: version strings, adapter id, wire names,
// counts, and the adapter's declarations from `hello` (feeds, metrics, units), which are
// the only text values of the batch that the report copies.
struct DiagnosticContext
{
    DiagnosticContext()
        : batches(0), recordsReceived(0), recordsValid(0), softViolations(0), hardViolations(0) {}

    QString applicationVersion;
    QString adapterId;
    QString adapterVersion;
    QString backendVersion;
    QString commandType;                    // wire name of the command, e.g. "fetch_range"
    QList<DiagnosticViolation> violations;  // in the order reported
    qint64 batches;
    qint64 recordsReceived;
    qint64 recordsValid;
    QMap<QString, qint64> quarantinedByReason;  // reason name -> count
    int softViolations;
    int hardViolations;
    QStringList declaredFeeds;
    QStringList declaredMetrics;
    QStringList declaredUnits;
};

// Builds the report as JSON Lines (one compact JSON object per line, each ending in \n):
//   line 1   {"line_type":"report", format, versions, command, violations, counts}
//   line 2   {"line_type":"batch", "status", and the anonymized batch envelope}
//   line 3.. {"line_type":"record", "index", ...one anonymized record each}
// `status` is "ok", "unparseable" (the line is not a JSON object) or "cut" (longer than
// maxLineBytes; only the size is reported). In the last two cases there are no record lines
// but the context is still complete. Keys are sorted, so the output is deterministic.
QByteArray buildDiagnosticReport(const QByteArray &rawBatchLine, const DiagnosticContext &context,
                                 int maxLineBytes = 1048576);

} // namespace lm

#endif
