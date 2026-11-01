// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDULERSETTINGS_H
#define LM_SCHEDULERSETTINGS_H

// Every tunable number of the scheduler and the session manager in one place
//.
// All values are PROVISIONAL starting points taken from measured source behaviour where one
// exists (one reading a minute, graph spacing 5 minutes) and otherwise from the
// documents; none is measured on the phone. Each is recorded
// and may be changed per build or per connection without touching the logic. Fields are in
// milliseconds; the comment names the constant of the document.

#include <QList>
#include <QString>
#include <QUuid>
#include <QtGlobal>

namespace lm {

struct SchedulerSettings
{
    SchedulerSettings()
        : currentIntervalMs(60 * 1000), graphIntervalMs(300 * 1000),
          logbookIntervalMs(6LL * 3600 * 1000),
          publishLagMs(10 * 1000), recheckDelayMs(15 * 1000), recheckMax(4),
          quietAfterMs(1800 * 1000), quietMaxIntervalMs(300 * 1000), minFetchGapMs(10 * 1000),
          startSettleMs(5 * 1000), startSpreadMs(25 * 1000),
          backfillWindowMs(12LL * 3600 * 1000), logbookWindowMs(13LL * 24 * 3600 * 1000),
          staleFactor(3),
          backoffBaseMs(30 * 1000), backoffFactor(2), backoffCapMs(900 * 1000), backoffJitter(0.25),
          rejectPollsMax(5), rateHoldInitialMs(1800 * 1000), rateHoldCapMs(6LL * 3600 * 1000),
          rateFloorCapMs(600 * 1000), rateCalmMs(24LL * 3600 * 1000), subjectFailMax(8),
          invalidCommandMax(3), readingQueueMax(20000),
          minAuthGapMs(60 * 1000), authRetryGapMs(300 * 1000), authRetryCapMs(3600 * 1000),
          authToFetchGapMs(2 * 1000), authTransientMax(6),
          authSuspectedRetryMs(6LL * 3600 * 1000), authSuspectedRetryMax(0),
          renewBeforeMs(7LL * 24 * 3600 * 1000), renewRetryMs(6LL * 3600 * 1000),
          linkIdleCloseMs(150 * 1000), linkMaxAgeMs(24LL * 3600 * 1000),
          secretRetryMs(300 * 1000), secretRetryCapMs(3600 * 1000), reloginMax(1) {}

    // Nominal intervals (REQ-SCH-12): kCurrentIntervalS, kGraphIntervalS, kLogbookIntervalS.
    // A connection may override them (ConnectionConfig). 0 for the logbook switches it off.
    qint64 currentIntervalMs;
    qint64 graphIntervalMs;
    qint64 logbookIntervalMs;

    qint64 publishLagMs;            // kPublishLagS
    qint64 recheckDelayMs;          // kRecheckDelayS
    int recheckMax;                 // kRecheckMax
    qint64 quietAfterMs;            // kQuietAfterS
    qint64 quietMaxIntervalMs;      // kQuietMaxIntervalS
    qint64 minFetchGapMs;           // kMinFetchGapS (REQ-SCH-05)
    qint64 startSettleMs;           // kStartSettleS
    qint64 startSpreadMs;           // kStartSpreadS
    qint64 backfillWindowMs;        // kBackfillWindowS
    qint64 logbookWindowMs;         // kLogbookWindowS
    int staleFactor;                // kStaleFactor

    qint64 backoffBaseMs;           // kBackoffBaseS
    int backoffFactor;              // kBackoffFactor
    qint64 backoffCapMs;            // kBackoffCapS
    double backoffJitter;           // kBackoffJitter
    int rejectPollsMax;             // kRejectPollsMax
    qint64 rateHoldInitialMs;       // kRateHoldInitialS
    qint64 rateHoldCapMs;           // kRateHoldCapS (also the cap on a retry_after hint)
    qint64 rateFloorCapMs;          // kRateFloorCapS
    qint64 rateCalmMs;              // kRateCalmS
    int subjectFailMax;             // kSubjectFailMax
    int invalidCommandMax;          // kInvalidCommandMax
    int readingQueueMax;            // records held while the store is busy (REQ-STO-15)

    // Session manager.
    qint64 minAuthGapMs;            // kMinAuthGapS
    qint64 authRetryGapMs;          // kAuthRetryGapS
    qint64 authRetryCapMs;          // cap of the doubling gap, 3600 s
    qint64 authToFetchGapMs;        // kAuthToFetchGapS
    int authTransientMax;           // kAuthTransientMax
    qint64 authSuspectedRetryMs;    // kAuthSuspectedRetryS
    int authSuspectedRetryMax;      // number of slow retries, 0 = no limit ( "a parameter")
    qint64 renewBeforeMs;           // kRenewBeforeS
    qint64 renewRetryMs;            // kRenewRetryS
    qint64 linkIdleCloseMs;         // kLinkIdleCloseS
    qint64 linkMaxAgeMs;            // kLinkMaxAgeS
    qint64 secretRetryMs;           // kSecretRetryS
    qint64 secretRetryCapMs;        // kSecretRetryCapS
    int reloginMax;                 // kReloginMax
};

// One Subject followed through a connection. `id` is the opaque UUID the store knows
// (REQ-SUB-01); `subjectRef` is the source's own identifier, opaque to the core. Names are
// not here: they live in the configuration, never in this module.
struct SubjectConfig
{
    QUuid id;
    QString subjectRef;
};

// One connection of the configuration (REQ-SUB-02): which adapter, which Subjects, and
// optional overrides of the nominal intervals (-1 = the SchedulerSettings default).
struct ConnectionConfig
{
    ConnectionConfig() : currentIntervalMs(-1), graphIntervalMs(-1), logbookIntervalMs(-1) {}

    QString id;
    QString adapterId;
    QList<SubjectConfig> subjects;
    qint64 currentIntervalMs;
    qint64 graphIntervalMs;
    qint64 logbookIntervalMs;
};

} // namespace lm

#endif
