/*
 * Copyright (c) 2026, the Photon developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/HTTPAuthentication.h>

using namespace Web::Fetch::Fetching;

// https://www.rfc-editor.org/rfc/rfc7616#section-3.9.1
static constexpr auto rfc_challenges = R"(Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm=SHA-256, nonce="7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v", opaque="FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS", Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm=MD5, nonce="7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v", opaque="FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS")"sv;
static constexpr auto rfc_client_nonce = "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"sv;

static String rfc_authorization(AuthenticationChallenge challenge)
{
    AuthenticationEntry entry { "Mufasa"_string, "Circle of Life"_string, move(challenge), 0 };
    auto url = URL::Parser::basic_parse("http://www.example.org/dir/index.html"sv);
    return authorization_value_for(entry, "GET"sv, *url, ByteString(rfc_client_nonce));
}

TEST_CASE(prefers_sha256_digest)
{
    auto challenge = choose_authentication_challenge(rfc_challenges);
    VERIFY(challenge.has_value());
    EXPECT_EQ(challenge->scheme, AuthenticationChallenge::Scheme::Digest);
    EXPECT_EQ(challenge->algorithm, AuthenticationChallenge::DigestAlgorithm::SHA256);
    EXPECT_EQ(challenge->realm, "http-auth@example.org"sv);
    EXPECT(challenge->quality_of_protection);
    EXPECT_EQ(challenge->opaque, "FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS"sv);
}

TEST_CASE(answers_rfc_sha256_example)
{
    auto value = rfc_authorization(*choose_authentication_challenge(rfc_challenges));
    EXPECT(value.contains("response=\"753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1\""sv));
    EXPECT(value.contains("uri=\"/dir/index.html\""sv));
    EXPECT(value.contains("qop=auth, nc=00000001"sv));
}

TEST_CASE(answers_rfc_md5_example)
{
    auto challenge = choose_authentication_challenge(rfc_challenges.substring_view(rfc_challenges.find("Digest realm"sv, 1).value()));
    VERIFY(challenge.has_value());
    EXPECT_EQ(challenge->algorithm, AuthenticationChallenge::DigestAlgorithm::MD5);
    auto value = rfc_authorization(challenge.release_value());
    EXPECT(value.contains("response=\"8ca523f5e9506fed4657c9700eebdbec\""sv));
}

TEST_CASE(falls_back_to_basic)
{
    auto challenge = choose_authentication_challenge(R"(Basic realm="Router", Digest realm="x", qop="auth-int", nonce="n")"sv);
    VERIFY(challenge.has_value());
    EXPECT_EQ(challenge->scheme, AuthenticationChallenge::Scheme::Basic);
    EXPECT_EQ(challenge->realm, "Router"sv);
    EXPECT_EQ(basic_authorization_value("Aladdin"sv, "open sesame"sv), "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="sv);
}

TEST_CASE(reads_stale_nonce)
{
    auto challenge = choose_authentication_challenge(R"(Digest realm="r", nonce="fresh", stale=TRUE)"sv);
    VERIFY(challenge.has_value());
    EXPECT(challenge->stale);
    EXPECT(!challenge->quality_of_protection);
}
