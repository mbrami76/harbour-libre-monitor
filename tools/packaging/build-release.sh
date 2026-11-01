#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# Builds the RELEASE packages (no devsource) from the COMMITTED tree (git archive, so that the package
# build's lupdate does not rewrite the .ts files of the worktree): the main package for aarch64 and
# i486, and the add-on (noarch). Then the Harbour suite on the aarch64 main package, the check that the
# release binaries carry no development-only trace (REQ-SIM-06), the metadata, and sha-256 of everything.
#
#   tools/packaging/build-release.sh TAG     -> ~/SFOS-DEV/harbour-libre-monitor-development/build/pkg-TAG/{build-aarch64,build-i486,build-addon}
#
# Run it from inside the working tree. Long: start it in the background with a log.
set -u
TAG=${1:?usage: build-release.sh TAG}
REPO=$(git rev-parse --show-toplevel)
OUT=$HOME/SFOS-DEV/harbour-libre-monitor-development/build/pkg-$TAG
SRC=$OUT/src
rm -rf "$OUT"; mkdir -p "$SRC"
git -C "$REPO" archive HEAD | tar -x -C "$SRC"
echo "commit $(git -C "$REPO" rev-parse --short HEAD)"
for arch in aarch64 i486; do
  B=$OUT/build-$arch; mkdir -p "$B"; cd "$B"
  sfdk -c target=SailfishOS-5.1.0.11-$arch build "$SRC" > build.log 2>&1
  echo "$arch build rc=$?"; grep -E ' error|Error [0-9]|^error' build.log | head -10
done
# The add-on: its own spec, built in its own copy (it has no qmake project).
B=$OUT/build-addon; cp -r "$SRC/addon" "$B"; cd "$B"
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build > build.log 2>&1
echo "addon build rc=$?"; grep -E -i 'error' build.log | head -10

R=$(ls "$OUT"/build-aarch64/RPMS/harbour-libre-monitor-[0-9]*.aarch64.rpm | head -1)
cd "$OUT/build-aarch64"
sfdk -c target=SailfishOS-5.1.0.11-aarch64 check --suites harbour "$R" > check.log 2>&1
echo "harbour check rc=$?"; sed 's/\x1b\[[0-9;]*m//g' check.log | grep -E 'ERROR|WARNING|FAILED|Validation' | head -20
# The release binaries must carry no development-only trace (REQ-SIM-06); the contact is compiled in.
for arch in aarch64 i486; do
  BIN=$OUT/build-$arch/app/harbour-libre-monitor
  echo "$arch traces: $( (strings "$BIN"; strings -el "$BIN") | grep -c -E 'LM_DEV_PAGE|LM_DEV_SCENARIO|LM_DEV_SOURCE|DevInject|DevFeed|dev-base-url|dev-credentials|notify\.log|Every band')"
  # QStringLiteral keeps the text in UTF-16: strings -el.
  echo "$arch contact strings: $(strings -el "$OUT/build-$arch/app/harbour-libre-monitor" | grep -c 'info@michelebrami.it')"
done
# Metadata and scriptlets, read with the build engine's rpm (the PC has none).
for p in "$OUT"/build-*/RPMS/*.rpm; do
  echo "--- $(basename "$p")"
  sfdk engine exec rpm -qp --qf 'version %{VERSION}-%{RELEASE} arch %{ARCH} url %{URL} packager %{PACKAGER} license %{LICENSE}\n' "$p" 2>/dev/null
  echo "scriptlets and triggers: $(sfdk engine exec rpm -qp --scripts --triggers "$p" 2>/dev/null | grep -c scriptlet)"
done
sha256sum "$OUT"/build-*/RPMS/*.rpm
echo PKG_DONE
