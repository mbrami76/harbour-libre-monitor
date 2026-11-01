// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the real data source of the interface (app/real): reading the service's Status(), the
// read-only store queries, and turning both into the snapshots the screens use. What they
// prove: an unknown Status format is refused, the older Status without the fields still
// works, a Subject is never current when nobody answers for it, a trend arrow is shown only
// with its own reading, and the plot series are exact mg/dL read without writing to the store.
#include "daemon/dbusservice.h"
#include "daemon/provisioning.h"
#include "real/realaccountbackend.h"
#include "real/realalertsbackend.h"
#include "real/realbackend.h"
#include "real/servicelink.h"
#include "real/snapshotbuilder.h"
#include "ui/configstore.h"
#include "real/statusparser.h"
#include "real/storequeries.h"
#include "real/realmaintenancebackend.h"
#include "record/record.h"
#include "store/compactor.h"
#include "store/store.h"
#include "ui/band.h"
#include "ui/alertview.h"
#include "ui/subjectmodel.h"
#include "ui/configsettings.h"
#include "ui/thresholdeditor.h"
#include "ui/thresholdview.h"
#include "ui/sensorview.h"
#include "ui/monitoringinfo.h"
#include "alerts/alertjournal.h"
#include "store/journal.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QJsonDocument>
#include <QtTest>

using namespace lm;
using namespace lm::real;

namespace {

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A stand-in for the monitoring service on the test's own bus: the alert methods of
// with canned answers, and their two signals. It lets the test check what
// the real alerts backend asks and when, through real D-Bus traffic between two connections, without
// the whole service. The integration run with the host service and the simulator is
// tests/integration/run_ui_alerts_integration.py.
class StandInService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "it.michelebrami.libre_monitor.Service")
public:
    QStringList calls;
    QString alertsJson = QStringLiteral(
        "{\"format\":1,\"do_not_disturb\":false,\"alerts\":[{\"subject\":\"{s1}\",\"slot\":\"low\","
        "\"level\":\"LL\",\"state\":\"active\",\"acknowledged\":false,\"episode\":\"{e}\","
        "\"since\":\"2026-10-19T08:00:00Z\",\"level_since\":\"2026-10-19T08:00:00Z\",\"limit\":54,"
        "\"snooze_until\":\"\",\"cause\":\"\"}],\"thresholds\":[{\"subject\":\"{s1}\",\"ll\":54,\"l\":70,"
        "\"h\":180,\"hh\":250,\"silent\":false,\"silent_until\":\"\"}],\"limits\":{}}");
public slots:
    QString Alerts() { calls << QStringLiteral("Alerts"); return alertsJson; }
    QString AcknowledgeAlert(const QString &s, const QString &slot) { calls << QStringLiteral("Ack ") + s + QLatin1Char(' ') + slot; return QStringLiteral("done"); }
    QString SnoozeAlert(const QString &s, const QString &slot, int m) { calls << QStringLiteral("Snooze %1 %2 %3").arg(s, slot).arg(m); return QStringLiteral("not-snoozable"); }
    QString SetSilent(const QString &s, int m) { calls << QStringLiteral("SetSilent %1 %2").arg(s).arg(m); return QStringLiteral("{\"result\":\"done\",\"silent_until\":\"2026-10-20T04:00:00Z\"}"); }
    QString EndSilent(const QString &s) { calls << QStringLiteral("EndSilent ") + s; return QStringLiteral("not-silent"); }
    QString SetThresholds(const QString &s, int l, int h, int tl, int th)       // no comment
    {
        calls << QStringLiteral("SetThresholds %1 %2 %3 %4 %5").arg(s).arg(l).arg(h).arg(tl).arg(th);
        return QStringLiteral("{\"result\":\"inconsistent\",\"l_min\":55,\"l_max\":59,\"h_min\":61,\"h_max\":249,\"offending\":\"L\"}");
    }
signals:
    void AlertChanged(const QString &subject, const QString &slot, const QString &state);
    void SilentChanged(const QString &subject, bool silent, const QString &untilIso);
};

// A stand-in for an OLDER monitoring service: it has Status() and Stop()
// but no alert method at all, as the 0.1-1 service, and its Status() may or may not carry `app_version`
// A call of a method it does not have is answered by the bus
// library with org.freedesktop.DBus.Error.UnknownMethod, as on the phone.
class OlderStandInService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "it.michelebrami.libre_monitor.Service")
public:
    QStringList calls;
    QString statusJson;
public slots:
    QString Status() { calls << QStringLiteral("Status"); return statusJson; }
    void Stop() { calls << QStringLiteral("Stop"); }
};

int countOf(const QStringList &calls, const QString &what)
{
    int n = 0;
    for (int i = 0; i < calls.size(); ++i)
        if (calls.at(i) == what)
            ++n;
    return n;
}

const char *kStatus =
    "{\"format\":1,\"service\":{\"state\":\"running\",\"notice\":\"active\","
    "\"last_heartbeat\":\"2026-10-17T10:00:00.000Z\",\"start_cause\":\"first-start\",\"waiting_connections\":[]},"
    "\"subjects\":[{\"subject\":\"{11111111-1111-4111-8111-111111111111}\",\"connection\":\"c1\",\"state\":\"idle\","
    "\"cause\":\"\",\"retry_at\":\"\",\"last_success_at\":\"2026-10-17T09:59:30.000Z\",\"last_contact_ok_at\":\"\","
    "\"stale\":false,\"stale_cause\":\"\",\"stale_detail\":\"\",\"stale_since\":\"\"}],"
    "\"maintenance\":{\"running\":false,\"kind\":\"\"}}";

qint64 ms(const char *iso) { return UtcTime::fromIso(QString::fromLatin1(iso)).msecsSinceEpoch(); }

Record sample(const QUuid &subj, const char *source, double value, const char *unit, const char *when)
{
    Record r;
    r.shape = Record::Sample;
    r.subject = subj;
    r.metric = "glucose";
    r.value = value;
    r.unit = unit;
    r.measuredAt = UtcTime::fromIso(QString::fromLatin1(when));
    r.ingestedAt = r.measuredAt;
    r.originalTimestamp = "1/1/2026 1:00:00 AM";
    r.source = source;
    r.origin = Origin::Adapter;
    return r;
}


// A secret store that remembers what was asked of it.
class FakeWriter : public CredentialWriter
{
public:
    FakeWriter() : failStore(false) {}
    bool store(const QString &id, const QString &user, const QString &pass, QString *err)
    {
        calls << "store:" + id;
        if (failStore) { *err = "x"; return false; }
        secrets[id] = user + "\n" + pass;
        return true;
    }
    bool remove(const QString &id, QString *)
    {
        calls << "remove:" + id;
        secrets.remove(id);
        return true;
    }
    QMap<QString, QString> secrets;
    QStringList calls;
    bool failStore;
};

// A service that answers as scripted and records what it was told.
class FakePort : public AccountServicePort
{
public:
    FakePort() : configResult("") {}
    QString discover(const QString &c, QString *err)
    {
        calls << "discover:" + c;
        *err = discoverError;
        return discoverJson;
    }
    bool credentialsChanged(const QString &c) { calls << "credentialsChanged:" + c; return true; }
    bool configChanged(QString *r) { calls << "configChanged"; *r = configResult; return true; }
    void stop() { calls << "stop"; }
    bool waitStopped(int) { calls << "waitStopped"; return true; }
    QString start() { calls << "start"; return "running"; }
    QString discoverJson, discoverError, configResult;
    QStringList calls;
};

}

class TstUiReal : public QObject
{
    Q_OBJECT

private slots:
    // ---- the Status() parser --------------------------------------------------------------------
    void parsesTheCurrentStatus()
    {
        const ServiceReport r = parseServiceStatus(kStatus);
        QVERIFY(r.ok);
        QCOMPARE(r.state, QStringLiteral("running"));
        QCOMPARE(r.lastHeartbeatMs, ms("2026-10-17T10:00:00.000Z"));
        QCOMPARE(r.subjects.size(), 1);
        const SubjectReport *s = r.subject("{11111111-1111-4111-8111-111111111111}");
        QVERIFY(s);
        QCOMPARE(s->connection, QStringLiteral("c1"));
        QVERIFY(!s->stale);
        QCOMPARE(s->lastSuccessAtMs, ms("2026-10-17T09:59:30.000Z"));
        QCOMPARE(s->staleSinceMs, qint64(-1));           // empty text means none
        QVERIFY(!s->hasTrend);                           // the older Status has no fields
        QVERIFY(!r.maintenanceRunning);
    }
    void parsesTheD64Additions()
    {
        QByteArray j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"adapter\":\"libre\",\"adapter_version\":\"0.1\","
                  "\"report_proposed\":true,\"trend_code\":4,\"trend_for\":\"2026-10-17T09:59:30.000Z\"}");
        const ServiceReport r = parseServiceStatus(j);
        QVERIFY(r.ok);
        const SubjectReport *s = r.subjects.first().subject.isEmpty() ? 0 : &r.subjects.first();
        QVERIFY(s);
        QCOMPARE(s->adapter, QStringLiteral("libre"));
        QCOMPARE(s->adapterVersion, QStringLiteral("0.1"));
        QVERIFY(s->reportProposed);
        QVERIFY(s->hasTrend);
        QCOMPARE(s->trendCode, 4);
        QCOMPARE(s->trendForMs, ms("2026-10-17T09:59:30.000Z"));
    }
    void parsesTheSensorObject()
    {
        QByteArray j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"sensor\":{\"id\":\"abc123\",\"activated_at\":\"2026-10-10T08:00:00.000Z\",\"warmup_minutes\":60}}");
        SubjectReport s = parseServiceStatus(j).subjects.first();
        QVERIFY(s.hasSensor);
        QCOMPARE(s.sensorId, QStringLiteral("abc123"));
        QCOMPARE(s.sensorActivatedMs, ms("2026-10-10T08:00:00.000Z"));
        QCOMPARE(s.sensorWarmupMinutes, 60);
        // Only the keys that are known: the others stay at "unknown".
        j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"sensor\":{\"id\":\"x\"}}");
        s = parseServiceStatus(j).subjects.first();
        QVERIFY(s.hasSensor && s.sensorActivatedMs == -1 && s.sensorWarmupMinutes == -1);
        // null means no sensor stored.
        j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"sensor\":null}");
        QVERIFY(!parseServiceStatus(j).subjects.first().hasSensor);
    }
    void aTrendIsNeverShownForAStaleSubjectAndTheReportFlagTravels()
    {
        const QString id = "{11111111-1111-4111-8111-111111111111}";
        QList<ConfigSubject> cfg; ConfigSubject c; c.id = id; c.name = "Anna"; cfg << c;
        QByteArray j = kStatus;
        j.replace("\"stale\":false", "\"stale\":true");
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"report_proposed\":true,\"trend_code\":4,\"trend_for\":\"2026-10-17T09:59:30.000Z\"}");
        const ServiceReport rep = parseServiceStatus(j);
        SubjectData d; d.hasReading = true; d.value = 6.2; d.unit = "mmol/L"; d.measuredAtMs = ms("2026-10-17T09:59:30.000Z");
        QHash<QString, SubjectData> data; data.insert(id, d);
        const QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, &rep, data, QVector<QPair<qint64, qint64> >(), ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(!s[0].hasTrend);                                   // the reading belongs to a stale Subject
        QVERIFY(s[0].reportProposed);
    }
    void aNullTrendIsNoTrend()
    {
        QByteArray j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"trend_code\":null}");
        QVERIFY(!parseServiceStatus(j).subjects.first().hasTrend);
    }
    void aNewerFormatAndGarbageAreRefusedByKey()
    {
        QByteArray j = kStatus;
        j.replace("\"format\":1", "\"format\":2");
        ServiceReport r = parseServiceStatus(j);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("format-newer"));
        r = parseServiceStatus("not json");
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("malformed"));
        QVERIFY(!parseServiceStatus("[1,2]").ok);
        QVERIFY(!parseServiceStatus("").ok);
    }
    void unknownKeysAreIgnored()
    {
        QByteArray j = kStatus;
        j.replace("\"format\":1,", "\"format\":1,\"future\":{\"x\":1},");
        QVERIFY(parseServiceStatus(j).ok);
    }

    // ---- the snapshots ----------------------------------------------------------------------------
    void aSubjectGetsItsReportAndItsReading()
    {
        const QString id = "{11111111-1111-4111-8111-111111111111}";
        QList<ConfigSubject> cfg;
        ConfigSubject c; c.id = id; c.name = "Anna"; c.label = "A"; c.connection = "c1"; cfg << c;
        const ServiceReport rep = parseServiceStatus(kStatus);
        SubjectData d;
        d.hasReading = true; d.measuredAtMs = ms("2026-10-17T09:59:30.000Z"); d.value = 6.2; d.unit = "mmol/L";
        d.current << ui::PlotPoint(d.measuredAtMs, 111.7); d.firstDataMs = d.measuredAtMs - 1000;
        QHash<QString, SubjectData> data; data.insert(id, d);
        const QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, &rep, data, QVector<QPair<qint64, qint64> >(),
                                                            ms("2026-10-17T10:00:00.000Z"));
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].name, QStringLiteral("Anna"));
        QCOMPARE(s[0].report.state, QStringLiteral("idle"));
        QVERIFY(!s[0].report.stale);
        QVERIFY(s[0].hasReading);
        QCOMPARE(s[0].unit, QStringLiteral("mmol/L"));
        QCOMPARE(s[0].current.size(), 1);
        // The screens turn it into a band-coloured, current reading.
        const ui::SubjectView v = ui::viewOfSnapshot(s[0], ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(ui::isFresh(v.id));
    }
    void withoutAnAnswerEverySubjectIsNotMonitoredAndNeverCurrent()
    {
        const QString id = "{11111111-1111-4111-8111-111111111111}";
        QList<ConfigSubject> cfg; ConfigSubject c; c.id = id; c.name = "Anna"; cfg << c;
        SubjectData d; d.hasReading = true; d.measuredAtMs = ms("2026-10-17T09:59:30.000Z"); d.value = 6.2; d.unit = "mmol/L";
        QHash<QString, SubjectData> data; data.insert(id, d);
        const QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, 0, data, QVector<QPair<qint64, qint64> >(),
                                                            ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(s[0].report.stale);
        QCOMPARE(s[0].report.staleCause, QStringLiteral("service-not-running"));
        const ui::SubjectView v = ui::viewOfSnapshot(s[0], ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(!ui::isFresh(v.id));                               // a recent value is still not current
    }
    void aSubjectTheServiceDoesNotKnowYetIsWaitingNotBroken()
    {
        QList<ConfigSubject> cfg; ConfigSubject c; c.id = "{22222222-2222-4222-8222-222222222222}"; c.name = "New"; cfg << c;
        const ServiceReport rep = parseServiceStatus(kStatus);
        const QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, &rep, QHash<QString, SubjectData>(),
                                                            QVector<QPair<qint64, qint64> >(), ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(!s[0].report.stale);
        QCOMPARE(ui::viewOfSnapshot(s[0], ms("2026-10-17T10:00:00.000Z")).id, ui::StateId::NoDataYet);
    }
    void aTrendIsKeptOnlyWithItsOwnReading()
    {
        const QString id = "{11111111-1111-4111-8111-111111111111}";
        QList<ConfigSubject> cfg; ConfigSubject c; c.id = id; c.name = "Anna"; cfg << c;
        QByteArray j = kStatus;
        j.replace("\"stale_since\":\"\"}", "\"stale_since\":\"\",\"trend_code\":4,\"trend_for\":\"2026-10-17T09:59:30.000Z\"}");
        const ServiceReport rep = parseServiceStatus(j);
        SubjectData d; d.hasReading = true; d.value = 6.2; d.unit = "mmol/L";
        d.measuredAtMs = ms("2026-10-17T09:59:30.000Z");
        QHash<QString, SubjectData> data; data.insert(id, d);
        QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, &rep, data, QVector<QPair<qint64, qint64> >(), ms("2026-10-17T10:00:00.000Z"));
        QVERIFY(s[0].hasTrend);
        QCOMPARE(s[0].trendCode, 4);
        // A newer reading is in the store: the arrow belongs to the older one and is dropped.
        data[id].measuredAtMs = ms("2026-10-17T10:00:30.000Z");
        s = buildSnapshots(cfg, &rep, data, QVector<QPair<qint64, qint64> >(), ms("2026-10-17T10:01:00.000Z"));
        QVERIFY(!s[0].hasTrend);
    }
    void theOrderIsTheConfigurationOrder()
    {
        QList<ConfigSubject> cfg;
        for (int i = 0; i < 3; ++i) { ConfigSubject c; c.id = QString("{0000000%1-0000-4000-8000-000000000000}").arg(i); c.name = QString::number(i); cfg << c; }
        const QList<ui::SubjectSnapshot> s = buildSnapshots(cfg, 0, QHash<QString, SubjectData>(), QVector<QPair<qint64, qint64> >(), 0);
        QCOMPARE(s.size(), 3);
        for (int i = 0; i < 3; ++i) QCOMPARE(s[i].name, QString::number(i));
    }
    void theServiceStripFollowsTheReport()
    {
        const ServiceReport rep = parseServiceStatus(kStatus);
        QCOMPARE(serviceStatusOf(&rep, true, true, false), ui::ServiceStatus::Running);
        QCOMPARE(serviceStatusOf(0, false, false, false), ui::ServiceStatus::Connecting);
        QCOMPARE(serviceStatusOf(&rep, true, false, false), ui::ServiceStatus::Stopped);   // the name was lost
        QCOMPARE(serviceStatusOf(&rep, true, true, true), ui::ServiceStatus::CallFailed);
        ServiceReport w = rep; w.state = "running-awaiting-approval";
        QCOMPARE(serviceStatusOf(&w, true, true, false), ui::ServiceStatus::AwaitingApproval);
        w.state = "starting";
        QCOMPARE(serviceStatusOf(&w, true, true, false), ui::ServiceStatus::Starting);
        w.state = "stopping";
        QCOMPARE(serviceStatusOf(&w, true, true, false), ui::ServiceStatus::Stopped);
    }

    // ---- an older service ---------------------------------------
    void theServiceVersionIsReadAndAnAbsentOneIsEmpty()
    {
        QCOMPARE(parseServiceStatus(kStatus).appVersion, QString());           // a service before 
        QByteArray j = kStatus;
        j.replace("\"start_cause\"", "\"app_version\":\"0.1-3\",\"start_cause\"");
        const ServiceReport r = parseServiceStatus(j);
        QVERIFY(r.ok);
        QCOMPARE(r.appVersion, QStringLiteral("0.1-3"));
    }
    void versionsCompareAsNumbers()
    {
        QCOMPARE(compareAppVersions("0.1-3", "0.1-3"), 0);
        QCOMPARE(compareAppVersions("0.1-3", "0.1-4"), -1);
        QCOMPARE(compareAppVersions("0.1-10", "0.1-9"), 1);          // numbers, not text
        QCOMPARE(compareAppVersions("0.2-1", "0.1-9"), 1);
        // The first stable release: a major jump, three parts against two, counts as newer
        // than every 0.1 package, including a two-digit release.
        QCOMPARE(compareAppVersions("1.0.0-1", "0.1-3"), 1);
        QCOMPARE(compareAppVersions("1.0.0-1", "0.1-10"), 1);
        QCOMPARE(compareAppVersions("0.1-10", "1.0.0-1"), -1);
        QCOMPARE(compareAppVersions("1.0.0-1", "1.0.0-1"), 0);
        QCOMPARE(serviceVersionRelation(0, "1.0.0-1"), VersionRelation::Same);
        QCOMPARE(compareAppVersions("1.0", "0.9-9"), 1);
        QCOMPARE(compareAppVersions("0.1", "0.1-0"), 0);             // a missing part is 0
        QCOMPARE(compareAppVersions("0.1-3", "0.1-3a"), -2);         // not numbers only: no order
        QCOMPARE(compareAppVersions("", "0.1-3"), -2);
        QCOMPARE(compareAppVersions("0.1--3", "0.1-3"), -2);
    }
    // X1: older (lower, absent, not comparable), newer only when it compares higher.
    void theServiceIsOlderNewerOrTheSame()
    {
        ServiceReport r = parseServiceStatus(kStatus);
        QCOMPARE(serviceVersionRelation(&r, "0.1-3"), VersionRelation::Older);   // no app_version
        r.appVersion = QStringLiteral("0.1-3");
        QCOMPARE(serviceVersionRelation(&r, "0.1-3"), VersionRelation::Same);
        r.appVersion = QStringLiteral("0.1-2");
        QCOMPARE(serviceVersionRelation(&r, "0.1-3"), VersionRelation::Older);
        r.appVersion = QStringLiteral("0.1-4");
        QCOMPARE(serviceVersionRelation(&r, "0.1-3"), VersionRelation::Newer);
        r.appVersion = QStringLiteral("0.1-10");
        QCOMPARE(serviceVersionRelation(&r, "0.1-9"), VersionRelation::Newer);
        r.appVersion = QStringLiteral("0.1-3x");
        QCOMPARE(serviceVersionRelation(&r, "0.1-3"), VersionRelation::Older);   // not comparable: the safe advice
        QCOMPARE(serviceVersionRelation(&r, QString()), VersionRelation::Same);  // a window without a version
        QCOMPARE(serviceVersionRelation(0, "0.1-3"), VersionRelation::Same);     // nobody answered
    }
    void theStripShowsAnOlderOrNewerServiceOnlyWhileItIsAlive()
    {
        const ServiceReport rep = parseServiceStatus(kStatus);
        const VersionRelation older = VersionRelation::Older, newer = VersionRelation::Newer;
        QCOMPARE(serviceStatusOf(&rep, true, true, false, older), ui::ServiceStatus::ServiceOlder);
        QCOMPARE(serviceStatusOf(&rep, true, true, false, newer), ui::ServiceStatus::ServiceNewer);
        ServiceReport w = rep;
        w.state = "starting";
        QCOMPARE(serviceStatusOf(&w, true, true, false, older), ui::ServiceStatus::ServiceOlder);
        QCOMPARE(serviceStatusOf(&w, true, true, false, newer), ui::ServiceStatus::ServiceNewer);
        w.state = "running-awaiting-approval";
        QCOMPARE(serviceStatusOf(&w, true, true, false, older), ui::ServiceStatus::ServiceOlder);
        w.state = "stopping";                                                   // about to be gone
        QCOMPARE(serviceStatusOf(&w, true, true, false, older), ui::ServiceStatus::Stopped);
        QCOMPARE(serviceStatusOf(&w, true, true, false, newer), ui::ServiceStatus::Stopped);
        QCOMPARE(serviceStatusOf(&rep, true, false, false, older), ui::ServiceStatus::Stopped);   // name lost
        QCOMPARE(serviceStatusOf(&rep, true, true, true, newer), ui::ServiceStatus::CallFailed);  // nothing known
        QCOMPARE(serviceStatusOf(0, false, false, false, older), ui::ServiceStatus::Connecting);
        QCOMPARE(serviceStatusOf(&rep, true, true, false), ui::ServiceStatus::Running);
    }
    void anOlderServiceHasItsOwnWordsOnThePages()
    {
        QCOMPARE(ui::alertResultKey(QStringLiteral("service-older")), QStringLiteral("ui-service-older"));
        QCOMPARE(ui::alertResultKey(QStringLiteral("call-failed")), QStringLiteral("ui-alert-err-no-service"));
        ui::ThresholdAnswer a;
        a.result = QStringLiteral("service-older");
        const ui::ThresholdMessage m = ui::thresholdMessage(a, QStringLiteral("mg/dL"), QLocale::c());
        QVERIFY(!m.success);
        QCOMPARE(m.parts.size(), 1);
        QCOMPARE(m.parts.at(0).first, QStringLiteral("ui-service-older"));
    }

    // ---- the store queries ------------------------------------------------------------------------------
    void readsBothFeedsAsExactMgDlWithoutWriting()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        const QUuid subj = QUuid("{11111111-1111-4111-8111-111111111111}");
        {
            Store w;
            QVERIFY(w.open(path));
            IngestContext ctx;
            ctx.now = UtcTime::fromIso("2026-10-17T10:05:00.000Z");
            ctx.adapter = "libre"; ctx.adapterVersion = "0.1"; ctx.sessionId = "s";
            QVector<IngestItem> items;
            const char *cur[] = { "2026-10-17T09:57:00.000Z", "2026-10-17T09:58:00.000Z", "2026-10-17T09:59:00.000Z" };
            for (int i = 0; i < 3; ++i) { IngestItem it; it.record = sample(subj, "libre.current", 6.0 + i * 0.1, "mmol/L", cur[i]); items << it; }
            IngestItem h; h.record = sample(subj, "libre.graph", 108, "mg/dL", "2026-10-17T09:40:00.000Z"); items << h;
            QVERIFY(w.ingest(items, ctx).ok);
        }
        StoreQueries q;
        QVERIFY(q.open(path, dir.path() + "/journal.db"));       // a missing journal is not fatal
        const SubjectData d = q.load(subj, ms("2026-10-17T00:00:00.000Z"));
        QVERIFY(d.hasReading);
        QCOMPARE(d.measuredAtMs, ms("2026-10-17T09:59:00.000Z"));
        QCOMPARE(d.unit, QStringLiteral("mmol/L"));
        QCOMPARE(d.value, 6.2);                                    // the stored value, not converted
        QCOMPARE(d.current.size(), 3);
        QCOMPARE(d.history.size(), 1);
        QVERIFY(qAbs(d.current[2].mgdl - 6.2 * 18.0156) < 1e-9);    // exact, not rounded
        QCOMPARE(d.history[0].mgdl, 108.0);
        QVERIFY(d.firstDataMs > 0);
        QVERIFY(d.current[0].tMs < d.current[1].tMs);              // sorted by time
    }
    // the feeds stay separate. After the connection to the source was lost the graph feed can
    // hold readings NEWER than the last `current` one (the source sends the graph of the gap when it is
    // reached again); the big reading and the cover must still come from `current` only, and the graph
    // readings go only into the plot's history series.
    void theBigReadingComesFromTheCurrentFeedEvenWhenTheGraphIsNewer()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        const QUuid subj = QUuid("{11111111-1111-4111-8111-111111111111}");
        {
            Store w;
            QVERIFY(w.open(path));
            IngestContext ctx;
            ctx.now = UtcTime::fromIso("2026-10-17T11:00:00.000Z");
            ctx.adapter = "libre"; ctx.adapterVersion = "0.1"; ctx.sessionId = "s";
            QVector<IngestItem> items;
            IngestItem c; c.record = sample(subj, "libre.current", 100, "mg/dL", "2026-10-17T10:00:00.000Z"); items << c;
            const char *graph[] = { "2026-10-17T10:15:00.000Z", "2026-10-17T10:30:00.000Z", "2026-10-17T10:45:00.000Z" };
            for (int i = 0; i < 3; ++i) { IngestItem h; h.record = sample(subj, "libre.graph", 120 + i, "mg/dL", graph[i]); items << h; }
            QVERIFY(w.ingest(items, ctx).ok);
        }
        StoreQueries q;
        QVERIFY(q.open(path, dir.path() + "/journal.db"));
        const SubjectData d = q.load(subj, ms("2026-10-17T00:00:00.000Z"));
        QVERIFY(d.hasReading);
        QCOMPARE(d.measuredAtMs, ms("2026-10-17T10:00:00.000Z"));  // the current feed's, not the graph's newer one
        QCOMPARE(d.value, 100.0);
        QCOMPARE(d.current.size(), 1);
        QCOMPARE(d.history.size(), 3);                             // the graph of the gap is in the plot
    }
    void refreshAppendsOnlyWhatIsNew()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        const QUuid subj = QUuid("{11111111-1111-4111-8111-111111111111}");
        Store w;
        QVERIFY(w.open(path));
        IngestContext ctx;
        ctx.now = UtcTime::fromIso("2026-10-17T10:05:00.000Z");
        ctx.adapter = "libre"; ctx.adapterVersion = "0.1"; ctx.sessionId = "s";
        QVector<IngestItem> a; IngestItem i1; i1.record = sample(subj, "libre.current", 100, "mg/dL", "2026-10-17T09:57:00.000Z"); a << i1;
        QVERIFY(w.ingest(a, ctx).ok);
        StoreQueries q;
        QVERIFY(q.open(path, dir.path() + "/journal.db"));
        SubjectData d = q.load(subj, ms("2026-10-17T00:00:00.000Z"));
        QCOMPARE(d.current.size(), 1);
        QVector<IngestItem> b; IngestItem i2; i2.record = sample(subj, "libre.current", 101, "mg/dL", "2026-10-17T09:58:00.000Z"); b << i2;
        QVERIFY(w.ingest(b, ctx).ok);
        q.refresh(subj, &d);
        QCOMPARE(d.current.size(), 2);
        QCOMPARE(d.value, 101.0);
        q.refresh(subj, &d);                                       // nothing new: nothing added
        QCOMPARE(d.current.size(), 2);
    }
    // (REQ-SEN-01): the sensor block reads the store's sensor table read-only and
    // picks the most recently seen sensor of the patient, as the service's Status() does; another
    // patient's sensor is not taken; a refresh brings a newer sensor; the file is not changed.
    void theSensorComesFromTheStoreReadOnlyAndTheNewestSeenWins()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        const QUuid subj = QUuid("{11111111-1111-4111-8111-111111111111}");
        const QUuid other = QUuid("{22222222-2222-4222-8222-222222222222}");
        Store w;
        QVERIFY(w.open(path));
        IngestContext ctx;
        ctx.now = UtcTime::fromIso("2026-10-17T10:05:00.000Z");
        ctx.adapter = "libre"; ctx.adapterVersion = "0.1"; ctx.sessionId = "s";
        QVector<IngestItem> a; IngestItem i1; i1.record = sample(subj, "libre.current", 100, "mg/dL", "2026-10-17T09:57:00.000Z"); a << i1;
        IngestItem i2; i2.record = sample(other, "libre.current", 100, "mg/dL", "2026-10-17T09:57:00.000Z"); a << i2;
        QVERIFY(w.ingest(a, ctx).ok);
        Sensor old; old.subject = subj; old.sensorId = "synthetic-old"; old.activatedAt = UtcTime::fromIso("2026-10-01T08:00:00.000Z");
        old.warmupMinutes = 60; old.firstSeen = UtcTime::fromIso("2026-10-01T09:00:00.000Z"); old.lastSeen = UtcTime::fromIso("2026-10-14T08:00:00.000Z");
        QVERIFY(w.noteSensor(old));
        Sensor theirs = old; theirs.subject = other; theirs.sensorId = "synthetic-other"; theirs.lastSeen = UtcTime::fromIso("2026-10-17T10:00:00.000Z");
        QVERIFY(w.noteSensor(theirs));
        StoreQueries q;
        QVERIFY(q.open(path, dir.path() + "/journal.db"));
        SubjectData d = q.load(subj, ms("2026-10-17T00:00:00.000Z"));
        QVERIFY(d.sensor.known);
        QCOMPARE(d.sensor.id, QStringLiteral("synthetic-old"));
        QCOMPARE(d.sensor.activatedMs, ms("2026-10-01T08:00:00.000Z"));
        QCOMPARE(d.sensor.warmupMinutes, 60);
        QCOMPARE(d.sensor.firstSeenMs, ms("2026-10-01T09:00:00.000Z"));
        QCOMPARE(d.sensor.lastSeenMs, ms("2026-10-14T08:00:00.000Z"));
        Sensor fresh; fresh.subject = subj; fresh.sensorId = "synthetic-new"; fresh.activatedAt = UtcTime::fromIso("2026-10-17T09:30:00.000Z");
        fresh.warmupMinutes = 60; fresh.firstSeen = fresh.lastSeen = UtcTime::fromIso("2026-10-17T10:00:00.000Z");
        QVERIFY(w.noteSensor(fresh));
        const QByteArray before = readAll(path);
        q.refresh(subj, &d);
        QCOMPARE(d.sensor.id, QStringLiteral("synthetic-new"));
        QCOMPARE(readAll(path), before);                           // reading changed nothing
        // The page never gets the stored identifier: only its short hash.
        const QVariantMap m = ui::sensorMap(d.sensor, ms("2026-10-17T10:10:00.000Z"));
        QVERIFY(m.value("warmingUp").toBool());
        QVERIFY(!QJsonDocument::fromVariant(m).toJson().contains("synthetic"));
    }
    // (REQ-STO-13, REQ-STO-16): the storage facts are read in the interface's own
    // process on read-only connections: sizes, free space, the dry-run estimate for a window and the rebuild
    // need; the store file is not changed; a missing store is said, not invented.
    void theStorageFactsAreReadReadOnly()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        const QUuid subj = QUuid("{11111111-1111-4111-8111-111111111111}");
        {
            Store w;
            QVERIFY(w.open(path));
            IngestContext ctx;
            ctx.now = UtcTime::fromIso("2026-10-17T10:05:00.000Z");
            ctx.adapter = "libre"; ctx.adapterVersion = "0.1"; ctx.sessionId = "s";
            QVector<IngestItem> items;
            // 60 runs two days apart from June to the end of September, ten one-minute readings of the
            // current feed in each: thinning keeps one per 5-minute slot, so the estimate has
            // something to count (the graph feed would never be thinned).
            for (int d = 0; d < 60; ++d)
                for (int k = 0; k < 10; ++k) {
                    IngestItem it;
                    const qint64 t = ms("2026-06-01T00:00:00.000Z") + qint64(d) * 2 * 86400000LL + qint64(k) * 60000LL;
                    it.record = sample(subj, "libre.current", 100 + k, "mg/dL", UtcTime::fromMSecsSinceEpoch(t).toIso().toLatin1().constData());
                    items << it;
                }
            QVERIFY(w.ingest(items, ctx).ok);
        }
        const QByteArray before = readAll(path);
        const qint64 now = ms("2026-10-17T10:10:00.000Z");
        const ui::StorageFacts f3 = StorageReader::readFacts(path, dir.path() + "/journal.db", 3, now);
        QVERIFY(f3.ok);
        QVERIFY(f3.storeBytes > 0);
        QVERIFY(f3.freeBytes > 0);
        QVERIFY(!f3.checkKnown);                                   // no journal: no weekly check yet
        QVERIFY(f3.estimateKnown);
        QVERIFY(f3.estimateRecords > 0);
        // REQ-STO-16: the rebuild need and the space check are the core's own (what remains plus the
        // margin, at least 1 MiB), not a figure of the window's.
        CompactionEstimate core;
        {
            Store ro;
            QVERIFY(ro.openReadOnly(path));
            QString err;
            QVERIFY(estimateCompaction(ro, FullResolutionWindow::ofMonths(3), UtcTime::fromMSecsSinceEpoch(now), &core, &err));
        }
        QCOMPARE(f3.rebuildNeedBytes, core.rebuildNeedBytes);
        QVERIFY(f3.rebuildNeedBytes >= kRebuildMarginMinimumBytes);
        QCOMPARE(f3.space, QStringLiteral("enough"));             // the PC has more than a mebibyte free
        QCOMPARE(f3.missingBytes, qint64(0));
        const ui::StorageFacts f0 = StorageReader::readFacts(path, dir.path() + "/journal.db", 0, now);
        QVERIFY(f0.estimateRecords > f3.estimateRecords);          // 14 days thins more than 3 months
        QVERIFY(f0.estimateBytes >= f3.estimateBytes);
        QCOMPARE(readAll(path), before);
        const ui::StorageFacts none = StorageReader::readFacts(dir.path() + "/none.db", dir.path() + "/none-j.db", 3, now);
        QVERIFY(!none.ok);
        QCOMPARE(none.error, QStringLiteral("store-unreadable"));
    }
    void aMissingStoreIsReportedNotInvented()
    {
        QTemporaryDir dir;
        StoreQueries q;
        QVERIFY(!q.open(dir.path() + "/none.db", dir.path() + "/none-journal.db"));
        QVERIFY(!q.isOpen());
        const SubjectData d = q.load(QUuid("{11111111-1111-4111-8111-111111111111}"), 0);
        QVERIFY(!d.hasReading && d.current.isEmpty());
        QVERIFY(q.serviceGaps().isEmpty());
    }
    // ---- the account backend --------------------------------------------------------------------------
    void discoverAnswersBecomeResults()
    {
        ui::LoginTestResult r = parseDiscoverAnswer(
            "{\"ok\":true,\"subjects\":[{\"subject_ref\":\"r1\",\"display_name\":\"Anna\",\"unit\":\"mmol/L\",\"unit_basis\":\"code\"},"
            "{\"subject_ref\":\"\",\"display_name\":\"nobody\"}]}", QString());
        QVERIFY(r.ok);
        QCOMPARE(r.subjects.size(), 1);                              // one without a reference is dropped
        QCOMPARE(r.subjects[0].unitBasis, QStringLiteral("code"));
        r = parseDiscoverAnswer("{\"ok\":false,\"error\":\"auth-failed\"}", QString());
        QVERIFY(!r.ok);
        QCOMPARE(r.errorKey, QStringLiteral("auth-failed"));
        r = parseDiscoverAnswer("{\"ok\":false,\"error\":\"something odd from the future\"}", QString());
        QCOMPARE(r.errorKey, QStringLiteral("internal"));            // never the service's own words
        r = parseDiscoverAnswer("", "not-available");
        QCOMPARE(r.errorKey, QStringLiteral("not-available"));
        QVERIFY(!parseDiscoverAnswer("garbage", QString()).ok);
    }
    void aTestStoresAProbeSecretAndAlwaysRemovesIt()
    {
        QTemporaryDir dir;
        FakeWriter w; FakePort p;
        p.discoverJson = "{\"ok\":true,\"subjects\":[{\"subject_ref\":\"r1\",\"display_name\":\"Anna\",\"unit\":\"mg/dL\",\"unit_basis\":\"relation\"}]}";
        RealAccountBackend b(&w, &p, dir.path() + "/config.json");
        qRegisterMetaType<lm::ui::LoginTestResult>();
        QSignalSpy spy(&b, SIGNAL(loginTested(lm::ui::LoginTestResult)));
        b.testLogin("me@example", "pw");
        QVERIFY(spy.wait(5000));
        const ui::LoginTestResult r = spy.at(0).at(0).value<ui::LoginTestResult>();
        QVERIFY(r.ok);
        QCOMPARE(r.subjects[0].displayName, QStringLiteral("Anna"));
        QCOMPARE(w.calls, QStringList() << "store:probe" << "remove:probe");
        QVERIFY(w.secrets.isEmpty());                                // nothing is left behind
        QCOMPARE(p.calls, QStringList() << "discover:probe");
        QVERIFY(!QFile::exists(dir.path() + "/config.json"));        // a test writes no configuration
        // And when the service has no Discover yet: the probe secret is still removed.
        FakeWriter w2; FakePort p2; p2.discoverError = "not-available";
        RealAccountBackend b2(&w2, &p2, dir.path() + "/config.json");
        QSignalSpy spy2(&b2, SIGNAL(loginTested(lm::ui::LoginTestResult)));
        b2.testLogin("me@example", "pw");
        QVERIFY(spy2.wait(5000));
        QCOMPARE(spy2.at(0).at(0).value<ui::LoginTestResult>().errorKey, QStringLiteral("not-available"));
        QVERIFY(w2.secrets.isEmpty());
    }
    void aFailingSecretStoreStopsTheTest()
    {
        QTemporaryDir dir;
        FakeWriter w; w.failStore = true; FakePort p;
        RealAccountBackend b(&w, &p, dir.path() + "/config.json");
        QSignalSpy spy(&b, SIGNAL(loginTested(lm::ui::LoginTestResult)));
        b.testLogin("me@example", "pw");
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).value<ui::LoginTestResult>().errorKey, QStringLiteral("secret-store-failed"));
        QVERIFY(p.calls.isEmpty());                                  // the service was never asked
    }
    void savingWritesTheSecretTheConfigurationAndTellsTheService()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/config.json";
        FakeWriter w; FakePort p;
        RealAccountBackend b(&w, &p, path);
        ui::SaveRequest req;
        req.connectionId = "c1"; req.username = "me@example"; req.password = "pw";
        ui::SaveSubject s; s.ref = "r1"; s.name = "Anna"; s.unit = "mmol/L"; s.unitBasis = "code"; req.subjects << s;
        qRegisterMetaType<lm::ui::SaveRequest>();
        QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
        b.save(req);
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.at(0).at(0).toBool());
        QCOMPARE(w.secrets["c1"], QStringLiteral("me@example\npw"));
        ui::ConfigStore c;
        QVERIFY(c.load(path));
        QCOMPARE(c.connections().size(), 1);
        QCOMPARE(c.connections()[0].adapter, QStringLiteral("libre"));
        QCOMPARE(c.connections()[0].subjects[0].name, QStringLiteral("Anna"));
        QVERIFY(c.connections()[0].subjects[0].unitConfirmed);
        QVERIFY(c.connections()[0].subjects[0].id.startsWith('{'));
        QCOMPARE(p.calls, QStringList() << "credentialsChanged:c1" << "configChanged");   // no restart asked for
        QCOMPARE(QFile::permissions(path) & (QFile::ReadGroup | QFile::ReadOther), QFileDevice::Permissions());
    }
    void aRestartIsRequestedOnlyWhenTheServiceSaysSo()
    {
        QTemporaryDir dir;
        FakeWriter w; FakePort p; p.configResult = "restart-needed";
        RealAccountBackend b(&w, &p, dir.path() + "/config.json");
        ui::SaveRequest req; req.connectionId = "c1"; req.username = "u"; req.password = "p";
        ui::SaveSubject s; s.ref = "r1"; s.name = "A"; s.unit = "mg/dL"; req.subjects << s;
        QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
        b.save(req);
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.at(0).at(0).toBool());
        QCOMPARE(p.calls, QStringList() << "credentialsChanged:c1" << "configChanged" << "stop" << "waitStopped" << "start");
    }
    void savingAgainKeepsTheUuidOfAKnownPerson()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/config.json";
        FakeWriter w; FakePort p;
        RealAccountBackend b(&w, &p, path);
        ui::SaveRequest req; req.connectionId = "c1"; req.username = "u"; req.password = "p";
        req.connectionLabel = "Account";                            // the page's default word
        ui::SaveSubject s; s.ref = "r1"; s.name = "Anna"; s.unit = "mmol/L"; req.subjects << s;
        QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
        b.save(req);
        QVERIFY(spy.wait(5000));
        ui::ConfigStore c1; c1.load(path);
        const QString first = c1.connections()[0].subjects[0].id;
        QCOMPARE(c1.connections()[0].label, QStringLiteral("Account"));
        // Round R3: the user renames the account and sets a patient's label and alert time in the settings;
        // saving the account again keeps all three.
        QVERIFY(c1.setConnectionLabel("c1", "Home"));
        QVERIFY(c1.setSubjectLabel(first, "AB"));
        QVERIFY(c1.setSubjectAlertTime(first, ui::AlertTimeKey::missingLimit(), 600));
        QVERIFY(c1.save());
        req.subjects[0].name = "Anna B.";                           // renamed, same person
        ui::SaveSubject s2; s2.ref = "r2"; s2.name = "Marco"; s2.unit = "mg/dL"; req.subjects << s2;
        b.save(req);
        QVERIFY(spy.wait(5000));
        ui::ConfigStore c2; c2.load(path);
        QCOMPARE(c2.connections()[0].subjects.size(), 2);
        QCOMPARE(c2.connections()[0].subjects[0].id, first);        // never reused, never renumbered
        QCOMPARE(c2.connections()[0].subjects[0].name, QStringLiteral("Anna B."));
        QVERIFY(c2.connections()[0].subjects[1].id != first);
        QCOMPARE(c2.connections()[0].label, QStringLiteral("Home"));
        QCOMPARE(c2.connections()[0].subjects[0].label, QStringLiteral("AB"));
        QCOMPARE(c2.connections()[0].subjects[0].missingLimitS, 600);
        QCOMPARE(c2.connections()[0].subjects[1].missingLimitS, -1);
    }
    void aFailedSecretOrConfigurationLeavesNothingHalfDone()
    {
        QTemporaryDir dir;
        // 1. the secret store fails: no configuration is written
        {
            FakeWriter w; w.failStore = true; FakePort p;
            RealAccountBackend b(&w, &p, dir.path() + "/config.json");
            ui::SaveRequest req; req.connectionId = "c1"; req.username = "u"; req.password = "p";
            ui::SaveSubject s; s.ref = "r1"; s.name = "A"; s.unit = "mg/dL"; req.subjects << s;
            QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
            b.save(req);
            QVERIFY(spy.wait(5000));
            QVERIFY(!spy.at(0).at(0).toBool());
            QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("secret-store-failed"));
            QVERIFY(!QFile::exists(dir.path() + "/config.json"));
            QVERIFY(p.calls.isEmpty());
        }
        // 2. a configuration file that is not valid is never overwritten, and no secret is stored
        {
            QFile f(dir.path() + "/bad.json"); f.open(QIODevice::WriteOnly); f.write("{not json"); f.close();
            FakeWriter w; FakePort p;
            RealAccountBackend b(&w, &p, dir.path() + "/bad.json");
            ui::SaveRequest req; req.connectionId = "c1"; req.username = "u"; req.password = "p";
            ui::SaveSubject s; s.ref = "r1"; s.name = "A"; s.unit = "mg/dL"; req.subjects << s;
            QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
            b.save(req);
            QVERIFY(spy.wait(5000));
            QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("config-invalid"));
            QVERIFY(w.secrets.isEmpty() && w.calls.isEmpty());
        }
        // 3. the configuration cannot be written (its folder does not exist): the secret is taken back
        {
            FakeWriter w; FakePort p;
            RealAccountBackend b(&w, &p, dir.path() + "/no/such/folder/config.json");
            ui::SaveRequest req; req.connectionId = "c1"; req.username = "u"; req.password = "p";
            ui::SaveSubject s; s.ref = "r1"; s.name = "A"; s.unit = "mg/dL"; req.subjects << s;
            QSignalSpy spy(&b, SIGNAL(saved(bool,QString)));
            b.save(req);
            QVERIFY(spy.wait(5000));
            QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("config-write-failed"));
            QVERIFY(w.secrets.isEmpty());                            // rolled back
            QVERIFY(p.calls.isEmpty());
        }
    }
    // ---- the service link -------------------------------------------------------------------------------
    void withoutAServiceTheLinkAnswersNotOkAndNeverBlocks()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        ServiceLink link;
        QSignalSpy startSpy(&link, SIGNAL(startAnswered(bool,QString)));
        QSignalSpy statusSpy(&link, SIGNAL(statusAnswered(bool,QByteArray)));
        QSignalSpy discSpy(&link, SIGNAL(discoverAnswered(QString,QString,QString)));
        link.start();
        link.refreshStatus();
        link.discover("probe");
        link.reportHandled("c1");                                   // no service: a no-op, no error
        QVERIFY(startSpy.wait(30000));
        QVERIFY(!startSpy.at(0).at(0).toBool());                     // the service could not be reached or started
        QTRY_VERIFY_WITH_TIMEOUT(!statusSpy.isEmpty(), 30000);
        QVERIFY(!statusSpy.at(0).at(0).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!discSpy.isEmpty(), 80000);
        QCOMPARE(discSpy.at(0).at(0).toString(), QStringLiteral("probe"));
        const QString key = discSpy.at(0).at(2).toString();
        QVERIFY(key == "not-available" || key == "unreachable");     // a key, never a text from elsewhere
    }
    // ---- the alerts over the bus -------------------------------------------------------
    // The real alerts backend against a stand-in service on a private bus: Alerts() is asked at the start,
    // once after a burst of AlertChanged or SilentChanged signals (coalesced), after each action, and
    // never by itself (nothing polls); the actions carry the service's encoding and their answers come
    // back as the service's words; a service that goes away makes the alerts "not known".
    void theRealAlertsBackendAsksAtStartOnSignalsAndAfterActionsAndNeverPolls()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        const QString name = QStringLiteral("it.michelebrami.libre-monitor-uitest");
        const QString path = QStringLiteral("/it/michelebrami/libre_monitor_uitest");
        QDBusConnection serviceBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("standin"));
        StandInService standIn;
        QVERIFY(serviceBus.registerObject(path, &standIn, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(serviceBus.registerService(name));

        ServiceLink link(name, path, QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("uiclient")));
        RealAlertsBackend alerts(&link);
        QSignalSpy changed(&alerts, SIGNAL(changed()));
        QSignalSpy answered(&alerts, SIGNAL(answered(QString,QString,QString)));
        QVERIFY(!alerts.facts().known);

        alerts.refresh();                                           // the start of the application
        QVERIFY(changed.wait(5000));
        QVERIFY(alerts.facts().known);
        QCOMPARE(alerts.facts().alerts.size(), 1);
        QCOMPARE(alerts.facts().alerts.at(0).level, QStringLiteral("LL"));
        QCOMPARE(countOf(standIn.calls, "Alerts"), 1);

        // A burst of the service's signals: one question after the pause.
        emit standIn.AlertChanged(QStringLiteral("{s1}"), QStringLiteral("low"), QStringLiteral("active"));
        emit standIn.AlertChanged(QStringLiteral("{s1}"), QStringLiteral("low"), QStringLiteral("acknowledged"));
        emit standIn.SilentChanged(QStringLiteral("{s1}"), true, QString());
        QTRY_COMPARE_WITH_TIMEOUT(countOf(standIn.calls, "Alerts"), 2, 5000);
        QTest::qWait(800);
        QCOMPARE(countOf(standIn.calls, "Alerts"), 2);

        // The actions, in the service's encoding, and the service's words back.
        alerts.acknowledge(QStringLiteral("{s1}"), QStringLiteral("low"));
        QVERIFY(answered.wait(5000));
        QCOMPARE(answered.last().at(0).toString(), QStringLiteral("acknowledge"));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QVERIFY(standIn.calls.contains(QStringLiteral("Ack {s1} low")));
        alerts.snooze(QStringLiteral("{s1}"), QStringLiteral("low"), 15);
        QVERIFY(answered.wait(5000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-snoozable"));
        alerts.setSilent(QStringLiteral("{s1}"), -1);
        QVERIFY(answered.wait(5000));
        QCOMPARE(answered.last().at(0).toString(), QStringLiteral("silent"));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QVERIFY(standIn.calls.contains(QStringLiteral("SetSilent {s1} -1")));
        alerts.endSilent(QStringLiteral("{s1}"));
        QVERIFY(answered.wait(5000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-silent"));
        // Round R3: SetThresholds carries the whole mg/dL and the values typed again, and its answer comes
        // back read, with the numbers the page explains a refusal with.
        qRegisterMetaType<lm::ui::ThresholdAnswer>();
        QSignalSpy thr(&alerts, SIGNAL(thresholdsAnswered(QString,lm::ui::ThresholdAnswer)));
        alerts.setThresholds(QStringLiteral("{s1}"), 60, 180, -1, -1);
        QVERIFY(thr.wait(5000));
        QVERIFY(standIn.calls.contains(QStringLiteral("SetThresholds {s1} 60 180 -1 -1")));
        const ui::ThresholdAnswer ta = thr.last().at(1).value<ui::ThresholdAnswer>();
        QCOMPARE(ta.result, QStringLiteral("inconsistent"));
        QCOMPARE(ta.lMax, 59);
        QCOMPARE(ta.offending, QStringLiteral("L"));
        // Each answer is followed by a fresh picture (coalesced), and then nothing more: no polling.
        QTRY_VERIFY_WITH_TIMEOUT(countOf(standIn.calls, "Alerts") >= 3, 5000);
        QTest::qWait(600);
        const int settled = countOf(standIn.calls, "Alerts");
        QTest::qWait(1500);
        QCOMPARE(countOf(standIn.calls, "Alerts"), settled);

        // The service goes away: what it said is no longer known to be true.
        QVERIFY(serviceBus.unregisterService(name));
        QTRY_VERIFY_WITH_TIMEOUT(!alerts.facts().known, 5000);
        QCOMPARE(alerts.facts().error, QStringLiteral("service-not-running"));
        serviceBus.unregisterObject(path);
        QDBusConnection::disconnectFromBus(QStringLiteral("standin"));
    }
    void anAlertsCallWithoutAServiceMakesTheAlertsNotKnown()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        ServiceLink link(QStringLiteral("it.michelebrami.libre-monitor-nobody"), QStringLiteral("/nobody"),
                         QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("uinobody")));
        RealAlertsBackend alerts(&link);
        QSignalSpy changed(&alerts, SIGNAL(changed()));
        QSignalSpy answered(&alerts, SIGNAL(answered(QString,QString,QString)));
        alerts.refresh();
        QVERIFY(changed.wait(30000));
        QVERIFY(!alerts.facts().known);
        QCOMPARE(alerts.facts().error, QStringLiteral("call-failed"));
        alerts.acknowledge(QStringLiteral("{s1}"), QStringLiteral("low"));
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("call-failed"));
    }
    // an OLDER service on the bus, the 0.1-1 kind without the alert
    // methods and without `app_version`. What it proves: the alerts become "not known" with the word
    // "service-older" (not "call-failed"), the actions and the thresholds come back with that word too, the
    // strip says "service-older" both by the version and by the "unknown method", the window does not stop
    // or start the service by itself, the Stop button reaches the service, and when the old name goes away
    // the strip shows "stopped" (the "unknown method" is not carried over to the next process). A service
    // that sends this window's own version is "running".
    void anOlderServiceOnTheBusIsShownAsOlderAndNeverRestartedByTheWindow()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        const QString name = QStringLiteral("it.michelebrami.libre-monitor-uiolder");
        const QString path = QStringLiteral("/it/michelebrami/libre_monitor_uiolder");
        QDBusConnection serviceBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("olderstandin"));
        OlderStandInService older;
        older.statusJson = QString::fromLatin1(kStatus);                        // no app_version
        QVERIFY(serviceBus.registerObject(path, &older, QDBusConnection::ExportAllSlots));
        QVERIFY(serviceBus.registerService(name));

        ServiceLink link(name, path, QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("uiolderclient")));
        QSignalSpy failed(&link, SIGNAL(callFailed(QString,QString)));
        RealAlertsBackend alerts(&link);
        QSignalSpy answered(&alerts, SIGNAL(answered(QString,QString,QString)));
        alerts.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(alerts.facts().error, QStringLiteral("service-older"), 5000);
        QVERIFY(!alerts.facts().known);
        QVERIFY(!failed.isEmpty());
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Alerts"));
        QVERIFY(ServiceClient::isOlderServiceError(failed.at(0).at(1).toString()));
        alerts.acknowledge(QStringLiteral("{s1}"), QStringLiteral("low"));
        QVERIFY(answered.wait(5000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("service-older"));
        qRegisterMetaType<lm::ui::ThresholdAnswer>();
        QSignalSpy thr(&alerts, SIGNAL(thresholdsAnswered(QString,lm::ui::ThresholdAnswer)));
        alerts.setThresholds(QStringLiteral("{s1}"), 60, 180, -1, -1);
        QVERIFY(thr.wait(5000));
        QCOMPARE(thr.last().at(1).value<ui::ThresholdAnswer>().result, QStringLiteral("service-older"));

        // The strip, by the version: this window is 0.1-3, the service sends none.
        QTemporaryDir dir;
        RealBackend backend(&link, dir.path() + "/config.json", dir.path() + "/store.db", dir.path() + "/journal.jsonl");
        ui::MonitoringInfo monitoring;
        monitoring.setBackend(&backend);
        backend.setOwnVersion(QStringLiteral("0.1-3"));
        link.refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("service-older"), 5000);
        // The same build: running.
        QByteArray same = kStatus;
        same.replace("\"start_cause\"", "\"app_version\":\"0.1-3\",\"start_cause\"");
        older.statusJson = QString::fromLatin1(same);
        link.refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("running"), 5000);
        // ... but a call answered "unknown method" still says older (a service that sends the version but
        // lacks a method of this window is another build too).
        alerts.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("service-older"), 5000);
        // X1: a service NEWER than this window: "service-newer", also after an "unknown method" (a newer
        // service may have dropped a method); the window is the old one, so nothing is stopped.
        QByteArray newer = kStatus;
        newer.replace("\"start_cause\"", "\"app_version\":\"0.1-4\",\"start_cause\"");
        older.statusJson = QString::fromLatin1(newer);
        link.refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("service-newer"), 5000);
        alerts.refresh();
        QTest::qWait(300);
        QCOMPARE(monitoring.status(), QStringLiteral("service-newer"));
        older.statusJson = QString::fromLatin1(same);
        link.refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("service-older"), 5000);   // the call's reason again
        // The window never stopped or started it by itself.
        QVERIFY(!older.calls.contains(QStringLiteral("Stop")));
        QVERIFY(!older.calls.contains(QStringLiteral("Start")));
        // The user's Stop reaches the service; the old name goes away: "stopped", and the "unknown method"
        // of the old process is forgotten.
        monitoring.stopMonitoring();
        QTRY_VERIFY_WITH_TIMEOUT(older.calls.contains(QStringLiteral("Stop")), 5000);
        QVERIFY(serviceBus.unregisterService(name));
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("stopped"), 5000);
        // A new process with this window's version: running again (the old reason did not stick).
        QVERIFY(serviceBus.registerService(name));
        QTRY_COMPARE_WITH_TIMEOUT(monitoring.status(), QStringLiteral("running"), 5000);
        QVERIFY(!older.calls.contains(QStringLiteral("Start")));
        serviceBus.unregisterService(name);
        serviceBus.unregisterObject(path);
        QDBusConnection::disconnectFromBus(QStringLiteral("olderstandin"));
    }
    // The integration run with the HOST SERVICE and the SIMULATOR: tests/integration/run_ui_alerts_integration.py starts the host service on a bus
    // of its own, lets a synthetic patient go very low, and runs this function with the service's name.
    // Without those variables it is skipped. What it proves: the real alerts backend sees the alert
    // through Alerts() and AlertChanged only (nothing polls), the model offers Acknowledge and no Snooze
    // for a very low alert, the acknowledgement from the application is done by the service and becomes
    // a pause, silent mode set and ended from the application is reported back with the service's end
    // time (until tomorrow: 06:00 local, computed by the service), and refusals come back in words.
    void liveHostServiceWithTheSimulator()
    {
        const QString name = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_NAME"));
        const QString path = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_PATH"));
        const QString subject = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_SUBJECT"));
        if (name.isEmpty() || path.isEmpty() || subject.isEmpty())
            QSKIP("run by tests/integration/run_ui_alerts_integration.py");
        const int waitS = qMax(60, qgetenv("LM_UI_LIVE_WAIT_S").toInt());
        ServiceLink link(name, path, QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("live")));
        RealAlertsBackend alerts(&link);
        QSignalSpy answered(&alerts, SIGNAL(answered(QString,QString,QString)));
        int changes = 0;
        connect(&alerts, &ui::AlertsBackend::changed, [&changes]() { ++changes; });
        alerts.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().known, 30000);
        qInfo("LIVE known: alerts %d, patients %d", alerts.facts().alerts.size(), alerts.facts().patients.size());
        QVERIFY(alerts.facts().patient(subject));

        // The very low alert arrives by the service's signal (the backend never asks by itself).
        auto lowLevel = [&]() {
            const ui::AlertsFacts f = alerts.facts();
            for (int i = 0; i < f.alerts.size(); ++i)
                if (f.alerts.at(i).subject == subject && f.alerts.at(i).slot == QLatin1String("low"))
                    return f.alerts.at(i).level + QLatin1Char('/') + f.alerts.at(i).state;
            return QString();
        };
        QTRY_VERIFY_WITH_TIMEOUT(lowLevel() == QLatin1String("LL/active"), waitS * 1000);
        qInfo("LIVE very low active after %d changes", changes);
        ui::SubjectSnapshot person;
        person.id = subject;
        const QList<ui::AlertRow> rows = ui::buildAlertRows(alerts.facts(), QList<ui::SubjectSnapshot>() << person,
                                                            QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!rows.isEmpty());
        QCOMPARE(rows.at(0).levelKey, QStringLiteral("ll"));
        QCOMPARE(rows.at(0).shortLabel, QStringLiteral("P1"));
        QVERIFY(rows.at(0).canAcknowledge);
        QVERIFY(!rows.at(0).canSnooze);
        QCOMPARE(rows.at(0).bandKey, QStringLiteral("ll"));

        // Snooze is refused by the service for very low, in words.
        alerts.snooze(subject, QStringLiteral("low"), 15);
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-snoozable"));
        // The acknowledgement from the application: done, and the slot becomes acknowledged (a pause).
        alerts.acknowledge(subject, QStringLiteral("low"));
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QTRY_COMPARE_WITH_TIMEOUT(lowLevel(), QStringLiteral("LL/acknowledged"), 15000);
        const QList<ui::AlertRow> after = ui::buildAlertRows(alerts.facts(), QList<ui::SubjectSnapshot>() << person,
                                                             QDateTime::currentMSecsSinceEpoch());
        QVERIFY(after.at(0).isPause && !after.at(0).canAcknowledge);
        qInfo("LIVE acknowledged from the application");

        // Silent mode: one hour, reported back with its end; until tomorrow: the service's 06:00 local.
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        alerts.setSilent(subject, 60);
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().patient(subject) && alerts.facts().patient(subject)->silent, 15000);
        const qint64 hourEnd = alerts.facts().patient(subject)->silentUntilMs;
        QVERIFY2(qAbs(hourEnd - (before + 3600000LL)) < 120000LL, "the end of one hour of silent mode");
        alerts.setSilent(subject, -1);
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().patient(subject)->silentUntilMs != hourEnd, 15000);
        const QDateTime tomorrow = QDateTime::fromMSecsSinceEpoch(alerts.facts().patient(subject)->silentUntilMs);
        QCOMPARE(tomorrow.time(), QTime(6, 0));
        QVERIFY(tomorrow.toMSecsSinceEpoch() - QDateTime::currentMSecsSinceEpoch() >= 3500000LL);
        alerts.setSilent(subject, 0);                                   // without limit (the page confirmed)
        QVERIFY(answered.wait(30000));
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().patient(subject)->silentUntilMs == 0, 15000);
        QVERIFY(alerts.facts().patient(subject)->silent);
        alerts.endSilent(subject);
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QTRY_VERIFY_WITH_TIMEOUT(!alerts.facts().patient(subject)->silent, 15000);
        alerts.endSilent(subject);
        QVERIFY(answered.wait(30000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-silent"));
        alerts.setSilent(subject, 7);                                   // any minutes > 0 are the service's to judge
        QVERIFY(answered.wait(30000));
        qInfo("LIVE silent mode set and ended from the application; SetSilent 7 answered %s",
              qPrintable(answered.last().at(2).toString()));
        alerts.endSilent(subject);
        QVERIFY(answered.wait(30000));
        qInfo("LIVE changes seen: %d", changes);
    }
    // Round R3, the integration run with the HOST SERVICE (run_ui_alerts_integration.py --functions
    // liveThresholdsAndSettingsWithTheHostService): the thresholds page's controller and the settings object,
    // exactly as the pages use them, against lm-service on a private bus. What it proves: a looser limit is
    // answered needs-confirmation and the page asks the challenge; a wrong value typed again is refused and the
    // page names the limit; an impossible order is refused with the allowed range of that limit; the right value is accepted,
    // the limits in force follow in Alerts() and the history is read from the service's own journal
    // read-only; a stricter limit needs no challenge; the same values again are "no change". The alert times
    // written to the configuration file are applied by ConfigChanged without a restart and come back in
    // Alerts(); a value outside the range is clamped by the service (the range is the engine's); the
    // language, the value in alerts and a label are applied without a restart. Nothing is printed but words.
    void liveThresholdsAndSettingsWithTheHostService()
    {
        const QString name = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_NAME"));
        const QString path = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_PATH"));
        const QString subject = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_SUBJECT"));
        const QString data = QString::fromLocal8Bit(qgetenv("LM_UI_LIVE_DATA"));
        if (name.isEmpty() || path.isEmpty() || subject.isEmpty() || data.isEmpty())
            QSKIP("run by tests/integration/run_ui_alerts_integration.py");
        ServiceLink link(name, path, QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("livethr")));
        RealAlertsBackend alerts(&link);
        alerts.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().known && alerts.facts().patient(subject), 30000);
        const ui::AlertsFacts f0 = alerts.facts();
        QVERIFY(f0.missingLimit.minS > 0 && f0.missingLimit.maxS > f0.missingLimit.minS);
        QVERIFY(f0.persistenceHigh.maxS > 0 && f0.persistenceLow.maxS > 0);

        // The journal, read the way the page reads it (the store's thread is left out here).
        class JournalData : public ui::Backend
        {
        public:
            QString journal;
            QList<ui::SubjectSnapshot> subjects() const { return QList<ui::SubjectSnapshot>(); }
            ui::ServiceStatus serviceStatus() const { return ui::ServiceStatus::Running; }
            void retry(const QString &) {}
            void reenable(const QString &) {}
            void startMonitoring() {}
            void askAgain() {}
            void requestThresholdHistory(const QString &s)
            {
                StoreQueries q;
                QList<ui::ThresholdHistoryEntry> out;
                const bool ok = q.thresholdHistory(journal, QUuid(s), &out);
                emit thresholdHistoryLoaded(s, ok, out);
            }
        } journal;
        journal.journal = data + QStringLiteral("/journal.db");
        ui::ThresholdEditor e;
        e.setBackends(&journal, &alerts);
        e.setDisplayUnit(QStringLiteral("mg/dL"));
        e.setLocale(QLocale::c());
        e.open(subject);
        QCOMPARE(e.historyState(), QStringLiteral("ready"));
        const int historyBefore = e.history().size();
        auto firstKey = [&e]() {
            const QVariantList parts = e.message().value(QStringLiteral("parts")).toList();
            return parts.isEmpty() ? QString() : parts.first().toMap().value(QStringLiteral("key")).toString();
        };
        auto allKeys = [&e]() {
            QStringList k;
            const QVariantList parts = e.message().value(QStringLiteral("parts")).toList();
            for (int i = 0; i < parts.size(); ++i)
                k << parts.at(i).toMap().value(QStringLiteral("key")).toString();
            return k.join(QLatin1Char(' '));
        };
        const QString lower = QString::number(lm::kGlobalL - 8), wrong = QString::number(lm::kGlobalL - 9);
        const QString std_ = QString::number(lm::kGlobalL), stdH = QString::number(lm::kGlobalH);

        e.save(lower, stdH);
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("challenge"), 15000);
        QVERIFY(e.challengeLow() && !e.challengeHigh());
        e.confirmChallenge(wrong, QString());
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("idle"), 15000);
        QCOMPARE(firstKey(), QStringLiteral("ui-thr-err-mismatch-low"));
        qInfo("LIVE challenge asked, wrong value refused: %s", qPrintable(firstKey()));

        e.save(QString::number(lm::kGlobalLL - 4), stdH);
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("idle"), 15000);
        QCOMPARE(allKeys(), QStringLiteral("ui-thr-err-order-low ui-thr-range-low"));
        qInfo("LIVE impossible order refused with the ranges: %s", qPrintable(allKeys()));

        e.save(lower, stdH);
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("challenge"), 15000);
        e.confirmChallenge(lower, QString());
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("idle"), 15000);
        QCOMPARE(firstKey(), QStringLiteral("ui-thr-saved"));
        QTRY_COMPARE_WITH_TIMEOUT(e.lowText(), lower, 15000);           // in force, from Alerts()
        QCOMPARE(e.historyState(), QStringLiteral("ready"));
        QCOMPARE(e.history().size(), historyBefore + 1);
        QCOMPARE(e.history().first().toMap().value(QStringLiteral("boundary")).toString(), QStringLiteral("low"));
        qInfo("LIVE looser limit accepted after the challenge; history entries now %d", e.history().size());

        e.save(std_, stdH);                                    // stricter again: no challenge
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("idle"), 15000);
        QCOMPARE(firstKey(), QStringLiteral("ui-thr-saved"));
        QTRY_COMPARE_WITH_TIMEOUT(e.history().size(), historyBefore + 2, 15000);
        e.save(std_, stdH);
        QTRY_COMPARE_WITH_TIMEOUT(e.state(), QStringLiteral("idle"), 15000);
        QCOMPARE(firstKey(), QStringLiteral("ui-thr-no-change"));
        qInfo("LIVE stricter limit without a challenge; the same values again: %s", qPrintable(firstKey()));

        // The settings of the configuration file, applied by ConfigChanged without a restart.
        ui::ConfigSettings cs;
        cs.load(data + QStringLiteral("/config.json"));
        QVERIFY(cs.available());
        QStringList answers;
        connect(&cs, &ui::ConfigSettings::written, &link, &ServiceLink::configChanged);
        connect(&link, &ServiceLink::configChangeAnswered, [&answers, &cs](bool ok, const QString &r) {
            answers << (ok ? (r.isEmpty() ? QStringLiteral("applied") : r) : QStringLiteral("no-answer"));
            cs.configApplied(ok, r);
        });
        const int missing = f0.missingLimit.minS + 300 <= f0.missingLimit.maxS ? f0.missingLimit.minS + 300 : f0.missingLimit.maxS;
        QVERIFY(cs.setAlertTime(subject, QStringLiteral("missing"), missing));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 1, 15000);
        QVERIFY(cs.setAlertTime(subject, QStringLiteral("high"), f0.persistenceHigh.minS));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 15000);
        alerts.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(alerts.facts().patient(subject) && alerts.facts().patient(subject)->missingLimitS == missing
                                 && alerts.facts().patient(subject)->persistenceHighS == f0.persistenceHigh.minS, 15000);
        // Below the range (written by hand, not offered by the page): the engine clamps it to its minimum.
        QVERIFY(cs.setAlertTime(subject, QStringLiteral("missing"), f0.missingLimit.minS / 5));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 3, 15000);
        alerts.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(alerts.facts().patient(subject)->missingLimitS, f0.missingLimit.minS, 15000);
        qInfo("LIVE alert times applied without a restart, a value below the range clamped by the service");
        QVERIFY(cs.setLanguage(QStringLiteral("it")));
        QVERIFY(cs.setAlertsShowValue(true));
        QVERIFY(cs.setPatientLabel(subject, QStringLiteral("AB")));
        QVERIFY(!cs.setPatientLabel(subject, QStringLiteral("ABCD")));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 6, 15000);
        QCOMPARE(answers, QStringList() << "applied" << "applied" << "applied" << "applied" << "applied" << "applied");
        QCOMPARE(cs.errorKey(), QStringLiteral("label-too-long"));
        qInfo("LIVE ConfigChanged answers: %s", qPrintable(answers.join(QLatin1Char(' '))));
    }
    // Round R3: the history of the threshold edits is read from the journal's `threshold` lines,
    // read-only, as the interface reads the store: one patient's edits, oldest first, the caregiver's comment not
    // carried; a journal that is not there is "cannot be read", not "no edits"; the file is not changed.
    void theThresholdHistoryIsReadFromTheJournalReadOnly()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        const QUuid mine("{dddddddd-0000-4000-8000-000000000001}"), other("{dddddddd-0000-4000-8000-000000000002}");
        {
            Journal j;
            QVERIFY(j.open(path));
            QVector<ThresholdChange> changes;
            ThresholdChange a; a.id = QUuid::createUuid(); a.subject = mine; a.boundary = Boundary::H;
            a.value = 200; a.previous = 180; a.time = UtcTime::fromIso("2026-10-18T08:00:00Z");
            ThresholdChange b = a; b.id = QUuid::createUuid(); b.boundary = Boundary::L; b.value = 65; b.previous = 70;
            b.time = UtcTime::fromIso("2026-10-17T08:00:00Z");
            ThresholdChange c = a; c.id = QUuid::createUuid(); c.subject = other; c.value = 220;
            changes << a << b << c;
            QVERIFY(writeThresholdChanges(&j, changes));
            j.close();
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray before = f.readAll();
        f.close();
        StoreQueries q;
        QList<ui::ThresholdHistoryEntry> out;
        QVERIFY(q.thresholdHistory(path, mine, &out));
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).boundary, QStringLiteral("L"));               // oldest first
        QCOMPARE(out.at(0).value, 65);
        QCOMPARE(out.at(0).previous, 70);
        QCOMPARE(out.at(1).boundary, QStringLiteral("H"));
        QCOMPARE(out.at(1).timeMs, ms("2026-10-18T08:00:00Z"));
        q.close();
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), before);
        StoreQueries none;
        QVERIFY(!none.thresholdHistory(dir.path() + "/missing.db", mine, &out));
        QVERIFY(out.isEmpty());
    }
    void unitsAreConvertedExactly()
    {
        double mg = 0;
        QVERIFY(toMgDl(5.0, "mmol/L", &mg));
        QVERIFY(qAbs(mg - 5.0 * 18.0156) < 1e-12);
        QVERIFY(toMgDl(90, "mg/dL", &mg));
        QCOMPARE(mg, 90.0);
        QVERIFY(!toMgDl(5, "stones", &mg));
    }
};

QTEST_GUILESS_MAIN(TstUiReal)
#include "tst_uireal.moc"
