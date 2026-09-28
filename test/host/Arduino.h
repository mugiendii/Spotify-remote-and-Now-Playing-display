/* =========================================================================
 *  test/host/Arduino.h - a stand-in for the real Arduino.h, holding the
 *  handful of symbols PlayerParser and AppState actually touch. It lets
 *  the parser be compiled and tested on a PC.
 *
 *  Reached only because tools/run_parser_tests.sh puts test/host on the
 *  include path ahead of everything else; the firmware build never sees
 *  this file.
 * ====================================================================== */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/* A fixed clock keeps assertions on progress anchoring deterministic. */
inline uint32_t millis() { return 100000; }

#ifndef strlcpy
inline size_t shim_strlcpy(char *dst, const char *src, size_t n) {
  const size_t len = strlen(src);
  if (n) {
    const size_t c = (len >= n) ? n - 1 : len;
    memcpy(dst, src, c);
    dst[c] = '\0';
  }
  return len;
}
inline size_t shim_strlcat(char *dst, const char *src, size_t n) {
  const size_t dl = strnlen(dst, n);
  if (dl == n) return n + strlen(src);
  return dl + shim_strlcpy(dst + dl, src, n - dl);
}
#define strlcpy shim_strlcpy
#define strlcat shim_strlcat
#endif
