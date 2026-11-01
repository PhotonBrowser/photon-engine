/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>

namespace Requests {

// What the browser shows about the certificate a host presented.
struct CertificateSummary {
    ByteString subject_common_name;
    ByteString subject_organization;
    ByteString issuer_common_name;
    ByteString issuer_organization;
    ByteString valid_from;
    ByteString valid_until;
    // Colon-separated uppercase hex, as certificate viewers show it.
    ByteString sha256_fingerprint;
    // Whether the connection verified it; false once the user chose to proceed past a certificate error.
    bool verified { false };
    // The connection's protocol and cipher, such as "TLSv1.3" and "TLS_AES_128_GCM_SHA256".
    ByteString protocol;
    ByteString cipher;
    // Who each certificate the server sent is issued to, from the site's own towards its root.
    Vector<ByteString> chain;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::CertificateSummary const&);

template<>
ErrorOr<Requests::CertificateSummary> decode(Decoder&);

}
