// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_QTHTTPTRANSPORT_H
#define LM_QTHTTPTRANSPORT_H

#include "httptransport.h"

namespace lm {

// Transport on QNetworkAccessManager. Needs a Qt event loop in the calling thread (a
// QCoreApplication). Default TLS verification, no redirect following, no cookies kept
// between requests, no caching. A reply larger than kMaxReplyBytes is aborted.
class QtHttpTransport : public HttpTransport
{
public:
    // Ceiling for one reply body. The largest measured reply is about 29 kB; the margin is for longer logbooks. The reply is untrusted input.
    static const qint64 kMaxReplyBytes = 8 * 1024 * 1024;

    HttpResponse send(const HttpRequest &request);
};

} // namespace lm

#endif
