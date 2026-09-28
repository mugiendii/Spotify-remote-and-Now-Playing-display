#include "AlbumArt.h"

#include <TJpg_Decoder.h>

#include "DisplayManager.h"
#include "Log.h"
#include "SpotifyCerts.h"
#include "UiLayout.h"
#include "config.h"

namespace {

/* The decoder callback is a plain C-style function pointer, so the target
 * panel has to reach it through file scope. Only one decode ever runs at a
 * time (it is synchronous, on the loop task), so a single pointer is safe. */
TFT_eSPI *g_panel = nullptr;

/* Push one decoded MCU block. Coordinates are relative to the viewport that
 * decodeAndDraw() installs around the art box, so TFT_eSPI clips anything
 * that would spill outside the frame - no arithmetic here can corrupt the
 * rest of the screen. */
bool jpegBlockToPanel(int16_t x, int16_t y, uint16_t w, uint16_t h,
                      uint16_t *bitmap) {
  if (!g_panel) return false;
  if (y >= Layout::ART_H) return false;     /* past the box: stop decoding */
  g_panel->pushImage(x, y, w, h, bitmap);
  return true;
}

}  // namespace

/* =========================================================================
 *  Artwork selection
 * ====================================================================== */
void chooseArtUrl(JsonVariantConst images, char *out, size_t outLen) {
  out[0] = '\0';
  JsonArrayConst arr = images.as<JsonArrayConst>();
  if (arr.isNull()) return;

  /* Spotify offers roughly 640 / 300 / 64 px squares. We want the smallest
   * one that still covers the box, because every extra pixel is download
   * time and heap we do not need: at a 160 px box that is the 300 px image
   * (~25 KB) rather than the 640 px one (~90 KB).
   *
   * Two passes rather than assuming the array is sorted - it usually is
   * largest-first, but nothing in the API documentation promises that. */
  const char *bestUrl = nullptr;
  int bestW = INT32_MAX;
  const char *fallbackUrl = nullptr;
  int fallbackW = -1;

  for (JsonObjectConst img : arr) {
    const char *u = img["url"] | "";
    if (!*u) continue;
    const int w = img["width"] | 0;

    if (w >= ART_BOX_PX && w < bestW) {      /* covers the box, smallest so far */
      bestW   = w;
      bestUrl = u;
    }
    if (w > fallbackW) {                     /* largest, in case none covers it */
      fallbackW   = w;
      fallbackUrl = u;
    }
  }

  const char *chosen = bestUrl ? bestUrl : fallbackUrl;
  if (chosen) {
    strlcpy(out, chosen, outLen);
    LOG_D("art: chose %dpx image", bestUrl ? bestW : fallbackW);
  }
}

/* =========================================================================
 *  Lifecycle
 * ====================================================================== */
void AlbumArt::begin(DisplayManager *display) {
  _display = display;

  /* One allocation, at boot, while the heap is still whole. Doing this
   * lazily would mean asking for 48 KB contiguous bytes after mbedTLS has
   * had its way with the heap - exactly when it is most likely to fail. */
  _cap = ART_MAX_BYTES;
  _buf = (uint8_t *)malloc(_cap);
  if (!_buf) {
    LOG_E("art: could not reserve %u byte buffer - artwork disabled",
          (unsigned)_cap);
    _cap = 0;
  } else {
    LOG_I("art: %u byte buffer reserved, free heap %u",
          (unsigned)_cap, (unsigned)ESP.getFreeHeap());
  }

  TJpgDec.setSwapBytes(true);        /* JPEG byte order -> TFT_eSPI order */
  TJpgDec.setCallback(jpegBlockToPanel);
}

/* How long to leave a failed cover alone. Long enough that a dead URL
 * costs nothing, short enough that a transient Wi-Fi blip still recovers
 * while the same track is playing. */
static constexpr uint32_t ART_RETRY_MS = 30000;

bool AlbumArt::needsFetch(const char *cacheKey, const char *url) const {
  if (!url || !*url || !_buf) return false;
  if (_state != State::Idle)  return false;

  const char *key = (cacheKey && *cacheKey) ? cacheKey : url;
  if (strncmp(_cacheKey, key, sizeof(_cacheKey) - 1) == 0) return false;

  if (_failedKey[0] && strncmp(_failedKey, key, sizeof(_failedKey) - 1) == 0 &&
      (int32_t)(millis() - _retryAt) < 0) {
    return false;                       /* still serving the back-off */
  }
  return true;
}

void AlbumArt::request(const char *cacheKey, const char *url) {
  /* Nothing to show. */
  if (!url || !*url) {
    if (_cacheKey[0] || _state != State::Idle) {
      _cacheKey[0] = '\0';
      _state = State::Idle;
      _http.end();
      _tls.stop();
      if (_display) _display->artDrawPlaceholder(false);
    }
    return;
  }

  const char *key = (cacheKey && *cacheKey) ? cacheKey : url;

  /* Already on the glass - this is the cache hit that keeps skipping
   * through an album from re-fetching one cover over and over. */
  if (strncmp(_cacheKey, key, sizeof(_cacheKey) - 1) == 0) return;

  /* Recently failed: hold off rather than hammering a URL that is not
   * going to start working within the next few milliseconds. */
  if (_failedKey[0] && strncmp(_failedKey, key, sizeof(_failedKey) - 1) == 0 &&
      (int32_t)(millis() - _retryAt) < 0) {
    return;
  }

  /* Already fetching this one. */
  if (_state != State::Idle &&
      strncmp(_pendingKey, key, sizeof(_pendingKey) - 1) == 0) {
    return;
  }

  /* A different track arrived mid-download: abandon the old transfer. */
  if (_state != State::Idle) {
    LOG_D("art: superseded, abandoning in-flight download");
    _http.end();
    _tls.stop();
  }

  if (!_buf) {
    if (_display) _display->artDrawPlaceholder(false);
    return;
  }

  strlcpy(_pendingKey, key, sizeof(_pendingKey));
  strlcpy(_url, url, sizeof(_url));
  _len      = 0;
  _expected = -1;
  _state    = State::Connect;
  if (_display) _display->artDrawPlaceholder(true);
}

void AlbumArt::abort() {
  if (_state == State::Idle) return;
  LOG_D("art: aborted (%u bytes in)", (unsigned)_len);
  _http.end();
  _tls.stop();
  _state = State::Idle;
  _len   = 0;
  /* Not a failure: no back-off, and the cache key is left alone so the
   * cover is fetched again when the art box is next on screen. */
  _pendingKey[0] = '\0';
}

void AlbumArt::fail(const char *why) {
  LOG_E("art: %s", why);
  _http.end();
  _tls.stop();
  _state       = State::Idle;
  _cacheKey[0] = '\0';
  _len         = 0;
  strlcpy(_failedKey, _pendingKey, sizeof(_failedKey));
  _retryAt = millis() + ART_RETRY_MS;
  if (_display) _display->artDrawPlaceholder(false);
}

void AlbumArt::finish() {
  _http.end();
  _tls.stop();
  _state = State::Idle;
  strlcpy(_cacheKey, _pendingKey, sizeof(_cacheKey));
  _failedKey[0] = '\0';
  _len = 0;
}

/* =========================================================================
 *  Decode
 * ====================================================================== */
void AlbumArt::decodeAndDraw() {
  uint16_t jw = 0, jh = 0;
  if (TJpgDec.getJpgSize(&jw, &jh, _buf, _len) != JDR_OK || jw == 0 || jh == 0) {
    fail("not a decodable JPEG");
    return;
  }

  /* Choose the largest whole-image reduction that still fits the box. A
   * baseline JPEG decoder gets 1/2, 1/4 and 1/8 essentially free because
   * they fall out of the IDCT, so scaling here costs nothing and saves
   * both time and the pixels we would otherwise discard. */
  uint8_t scale = 1;
  while (scale < 8 &&
         ((jw / scale) > Layout::ART_W || (jh / scale) > Layout::ART_H)) {
    scale *= 2;
  }
  const uint16_t dw = jw / scale;
  const uint16_t dh = jh / scale;

  if (dw > Layout::ART_W || dh > Layout::ART_H) {
    /* Even 1/8 is too big - the source must be enormous. Better a
     * placeholder than a cover bleeding over the track title. */
    fail("image too large even at 1/8 scale");
    return;
  }

  /* Centre inside the box. Scaling both axes by the same factor is what
   * preserves the aspect ratio; centring is what stops a non-square image
   * (some podcast art) sitting awkwardly in the corner. */
  const int16_t ox = (Layout::ART_W - dw) / 2;
  const int16_t oy = (Layout::ART_H - dh) / 2;

  TFT_eSPI &tft = _display->tft();
  g_panel = &tft;

  _display->artClearBox();

  /* Everything the decoder draws is clipped to the art box, and block
   * coordinates become box-relative. */
  tft.setViewport(Layout::ART_X, Layout::ART_Y, Layout::ART_W, Layout::ART_H);
  TJpgDec.setJpgScale(scale);
  const JRESULT r = TJpgDec.drawJpg(ox, oy, _buf, _len);
  tft.resetViewport();
  g_panel = nullptr;

  if (r != JDR_OK) {
    fail("JPEG decode failed mid-image");
    return;
  }

  _display->artFrame();
  _display->artMarkPainted();
  LOG_I("art: %ux%u -> %ux%u (1/%u), %u bytes",
        (unsigned)jw, (unsigned)jh, (unsigned)dw, (unsigned)dh,
        (unsigned)scale, (unsigned)_len);
  finish();
}

/* =========================================================================
 *  Incremental download
 * ====================================================================== */
void AlbumArt::service() {
  switch (_state) {
    case State::Idle:
      return;

    case State::Connect: {
      /* The TLS handshake is the one unavoidably slow step (~1 s of RSA on
       * this chip). It happens once per album, not per frame. */
#if TLS_VERIFY_CERTIFICATES
      _tls.setCACert(CA_SPOTIFY_IMAGES);
#else
      _tls.setInsecure();
#endif
      _tls.setTimeout(ART_HTTP_TIMEOUT_MS / 1000);

      _http.setTimeout(ART_HTTP_TIMEOUT_MS);
      _http.setConnectTimeout(ART_HTTP_TIMEOUT_MS);
      _http.setReuse(false);
      _http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

      if (!_http.begin(_tls, _url)) {
        fail("artwork begin() failed");
        return;
      }
      const int code = _http.GET();
      if (code != HTTP_CODE_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "artwork HTTP %d", code);
        fail(msg);
        return;
      }

      _expected = _http.getSize();       /* -1 when chunked */
      if (_expected > (int)_cap) {
        char msg[80];
        snprintf(msg, sizeof(msg), "artwork %d bytes exceeds %u byte buffer",
                 _expected, (unsigned)_cap);
        fail(msg);
        return;
      }
      _len      = 0;
      _deadline = millis() + ART_HTTP_TIMEOUT_MS;
      _state    = State::Download;
      return;
    }

    case State::Download: {
      WiFiClient *stream = _http.getStreamPtr();
      if (!stream) {
        fail("artwork stream vanished");
        return;
      }

      /* Bounded work per loop iteration. This is the whole reason touch
       * stays alive during a download: we take one TCP segment and hand
       * control straight back to the main loop. */
      size_t budget = ART_CHUNK_BYTES;
      while (budget > 0) {
        const size_t avail = stream->available();
        if (avail == 0) break;
        if (_len >= _cap) {
          fail("artwork larger than the buffer");
          return;
        }
        size_t want = avail;
        if (want > budget)        want = budget;
        if (want > _cap - _len)   want = _cap - _len;
        const int n = stream->read(_buf + _len, want);
        if (n <= 0) break;
        _len   += (size_t)n;
        budget -= (size_t)n;
      }

      const bool complete =
          (_expected > 0 && _len >= (size_t)_expected) ||
          (_expected < 0 && !stream->connected() && stream->available() == 0);

      if (complete) {
        if (_len == 0) {
          fail("artwork download was empty");
          return;
        }
        _state = State::Decode;
        return;
      }

      if ((int32_t)(millis() - _deadline) >= 0) {
        fail("artwork download timed out");
        return;
      }
      /* Peer closed early with a Content-Length still outstanding. */
      if (!stream->connected() && stream->available() == 0) {
        fail("artwork connection closed early");
        return;
      }
      return;
    }

    case State::Decode:
      decodeAndDraw();
      return;
  }
}
