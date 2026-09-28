/* =========================================================================
 *  ApiServer.h - the on-device HTTP server.
 *
 *  Two jobs:
 *    1. Setup pages: Spotify sign-in (paste the redirect URL) and the
 *       client-ID field. Wi-Fi is configured on the panel itself, so it is
 *       only surfaced here for reference.
 *    2. The simplified REST API under /api/spotify/ .
 *
 *  READS ARE CACHED, WRITES ARE QUEUED
 *  -----------------------------------
 *  No handler calls Spotify synchronously. GETs answer from the state the
 *  poller already maintains, and commands go onto SpotifyClient's queue and
 *  return 202. Two reasons, both practical rather than stylistic:
 *
 *    - A blocking upstream call inside a handler stalls the whole loop,
 *      including the display and touch, for the length of a TLS round trip.
 *    - It would open a second TLS session while the poller or an artwork
 *      download already has one, and two mbedTLS contexts at once is about
 *      40 KB the heap cannot always spare.
 *
 *  Cached data is at most one poll interval old, which is well inside what
 *  a remote control needs.
 *
 *  The sign-in exchange is the one deliberate exception: it blocks, because
 *  it happens once, during setup, when nothing else is running.
 *
 *  NO AUTHENTICATION
 *  -----------------
 *  Anyone who can reach the device on your LAN can control playback and
 *  read the setup page. That is the same trust boundary as a Chromecast,
 *  and it is stated plainly in the README rather than papered over with a
 *  password field that would have to live in NVS anyway.
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "AppState.h"
#include "SpotifyClient.h"
#include "config.h"   /* WEB_SERVER_PORT */

class NetManager;

class ApiServer {
 public:
  void begin(SpotifyClient *spotify, PlaybackState *state, AppStatus *status,
             NetManager *net);
  /* Start listening. Safe to call repeatedly; only the first call binds. */
  void start();
  void stop();
  void service();

  bool running() const { return _running; }

 private:
  void routes();

  /* ---- Wi-Fi (usable without the touchscreen) ---- */
  void handleWifiScan();
  void handleWifiSet();
  void handleWifiForget();

  /* ---- setup pages ---- */
  void handleRoot();
  void handleAuthStart();
  void handleAuthComplete();
  void handleSignOut();
  void handleClientId();
  void handleStatus();

  /* ---- /api/spotify/ endpoints ---- */
  void handleDevices();
  void handleActiveDevice();
  void handleCurrentlyPlaying();
  void handlePlay();
  void handlePause();
  void handleNext();
  void handlePrevious();
  void handleSeek();
  void handleVolume();
  void handleShuffle();
  void handleRepeat();

  /* ---- helpers ---- */
  void sendJson(int code, const String &body);
  void sendAccepted(const char *what);
  bool requireAuth();
  /* Read a parameter from the query string or a form body. */
  bool param(const char *name, String &out);
  bool paramInt(const char *name, long &out);

  WebServer      _server{WEB_SERVER_PORT};
  SpotifyClient *_spotify = nullptr;
  PlaybackState *_state   = nullptr;
  AppStatus     *_status  = nullptr;
  NetManager    *_net     = nullptr;
  bool           _running = false;
  bool           _routed  = false;

  /* Joining a new network tears down the socket this request arrived on,
   * so the change is applied one loop iteration later - after the response
   * has actually been flushed. Otherwise the browser reports a failure for
   * an operation that worked. */
  bool _wifiChangePending = false;
  char _pendingSsid[33]   = {0};
  char _pendingPass[65]   = {0};
};
