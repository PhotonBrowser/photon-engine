/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>

namespace Web::Fetch::Fetching {

// A `WWW-Authenticate` challenge the user can answer with a username and password.
struct AuthenticationChallenge {
    enum class Scheme : u8 {
        Basic,
        Digest,
    };

    // https://www.rfc-editor.org/rfc/rfc7616#section-3.3
    enum class DigestAlgorithm : u8 {
        MD5,
        MD5Session,
        SHA256,
        SHA256Session,
    };

    Scheme scheme { Scheme::Basic };
    String realm;

    // Digest only.
    String nonce;
    Optional<String> opaque;
    DigestAlgorithm algorithm { DigestAlgorithm::MD5 };
    bool quality_of_protection { false };
    bool stale { false };
};

// https://fetch.spec.whatwg.org/#authentication-entries
struct AuthenticationEntry {
    String username;
    String password;
    AuthenticationChallenge challenge;
    // How many requests have answered the challenge's nonce, sent as `nc`.
    u32 nonce_count { 0 };
};

// Kept for this process's lifetime and keyed by origin, which stands in for the protection space until requests
// carry their realm.
HashMap<String, AuthenticationEntry>& authentication_entries();
Optional<String> authentication_entry_key(URL::URL const&);

// The strongest challenge in a `WWW-Authenticate` value that a username and password can answer, preferring Digest
// over Basic as RFC 9110 asks.
WEB_API Optional<AuthenticationChallenge> choose_authentication_challenge(StringView www_authenticate);

// The `Authorization` value answering the entry's challenge for a request with `method` to `url`.
// `client_nonce` replaces the random one, for tests.
WEB_API String authorization_value_for(AuthenticationEntry&, StringView method, URL::URL const&, Optional<ByteString> client_nonce = {});

// The `Authorization` value for a username and password using the Basic scheme of RFC 7617.
WEB_API String basic_authorization_value(StringView username, StringView password);

}
