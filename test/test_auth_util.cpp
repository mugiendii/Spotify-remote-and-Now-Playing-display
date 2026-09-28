/* =========================================================================
 *  Host-side tests for AuthUtil: base64url and redirect-URL parsing.
 *  Build and run with tools/run_parser_tests.sh.
 * ====================================================================== */
#include "../src/AuthUtil.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, what)                                             \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      std::cout << "  FAIL: " << (what) << "  [" << #cond << "]\n";   \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

#define CHECK_STR(actual, expected, what)                                  \
  do {                                                                     \
    ++g_checks;                                                            \
    if (std::strcmp((actual), (expected)) != 0) {                          \
      std::cout << "  FAIL: " << (what) << "\n      expected: \""          \
                << (expected) << "\"\n      actual:   \"" << (actual)      \
                << "\"\n";                                                 \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static std::string b64(const std::string &in) {
  char out[512];
  memset(out, 0xAA, sizeof(out));
  const size_t n = AuthUtil::base64UrlEncode(
      reinterpret_cast<const uint8_t *>(in.data()), in.size(), out, sizeof(out));
  /* A 0 return means "nothing written", which is failure for non-empty
   * input but the correct answer for empty input. Distinguish the two
   * rather than reading 0 as failure in both cases. */
  if (n == 0 && !in.empty()) return "<fail>";
  return std::string(out, n);
}

static void testBase64Vectors() {
  std::cout << "base64url - RFC 4648 test vectors, url alphabet, unpadded\n";
  CHECK_STR(b64("").c_str(), "", "empty");
  CHECK_STR(b64("f").c_str(), "Zg", "f");
  CHECK_STR(b64("fo").c_str(), "Zm8", "fo");
  CHECK_STR(b64("foo").c_str(), "Zm9v", "foo");
  CHECK_STR(b64("foob").c_str(), "Zm9vYg", "foob");
  CHECK_STR(b64("fooba").c_str(), "Zm9vYmE", "fooba");
  CHECK_STR(b64("foobar").c_str(), "Zm9vYmFy", "foobar");
}

static void testBase64UrlAlphabet() {
  std::cout << "base64url - '+' and '/' must become '-' and '_'\n";
  /* 0xFB 0xFF encodes to "+/8" in standard base64 -> "-_8" in base64url.
   * This is the substitution Spotify rejects if you get it wrong. */
  const uint8_t raw[] = {0xFB, 0xFF, 0xFF};
  char out[16];
  AuthUtil::base64UrlEncode(raw, sizeof(raw), out, sizeof(out));
  CHECK(strchr(out, '+') == nullptr, "no '+' in output");
  CHECK(strchr(out, '/') == nullptr, "no '/' in output");
  CHECK(strchr(out, '=') == nullptr, "no padding in output");
  CHECK_STR(out, "-___", "url alphabet applied");
}

static void testBase64Lengths() {
  std::cout << "base64url - PKCE lengths and buffer guards\n";
  uint8_t raw[64];
  memset(raw, 0xA5, sizeof(raw));
  char out[128];

  /* 64 random bytes is what buildAuthUrl() uses; RFC 7636 requires the
   * verifier to be 43..128 characters. */
  size_t n = AuthUtil::base64UrlEncode(raw, 64, out, sizeof(out));
  CHECK(n == 86, "64 bytes -> 86 chars");
  CHECK(n >= 43 && n <= 128, "verifier length inside RFC 7636 range");

  /* A SHA-256 digest is 32 bytes -> 43 characters. */
  n = AuthUtil::base64UrlEncode(raw, 32, out, sizeof(out));
  CHECK(n == 43, "32 bytes -> 43 chars (the challenge)");

  /* Must refuse rather than overflow. */
  char tiny[8];
  CHECK(AuthUtil::base64UrlEncode(raw, 64, tiny, sizeof(tiny)) == 0,
        "refuses a buffer that is too small");
  /* Exactly big enough must succeed. */
  char exact[44];
  CHECK(AuthUtil::base64UrlEncode(raw, 32, exact, sizeof(exact)) == 43,
        "accepts an exactly-sized buffer");
  char oneShort[43];
  CHECK(AuthUtil::base64UrlEncode(raw, 32, oneShort, sizeof(oneShort)) == 0,
        "refuses when there is no room for the NUL");

  /* Every character must be in the unreserved set. */
  AuthUtil::base64UrlEncode(raw, 64, out, sizeof(out));
  bool clean = true;
  for (char *p = out; *p; ++p) {
    if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) clean = false;
  }
  CHECK(clean, "output is URL-safe throughout");
}

static void testQueryParam() {
  std::cout << "redirect URL parsing\n";
  char v[256];

  CHECK(AuthUtil::queryParam("http://127.0.0.1:8888/callback?code=ABC123",
                             "code", v, sizeof(v)),
        "plain code found");
  CHECK_STR(v, "ABC123", "plain code value");

  CHECK(AuthUtil::queryParam("http://127.0.0.1:8888/callback?state=xy&code=ABC",
                             "code", v, sizeof(v)),
        "code after another param");
  CHECK_STR(v, "ABC", "code value after state");

  CHECK(AuthUtil::queryParam("http://h/cb?code=ABC&state=xy", "code", v, sizeof(v)),
        "code before another param");
  CHECK_STR(v, "ABC", "value stops at '&'");

  CHECK(AuthUtil::queryParam("http://h/cb?code=ABC#frag", "code", v, sizeof(v)),
        "fragment present");
  CHECK_STR(v, "ABC", "value stops at '#'");

  /* The whole reason this function is not a one-line strstr. */
  CHECK(!AuthUtil::queryParam("http://h/cb?error_code=99", "code", v, sizeof(v)),
        "must NOT match inside 'error_code'");
  CHECK(AuthUtil::queryParam("http://h/cb?error_code=99&code=REAL",
                             "code", v, sizeof(v)),
        "finds the real code after a decoy key");
  CHECK_STR(v, "REAL", "decoy key skipped, real value taken");

  CHECK(AuthUtil::queryParam("http://h/cb?device_code=1&code=X2",
                             "code", v, sizeof(v)),
        "decoy 'device_code' skipped");
  CHECK_STR(v, "X2", "value after decoy");

  CHECK(AuthUtil::queryParam("http://h/cb?error=access_denied&state=q",
                             "error", v, sizeof(v)),
        "error param found");
  CHECK_STR(v, "access_denied", "error value");

  CHECK(!AuthUtil::queryParam("http://h/cb?state=xy", "code", v, sizeof(v)),
        "absent key");
  CHECK(!AuthUtil::queryParam("http://h/cb?code=", "code", v, sizeof(v)),
        "empty value is not a hit");
  CHECK(!AuthUtil::queryParam("", "code", v, sizeof(v)), "empty url");
  CHECK(!AuthUtil::queryParam(nullptr, "code", v, sizeof(v)), "null url");

  /* A bare query string, with no scheme or host. */
  CHECK(AuthUtil::queryParam("code=BARE", "code", v, sizeof(v)),
        "match at the very start of the string");
  CHECK_STR(v, "BARE", "bare value");

  /* Truncation must stay in bounds and stay terminated. */
  char small[5];
  AuthUtil::queryParam("http://h/cb?code=ABCDEFGHIJ", "code", small, sizeof(small));
  CHECK(strlen(small) == 4, "value truncated to fit");
  CHECK_STR(small, "ABCD", "truncated prefix kept");
}

int main() {
  std::cout << "AuthUtil host tests\n\n";
  testBase64Vectors();
  testBase64UrlAlphabet();
  testBase64Lengths();
  testQueryParam();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
            << " checks passed\n";
  if (g_failures) std::cout << g_failures << " FAILURE(S)\n";
  return g_failures ? 1 : 0;
}
