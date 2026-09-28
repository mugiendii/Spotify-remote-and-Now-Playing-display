#include "NetManager.h"

#include <Preferences.h>
#include <WiFi.h>

#include "Log.h"
#include "SecretsGate.h"
#include "config.h"

namespace {
/* How long to wait before retrying a scan that would not start, and how
 * many times to try before leaving it to the user. */
constexpr uint32_t SCAN_RETRY_MS  = 1500;
constexpr uint8_t  SCAN_MAX_TRIES = 4;

constexpr char NVS_NS[]   = "wifi";
constexpr char NVS_SSID[] = "ssid";
constexpr char NVS_PASS[] = "pass";
}  // namespace

void NetManager::begin() {
  WiFi.persistent(false);       /* do not wear out NVS rewriting the SSID */
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);         /* modem sleep adds latency to every poll */
  /* A stable hostname makes the device findable on the LAN and keeps DHCP
   * leases tidy; it is also what you type if your router does local DNS. */
  WiFi.setHostname("spotify-tft");

  loadCredentials();
  if (!hasCredentials()) {
    LOG_I("wifi: no credentials stored - waiting for on-screen setup");
    _state = NetState::Down;
    return;
  }
  startAttempt();
}

/* NVS wins over secrets.h: the stored value is whatever the user last
 * entered on the device, while secrets.h only ever holds the build-time
 * seed. */
void NetManager::loadCredentials() {
  Preferences prefs;
  memset(_ssid, 0, sizeof(_ssid));
  memset(_pass, 0, sizeof(_pass));

  prefs.begin(NVS_NS, /*readOnly=*/true);
  prefs.getString(NVS_SSID, _ssid, sizeof(_ssid));
  prefs.getString(NVS_PASS, _pass, sizeof(_pass));
  prefs.end();

  if (_ssid[0]) {
    LOG_I("wifi: credentials from NVS for \"%s\"", _ssid);
    return;
  }

  strlcpy(_ssid, WIFI_SSID, sizeof(_ssid));
  strlcpy(_pass, WIFI_PASSWORD, sizeof(_pass));
  if (strstr(_ssid, "your-wifi") != nullptr) {
    /* Untouched template - treat as unconfigured rather than trying to
     * associate with a network called "your-wifi-ssid". */
    memset(_ssid, 0, sizeof(_ssid));
    memset(_pass, 0, sizeof(_pass));
    return;
  }
  if (_ssid[0]) {
    LOG_I("wifi: seeding credentials from secrets.h for \"%s\"", _ssid);
    setCredentials(_ssid, _pass);
  }
}

void NetManager::setCredentials(const char *ssid, const char *password) {
  if (!ssid || !*ssid) return;
  strlcpy(_ssid, ssid, sizeof(_ssid));
  strlcpy(_pass, password ? password : "", sizeof(_pass));

  Preferences prefs;
  prefs.begin(NVS_NS, /*readOnly=*/false);
  prefs.putString(NVS_SSID, _ssid);
  prefs.putString(NVS_PASS, _pass);
  prefs.end();

  /* SSID is fine to log; the password must never appear. */
  LOG_I("wifi: credentials stored for \"%s\"", _ssid);

  _attempts    = 0;
  _nextAttempt = millis();
  _state       = NetState::Down;
  WiFi.disconnect(false, false);
}

void NetManager::forgetCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NS, /*readOnly=*/false);
  prefs.clear();
  prefs.end();
  memset(_ssid, 0, sizeof(_ssid));
  memset(_pass, 0, sizeof(_pass));
  WiFi.disconnect(true, false);
  _state = NetState::Down;
  LOG_I("wifi: credentials erased");
}

void NetManager::startAttempt() {
  if (!hasCredentials()) return;
  _attempts++;
  _state        = NetState::Connecting;
  _attemptStart = millis();
  LOG_I("wifi: connecting to \"%s\" (attempt %u)", _ssid, (unsigned)_attempts);
  WiFi.disconnect(false, false);
  WiFi.begin(_ssid, _pass);
}

int8_t NetManager::rssiToBars(int32_t rssi) {
  /* Thresholds chosen so a typical room-away signal reads 3 bars rather
   * than a misleading 4. */
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void NetManager::sampleRssi() {
  const uint32_t now = millis();
  if (now - _lastRssi < 2000) return;
  _lastRssi = now;
  _bars = rssiToBars(WiFi.RSSI());
}

/* =========================================================================
 *  Asynchronous scanning
 *
 *  WiFi.scanNetworks(async=true) returns immediately and the result is
 *  collected by polling scanComplete(). A synchronous scan blocks for
 *  roughly two seconds, which on this single-task design would freeze the
 *  keyboard mid-keystroke.
 * ====================================================================== */
/* Kick off one scan attempt. Returns false if the driver refused to start
 * it at all, which is a different failure from "scanned and found nothing"
 * and must not be reported as the latter. */
bool NetManager::beginScan() {
  /* The radio cannot associate and scan at the same time. If we are part
   * way through an association attempt, abandon it: the user is standing
   * on the network picker, which means the stored network is not the one
   * they want anyway. A live connection (Up) is left alone - scanning from
   * an associated state works fine. */
  if (_state == NetState::Connecting) {
    LOG_I("wifi: pausing association to scan");
    WiFi.disconnect(false, false);
    _state = NetState::Down;
  }

  WiFi.scanDelete();
  const int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);

  /* scanNetworks() returns WIFI_SCAN_RUNNING when it started. Anything
   * negative and different means it never began - ignoring this was why a
   * failed start showed up as an empty list that Rescan could not fix. */
  if (r != WIFI_SCAN_RUNNING) {
    LOG_E("wifi: could not start scan (%d)", (int)r);
    return false;
  }
  return true;
}

void NetManager::startScan() {
  if (_scanning) return;
  LOG_I("wifi: scanning (attempt %u)", (unsigned)(_scanTries + 1));
  _scanDone   = false;
  _scanFailed = false;

  if (!beginScan()) {
    _scanFailed = true;
    _scanDone   = true;          /* let the UI repaint with the failure */
    _nextScanAt = millis() + SCAN_RETRY_MS;
    return;
  }
  _scanning = true;
}

void NetManager::pollScan() {
  if (!_scanning) return;

  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  _scanning = true;   /* cleared below once results are copied */
  _netCount = 0;

  if (n < 0) {
    LOG_E("wifi: scan failed (%d)", (int)n);
    _scanFailed = true;
    _nextScanAt = millis() + SCAN_RETRY_MS;
  } else {
    /* Results arrive sorted by RSSI. Copy the strongest, skipping
     * duplicate SSIDs - a mesh network advertises the same name from every
     * node, and listing it five times helps nobody. */
    for (int16_t i = 0; i < n && _netCount < MAX_NETS; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;

      bool dup = false;
      for (uint8_t j = 0; j < _netCount; ++j) {
        if (ssid.equals(_nets[j].ssid)) { dup = true; break; }
      }
      if (dup) continue;

      ScannedNet &net = _nets[_netCount];
      strlcpy(net.ssid, ssid.c_str(), sizeof(net.ssid));
      net.bars    = rssiToBars(WiFi.RSSI(i));
      net.secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      _netCount++;
    }
    _scanFailed = false;
    _scanTries  = 0;
    LOG_I("wifi: scan found %u network(s) of %d visible",
          (unsigned)_netCount, (int)n);
    if (n == 0) {
      /* Not an error - but on this chip it almost always means the only
       * networks in range are 5 GHz, which the radio cannot see at all. */
      LOG_I("wifi: nothing visible - note the ESP32-WROOM-32 is 2.4 GHz only");
    }
  }

  WiFi.scanDelete();
  _scanning = false;
  _scanDone = true;
}

/* =========================================================================
 *  State machine
 * ====================================================================== */
void NetManager::service() {
  pollScan();

  /* A scan that failed to start is retried a few times on its own. Without
   * this, one transient refusal leaves an empty list that only a manual
   * Rescan can clear - and the user has no way to know that is what they
   * are looking at. */
  if (_scanFailed && !_scanning && _scanTries < SCAN_MAX_TRIES &&
      (int32_t)(millis() - _nextScanAt) >= 0) {
    _scanTries++;
    startScan();
  }

  const uint32_t now = millis();
  const bool connected = (WiFi.status() == WL_CONNECTED);

  switch (_state) {
    case NetState::Up:
      if (!connected) {
        LOG_E("wifi: link lost");
        _state       = NetState::Down;
        _bars        = 0;
        _ip[0]       = '\0';
        _nextAttempt = now;            /* reconnect immediately */
      } else {
        sampleRssi();
      }
      break;

    case NetState::Connecting:
      if (connected) {
        _state         = NetState::Up;
        _justConnected = true;
        _attempts      = 0;
        sampleRssi();
        strlcpy(_ip, WiFi.localIP().toString().c_str(), sizeof(_ip));
        LOG_I("wifi: up, ip %s rssi %ld dBm", _ip, (long)WiFi.RSSI());
      } else if (now - _attemptStart > WIFI_CONNECT_TIMEOUT_MS) {
        LOG_E("wifi: association timed out");
        WiFi.disconnect(false, false);
        _state       = NetState::Down;
        _nextAttempt = now + WIFI_RETRY_INTERVAL_MS;
      }
      break;

    case NetState::Down:
      /* The ESP32 supplicant retries on its own, so a reassociation may
       * land without us asking. Take it if it does. */
      if (connected) {
        _state         = NetState::Up;
        _justConnected = true;
        sampleRssi();
        strlcpy(_ip, WiFi.localIP().toString().c_str(), sizeof(_ip));
        LOG_I("wifi: recovered, ip %s", _ip);
      } else if (hasCredentials() && (int32_t)(now - _nextAttempt) >= 0) {
        /* Never retry while a scan is in flight: the radio cannot do both,
         * and the scan would be aborted halfway. */
        if (!_scanning) startAttempt();
      }
      break;
  }
}
