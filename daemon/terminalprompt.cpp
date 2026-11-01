// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "terminalprompt.h"

#include <QByteArray>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

namespace lm {

namespace {

// What the interrupt handler needs to put the terminal back. A signal handler may only touch
// data like this and call async-signal-safe functions (tcsetattr is one of them).
struct termios gSaved;
volatile sig_atomic_t gSavedFd = -1;

void restoreOnSignal(int sig)
{
    if (gSavedFd >= 0)
        ::tcsetattr(gSavedFd, TCSANOW, &gSaved);
    // Hand the signal on with its default effect (end the program), as if no handler existed.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void wipe(QByteArray &b)
{
    // volatile so the compiler cannot drop the writes as dead stores.
    volatile char *p = b.data();
    for (int i = 0; i < b.size(); ++i)
        p[i] = 0;
}

void writeAll(int fd, const char *s)
{
    const size_t n = ::strlen(s);
    size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(fd, s + done, n - done);
        if (w <= 0)
            return;
        done += size_t(w);
    }
}

} // namespace

bool readHiddenLine(int inFd, int outFd, const char *label, QString *line, QString *errorKey)
{
    line->clear();
    if (!::isatty(inFd)) {
        *errorKey = QStringLiteral("no-terminal");
        return false;
    }
    struct termios hidden;
    if (::tcgetattr(inFd, &gSaved) != 0) {
        *errorKey = QStringLiteral("no-terminal");
        return false;
    }
    hidden = gSaved;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL);
    gSavedFd = inFd;
    struct sigaction onSignal, oldInt, oldTerm;
    ::memset(&onSignal, 0, sizeof(onSignal));
    onSignal.sa_handler = restoreOnSignal;
    ::sigemptyset(&onSignal.sa_mask);
    ::sigaction(SIGINT, &onSignal, &oldInt);
    ::sigaction(SIGTERM, &onSignal, &oldTerm);
    // TCSAFLUSH also throws away anything typed ahead before the prompt appeared.
    if (::tcsetattr(inFd, TCSAFLUSH, &hidden) != 0) {
        gSavedFd = -1;
        ::sigaction(SIGINT, &oldInt, 0);
        ::sigaction(SIGTERM, &oldTerm, 0);
        *errorKey = QStringLiteral("no-terminal");
        return false;
    }

    writeAll(outFd, label);
    QByteArray typed;
    typed.reserve(1100);        // never reallocated, so no stale copy of the bytes is left behind
    bool ok = true;
    for (;;) {
        char c;
        const ssize_t n = ::read(inFd, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = !typed.isEmpty();      // end of input after some text still gives a line
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (typed.size() >= 1024) {      // a line this long is not a credential
            ok = false;
            break;
        }
        typed.append(c);
    }

    ::tcsetattr(inFd, TCSAFLUSH, &gSaved);
    gSavedFd = -1;
    ::sigaction(SIGINT, &oldInt, 0);
    ::sigaction(SIGTERM, &oldTerm, 0);
    writeAll(outFd, "\n");      // the Enter key was not echoed either

    if (ok)
        *line = QString::fromUtf8(typed.constData(), typed.size());
    wipe(typed);
    if (!ok)
        *errorKey = QStringLiteral("input-failed");
    return ok;
}

} // namespace lm
