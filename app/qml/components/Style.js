// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
.pragma library

// The only colours of the application (REQ-UI-03): the five glucose bands, as a box with a fixed
// fill and a fixed text colour, so the contrast does not depend on the ambience (
// 6.2). Text-on-fill contrast ratios are 6.2 or better. The keys are the band identifiers of the model
// (`bandKey`, "ll" to "hh"); the same fills are in the round badges of design/export/ui.
var bandFill = { "ll": "#8E0F1B", "l": "#FF9F1C", "n": "#1F6F3A", "h": "#F2D12E", "hh": "#5B2A86" }
var bandText = { "ll": "#FFFFFF", "l": "#000000", "n": "#FFFFFF", "h": "#000000", "hh": "#FFFFFF" }

// THE PICTOGRAM LIST: every pictogram of a
// non-glucose state, in one place, so that one test can check them (tests/integration/
// check_emulator_pictograms.py reads this list and looks for each Silica name in the theme of the
// emulator image, /usr/share/themes/sailfish-default/silica/*/icons-monochrome/). 18 are Silica's
// monochrome icons; 3 are the author's own drawings (design/export/ui, white strokes like Silica's,
// installed in the application's icons/ui folder), written "own:<file>". Every pictogram is drawn
// with the Silica `Icon` item and tinted with the theme's colour, the own ones exactly like Silica's
// None is a triangle or looks like a band badge:
// the problem and the guard states have their own round drawings because `icon-m-warning` is a
// triangle. A missing icon draws nothing; the text always carries the meaning (REQ-UI-04).
var pictograms = {
    "busy": "icon-m-sync",                  // connecting, starting, no reading yet
    "running": "icon-m-acknowledge",
    "stopped": "icon-m-stop",               // not running
    "interrupted": "icon-m-clock",          // interrupted, no new readings
    "approval": "icon-m-device-lock",       // waiting for approval, login locked
    "problem": "own:state-problem",         // problem, attention banner, no answer
    "unreachable": "icon-m-wlan-no-signal", // source unreachable
    "warmup": "icon-m-timer",               // sensor warm-up
    "guard": "own:state-guard",             // stopped for safety
    "retry": "icon-m-refresh",
    "login": "icon-m-keys",                 // login refused, credentials missing
    "document": "icon-m-document",          // terms, report
    "person": "icon-m-person",              // a patient no longer shared
    "unconfirmed": "icon-m-question",
    "storage": "icon-m-sd-card",
    "add": "icon-m-add",
    "about": "icon-m-about",
    "simulated": "icon-m-developer-mode",
    "silent": "icon-m-speaker-mute",        // silent mode
    "dnd": "icon-m-do-not-disturb",         // Do not disturb
    "sensor": "own:sensor"                  // sensor block
}

// The monitoring strip's states (MonitoringInfo.status) and their pictograms.
var icons = {
    "connecting": pictograms["busy"],
    "starting": pictograms["busy"],
    "running": pictograms["running"],
    "stopped": pictograms["stopped"],
    "approval": pictograms["approval"],
    "call-failed": pictograms["problem"],
    "service-older": pictograms["problem"],         // a problem to act on, not a band
    "service-newer": pictograms["problem"]          // the same, the window is the old one
}

// The image source of a pictogram name of the list. A relative path is resolved against the QML file
// that binds it; every page and component lives one folder below qml/, so "../../icons/ui/" is the
// application's icons/ui folder from all of them.
function pictogramSource(name) {
    if (name.indexOf("own:") === 0)
        return "../../icons/ui/" + name.substring(4) + ".svg"
    return "image://theme/" + name
}

// Which pictogram goes with a state key of the model (stateKey role).
function iconForState(stateKey) {
    switch (stateKey) {
    case "ui-stale-unreachable":
    case "ui-stale-rate-limited": return pictograms["unreachable"]
    case "ui-stale-no-new":
    case "ui-stale-service-gap": return pictograms["interrupted"]
    case "ui-stale-warmup": return pictograms["warmup"]
    case "ui-stale-guard": return pictograms["guard"]
    case "ui-stale-locked": return pictograms["approval"]
    case "ui-attn-auth-failed":
    case "ui-attn-credentials-missing": return pictograms["login"]
    case "ui-attn-terms": return pictograms["document"]
    case "ui-attn-subject-unavailable": return pictograms["person"]
    case "ui-subj-no-data-yet": return pictograms["busy"]
    default: return pictograms["problem"]          // source changed, internal, a cause this version does not know
    }
}
