#include "SpotifyClient.h"

#include <esp_system.h>
#include <mbedtls/sha256.h>

#include "AlbumArt.h"      /* chooseArtUrl() - artwork choice is a
                            display concern, not an API one */
#include "AuthUtil.h"
#include "Log.h"
#include "PlayerParser.h"
#include "SpotifyCerts.h"
#include "config.h"
#include "SecretsGate.h"

namespace {

constexpr char TOKEN_URL[]  = "https://accounts.spotify.com/api/token";
constexpr char PLAYER_URL[] =
    "https://api.spotify.com/v1/me/player?additional_types=track,episode";
constexpr char API_BASE[]   = "https://api.spotify.com/v1/me/player";

/* NVS namespace and key for the rotating refresh token. */
constexpr char DEVICES_URL[] = "https://api.spotify.com/v1/me/player/devices";
constexpr char AUTHORIZE_URL[] = "https://accounts.spotify.com/authorize";

/* Exactly the scopes the display and the controls need, and no more. */
constexpr char SCOPES[] =
    "user-read-currently-playing user-read-playback-state "
    "user-modify-playback-state";

constexpr char NVS_NS[]     = "spotify";
constexpr char NVS_KEY[]    = "refresh";
constexpr char NVS_CLIENT[] = "clientid";

/* Percent-encode for application/x-www-form-urlencoded. Refresh tokens are
 * base64url and client IDs are hex, so in practice little needs escaping -
 * but building a request body by blind concatenation is how injection bugs
 * start, and the cost here is a few hundred bytes of code.
 *
 * Returns false if the result did not fit, having written an empty string.
 * It deliberately does NOT truncate: a half-written redirect_uri is still a
 * perfectly well-formed parameter, so Spotify would reject the exchange
 * with a mismatch error and nothing would point at the real cause. Failing
 * loudly here turns a silent dead end into one log line. */
bool formEncode(const char *in, char *out, size_t outLen) {
  static const char hex[] = "0123456789ABCDEF";
  if (!in || !out || outLen == 0) return false;

  size_t o = 0;
  for (const char *p = in; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    const size_t need = (isalnum(c) || c == '-' || c == '_' || c == '.' ||
                         c == '~')
                            ? 1u
                            : 3u;
    if (o + need + 1 > outLen) {      /* +1 for the terminator */
      out[0] = '\0';
      return false;
    }
    if (need == 1) {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0x0F];
    }
  }
  out[o] = '\0';
  return true;
}

/* Worst case every character percent-encodes to three bytes, so sizing the
 * encode buffers off the source length makes truncation impossible however
 * long a redirect URI you configure. */
constexpr size_t encCap(size_t rawLen) { return rawLen * 3 + 1; }

constexpr size_t REDIRECT_ENC_CAP = encCap(sizeof(SPOTIFY_REDIRECT_URI) - 1);

}  // namespace

/* =========================================================================
 *  Lifecycle
 * ====================================================================== */
void SpotifyClient::begin(PlaybackState *state, AppStatus *status) {
  _st     = state;
  _status = status;

  /* Spotify permits plain HTTP only for loopback redirect URIs. Getting
   * this wrong shows up much later as a bare INVALID_CLIENT in the browser,
   * with nothing to connect it back to config.h - so say it at boot. */
  {
    const char *r = SPOTIFY_REDIRECT_URI;
    const bool https = (strncmp(r, "https://", 8) == 0);
    const bool loopback = (strncmp(r, "http://127.0.0.1", 16) == 0) ||
                          (strncmp(r, "http://[::1]", 12) == 0);
    if (!https && !loopback) {
      LOG_E("config: SPOTIFY_REDIRECT_URI \"%s\" will be rejected - Spotify "
            "requires https:// unless the host is 127.0.0.1 or [::1]", r);
    } else {
      LOG_I("auth: redirect URI %s", r);
    }
  }

  loadClientId();
  loadRefreshToken();

  /* Catch an unedited template before spending a TLS handshake discovering
   * the same thing from Spotify. Both placeholders are checked: a valid
   * refresh token with a placeholder client ID fails just as hard, and the
   * error Spotify returns for it is far less obvious. */
  if (!_clientId[0]) {
    LOG_E("spotify: no client ID - sign in from the setup page");
    _authDead    = true;
    _status->api = ApiState::NoToken;
    _status->setMessage("Sign in: open the setup page", 3600000UL);
    return;
  }

  if (!_refreshToken[0]) {
    LOG_E("spotify: no refresh token in NVS or secrets.h");
    _authDead      = true;
    _status->api   = ApiState::NoToken;
    _status->setMessage("No refresh token - see README", 3600000UL);
  }
  _nextPollAt = millis();
}

/* -------------------------------------------------------------------------
 *  Refresh-token persistence.
 *
 *  NVS wins over secrets.h, because NVS holds the most recently rotated
 *  value while secrets.h only ever holds the original seed.
 * ---------------------------------------------------------------------- */
/* The client ID is app-level rather than user-level, but keeping it in NVS
 * means the whole device can be re-pointed at a different Spotify app from
 * the setup page with no reflash. secrets.h is only the seed. */
void SpotifyClient::loadClientId() {
  memset(_clientId, 0, sizeof(_clientId));
  _prefs.begin(NVS_NS, /*readOnly=*/true);
  _prefs.getString(NVS_CLIENT, _clientId, sizeof(_clientId));
  _prefs.end();

  if (_clientId[0]) {
    LOG_I("spotify: client ID from NVS (%s)", _clientId);
    return;
  }
  strlcpy(_clientId, SPOTIFY_CLIENT_ID, sizeof(_clientId));
  if (strstr(_clientId, "your-32-char") != nullptr) {
    _clientId[0] = '\0';          /* the untouched placeholder */
    return;
  }
  if (_clientId[0]) setClientId(_clientId);
}

void SpotifyClient::setClientId(const char *id) {
  if (!id) return;
  strlcpy(_clientId, id, sizeof(_clientId));
  _prefs.begin(NVS_NS, /*readOnly=*/false);
  _prefs.putString(NVS_CLIENT, _clientId);
  _prefs.end();
  LOG_I("spotify: client ID stored (%s)", _clientId);
}

void SpotifyClient::loadRefreshToken() {
  memset(_refreshToken, 0, sizeof(_refreshToken));
  _prefs.begin(NVS_NS, /*readOnly=*/true);
  const size_t n = _prefs.getString(NVS_KEY, _refreshToken, sizeof(_refreshToken));
  _prefs.end();

  if (n > 0 && _refreshToken[0]) {
    LOG_I("spotify: refresh token from NVS %s", redact(_refreshToken));
    return;
  }

  strlcpy(_refreshToken, SPOTIFY_REFRESH_TOKEN, sizeof(_refreshToken));
  if (strstr(_refreshToken, "your-spotify") != nullptr) {
    _refreshToken[0] = '\0';        /* the untouched placeholder */
    return;
  }
  if (_refreshToken[0]) {
    LOG_I("spotify: seeding refresh token from secrets.h %s", redact(_refreshToken));
    storeRefreshToken(_refreshToken);
  }
}

void SpotifyClient::storeRefreshToken(const char *token) {
  if (!token || !*token) return;
  _prefs.begin(NVS_NS, /*readOnly=*/false);
  _prefs.putString(NVS_KEY, token);
  _prefs.end();
  LOG_I("spotify: refresh token persisted %s", redact(token));
}

/* Erase a refresh token Spotify has rejected.
 *
 * Spotify's guidance for invalid_grant is explicit: "discard the refresh
 * token and start the appropriate authorization code flow instead of
 * retrying". Keeping it would mean burning one doomed request on every
 * reboot, and - worse - authorised() would report true at boot, so the UI
 * would show a broken player for a second before working out it is not
 * signed in. Erasing it sends the user straight to the sign-in screen. */
void SpotifyClient::discardRefreshToken() {
  _prefs.begin(NVS_NS, /*readOnly=*/false);
  _prefs.remove(NVS_KEY);
  _prefs.end();
  memset(_refreshToken, 0, sizeof(_refreshToken));
  memset(_accessToken, 0, sizeof(_accessToken));
  _tokenHeld = false;
  LOG_I("spotify: dead refresh token discarded from NVS");
}

bool SpotifyClient::tokenValid() const {
  return _tokenHeld && (int32_t)(millis() - _tokenExpiresAt) < 0;
}

void SpotifyClient::onNetworkUp() {
  _backoffMs        = 0;
  _consecutiveFails = 0;
  _nextPollAt       = millis();
  /* The old socket belongs to a dead association - drop it so HTTPClient
   * does not try to reuse a stale file descriptor. */
  releaseConnection();
}

void SpotifyClient::releaseConnection() {
  _apiHttp.end();
  _apiTls.stop();
  _apiConfigured = false;
}

/* =========================================================================
 *  Access-token refresh (PKCE: client ID only, no secret)
 * ====================================================================== */
bool SpotifyClient::refreshAccessToken() {
  if (_authDead || !_refreshToken[0]) return false;

  _status->api = ApiState::Refreshing;
  LOG_I("spotify: refreshing access token");

  /* A separate client from the API one: different host, and this runs at
   * most once an hour, so there is nothing to gain from keeping it open. */
  WiFiClientSecure tls;
#if TLS_VERIFY_CERTIFICATES
  tls.setCACert(CA_SPOTIFY_ACCOUNTS);
#else
  tls.setInsecure();
#endif
  tls.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(tls, TOKEN_URL)) {
    LOG_E("spotify: token endpoint begin() failed");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  /* Stack buffer - no String concatenation on the heap. */
  char encToken[encCap(sizeof(_refreshToken))];
  char encId[encCap(sizeof(_clientId))];
  if (!formEncode(_refreshToken, encToken, sizeof(encToken)) ||
      !formEncode(_clientId, encId, sizeof(encId))) {
    LOG_E("spotify: could not encode the refresh request");
    http.end();
    return false;
  }

  char body[sizeof(encToken) + sizeof(encId) + 64];
  const int bodyLen = snprintf(body, sizeof(body),
                               "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                               encToken, encId);
  if (bodyLen <= 0 || (size_t)bodyLen >= sizeof(body)) {
    LOG_E("spotify: token request body overflow");
    http.end();
    return false;
  }

  const int code = http.POST((uint8_t *)body, (size_t)bodyLen);
  /* Wipe the body: it held the refresh token in clear text. */
  memset(body, 0, sizeof(body));
  memset(encToken, 0, sizeof(encToken));

  if (code != HTTP_CODE_OK) {
    if (code == HTTP_CODE_BAD_REQUEST || code == HTTP_CODE_UNAUTHORIZED) {
      /* invalid_grant: the refresh token has been revoked or superseded.
       * Retrying cannot help, and hammering the endpoint with a dead
       * credential is exactly what gets an app rate-limited. */
      /* Also how a 6-month-old token ends: the lifetime runs from
       * authorisation and is NOT extended by use, so even a device that has
       * refreshed hourly for six months lands here exactly once. */
      LOG_E("spotify: refresh rejected (HTTP %d) - refresh token is dead", code);
      discardRefreshToken();
      _authDead       = true;
      _status->authed = false;
      _status->api    = ApiState::AuthFailed;
      _status->setMessage("Sign in again from the setup page", 3600000UL);
    } else {
      LOG_E("spotify: token refresh failed, HTTP %d", code);
      _status->api = ApiState::Error;
    }
    http.end();
    return false;
  }

  /* The token response is small and flat; a filter would cost more than it
   * saves. scope/token_type are ignored on purpose - we requested the
   * scopes at authorisation time and Spotify only ever returns Bearer. */
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    LOG_E("spotify: token response parse error: %s", err.c_str());
    _status->api = ApiState::Error;
    return false;
  }

  const char *access = doc["access_token"];
  if (!access || !*access) {
    LOG_E("spotify: token response had no access_token");
    _status->api = ApiState::Error;
    return false;
  }
  strlcpy(_accessToken, access, sizeof(_accessToken));
  _tokenHeld = true;

  const uint32_t expiresIn = doc["expires_in"] | 3600UL;
  /* Renew early so a poll can never race the expiry. */
  const uint32_t lifetime =
      (expiresIn > TOKEN_REFRESH_MARGIN_S) ? expiresIn - TOKEN_REFRESH_MARGIN_S : 60;
  _tokenExpiresAt = millis() + lifetime * 1000UL;

  /* THE IMPORTANT BIT: PKCE refreshes rotate the refresh token. If the
   * response carries a new one and we do not persist it, the old one stops
   * working at the next rotation and the device is locked out for good. */
  const char *rotated = doc["refresh_token"];
  if (rotated && *rotated && strcmp(rotated, _refreshToken) != 0) {
    LOG_I("spotify: refresh token rotated");
    strlcpy(_refreshToken, rotated, sizeof(_refreshToken));
    storeRefreshToken(_refreshToken);
  }

  _status->authed = true;
  LOG_I("spotify: access token ok, valid %lus %s",
        (unsigned long)lifetime, redact(_accessToken));
  return true;
}

/* =========================================================================
 *  HTTP plumbing
 * ====================================================================== */
bool SpotifyClient::ensureApiClient() {
  if (_apiConfigured) return true;
#if TLS_VERIFY_CERTIFICATES
  _apiTls.setCACert(CA_SPOTIFY_API);
#else
  _apiTls.setInsecure();
#endif
  _apiTls.setTimeout(HTTP_TIMEOUT_MS / 1000);
  _apiConfigured = true;
  return true;
}

int SpotifyClient::retryAfterSeconds() {
  const String h = _apiHttp.header("Retry-After");
  if (h.length() == 0) return 0;
  const long v = h.toInt();
  /* Clamp: a malformed or hostile header must not park the device for a
   * week, and 0 would defeat the back-off entirely. */
  if (v <= 0)    return 5;
  if (v > 3600)  return 3600;
  return (int)v;
}

/* Issue one request against api.spotify.com. Returns the HTTP status, or a
 * negative HTTPClient error code. The response body is left unread so the
 * caller can stream it. */
int SpotifyClient::apiSend(const char *method, const char *url, const char *body) {
  ensureApiClient();

  char auth[sizeof(_accessToken) + 16];
  snprintf(auth, sizeof(auth), "Bearer %s", _accessToken);

  /* Must be requested before sendRequest() or the header is discarded. */
  static const char *kCollect[] = {"Retry-After"};

  /* One place that prepares a request, so the retry below cannot drift out
   * of step with the first attempt - which is exactly how the missing
   * Content-Length would have survived a partial fix. */
  auto prepare = [&]() {
    _apiHttp.addHeader("Authorization", auth);
    if (body) {
      _apiHttp.addHeader("Content-Type", "application/json");
    } else if (strcmp(method, "GET") != 0) {
      /* THIS LINE IS LOAD-BEARING.
       *
       * Arduino's HTTPClient only emits Content-Length when there is a
       * payload (`if(payload && size > 0)` in sendRequest). Spotify's edge
       * rejects a bodyless PUT or POST with 411 Length Required before it
       * even looks at the token, so pause / next / previous / shuffle /
       * repeat all failed while play - the one command that sends a body -
       * worked. Verified against the live API:
       *
       *   PUT /v1/me/player/pause, no Content-Length   -> 411
       *   PUT /v1/me/player/pause, Content-Length: 0   -> 401 (shape OK)
       *
       * addHeader() permits this; it only blocks Connection, User-Agent,
       * Host and Authorization. */
      _apiHttp.addHeader("Content-Length", "0");
    }
    _apiHttp.collectHeaders(kCollect, 1);
    _apiHttp.setTimeout(HTTP_TIMEOUT_MS);
    _apiHttp.setConnectTimeout(HTTP_TIMEOUT_MS);
    /* Keep the TLS session open between polls. A fresh handshake to
     * api.spotify.com costs 1-2 s of RSA work on this chip; at a 4 s poll
     * interval that would leave the UI unresponsive roughly a third of the
     * time. */
    _apiHttp.setReuse(true);
  };

  auto fire = [&]() {
    return body ? _apiHttp.sendRequest(method, (uint8_t *)body, strlen(body))
                : _apiHttp.sendRequest(method, (uint8_t *)nullptr, 0);
  };

  if (!_apiHttp.begin(_apiTls, url)) {
    LOG_E("spotify: begin() failed for %s", url);
    memset(auth, 0, sizeof(auth));
    return -1000;
  }
  prepare();
  int code = fire();

  /* A kept-alive socket the server has since closed shows up as a connection
   * error on the next use. Drop it and try once more before giving up. */
  if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_CONNECTION_LOST ||
      code == HTTPC_ERROR_SEND_HEADER_FAILED || code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
    LOG_D("spotify: stale keep-alive (%d), reconnecting", code);
    releaseConnection();
    ensureApiClient();
    if (_apiHttp.begin(_apiTls, url)) {
      prepare();
      code = fire();
    }
  }

  memset(auth, 0, sizeof(auth));
  return code;
}

void SpotifyClient::setTargetDevice(const char *deviceId) {
  if (!deviceId || !*deviceId) {
    _targetDevice[0] = '\0';
    LOG_I("spotify: command target reset to the active device");
    return;
  }
  strlcpy(_targetDevice, deviceId, sizeof(_targetDevice));
  LOG_I("spotify: command target pinned to %s", _targetDevice);
}

/* Compose an endpoint URL. When a target device is pinned, device_id is
 * appended so the command goes there rather than to whatever Spotify
 * happens to consider active at that instant. */
void SpotifyClient::buildUrl(char *out, size_t outLen, const char *path,
                             const char *query) {
  const bool hasQuery  = (query && *query);
  const bool hasTarget = (_targetDevice[0] != '\0');

  if (hasQuery && hasTarget) {
    snprintf(out, outLen, "%s%s?%s&device_id=%s", API_BASE, path, query, _targetDevice);
  } else if (hasQuery) {
    snprintf(out, outLen, "%s%s?%s", API_BASE, path, query);
  } else if (hasTarget) {
    snprintf(out, outLen, "%s%s?device_id=%s", API_BASE, path, _targetDevice);
  } else {
    snprintf(out, outLen, "%s%s", API_BASE, path);
  }
}

void SpotifyClient::handleHttpFailure(int code, const char *what) {
  _consecutiveFails++;
  /* Exponential back-off, doubling from the error interval up to the cap.
   * Without this, an API outage turns into a tight retry loop that burns
   * the radio and the rate-limit quota. */
  if (_backoffMs == 0) _backoffMs = POLL_INTERVAL_ERROR_MS;
  else                 _backoffMs = min<uint32_t>(_backoffMs * 2, POLL_BACKOFF_MAX_MS);

  LOG_E("spotify: %s failed (%d), backing off %lums",
        what, code, (unsigned long)_backoffMs);
  _status->api = ApiState::Error;
  _nextPollAt  = millis() + _backoffMs;
}

/* =========================================================================
 *  Player poll
 * ====================================================================== */
void SpotifyClient::applyNoContent() {
  /* HTTP 204 from /me/player means: authenticated fine, but Spotify has no
   * active device for this account. Not an error - the most common steady
   * state for a display like this. */
  _st->clearItem();
  _st->hasDevice = false;
  _st->isPrivate = false;
  _st->device[0] = '\0';
  _st->volumePercent = -1;
  _st->supportsVolume = false;
  _status->api = ApiState::NoDevice;
}

void SpotifyClient::parsePlayer(Stream &stream) {
  /* The filter is what makes this affordable: the raw /me/player payload
   * can exceed 6 KB, most of it available-market arrays, and parsing it
   * whole would allocate far more heap than the live TLS session can
   * spare. Filtered, the document is a few hundred bytes. */
  JsonDocument filter;
  PlayerParser::buildFilter(filter);

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, stream, DeserializationOption::Filter(filter));
  if (err) {
    LOG_E("spotify: player parse error: %s", err.c_str());
    handleHttpFailure(0, "parse");
    return;
  }

  _consecutiveFails = 0;
  _backoffMs        = 0;
  _status->api      = ApiState::Ok;

  const char *note = PlayerParser::apply(doc, *_st, chooseArtUrl);
  if (note) _status->setMessage(note, MESSAGE_TIMEOUT_MS);

  /* Follow Spotify Connect. If the user moved playback in the Spotify app,
   * the device they chose here is stale - keeping it would send the next
   * command back to the device they just walked away from. */
  const bool grace = (int32_t)(millis() - _transferGraceUntil) < 0;
  if (shouldReleaseDevicePin(_targetDevice, _st->deviceId, grace)) {
    LOG_I("spotify: playback moved to \"%s\" elsewhere - following it",
          _st->device);
    setTargetDevice(nullptr);
  }

  LOG_D("spotify: %s / %s  %lu/%lums  %s",
        _st->title, _st->artist, (unsigned long)_st->progressMs,
        (unsigned long)_st->durationMs, _st->isPlaying ? "playing" : "paused");
}

void SpotifyClient::pollPlayer() {
  const int code = apiSend("GET", PLAYER_URL, nullptr);

  if (code == HTTP_CODE_OK) {
    parsePlayer(_apiHttp.getStream());
    _apiHttp.end();
    _nextPollAt = millis() + (_st->isPlaying ? POLL_INTERVAL_PLAYING_MS
                                             : POLL_INTERVAL_IDLE_MS);
    return;
  }

  if (code == HTTP_CODE_NO_CONTENT) {
    applyNoContent();
    _apiHttp.end();
    _consecutiveFails = 0;
    _backoffMs        = 0;
    _nextPollAt       = millis() + POLL_INTERVAL_IDLE_MS;
    return;
  }

  if (code == HTTP_CODE_UNAUTHORIZED) {
    /* Token expired early, or was revoked. Force a refresh on the next
     * service() pass rather than refreshing inline, so one loop iteration
     * never carries two TLS handshakes. */
    LOG_I("spotify: 401 - access token rejected, forcing refresh");
    _tokenHeld  = false;
    _apiHttp.end();
    _nextPollAt = millis() + 200;
    return;
  }

  if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
    const int wait = retryAfterSeconds();
    LOG_E("spotify: 429 rate limited, Retry-After %ds", wait);
    _apiHttp.end();
    _status->api   = ApiState::RateLimited;
    _rateLimitedTo = millis() + (uint32_t)wait * 1000UL;
    _nextPollAt    = _rateLimitedTo;
    _status->setMessage("Rate limited by Spotify", MESSAGE_TIMEOUT_MS);
    return;
  }

  if (code == HTTP_CODE_FORBIDDEN) {
    /* A 403 on a plain read is almost never a scope problem in practice -
     * the scopes are fixed in this firmware. It is nearly always a new app
     * still in Development Mode with the listening account missing from
     * Users Management, which returns 403 for every request. Name that
     * first, because "check scopes" sends people off to debug something
     * that is not wrong. */
    LOG_E("spotify: 403 on poll - account not on the app's allowlist?");
    _apiHttp.end();
    _status->setMessage("403 denied - add your account in Users Management",
                        MESSAGE_TIMEOUT_MS);
    _nextPollAt = millis() + POLL_INTERVAL_ERROR_MS;
    return;
  }

  _apiHttp.end();
  handleHttpFailure(code, "poll");
}

/* =========================================================================
 *  Transport commands
 * ====================================================================== */
bool SpotifyClient::enqueue(Cmd c, int32_t arg, const char *deviceId) {
  if (_queueCount >= QUEUE_LEN) {
    LOG_E("spotify: command queue full, dropping");
    return false;
  }
  const uint8_t slot = (uint8_t)((_queueHead + _queueCount) % QUEUE_LEN);
  Pending &p = _queue[slot];
  p.cmd = c;
  p.arg = arg;
  if (deviceId) strlcpy(p.deviceId, deviceId, sizeof(p.deviceId));
  else          p.deviceId[0] = '\0';
  _queueCount++;
  return true;
}

void SpotifyClient::cmdPrevious() { enqueue(Cmd::Prev); }
void SpotifyClient::cmdNext()     { enqueue(Cmd::Next); }
void SpotifyClient::cmdPlay()     { enqueue(Cmd::Play);  _st->isPlaying = true; }
void SpotifyClient::cmdPause()    { enqueue(Cmd::Pause); _st->isPlaying = false; }

void SpotifyClient::cmdPlayPause() {
  /* Optimistic local flip so the button reacts instantly; the follow-up
   * poll corrects it if Spotify disagreed. */
  const bool wantPlay = !_st->isPlaying;
  enqueue(wantPlay ? Cmd::Play : Cmd::Pause);
  _st->isPlaying = wantPlay;
  _st->anchorProgress(_st->progressNow());
}

void SpotifyClient::cmdVolume(int16_t percent) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  enqueue(Cmd::Volume, percent);
  _st->volumePercent = percent;     /* optimistic, corrected on next poll */
}

void SpotifyClient::cmdSeek(uint32_t positionMs) {
  if (_st->durationMs && positionMs > _st->durationMs) {
    positionMs = _st->durationMs;
  }
  enqueue(Cmd::Seek, (int32_t)positionMs);
  /* Re-anchor immediately so the bar jumps to where the user tapped
   * instead of crawling back from the old position over the next poll. */
  _st->anchorProgress(positionMs);
}

void SpotifyClient::cmdShuffle(bool on) {
  enqueue(Cmd::Shuffle, on ? 1 : 0);
  _st->shuffle = on;
}

void SpotifyClient::cmdRepeat(RepeatMode mode) {
  enqueue(Cmd::Repeat, (int32_t)mode);
  _st->repeat = mode;
}

void SpotifyClient::cmdTransfer(const char *deviceId, bool play) {
  if (!deviceId || !*deviceId) return;
  enqueue(Cmd::Transfer, play ? 1 : 0, deviceId);
  /* Pin future commands to the chosen device straight away: Spotify can
   * take a second or two to report the handover, and commands issued in
   * that window would otherwise go to the old device. */
  setTargetDevice(deviceId);
  _transferGraceUntil = millis() + TRANSFER_GRACE_MS;
  /* Show the new destination immediately rather than waiting for the poll. */
  const SpotifyDevice *d = _devices.byId(deviceId);
  if (d) {
    strlcpy(_st->device, d->name, sizeof(_st->device));
    strlcpy(_st->deviceId, d->id, sizeof(_st->deviceId));
    strlcpy(_st->deviceType, d->type, sizeof(_st->deviceType));
    _st->hasDevice      = true;
    _st->supportsVolume = d->supportsVolume;
    if (d->volumePercent >= 0) _st->volumePercent = d->volumePercent;
  }
  if (play) _st->isPlaying = true;
  refreshDevicesSoon();
}

bool SpotifyClient::runCommand(const Pending &p) {
  char url[256];
  char query[96];
  const char *method = "PUT";
  const char *body   = nullptr;
  char bodyBuf[128];

  switch (p.cmd) {
    case Cmd::Prev:
      method = "POST";
      buildUrl(url, sizeof(url), "/previous", nullptr);
      break;
    case Cmd::Next:
      method = "POST";
      buildUrl(url, sizeof(url), "/next", nullptr);
      break;
    case Cmd::Play:
      buildUrl(url, sizeof(url), "/play", nullptr);
      /* An empty JSON object resumes where playback left off; omitting the
       * body makes Spotify restart the current context from the top. */
      body = "{}";
      break;
    case Cmd::Pause:
      buildUrl(url, sizeof(url), "/pause", nullptr);
      break;
    case Cmd::Volume:
      snprintf(query, sizeof(query), "volume_percent=%d", (int)p.arg);
      buildUrl(url, sizeof(url), "/volume", query);
      break;
    case Cmd::Seek:
      snprintf(query, sizeof(query), "position_ms=%ld", (long)p.arg);
      buildUrl(url, sizeof(url), "/seek", query);
      break;
    case Cmd::Shuffle:
      snprintf(query, sizeof(query), "state=%s", p.arg ? "true" : "false");
      buildUrl(url, sizeof(url), "/shuffle", query);
      break;
    case Cmd::Repeat:
      snprintf(query, sizeof(query), "state=%s",
               repeatToApi((RepeatMode)p.arg));
      buildUrl(url, sizeof(url), "/repeat", query);
      break;
    case Cmd::Transfer:
      /* Transfer is the one command that must NOT carry device_id in the
       * query - the target goes in the body, and Spotify rejects the
       * combination. */
      snprintf(url, sizeof(url), "%s", API_BASE);
      snprintf(bodyBuf, sizeof(bodyBuf),
               "{\"device_ids\":[\"%s\"],\"play\":%s}",
               p.deviceId, p.arg ? "true" : "false");
      body = bodyBuf;
      break;
  }

  const int code = apiSend(method, url, body);

  /* Read the header BEFORE end(): once the response object is torn down
   * the collected headers are not guaranteed to survive. */
  const int retryAfter =
      (code == HTTP_CODE_TOO_MANY_REQUESTS) ? retryAfterSeconds() : 0;
  _apiHttp.end();

  switch (code) {
    case HTTP_CODE_OK:
    case HTTP_CODE_NO_CONTENT:
    case HTTP_CODE_ACCEPTED:
      LOG_I("spotify: command ok (%d)", code);
      /* Re-poll shortly: Spotify needs a moment to apply the change, and
       * the UI should show what actually happened, not what we asked for. */
      _nextPollAt = millis() + POLL_AFTER_COMMAND_MS;
      if (p.cmd == Cmd::Transfer) refreshDevicesSoon();
      return true;

    case HTTP_CODE_NOT_FOUND:
      /* Spotify returns 404 NO_ACTIVE_DEVICE when nothing is listening, and
       * also when a pinned device has gone away. Unpin so the next command
       * has a chance of landing somewhere real. */
      LOG_E("spotify: 404 - device not found or not active");
      _status->setMessage("Device unavailable - pick another",
                          MESSAGE_TIMEOUT_MS);
      if (_targetDevice[0]) setTargetDevice(nullptr);
      _st->hasDevice = false;
      _status->api   = ApiState::NoDevice;
      refreshDevicesSoon();
      return false;

    case HTTP_CODE_FORBIDDEN:
      /* Almost always a free account (playback control is Premium-only) or
       * a restricted device such as a TV app. */
      LOG_E("spotify: 403 - command not permitted");
      if (p.cmd == Cmd::Volume) {
        _status->setMessage("This device has no remote volume",
                            MESSAGE_TIMEOUT_MS);
      } else {
        _status->setMessage("Not permitted - Premium or device restricted",
                            MESSAGE_TIMEOUT_MS);
      }
      return false;

    case HTTP_CODE_UNAUTHORIZED:
      LOG_I("spotify: 401 on command, forcing token refresh");
      _tokenHeld = false;
      return false;

    case HTTP_CODE_TOO_MANY_REQUESTS:
      LOG_E("spotify: 429 on command, Retry-After %ds", retryAfter);
      _rateLimitedTo = millis() + (uint32_t)retryAfter * 1000UL;
      _status->api   = ApiState::RateLimited;
      _status->setMessage("Rate limited by Spotify", MESSAGE_TIMEOUT_MS);
      return false;

    case HTTP_CODE_LENGTH_REQUIRED:
      /* Regression guard. Spotify's edge rejects a bodyless PUT/POST that
       * carries no Content-Length, and Arduino's HTTPClient only emits one
       * when there is a payload - so this is a firmware bug, never a user
       * or account problem. apiSend() adds "Content-Length: 0"; if this
       * ever fires again, that is what broke. */
      LOG_E("spotify: 411 - Content-Length missing (firmware bug, not your "
            "account)");
      _status->setMessage("Firmware bug: request rejected (411)",
                          MESSAGE_TIMEOUT_MS);
      return false;

    default:
      LOG_E("spotify: command failed, HTTP %d", code);
      _status->setMessage("Playback command failed", MESSAGE_TIMEOUT_MS);
      return false;
  }
}

/* =========================================================================
 *  Devices
 * ====================================================================== */
void SpotifyClient::pollDevices() {
  const int code = apiSend("GET", DEVICES_URL, nullptr);

  if (code != HTTP_CODE_OK) {
    _apiHttp.end();
    /* Not fatal: the device list is a convenience, and a stale one is far
     * better than tearing down the whole UI over it. */
    LOG_E("spotify: devices fetch failed, HTTP %d", code);
    _nextDevicesAt = millis() + DEVICE_POLL_INTERVAL_MS;
    return;
  }

  JsonDocument filter;
  PlayerParser::buildDevicesFilter(filter);
  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, _apiHttp.getStream(), DeserializationOption::Filter(filter));
  _apiHttp.end();

  if (err) {
    LOG_E("spotify: devices parse error: %s", err.c_str());
    _nextDevicesAt = millis() + DEVICE_POLL_INTERVAL_MS;
    return;
  }

  const uint8_t n = PlayerParser::applyDevices(doc, _devices);
  (void)n;   /* only read by LOG_D, which compiles out below log level 3 */
  LOG_D("spotify: %u device(s) available", (unsigned)n);

  /* If the pinned target has disappeared, stop aiming at it - otherwise
   * every command 404s until the user notices. */
  if (_targetDevice[0] && !_devices.byId(_targetDevice)) {
    LOG_I("spotify: pinned device is gone, reverting to the active one");
    setTargetDevice(nullptr);
  }

  _nextDevicesAt = millis() + DEVICE_POLL_INTERVAL_MS;
}

/* =========================================================================
 *  Scheduler - at most one HTTP transaction per call
 * ====================================================================== */
void SpotifyClient::service(bool networkUp) {
  if (!networkUp) {
    if (_status->api != ApiState::AuthFailed) _status->api = ApiState::Error;
    return;
  }
  if (_authDead) return;

  const uint32_t now = millis();

  /* A 429 gags every endpoint, commands included. */
  if (_rateLimitedTo && (int32_t)(now - _rateLimitedTo) < 0) return;
  _rateLimitedTo = 0;

  /* 1. A valid access token is a precondition for everything else. */
  if (!tokenValid()) {
    if (!refreshAccessToken()) {
      if (!_authDead) {
        if (_backoffMs == 0) _backoffMs = POLL_INTERVAL_ERROR_MS;
        else _backoffMs = min<uint32_t>(_backoffMs * 2, POLL_BACKOFF_MAX_MS);
        _nextPollAt = now + _backoffMs;
      }
    }
    return;                       /* one transaction per call */
  }

  /* 2. Queued commands outrank polling - they are user input, and the user
   *    is waiting for the screen to react. */
  if (_queueCount > 0) {
    const Pending p = _queue[_queueHead];
    _queueHead = (uint8_t)((_queueHead + 1) % QUEUE_LEN);
    _queueCount--;
    runCommand(p);

    /* Resync shortly whether or not it worked. On success this picks up
     * what Spotify actually did; on failure it undoes the optimistic local
     * state - without it, a rejected pause leaves the screen showing
     * "paused" while the music plays on for up to a full idle interval.
     *
     * Skipped while rate-limited, where Retry-After is the only schedule
     * we are allowed to keep. */
    if (!_rateLimitedTo) _nextPollAt = millis() + POLL_AFTER_COMMAND_MS;
    return;
  }

  /* 3. Player state. This is what the display is for, so it outranks the
   *    device list. */
  if ((int32_t)(now - _nextPollAt) >= 0) {
    pollPlayer();
    return;
  }

  /* 4. Device list, on its own much lazier schedule. */
  if ((int32_t)(now - _nextDevicesAt) >= 0) {
    pollDevices();
  }
}

/* =========================================================================
 *  Authorisation - Authorization Code + PKCE, performed on the device
 * ====================================================================== */
bool SpotifyClient::buildAuthUrl(char *out, size_t outLen) {
  if (!_clientId[0]) {
    snprintf(out, outLen, "%s", "");
    return false;
  }

  /* 64 bytes from the hardware RNG -> 86 base64url chars, inside RFC 7636's
   * 43..128 range. esp_random() is only properly seeded once the RF
   * subsystem is up, which it is by the time anyone can reach the setup
   * page over Wi-Fi. */
  uint8_t raw[64];
  for (size_t i = 0; i < sizeof(raw); i += 4) {
    const uint32_t r = esp_random();
    memcpy(raw + i, &r, 4);
  }
  if (!AuthUtil::base64UrlEncode(raw, sizeof(raw), _pkceVerifier,
                                 sizeof(_pkceVerifier))) {
    LOG_E("auth: could not build a PKCE verifier");
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256((const unsigned char *)_pkceVerifier, strlen(_pkceVerifier),
                 digest, /*is224=*/0);
  char challenge[64];
  if (!AuthUtil::base64UrlEncode(digest, sizeof(digest), challenge,
                                 sizeof(challenge))) {
    LOG_E("auth: could not build a PKCE challenge");
    return false;
  }

  char encRedirect[REDIRECT_ENC_CAP];
  char encScope[encCap(sizeof(SCOPES))];
  if (!formEncode(SPOTIFY_REDIRECT_URI, encRedirect, sizeof(encRedirect)) ||
      !formEncode(SCOPES, encScope, sizeof(encScope))) {
    LOG_E("auth: SPOTIFY_REDIRECT_URI did not fit - shorten it in config.h");
    return false;
  }

  const int n = snprintf(
      out, outLen,
      "%s?client_id=%s&response_type=code&redirect_uri=%s"
      "&scope=%s&code_challenge_method=S256&code_challenge=%s&show_dialog=true",
      AUTHORIZE_URL, _clientId, encRedirect, encScope, challenge);

  if (n <= 0 || (size_t)n >= outLen) {
    LOG_E("auth: authorisation URL did not fit");
    return false;
  }
  LOG_I("auth: authorisation URL generated");
  return true;
}

bool SpotifyClient::completeAuth(const char *pastedUrlOrCode, char *err,
                                 size_t errLen) {
  auto failWith = [&](const char *m) {
    strlcpy(err, m, errLen);
    LOG_E("auth: %s", m);
    return false;
  };

  if (!pastedUrlOrCode || !*pastedUrlOrCode) return failWith("Nothing pasted");
  if (!_pkceVerifier[0]) {
    return failWith("No sign-in in progress - start again");
  }
  if (!_clientId[0]) return failWith("No client ID set");

  /* Spotify reports a refusal as ?error=access_denied on the redirect, and
   * that is far more useful to surface than "no code found". */
  char errParam[64];
  if (AuthUtil::queryParam(pastedUrlOrCode, "error", errParam, sizeof(errParam))) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Spotify returned: %s", errParam);
    return failWith(msg);
  }

  /* Accept either the whole redirected URL or a bare authorisation code,
   * because people paste both. */
  char code[512];
  if (!AuthUtil::queryParam(pastedUrlOrCode, "code", code, sizeof(code))) {
    if (strchr(pastedUrlOrCode, '?') || strchr(pastedUrlOrCode, '/')) {
      return failWith("No ?code= found in that URL");
    }
    strlcpy(code, pastedUrlOrCode, sizeof(code));
  }

  LOG_I("auth: exchanging authorisation code");

  WiFiClientSecure tls;
#if TLS_VERIFY_CERTIFICATES
  tls.setCACert(CA_SPOTIFY_ACCOUNTS);
#else
  tls.setInsecure();
#endif
  tls.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(tls, TOKEN_URL)) return failWith("Could not reach Spotify");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  char encCode[encCap(sizeof(code))];
  char encRedirect[REDIRECT_ENC_CAP];
  char encVerifier[encCap(sizeof(_pkceVerifier))];
  char encId[encCap(sizeof(_clientId))];
  if (!formEncode(code, encCode, sizeof(encCode)) ||
      !formEncode(SPOTIFY_REDIRECT_URI, encRedirect, sizeof(encRedirect)) ||
      !formEncode(_pkceVerifier, encVerifier, sizeof(encVerifier)) ||
      !formEncode(_clientId, encId, sizeof(encId))) {
    http.end();
    return failWith("Could not encode the request - check SPOTIFY_REDIRECT_URI");
  }

  char body[sizeof(encCode) + sizeof(encRedirect) + sizeof(encVerifier) +
            sizeof(encId) + 96];
  const int bodyLen = snprintf(
      body, sizeof(body),
      "grant_type=authorization_code&code=%s&redirect_uri=%s"
      "&client_id=%s&code_verifier=%s",
      encCode, encRedirect, encId, encVerifier);
  if (bodyLen <= 0 || (size_t)bodyLen >= sizeof(body)) {
    http.end();
    return failWith("Authorisation request too large");
  }

  const int httpCode = http.POST((uint8_t *)body, (size_t)bodyLen);
  memset(body, 0, sizeof(body));
  memset(encVerifier, 0, sizeof(encVerifier));
  memset(encCode, 0, sizeof(encCode));

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    char msg[80];
    /* An authorisation code is single-use and expires in about a minute,
     * which is overwhelmingly the reason this fails in practice. */
    snprintf(msg, sizeof(msg),
             httpCode == HTTP_CODE_BAD_REQUEST
                 ? "Code rejected (expired or already used) - start again"
                 : "Token exchange failed (HTTP %d)",
             httpCode);
    return failWith(msg);
  }

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, http.getStream());
  http.end();
  if (jerr) return failWith("Could not read Spotify's reply");

  const char *access  = doc["access_token"];
  const char *refresh = doc["refresh_token"];
  if (!refresh || !*refresh) return failWith("No refresh token returned");

  strlcpy(_refreshToken, refresh, sizeof(_refreshToken));
  storeRefreshToken(_refreshToken);

  if (access && *access) {
    strlcpy(_accessToken, access, sizeof(_accessToken));
    _tokenHeld = true;
    const uint32_t expiresIn = doc["expires_in"] | 3600UL;
    const uint32_t lifetime =
        (expiresIn > TOKEN_REFRESH_MARGIN_S) ? expiresIn - TOKEN_REFRESH_MARGIN_S : 60;
    _tokenExpiresAt = millis() + lifetime * 1000UL;
  }

  /* The verifier is spent - one authorisation, one verifier. */
  memset(_pkceVerifier, 0, sizeof(_pkceVerifier));

  _authDead         = false;
  _consecutiveFails = 0;
  _backoffMs        = 0;
  _nextPollAt       = millis();
  _nextDevicesAt    = millis();
  _status->api      = ApiState::Ok;
  _status->authed   = true;
  _status->setMessage("Signed in to Spotify", MESSAGE_TIMEOUT_MS);
  LOG_I("auth: signed in, refresh token stored %s", redact(_refreshToken));
  err[0] = '\0';
  return true;
}

void SpotifyClient::signOut() {
  _prefs.begin(NVS_NS, /*readOnly=*/false);
  _prefs.remove(NVS_KEY);
  _prefs.end();

  memset(_refreshToken, 0, sizeof(_refreshToken));
  memset(_accessToken, 0, sizeof(_accessToken));
  memset(_pkceVerifier, 0, sizeof(_pkceVerifier));
  _tokenHeld      = false;
  _authDead       = true;
  _targetDevice[0] = '\0';
  _devices.count  = 0;
  _devices.valid  = false;
  _st->clearItem();
  _st->hasDevice  = false;
  _status->authed = false;
  _status->api    = ApiState::NoToken;
  _status->setMessage("Signed out", MESSAGE_TIMEOUT_MS);
  LOG_I("auth: signed out, refresh token erased from NVS");
}
