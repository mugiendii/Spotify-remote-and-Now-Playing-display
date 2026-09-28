/* =========================================================================
 *  SpotifyClient.h - OAuth token lifecycle, playback polling and transport
 *  commands. The only module that talks to the Spotify Web API.
 *
 *  OAUTH MODEL
 *  -----------
 *  Authorization Code + PKCE. The browser half runs once on a PC
 *  (tools/get_refresh_token.py); the device only ever performs the refresh
 *  leg, which under PKCE needs nothing but the client ID and the refresh
 *  token. No client secret is compiled in - see README "Security model".
 *
 *  Spotify ROTATES the refresh token on PKCE refreshes. Every rotation is
 *  written to NVS, so the value in secrets.h is a first-boot seed only.
 *  Missing that detail is the classic way one of these builds works for an
 *  hour and then dies permanently.
 *
 *  SCHEDULING
 *  ----------
 *  service() performs at most one HTTP transaction per call and returns.
 *  It never blocks on a timer: the next action is always a millis()
 *  deadline. Requests themselves are short blocking reads with bounded
 *  timeouts - a poll is a couple of kilobytes over a kept-alive TLS
 *  socket, typically under 300 ms. The artwork download, which is the one
 *  genuinely slow transfer, is incremental and lives in AlbumArt.
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include "AppState.h"
#include "config.h"   /* SPOTIFY_REDIRECT_URI */

class SpotifyClient {
 public:
  void begin(PlaybackState *state, AppStatus *status);

  /* Drive the state machine. Call every loop iteration. */
  void service(bool networkUp);

  /* Network came back: retry immediately instead of waiting out a back-off. */
  void onNetworkUp();

  /* ---- transport commands ---------------------------------------------
   *  These only enqueue. The HTTP call happens in service(), so neither a
   *  touch handler nor a web request ever blocks on the network.
   *
   *  Every command targets the explicitly selected device when there is
   *  one, and otherwise whatever Spotify currently considers active.     */
  void cmdPrevious();
  void cmdNext();
  void cmdPlayPause();
  void cmdPlay();
  void cmdPause();
  void cmdVolume(int16_t percent);
  void cmdSeek(uint32_t positionMs);
  void cmdShuffle(bool on);
  void cmdRepeat(RepeatMode mode);
  /* PUT /v1/me/player - move playback to another Connect device.
   * `play` true keeps audio running through the handover. */
  void cmdTransfer(const char *deviceId, bool play);

  /* ---- devices --------------------------------------------------------- */
  const DeviceList &devices() const { return _devices; }
  /* Ask for a devices refresh at the next opportunity. */
  void refreshDevicesSoon() { _nextDevicesAt = millis(); }

  /* Explicit command target. Pass nullptr or "" to fall back to whichever
   * device Spotify reports as active. */
  void setTargetDevice(const char *deviceId);
  const char *targetDevice() const { return _targetDevice; }

  /* ---- authorisation ---------------------------------------------------
   *  Authorization Code + PKCE, run entirely on the device. buildAuthUrl()
   *  mints a fresh verifier and returns the URL to open in a browser;
   *  completeAuth() takes the redirected URL (or a bare code) and performs
   *  the token exchange.                                                  */
  bool authorised() const { return !_authDead && _refreshToken[0]; }

  /* Buffer size buildAuthUrl() needs. Derived from the configured redirect
   * URI (worst case three bytes per character once percent-encoded) plus
   * headroom for the client ID, the scopes, the PKCE challenge and the
   * fixed parameters - so pointing the device at a long domain cannot
   * quietly outgrow the caller's buffer. */
  static constexpr size_t AUTH_URL_CAP =
      420 + (sizeof(SPOTIFY_REDIRECT_URI) - 1) * 3;

  bool buildAuthUrl(char *out, size_t outLen);
  bool completeAuth(const char *pastedUrlOrCode, char *err, size_t errLen);
  void signOut();

  /* Runtime client ID, stored in NVS and seeded from secrets.h. */
  const char *clientId() const { return _clientId; }
  void setClientId(const char *id);

  /* Free the kept-alive API socket so the artwork download does not have
   * to hold a second TLS session open at the same time. ~20 KB of heap. */
  void releaseConnection();

  /* True while a command is queued or a request is due imminently - main
   * uses this to avoid starting an artwork download across a poll. */
  bool busy() const { return _queueCount > 0; }

 private:
  enum class Cmd : uint8_t {
    Prev, Next, Play, Pause, Volume, Seek, Shuffle, Repeat, Transfer
  };
  struct Pending {
    Cmd      cmd = Cmd::Prev;
    int32_t  arg = 0;               /* volume %, seek ms, shuffle, repeat  */
    char     deviceId[ID_LEN] = {0};/* Transfer target, else empty         */
  };

  /* ---- token ---------------------------------------------------------- */
  bool refreshAccessToken();
  bool tokenValid() const;
  void loadRefreshToken();
  void storeRefreshToken(const char *token);
  void discardRefreshToken();

  /* ---- requests -------------------------------------------------------- */
  bool  ensureApiClient();
  int   apiSend(const char *method, const char *url, const char *body);
  void  pollPlayer();
  bool  runCommand(const Pending &p);
  void  handleHttpFailure(int code, const char *what);
  int   retryAfterSeconds();

  /* ---- parsing --------------------------------------------------------- */
  void parsePlayer(Stream &stream);
  void applyNoContent();

  /* ---- queue ----------------------------------------------------------- */
  bool enqueue(Cmd c, int32_t arg = 0, const char *deviceId = nullptr);

  /* Build an endpoint URL, appending device_id= when a target is set. */
  void buildUrl(char *out, size_t outLen, const char *path, const char *query);

  void pollDevices();
  void loadClientId();

  static constexpr uint8_t QUEUE_LEN = 4;

  PlaybackState *_st     = nullptr;
  AppStatus     *_status = nullptr;

  WiFiClientSecure _apiTls;
  HTTPClient       _apiHttp;
  bool             _apiConfigured = false;

  Preferences _prefs;

  char     _accessToken[300]  = {0};
  char     _refreshToken[300] = {0};
  uint32_t _tokenExpiresAt    = 0;    /* millis() deadline                 */
  bool     _tokenHeld         = false;

  DeviceList _devices;
  char       _targetDevice[ID_LEN] = {0};
  uint32_t   _nextDevicesAt = 0;
  /* Spotify keeps reporting the previous device for a moment after a
   * transfer, so the pin is protected until this deadline passes. */
  uint32_t   _transferGraceUntil = 0;

  /* PKCE verifier for an in-progress authorisation. RAM only: it is valid
   * for one sign-in attempt, and a reboot mid-flow simply means starting
   * the sign-in again. */
  char _pkceVerifier[100] = {0};
  char _clientId[64]      = {0};

  uint32_t _nextPollAt    = 0;
  uint32_t _backoffMs     = 0;
  uint32_t _rateLimitedTo = 0;
  uint8_t  _consecutiveFails = 0;
  bool     _authDead      = false;    /* refresh token rejected: stop trying */

  Pending _queue[QUEUE_LEN];
  uint8_t _queueHead  = 0;
  uint8_t _queueCount = 0;
};
