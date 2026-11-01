// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTNOTIFICATION_H
#define LM_ALERTNOTIFICATION_H

// WHAT THE USER IS SHOWN, AND WHICH NOTIFICATION REPLACES WHICH (sections
// 2, 5, 6 and 8; REQ-ALR-04).
//
// The alert engine answers a reading with a list of effects: "raise the notification
// of this slot", "raise it again", "replace it quietly", "take it down", "tell the user monitoring
// was interrupted". AlertNotificationPresenter carries those out as system notifications. It is
// the single implementation of the AlertNotifier port for the phone; the phone's library
// (libnemonotifications) sits behind the small AlertNotificationBackend interface, and the PC
// host and the tests substitute a fake. So everything that matters is testable on the host: what
// is posted, with which hints and words, which notification replaces which, and what the service
// does when the user deletes one.
//
// Pattern: adapter (effects -> backend calls) with state, in the order of:
//  - IDS. One notification per (Subject, slot), replaced in place. The platform gives the
//    id; it is kept in the service settings so that after a restart the notification the restored
//    alert raises REPLACES the old one, and an entry left by an alert that no longer exists is
//    closed (closeOrphans). The service remembers the ids it closed itself, because it cannot see
//    the reason of its own close: a `closed` signal for any other id with the reason
//    "dismissed by the user" is a deletion, which is the user's acknowledgement. A notification
//    re-published after a deletion comes back under a NEW id; the id given back is the one used
//    from then on. A `closed` signal with the reason "closed by call" is ignored: it arrives on
//    older objects of an id the service re-published and closed.
//  - THE ARBITER. Several alarms may be active at once; they cannot layer, two
//    `clock` requests give one tone, and a short sound over a ringing tone is not
//    audible. So one alarm holds the long tone at a time: the best of the urgent alarms
//    that are active, unacknowledged and not silent, by the rank urgent low before urgent high and,
//    for the same rank, the earlier onset. Every other alarm keeps its banner, entry, wake-up and
//    list line but gets NO sound while the tone rings, and gives its short sound once when the tone
//    has ended if it is still unacknowledged. A higher alarm that arrives takes the tone by a
//    replacement in place of the old holder WITHOUT hints; when the holder is acknowledged,
//    cleared, lowered or silenced the next urgent alarm is re-posted with the alarm hints (a
//    fade-in). When no urgent tone rings, an alarm's own sound at its first post and at each
//    reminder stands (the long tone only ever belongs to the holder).
//  - the rest (silent mode, notices) are added by the items that follow.
//
// What the text holds is notificationtexts.h's business and what the hints are is the
// catalogue's (notificationcatalog.h); this class decides neither. Not thread-safe: the service's
// main thread calls it, as for the coordinator.

#include "alerts/alertcoordinator.h"
#include "service/effectfacts.h"
#include "service/notificationcatalog.h"
#include "service/notificationtexts.h"
#include "service/serviceevents.h"
#include "service/serviceports.h"
#include "service/subjectlabel.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>

namespace lm {

// Everything the backend needs to post one notification. No value unless the user's setting asked
// for it, no name, never a comment (the texts decide). The banner shows the same summary and body.
struct NotificationRequest
{
    QString summary;
    QString body;
    NoteHints hints;
};

// The platform behind the presenter: libnemonotifications on the phone, a recorder in tests.
class AlertNotificationBackend
{
public:
    typedef std::function<void(quint32 id, uint reason)> ClosedHandler;

    virtual ~AlertNotificationBackend() {}
    // Posts the notification, replacing the one with `replacesId` when that is not 0. Returns the
    // platform's id of the notification now up (a new one when the id given was deleted by the
    // user), 0 when it could not be posted.
    virtual quint32 publish(const NotificationRequest &request, quint32 replacesId) = 0;
    // Takes the notification down. An id the platform no longer knows is not an error. The
    // platform sends no `closed` signal to the caller for this.
    virtual void close(quint32 id) = 0;
    // Called when a notification the backend posted is closed by someone else: reason 1 expired,
    // 2 dismissed by the user, 3 closed by a call (also on older objects of a re-published id).
    virtual void setClosedHandler(const ClosedHandler &handler) = 0;
};

// The short label of each Subject for the notification text. The service fills it from the
// configuration file's `label` key and again when the file changes. A Subject
// without a usable label is named P1, P2, ... by its position in the configuration file
// the rule itself is the shared pure function patientLabel()
// (core/service/subjectlabel.h), so a notification is never blank and never carries a name.
class SubjectLabelBook
{
public:
    // `order` is the Subjects in configuration order; `labels` maps a Subject to its label. A
    // label longer than kMaxLabelChars is ignored (it is not a short label).
    void set(const QList<QUuid> &order, const QHash<QUuid, QString> &labels);
    QString labelOf(const QUuid &subject) const;
    int count() const { return m_order.size(); }
    // Labels of Subjects that are not in the configuration (the developer-only synthetic feed,
    //): looked up after the configured ones, kept apart so a change of the
    // configuration does not forget them.
    void setExtra(const QHash<QUuid, QString> &extra) { m_extra = extra; }

    static const int kMaxLabelChars = kPatientLabelMaxChars;

private:
    QList<QUuid> m_order;
    QHash<QUuid, QString> m_labels;
    QHash<QUuid, QString> m_extra;
};

// The newest stored reading of a Subject, for the optional setting that shows the value. Read at
// posting time; the effects carry no value.
class AlertValueSource
{
public:
    virtual ~AlertValueSource() {}
    // `trendCode` is -1 when there is none; `measuredAt` is the reading's measurement time.
    virtual bool newest(const QUuid &subject, double *value, QString *unit, int *trendCode,
                        UtcTime *measuredAt) const = 0;
};

// Between the alert coordinator and whatever shows the alerts: passes every effect on to the
// notifier (the user's notification) and ALSO tells the service's event sinks what changed, so the
// D-Bus signal `AlertChanged` reaches an open user interface. The
// notification goes first: a user who is not looking at the screen must never wait for a signal.
//
// An alert's visible state changes exactly when the engine asks for a Notify (raised, escalated,
// re-alerted, restored), an Update (acknowledged, lowered, cause changed, silent mode started) or a
// Withdraw (cleared, snoozed); a Remind repeats the same state and an Interruption is a notice, so
// neither is a change. The state carried is the effect's own.
class AlertChangeRelay : public AlertNotifier
{
public:
    // Neither is owned; either may be null.
    AlertChangeRelay(AlertNotifier *next, ServiceEventSink *events) : m_next(next), m_events(events) {}
    void deliver(const AlertEffect &effect);

private:
    AlertNotifier *m_next;
    ServiceEventSink *m_events;
};

class AlertNotificationPresenter : public AlertNotifier
{
public:
    // Nothing is owned; all must outlive the presenter. `labels`, `values` and `facts` may be null
    // (then a Subject shows "?", no value is shown, and the facts are the effect's own).
    AlertNotificationPresenter(AlertNotificationBackend *backend, ServiceSettingsStore *settings,
                               const SubjectLabelBook *labels, Clock *clock,
                               const NotificationCatalog &catalog = NotificationCatalog());

    void setTexts(const NotificationTexts &texts) { m_texts = texts; }
    const NotificationTexts &texts() const { return m_texts; }
    void setFactsReader(const EffectFactsReader *facts) { m_facts = facts; }
    void setValueSource(const AlertValueSource *values) { m_values = values; }
    void setShowValue(bool on) { m_showValue = on; }

    // What to do when the user's deletion of a notification acknowledges the slot. The service sets
    // it to the coordinator's acknowledge.
    typedef std::function<void(const QUuid &subject, AlertSlot slot)> AcknowledgeHandler;
    void setAcknowledgeHandler(const AcknowledgeHandler &handler) { m_acknowledge = handler; }

    void deliver(const AlertEffect &effect);

    // After the restore at start has delivered its effects: closes the notifications of alerts that
    // no longer exist (they ended while the service was off).
    void closeOrphans();

    // The notices that are not alerts. Each is one notification
    // with its own id, replaced in place by a newer one and withdrawn when its cause ends. None is
    // ever covered by a Subject's silent mode. The connection is an opaque id; its words come
    // from the callbacks below (a connection's label, and the labels of its Subjects).
    typedef std::function<QStringList(const QString &connection)> SubjectsOfConnection;
    typedef std::function<QString(const QString &connection)> ConnectionLabel;
    void setConnectionInfo(const SubjectsOfConnection &subjects, const ConnectionLabel &label)
    { m_subjectsOf = subjects; m_connectionLabel = label; }
    // The approval of a new patient is asked for (at the start and at each 15-minute re-raise): the
    // notice sounds its `general_warning` each time, and is withdrawn when the approval is given.
    void notifyApproval(const QString &connection);
    void approvalSettled(const QString &connection);
    // The guard disabled a connection after `attempts` failures; withdrawn when it is enabled again.
    void notifyGuardDisabled(const QString &connection, int attempts, const QString &causeKey);
    void guardEnabled(const QString &connection);
    // A diagnostic report was prepared; withdrawn when the user handled it.
    void notifyReportProposed(const QString &connection);
    void reportSettled(const QString &connection);
    // Do not disturb turned on or off: the notice "alerts will be silent".
    void notifyDoNotDisturb(bool on);
    // The same notice when the service STARTS and reads the state: a start
    // is not a change, so nothing is raised again. If the notice the last run left is still known by its
    // stored id and Do not disturb is still on, it is adopted as it is (no new post, so no new banner);
    // it is posted only when Do not disturb is on and the entry is absent (it came on while the service
    // was down, or the user deleted the entry), and withdrawn when Do not disturb went off meanwhile.
    void adoptDoNotDisturb(bool on);

    // The labels of a connection's Subjects, "ABC, DEF", as the approval notices print them.
    QString subjectLabelsOf(const QString &connection) const;
    // Takes down the "ended" entries of the Subjects and the interruption notice (the developer-only
    // synthetic feed uses it to leave a clean tray; a real run never does).
    void clearEndedEntriesAndInterruption(const QList<QUuid> &subjects);

    // For tests and the resident notice.
    quint32 idOf(const QUuid &subject, AlertSlot slot) const;
    bool hasAlarm(const QUuid &subject, AlertSlot slot) const;
    // When the current level of an alarm began, as noted at its raise; invalid when the presenter
    // holds no such alarm (a snoozed one, or after a restart before the restore).
    UtcTime levelSinceOf(const QUuid &subject, AlertSlot slot) const;
    // The Subject and slot that hold the long tone now; false when none does.
    bool toneHolder(QUuid *subject, AlertSlot *slot) const;

private:
    struct Alarm
    {
        Alarm() : slot(AlertSlot::Low), type(AlertType::L), state(AlertState::Active), id(0),
                  acknowledged(false), everAcknowledged(false), toneRinging(false), owesShort(false) {}
        QUuid subject;
        AlertSlot slot;
        AlertType type;
        AlertState state;
        quint32 id;                 // the notification up for it; 0 = none (acknowledged, or deleted)
        bool acknowledged;
        bool everAcknowledged;      // acknowledged or snoozed in this episode: no "ended" entry when it clears
        bool toneRinging;           // its last post carried the long tone
        bool owesShort;             // posted without sound over a ringing tone: gives its short sound once the tone ends
        EffectFacts facts;
    };

    static QString keyOf(const QUuid &subject, AlertSlot slot);
    NotificationRequest build(const Alarm &alarm, Sound sound, int reminder) const;
    void post(Alarm *alarm, Sound sound, int reminder);
    void closeNotification(Alarm *alarm);
    EffectFacts factsOf(const AlertEffect &effect) const;
    AlarmTextInput inputFor(const Alarm &alarm) const;

    void onClosed(quint32 id, uint reason);

    // One notification of a notice kind under a key of its own.
    void postNotice(const QString &key, NoteKind kind, const NoteText &text, Sound sound);
    void withdrawNotice(const QString &key);

    // The arbiter: who holds the long tone, the sound an alarm may ask for now, and the settling
    // after every effect (hand-over, the short sounds owed when the tone has ended).
    QString toneHolder() const;
    Sound soundFor(Alarm *alarm, const QString &holder);
    void settle();
    quint32 storedId(const QString &key) const;
    void storeId(const QString &key, quint32 id);
    void rememberClosed(quint32 id);

    AlertNotificationBackend *m_backend;
    ServiceSettingsStore *m_settings;
    const SubjectLabelBook *m_labels;
    Clock *m_clock;
    NotificationCatalog m_catalog;
    NotificationTexts m_texts;
    const EffectFactsReader *m_facts;
    const AlertValueSource *m_values;
    bool m_showValue;
    AcknowledgeHandler m_acknowledge;

    SubjectsOfConnection m_subjectsOf;
    ConnectionLabel m_connectionLabel;
    QMap<QString, quint32> m_noticeIds; // ids of the notices and "ended" entries, by their key
    QSet<QString> m_snoozed;            // slots whose alert was snoozed in the running episode
    QSet<QString> m_posted;             // keys posted in this run (closeOrphans leaves them)
    QMap<QString, Alarm> m_alarms;      // by keyOf(subject, slot)
    QList<quint32> m_closedByUs;        // ids this service closed, newest last (bounded)
};

} // namespace lm

#endif
