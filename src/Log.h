/* =========================================================================
 *  Log.h - level-gated Serial logging.
 *
 *  Calls below the configured LOG_LEVEL compile to nothing, so a release
 *  build carries neither the format strings nor the call sites.
 *
 *  SECURITY: never pass a Wi-Fi password, an access token or the refresh
 *  token to these macros. Use redact() for anything credential-shaped - it
 *  prints a length and a fingerprint, which is enough to tell "the token
 *  changed" or "the token is empty" apart without disclosing the value.
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include "config.h"

#ifndef SPOTIFY_TFT_CONFIG_H_OK
#error "include/config.h not found (or another library's config.h was picked up first). Run:  cp include/config.example.h include/config.h"
#endif

#define LOG_PRINT_(tag, fmt, ...) \
  Serial.printf("[%8lu] " tag " " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

#if LOG_LEVEL >= 1
  #define LOG_E(fmt, ...) LOG_PRINT_("E", fmt, ##__VA_ARGS__)
#else
  #define LOG_E(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 2
  #define LOG_I(fmt, ...) LOG_PRINT_("I", fmt, ##__VA_ARGS__)
#else
  #define LOG_I(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 3
  #define LOG_D(fmt, ...) LOG_PRINT_("D", fmt, ##__VA_ARGS__)
#else
  #define LOG_D(fmt, ...) ((void)0)
#endif

/* -------------------------------------------------------------------------
 *  redact() - render a credential as "<len=123 fp=a1b2>" .
 *
 *  The fingerprint is a cheap FNV-1a hash folded to 16 bits. It is NOT a
 *  cryptographic digest and is not meant to be: its only job is to let you
 *  see at a glance whether a token changed between two log lines. 16 bits
 *  of a non-reversible hash of a 130-character token discloses nothing
 *  useful about the token itself.
 *
 *  Returns a pointer to a static buffer - one call per printf argument list.
 * ---------------------------------------------------------------------- */
inline const char *redact(const char *secret) {
  static char buf[32];
  if (!secret || !*secret) {
    snprintf(buf, sizeof(buf), "<empty>");
    return buf;
  }
  uint32_t h = 2166136261u;
  size_t n = 0;
  for (const char *p = secret; *p; ++p, ++n) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  snprintf(buf, sizeof(buf), "<len=%u fp=%04x>",
           (unsigned)n, (unsigned)((h ^ (h >> 16)) & 0xFFFF));
  return buf;
}
