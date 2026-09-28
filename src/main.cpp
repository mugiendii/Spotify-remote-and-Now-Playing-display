/* =========================================================================
 *  Spotify Now Playing display - ESP32 + SPI TFT
 *
 *  A REMOTE AND A DISPLAY, NOT A PLAYER.
 *  -------------------------------------
 *  This device never downloads, decodes, streams or outputs Spotify audio,
 *  and it is not a Spotify Connect endpoint - it will not appear in
 *  anyone's device list. Audio plays on your phone, TV, computer or
 *  Connect speaker; this shows what is playing there and drives it through
 *  the Spotify Web API. Commands go to the device you pick on the device
 *  screen, or to whichever device Spotify currently reports as active.
 *
 *  CONCURRENCY
 *  -----------
 *  There is none, by design. Network, parsing, rendering, input and the
 *  web server all run on the Arduino loop task. That is a deliberate
 *  choice, not a shortcut: PlaybackState needs no mutex, the shared VSPI
 *  bus has exactly one user, and there is no way for a display write and
 *  an HTTP read to interleave. The cost is that slow work must be broken
 *  up rather than blocked on, which is why the artwork download is a state
 *  machine, the web API queues rather than calls, and every schedule is a
 *  millis() deadline. There is not one delay() in the run loop.
 * ====================================================================== */
#include <Arduino.h>

#include "AlbumArt.h"
#include "ApiServer.h"
#include "AppState.h"
#include "DisplayManager.h"
#include "InputManager.h"
#include "Log.h"
#include "NetManager.h"
#include "SpotifyClient.h"
#include "UiController.h"
#include "config.h"

/* mbedTLS chain verification is stack-hungry and the Arduino default of
 * 8 KB leaves too little headroom once JSON parsing, JPEG decoding and the
 * web server share the same task. This macro is the supported way to raise
 * it; a -D flag is silently overridden by the SDK's own sdkconfig.h. */
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static DisplayManager display;
static NetManager     net;
static SpotifyClient  spotify;
static AlbumArt       albumArt;
static InputManager   input;
static UiController   ui;
static ApiServer      api;

static PlaybackState  state;
static AppStatus      status;

/* Heap watchdog: a slow leak on an embedded device shows up as a reboot
 * days later, which is miserable to debug. Logging the floor every 30 s
 * turns that into a visible trend. */
static void logHeap() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 30000UL) return;
  last = now;
  LOG_I("heap: free %u, min-ever %u, largest block %u",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap());
}

/* Artwork is fetched only when the API is quiet. Two simultaneous TLS
 * sessions would need ~40 KB of heap at once; releasing the kept-alive API
 * socket first keeps the peak to one. */
static void serviceArtwork() {
  if (albumArt.busy()) {
    albumArt.service();
    status.artBusy = albumArt.busy();
    return;
  }
  status.artBusy = false;

  /* Nothing to show, or the art box is hidden behind another screen. */
  if (ui.overlayActive()) return;
  if (!state.hasItem || !state.artUrl[0]) {
    albumArt.request(nullptr, nullptr);
    return;
  }

  /* Do not start a transfer on top of a queued command - the user is
   * waiting on that, and it needs the API socket. */
  if (spotify.busy()) return;

  /* Ask before acting: a cache hit must not cost us the kept-alive API
   * connection, and only a real fetch justifies dropping it. */
  if (!albumArt.needsFetch(state.artKey, state.artUrl)) return;

  spotify.releaseConnection();
  albumArt.request(state.artKey, state.artUrl);
  status.artBusy = albumArt.busy();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(100);                       /* let the USB-serial bridge settle   */
  Serial.println();
  LOG_I("=== Spotify Now Playing (remote / display) ===");
  /* The fingerprint identifies which display_config.h this image was built
   * from - the quickest way to confirm a board is running the pin map you
   * think it is. Injected by tools/pio_config_fingerprint.py. */
  LOG_I("panel %dx%d, cfg 0x%08x, free heap %u", SCREEN_W, SCREEN_H,
        (unsigned)DISPLAY_CONFIG_FINGERPRINT, (unsigned)ESP.getFreeHeap());

  display.begin();
  display.showBootScreen("starting");

  /* Reserve the big buffers before the network stack fragments the heap. */
  albumArt.begin(&display);
  input.begin();

  net.begin();
  spotify.begin(&state, &status);
  ui.begin(&display, &spotify, &net, &albumArt, &state, &status);
  api.begin(&spotify, &state, &status, &net);
}

void loop() {
  /* 1. Network first: everything downstream depends on it. */
  net.service();
  status.net      = net.state();
  status.rssiBars = net.bars();
  status.authed   = spotify.authorised();
  strlcpy(status.ip, net.ip(), sizeof(status.ip));

  if (net.consumeJustConnected()) {
    spotify.onNetworkUp();
    albumArt.invalidate();          /* re-fetch art after a reconnect */
    api.start();                    /* the listener needs an address  */
    status.setMessage("Wi-Fi connected", 2500);
  }
  if (!net.isUp() && api.running()) api.stop();

  /* 2. Input, sampled every iteration so it always feels immediate. */
  ui.handleTouch(input.pollTouch());
  ui.handleButton(input.pollButtons());

  /* 3. One Spotify transaction at most, on its own schedule. */
  spotify.service(net.isUp());

  /* 4. Artwork, a TCP segment at a time. */
  if (net.isUp()) serviceArtwork();

  /* 5. Serve at most one web request. */
  api.service();

  /* 6. Repaint whatever changed on whichever screen is showing. */
  status.expireMessage();
  ui.render();

  /* A full repaint erases the art box. Drop the cache so the cover comes
   * back instead of leaving an empty frame until the next album. */
  if (display.consumeArtInvalidated()) albumArt.invalidate();

  logHeap();
}
