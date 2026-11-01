// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The words for the state keys the C++ models return.
//
// Why a long switch: the models return KEYS ("ui-stale-unreachable"), never display text, and
// lupdate can only extract an identifier that appears as a literal in qsTrId("..."). So every
// key is written out here once, with its English wording beside it. A key this version does not
// know falls to the last case, which is honest about it (state ST-X).
function stateText(key) {
    switch (key) {
    case "ui-band-ll":
        //% "Very low"
        return qsTrId("ui-band-ll")
    case "ui-band-l":
        //% "Low"
        return qsTrId("ui-band-l")
    case "ui-band-n":
        //% "In range"
        return qsTrId("ui-band-n")
    case "ui-band-h":
        //% "High"
        return qsTrId("ui-band-h")
    case "ui-band-hh":
        //% "Very high"
        return qsTrId("ui-band-hh")
    case "ui-band-unknown":
        //% "Current reading"
        return qsTrId("ui-band-unknown")
    case "ui-subj-no-data-yet":
        //% "Waiting for the first reading"
        return qsTrId("ui-subj-no-data-yet")
    case "ui-stale-unreachable":
        //% "The source cannot be reached"
        return qsTrId("ui-stale-unreachable")
    case "ui-stale-rate-limited":
        //% "The source asked to slow down"
        return qsTrId("ui-stale-rate-limited")
    case "ui-stale-no-new":
        //% "The source has no new readings"
        return qsTrId("ui-stale-no-new")
    case "ui-stale-warmup":
        //% "The sensor is warming up"
        return qsTrId("ui-stale-warmup")
    case "ui-stale-guard":
        //% "Reading stopped for safety"
        return qsTrId("ui-stale-guard")
    case "ui-stale-service-gap":
        //% "Monitoring was not running"
        return qsTrId("ui-stale-service-gap")
    case "ui-stale-locked":
        //% "The stored login waits for your approval"
        return qsTrId("ui-stale-locked")
    case "ui-attn-auth-failed":
        //% "The login was refused. Check the password."
        return qsTrId("ui-attn-auth-failed")
    case "ui-attn-terms":
        //% "The source wants you to accept updated terms. Open the official app to do it. This app never accepts them for you."
        return qsTrId("ui-attn-terms")
    case "ui-attn-protocol-changed":
        //% "The source changed in a way this app does not understand"
        return qsTrId("ui-attn-protocol-changed")
    case "ui-attn-credentials-missing":
        //% "No login is stored for this account"
        return qsTrId("ui-attn-credentials-missing")
    case "ui-attn-subject-unavailable":
        //% "This patient is no longer visible to the account"
        return qsTrId("ui-attn-subject-unavailable")
    case "ui-attn-internal":
        //% "An internal problem stopped the reading"
        return qsTrId("ui-attn-internal")
    default:
        //% "No current reading"
        return qsTrId("ui-stale-other")
    }
}

// The word for the direction (the symbol is drawn by the model).
function directionText(key) {
    switch (key) {
    case "ui-dir-falling-fast":
        //% "Falling fast"
        return qsTrId("ui-dir-falling-fast")
    case "ui-dir-falling":
        //% "Falling"
        return qsTrId("ui-dir-falling")
    case "ui-dir-steady":
        //% "Steady"
        return qsTrId("ui-dir-steady")
    case "ui-dir-rising":
        //% "Rising"
        return qsTrId("ui-dir-rising")
    case "ui-dir-rising-fast":
        //% "Rising fast"
        return qsTrId("ui-dir-rising-fast")
    case "ui-dir-unknown-code":
        //% "Unknown trend code"
        return qsTrId("ui-dir-unknown-code")
    default:
        //% "No trend received"
        return qsTrId("ui-dir-null")
    }
}

// The words for the answers of the account page. The key comes from the
// logic (AccountSetup.errorKey): a fixed key, never the source's own text.
function accountError(key) {
    switch (key) {
    case "auth-failed":
        //% "The login was refused. Check the account name and the password."
        return qsTrId("ui-acct-err-auth")
    case "terms-must-be-accepted":
        //% "The source wants you to accept updated terms. Open the official app to do it, then test again. This app never accepts them for you."
        return qsTrId("ui-acct-err-terms")
    case "unreachable":
        //% "The source cannot be reached. Check the connection and try again."
        return qsTrId("ui-acct-err-unreachable")
    case "rate-limited":
        //% "The source asked to slow down. Wait a while before trying again."
        return qsTrId("ui-acct-err-rate")
    case "protocol-changed":
        //% "The source changed in a way this app does not understand."
        return qsTrId("ui-acct-err-protocol")
    case "no-subjects":
        //% "The login works, but no patient is shared with this account. Ask the patient to share their data with you in the official app."
        return qsTrId("ui-acct-err-nosubjects")
    case "not-available":
        //% "The monitoring service cannot do this: it is an older version, or it could not be started. Update the app, or start monitoring from the main page, and try again."
        return qsTrId("ui-acct-err-notavailable")
    case "secret-store-failed":
        //% "The login could not be stored safely. Nothing was saved."
        return qsTrId("ui-acct-err-secret")
    case "config-invalid":
    case "config-write-failed":
        //% "The settings could not be written. Nothing was saved."
        return qsTrId("ui-acct-err-config")
    case "unknown-connection":
        //% "The service does not know this account yet. Save it first, then try again."
        return qsTrId("ui-acct-err-unknown-connection")
    case "credentials-missing":
        //% "The service could not find the stored login. Enter the account name and the password again."
        return qsTrId("ui-acct-err-credentials-missing")
    case "secrets-locked":
        //% "The phone's secret store is locked. After a restart the system asks once for your approval: look for its notification or dialog, approve it, then try again."
        return qsTrId("ui-acct-err-secrets-locked")
    case "stopped-by-guard":
        //% "This account was stopped by the safety guard after unexpected behaviour of the source. Re-enable it on the main page first, then test again."
        return qsTrId("ui-acct-err-stopped-by-guard")
    case "service-stopping":
        //% "The monitoring service is stopping. Wait a moment and try again."
        return qsTrId("ui-acct-err-service-stopping")
    case "service-not-running":
        //% "The monitoring service is not running. Start monitoring from the main page, then try again."
        return qsTrId("ui-acct-err-service-not-running")
    case "internal":
    default:         // a key this version does not know is shown as "internal", never as received
        //% "An internal problem stopped this step. Nothing was saved."
        return qsTrId("ui-acct-err-internal")
    }
}

// The unit of a person and how it was determined (REQ-SUB-05).
function unitBasisText(basis, unit) {
    switch (basis) {
    case "code":
        //% "Unit: %1, from the source's unit code"
        return qsTrId("ui-acct-basis-code").arg(unit)
    case "relation":
        //% "Unit: %1, from the relation of the two values"
        return qsTrId("ui-acct-basis-relation").arg(unit)
    case "user":
        //% "Unit: %1, set by you"
        return qsTrId("ui-acct-basis-user").arg(unit)
    default:
        //% "Unit: %1, guessed from the size of the values"
        return qsTrId("ui-acct-basis-magnitude").arg(unit)
    }
}

// The name to show for a patient. A configuration written by the provisioning mode has no name (names
// are entered on the account page), so an empty name shows the patient's short label instead: the
// label the user chose, or P1, P2 by the place in the configuration file. The model computes that
// label with the same function the notifications use (lm::patientLabel), so a patient
// is named the same way on a page, on the cover and on the lock screen.
function displayName(name, shortLabel) {
    if (name && name.length > 0)
        return name
    return shortLabel || ""
}
