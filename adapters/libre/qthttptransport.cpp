// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "qthttptransport.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace lm {

HttpResponse QtHttpTransport::send(const HttpRequest &request)
{
    HttpResponse response;
    if (request.timeoutMs <= 0) {
        response.fault = HttpResponse::Timeout;
        return response;
    }

    // A manager per request: no cookie jar or connection state survives (REQ-ADP-03).
    QNetworkAccessManager manager;
    QNetworkRequest nr(request.url);
    nr.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    for (int i = 0; i < request.headers.size(); ++i)
        nr.setRawHeader(request.headers.at(i).first, request.headers.at(i).second);

    QNetworkReply *reply = request.method == "POST" ? manager.post(nr, request.body)
                                                    : manager.get(nr);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    bool tooLarge = false;

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        timedOut = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 received, qint64) {
        if (received > kMaxReplyBytes) {
            tooLarge = true;
            reply->abort();
        }
    });
    timer.start(request.timeoutMs);
    loop.exec();
    timer.stop();

    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (timedOut) {
        response.fault = HttpResponse::Timeout;
    } else if (tooLarge) {
        response.fault = HttpResponse::TooLarge;
    } else if (!status.isValid()) {
        response.fault = HttpResponse::NetworkError;
    } else {
        response.status = status.toInt();
        response.body = reply->readAll();
        response.lvapi = reply->rawHeader("lvapi");
        response.retryAfter = reply->rawHeader("Retry-After");
    }
    reply->deleteLater();
    return response;
}

} // namespace lm
