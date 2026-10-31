/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibRequests/CertificateSummary.h>
#include <LibURL/Forward.h>

typedef void CURL;

namespace RequestServer {

// Remembers the certificate each host last presented to this RequestServer, so the browser can show it.
void record_peer_certificate(CURL*, URL::URL const&);
Optional<Requests::CertificateSummary> peer_certificate_for_host(ByteString const& host);

}
