/* =========================================================================
 *  SecretsGate.h - the only place that includes secrets.h.
 *
 *  Exists so a missing credentials file produces one clear instruction
 *  instead of a cascade of "WIFI_SSID was not declared" errors.
 * ====================================================================== */
#pragma once

#if defined(__has_include)
#  if !__has_include("secrets.h")
#    error "include/secrets.h not found. Run:  cp include/secrets.example.h include/secrets.h  and fill it in (see README)."
#  endif
#endif

#include "secrets.h"
