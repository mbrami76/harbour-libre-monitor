// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_NOTIFIERTEST_H
#define LM_NOTIFIERTEST_H

// Fakes for the tests of the alert notifier: a backend that records
// what is posted and closed and can play the platform's `closed` signals, settings in memory, a
// clock that stands still until told, and effect and facts builders. Everything is synthetic.

#include "service/alertnotification.h"

#include <QProcess>
#include <QTemporaryDir>

#include <QDateTime>
#include <QHash>
#include <QSet>

namespace lm {
namespace testing {

class FakeNoteBackend : public AlertNotificationBackend
{
public:
    struct Post { NotificationRequest request; quint32 replaces; quint32 id; };

    FakeNoteBackend() : next(100), failNext(false), calls(0) {}

    quint32 publish(const NotificationRequest &r, quint32 replacesId)
    {
        Post p;
        p.request = r;
        p.replaces = replacesId;
        if (failNext) {
            failNext = false;
            p.id = 0;
        } else if (replacesId != 0 && !deleted.contains(replacesId) && !closedIds.contains(replacesId)) {
            p.id = replacesId;                                  // replaced in place
        } else {
            p.id = next++;                                      // a new id: first post, or the old one was deleted
        }
        posts.append(p);
        if (calls) calls->append(QStringLiteral("post"));
        if (p.id)
            live.insert(p.id);
        return p.id;
    }
    void close(quint32 id) { closes.append(id); closedIds.insert(id); live.remove(id); }
    void setClosedHandler(const ClosedHandler &h) { handler = h; }

    // The user deletes the notification: the platform tells the service, with the reason 2.
    void userDeletes(quint32 id)
    {
        deleted.insert(id);
        live.remove(id);
        if (handler) handler(id, 2);
    }
    // The platform tells the service of a close with another reason (1 expired, 3 closed by call).
    void closedWith(quint32 id, uint reason) { if (handler) handler(id, reason); }

    const Post &last() const { return posts.last(); }
    int postsOf(quint32 id) const { int n = 0; for (int i = 0; i < posts.size(); ++i) n += posts.at(i).id == id; return n; }

    QList<Post> posts;
    QList<quint32> closes;
    QSet<quint32> deleted, closedIds, live;
    ClosedHandler handler;
    quint32 next;
    bool failNext;
    QStringList *calls;         // optional: one list shared with a FakeNotifier, to see the order of the calls
};

class MemoryNoteSettings : public ServiceSettingsStore
{
public:
    QString value(const QString &key) const { return data.value(key); }
    void setValue(const QString &key, const QString &value) { data.insert(key, value); }
    QHash<QString, QString> data;
};

class NoteClock : public Clock
{
public:
    NoteClock() : ms(1792300800000LL) {}       // 2026-10-18T05:20:00Z
    UtcTime now() const { return UtcTime::fromMSecsSinceEpoch(ms); }
    void advanceS(int s) { ms += qint64(s) * 1000; }
    qint64 ms;
};

// Facts the test sets per Subject and slot, standing in new effect fields.
class FakeFacts : public EffectFactsReader
{
public:
    EffectFacts factsOf(const AlertEffect &e) const
    {
        EffectFacts f = facts.value(key(e.subject, e.slot));
        f.cause = e.cause.isEmpty() ? f.cause : e.cause;
        return f;
    }
    static QString key(const QUuid &s, AlertSlot slot) { return s.toString() + alertSlotName(slot); }
    void set(const QUuid &s, AlertSlot slot, const EffectFacts &f) { facts.insert(key(s, slot), f); }
    QHash<QString, EffectFacts> facts;
};

class FakeValues : public AlertValueSource
{
public:
    FakeValues() : has(false), value(0), trend(-1) {}
    bool newest(const QUuid &, double *v, QString *u, int *t, UtcTime *m) const
    {
        if (!has) return false;
        *v = value; *u = unit; *t = trend; *m = measuredAt;
        return true;
    }
    bool has;
    double value;
    QString unit;
    int trend;
    UtcTime measuredAt;
};

inline QUuid noteSubject(int n)
{
    return QUuid::createUuidV5(QUuid("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"), QString("note-%1").arg(n));
}

inline AlertEffect noteEffect(AlertEffect::Kind kind, int subject, AlertSlot slot, AlertType type, AlertState state)
{
    AlertEffect e;
    e.kind = kind;
    e.subject = noteSubject(subject);
    e.slot = slot;
    e.type = type;
    e.state = state;
    e.episode = QUuid::createUuid();
    return e;
}

inline UtcTime noteAt(int h, int m)
{
    return UtcTime::fromIso(QStringLiteral("2026-10-18T%1:%2:00Z").arg(h, 2, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')));
}

inline QString noteZoneTime(const UtcTime &t)
{
    return QDateTime::fromMSecsSinceEpoch(t.msecsSinceEpoch(), Qt::UTC).toString(QStringLiteral("HH:mm"));
}


// Compiles the translation file of a language with lrelease, the way the package does, and loads it
// as the service's translator. LM_TRANSLATIONS_DIR is the app's folder, defined by the test's .pro.
inline bool loadNoteLanguage(QTemporaryDir *dir, NotificationTranslator *translator, const char *language)
{
    const QString base = QStringLiteral("harbour-libre-monitor");
    const bool english = QByteArray(language) == "en";
    const QString file = english ? base : base + QLatin1Char('-') + QLatin1String(language);
    const QStringList names = english ? QStringList() << base : QStringList() << base << file;
    for (int i = 0; i < names.size(); ++i) {
        if (QFile::exists(dir->path() + QLatin1Char('/') + names.at(i) + QStringLiteral(".qm")))
            continue;
        QProcess p;
        p.start(QStringLiteral("lrelease"), QStringList() << QStringLiteral("-idbased")
                << QString::fromLatin1(LM_TRANSLATIONS_DIR) + QLatin1Char('/') + names.at(i) + QStringLiteral(".ts")
                << QStringLiteral("-qm") << dir->path() + QLatin1Char('/') + names.at(i) + QStringLiteral(".qm"));
        if (!p.waitForFinished(30000) || p.exitCode() != 0)
            return false;
    }
    return translator->load(dir->path(), base, QString::fromLatin1(language)) == QString::fromLatin1(language);
}

// A presenter on fakes with two synthetic Subjects, and the calls a script of effects needs.
struct NoteRig
{
    NoteRig() : presenter(&backend, &settings, &labels, &clock)
    {
        QHash<QUuid, QString> l;
        l.insert(noteSubject(1), QStringLiteral("AB"));
        l.insert(noteSubject(2), QStringLiteral("CD"));
        labels.set(QList<QUuid>() << noteSubject(1) << noteSubject(2), l);
        presenter.setTexts(NotificationTexts(noteZoneTime));
        presenter.setFactsReader(&facts);
    }
    // Sets the onset and silence of a Subject's slot, then delivers the effect.
    void send(AlertEffect::Kind kind, int subject, AlertSlot slot, AlertType type, AlertState state = AlertState::Active,
              int onsetMinute = 0, bool silent = false)
    {
        EffectFacts f;
        f.levelSince = noteAt(3, onsetMinute);
        f.episodeSince = f.levelSince;
        f.limit = type == AlertType::LL ? 54 : type == AlertType::L ? 70 : type == AlertType::H ? 180 : 250;
        f.lastReadingAt = noteAt(3, 0);
        f.silent = silent;
        facts.set(noteSubject(subject), slot, f);
        presenter.deliver(noteEffect(kind, subject, slot, type, state));
    }
    void notify(int subject, AlertSlot slot, AlertType type, int onset = 0, bool silent = false)
    { send(AlertEffect::Notify, subject, slot, type, AlertState::Active, onset, silent); }
    void remind(int subject, AlertSlot slot, AlertType type, int onset = 0, bool silent = false)
    { send(AlertEffect::Remind, subject, slot, type, AlertState::Active, onset, silent); }
    void acknowledge(int subject, AlertSlot slot, AlertType type, int onset = 0)
    { send(AlertEffect::Update, subject, slot, type, AlertState::Acknowledged, onset); }
    void clear(int subject, AlertSlot slot, AlertType type) { send(AlertEffect::Withdraw, subject, slot, type, AlertState::Cleared); }

    // The feedback of the newest post of a Subject's slot, "(none)" when it carried none, "-" when never posted.
    QString lastFeedback(int subject, AlertSlot slot) const
    {
        const quint32 id = presenter.idOf(noteSubject(subject), slot);
        for (int i = backend.posts.size() - 1; i >= 0; --i)
            if (backend.posts.at(i).id == id && id != 0)
                return backend.posts.at(i).request.hints.feedback.isEmpty() ? QStringLiteral("(none)") : backend.posts.at(i).request.hints.feedback;
        return QStringLiteral("-");
    }
    int postsOf(int subject, AlertSlot slot) const
    {
        const quint32 id = presenter.idOf(noteSubject(subject), slot);
        int n = 0;
        for (int i = 0; i < backend.posts.size(); ++i)
            n += id != 0 && backend.posts.at(i).id == id;
        return n;
    }
    int clockPostsOf(int subject, AlertSlot slot) const
    {
        const quint32 id = presenter.idOf(noteSubject(subject), slot);
        int n = 0;
        for (int i = 0; i < backend.posts.size(); ++i)
            n += backend.posts.at(i).id == id && backend.posts.at(i).request.hints.feedback == QLatin1String("clock");
        return n;
    }
    bool holds(int subject, AlertSlot slot) const
    {
        QUuid s; AlertSlot sl;
        return presenter.toneHolder(&s, &sl) && s == noteSubject(subject) && sl == slot;
    }
    bool noHolder() const { QUuid s; AlertSlot sl; return !presenter.toneHolder(&s, &sl); }

    FakeNoteBackend backend;
    MemoryNoteSettings settings;
    SubjectLabelBook labels;
    NoteClock clock;
    FakeFacts facts;
    AlertNotificationPresenter presenter;
};

} // namespace testing
} // namespace lm

#endif
