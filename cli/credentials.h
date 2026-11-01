// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LMCLI_CREDENTIALS_H
#define LMCLI_CREDENTIALS_H

#include "session/adapterlink.h"

#include <QString>

namespace lmcli {

// A secret file must be a regular file (no symbolic link), owned by the user running the
// program, with no permission for group or others (REQ-ADP-11, REQ-SEC-06). `error`
// never contains the file's content.
bool checkPrivateFile(const QString &path, QString *error);

// File of lines `username=<text>` and `password=<text>` (the text is the rest of the
// line, taken as it is). Blank lines and lines starting with # are ignored.
bool readCredentialsFile(const QString &path, lm::Credentials *out, QString *error);
bool parseCredentials(const QByteArray &content, lm::Credentials *out, QString *error);

// First line of the file is the opaque session. A missing file is not an error (empty).
bool readSessionFile(const QString &path, QString *session, QString *error);
// Created with mode 0600, never following a symbolic link.
bool writeSessionFile(const QString &path, const QString &session, QString *error);

// Username and hidden password on the controlling terminal. Fails when there is none.
bool promptCredentials(lm::Credentials *out, QString *error);

} // namespace lmcli

#endif
