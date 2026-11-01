// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "credentials.h"

#include <QFile>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace lmcli {

bool checkPrivateFile(const QString &path, QString *error)
{
    struct stat st;
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &st) != 0) {
        *error = QStringLiteral("the file cannot be read");
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *error = QStringLiteral("the file is not a regular file (symbolic links are refused)");
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        *error = QStringLiteral("the file is not owned by the current user");
        return false;
    }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        *error = QStringLiteral("the file is accessible to other users; run chmod 600 on it");
        return false;
    }
    return true;
}

bool parseCredentials(const QByteArray &content, lm::Credentials *out, QString *error)
{
    bool haveUser = false, havePassword = false;
    lm::Credentials c;
    const QList<QByteArray> lines = content.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith('#'))
            continue;
        if (line.startsWith("username=")) {
            if (haveUser) {
                *error = QStringLiteral("username is given twice");
                return false;
            }
            c.username = QString::fromUtf8(line.mid(9));
            haveUser = true;
        } else if (line.startsWith("password=")) {
            if (havePassword) {
                *error = QStringLiteral("password is given twice");
                return false;
            }
            c.password = QString::fromUtf8(line.mid(9));
            havePassword = true;
        } else {
            // Do not quote the line: it may be a secret in the wrong place.
            *error = QStringLiteral("line %1 is neither username= nor password=").arg(i + 1);
            return false;
        }
    }
    if (!haveUser || !havePassword || c.username.isEmpty() || c.password.isEmpty()) {
        *error = QStringLiteral("the file needs a username= and a password= line");
        return false;
    }
    *out = c;
    return true;
}

bool readCredentialsFile(const QString &path, lm::Credentials *out, QString *error)
{
    if (!checkPrivateFile(path, error))
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("the file cannot be read");
        return false;
    }
    return parseCredentials(f.readAll(), out, error);
}

bool readSessionFile(const QString &path, QString *session, QString *error)
{
    session->clear();
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0 && errno == ENOENT)
        return true;
    if (!checkPrivateFile(path, error))
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("the file cannot be read");
        return false;
    }
    QByteArray line = f.readLine();
    while (line.endsWith('\n') || line.endsWith('\r'))
        line.chop(1);
    *session = QString::fromUtf8(line);
    return true;
}

bool writeSessionFile(const QString &path, const QString &session, QString *error)
{
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = QStringLiteral("the session file cannot be written");
        return false;
    }
    ::fchmod(fd, 0600);
    const QByteArray data = session.toUtf8() + '\n';
    const ssize_t n = ::write(fd, data.constData(), data.size());
    ::close(fd);
    if (n != data.size()) {
        *error = QStringLiteral("the session file cannot be written");
        return false;
    }
    return true;
}

bool promptCredentials(lm::Credentials *out, QString *error)
{
    if (!::isatty(STDIN_FILENO) || !::isatty(STDERR_FILENO)) {
        *error = QStringLiteral("no terminal for a prompt: use --credentials-file");
        return false;
    }
    char buffer[512];
    std::fputs("username: ", stderr);
    std::fflush(stderr);
    if (!std::fgets(buffer, sizeof(buffer), stdin)) {
        *error = QStringLiteral("no username entered");
        return false;
    }
    lm::Credentials c;
    c.username = QString::fromUtf8(buffer).trimmed();

    struct termios old;
    ::tcgetattr(STDIN_FILENO, &old);
    struct termios hidden = old;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    std::fputs("password: ", stderr);
    std::fflush(stderr);
    const bool got = std::fgets(buffer, sizeof(buffer), stdin) != 0;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    std::fputs("\n", stderr);
    if (!got) {
        *error = QStringLiteral("no password entered");
        return false;
    }
    QString password = QString::fromUtf8(buffer);
    while (password.endsWith(QLatin1Char('\n')) || password.endsWith(QLatin1Char('\r')))
        password.chop(1);
    c.password = password;
    for (size_t i = 0; i < sizeof(buffer); ++i)
        buffer[i] = 0;
    if (c.username.isEmpty() || c.password.isEmpty()) {
        *error = QStringLiteral("username and password must not be empty");
        return false;
    }
    *out = c;
    return true;
}

} // namespace lmcli
