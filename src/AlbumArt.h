/* =========================================================================
 *  AlbumArt.h - artwork selection, incremental download, JPEG decode and
 *  caching.
 *
 *  MEMORY STRATEGY (the part that decides whether this fits on an ESP32)
 *  --------------------------------------------------------------------
 *  A 160x160 RGB565 bitmap is 51 KB and a full-resolution 640x640 one is
 *  820 KB - far past what a PSRAM-less ESP32-WROOM-32 can hold while
 *  mbedTLS is also live. So the decoded image is never held in RAM at all:
 *
 *    1. Pick the SMALLEST image Spotify offers that still covers the art
 *       box, so we download ~20 KB instead of ~90 KB.
 *    2. Buffer only the compressed JPEG - one fixed allocation made at
 *       boot, never grown, never freed, so it cannot fragment the heap.
 *    3. Decode with TJpg_Decoder, which emits 16x16 MCU blocks through a
 *       callback. Each block is pushed straight to the panel and dropped.
 *       Peak decode footprint is a few hundred bytes of working state.
 *    4. Use the JPEG scale factor (1, 1/2, 1/4, 1/8 - the only ratios a
 *       baseline decoder can do for free) to land just under the box, so
 *       the decoder never even produces pixels we would throw away.
 *
 *  RESPONSIVENESS
 *  --------------
 *  The download is spread over many loop iterations, a TCP segment at a
 *  time, so touch keeps being polled while artwork is in flight. Only the
 *  decode itself is a single blocking burst, and that is ~100 ms once per
 *  album.
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "AppState.h"

class DisplayManager;

/* -------------------------------------------------------------------------
 *  Pick the best artwork URL out of a Spotify "images" array.
 *
 *  Lives here rather than in SpotifyClient because "best" is a function of
 *  the panel and the decoder, not of the API.
 * ---------------------------------------------------------------------- */
void chooseArtUrl(JsonVariantConst images, char *out, size_t outLen);

class AlbumArt {
 public:
  void begin(DisplayManager *display);

  /* Ask for the artwork identified by cacheKey. A repeat request for the
   * key already on screen is free - this is what stops the device
   * re-downloading a cover every time you skip within an album. */
  void request(const char *cacheKey, const char *url);

  /* Advance the download. Call every loop iteration. */
  void service();

  /* Would request() actually start a transfer for this key? Callers use
   * this to free other network resources first - without it there is no
   * way to tell a real fetch from a cache hit until the socket is already
   * open. */
  bool needsFetch(const char *cacheKey, const char *url) const;

  bool busy() const { return _state != State::Idle; }

  /* Drop the cache so the next request re-downloads (e.g. after the art
   * box was overdrawn by another screen). */
  void invalidate() { _cacheKey[0] = '\0'; }

  /* Stop an in-flight transfer without drawing anything.
   *
   * Needed whenever the art box stops being visible: a decode that
   * completes while another screen is up would push a 160x160 cover
   * straight over that screen, because the decoder writes to fixed panel
   * coordinates and knows nothing about screen routing. */
  void abort();

 private:
  enum class State : uint8_t { Idle, Connect, Download, Decode };

  void fail(const char *why);
  void finish();
  void decodeAndDraw();

  DisplayManager *_display = nullptr;

  WiFiClientSecure _tls;
  HTTPClient       _http;

  uint8_t *_buf      = nullptr;     /* one permanent allocation           */
  size_t   _cap      = 0;
  size_t   _len      = 0;
  int      _expected = -1;          /* Content-Length, -1 when chunked    */

  State    _state    = State::Idle;
  uint32_t _deadline = 0;

  char _cacheKey[ID_LEN] = {0};     /* what is currently on the glass     */
  char _pendingKey[ID_LEN] = {0};
  char _url[URL_LEN] = {0};

  /* Failure memory. Without it a cover that 404s is retried on every
   * single loop iteration - thousands of requests a minute at the CDN,
   * and a device that never does anything else. */
  char     _failedKey[ID_LEN] = {0};
  uint32_t _retryAt = 0;
};
