/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/CertificateSummary.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::CertificateSummary const& summary)
{
    TRY(encoder.encode(summary.subject_common_name));
    TRY(encoder.encode(summary.subject_organization));
    TRY(encoder.encode(summary.issuer_common_name));
    TRY(encoder.encode(summary.issuer_organization));
    TRY(encoder.encode(summary.valid_from));
    TRY(encoder.encode(summary.valid_until));
    TRY(encoder.encode(summary.sha256_fingerprint));
    TRY(encoder.encode(summary.verified));
    TRY(encoder.encode(summary.protocol));
    TRY(encoder.encode(summary.cipher));
    TRY(encoder.encode(summary.chain));
    return {};
}

template<>
ErrorOr<Requests::CertificateSummary> decode(Decoder& decoder)
{
    Requests::CertificateSummary summary;
    summary.subject_common_name = TRY(decoder.decode<ByteString>());
    summary.subject_organization = TRY(decoder.decode<ByteString>());
    summary.issuer_common_name = TRY(decoder.decode<ByteString>());
    summary.issuer_organization = TRY(decoder.decode<ByteString>());
    summary.valid_from = TRY(decoder.decode<ByteString>());
    summary.valid_until = TRY(decoder.decode<ByteString>());
    summary.sha256_fingerprint = TRY(decoder.decode<ByteString>());
    summary.verified = TRY(decoder.decode<bool>());
    summary.protocol = TRY(decoder.decode<ByteString>());
    summary.cipher = TRY(decoder.decode<ByteString>());
    summary.chain = TRY(decoder.decode<Vector<ByteString>>());
    return summary;
}

}
