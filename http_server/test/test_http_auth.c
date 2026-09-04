/*
 * Host test for Basic-auth credential handling.
 *
 * A credential check is the last place to discover a subtle bug on hardware, and
 * the two properties that matter here -- that the encoding is correct, and that
 * the comparison does not leak how far it matched -- are invisible from outside.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "http_auth.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

static void encodes(const char *in, const char *want)
{
    char out[128];
    bool ok = http_auth_base64(in, strlen(in), out, sizeof(out));
    char label[200];
    snprintf(label, sizeof(label), "base64('%s') == '%s'", in, want);
    expect(ok && strcmp(out, want) == 0, label);
    if (!ok || strcmp(out, want) != 0) printf("       got '%s'\n", out);
}

static void test_base64(void)
{
    /* The three padding cases; RFC 4648 test vectors. */
    encodes("",       "");
    encodes("f",      "Zg==");
    encodes("fo",     "Zm8=");
    encodes("foo",    "Zm9v");
    encodes("foob",   "Zm9vYg==");
    encodes("fooba",  "Zm9vYmE=");
    encodes("foobar", "Zm9vYmFy");

    /* The actual use: "user:pass". */
    encodes("admin:secret", "YWRtaW46c2VjcmV0");

    /* High bytes must not be sign-extended. */
    char high[] = {(char)0xff, (char)0xfe, (char)0xfd, '\0'};
    char out[16];
    expect(http_auth_base64(high, 3, out, sizeof(out)) && strcmp(out, "//79") == 0,
           "bytes above 0x7f encode without sign extension");

    char small[4];
    expect(!http_auth_base64("foobar", 6, small, sizeof(small)),
           "encoding refuses to truncate");
}

static void test_expected_header(void)
{
    char header[128];
    expect(http_auth_expected_header("admin", "secret", header, sizeof(header)),
           "expected header builds");
    expect(strcmp(header, "Basic YWRtaW46c2VjcmV0") == 0, "and has the right value");

    char small[8];
    expect(!http_auth_expected_header("admin", "secret", small, sizeof(small)),
           "header building refuses to truncate");
}

static void test_matching(void)
{
    char expected[128];
    http_auth_expected_header("admin", "secret", expected, sizeof(expected));

    expect(http_auth_header_matches("Basic YWRtaW46c2VjcmV0", expected),
           "the correct header matches");
    expect(!http_auth_header_matches("Basic YWRtaW46c2VjcmV1", expected),
           "a wrong password does not match");
    expect(!http_auth_header_matches("Basic YWRtaW4xOnNlY3JldA==", expected),
           "a wrong username does not match");

    /* Length differences must not short-circuit into a different code path. */
    expect(!http_auth_header_matches("", expected), "an empty header does not match");
    expect(!http_auth_header_matches("Basic", expected), "a truncated header does not match");
    expect(!http_auth_header_matches("Basic YWRtaW46c2VjcmV0X", expected),
           "a longer header does not match");

    /* A prefix of the correct value must not match: this is the case a
     * short-circuiting comparison gets wrong when it stops at the first
     * difference and the shorter string has run out. */
    expect(!http_auth_header_matches("Basic YWRtaW46c2Vjcm", expected),
           "a prefix of the correct header does not match");

    expect(!http_auth_header_matches(NULL, expected), "NULL header does not match");
    expect(!http_auth_header_matches("Basic x", NULL), "NULL expected does not match");

    /* Different scheme entirely. */
    expect(!http_auth_header_matches("Bearer YWRtaW46c2VjcmV0", expected),
           "a different auth scheme does not match");

    /* An empty password is still a specific credential, not a wildcard. */
    char empty_pw[128];
    http_auth_expected_header("admin", "", empty_pw, sizeof(empty_pw));
    expect(http_auth_header_matches("Basic YWRtaW46", empty_pw),
           "an empty password matches only its own encoding");
    expect(!http_auth_header_matches("Basic YWRtaW46eA==", empty_pw),
           "and not some other password");
}

int main(void)
{
    test_base64();
    test_expected_header();
    test_matching();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
