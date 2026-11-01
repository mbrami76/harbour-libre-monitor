// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_HTTPTRANSPORT_H
#define LM_HTTPTRANSPORT_H

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QUrl>

namespace lm {

// One HTTP request. Headers carry secrets (token, account-id): nobody logs a request.
struct HttpRequest
{
    HttpRequest() : timeoutMs(0) {}

    QByteArray method;                              // "GET" or "POST"
    QUrl url;
    QList<QPair<QByteArray, QByteArray> > headers;
    QByteArray body;
    int timeoutMs;                                  // whole request, from the command deadline
};

struct HttpResponse
{
    enum Fault { NoFault, Timeout, NetworkError, TooLarge };

    HttpResponse() : fault(NoFault), status(0) {}

    Fault fault;            // anything but NoFault: no usable HTTP answer
    int status;             // HTTP status code
    QByteArray body;
    QByteArray lvapi;       // value of the "lvapi" reply header, a possible change signal
    QByteArray retryAfter;  // value of "Retry-After", if present
};

// The only door to the network. One request in flight; calls block until the answer, a
// fault or the timeout. A fake implementation lets tests run without sockets.
class HttpTransport
{
public:
    virtual ~HttpTransport() {}
    virtual HttpResponse send(const HttpRequest &request) = 0;
};

} // namespace lm

#endif
