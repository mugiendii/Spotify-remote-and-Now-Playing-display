/* =========================================================================
 *  secrets.example.h  ->  copy to  include/secrets.h  and fill in.
 *
 *      cp include/secrets.example.h include/secrets.h
 *
 *  include/secrets.h is listed in .gitignore and must never be committed.
 *
 *  WHY THERE IS NO CLIENT SECRET HERE
 *  ----------------------------------
 *  This firmware uses the Authorization Code + PKCE flow. PKCE was designed
 *  precisely for clients that cannot keep a secret - a refresh against
 *  accounts.spotify.com needs only the client ID, so no client secret is
 *  ever compiled into the image.
 *
 *  This matters: anyone with a few dollars of hardware can dump the flash of
 *  an ESP32 over UART. A client secret in firmware is a published client
 *  secret. If you ever find a flow that demands one, use the token-broker
 *  approach described in README.md ("Security model") instead of pasting it
 *  here.
 *
 *  The refresh token below is still sensitive - it grants ongoing access to
 *  your playback. It is a per-device credential: revoke it at
 *  https://www.spotify.com/account/apps/ if a device is lost.
 * ====================================================================== */
#ifndef SECRETS_H
#define SECRETS_H

/* -------------------------------------------------------------------------
 *  Wi-Fi. 2.4 GHz only - the ESP32-WROOM-32 has no 5 GHz radio.
 * ---------------------------------------------------------------------- */
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

/* -------------------------------------------------------------------------
 *  Spotify application credentials.
 *
 *  SPOTIFY_CLIENT_ID comes from https://developer.spotify.com/dashboard
 *  (see README "Spotify Developer Dashboard setup").
 *
 *  SPOTIFY_REFRESH_TOKEN comes from tools/get_refresh_token.py, which runs
 *  the browser half of the OAuth flow on your PC - the ESP32 never handles
 *  your password.
 *
 *  This value seeds NVS on first boot only. Spotify rotates refresh tokens
 *  on PKCE refreshes, and the firmware persists each new one to NVS, so the
 *  stored token soon differs from the one below. That is expected and
 *  correct. To force the device back to this value, erase NVS:
 *      pio run -t erase && pio run -t upload
 * ---------------------------------------------------------------------- */
#define SPOTIFY_CLIENT_ID      "your-32-char-spotify-client-id"
#define SPOTIFY_REFRESH_TOKEN  "your-spotify-refresh-token"

#endif /* SECRETS_H */
