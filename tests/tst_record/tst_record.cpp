// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "record/record.h"
#include "record/registry.h"

#include <QJsonArray>
#include <QSet>
#include <QtTest>

using namespace lm;

class TstRecord : public QObject
{
    Q_OBJECT

    static UtcTime now() { return UtcTime::fromMSecsSinceEpoch(1700000000000LL); }

    static Record goodSample()
    {
        Record r;
        r.shape = Record::Sample;
        r.subject = QUuid("11111111-2222-3333-4444-555555555555");
        r.metric = metricGlucose();
        r.value = 110.0;
        r.unit = "mg/dL";
        r.measuredAt = UtcTime::fromMSecsSinceEpoch(now().msecsSinceEpoch() - 60000);
        r.ingestedAt = now();
        r.originalTimestamp = "synthetic timestamp string";
        r.source = "test:feed-a";
        r.origin = Origin::Adapter;
        return r;
    }
    static Validation::Reason reasonOf(const Record &r)
    {
        return validateRecord(r, now()).reason;
    }

private slots:
    void goodRecordIsValid()
    {
        QVERIFY(validateRecord(goodSample(), now()).ok());
    }
    void eachMandatoryField()
    {
        Record r = goodSample(); r.subject = QUuid();
        QCOMPARE(reasonOf(r), Validation::MissingSubject);
        r = goodSample(); r.metric = "";
        QCOMPARE(reasonOf(r), Validation::UnknownMetric);
        r = goodSample(); r.unit = "";
        QCOMPARE(reasonOf(r), Validation::MissingUnit);
        r = goodSample(); r.source = "";
        QCOMPARE(reasonOf(r), Validation::MissingSource);
        r = goodSample(); r.measuredAt = UtcTime();
        QCOMPARE(reasonOf(r), Validation::MissingMeasurementTime);
        r = goodSample(); r.ingestedAt = UtcTime();
        QCOMPARE(reasonOf(r), Validation::MissingIngestTime);
        r = goodSample(); r.originalTimestamp = "";
        QCOMPARE(reasonOf(r), Validation::MissingOriginalTimestamp);
    }
    void originSpecificFields()
    {
        Record r = goodSample(); r.origin = Origin::Manual;
        QCOMPARE(reasonOf(r), Validation::MissingEntryId);
        r.entryId = "e1";
        QVERIFY(validateRecord(r, now()).ok());   // manual: no source string needed
        r.originalTimestamp = "";
        QVERIFY(validateRecord(r, now()).ok());
        r = goodSample(); r.origin = Origin::Derived;
        QCOMPARE(reasonOf(r), Validation::MissingRuleVersion);
        r.ruleVersion = "r1";
        r.originalTimestamp = "";
        QVERIFY(validateRecord(r, now()).ok());
    }
    void unitNotAcceptedAndNeverCoerced()
    {
        Record r = goodSample(); r.unit = "mg/dl";   // case matters
        QCOMPARE(reasonOf(r), Validation::UnitNotAccepted);
        r.unit = "mmol";
        QCOMPARE(reasonOf(r), Validation::UnitNotAccepted);
        r.unit = "mmol/L"; r.value = 6.1;
        QVERIFY(validateRecord(r, now()).ok());
        QCOMPARE(r.value, 6.1);   // validation leaves the record alone
    }
    void valueRange()
    {
        const MetricDef *g = MetricRegistry::instance().find(metricGlucose());
        QVERIFY(g);
        const UnitRange *mg = g->findUnit("mg/dL");
        QVERIFY(mg);
        Record r = goodSample();
        r.value = mg->min;
        QVERIFY(validateRecord(r, now()).ok());
        r.value = mg->max;
        QVERIFY(validateRecord(r, now()).ok());
        r.value = mg->min - 0.1;
        QCOMPARE(reasonOf(r), Validation::ValueOutOfRange);
        r.value = mg->max + 0.1;
        QCOMPARE(reasonOf(r), Validation::ValueOutOfRange);
        r.value = 6.1;   // a mmol/L number labelled mg/dL is out of range, not converted
        QCOMPARE(reasonOf(r), Validation::ValueOutOfRange);
        r.value = qQNaN();
        QCOMPARE(reasonOf(r), Validation::ValueNotFinite);
        r.value = qInf();
        QCOMPARE(reasonOf(r), Validation::ValueNotFinite);
    }
    void timeInTheFuture()
    {
        Record r = goodSample();
        r.measuredAt = UtcTime::fromMSecsSinceEpoch(now().msecsSinceEpoch() + 1000);
        QVERIFY(!validateRecord(r, now(), 0).ok());
        QCOMPARE(validateRecord(r, now(), 0).reason, Validation::MeasurementTimeInFuture);
        QVERIFY(validateRecord(r, now(), 1000).ok());
        r.measuredAt = UtcTime::fromMSecsSinceEpoch(now().msecsSinceEpoch() + kDefaultClockSkewToleranceMs + 1);
        QCOMPARE(reasonOf(r), Validation::MeasurementTimeInFuture);
    }
    void shapeAndEvents()
    {
        Record r = goodSample(); r.shape = Record::Event;
        QCOMPARE(reasonOf(r), Validation::ShapeMismatch);
        r = goodSample(); r.metric = "insulin";   // designed, not implemented in V1
        QCOMPARE(reasonOf(r), Validation::UnknownMetric);
    }
    void registryHoldsGlucoseAndLogbook()
    {
        QCOMPARE(MetricRegistry::instance().names(),
                 QStringList() << metricGlucose() << metricLogbookEntry());
        const MetricDef *g = MetricRegistry::instance().find(metricGlucose());
        QVERIFY(g->maxSamplesPerMinute > 0);
        QVERIFY(g->findUnit("mmol/L"));
        QVERIFY(!g->findUnit("g/L"));
    }
    // --- logbook_entry (REQ-SRC-08) ---
    static Record goodLogbook()
    {
        Record r = goodSample();
        r.shape = Record::Event;
        r.metric = metricLogbookEntry();
        r.unit = QString();     // the unit of an entry lives in the payload
        r.value = 0.0;
        r.source = "test:logbook";
        QJsonObject raw; raw["Value"] = 117; raw["type"] = 3;
        r.payload = QJsonObject{ { "entry_type", 3 }, { "raw", raw } };
        return r;
    }
    void logbookMinimalAndFullAreValid()
    {
        QVERIFY(validateRecord(goodLogbook(), now()).ok());
        Record r = goodLogbook();
        r.payload["value"] = 6.1;
        r.payload["unit"] = "mmol/L";
        r.payload["trend_code"] = 3;
        r.payload["alarm_code"] = 0;
        r.payload["color_code"] = 1;
        QVERIFY(validateRecord(r, now()).ok());
        r.payload["value"] = QJsonValue(QJsonValue::Null);      // optional fields may be null
        r.payload["unit"] = QJsonValue(QJsonValue::Null);
        r.payload["trend_code"] = QJsonValue(QJsonValue::Null);
        r.payload["alarm_code"] = QJsonValue(QJsonValue::Null);
        r.payload["color_code"] = QJsonValue(QJsonValue::Null);
        QVERIFY(validateRecord(r, now()).ok());
    }
    void logbookRejectsUnknownField()
    {
        Record r = goodLogbook(); r.payload["note"] = "x";
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        QCOMPARE(validateRecord(r, now()).detail, QString("payload.note"));
    }
    void logbookRejectsWrongTypes()
    {
        const char *integers[] = { "entry_type", "trend_code", "alarm_code", "color_code" };
        for (const char *name : integers) {
            Record r = goodLogbook();
            r.payload[name] = "3";
            QCOMPARE(reasonOf(r), Validation::InvalidPayload);
            r.payload[name] = 3.5;
            QCOMPARE(reasonOf(r), Validation::InvalidPayload);
            r.payload[name] = true;
            QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        }
        Record r = goodLogbook(); r.payload["value"] = "117";
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        r = goodLogbook(); r.payload["unit"] = 1;
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        r = goodLogbook(); r.payload["raw"] = "text";
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        r = goodLogbook(); r.payload["raw"] = QJsonArray();
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
    }
    void logbookRequiresEntryTypeAndRaw()
    {
        Record r = goodLogbook(); r.payload.remove("entry_type");
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        QCOMPARE(validateRecord(r, now()).detail, QString("payload.entry_type"));
        r = goodLogbook(); r.payload.remove("raw");
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        QCOMPARE(validateRecord(r, now()).detail, QString("payload.raw"));
        r = goodLogbook(); r.payload["entry_type"] = QJsonValue(QJsonValue::Null);
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
        r = goodLogbook(); r.payload["raw"] = QJsonValue(QJsonValue::Null);
        QCOMPARE(reasonOf(r), Validation::InvalidPayload);
    }
    void logbookRejectsUnknownUnit()
    {
        Record r = goodLogbook(); r.payload["unit"] = "mg/dl";   // case matters
        QCOMPARE(reasonOf(r), Validation::UnitNotAccepted);
        r.payload["unit"] = "";
        QCOMPARE(reasonOf(r), Validation::UnitNotAccepted);
        r.payload["unit"] = "g/L";
        QCOMPARE(reasonOf(r), Validation::UnitNotAccepted);
    }
    void logbookValueIsNotRangeChecked()
    {
        const double values[] = { 0.0, -5.0, 19.9, 600.1, 100000.0 };
        for (double v : values) {
            Record r = goodLogbook();
            r.payload["value"] = v;
            r.payload["unit"] = "mg/dL";
            QVERIFY2(validateRecord(r, now()).ok(), "value must not be range checked");
        }
    }
    void logbookShapeAndTimeStillChecked()
    {
        Record r = goodLogbook(); r.shape = Record::Sample;
        QCOMPARE(reasonOf(r), Validation::ShapeMismatch);
        r = goodLogbook(); r.measuredAt = UtcTime();
        QCOMPARE(reasonOf(r), Validation::MissingMeasurementTime);
        r = goodLogbook(); r.durationMs = -2;
        QCOMPARE(reasonOf(r), Validation::InvalidDuration);
    }

    // --- resolution and thinning data (REQ-DAT-12, REQ-STO-07, REQ-STO-12) ---
    void nativeResolution()
    {
        const MetricRegistry &reg = MetricRegistry::instance();
        QCOMPARE(reg.unitDecimals("mg/dL"), 0);
        QCOMPARE(reg.unitDecimals("mmol/L"), 1);
        QCOMPARE(reg.unitDecimals("mg/dl"), -1);
        QCOMPARE(reg.unitDecimals(""), -1);
        QVERIFY(reg.isKnownUnit("mmol/L"));
        QVERIFY(!reg.isKnownUnit("g/L"));
    }
    void thinningRules()
    {
        const MetricRegistry &reg = MetricRegistry::instance();
        QVERIFY(reg.thinningFor(metricGlucose()).applies);
        QCOMPARE(reg.thinningFor(metricGlucose()).slotMinutes, 5);
        QVERIFY(reg.thinningFor(metricGlucose()).thinsFeed("current"));
        QVERIFY(!reg.thinningFor(metricGlucose()).thinsFeed("graph"));            // the history feed is kept
        QVERIFY(!reg.thinningFor(metricGlucose()).thinsFeed("logbook"));
        QVERIFY(!reg.thinningFor(metricLogbookEntry()).applies);
        QVERIFY(!reg.thinningFor("heart_rate").applies);    // unknown kinds are never thinned
        QVERIFY(!reg.thinningFor("").applies);
    }
    void glucoseLimitsUnchanged()
    {
        const MetricDef *g = MetricRegistry::instance().find(metricGlucose());
        QCOMPARE(g->findUnit("mg/dL")->min, 20.0);
        QCOMPARE(g->findUnit("mg/dL")->max, 600.0);
        QCOMPARE(g->findUnit("mmol/L")->min, 1.1);
        QCOMPARE(g->findUnit("mmol/L")->max, 33.4);
        QCOMPARE(g->maxSamplesPerMinute, 2.0);
    }
    void keyIdentity()
    {
        const Record a = goodSample();
        Record b = goodSample();
        b.value = 999;                          // value and ingest time are not part of the key
        b.ingestedAt = UtcTime::fromMSecsSinceEpoch(1);
        QCOMPARE(a.key(), b.key());
        QCOMPARE(a.key().toString(), b.key().toString());
    }
    void keyDiffersByEachPart()
    {
        const Record a = goodSample();
        Record b = goodSample(); b.subject = QUuid("99999999-2222-3333-4444-555555555555");
        QVERIFY(a.key() != b.key());
        b = goodSample(); b.metric = "other";
        QVERIFY(a.key() != b.key());
        b = goodSample(); b.measuredAt = UtcTime::fromMSecsSinceEpoch(a.measuredAt.msecsSinceEpoch() + 1);
        QVERIFY(a.key() != b.key());
        b = goodSample(); b.source = "test:feed-b";     // the feed is part of the source tag
        QVERIFY(a.key() != b.key());
    }
    void manualKeyIncludesEntryId()
    {
        Record a = goodSample(); a.origin = Origin::Manual; a.entryId = "e1";
        Record b = a; b.entryId = "e2";
        QVERIFY(a.key() != b.key());
        Record c = goodSample(); c.entryId = "ignored for adapter records";
        QCOMPARE(c.key(), goodSample().key());
    }
    void keyOrderingForContainers()
    {
        QSet<QString> seen;
        Record a = goodSample();
        Record b = goodSample(); b.measuredAt = UtcTime::fromMSecsSinceEpoch(5);
        QVERIFY(a.key() < b.key() || b.key() < a.key());
        QVERIFY(!(a.key() < a.key()));
        seen.insert(a.key().toString());
        seen.insert(goodSample().key().toString());   // re-fetched overlap is harmless
        QCOMPARE(seen.size(), 1);
    }
    void originNames()
    {
        QCOMPARE(originName(Origin::Adapter), QString("adapter"));
        QCOMPARE(originName(Origin::Manual), QString("manual"));
        QCOMPARE(originName(Origin::Derived), QString("derived"));
    }
};

QTEST_APPLESS_MAIN(TstRecord)
#include "tst_record.moc"
