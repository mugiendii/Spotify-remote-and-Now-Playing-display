/* =========================================================================
 *  config.example.h  ->  copy to  include/config.h  and adjust to taste.
 *
 *      cp include/config.example.h include/config.h
 *
 *  Nothing in here is secret - it is gitignored alongside secrets.h only so
 *  that your local tuning never collides with an upstream update. Every
 *  value has a working default; you can copy it across untouched.
 *
 *  Hardware pins are NOT here. They live in include/display_config.h.
 * ====================================================================== */
#ifndef CONFIG_H
#define CONFIG_H

/* Sentinel. Several Arduino libraries also ship a "config.h"; this lets the
 * firmware tell "the user's config is missing" apart from "the include path
 * found somebody else's config.h first", which otherwise surfaces as a pile
 * of unrelated errors. */
#define SPOTIFY_TFT_CONFIG_H_OK 1

/* =========================================================================
 *  POLLING AND RATE LIMITS
 *
 *  Spotify enforces a rolling ~30 second rate-limit window and does not
 *  publish the exact quota. One request every few seconds from a single
 *  device is comfortably inside it; the firmware also backs off hard
 *  whenever a 429 arrives and honours the Retry-After header.
 *
 *  Polling slows down when nothing is playing, because an idle account
 *  produces no new information and there is no reason to spend the radio
 *  time or the quota on it.
 * ====================================================================== */
#define POLL_INTERVAL_PLAYING_MS   4000UL   /* while audio is playing      */
#define POLL_INTERVAL_IDLE_MS     12000UL   /* paused / nothing playing    */
#define POLL_INTERVAL_ERROR_MS    15000UL   /* after an API failure        */

/* After sending a transport command, poll again quickly so the UI catches
 * up with reality instead of waiting out a full interval. Spotify needs a
 * moment to apply the change, hence the delay rather than an instant poll. */
#define POLL_AFTER_COMMAND_MS       700UL

/* How long a successful command's intent outranks a contradicting poll.
 *
 * Spotify regularly takes over a second to report a transport change, so
 * without this the fast poll above arrives still saying "playing", undoes
 * the optimistic "paused", and the button looks like it did nothing. The
 * window closes early the moment a poll agrees, so this is a ceiling, not
 * a delay. Raise it if your playback device is slow to report; lower it if
 * a genuinely rejected command lingers on screen. */
#define COMMAND_SETTLE_MS          3000UL

/* How long an explicit device selection is protected after we ask Spotify
 * to transfer. Spotify keeps reporting the previous device for a moment,
 * and releasing the pin inside that window would send the next command
 * straight back to the device the user just moved away from.
 *
 * Longer than this, a differing active device means the user switched
 * devices in the Spotify app, and the pin is dropped so the remote follows
 * them. */
#define TRANSFER_GRACE_MS          8000UL

/* Ceiling on the exponential back-off applied to repeated failures.       */
#define POLL_BACKOFF_MAX_MS      120000UL

/* How often to re-read GET /v1/me/player/devices. Devices appear and
 * disappear far more slowly than tracks change, so this is deliberately
 * lazy; the device screen forces an immediate refresh when you open it. */
#define DEVICE_POLL_INTERVAL_MS   30000UL

/* Refresh the access token this long before it actually expires, so a poll
 * never races the expiry. Spotify issues 3600 s tokens. */
#define TOKEN_REFRESH_MARGIN_S       120

/* =========================================================================
 *  OAUTH
 *
 *  The redirect URI must match a value registered in the Spotify dashboard
 *  EXACTLY, character for character.
 *
 *  Why a loopback address the device cannot serve: Spotify permits plain
 *  HTTP only for loopback redirect URIs ("localhost" is explicitly not
 *  allowed; use the literal 127.0.0.1). The ESP32 sits on a LAN address,
 *  so it can never legally be the redirect target over HTTP.
 *
 *  It does not need to be. The device generates the PKCE verifier, you
 *  authorise in any browser, the browser lands on a dead 127.0.0.1 page -
 *  and the authorisation code is sitting in its address bar. Paste that
 *  URL into the device's setup page and the ESP32 completes the exchange
 *  itself. The "failed" redirect is irrelevant; only the code matters.
 *
 *  USING YOUR OWN DOMAIN INSTEAD
 *  -----------------------------
 *  Any HTTPS URL you control is a valid redirect target, and it buys you a
 *  page that actually renders instead of a browser error. Set it here, in
 *  the Spotify dashboard, and host tools/callback.html at that path - it
 *  shows the code with a copy button.
 *
 *      #define SPOTIFY_REDIRECT_URI "https://example.com/callback"
 *
 *  The three strings must match EXACTLY: this macro, the dashboard entry,
 *  and the served path. A trailing slash is a different URI.
 *
 *  It must be https:// - plain http is accepted only for 127.0.0.1 and
 *  [::1]. The firmware logs an error at boot if this is set to anything
 *  else, rather than letting it surface later as a bare INVALID_CLIENT.
 *
 *  Length is not a constraint: the encode buffers are sized from this
 *  string, so a long domain cannot overflow or silently truncate.
 *
 *  Your web server's access log will contain the authorisation code. That
 *  is harmless under PKCE - the code is single-use, expires in about a
 *  minute, and is worthless without the verifier, which never leaves the
 *  device.
 * ====================================================================== */
#define SPOTIFY_REDIRECT_URI "http://127.0.0.1:8888/callback"

/* Port for the on-device web server: setup pages plus the REST API. */
#define WEB_SERVER_PORT 80

/* =========================================================================
 *  NETWORK TIMEOUTS
 * ====================================================================== */
#define WIFI_CONNECT_TIMEOUT_MS   20000UL   /* per connection attempt      */
#define WIFI_RETRY_INTERVAL_MS    10000UL   /* wait between attempts       */
/* Per API request. This is a UI budget as much as a network one: the whole
 * firmware shares a single task, so a request that hangs freezes touch
 * sampling and redrawing for exactly this long. When that happens it looks
 * precisely like broken touch hardware - buttons do not light up and the
 * screen stops following changes made elsewhere.
 *
 * A poll response is two or three kilobytes, so anything beyond a couple of
 * seconds is already a failure; waiting longer only prolongs the freeze.
 * The loop-time watchdog in main.cpp reports when this bites. */
#define HTTP_TIMEOUT_MS            4000UL
#define ART_HTTP_TIMEOUT_MS       12000UL   /* album-art download          */

/* =========================================================================
 *  TLS CERTIFICATE VERIFICATION
 *
 *  1 = verify the server against the pinned root CAs in src/SpotifyCerts.h
 *  0 = accept any certificate (WiFiClientSecure::setInsecure)
 *
 *  Leave this at 1. Setting it to 0 means anything on your network can
 *  impersonate Spotify and harvest the refresh token in flight.
 *
 *  The honest caveat: pinned roots can go stale. Spotify's three hosts
 *  currently chain to three different roots, and accounts.spotify.com is
 *  served through a cross-signed intermediate that has changed before. All
 *  the bundled roots are valid until 2038+ and each host bundles more than
 *  one acceptable root, but if token refresh starts failing with a TLS
 *  error after years of service, a rotated CA is the first thing to check.
 *  README "Troubleshooting" explains how to re-extract the roots.
 * ====================================================================== */
#define TLS_VERIFY_CERTIFICATES 1

/* =========================================================================
 *  ALBUM ARTWORK
 *
 *  ART_BOX_PX is the on-screen frame. The decoder picks the smallest image
 *  Spotify offers that still fills the box, then uses the JPEG scale factor
 *  (1, 1/2, 1/4, 1/8 - the only ratios libjpeg-style decoders support) that
 *  lands closest under it. The decoded image is centred in the frame with
 *  its aspect ratio preserved.
 *
 *  ART_MAX_BYTES caps the download. A 300x300 Spotify JPEG is 15-35 KB; the
 *  48 KB ceiling leaves headroom for an unusual image while guaranteeing the
 *  buffer can never exhaust the heap that TLS needs. Oversized images are
 *  rejected and the placeholder is shown.
 * ====================================================================== */
#define ART_BOX_PX          160
#define ART_MAX_BYTES     49152UL

/* Bytes pulled from the socket per loop iteration while downloading art.
 * The download is spread across many iterations so touch stays responsive;
 * this is the granularity. Larger = fewer iterations but longer stalls. */
#define ART_CHUNK_BYTES    1460            /* one TCP segment              */

/* =========================================================================
 *  USER INTERFACE
 * ====================================================================== */
/* Marquee scrolling for text too long for its band. */
#define MARQUEE_STEP_MS       33           /* ~30 fps, 1 px per step       */
#define MARQUEE_PAUSE_MS    1500           /* hold at each end             */

/* How long a transient message ("No active device", "Premium required")
 * stays on screen before the line reverts to normal. */
#define MESSAGE_TIMEOUT_MS   6000UL

/* Backlight duty, 0-255. Only has an effect when PIN_TFT_BL is set. */
#define BACKLIGHT_LEVEL       255

/* Volume steps applied by the on-screen slider, in percent. */
#define VOLUME_STEP_PERCENT     5

/* =========================================================================
 *  SERIAL LOGGING
 *
 *  0 = silent, 1 = errors, 2 = + status, 3 = + per-request detail.
 *  Level 3 prints request URLs and response codes but NEVER tokens,
 *  passwords or the refresh token - see logSafe() in src/Log.h.
 * ====================================================================== */
#define LOG_LEVEL 2
#define SERIAL_BAUD 115200

#endif /* CONFIG_H */
