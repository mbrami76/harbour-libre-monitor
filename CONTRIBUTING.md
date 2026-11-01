# Contributing

Thank you for your interest in Libre Monitor. Small, focused changes with tests are the easiest to review. For anything larger, open an issue or write to `info@michelebrami.it` first, so the idea can be discussed before you spend time on it.

Read the safety statement in `README.md` first. This is a supplementary monitor for people who care for someone with diabetes; a mistake in an alert can matter, so behaviour changes need a test and a clear reason.

## Building and testing

The project builds with the Sailfish SDK (3.13 or later), qmake and the Docker build engine. Packages are built for the aarch64 (phone) and i486 (emulator) targets. Paths must not contain spaces.

The unit tests do not need a phone. They run on the PC in the SDK's i486 target, which provides the Qt version of the target (5.6). One-time setup of the test library:

```
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell --maintain zypper -n install qt5-qttest-devel
```

Build and run the tests in a shadow build folder outside the source tree:

```
mkdir -p ../build-host-i486 && cd ../build-host-i486
sfdk -c target=SailfishOS-5.1.0.11-i486 build-init                      # once
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell qmake <path to the source>/harbour-libre-monitor.pro CONFIG+=hostbuild
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell make -j4
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell make check
```

Look for `Totals` and `FAIL` in the output of the last command. The `hostbuild` configuration also builds the command-line program and turns on the development switches (a simulated data source and a base-URL override). Release and Harbour packages are built without them.

The simulator for the data source is a Python 3 program that uses only the standard library. Its tests run with `cd tools/simulator && python3 -m unittest`; `tools/simulator/README.md` explains how to start it.

Before you send a change:

- the host tests pass, and a new behaviour has a test that fails without it;
- a release package builds, and the Harbour check passes on the aarch64 package (`sfdk -c target=SailfishOS-5.1.0.11-aarch64 check --suites harbour RPMS/<package>.rpm`) if you changed the package, the `.pro` files, the desktop file or anything that is installed;
- tests and examples use invented data only. Never put real glucose values, names, account details or captures of the real service into the repository.

## Code style

Follow the code that is already there.

- C++11 with Qt 5.6, QML with Silica. No new dependency without a discussion.
- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later` and `// Copyright (C) 2026 <your name>` (for a new file; keep the existing line in a file you change).
- Documentation and comments are in English. User-visible text is a translatable string (English in the code, translations in the `.ts` files).
- Comments say what a file, class or non-trivial function is for, which pattern it follows and why, and which assumptions it relies on, in plain words. Explain why, not what the next line does. A requirement number (`REQ-...`, see `docs/requirements.md`) may point to the detail but never replaces the explanation.
- Glucose values are stored as the source reports them, with their unit. Conversion happens only for display.
- Colour in the interface is reserved for the five glucose bands. Every other state uses an icon and text.
- Keep the architecture rules in `docs/architecture.md`: the store is append-only, the adapter process never opens the store or the configuration file, secrets are never written to a file, a log or a command line.

## AI-generated code

Much of this project's own code was written with the help of an AI assistant, and the author reviews and tests everything that is published. Contributions written with AI help are accepted under the same conditions as any other contribution:

- you have read and understood every line you submit, and you can explain why it is correct;
- you have run it and tested it, and the tests are part of the change;
- you have the right to submit it under GPL-3.0-or-later, and it contains nothing copied from a source with an incompatible licence.

Say in the description of your change that AI help was used, if it was. A change that its author cannot explain will be declined, whatever tool wrote it.

## Licence

By contributing you agree that your work is published under GPL-3.0-or-later, the licence of the project (`LICENSE`).
