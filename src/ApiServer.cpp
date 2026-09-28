#include "ApiServer.h"

#include <ArduinoJson.h>

#include "Log.h"
#include "NetManager.h"

/* =========================================================================
 *  The setup page.
 *
 *  Held in PROGMEM as one string: no filesystem partition to flash, no
 *  SPIFFS/LittleFS image to keep in sync with the firmware, and nothing
 *  that can be left stale by an OTA. The cost is that it is not pretty to
 *  edit; the benefit is that `pio run -t upload` is the whole deployment.
 * ====================================================================== */
static const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Now Playing - setup</title>
<style>
 :root{color-scheme:dark}
 body{margin:0;background:#0b0c0b;color:#eee;font:15px/1.5 system-ui,sans-serif}
 .wrap{max-width:640px;margin:0 auto;padding:24px 16px}
 h1{font-size:20px;margin:0 0 4px}
 h2{font-size:15px;margin:28px 0 8px;color:#2ed15c}
 p{color:#aaa;margin:8px 0}
 .card{background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:16px;margin:12px 0}
 input{width:100%;box-sizing:border-box;padding:10px;border-radius:7px;
       border:1px solid #444;background:#111;color:#eee;font:14px monospace}
 button{background:#2ed15c;color:#06210f;border:0;border-radius:7px;
        padding:10px 16px;font-weight:600;cursor:pointer;margin-top:10px}
 button.sec{background:#333;color:#eee}
 a{color:#2ed15c;word-break:break-all}
 code{background:#111;padding:2px 5px;border-radius:4px}
 .ok{color:#2ed15c}.bad{color:#f56}.muted{color:#777;font-size:13px}
 ol{color:#aaa;padding-left:20px}
</style></head><body><div class="wrap">
<h1>Now Playing</h1>
<p class="muted">Spotify remote &amp; display &middot; <span id="st">loading...</span></p>

<div class="card" id="setup">
 <h2>1. Spotify app</h2>
 <p>Create an app at <a href="https://developer.spotify.com/dashboard" target="_blank" rel="noreferrer">developer.spotify.com/dashboard</a>
    and add this exact redirect URI:</p>
 <p><code id="redir">http://127.0.0.1:8888/callback</code></p>
 <input id="cid" placeholder="Client ID" autocomplete="off" spellcheck="false">
 <button onclick="saveCid()">Save client ID</button>

 <h2>2. Authorise</h2>
 <p>Open the link, approve access, then copy the address bar of the page you
    land on &mdash; it will fail to load, which is expected &mdash; and paste
    it below.</p>
 <p><a id="auth" href="#" target="_blank" rel="noreferrer">Generating link...</a></p>
 <input id="paste" placeholder="http://127.0.0.1:8888/callback?code=..." autocomplete="off" spellcheck="false">
 <button onclick="finish()">Complete sign-in</button>
 <p id="msg"></p>
</div>

<div class="card">
 <h2>Playback</h2>
 <p id="np">-</p>
 <button class="sec" onclick="cmd('previous')">Prev</button>
 <button class="sec" onclick="cmd('play')">Play</button>
 <button class="sec" onclick="cmd('pause')">Pause</button>
 <button class="sec" onclick="cmd('next')">Next</button>
 <p><a href="/api/spotify/devices">/api/spotify/devices</a> &middot;
    <a href="/api/spotify/currently-playing">/api/spotify/currently-playing</a></p>
 <button class="sec" onclick="signout()">Sign out of Spotify</button>
</div>

<script>
const $=id=>document.getElementById(id);
async function refresh(){
 try{
  const s=await (await fetch('/api/status')).json();
  $('st').innerHTML = (s.wifi?'Wi-Fi '+s.ssid:'<span class=bad>no Wi-Fi</span>')
    +' &middot; '+(s.authed?'<span class=ok>signed in</span>'
                           :'<span class=bad>not signed in</span>');
  $('redir').textContent=s.redirect_uri;
  if(s.client_id) $('cid').value=s.client_id;
  const p=await (await fetch('/api/spotify/currently-playing')).json();
  $('np').textContent = p.item ? (p.item.name+' - '+p.item.artist+
      (p.device?'  |  on '+p.device.name:'')) : 'Nothing playing';
 }catch(e){ $('st').textContent='device unreachable'; }
}
async function saveCid(){
 const r=await fetch('/api/client-id',{method:'PUT',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'client_id='+encodeURIComponent($('cid').value.trim())});
 $('msg').textContent = r.ok?'Client ID saved.':'Could not save client ID.';
 link();
}
async function link(){
 const r=await fetch('/api/auth/start',{method:'POST'});
 const j=await r.json();
 if(j.url){ $('auth').href=j.url; $('auth').textContent='Authorise with Spotify'; }
 else { $('auth').textContent=j.error||'Set a client ID first'; $('auth').href='#'; }
}
async function finish(){
 $('msg').textContent='Exchanging...';
 const r=await fetch('/api/auth/complete',{method:'POST',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'redirect_url='+encodeURIComponent($('paste').value.trim())});
 const j=await r.json();
 $('msg').innerHTML = j.ok ? '<span class=ok>Signed in.</span>'
                           : '<span class=bad>'+(j.error||'Failed')+'</span>';
 refresh();
}
async function signout(){
 await fetch('/api/auth/signout',{method:'POST'}); refresh();
}
async function cmd(c){
 await fetch('/api/spotify/'+c,{method:'POST'});
 setTimeout(refresh,900);
}
refresh(); link(); setInterval(refresh,5000);
</script>
</div></body></html>)HTML";

/* =========================================================================
 *  Lifecycle
 * ====================================================================== */
void ApiServer::begin(SpotifyClient *spotify, PlaybackState *state,
                      AppStatus *status, NetManager *net) {
  _spotify = spotify;
  _state   = state;
  _status  = status;
  _net     = net;
}

void ApiServer::start() {
  if (_running) return;
  if (!_routed) {
    routes();
    _routed = true;
  }
  _server.begin();
  _running = true;
  LOG_I("http: listening on port %d", (int)WEB_SERVER_PORT);
}

void ApiServer::stop() {
  if (!_running) return;
  _server.stop();
  _running = false;
  LOG_I("http: stopped");
}

void ApiServer::service() {
  if (_running) _server.handleClient();
}

/* =========================================================================
 *  Helpers
 * ====================================================================== */
void ApiServer::sendJson(int code, const String &body) {
  /* The setup page is served from the device itself, but allowing any
   * origin means you can also drive the API from a script or another page
   * on your own machine. There is no authentication to leak - see the
   * header comment about the trust boundary. */
  _server.sendHeader("Access-Control-Allow-Origin", "*");
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(code, "application/json", body);
}

void ApiServer::sendAccepted(const char *what) {
  /* 202, not 200: the command has been queued, not performed. Spotify has
   * not been told yet, and it may still refuse. */
  String b = "{\"accepted\":true,\"command\":\"";
  b += what;
  b += "\"}";
  sendJson(202, b);
}

bool ApiServer::requireAuth() {
  if (_spotify->authorised()) return true;
  sendJson(401, F("{\"error\":\"not_signed_in\",\"detail\":"
                  "\"Open the setup page and sign in to Spotify\"}"));
  return false;
}

bool ApiServer::param(const char *name, String &out) {
  if (!_server.hasArg(name)) return false;
  out = _server.arg(name);
  return out.length() > 0;
}

bool ApiServer::paramInt(const char *name, long &out) {
  String s;
  if (!param(name, s)) return false;
  char *end = nullptr;
  const long v = strtol(s.c_str(), &end, 10);
  if (end == s.c_str()) return false;      /* not a number at all */
  out = v;
  return true;
}

/* =========================================================================
 *  Routing
 * ====================================================================== */
void ApiServer::routes() {
  _server.on("/", HTTP_GET, [this]() { handleRoot(); });
  _server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  _server.on("/api/auth/start", HTTP_POST, [this]() { handleAuthStart(); });
  _server.on("/api/auth/complete", HTTP_POST, [this]() { handleAuthComplete(); });
  _server.on("/api/auth/signout", HTTP_POST, [this]() { handleSignOut(); });
  _server.on("/api/client-id", HTTP_PUT, [this]() { handleClientId(); });
  _server.on("/api/client-id", HTTP_POST, [this]() { handleClientId(); });

  /* ---- the simplified device + playback API ---- */
  _server.on("/api/spotify/devices", HTTP_GET, [this]() { handleDevices(); });
  _server.on("/api/spotify/active-device", HTTP_PUT, [this]() { handleActiveDevice(); });
  _server.on("/api/spotify/currently-playing", HTTP_GET,
             [this]() { handleCurrentlyPlaying(); });
  _server.on("/api/spotify/play", HTTP_POST, [this]() { handlePlay(); });
  _server.on("/api/spotify/pause", HTTP_POST, [this]() { handlePause(); });
  _server.on("/api/spotify/next", HTTP_POST, [this]() { handleNext(); });
  _server.on("/api/spotify/previous", HTTP_POST, [this]() { handlePrevious(); });
  _server.on("/api/spotify/seek", HTTP_PUT, [this]() { handleSeek(); });
  _server.on("/api/spotify/volume", HTTP_PUT, [this]() { handleVolume(); });
  _server.on("/api/spotify/shuffle", HTTP_PUT, [this]() { handleShuffle(); });
  _server.on("/api/spotify/repeat", HTTP_PUT, [this]() { handleRepeat(); });

  /* Browsers preflight the PUTs from the setup page's fetch calls. */
  _server.onNotFound([this]() {
    if (_server.method() == HTTP_OPTIONS) {
      _server.sendHeader("Access-Control-Allow-Origin", "*");
      _server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS");
      _server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      _server.send(204);
      return;
    }
    sendJson(404, F("{\"error\":\"not_found\"}"));
  });
}

/* =========================================================================
 *  Setup pages
 * ====================================================================== */
void ApiServer::handleRoot() {
  _server.sendHeader("Cache-Control", "no-store");
  _server.send_P(200, "text/html", PAGE_INDEX);
}

void ApiServer::handleStatus() {
  JsonDocument doc;
  doc["wifi"]         = _net && _net->isUp();
  doc["ssid"]         = _net ? _net->ssid() : "";
  doc["ip"]           = _net ? _net->ip() : "";
  doc["authed"]       = _spotify->authorised();
  doc["client_id"]    = _spotify->clientId();
  doc["redirect_uri"] = SPOTIFY_REDIRECT_URI;
  doc["device_count"] = _spotify->devices().count;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void ApiServer::handleAuthStart() {
  char url[SpotifyClient::AUTH_URL_CAP];
  if (!_spotify->buildAuthUrl(url, sizeof(url))) {
    /* Two causes, and they need different fixes, so do not guess: the
     * serial log names which one it was. */
    sendJson(400, F("{\"error\":\"Could not build the sign-in link - set a "
                    "client ID, or check SPOTIFY_REDIRECT_URI is not too "
                    "long (see the serial log)\"}"));
    return;
  }
  JsonDocument doc;
  doc["url"] = url;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void ApiServer::handleAuthComplete() {
  String pasted;
  if (!param("redirect_url", pasted) && !param("code", pasted)) {
    sendJson(400, F("{\"ok\":false,\"error\":\"Nothing pasted\"}"));
    return;
  }

  /* This is the one handler that blocks on the network. It runs once,
   * during setup, when no poll or artwork download is in flight. */
  char err[96] = {0};
  const bool ok = _spotify->completeAuth(pasted.c_str(), err, sizeof(err));

  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok) doc["error"] = err;
  String out;
  serializeJson(doc, out);
  sendJson(ok ? 200 : 400, out);
}

void ApiServer::handleSignOut() {
  _spotify->signOut();
  sendJson(200, F("{\"ok\":true}"));
}

void ApiServer::handleClientId() {
  String id;
  if (!param("client_id", id)) {
    sendJson(400, F("{\"error\":\"client_id required\"}"));
    return;
  }
  _spotify->setClientId(id.c_str());
  sendJson(200, F("{\"ok\":true}"));
}

/* =========================================================================
 *  /api/spotify/ endpoints
 * ====================================================================== */
void ApiServer::handleDevices() {
  if (!requireAuth()) return;

  /* Answer from cache and nudge a refresh, so a client that polls this
   * endpoint gets fresher data next time without ever blocking on us. */
  _spotify->refreshDevicesSoon();

  const DeviceList &list = _spotify->devices();
  JsonDocument doc;
  doc["stale_ms"] = list.updatedAt ? (millis() - list.updatedAt) : 0;
  doc["selected"] = _spotify->targetDevice();
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (uint8_t i = 0; i < list.count; ++i) {
    const SpotifyDevice &d = list.items[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"]              = d.id;
    o["name"]            = d.name;
    o["type"]            = d.type;
    o["is_active"]       = d.isActive;
    o["is_restricted"]   = d.isRestricted;
    o["supports_volume"] = d.supportsVolume;
    if (d.volumePercent >= 0) o["volume_percent"] = d.volumePercent;
    else                      o["volume_percent"] = nullptr;
  }
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void ApiServer::handleActiveDevice() {
  if (!requireAuth()) return;
  String id;
  if (!param("device_id", id)) {
    sendJson(400, F("{\"error\":\"device_id required\"}"));
    return;
  }
  /* Default to keeping audio running, which is what "transfer" almost
   * always means; ?play=false parks it instead. */
  String playArg;
  bool play = true;
  if (param("play", playArg)) {
    play = !(playArg == "false" || playArg == "0");
  }
  _spotify->cmdTransfer(id.c_str(), play);
  sendAccepted("active-device");
}

void ApiServer::handleCurrentlyPlaying() {
  if (!requireAuth()) return;

  JsonDocument doc;
  doc["is_playing"] = _state->isPlaying;
  doc["progress_ms"] = _state->progressNow();
  doc["shuffle"] = _state->shuffle;
  doc["repeat"] = repeatToApi(_state->repeat);

  if (_state->hasDevice) {
    JsonObject d = doc["device"].to<JsonObject>();
    d["id"]              = _state->deviceId;
    d["name"]            = _state->device;
    d["type"]            = _state->deviceType;
    d["supports_volume"] = _state->supportsVolume;
    if (_state->volumePercent >= 0) d["volume_percent"] = _state->volumePercent;
    else                            d["volume_percent"] = nullptr;
  } else {
    doc["device"] = nullptr;
  }

  if (_state->hasItem) {
    JsonObject it = doc["item"].to<JsonObject>();
    it["id"]          = _state->itemId;
    it["type"]        = (_state->kind == ItemKind::Episode) ? "episode" : "track";
    it["name"]        = _state->title;
    it["artist"]      = _state->artist;
    it["album"]       = _state->album;
    it["duration_ms"] = _state->durationMs;
    it["image"]       = _state->artUrl;
  } else {
    doc["item"] = nullptr;
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void ApiServer::handlePlay() {
  if (!requireAuth()) return;
  _spotify->cmdPlay();
  sendAccepted("play");
}

void ApiServer::handlePause() {
  if (!requireAuth()) return;
  _spotify->cmdPause();
  sendAccepted("pause");
}

void ApiServer::handleNext() {
  if (!requireAuth()) return;
  _spotify->cmdNext();
  sendAccepted("next");
}

void ApiServer::handlePrevious() {
  if (!requireAuth()) return;
  _spotify->cmdPrevious();
  sendAccepted("previous");
}

void ApiServer::handleSeek() {
  if (!requireAuth()) return;
  long pos = 0;
  if (!paramInt("position_ms", pos) || pos < 0) {
    sendJson(400, F("{\"error\":\"position_ms required (>= 0)\"}"));
    return;
  }
  _spotify->cmdSeek((uint32_t)pos);
  sendAccepted("seek");
}

void ApiServer::handleVolume() {
  if (!requireAuth()) return;
  long v = 0;
  if (!paramInt("volume_percent", v)) {
    sendJson(400, F("{\"error\":\"volume_percent required (0-100)\"}"));
    return;
  }
  if (v < 0 || v > 100) {
    sendJson(400, F("{\"error\":\"volume_percent must be 0-100\"}"));
    return;
  }
  if (!_state->supportsVolume) {
    sendJson(409, F("{\"error\":\"device_no_volume\",\"detail\":"
                    "\"The active device does not support remote volume\"}"));
    return;
  }
  _spotify->cmdVolume((int16_t)v);
  sendAccepted("volume");
}

void ApiServer::handleShuffle() {
  if (!requireAuth()) return;
  String s;
  if (!param("state", s)) {
    sendJson(400, F("{\"error\":\"state required (true|false)\"}"));
    return;
  }
  _spotify->cmdShuffle(s == "true" || s == "1");
  sendAccepted("shuffle");
}

void ApiServer::handleRepeat() {
  if (!requireAuth()) return;
  String s;
  if (!param("state", s)) {
    sendJson(400, F("{\"error\":\"state required (off|context|track)\"}"));
    return;
  }
  if (s != "off" && s != "context" && s != "track") {
    sendJson(400, F("{\"error\":\"state must be off, context or track\"}"));
    return;
  }
  _spotify->cmdRepeat(repeatFromApi(s.c_str()));
  sendAccepted("repeat");
}
