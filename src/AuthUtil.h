/* =========================================================================
 *  AuthUtil.h - the pure string/encoding parts of the OAuth flow.
 *
 *  Header-only and free of any Arduino or ESP-IDF dependency, so the host
 *  tests can exercise it directly. Both functions below are the sort of
 *  fiddly buffer code that fails quietly - a base64 alphabet slip or a
 *  substring match in the wrong place produces a plausible-looking string
 *  that Spotify simply rejects - which is exactly why they live here
 *  rather than inline in SpotifyClient.cpp.
 * ====================================================================== */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   /* snprintf */
#include <string.h>

namespace AuthUtil {

/* -------------------------------------------------------------------------
 *  base64url, unpadded - the encoding RFC 7636 requires for both the PKCE
 *  verifier and the challenge.
 *
 *  Self-contained rather than a wrapper over mbedTLS's base64 plus a fix-up
 *  pass: the alphabet differs in two characters and the padding must go, so
 *  writing it directly is both shorter and testable off-device.
 *
 *  Returns the number of characters written, or 0 if `out` is too small.
 *  Zero-length input also returns 0, having written an empty string - the
 *  two are only ambiguous for an input that cannot occur here (a PKCE
 *  verifier is always 64 bytes). `out` needs ceil(n*8/6) + 1 bytes, and
 *  the function refuses rather than truncating.
 * ---------------------------------------------------------------------- */
inline size_t base64UrlEncode(const uint8_t *in, size_t n, char *out,
                              size_t outLen) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  if (!in || !out) return 0;
  const size_t needed = (n * 8 + 5) / 6;
  if (outLen < needed + 1) return 0;

  size_t o = 0, i = 0;
  while (i + 2 < n) {
    const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = tbl[(v >> 6) & 63];
    out[o++] = tbl[v & 63];
    i += 3;
  }
  const size_t rem = n - i;
  if (rem == 1) {
    const uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
  } else if (rem == 2) {
    const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = tbl[(v >> 6) & 63];
  }
  out[o] = '\0';
  return o;
}

/* -------------------------------------------------------------------------
 *  Pull one query parameter out of a pasted redirect URL.
 *
 *  The subtlety: a naive strstr for "code=" also matches inside
 *  "error_code=" or "&device_code=", and silently returns the wrong value.
 *  A match only counts when it sits at the start of the string or directly
 *  after '?' or '&'; otherwise the search continues.
 *
 *  Stops at '&' or '#'. Returns false when the key is absent or its value
 *  is empty.
 * ---------------------------------------------------------------------- */
inline bool queryParam(const char *url, const char *key, char *out,
                       size_t outLen) {
  if (!url || !key || !out || outLen == 0) return false;
  out[0] = '\0';

  char needle[32];
  const int nl = snprintf(needle, sizeof(needle), "%s=", key);
  if (nl <= 0 || (size_t)nl >= sizeof(needle)) return false;

  for (const char *p = strstr(url, needle); p; p = strstr(p + 1, needle)) {
    if (p != url) {
      const char prev = *(p - 1);
      if (prev != '?' && prev != '&') continue;   /* part of a longer key */
    }
    const char *v = p + nl;
    size_t i = 0;
    while (*v && *v != '&' && *v != '#' && i + 1 < outLen) out[i++] = *v++;
    out[i] = '\0';
    return i > 0;
  }
  return false;
}

}  // namespace AuthUtil
