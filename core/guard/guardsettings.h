// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_GUARDSETTINGS_H
#define LM_GUARDSETTINGS_H

// Every tunable number of the guard in one place.
// All values are PROVISIONAL starting points chosen from the documents, not
// from measurements on a phone; each is recorded and may be
// changed per build by filling another GuardSettings, without touching the logic. The field
// comments name the constant of the document.

#include "session/linkpolicy.h"

#include <QtGlobal>

namespace lm {

struct GuardSettings
{
    GuardSettings()
        : weightHard(10.0), weightBatchTooLarge(2.0), weightRateAboveRegistry(3.0),
          weightBudgetExceeded(1.0), weightStderrFlood(1.0), weightRecordsSentMismatch(1.0),
          decayHalfLifeMs(30LL * 60 * 1000), tripLimit(15.0),
          lineBucketCap(200), lineRefillPerS(100), byteBucketCap(2.0 * 1024 * 1024),
          byteRefillPerS(512.0 * 1024), bucketRearmFraction(0.5), pauseMaxMs(2000),
          recordsPerCommandMax(5000), recordsWindowMax(20000), recordsWindowMs(600LL * 1000),
          rateSlack(5), spawnBucketCap(5), spawnRefillMs(120LL * 1000),
          circuitFailures(3), circuitWindowMs(600LL * 1000), circuitOpenMs(900LL * 1000),
          circuitOpenCapMs(7200LL * 1000), versionTripWindowMs(7LL * 24 * 3600 * 1000),
          versionTripConnections(2), ingestQueueMax(1000),
          memoryLimitBytes(0), cpuTimeLimitS(1800), fileDescriptorLimit(64),
          residentLimitBytes(128LL * 1024 * 1024) {}

    // Violation weights (REQ-GRD-04): kWeightHard and kWeightSoft[kind].
    double weightHard;
    double weightBatchTooLarge;
    double weightRateAboveRegistry;
    double weightBudgetExceeded;
    double weightStderrFlood;
    double weightRecordsSentMismatch;

    qint64 decayHalfLifeMs;         // kDecayHalfLifeS
    double tripLimit;               // kTripLimit

    // Token buckets per link (REQ-GRD-01): kLineBucket*, kByteBucket*.
    double lineBucketCap;
    double lineRefillPerS;
    double byteBucketCap;
    double byteRefillPerS;
    double bucketRearmFraction;     // kBucketRearmFraction
    int pauseMaxMs;                 // kPauseMaxMs

    int recordsPerCommandMax;       // kRecordsPerCommandMax
    int recordsWindowMax;           // kRecordsWindowMax
    qint64 recordsWindowMs;         // kRecordsWindowS
    int rateSlack;                  // kRateSlack

    // Spawn rate (REQ-GRD-01): kSpawnBucketCap, kSpawnRefillS.
    double spawnBucketCap;
    qint64 spawnRefillMs;

    // Circuit breaker (REQ-GRD-03): kCircuitFailures, kCircuitWindowS, kCircuitOpenS, cap.
    int circuitFailures;
    qint64 circuitWindowMs;
    qint64 circuitOpenMs;
    qint64 circuitOpenCapMs;

    // Per-adapter-version trip (REQ-GRD-07).
    qint64 versionTripWindowMs;
    int versionTripConnections;

    int ingestQueueMax;             // kIngestQueueMax

    // OS limits for the adapter process, 0 = none. The address-space limit stays off: the
    // adapter's address space is hundreds of MB while its resident size is about 26 MB
    // The resident limit that replaced it is below.
    qint64 memoryLimitBytes;
    int cpuTimeLimitS;
    int fileDescriptorLimit;
    // The driver ends an adapter whose resident size (VmHWM) passes this many bytes:
    // ON by default at 128 MB, about five times the 26 MB measured after 11 hours of real
    // polling over TLS. PROVISIONAL; 0 turns it off for a test.
    qint64 residentLimitBytes;

    // Weight of a violation kind; hard kinds all weigh weightHard.
    double weightOf(ViolationKind kind) const
    {
        switch (kind) {
        case ViolationKind::BatchTooLarge: return weightBatchTooLarge;
        case ViolationKind::RateAboveRegistry: return weightRateAboveRegistry;
        case ViolationKind::BudgetExceeded: return weightBudgetExceeded;
        case ViolationKind::StderrFlood: return weightStderrFlood;
        case ViolationKind::RecordsSentMismatch: return weightRecordsSentMismatch;
        default: return weightHard;
        }
    }
};

} // namespace lm

#endif
