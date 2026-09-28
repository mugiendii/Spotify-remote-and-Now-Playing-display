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
 <h2>Wi-Fi</h2>
 <p class="muted">Usable when the touchscreen is not. Switching networks will
    drop this page &mdash; reconnect to the new address the device shows.</p>
 <p id="wnow">-</p>
 <select id="wlist" style="width:100%;padding:10px;border-radius:7px;
    border:1px solid #444;background:#111;color:#eee;font:14px system-ui">
   <option value="">Scanning...</option></select>
 <input id="wpass" type="password" placeholder="Password (blank if open)"
        autocomplete="off" style="margin-top:10px">
 <button onclick="wjoin()">Join network</button>
 <button class="sec" onclick="wscan()">Rescan</button>
 <p id="wmsg" class="muted"></p>
</div>

<div class="card">
 <h2>Now playing</h2>
 <p id="np">-</p>
 <p class="muted" id="npdev">-</p>

 <div id="seekwrap">
   <input id="seek" type="range" min="0" max="1000" value="0" style="width:100%">
   <p class="muted"><span id="tpos">0:00</span> / <span id="tdur">0:00</span></p>
 </div>

 <button class="sec" onclick="cmd('previous')">&#9664;&#9664;</button>
 <button onclick="cmd('play')">&#9654; Play</button>
 <button class="sec" onclick="cmd('pause')">&#10073;&#10073; Pause</button>
 <button class="sec" onclick="cmd('next')">&#9654;&#9654;</button>
 <br>
 <button class="sec" id="bshuf" onclick="toggleShuffle()">Shuffle: ?</button>
 <button class="sec" id="brep" onclick="cycleRepeat()">Repeat: ?</button>

 <p class="muted" style="margin-top:16px">Volume <span id="vpct">-</span></p>
 <input id="vol" type="range" min="0" max="100" value="50" style="width:100%">
 <p id="pmsg" class="muted"></p>
</div>

<div class="card">
 <h2>Playback device</h2>
 <p class="muted">Pick where Spotify should play. This is the device-selection
    screen, usable without the touchscreen.</p>
 <label class="muted"><input type="checkbox" id="keep" checked>
   Keep playing during transfer</label>
 <div id="devs"><p class="muted">Loading...</p></div>
 <button class="sec" onclick="loadDevs()">Refresh devices</button>
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
function mmss(ms){
 const t=Math.floor(ms/1000), m=Math.floor(t/60), s=t%60;
 return m+':'+String(s).padStart(2,'0');
}
let dragging=false, vdragging=false, state={};
$('seek').addEventListener('input',()=>{dragging=true;});
$('seek').addEventListener('change',async e=>{
 dragging=false;
 if(!state.item) return;
 const ms=Math.round(state.item.duration_ms*(e.target.value/1000));
 await put('/api/spotify/seek?position_ms='+ms);
});
$('vol').addEventListener('input',()=>{vdragging=true; $('vpct').textContent=$('vol').value+'%';});
$('vol').addEventListener('change',async e=>{
 vdragging=false;
 await put('/api/spotify/volume?volume_percent='+e.target.value);
});

async function put(url){
 try{
  const r=await fetch(url,{method:'PUT'});
  const j=await r.json().catch(()=>({}));
  $('pmsg').textContent = r.ok ? '' : (j.detail||j.error||('HTTP '+r.status));
  setTimeout(np,900);
  return r.ok;
 }catch(e){ $('pmsg').textContent='device unreachable'; return false; }
}
async function cmd(c){
 try{
  const r=await fetch('/api/spotify/'+c,{method:'POST'});
  const j=await r.json().catch(()=>({}));
  $('pmsg').textContent = r.ok ? '' : (j.detail||j.error||('HTTP '+r.status));
 }catch(e){ $('pmsg').textContent='device unreachable'; }
 setTimeout(np,900);
}
async function toggleShuffle(){ await put('/api/spotify/shuffle?state='+(state.shuffle?'false':'true')); }
async function cycleRepeat(){
 const order={off:'context',context:'track',track:'off'};
 await put('/api/spotify/repeat?state='+(order[state.repeat]||'context'));
}

async function np(){
 try{
  const p=await (await fetch('/api/spotify/currently-playing')).json();
  state=p;
  if(p.item){
   $('np').textContent=p.item.name+' - '+p.item.artist;
   $('npdev').textContent=(p.item.album||'')+(p.is_playing?'  |  playing':'  |  paused');
   $('tdur').textContent=mmss(p.item.duration_ms);
   $('tpos').textContent=mmss(p.progress_ms);
   if(!dragging && p.item.duration_ms)
     $('seek').value=Math.round(1000*p.progress_ms/p.item.duration_ms);
  } else {
   $('np').textContent='Nothing playing';
   $('npdev').textContent='';
  }
  if(p.device){
   $('npdev').textContent += '  |  on '+p.device.name;
   if(!vdragging && p.device.volume_percent!==null){
     $('vol').value=p.device.volume_percent;
     $('vpct').textContent=p.device.volume_percent+'%';
   }
   $('vol').disabled = !p.device.supports_volume;
   if(!p.device.supports_volume) $('vpct').textContent='not supported';
  } else {
   $('npdev').textContent='No active Spotify device. Open Spotify on your phone, TV or computer.';
  }
  $('bshuf').textContent='Shuffle: '+(p.shuffle?'on':'off');
  $('brep').textContent='Repeat: '+(p.repeat||'off');
 }catch(e){ /* left alone; the status line reports reachability */ }
}

async function loadDevs(){
 try{
  const d=await (await fetch('/api/spotify/devices')).json();
  if(!d.devices || !d.devices.length){
   $('devs').innerHTML='<p class="muted">No Spotify devices found. '+
     'Open Spotify on your phone, TV or computer.</p>';
   return;
  }
  $('devs').innerHTML = d.devices.map(v => {
   const flags=[v.type];
   if(v.volume_percent!==null) flags.push('vol '+v.volume_percent+'%');
   if(v.is_restricted) flags.push('restricted');
   else if(!v.supports_volume) flags.push('no remote volume');
   const btn = v.is_restricted
     ? '<button class="sec" disabled>restricted</button>'
     : '<button class="sec" onclick="useDev(\''+v.id+'\')">'+
       (v.is_active?'active':'Use this')+'</button>';
   return '<p>'+(v.is_active?'&#9679; ':'&#9675; ')+'<strong>'+v.name+'</strong><br>'+
          '<span class="muted">'+flags.join(' &middot; ')+'</span><br>'+btn+'</p>';
  }).join('');
 }catch(e){ $('devs').innerHTML='<p class="bad">Could not read devices.</p>'; }
}
async function useDev(id){
 await put('/api/spotify/active-device?device_id='+encodeURIComponent(id)+
           '&play='+($('keep').checked?'true':'false'));
 setTimeout(loadDevs,1200);
}

let wtimer=null;
async function wscan(){
 $('wmsg').textContent='Scanning...';
 try{
  const w=await (await fetch('/api/wifi/scan')).json();
  $('wnow').textContent = w.connected
    ? 'Connected to '+w.current+'  (signal '+w.bars+'/4)'
    : (w.current ? 'Saved: '+w.current+' (not connected)' : 'No network saved');
  const sel=$('wlist');
  if(w.networks.length){
   sel.innerHTML = w.networks.map(n =>
     '<option value="'+n.ssid.replace(/"/g,'&quot;')+'">'+n.ssid+
     '  ('+n.bars+'/4'+(n.secured?'':', open')+')</option>').join('');
   $('wmsg').textContent='';
  } else {
   /* value="" on purpose: an <option> with no value attribute returns its
      LABEL from .value, so a placeholder would be submitted as an SSID.
      That is exactly how a device once ended up trying to associate with a
      network called "No 2.4 GHz networks found". */
   sel.innerHTML='<option value="">'+
     (w.failed?'Scan failed - retrying':'No 2.4 GHz networks found')+'</option>';
  }
  /* The scan is asynchronous on the device; poll until it settles. */
  if(w.scanning){ clearTimeout(wtimer); wtimer=setTimeout(wscan,1500); }
 }catch(e){ $('wmsg').textContent='Could not reach the device.'; }
}
async function wjoin(){
 const ssid=$('wlist').value;
 /* Every placeholder carries value="", so this one check covers all of
    them - scanning, scan-failed and nothing-found alike. */
 if(!ssid){ $('wmsg').textContent='Pick a real network first.'; return; }
 $('wmsg').textContent='Joining '+ssid+'...';
 try{
  const r=await fetch('/api/wifi',{method:'PUT',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent($('wpass').value)});
  const j=await r.json();
  $('wmsg').textContent = j.note || 'Joining...';
 }catch(e){
  /* Expected: the socket dies with the old association. */
  $('wmsg').textContent='Joining - this page has lost contact, which is normal.';
 }
 $('wpass').value='';
}
refresh(); link(); wscan(); np(); loadDevs();
setInterval(refresh,5000); setInterval(np,2000);
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

  /* Applied here rather than in the handler so the HTTP response is already
   * on the wire before the association drops. */
  if (_wifiChangePending) {
    _wifiChangePending = false;
    LOG_I("http: switching Wi-Fi to \"%s\"", _pendingSsid);
    if (_net) _net->setCredentials(_pendingSsid, _pendingPass);
    memset(_pendingPass, 0, sizeof(_pendingPass));
  }
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

  /* Wi-Fi, so a device whose touchscreen is unreachable can still be moved
   * to another network. */
  _server.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScan(); });
  _server.on("/api/wifi", HTTP_PUT, [this]() { handleWifiSet(); });
  _server.on("/api/wifi", HTTP_POST, [this]() { handleWifiSet(); });
  _server.on("/api/wifi/forget", HTTP_POST, [this]() { handleWifiForget(); });

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
 *  Wi-Fi
 * ====================================================================== */
void ApiServer::handleWifiScan() {
  if (!_net) {
    sendJson(500, F("{\"error\":\"no network manager\"}"));
    return;
  }
  /* Scanning is asynchronous and takes a few seconds. Kick one off if none
   * is running and answer with whatever is currently known, so the page can
   * poll rather than hold a connection open across a scan. */
  if (!_net->scanning()) _net->startScan();

  JsonDocument doc;
  doc["scanning"]   = _net->scanning();
  doc["failed"]     = _net->scanFailed();
  doc["current"]    = _net->ssid();
  doc["connected"]  = _net->isUp();
  doc["bars"]       = _net->bars();
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (uint8_t i = 0; i < _net->networkCount(); ++i) {
    const ScannedNet &n = _net->networks()[i];
    JsonObject o = arr.add<JsonObject>();
    o["ssid"]    = n.ssid;
    o["bars"]    = n.bars;
    o["secured"] = n.secured;
  }
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void ApiServer::handleWifiSet() {
  String ssid, pass;
  if (!param("ssid", ssid)) {
    sendJson(400, F("{\"error\":\"ssid required\"}"));
    return;
  }
  param("password", pass);       /* absent or empty = open network */

  if (ssid.length() > 32) {
    sendJson(400, F("{\"error\":\"ssid longer than 32 characters\"}"));
    return;
  }
  if (pass.length() > 63) {
    sendJson(400, F("{\"error\":\"password longer than 63 characters\"}"));
    return;
  }

  /* Second line of defence. A UI slip should not be able to strand the
   * device on a network that does not exist - which, with no working touch
   * panel, needs a serial cable to undo. `force=1` overrides, for hidden
   * SSIDs that a scan can never list. */
  String force;
  const bool forced = param("force", force) && (force == "1" || force == "true");
  if (!forced && _net) {
    bool seen = false;
    for (uint8_t i = 0; i < _net->networkCount(); ++i) {
      if (ssid == _net->networks()[i].ssid) { seen = true; break; }
    }
    if (!seen) {
      sendJson(409, F("{\"error\":\"ssid_not_visible\",\"detail\":"
                      "\"That network was not in the last scan. Rescan, or "
                      "resend with force=1 if it is hidden.\"}"));
      return;
    }
  }

  strlcpy(_pendingSsid, ssid.c_str(), sizeof(_pendingSsid));
  strlcpy(_pendingPass, pass.c_str(), sizeof(_pendingPass));
  _wifiChangePending = true;

  /* Say plainly that this connection is about to die - a browser error
   * after this point is expected, not a failure. */
  sendJson(200, F("{\"ok\":true,\"note\":\"Joining the new network. This page "
                  "will lose contact; reconnect your browser to the new "
                  "address shown on the device.\"}"));
}

void ApiServer::handleWifiForget() {
  if (_net) _net->forgetCredentials();
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
