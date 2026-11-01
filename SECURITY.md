# Security policy

## Reporting a vulnerability

Please report security problems privately, by e-mail to `info@michelebrami.it`. Do not open a public issue for them. Say what you found, how to reproduce it, which version you tested (the About page of the application shows it) and what you think the impact is. You can expect an answer within a few days; this is a one-person project, so a fix may take longer.

**Do not send real health data.** No glucose values, patient names, account names or passwords, tokens, or screenshots of the patient pages. Describe the problem with invented data, or use the simulator in `tools/simulator`. The diagnostic report that the application can write contains none of these and is safe to attach.

## What is in scope

- Leaks of credentials, session tokens or glucose data: into a file, a log, a command line, an environment variable, a notification, a diagnostic report or the network.
- Any network traffic other than the calls to the glucose sharing service that the user set up the account for. The application is local-only and has no telemetry.
- Ways for the data source, the adapter process or a crafted configuration or store file to run code, crash the service or make it silently stop alerting.
- Problems in how the secrets (the account login) are stored or read through Sailfish Secrets.
- Alert logic that can be made to stay silent without the user's own action (silent mode, snooze, Do not disturb).
- The packages: files installed with the wrong permissions, or a package that contains development-only code (the simulated data source and the base-URL override must be absent from release packages).

## What is out of scope

- The behaviour of the glucose sharing service itself, and its terms of use.
- An attacker who already has the unlocked phone, root access or the user's device-lock code.
- The fact that the interface the application reads is unofficial and may change or stop. This is stated in `README.md`.
- Findings that need a modified, non-release build.

## Supported versions

Only the latest release receives fixes.
