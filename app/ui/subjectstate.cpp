// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "subjectstate.h"

#include "uiconstants.h"

namespace lm {
namespace ui {

QString stateIdName(StateId id)
{
    switch (id) {
    case StateId::BandLL: return QStringLiteral("B-LL");
    case StateId::BandL: return QStringLiteral("B-L");
    case StateId::BandN: return QStringLiteral("B-N");
    case StateId::BandH: return QStringLiteral("B-H");
    case StateId::BandHH: return QStringLiteral("B-HH");
    case StateId::BandUnknown: return QStringLiteral("B-U");
    case StateId::NoDataYet: return QStringLiteral("N-00");
    case StateId::Unreachable: return QStringLiteral("ST-1");
    case StateId::NoNewReadings: return QStringLiteral("ST-2");
    case StateId::WarmUp: return QStringLiteral("ST-3");
    case StateId::StoppedByGuard: return QStringLiteral("ST-4");
    case StateId::ServiceGap: return QStringLiteral("ST-5");
    case StateId::CredentialsLocked: return QStringLiteral("ST-6");
    case StateId::LoginRefused: return QStringLiteral("ST-7a");
    case StateId::TermsMustBeAccepted: return QStringLiteral("ST-7b");
    case StateId::SourceChanged: return QStringLiteral("ST-7c");
    case StateId::CredentialsMissing: return QStringLiteral("ST-7d");
    case StateId::SubjectUnavailable: return QStringLiteral("ST-7e");
    case StateId::InternalProblem: return QStringLiteral("ST-7f");
    case StateId::NoCurrentReading: return QStringLiteral("ST-X");
    }
    return QString();
}

bool isFresh(StateId id)
{
    switch (id) {
    case StateId::BandLL: case StateId::BandL: case StateId::BandN:
    case StateId::BandH: case StateId::BandHH: case StateId::BandUnknown:
        return true;
    default:
        return false;
    }
}

namespace {

// A "needs attention" state: the detail names what the user has to do. The scheduler writes it
// in `cause`; the staleness record repeats it in `staleDetail`, so
// either is accepted. An unknown detail is an internal problem, not a guess.
StateId attentionId(const QString &detail)
{
    if (detail == QLatin1String("credentials-locked")) return StateId::CredentialsLocked;
    if (detail == QLatin1String("auth-failed")) return StateId::LoginRefused;
    if (detail == QLatin1String("terms")) return StateId::TermsMustBeAccepted;
    if (detail == QLatin1String("protocol-changed")) return StateId::SourceChanged;
    if (detail == QLatin1String("credentials-missing")) return StateId::CredentialsMissing;
    if (detail == QLatin1String("subject-unavailable")) return StateId::SubjectUnavailable;
    return StateId::InternalProblem;   // "internal", "adapter-rejected" and anything new
}

StateId bandId(Band b)
{
    switch (b) {
    case Band::LL: return StateId::BandLL;
    case Band::L: return StateId::BandL;
    case Band::N: return StateId::BandN;
    case Band::H: return StateId::BandH;
    case Band::HH: return StateId::BandHH;
    case Band::Unknown: return StateId::BandUnknown;
    }
    return StateId::BandUnknown;
}

} // namespace

SubjectView resolveSubject(const SubjectInput &in)
{
    SubjectView v;

    const bool disabled = in.state == QLatin1String("disabled-by-guard");
    const bool attention = in.state == QLatin1String("needs-attention");
    const bool guardKill = in.state == QLatin1String("backoff") && in.cause == QLatin1String("guard-kill");

    // The guard strip is independent of the headline.
    if (disabled) {
        v.guard = in.cause == QLatin1String("adapter-version") ? GuardStrip::VersionStopped
                                                                : GuardStrip::ConnectionStopped;
        v.showReenable = true;      // a button on the page, never only in a menu
    } else if (guardKill) {
        v.guard = GuardStrip::Interrupted;
        v.showRetry = true;         // REQ-GRD-14
    }

    // Headline, first match wins.
    if (disabled) {
        v.id = StateId::StoppedByGuard;
        return v;
    }
    if (attention) {
        const QString detail = in.cause.isEmpty() ? in.staleDetail : in.cause;
        v.id = attentionId(detail);
        switch (v.id) {
        case StateId::CredentialsLocked: v.showAskAgain = true; break;
        case StateId::LoginRefused: v.showEditAccount = true; break;
        case StateId::TermsMustBeAccepted: v.showRetry = true; break;
        case StateId::CredentialsMissing: v.showSetUpAccount = true; break;
        case StateId::SubjectUnavailable: v.showEditAccount = true; break;
        case StateId::InternalProblem: v.showRetry = true; break;
        default: break;
        }
        return v;
    }

    // The display grace: a LATE reading is not yet shown as a stale one. For the lateness causes
    // only (the source cannot be reached, the source has no new readings; rate-limited is a detail of
    // the first), the screen keeps the band, its colour and the blinking marker until the service's stale
    // flag has held for kUiStaleGraceS. Known states show at once. An unknown time since the flag came
    // (-1) gets no grace: the screen cannot claim a reading is fresh when it does not know how long it has
    // been late. The same service-side facts are used (the service's limit is unchanged).
    const bool lateness = in.stale && (in.staleCause == QLatin1String("source-unreachable")
                                       || in.staleCause == QLatin1String("no-new-readings"));
    const bool inGrace = lateness && in.staleSinceAgeS >= 0 && in.staleSinceAgeS < kUiStaleGraceS;
    const bool stale = in.stale && !inGrace;

    // A reading is shown as current only if the service says it is not stale (or is only just late)
    // AND the UI's own age check passes (REQ-UI-09). The second test is the
    // safety net for a dead service whose flag is frozen; during the grace the service is alive and
    // talking, so the net is widened by the grace.
    const qint64 maxAge = kUiFreshMaxS + (inGrace ? kUiStaleGraceS : 0);
    const bool ageOk = in.hasReading && in.ageS >= 0 && in.ageS <= maxAge;
    if (!stale && ageOk) {
        v.id = bandId(in.band);
        v.band = in.band;           // the only case that carries glucose colour (REQ-UI-05)
        return v;
    }

    if (!stale) {
        // The service says "fresh" but the newest reading is too old, absent or from the
        // future: never show it as current. With no reading at all it is a new Subject.
        v.id = in.hasReading ? StateId::NoCurrentReading : StateId::NoDataYet;
        return v;
    }

    // Stale: by cause (REQ-SCH-07).
    const QString &c = in.staleCause;
    if (c == QLatin1String("source-unreachable")) {
        v.id = StateId::Unreachable;
        v.rateLimited = in.staleDetail == QLatin1String("rate-limited");
        if (in.staleSinceAgeS >= kUiRetryOfferS)
            v.showRetry = true;
    } else if (c == QLatin1String("no-new-readings")) {
        v.id = StateId::NoNewReadings;
    } else if (c == QLatin1String("sensor-warm-up")) {
        v.id = StateId::WarmUp;
    } else if (c == QLatin1String("disabled-by-guard")) {
        v.id = StateId::StoppedByGuard;
    } else if (c == QLatin1String("service-not-running")) {
        v.id = StateId::ServiceGap;
    } else if (c == QLatin1String("credentials-locked")) {
        v.id = StateId::CredentialsLocked;
        v.showAskAgain = true;
    } else if (c == QLatin1String("needs-attention")) {
        v.id = attentionId(in.staleDetail);
    } else {
        v.id = StateId::NoCurrentReading;   // a cause this UI version does not know (ST-X)
    }
    return v;
}

QString stateTextKey(StateId id, bool rateLimited)
{
    switch (id) {
    case StateId::BandLL: return QStringLiteral("ui-band-ll");
    case StateId::BandL: return QStringLiteral("ui-band-l");
    case StateId::BandN: return QStringLiteral("ui-band-n");
    case StateId::BandH: return QStringLiteral("ui-band-h");
    case StateId::BandHH: return QStringLiteral("ui-band-hh");
    case StateId::BandUnknown: return QStringLiteral("ui-band-unknown");
    case StateId::NoDataYet: return QStringLiteral("ui-subj-no-data-yet");
    case StateId::Unreachable:
        return rateLimited ? QStringLiteral("ui-stale-rate-limited") : QStringLiteral("ui-stale-unreachable");
    case StateId::NoNewReadings: return QStringLiteral("ui-stale-no-new");
    case StateId::WarmUp: return QStringLiteral("ui-stale-warmup");
    case StateId::StoppedByGuard: return QStringLiteral("ui-stale-guard");
    case StateId::ServiceGap: return QStringLiteral("ui-stale-service-gap");
    case StateId::CredentialsLocked: return QStringLiteral("ui-stale-locked");
    case StateId::LoginRefused: return QStringLiteral("ui-attn-auth-failed");
    case StateId::TermsMustBeAccepted: return QStringLiteral("ui-attn-terms");
    case StateId::SourceChanged: return QStringLiteral("ui-attn-protocol-changed");
    case StateId::CredentialsMissing: return QStringLiteral("ui-attn-credentials-missing");
    case StateId::SubjectUnavailable: return QStringLiteral("ui-attn-subject-unavailable");
    case StateId::InternalProblem: return QStringLiteral("ui-attn-internal");
    case StateId::NoCurrentReading: return QStringLiteral("ui-stale-other");
    }
    return QString();
}

QString bandKey(Band band)
{
    switch (band) {
    case Band::LL: return QStringLiteral("ll");
    case Band::L: return QStringLiteral("l");
    case Band::N: return QStringLiteral("n");
    case Band::H: return QStringLiteral("h");
    case Band::HH: return QStringLiteral("hh");
    case Band::Unknown: break;
    }
    return QString();
}

} // namespace ui
} // namespace lm
