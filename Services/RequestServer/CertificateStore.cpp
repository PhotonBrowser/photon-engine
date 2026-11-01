/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <RequestServer/CertificateStore.h>
#include <RequestServer/Resolver.h>
#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace RequestServer {

static HashMap<ByteString, Requests::CertificateSummary>& certificates()
{
    static NeverDestroyed<HashMap<ByteString, Requests::CertificateSummary>> certificates;
    return *certificates;
}

static ByteString name_entry(X509_NAME* name, int nid)
{
    if (!name)
        return {};
    auto index = X509_NAME_get_index_by_NID(name, nid, -1);
    if (index < 0)
        return {};
    auto* data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index));
    unsigned char* utf8 = nullptr;
    auto length = ASN1_STRING_to_UTF8(&utf8, data);
    if (length < 0)
        return {};
    ByteString result { reinterpret_cast<char const*>(utf8), static_cast<size_t>(length) };
    OPENSSL_free(utf8);
    return result;
}

static ByteString time_string(ASN1_TIME const* time)
{
    auto* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    ByteString result;
    if (ASN1_TIME_print(bio, time) == 1) {
        char* data = nullptr;
        auto length = BIO_get_mem_data(bio, &data);
        if (length > 0)
            result = ByteString { data, static_cast<size_t>(length) };
    }
    BIO_free(bio);
    return result;
}

static ByteString sha256_fingerprint(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &length) != 1)
        return {};
    StringBuilder builder;
    for (unsigned int i = 0; i < length; ++i) {
        if (i)
            builder.append(':');
        builder.appendff("{:02X}", digest[i]);
    }
    return builder.to_byte_string();
}

void record_peer_certificate(CURL* handle, URL::URL const& url)
{
    if (url.scheme() != "https"sv || !url.host().has_value())
        return;

    // A reused connection presents the certificate already recorded for its host, so only a new one needs reading.
    long new_connections = 0;
    auto host = url.serialized_host().to_byte_string();
    if (curl_easy_getinfo(handle, CURLINFO_NUM_CONNECTS, &new_connections) == CURLE_OK && new_connections == 0
        && certificates().contains(host))
        return;

    curl_tlssessioninfo* session = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_TLS_SSL_PTR, &session) != CURLE_OK || !session
        || session->backend != CURLSSLBACKEND_OPENSSL || !session->internals)
        return;

    auto* ssl = static_cast<SSL*>(session->internals);
    auto* certificate = SSL_get0_peer_certificate(ssl);
    if (!certificate)
        return;

    auto* subject = X509_get_subject_name(certificate);
    auto* issuer = X509_get_issuer_name(certificate);
    Requests::CertificateSummary summary;
    summary.subject_common_name = name_entry(subject, NID_commonName);
    summary.subject_organization = name_entry(subject, NID_organizationName);
    summary.issuer_common_name = name_entry(issuer, NID_commonName);
    summary.issuer_organization = name_entry(issuer, NID_organizationName);
    summary.valid_from = time_string(X509_get0_notBefore(certificate));
    summary.valid_until = time_string(X509_get0_notAfter(certificate));
    summary.sha256_fingerprint = sha256_fingerprint(certificate);
    // The request got this far, so curl either verified the certificate or was told not to.
    summary.verified = !certificate_errors_allowed_for(url);
    if (auto const* protocol = SSL_get_version(ssl))
        summary.protocol = protocol;
    if (auto const* cipher = SSL_get_cipher_name(ssl))
        summary.cipher = cipher;
    if (auto* chain = SSL_get_peer_cert_chain(ssl)) {
        for (int i = 0; i < sk_X509_num(chain); ++i) {
            auto* link = sk_X509_value(chain, i);
            auto name = name_entry(X509_get_subject_name(link), NID_commonName);
            if (name.is_empty())
                name = name_entry(X509_get_subject_name(link), NID_organizationName);
            summary.chain.append(move(name));
        }
    }
    certificates().set(move(host), move(summary));
}

Optional<Requests::CertificateSummary> peer_certificate_for_host(ByteString const& host)
{
    auto certificate = certificates().find(host);
    if (certificate == certificates().end())
        return {};
    return certificate->value;
}

}
