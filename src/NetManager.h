/* =========================================================================
 *  NetManager.h - Wi-Fi association, reconnection, credential storage and
 *  network scanning.
 *
 *  Credentials live in NVS, not in the firmware image. secrets.h is only a
 *  first-boot seed, exactly like the Spotify refresh token: once anything
 *  is stored, NVS wins. That is what lets the device be moved to another
 *  network from the on-screen keyboard with no reflash.
 *
 *  Entirely non-blocking. begin() kicks off an association and returns;
 *  service() nudges the state machine; nothing ever spins on WiFi.status()
 *  in a delay loop. Scanning is asynchronous too - a synchronous scan
 *  blocks for two seconds and would stall the UI mid-tap.
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "AppState.h"

/* One scanned access point, trimmed to what the picker shows. */
struct ScannedNet {
  char    ssid[33] = {0};    /* 32 chars + NUL, per 802.11 */
  int8_t  bars     = 0;      /* 0..4 */
  bool    secured  = true;
};

class NetManager {
 public:
  void begin();
  void service();

  bool     isUp()  const { return _state == NetState::Up; }
  NetState state() const { return _state; }
  int8_t   bars()  const { return _bars; }
  const char *ssid() const { return _ssid; }
  const char *ip()   const { return _ip; }

  /* True exactly once per transition into the Up state, so callers can
   * re-arm anything that needs a live network. */
  bool consumeJustConnected() {
    const bool v = _justConnected;
    _justConnected = false;
    return v;
  }

  /* Do we have credentials at all? False means "show the setup screen". */
  bool hasCredentials() const { return _ssid[0] != '\0'; }

  /* Store new credentials and immediately try them. */
  void setCredentials(const char *ssid, const char *password);
  void forgetCredentials();

  /* ---- asynchronous scanning ------------------------------------------ */
  void startScan();
  bool scanning() const { return _scanning; }
  /* True when the last scan could not even be started, or the driver
   * reported a failure. Distinct from "completed and found nothing",
   * because the two mean entirely different things to the user. */
  bool scanFailed() const { return _scanFailed; }
  /* Results of the most recent completed scan, strongest first. */
  const ScannedNet *networks() const { return _nets; }
  uint8_t networkCount() const { return _netCount; }
  /* True once after a scan completes, so the UI knows to repaint. */
  bool consumeScanDone() {
    const bool v = _scanDone;
    _scanDone = false;
    return v;
  }

  static constexpr uint8_t MAX_NETS = 16;

 private:
  void startAttempt();
  void sampleRssi();
  void loadCredentials();
  void pollScan();
  bool beginScan();
  static int8_t rssiToBars(int32_t rssi);

  NetState _state         = NetState::Down;
  bool     _justConnected = false;
  int8_t   _bars          = 0;
  uint32_t _attemptStart  = 0;
  uint32_t _nextAttempt   = 0;
  uint32_t _lastRssi      = 0;
  uint16_t _attempts      = 0;

  char _ssid[33] = {0};
  char _pass[65] = {0};      /* 64 chars + NUL, per WPA2 */
  char _ip[16]   = {0};

  bool       _scanning   = false;
  bool       _scanDone   = false;
  bool       _scanFailed = false;
  uint8_t    _scanTries  = 0;
  uint32_t   _nextScanAt = 0;
  uint8_t    _netCount = 0;
  ScannedNet _nets[MAX_NETS];
};
