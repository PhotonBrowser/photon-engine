/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/Hex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCrypto/Hash/MD5.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Fetching/HTTPAuthentication.h>

namespace Web::Fetch::Fetching {

HashMap<String, AuthenticationEntry>& authentication_entries()
{
    static NeverDestroyed<HashMap<String, AuthenticationEntry>> entries;
    return *entries;
}

Optional<String> authentication_entry_key(URL::URL const& url)
{
    auto origin = url.origin();
    if (origin.is_opaque())
        return {};
    return origin.serialize();
}

namespace {

struct ParsedChallenge {
    StringView scheme;
    HashMap<ByteString, ByteString> parameters;

    Optional<StringView> parameter(StringView name) const
    {
        auto value = parameters.get(name);
        if (!value.has_value())
            return {};
        return value->view();
    }
};

bool is_token_end(char c)
{
    return is_ascii_space(c) || c == ',' || c == '=';
}

// https://www.rfc-editor.org/rfc/rfc9110#section-11.2
// A challenge is a scheme followed by comma-separated `name=value` parameters, and one header value may hold several
// challenges, so a token that is not followed by `=` starts the next challenge.
Vector<ParsedChallenge> parse_challenges(StringView value)
{
    Vector<ParsedChallenge> challenges;
    GenericLexer lexer { value };
    while (true) {
        lexer.ignore_while([](char c) { return is_ascii_space(c) || c == ','; });
        if (lexer.is_eof())
            break;
        auto token = lexer.consume_until(is_token_end);
        if (token.is_empty()) {
            lexer.ignore();
            continue;
        }
        lexer.ignore_while(is_ascii_space);
        if (!lexer.next_is('=')) {
            challenges.append({ token, {} });
            continue;
        }
        lexer.ignore();
        lexer.ignore_while(is_ascii_space);
        ByteString parameter_value;
        if (lexer.next_is('"')) {
            lexer.ignore();
            StringBuilder builder;
            while (!lexer.is_eof() && !lexer.next_is('"')) {
                if (lexer.next_is('\\'))
                    lexer.ignore();
                if (!lexer.is_eof())
                    builder.append(lexer.consume());
            }
            lexer.ignore();
            parameter_value = builder.to_byte_string();
        } else {
            parameter_value = lexer.consume_until([](char c) { return is_ascii_space(c) || c == ','; });
        }
        if (!challenges.is_empty())
            challenges.last().parameters.set(ByteString(token).to_lowercase(), move(parameter_value));
    }
    return challenges;
}

Optional<AuthenticationChallenge::DigestAlgorithm> digest_algorithm(Optional<StringView> name)
{
    using enum AuthenticationChallenge::DigestAlgorithm;
    if (!name.has_value() || name->equals_ignoring_ascii_case("MD5"sv))
        return MD5;
    if (name->equals_ignoring_ascii_case("MD5-sess"sv))
        return MD5Session;
    if (name->equals_ignoring_ascii_case("SHA-256"sv))
        return SHA256;
    if (name->equals_ignoring_ascii_case("SHA-256-sess"sv))
        return SHA256Session;
    return {};
}

StringView digest_algorithm_name(AuthenticationChallenge::DigestAlgorithm algorithm)
{
    switch (algorithm) {
    case AuthenticationChallenge::DigestAlgorithm::MD5:
        return "MD5"sv;
    case AuthenticationChallenge::DigestAlgorithm::MD5Session:
        return "MD5-sess"sv;
    case AuthenticationChallenge::DigestAlgorithm::SHA256:
        return "SHA-256"sv;
    case AuthenticationChallenge::DigestAlgorithm::SHA256Session:
        return "SHA-256-sess"sv;
    }
    VERIFY_NOT_REACHED();
}

String string_from(Optional<StringView> value)
{
    return String::from_utf8(value.value_or({})).value_or({});
}

Optional<AuthenticationChallenge> digest_challenge(ParsedChallenge const& parsed)
{
    auto nonce = parsed.parameter("nonce"sv);
    auto algorithm = digest_algorithm(parsed.parameter("algorithm"sv));
    if (!nonce.has_value() || !algorithm.has_value())
        return {};

    // Only `auth` protects nothing but the request line, which is all we can answer; a challenge offering only
    // `auth-int` cannot be met.
    bool quality_of_protection = false;
    if (auto qop = parsed.parameter("qop"sv); qop.has_value()) {
        for (auto option : qop->split_view(',')) {
            if (option.trim_whitespace().equals_ignoring_ascii_case("auth"sv))
                quality_of_protection = true;
        }
        if (!quality_of_protection)
            return {};
    }

    AuthenticationChallenge challenge;
    challenge.scheme = AuthenticationChallenge::Scheme::Digest;
    challenge.realm = string_from(parsed.parameter("realm"sv));
    challenge.nonce = string_from(nonce);
    if (auto opaque = parsed.parameter("opaque"sv); opaque.has_value())
        challenge.opaque = string_from(opaque);
    challenge.algorithm = *algorithm;
    challenge.quality_of_protection = quality_of_protection;
    challenge.stale = parsed.parameter("stale"sv).value_or({}).equals_ignoring_ascii_case("true"sv);
    return challenge;
}

bool uses_sha256(AuthenticationChallenge::DigestAlgorithm algorithm)
{
    return algorithm == AuthenticationChallenge::DigestAlgorithm::SHA256
        || algorithm == AuthenticationChallenge::DigestAlgorithm::SHA256Session;
}

bool uses_session(AuthenticationChallenge::DigestAlgorithm algorithm)
{
    return algorithm == AuthenticationChallenge::DigestAlgorithm::MD5Session
        || algorithm == AuthenticationChallenge::DigestAlgorithm::SHA256Session;
}

ByteString hash_hex(AuthenticationChallenge::DigestAlgorithm algorithm, StringView data)
{
    if (uses_sha256(algorithm))
        return encode_hex(Crypto::Hash::SHA256::hash(data.bytes()).bytes());
    return encode_hex(Crypto::Hash::MD5::hash(data.bytes()).bytes());
}

// https://www.rfc-editor.org/rfc/rfc9110#section-5.6.4
ByteString quoted(StringView value)
{
    StringBuilder builder;
    builder.append('"');
    for (auto c : value) {
        if (c == '"' || c == '\\')
            builder.append('\\');
        builder.append(c);
    }
    builder.append('"');
    return builder.to_byte_string();
}

}

Optional<AuthenticationChallenge> choose_authentication_challenge(StringView www_authenticate)
{
    Optional<AuthenticationChallenge> basic;
    Optional<AuthenticationChallenge> digest;
    for (auto const& parsed : parse_challenges(www_authenticate)) {
        if (parsed.scheme.equals_ignoring_ascii_case("Digest"sv)) {
            auto challenge = digest_challenge(parsed);
            // Prefer the stronger algorithm when a server offers several.
            if (challenge.has_value() && (!digest.has_value() || uses_sha256(challenge->algorithm)))
                digest = move(challenge);
        } else if (parsed.scheme.equals_ignoring_ascii_case("Basic"sv) && !basic.has_value()) {
            basic = AuthenticationChallenge {};
            basic->realm = string_from(parsed.parameter("realm"sv));
        }
    }
    if (digest.has_value())
        return digest;
    return basic;
}

String basic_authorization_value(StringView username, StringView password)
{
    auto payload = ByteString::formatted("{}:{}", username, password);
    return MUST(String::formatted("Basic {}", MUST(encode_base64(payload.bytes()))));
}

// https://www.rfc-editor.org/rfc/rfc7616#section-3.4
String authorization_value_for(AuthenticationEntry& entry, StringView method, URL::URL const& url, Optional<ByteString> client_nonce)
{
    auto const& challenge = entry.challenge;
    if (challenge.scheme == AuthenticationChallenge::Scheme::Basic)
        return basic_authorization_value(entry.username, entry.password);

    auto algorithm = challenge.algorithm;
    StringBuilder uri;
    uri.append(url.serialize_path());
    if (auto query = url.query(); query.has_value())
        uri.appendff("?{}", *query);
    auto request_uri = uri.to_byte_string();

    auto cnonce = client_nonce.value_or_lazy_evaluated([] {
        u8 bytes[16];
        fill_with_random({ bytes, sizeof(bytes) });
        return encode_hex({ bytes, sizeof(bytes) });
    });
    auto nonce_count = ByteString::formatted("{:08x}", ++entry.nonce_count);

    auto a1 = hash_hex(algorithm, ByteString::formatted("{}:{}:{}", entry.username, challenge.realm, entry.password));
    if (uses_session(algorithm))
        a1 = hash_hex(algorithm, ByteString::formatted("{}:{}:{}", a1, challenge.nonce, cnonce));
    auto a2 = hash_hex(algorithm, ByteString::formatted("{}:{}", method, request_uri));
    auto response = challenge.quality_of_protection
        ? hash_hex(algorithm, ByteString::formatted("{}:{}:{}:{}:auth:{}", a1, challenge.nonce, nonce_count, cnonce, a2))
        : hash_hex(algorithm, ByteString::formatted("{}:{}:{}", a1, challenge.nonce, a2));

    StringBuilder value;
    value.appendff("Digest username={}, realm={}, nonce={}, uri={}, algorithm={}, response={}",
        quoted(entry.username), quoted(challenge.realm), quoted(challenge.nonce), quoted(request_uri),
        digest_algorithm_name(algorithm), quoted(response));
    if (challenge.opaque.has_value())
        value.appendff(", opaque={}", quoted(*challenge.opaque));
    if (challenge.quality_of_protection)
        value.appendff(", qop=auth, nc={}, cnonce={}", nonce_count, quoted(cnonce));
    return MUST(value.to_string());
}

}
