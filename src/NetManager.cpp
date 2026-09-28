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

/* Hard ceilings. A scan is a convenience; connecting is the job. Neither of
 * these should ever be reached, which is exactly why they exist: without
 * them a scan that never completes leaves the radio parked and the device
 * unable to associate with anything, forever. */
constexpr uint32_t SCAN_TIMEOUT_MS = 10000;
constexpr uint32_t PARK_MAX_MS     = 15000;

/* The common esp_wifi disconnect reasons, in plain words. "Not connecting"
 * is not a diagnosis; "auth failed" and "no AP found" are. */
const char *disconnectReason(uint8_t r) {
  switch (r) {
    case 2:   return "auth expired";
    case 4:   return "association expired";
    case 15:  return "4-way handshake timeout (wrong password?)";
    case 200: return "beacon timeout";
    case 201: return "no AP found with that SSID";
    case 202: return "authentication failed (wrong password?)";
    case 203: return "association failed";
    case 204: return "handshake timeout";
    case 205: return "connection failed";
    default:  return "see esp_wifi_types.h";
  }
}

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
  WiFi.onEvent(onWifiEvent);

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

  /* An explicit instruction outranks any background scan; without this a
   * scan parked on the radio silently blocks the connection the user just
   * asked for. */
  cancelScan("credentials changed");

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
/* Hand the radio back to the connection state machine. */
void NetManager::releaseRadio() {
  if (!_radioParked) return;
  _radioParked  = false;
  _parkDeadline = 0;
  WiFi.setAutoReconnect(true);
}

/* Give up on scanning entirely and let the connection state machine have
 * the radio back. Called whenever connecting matters more than listing -
 * which is always, when the user has just told us which network to join. */
void NetManager::cancelScan(const char *why) {
  if (!_scanning && !_scanWanted && !_radioParked) return;
  LOG_I("wifi: abandoning scan (%s)", why);
  WiFi.scanDelete();
  _scanning     = false;
  _scanWanted   = false;
  _scanDeadline = 0;
  releaseRadio();
}

/* Disconnect reasons arrive asynchronously from the driver and are the only
 * place the real cause of a failed association is reported. */
void NetManager::onWifiEvent(arduino_event_id_t event,
                             arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    const uint8_t r = info.wifi_sta_disconnected.reason;
    LOG_I("wifi: disconnected, reason %u (%s)", (unsigned)r, disconnectReason(r));
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    LOG_I("wifi: associated, waiting for an IP address");
  }
}

/* Actually start the scan. Only called once the radio has been parked. */
bool NetManager::beginScan() {
  WiFi.scanDelete();
  const int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);

  /* scanNetworks() returns WIFI_SCAN_RUNNING when it started. Anything
   * else means it never began - ignoring this was why a failed start
   * showed up as an empty list that Rescan could not fix. */
  if (r != WIFI_SCAN_RUNNING) {
    LOG_E("wifi: could not start scan (%d)", (int)r);
    return false;
  }
  return true;
}

void NetManager::startScan() {
  if (_scanning) return;
  _scanWanted = true;
  _scanFailed = false;
  _scanDone   = false;
  _scanTries  = 0;
  _scanAt     = millis();
}

/* -------------------------------------------------------------------------
 *  Scan arbitration.
 *
 *  The radio cannot associate and scan at the same time, and the ESP32
 *  supplicant retries an association on its own whenever autoreconnect is
 *  enabled - so a scan can be started on top of an attempt this state
 *  machine does not even know about, and comes back WIFI_SCAN_FAILED
 *  several seconds later.
 *
 *  That is what produced an endless "scanning -> scan failed (-2) ->
 *  connecting -> scanning" loop on a device holding credentials for a
 *  network that no longer exists: each activity killed the other, forever.
 *
 *  So a scan request first PARKS the radio - autoreconnect off, association
 *  dropped - waits for that to settle, and only then scans. Association is
 *  suppressed until the scan finishes.
 * ---------------------------------------------------------------------- */
void NetManager::serviceScan() {
  /* Enforce the ceilings first, so no scan state can outlive its welcome
   * and strand the radio. */
  if (_scanning && _scanDeadline && (int32_t)(millis() - _scanDeadline) >= 0) {
    _scanFailed = true;
    _scanDone   = true;
    cancelScan("scan timed out");
  }
  if (_radioParked && _parkDeadline &&
      (int32_t)(millis() - _parkDeadline) >= 0) {
    _scanFailed = true;
    _scanDone   = true;
    cancelScan("held the radio too long");
  }

  pollScan();
  if (_scanning || !_scanWanted) return;
  if ((int32_t)(millis() - _scanAt) < 0) return;

  if (!_radioParked) {
    LOG_I("wifi: parking the radio to scan");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    if (_state == NetState::Connecting) _state = NetState::Down;
    _radioParked  = true;
    _parkDeadline = millis() + PARK_MAX_MS;
    /* The disconnect is asynchronous; scanning immediately after it still
     * lands on a busy radio. */
    _scanAt = millis() + 400;
    return;
  }

  LOG_I("wifi: scanning (attempt %u)", (unsigned)(_scanTries + 1));
  if (beginScan()) {
    _scanning     = true;
    _scanWanted   = false;
    _scanDeadline = millis() + SCAN_TIMEOUT_MS;
    return;
  }

  if (++_scanTries >= SCAN_MAX_TRIES) {
    LOG_E("wifi: giving up on scanning after %u attempts", (unsigned)_scanTries);
    _scanWanted = false;
    _scanFailed = true;
    _scanDone   = true;
    releaseRadio();
  } else {
    _scanAt = millis() + SCAN_RETRY_MS;
  }
}

void NetManager::pollScan() {
  if (!_scanning) return;

  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  _scanning = true;   /* cleared below once results are copied */
  _netCount = 0;

  if (n < 0) {
    LOG_E("wifi: scan failed (%d)", (int)n);
    if (++_scanTries < SCAN_MAX_TRIES) {
      /* Go round through the arbitration again rather than firing another
       * scan straight at a radio that has just refused one. */
      _scanning   = false;
      _scanWanted = true;
      _scanAt     = millis() + SCAN_RETRY_MS;
      return;
    }
    _scanFailed = true;
    releaseRadio();
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
  releaseRadio();
}

/* =========================================================================
 *  State machine
 * ====================================================================== */
void NetManager::service() {
  serviceScan();

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
        /* The radio cannot do both. While a scan is pending or running it
         * owns the radio outright; associating underneath it is what made
         * the two starve each other indefinitely. */
        if (!_scanning && !_scanWanted && !_radioParked) startAttempt();
      }
      break;
  }
}
