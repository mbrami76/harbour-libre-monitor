// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_CONFIGSTORE_H
#define LM_UI_CONFIGSTORE_H

// The configuration file as the user interface edits it:
// which connections exist and, per Subject, the name, a short label and the confirmed
// unit. The service reads the same file (daemon/serviceconfig.h) and ignores every key it does
// not know, so the extra keys written here never reach it.
//
// Rules this class exists to keep:
//  - NAMES ARE PERSONAL DATA (REQ-SUB-02, REQ-SUB-03): they live in this file only, which is
//    written with permissions for the user only (mode 600), and nowhere else.
//  - WRITING IS ATOMIC: the new content goes to a temporary file in the same directory and
//    replaces the old one in one step (QSaveFile), so a crash or a full disk never leaves a
//    half-written file that the service would refuse or misread.
//  - UNKNOWN KEYS SURVIVE: the original document is kept and only the keys this class knows are
//    patched in place, so keys added by a newer service or app are not lost.
//  - A FILE THE UI DOES NOT UNDERSTAND IS NEVER OVERWRITTEN: a `format` newer than
//    kKnownConfigFormat, or a file that is not valid JSON, makes load() fail and save() refuse.
//
// Assumption: the shape below is a proposal. Until it is fixed,
// the keys `name`, `label`, `unit`, `unit_basis`, `unit_confirmed` are provisional.

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>

#include <functional>

namespace lm {
namespace ui {

const int kKnownConfigFormat = 1;

// The three per-patient alert times the service reads,
// in whole seconds. The keys are the service's; the ranges are the alert engine's and reach the
// pages through Alerts() `limits`, never from here.
namespace AlertTimeKey {
inline const char *persistenceLow() { return "alert_persistence_low_s"; }
inline const char *persistenceHigh() { return "alert_persistence_high_s"; }
inline const char *missingLimit() { return "missing_limit_s"; }
}

struct SubjectEntry
{
    SubjectEntry() : unitConfirmed(false), persistenceLowS(-1), persistenceHighS(-1), missingLimitS(-1) {}

    QString id;             // the opaque UUID in braces, as the store knows it (REQ-SUB-01)
    QString ref;            // the source's own identifier: personal data, never logged
    QString name;           // personal data
    QString label;          // up to 3 characters, for the cover and the notifications
    QString unit;           // "mg/dL" or "mmol/L", the confirmed unit (REQ-SUB-05)
    QString unitBasis;      // "code", "relation" or "magnitude": how it was determined
    bool unitConfirmed;
    // The alert times written by the thresholds page, whole seconds; -1 = the key is absent, the service
    // then uses its default. Kept through a round trip so that saving the account never drops them.
    int persistenceLowS, persistenceHighS, missingLimitS;
};

struct ConnectionEntry
{
    ConnectionEntry() : currentIntervalS(0), graphIntervalS(0), logbookIntervalS(0) {}

    QString id;
    QString adapter;
    // The name the guard and approval notices use for this account instead of an account name
    // (at most kConnectionLabelMaxChars: the account page writes the translated word
    // "Account" by default, editable in the settings).
    QString label;
    QList<SubjectEntry> subjects;
    int currentIntervalS, graphIntervalS, logbookIntervalS;     // 0 = the default
};

// The longest connection label the service takes (: "at most 24
// characters"; a longer one is ignored by the service, so the settings page refuses it).
const int kConnectionLabelMaxChars = 24;

class ConfigStore
{
public:
    ConfigStore();

    // A missing file is an empty, valid configuration (first run). A file that exists but is
    // not valid JSON or has a newer format fails, with a fixed error key (never a path or a
    // value from the file).
    bool load(const QString &path);
    // Writes atomically with mode 600. False on failure or when load() refused the file.
    bool save();

    bool isValid() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }

    QList<ConnectionEntry> connections() const;
    // Adds or replaces a connection (matched by id) and its Subjects.
    void setConnection(const ConnectionEntry &connection);
    bool removeConnection(const QString &connectionId);
    // A Subject's UUID is created once and never reused (REQ-SUB-02).
    static QString newSubjectId();

    // The settings the service reads at the top of the file (REQ-ALR-10):
    // the language of the notification texts ("" = the phone's, the key is removed) and
    // whether an alarm's first line shows the newest value (off by default).
    QString language() const;
    void setLanguage(const QString &language);
    bool alertsShowValue() const;
    void setAlertsShowValue(bool on);
    // One key of one connection or one patient, patched in place (the other keys survive). False when
    // the connection or the patient is not in the file. An empty label removes the key; a negative
    // time removes the key (the service's default then applies).
    bool setConnectionLabel(const QString &connectionId, const QString &label);
    bool setSubjectLabel(const QString &subjectId, const QString &label);
    bool setSubjectAlertTime(const QString &subjectId, const char *key, int seconds);

private:
    // Applies `patch` to the patient `subjectId` of whichever connection holds it.
    bool patchSubject(const QString &subjectId, const std::function<void(QJsonObject &)> &patch);

    QString m_path;
    QString m_error;
    QJsonObject m_root;
};

} // namespace ui
} // namespace lm

#endif
