# Spotify "Now Playing" — ESP32 + SPI TFT

A **Spotify remote and Now Playing display** built on an ESP32 and a 480×320
SPI TFT. It shows the current track, artist, album, artwork, progress and
active device, and controls playback through the Spotify Web API.

> **This is not a Spotify player.** It never downloads, decodes, streams or
> outputs audio, and it is **not a Spotify Connect endpoint** — it will not
> appear in anyone's device list. Audio plays on your phone, TV, computer or
> Connect speaker. This device watches that playback and drives it.

It follows Spotify Connect automatically: switch from your phone to the TV
mid-song and the display follows within a poll interval, with no interaction.

Rendering is direct `TFT_eSPI` drawing — no LVGL.

---

## Contents

- [What it does](#what-it-does)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Quick start](#quick-start)
- [Spotify Developer Dashboard setup](#spotify-developer-dashboard-setup)
- [First-run setup on the device](#first-run-setup-on-the-device)
- [Using your own domain](#using-your-own-domain)
- [Device selection and transfer](#device-selection-and-transfer)
- [Running it without a touchscreen](#running-it-without-a-touchscreen)
- [HTTP API](#http-api)
- [Build and upload](#build-and-upload)
- [Configuration reference](#configuration-reference)
- [Security model](#security-model)
- [Architecture](#architecture)
- [RAM usage and the artwork strategy](#ram-usage-and-the-artwork-strategy)
- [Why TFT_eSPI and not LovyanGFX](#why-tft_espi-and-not-lovyangfx)
- [Concurrency](#concurrency)
- [Recovery console (serial)](#recovery-console-serial)
- [Tests](#tests)
- [Troubleshooting](#troubleshooting)

---

## What it does

**Displays**

| | |
|---|---|
| Track / episode title, artist, album | scrolls when too long for its band |
| Album artwork | downloaded, scaled and cached per album |
| Progress and duration | advances locally between polls, resynced on every response |
| Play / pause state | |
| Active device | shown as `Playing on: Living Room TV` |
| Volume | where the device reports one |
| Shuffle and repeat | lit when active; repeat-one shows a `1` |
| Wi-Fi and Spotify link status | signal bars, connection dot |

**Controls** — play/resume, pause, previous, next, seek (tap the progress
bar), volume (drag the slider), shuffle, repeat (off → all → one), device
selection and playback transfer.

All of them work from the touchscreen, from optional GPIO buttons, and over
the [HTTP API](#http-api).

**Handles** — no active device, nothing playing, paused playback, podcasts and
episodes, private sessions, adverts, restricted devices, devices with no
remote volume, expired tokens, HTTP 204/401/403/404/429 (honouring
`Retry-After`), Wi-Fi loss, and artwork download or decode failure.

---

## Hardware

Defaults match a known-working build. **Every one of these is configurable in
[`include/display_config.h`](include/display_config.h)** — nothing is
auto-detected, and nothing is assumed silently.

| Item | Default | Where to change |
|---|---|---|
| MCU | ESP32-WROOM-32 DevKit, 38-pin (`esp32dev`) | `platformio.ini` → `board` |
| PSRAM | none (not required) | — |
| Panel | ILI9488, 480×320, SPI | `display_config.h` §1 |
| Panel geometry | 320×480 native, rotation 1 → 480×320 landscape | `display_config.h` §2 |
| Touch | XPT2046 resistive | `display_config.h` §5 |
| Backlight | tied to 3V3, not switchable | `display_config.h` §4 |

**Using different hardware?** The three things that matter:

1. **Panel controller.** ILI9488 and ST7796 are drop-in compatible on the
   common 4.0″ 480×320 modules — flip one `#define` in §1. ILI9341 panels are
   240×320, so you must also change §2 *and* revisit the layout constants in
   [`src/UiLayout.h`](src/UiLayout.h), which are tuned for 480×320.
2. **Touch controller.** Set `TOUCH_ENABLED 0` for a panel with no touch
   layer; the firmware then compiles out all XPT2046 code and the same three
   actions come from GPIO buttons instead (§6).
3. **Pins.** All in §3 and §5.

If you are unsure which controller you have, build with `ILI9488_DRIVER`
first. A white or blank screen, or badly inverted colours, usually means it is
the other one.

### ESP32-S3 note

The defaults target the classic ESP32. On an ESP32-S3 change `board`, and note
the comment in §7 about `USE_HSPI_PORT` — the bus-sharing argument there is
specific to the classic ESP32's VSPI. See
[Why TFT_eSPI and not LovyanGFX](#why-tft_espi-and-not-lovyangfx) for when the
other library becomes the better choice.

---

## Wiring

TFT and touch share one SPI bus and are separated only by their chip selects.

| Signal | ESP32 GPIO | Panel pin | Notes |
|---|---|---|---|
| SCLK | **18** | `SCK` / `T_CLK` | shared |
| MOSI | **23** | `SDI(MOSI)` / `T_DIN` | shared |
| MISO | **19** | `SDO(MISO)` / `T_DO` | shared |
| TFT CS | **5** | `CS` | |
| TFT DC/RS | **27** | `DC` / `RS` | |
| TFT RESET | **33** | `RESET` | set to `-1` if wired to ESP32 `EN` |
| Touch CS | **26** | `T_CS` | |
| Touch IRQ | **25** | `T_IRQ` | set to `-1` for polled mode |
| Backlight | *(3V3)* | `LED` | set `PIN_TFT_BL` for PWM dimming |
| Power | 5V or 3V3 | `VCC` | per your module's marking |
| Ground | GND | `GND` | |

Optional physical buttons (`display_config.h` §6, all `-1` by default) wire to
GND and use the internal pull-up.

> **Avoid when reassigning:** GPIO 6–11 (SPI flash), 34–39 (input-only, no
> pull-ups) and 0/2/12/15 (boot strapping).

### Why MISO must be shared carefully

Both the panel and the XPT2046 can drive MISO. If both chip selects are ever
low at once they fight, and you get a display that works fine until you touch
it and then fills with garbage. [`src/InputManager.cpp`](src/InputManager.cpp)
forces the panel's CS high around every touch read and releases the touch CS
afterwards. Keep that interlock if you change the input code.

---

## Quick start

Nothing is compiled in. Wi-Fi is entered on the panel, Spotify sign-in happens
in a browser against the device itself, and both are stored in NVS.

```bash
cp include/secrets.example.h include/secrets.h   # optional seeds; can stay blank
cp include/config.example.h  include/config.h
pio run -t upload && pio device monitor
```

Then follow [First-run setup on the device](#first-run-setup-on-the-device).

`secrets.h` still exists as an optional **first-boot seed** for Wi-Fi and the
Spotify client ID, which saves retyping when you flash several units. Leave the
placeholders untouched and the device simply asks for everything on screen.

---

## Spotify Developer Dashboard setup

1. Sign in at <https://developer.spotify.com/dashboard>.
2. **Create app**. Name and description are free text.
3. Under **Redirect URIs** add exactly:

   ```
   http://127.0.0.1:8888/callback
   ```

   Use `127.0.0.1`, **not** `localhost` — Spotify's documentation states
   *"`localhost` is not allowed as redirect URI"*, and requires HTTPS for
   everything except loopback addresses.

   **This address is never actually served, and that is fine** — see
   [how sign-in works](#why-the-redirect-goes-nowhere) below.

   Own a domain? You can use it instead — see
   [Using your own domain](#using-your-own-domain).
4. If the form shows **Which API/SDKs are you planning to use?**, tick
   **Web API**.
5. Copy the **Client ID**. You do **not** need the client secret — see
   [Security model](#security-model).
6. **Settings → User Management → Add new user**: add the name and Spotify
   account email of whoever will listen. New apps start in **Development
   Mode**, where only allowlisted users can use them (up to 5) and everyone
   else gets HTTP 403 on *every* request. Add your own account even if you
   own the app — it costs nothing and rules out the most common cause of a
   device that signs in fine and then shows `403 denied`.

The device requests exactly three scopes, and no more:

| Scope | Used for |
|---|---|
| `user-read-currently-playing` | the track / episode being played |
| `user-read-playback-state` | device list, volume, play state, progress |
| `user-modify-playback-state` | transport, seek, volume, shuffle, repeat, transfer |

> **Playback control needs Spotify Premium.** On a free account the display
> works fully, but every command returns HTTP 403 and the screen says
> *"Not permitted — Premium or device restricted"*.

### Telling the two 403s apart

Both the allowlist and the Premium requirement surface as HTTP 403, and they
look nothing alike on screen:

| What you see | Cause |
|---|---|
| `403 denied - add your account in Users Management` — nothing ever loads, not even track names | The listening account is not on the app's allowlist. Reads are refused too. |
| Track, artist and artwork display fine, but **buttons** say `Not permitted — Premium or device restricted` | Reads work, so the allowlist is fine. Either the account is not Premium, or that device refuses remote control. |

The rule of thumb: if the display works, the allowlist is fine.

---

## First-run setup on the device

### 1. Wi-Fi, on the panel

With no stored network the device boots straight to the Wi-Fi picker.

1. Tap a network. **Rescan** re-scans; scanning is asynchronous so the UI
   stays responsive.
2. Type the password on the on-screen keyboard. `?123` reaches digits and
   symbols, and **more** reaches a second symbol page — between them every
   printable ASCII character a WPA2 passphrase can contain is available.
   Open networks skip this step.
3. Tap **Connect**. Credentials go to NVS; the device reconnects to them on
   every boot and reconnects automatically if the link drops.

You can return to this screen any time from the sign-in screen's **Wi-Fi**
button. A tap during the boot splash also goes there, which is the way out if
the stored network no longer exists.

### 2. Spotify, in a browser

Once Wi-Fi is up the screen shows a URL like `http://192.168.1.50/`. Open it
from any phone or computer on the same network.

1. Paste your **Client ID** and save it.
2. Follow **Authorise with Spotify** and approve access.
3. The browser lands on a page that **fails to load**. That is expected.
   Copy the whole address out of the address bar.
4. Paste it into the box and press **Complete sign-in**.

The device exchanges the code for tokens itself and stores the refresh token in
NVS. Your Spotify password never touches the ESP32.

#### Why the redirect goes nowhere

Spotify permits plain HTTP for loopback redirect URIs only. The ESP32 lives on
a LAN address, so it can never legally be the redirect target over HTTP, and
giving it a real HTTPS certificate is not practical.

It does not need to be. Under PKCE the only secret is the **code verifier**,
which the device generates and keeps. The redirect exists purely to hand back
an authorisation code, and that code is sitting in the address bar of the page
that failed to load. Pasting the URL back gives the device the one piece it was
missing, and it completes the exchange itself.

That is also why the failed page is harmless: the code is single-use, expires
in about a minute, and is worthless without the verifier the device is holding.

### Optional: pre-seed credentials for a batch

`tools/get_refresh_token.py` runs the same PKCE flow on a PC and prints
`SPOTIFY_CLIENT_ID` / `SPOTIFY_REFRESH_TOKEN` lines for `include/secrets.h`.
It is not needed for normal setup — it exists for flashing several units
without repeating the browser step on each. Python 3.8+, standard library
only:

```bash
python3 tools/get_refresh_token.py --client-id YOUR_CLIENT_ID
```

Seeded values are written to NVS on first boot, after which NVS is
authoritative.

### Using your own domain

Any **HTTPS** URL you control works as the redirect, and gives you a page
that renders instead of a browser error. Plain `http://` is accepted only for
`127.0.0.1` and `[::1]`, so the ESP32's own LAN address can never be used.

1. Register e.g. `https://example.com/callback` in the dashboard.
2. Set the identical string in [`include/config.h`](include/config.h):

   ```c
   #define SPOTIFY_REDIRECT_URI "https://example.com/callback"
   ```

3. Upload [`tools/callback.html`](tools/callback.html) so it is served at
   that exact path. It shows the authorisation code with a copy button.

All three strings must match **exactly** — a trailing slash is a different
URI. The firmware logs an error at boot if the scheme will be rejected,
rather than letting it surface later as a bare `INVALID_CLIENT`.

The page is entirely static: no server-side code, and the code is read from
the address bar and displayed, never transmitted.

**What it does not change.** The flow is the same — you still copy the code
into the device. A hosted page cannot post it to the ESP32 directly, because
browsers block an HTTPS page from calling a plain-HTTP LAN address. What you
gain is a real page instead of a connection error, and a copy button instead
of selecting a URL out of the address bar.

**Is it safe to route the code through your own server?** Yes, and this is
worth understanding rather than taking on trust. Your access log will contain
the authorisation code in the query string. Under PKCE that is close to
worthless: the code is single-use, expires in about a minute, and cannot be
redeemed without the **code verifier** — which the ESP32 generates, keeps in
RAM, and never transmits to anyone but Spotify. An attacker with your logs
has one half of a pair and no way to get the other.

Buffer sizing is not a constraint: every encode buffer in the auth path is
sized from `SPOTIFY_REDIRECT_URI`, so a long domain cannot overflow or
truncate. Verified up to a 126-character URI.

### Token rotation, and the 6-month wall

Two separate things, both worth knowing.

**Rotation.** Spotify may issue a new refresh token on any PKCE refresh, and
the old one stops working. The firmware persists every rotation to NVS.
Missing this is the classic way one of these builds works for an hour and then
dies permanently.

**Expiry.** Refresh tokens issued through the dashboard have a **6-month
lifetime, and using them does not extend it** — the clock runs from when you
authorised, not from last use. The dashboard shows this as *Refresh Token
Lifetime: 180 days*.

So a device that has been refreshing happily every hour for six months will
still stop one day and need signing in again. That is Spotify's design, not a
fault here, and there is no way to avoid it.

When it happens the device follows Spotify's stated guidance — discard the
token rather than retry — erases it from NVS, and shows
*"Sign in again from the setup page"*. Because the token is gone rather than
merely flagged, a reboot goes straight to the sign-in screen instead of
burning a doomed request first. Sign in again and the 6 months restart.

Put a calendar reminder somewhere if the device lives out of reach.

To wipe stored credentials: **Sign out of Spotify** on the setup page, or
`pio run -t erase` to clear Wi-Fi and Spotify together. Access can also be
revoked at <https://www.spotify.com/account/apps/>.

---

## Device selection and transfer

Tap **`Playing on: ...`** in the status bar to open the device picker. It lists
what `GET /v1/me/player/devices` returned:

- device name
- device type (Computer, Smartphone, Speaker, TV, ...)
- whether it is active — green dot
- current volume, or `--` when it does not report one
- `restricted` when the device rejects Web API control
- `no remote volume` when volume cannot be set

Tapping a device transfers playback to it with `PUT /v1/me/player`. The
**Keep playing during transfer** toggle controls the `play` flag; it defaults
to on, because pausing on handover is almost never what anyone wants.

Restricted devices are refused before the request is sent rather than after a
confusing 403.

**Command targeting.** After an explicit selection, commands carry that
`device_id` so they cannot drift to whatever became active in the meantime.

**Following Connect.** The display always follows the active device, so moving
playback from your phone to the TV in the Spotify app updates the screen on the
next poll with no interaction here.

Commands follow too, which needs a little care. Two requirements pull against
each other: commands should go to the device you picked, *and* the remote
should follow you when you switch devices in the Spotify app. Holding the
selection forever breaks the second — press Next after moving to your phone
and the old device wakes up instead. Dropping it the instant the active device
differs breaks the first, because Spotify keeps reporting the **old** device
for a second or two after our own transfer request.

So the selection is protected for `TRANSFER_GRACE_MS` (8 s) after a transfer,
and released afterwards if Spotify reports something else active — at that
point you have chosen a different device, just not through this UI. It is also
released if the device disappears from the list or returns 404.

One deliberate exception: an *empty* active device never releases the pin.
Nothing is playing anywhere, and the device you picked is exactly what a Play
press should wake. The policy is a pure function,
`shouldReleaseDevicePin()` in [`src/AppState.h`](src/AppState.h), covered by
seven assertions in the host tests.

---

## Running it without a touchscreen

The page at `http://<device-ip>/` is a **complete replacement for the touch
panel**, not just a setup page. That matters for two groups: anyone who built
with `TOUCH_ENABLED 0`, and anyone whose touch layer turns out not to be
wired (see [Buttons never light up](#buttons-never-light-up-when-pressed)).

From a browser you get:

| | |
|---|---|
| Now playing | title, artist, album, device, play state |
| Transport | previous, play, pause, next |
| Seek | drag the position slider |
| Volume | slider, disabled when the device does not support it |
| Shuffle / repeat | toggle and cycle |
| **Device selection** | list with type, volume and restricted/no-volume flags, plus *Use this* to transfer, and a *Keep playing during transfer* toggle |
| Wi-Fi | scan, join, forget |
| Spotify | sign in, sign out, client ID |

When the boot probe finds the touch controller unresponsive, the device puts
the URL on screen rather than leaving you prodding a panel that will never
answer:

```
No touch detected - control at http://192.168.1.50/
```

If touch is dead and the device is on the wrong network, neither the panel
nor the browser can reach it — that is what the
[serial recovery console](#recovery-console-serial) is for.

---

## HTTP API

The device serves a small REST API on port 80 (`WEB_SERVER_PORT`). The setup
page at `/` uses it, and so can anything else on your network.

**Including Wi-Fi**, so a device whose touchscreen is unreachable can still be
moved to another network from a browser:

| Method | Path | Parameters |
|---|---|---|
| GET | `/api/wifi/scan` | — (starts a scan, returns what is known so far) |
| PUT | `/api/wifi` | `ssid`, optional `password` |
| POST | `/api/wifi/forget` | — |

```bash
curl "http://192.168.1.50/api/wifi/scan"
curl -X PUT "http://192.168.1.50/api/wifi" \
     --data-urlencode "ssid=FasterNetwork" --data-urlencode "password=..."
```

Joining a new network drops the connection the request arrived on, so the
change is applied one loop iteration *after* the response is flushed — a
browser error at that point is expected, not a failure. Reconnect to the new
address the device shows on screen.

| Method | Path | Parameters |
|---|---|---|
| GET | `/api/spotify/devices` | — |
| PUT | `/api/spotify/active-device` | `device_id`, optional `play` (default `true`) |
| GET | `/api/spotify/currently-playing` | — |
| POST | `/api/spotify/play` | — |
| POST | `/api/spotify/pause` | — |
| POST | `/api/spotify/next` | — |
| POST | `/api/spotify/previous` | — |
| PUT | `/api/spotify/seek` | `position_ms` |
| PUT | `/api/spotify/volume` | `volume_percent` (0–100) |
| PUT | `/api/spotify/shuffle` | `state` = `true` \| `false` |
| PUT | `/api/spotify/repeat` | `state` = `off` \| `context` \| `track` |
| GET | `/api/status` | Wi-Fi, IP, sign-in state, client ID |
| POST | `/api/auth/start` | returns the authorisation URL |
| POST | `/api/auth/complete` | `redirect_url` or `code` |
| POST | `/api/auth/signout` | — |
| PUT | `/api/client-id` | `client_id` |

```bash
curl http://192.168.1.50/api/spotify/devices
curl -X POST http://192.168.1.50/api/spotify/next
curl -X PUT  "http://192.168.1.50/api/spotify/volume?volume_percent=40"
curl -X PUT  "http://192.168.1.50/api/spotify/active-device?device_id=abc&play=true"
```

### Reads are cached, writes are queued

No handler calls Spotify synchronously.

- **GETs** answer from the state the poller already maintains, so they are
  instant and at most one poll interval stale.
- **Commands** return **202 Accepted** — queued, not performed. Spotify has not
  been told yet and may still refuse.

Two concrete reasons, not style. A blocking upstream call inside a handler
would stall the whole loop — display and touch included — for a TLS round
trip. And it would open a second mbedTLS session while the poller or an artwork
download already holds one, which is about 40 KB the heap cannot always spare.

The sign-in exchange is the one deliberate exception: it blocks, because it
happens once, during setup, when nothing else is running.

### No authentication

**Anyone who can reach the device on your LAN can control playback and read the
setup page.** That is the same trust boundary as a Chromecast or a printer.
It is stated here rather than papered over with a password that would have to
live in NVS on the same readable flash.

The refresh token is never exposed by any endpoint. If your network is not
somewhere you would put a Chromecast, set `WEB_SERVER_PORT` aside and keep the
device on a VLAN, or drop the `ApiServer` from `main.cpp` — nothing else
depends on it.

---

## Build and upload

```bash
pio run                 # build
pio run -t upload       # build and flash
pio device monitor      # serial log at 115200
pio run -t erase        # wipe NVS (clears the stored refresh token)
```

Current footprint on the reference hardware:

| | Used | Available | |
|---|---|---|---|
| Static RAM | 58,604 B (~57 KiB) | 327,680 B | 18% |
| Flash | 1,108,209 B (~1082 KiB) | 3,145,728 B | 35% |

Plus two runtime allocations made at boot — a 48 KiB artwork buffer and a
15.4 KiB text sprite — detailed under
[RAM usage](#ram-usage-and-the-artwork-strategy).

The 3 MB comes from `board_build.partitions = huge_app.csv`. The default
`esp32dev` table gives an app only 1.31 MB, which TFT_eSPI + mbedTLS + Wi-Fi +
fonts overruns. Switch to `min_spiffs.csv` (1.9 MB app, keeps an OTA slot) if
you add over-the-air updates.

### Where the pin configuration lives

`platformio.ini` force-includes `include/display_config.h` into **every**
translation unit:

```ini
build_flags = -include $PROJECT_INCLUDE_DIR/display_config.h
```

TFT_eSPI is normally configured by editing `User_Setup.h` inside the library
folder — which is under `.pio/libdeps/` and is destroyed on every clean build.
Because `USER_SETUP_LOADED` is defined in `display_config.h`, TFT_eSPI skips
its own setup headers entirely and reads ours. One file is the truth for both
the library and our code, and nothing in `.pio/` ever needs editing.

The side effect: `display_config.h` is also compiled as part of C sources
(TJpg_Decoder ships `tjpgd.c`), so it must stay valid C. C++-only content goes
in the guarded block at the bottom.

#### The rebuild trap this creates

PlatformIO's SCons scanner finds dependencies by parsing `#include`
directives out of source files. A header injected with `-include` is on the
command line, not in any source file, so **SCons cannot see it**. Left alone,
that means: edit a pin, run `pio run -t upload`, and the *old* firmware is
flashed, because the build system believes nothing changed. It fails silently
and costs an hour of debugging wiring that was never wrong.

`tools/pio_config_fingerprint.py` (wired in via `extra_scripts`) fixes it by
hashing the header and injecting the digest as a `-D`. Any edit changes the
digest, which changes every compile command line, which is part of the SCons
build signature — so everything rebuilds. Verified: an unchanged build
recompiles 0 files, changing `TFT_CS` recompiles 79 including `TFT_eSPI.cpp`.

The digest is compiled in and printed at boot:

```
[     118] I panel 480x320, cfg 0xaf97cc89, free heap 243104
```

which is the quickest way to confirm a board is running the pin map you think
it is.

---

## Configuration reference

| File | Committed? | Holds |
|---|---|---|
| `include/display_config.h` | yes | pins, panel driver, geometry, touch calibration, SPI clocks, fonts |
| `include/config.example.h` | yes | template for the file below |
| `include/config.h` | **no** | poll intervals, timeouts, TLS policy, artwork sizing, redirect URI, web port, log level |
| `include/secrets.example.h` | yes | template for the file below |
| `include/secrets.h` | **no** | optional first-boot seeds only |

`config.h` holds nothing secret — it is gitignored only so your local tuning
never collides with an upstream change. Copy it across untouched if you like.

**`secrets.h` is optional.** Wi-Fi and the Spotify client ID are entered on the
device and kept in NVS; the file only pre-seeds them, which saves retyping when
flashing several units. Leave the placeholders alone and the device asks for
everything on screen. Whatever is in NVS always wins.

| Setting | Default | Notes |
|---|---|---|
| `POLL_INTERVAL_PLAYING_MS` | 4000 | while audio is playing |
| `POLL_INTERVAL_IDLE_MS` | 12000 | paused or nothing playing |
| `DEVICE_POLL_INTERVAL_MS` | 30000 | device list; forced when you open the picker |
| `TRANSFER_GRACE_MS` | 8000 | how long a device selection survives before following Connect |
| `SPOTIFY_REDIRECT_URI` | `http://127.0.0.1:8888/callback` | must match the dashboard exactly |
| `WEB_SERVER_PORT` | 80 | on-device HTTP server |
| `ART_BOX_PX` / `ART_MAX_BYTES` | 160 / 48 KiB | artwork frame and download ceiling |
| `TLS_VERIFY_CERTIFICATES` | 1 | see [Security model](#security-model) |
| `LOG_LEVEL` | 2 | 3 adds per-request detail, never credentials |

### Compile-time checks

Misconfigurations that would otherwise show up as a dead panel are rejected at
build time rather than at run time:

| Check | Message |
|---|---|
| `TFT_ROTATION` not 1 or 3 | *TFT_ROTATION must be 1 or 3 for landscape* |
| Landscape geometry inconsistent | *This UI is landscape-only…* |
| Touch CS equals TFT CS | *Touch and TFT cannot share a chip select* |
| Touch calibration min ≥ max | *Touch calibration maxima must exceed the minima* |
| Layout rectangles overlapping | e.g. *transport buttons overlap the volume slider* |
| `include/config.h` missing | *Run: cp include/config.example.h include/config.h* |
| `include/secrets.h` missing | *Run: cp include/secrets.example.h include/secrets.h* |

### Touch calibration

The defaults came off the reference panel; yours will differ. If touches land
in the wrong place, fix the **three flags first**, reflash, and only then
adjust the min/max values:

| Symptom | Fix |
|---|---|
| Moves vertically when you slide sideways | flip `TOUCH_SWAP_XY` |
| Moves left when you slide right | flip `TOUCH_INVERT_X` |
| Moves up when you slide down | flip `TOUCH_INVERT_Y` |

Then set `TOUCH_DEBUG_RAW 1`, touch each corner, and read the raw values off
the serial log into `TOUCH_MIN_X` … `TOUCH_MAX_Y`.

### Polling and rate limits

Spotify enforces a rolling ~30 second rate-limit window and does not publish
the quota. Defaults: one request every **4 s while playing**, **12 s when
idle** — comfortably inside it.

The firmware also:

- honours `Retry-After` on HTTP 429 (clamped to 1–3600 s, so a malformed or
  hostile header cannot park the device for a week),
- applies exponential back-off on repeated failures, up to 120 s,
- stops polling entirely if the refresh token is rejected, rather than
  hammering the endpoint with a dead credential.

---

## Security model

### No client secret anywhere

This build uses **Authorization Code + PKCE**, run entirely on the device: it
generates its own verifier, and both the authorisation and refresh legs need
only the client ID. No client secret is ever compiled in, stored, or typed.

That matters because ESP32 flash is readable over UART with a USB cable and no
special equipment. **A client secret in firmware is a published client
secret** — and a Spotify client secret is not per-device: leaking it
compromises the application registration for every user of it.

Verified against the live endpoint — the secret-less refresh shape is accepted,
and only the token itself is validated:

```console
$ curl -X POST https://accounts.spotify.com/api/token \
    -d "grant_type=refresh_token&refresh_token=...&client_id=..."
{"error":"invalid_grant","error_description":"Invalid refresh token"}
```

### If a flow ever does require a secret

Do not paste it into `secrets.h`. Put a **token broker** between the device and
Spotify:

```
ESP32  ──HTTPS──▶  your broker  ──HTTPS──▶  accounts.spotify.com
                (holds the secret        (never sees the device)
                 and refresh token)
```

The broker holds the client secret and refresh token server-side and hands the
device only short-lived access tokens, authenticated with a per-device key you
can revoke individually. A leaked device key then costs you one device, not the
application registration. Any small always-on host works — the broker is a few
dozen lines.

### TLS certificate verification

On by default (`TLS_VERIFY_CERTIFICATES 1`). Roots are pinned per host in
[`src/SpotifyCerts.h`](src/SpotifyCerts.h), generated from a trust store and
each verified against the live endpoint.

The three hosts do **not** share a root:

| Host | Chain terminates at |
|---|---|
| `accounts.spotify.com` | Starfield Root CA G2 *(cross-signing a Certainly intermediate)* |
| `api.spotify.com` | DigiCert Global Root G2 |
| `i.scdn.co` | DigiCert Global Root G3 |

`accounts.spotify.com` is the fragile one: its Certainly intermediate is
currently **cross-signed by Starfield**, so the chain terminates at Starfield
rather than at Certainly Root R1. Cross-signs exist to be retired, so **both**
roots are bundled and mbedTLS accepts a chain validating to either — that
particular rotation will pass without a firmware update.

Pinning trades availability for security: if Spotify moves to a CA that is not
listed, TLS fails and the device stops until reflashed. That is the right
failure direction for a device carrying a credential, but it is a real
maintenance obligation. See
[Troubleshooting](#tls-handshake-failures-after-months-of-working) for how to
regenerate.

### What the device stores

| Item | Where | Cleared by |
|---|---|---|
| Wi-Fi SSID + password | NVS (`wifi`) | `pio run -t erase` |
| Spotify client ID | NVS (`spotify`) | setup page, or erase |
| Spotify refresh token | NVS (`spotify`) | **Sign out** on the setup page, or erase |
| PKCE verifier | RAM only | any reboot |

The verifier is deliberately RAM-only: it is valid for a single sign-in
attempt, so a reboot mid-flow just means starting sign-in again.

`include/secrets.h` remains an optional first-boot seed. It is gitignored, and
once anything is in NVS the stored value wins.

### What is never logged

Serial output never contains the Wi-Fi password, the access token or the
refresh token. Credentials are logged through `redact()`
([`src/Log.h`](src/Log.h)), which prints `<len=131 fp=a1b2>` — enough to tell
"the token rotated" from "the token is empty" without disclosing the value.
The fingerprint is 16 bits of a non-cryptographic hash, chosen so it conveys
nothing useful about the token itself.

---

## Architecture

```
src/
├── main.cpp            scheduling; owns the state, wires the modules together
├── AppState.h          PlaybackState, DeviceList, AppStatus — the shared model
├── NetManager.*        Wi-Fi association, NVS credentials, async scanning
├── SpotifyClient.*     OAuth lifecycle, HTTP transport, command queue, devices
├── PlayerParser.*      JSON → model (no networking, host-testable)
├── AuthUtil.h          base64url + redirect-URL parsing (host-testable)
├── SpotifyCerts.h      pinned root CAs (generated)
├── AlbumArt.*          artwork choice, incremental download, decode, cache
├── ApiServer.*         on-device HTTP server: setup pages + REST API
├── UiController.*      screen routing; taps → Spotify commands
├── DisplayManager.*    all rendering + primitives; nothing else touches the TFT
├── DeviceScreen.*      Spotify Connect device picker
├── SetupScreens.*      Wi-Fi picker, on-screen keyboard, sign-in screen
├── InputManager.*      touch → debounced events; GPIO buttons → actions
├── UiLayout.h          every rectangle, colour and font
└── Log.h               level-gated logging with credential redaction
```

Four deliberate structural choices:

- **`PlayerParser` and `AuthUtil` are split out** of the modules that use
  them. Both are pure logic that fails *quietly* — a mistyped field name
  gives a blank label, a base64 alphabet slip gives a string Spotify simply
  rejects. Separated from transport they compile and run on a PC, and
  [the tests](#tests) cover podcasts, private sessions, adverts, restricted
  devices and adversarial redirect URLs without needing any of them to occur.
- **`InputManager` emits touch events, not actions.** The same tap means
  "next track", "pick this device" or "type a Q" depending on the screen, so
  hit-testing belongs to whichever screen is showing. GPIO buttons are the
  exception — they have no screen context and map straight to transport,
  which is why `TOUCH_ENABLED 0` needs no other change.
- **`UiController` owns screen routing**, so `main.cpp` stays a scheduler and
  adding a screen touches one file.
- **`DisplayManager` owns every pixel.** The other screens hold no TFT
  reference; they compose its primitives. One place can write to the panel,
  which matters on a bus shared with the touch controller.

### Screens

```text
            ┌─────────── boot splash ───────────┐
            │ no Wi-Fi saved, or won't associate│
            ▼                                   ▼
      Wi-Fi picker ──tap network──▶ keyboard  Now Playing ◀─┐
            ▲                          │           │       │
            └──────── Wi-Fi ──── sign-in screen ◀──┘       │
                                       │  "Playing on:" tap│
                                       └── device picker ──┘
```

### Display updates

`DisplayManager::update()` runs every loop iteration but repaints almost
nothing. It diffs the incoming state against a snapshot of what is on the glass
and redraws only what differs. On a steady playing track that is one
progress-bar delta of a couple of pixels per second plus a timecode every
second — a few hundred bytes of SPI traffic, instead of the ~460 KB a full
480×320 repaint would cost.

Progress advances locally from `millis()` between polls and is re-anchored on
every fresh response, so the bar is smooth without extra requests and cannot
drift away from the server's value.

Text too wide for its band ping-pongs left and right with a pause at each end.

### Handled conditions

| Condition | Behaviour |
|---|---|
| Nothing playing | "Nothing playing", controls greyed |
| No active device (HTTP 204 / 404) | hollow status dot, explanatory message |
| Paused | play icon, progress frozen |
| Podcasts / episodes | show → album line, publisher → artist line, episode art |
| Private session | "Private session — details hidden"; device still shown |
| Advertisement | "Advertisement", placeholder art |
| Access token expired (401) | silent refresh, request retried |
| Refresh token dead or 6 months old (400/401) | token discarded from NVS, prompts re-sign-in |
| Forbidden (403) | "Not permitted — Premium or device restricted" |
| Rate limited (429) | honours `Retry-After`, backs off |
| Wi-Fi lost | crossed-out indicator, automatic reconnect, UI keeps running |
| API unreachable | exponential back-off; last known state stays on screen |
| Artwork download/decode failure | placeholder, 30 s before retrying that cover |
| Device restricted | refused before sending; row marked in the picker |
| Device has no remote volume | slider hidden, message on attempt |
| Pinned device disappears | pin dropped, falls back to the active device |
| User switches device in the Spotify app | display and commands both follow |

---

## RAM usage and the artwork strategy

The constraint: an ESP32-WROOM-32 has ~320 KB of DRAM and no PSRAM, and
mbedTLS wants tens of kilobytes while a TLS session is live.

What that rules out:

| Approach | Cost | Verdict |
|---|---|---|
| Full-screen 480×320 framebuffer | 307 KB | impossible |
| Decoded 640×640 album art | 820 KB | impossible |
| Decoded 160×160 art held in RAM | 51 KB | possible, but pointless |

So the decoded image is **never held in RAM at all**:

1. **Pick the smallest image that still covers the box.** Spotify offers
   roughly 640/300/64 px squares; for a 160 px box that is the 300 px one —
   ~25 KB to download instead of ~90 KB.
2. **Buffer only the compressed JPEG.** One 48 KB allocation, made at boot
   while the heap is still whole, never grown and never freed. Allocating it
   lazily would mean asking for 48 KB contiguous *after* mbedTLS has
   fragmented the heap — exactly when it is most likely to fail.
3. **Stream the decode.** TJpg_Decoder emits 16×16 MCU blocks through a
   callback; each block is pushed straight to the panel and dropped. Peak
   decode footprint is a few hundred bytes of working state.
4. **Scale during decode.** A baseline JPEG decoder gets ½, ¼ and ⅛ almost
   free because they fall out of the IDCT. The firmware picks the largest
   reduction that still fits the box, so the decoder never even produces
   pixels that would be discarded. A 300 px source into a 160 px box decodes
   at ½ → 150×150, centred, aspect ratio intact.

Runtime allocations, once each, at boot:

| Allocation | Size | Purpose |
|---|---|---|
| JPEG download buffer | 49,152 B | compressed artwork |
| Marquee sprite (282×28×2 B) | 15,792 B | flicker-free scrolling text |

All four text bands are deliberately the same size so **one** sprite renders
any of them. Four dedicated sprites would cost ~55 KB that TLS needs. If the
sprite cannot be allocated, the code falls back to clipped direct drawing via
`setViewport` — slightly flickery, but it always works.

The device logs free / minimum-ever / largest-block heap every 30 s, so a slow
leak shows as a visible trend rather than a reboot three days later.

### Keeping one TLS session at a time

Two simultaneous TLS sessions would need ~40 KB at once. The main loop releases
the kept-alive API socket before starting an artwork download, and only when a
download will genuinely happen — `AlbumArt::needsFetch()` exists so a cache hit
does not cost the connection.

Conversely the API connection *is* kept alive between polls: a fresh handshake
to `api.spotify.com` costs 1–2 s of RSA work on this chip, and at a 4 s poll
interval that would leave the UI unresponsive roughly a third of the time.

---

## Why TFT_eSPI and not LovyanGFX

Both would work. TFT_eSPI wins here on specifics:

- **Proven on this exact panel.** The ILI9488-over-SPI path, the 27 MHz clock
  and the XPT2046 CS interlock are all carried over from a working build on
  the same hardware. That is worth more than a feature comparison.
- **The whole configuration is compile-time macros**, which is what makes the
  single-file `-include` scheme work — TFT_eSPI's setup *is* preprocessor
  defines, so one header can configure the library and the application
  together. LovyanGFX configures through a C++ bus/panel object graph, which
  cannot be force-included into `tjpgd.c`.
- **TJpg_Decoder is by the same author** and its callback pushes straight into
  `pushImage()` with no glue.
- Smaller flash footprint for the subset used here.

**Choose LovyanGFX instead if** you move to an ESP32-S3 with a parallel or
RGB panel, want DMA-driven full-frame pushes, need runtime panel
reconfiguration, or want its richer sprite/anti-aliasing support. On the
classic ESP32 with an SPI panel there is no real advantage, and the layout
code here is thin enough to port either way.

---

## Concurrency

There is none, by design. Network, parsing, rendering and input all run on the
Arduino loop task.

That is a choice, not a shortcut: `PlaybackState` needs no mutex, the shared
VSPI bus has exactly one user, and there is no way for a display write and an
HTTP read to interleave. FreeRTOS tasks here would buy nothing and would need
a mutex around the SPI bus, the playback state and the HTTP client.

The cost is that slow work must be broken up rather than blocked on:

- **No `delay()` anywhere in the run loop.** Every schedule is a `millis()`
  deadline.
- **The artwork download is a state machine**, consuming one TCP segment per
  loop iteration, so touch keeps being sampled while artwork is in flight.
- **`SpotifyClient::service()` performs at most one HTTP transaction per
  call**, so a single iteration never carries two TLS handshakes.
- **The web API queues rather than calls**, so an HTTP request from a browser
  cannot stall the display or open a second TLS session.

The one place that does block is the browser sign-in exchange, which runs once
during setup when nothing else is in flight.

Loop budget in steady state: a poll costs ~200 ms once every 4 s, artwork
~1.2 s spread over ~400 iterations once per album, everything else well under
a millisecond. Touch is sampled every 15 ms throughout.

The loop task stack is raised to 16 KB via `SET_LOOP_TASK_STACK_SIZE()` in
`main.cpp`, because mbedTLS chain verification is stack-hungry and the 8 KB
default is marginal once JSON parsing and JPEG decoding share it. Note this
**cannot** be done with a `-D` build flag — the SDK's own `sdkconfig.h`
redefines `CONFIG_ARDUINO_LOOP_STACK_SIZE` later in the include order and
silently wins.

---

## Tests

The pure-logic modules compile and run on a PC:

```bash
./tools/run_parser_tests.sh
```

**125 assertions** across two suites (run `pio run` once first so ArduinoJson
is downloaded):

| Suite | Covers |
|---|---|
| `AuthUtil` (41) | base64url against the RFC 4648 vectors, the `+`/`/` → `-`/`_` substitution, PKCE verifier and challenge lengths, buffer-overflow refusal, and redirect-URL parsing including decoys like `error_code=` that a naive `strstr` would match |
| `PlayerParser` (84) | a track with multiple credited artists, a podcast episode, a private session, an advert, a restricted device, a response with no `device`, artwork smaller than the box, shuffle/repeat mapping, the device list, list overflow, an empty device list, and the command-target pin policy |

Several are regression tests for bugs found during review — the private-session
flag being wiped by `clearItem()`, and a restricted device advertising volume
control it does not have.

The layout is checked at **compile time** rather than by test: overlapping
rectangles, a keyboard row wider than the panel, and a non-landscape rotation
are all `static_assert` failures. See
[Compile-time checks](#compile-time-checks).

---

## Recovery console (serial)

Every other way into the device can itself fail: the touchscreen needs
working touch hardware, and the web page needs the device already on your
network. Store one bad SSID on a unit whose touch layer is not wired, and
there is no way back short of erasing flash.

The serial console needs neither — just the USB cable you flash with.

```
pio device monitor        # 115200
help
```

| Command | Does |
|---|---|
| `status` | Wi-Fi state, SSID, IP, signal, Spotify sign-in, heap |
| `scan` | list visible networks |
| `ssid <name>` | stage an SSID (spaces are fine) |
| `pass <password>` | stage a password (spaces are fine; never echoed) |
| `join` | save the staged credentials and connect |
| `forget` | erase stored Wi-Fi credentials |
| `touch` | re-run the touch controller probe |
| `signout` | erase the Spotify refresh token |
| `reboot` | restart |

```
ssid MyHotspot
pass hunter2
join
```

Credentials are staged and then committed rather than parsed from one line,
because SSIDs and passwords both routinely contain spaces and any
single-line syntax would have to guess where one ends.

---

## Troubleshooting

### Blank or white screen

Wrong driver — swap `ILI9488_DRIVER` / `ST7796_DRIVER` in `display_config.h`
§1. If it stays blank, check `TFT_RST` and that the module is getting the
voltage its silkscreen asks for.

### Garbled display, but only after touching the screen

The CS interlock is broken. Both the panel and the XPT2046 are driving MISO.
Check `PIN_TOUCH_CS` differs from `TFT_CS`, and that `USE_HSPI_PORT` is *not*
defined.

### Buttons never light up when pressed

Two completely different faults look identical from the front of the panel,
so the firmware distinguishes them for you in the serial log at 115200.

**1. The touch controller is not responding.** At boot it is probed directly
over SPI:

```
touch: probe cs=26 irq=25 -> x 1873..1902 (varying), z1 12
touch: controller responding normally
```

An untouched panel floats, so a live controller returns slightly different
values each time. A bus that is not wired through returns the same value
every read:

```
touch: probe cs=26 irq=25 -> x 0..0 (IDENTICAL), z1 0
touch: controller is NOT responding (every read was 0). Check T_CS=26,
       T_CLK=18, T_DIN=23, T_DO=19 ...
```

That is a wiring fault — most often the touch header simply not connected,
or `T_DO`/`T_DIN` swapped. The display half will work perfectly throughout,
because it uses different pins for chip select.

**2. The main loop is starved.** Everything shares one task, so a blocking
network call freezes touch sampling *and* redrawing. The symptoms are
identical to dead touch: nothing lights up, and the screen stops following
changes made elsewhere. The loop-time watchdog names it:

```
loop: worst iteration 4100ms (6s ago) - the UI was frozen that long;
      touch and redraws were starved
```

Healthy is under a millisecond, with a poll costing a couple of hundred.
Anything approaching a second means the UI was unresponsive for that long.
`HTTP_TIMEOUT_MS` is the ceiling on a single freeze — it is a UI budget as
much as a network one.

A third, simpler possibility if presses register but land wrong: see
[Touch calibration](#touch-calibration).

### Display works, touch does nothing

Set `PIN_TOUCH_IRQ` to `-1` (polled mode). Many modules do not route `T_IRQ`.

### Touch lands in the wrong place

See [Touch calibration](#touch-calibration). Fix the three flags before
touching the min/max numbers.

### Wrong colours / noise / tearing

Lower `SPI_FREQUENCY` to 20000000. 27 MHz is safe on DuPont jumpers; long or
untidy wiring may need less.

### "Sign in: open the setup page"

No Spotify client ID is stored. Open `http://<device-ip>/` and paste one.

### "Sign in again from the setup page"

Spotify rejected the refresh token (`invalid_grant`). Three causes, in
rough order of likelihood:

1. **It is six months old.** Refresh tokens expire 180 days after
   authorisation and using them does not extend that. Expected, unavoidable,
   and it will recur every 6 months.
2. Access was revoked at <https://www.spotify.com/account/apps/>.
3. A rotation was lost — for example the device was erased after it had
   rotated past the seed in `secrets.h`.

In every case the fix is the same: sign in again from the setup page. The
device has already discarded the dead token, so it will be waiting on the
sign-in screen.

### "Code rejected (expired or already used)"

An authorisation code is single-use and expires in about a minute. Click
**Authorise with Spotify** again for a fresh one, and paste the new URL. Note
that reloading the failed redirect page does *not* produce a new code.

### "No sign-in in progress — start again"

The device rebooted between generating the link and your paste. The PKCE
verifier is held in RAM only, so start from **Authorise with Spotify**.

### Stuck on the boot splash

If credentials are already stored, the device spends up to 25 s trying them
before falling back to the picker. **Tap the splash** to jump straight to
Wi-Fi setup — that is the way out when the stored network no longer exists.

### The setup page will not load

Check the IP on the sign-in screen (tap the boot splash, or the status bar,
to reach it). The device must be on the same network as your browser. The
server is stopped while Wi-Fi is down and restarted on reconnect.

### The on-screen keyboard misses characters

`?123` reaches digits and common symbols; **more** reaches a second symbol
page with `~ \` | ^ { } [ ] < > = % \\ / ! ?`. Between the four layers every
printable ASCII character is available. If a key lands on the wrong character,
the touch panel needs calibrating — see [Touch calibration](#touch-calibration).

### Wi-Fi scan finds nothing

The screen distinguishes three cases, and they need different responses:

| Screen says | Meaning | What to do |
|---|---|---|
| `Looking for networks...` | scan in progress | wait ~3 s |
| `Scan failed` | the radio refused to start a scan | it retries 4× automatically; then tap **Rescan** |
| `No networks found` + *2.4 GHz only* | the scan ran and genuinely saw nothing | see below |

**If it scanned and found nothing**, in order of likelihood:

1. **Your network is 5 GHz.** The ESP32-WROOM-32 has no 5 GHz radio — those
   networks are not weak here, they are *invisible*. If your router publishes
   one name for both bands, it will still appear, because the 2.4 GHz radio
   is broadcasting it. If you have separate `…-5G` names, you need the other
   one.
2. **Hidden SSID.** Hidden networks are not listed. Seed it through
   `secrets.h` instead and reflash.
3. **Out of range**, or the antenna is not connected on a U.FL variant.

Whatever the screen says, the serial log at 115200 is definitive:

```
wifi: scanning (attempt 1)
wifi: scan found 6 network(s) of 9 visible      <- working (3 were duplicates)
wifi: nothing visible - note the ESP32-WROOM-32 is 2.4 GHz only
wifi: could not start scan (-2)                 <- driver refused; auto-retries
```

> Fixed in this build: `WiFi.scanNetworks()`'s return value used to be
> ignored, so a scan that never started was reported as an empty list that
> **Rescan could not clear**. It is now checked, retried automatically, and
> reported as a failure rather than as "no networks".

### Playback transfers, then jumps back

Something else is asserting control — usually the Spotify app still open and
active on another device. The pin follows the device you chose, but the
Spotify app can transfer it straight back.

### Everything reads fine, but only Play works

Fixed in this build — recorded because it is a trap for anything built on
Arduino's `HTTPClient`.

Spotify's edge rejects a bodyless `PUT`/`POST` that carries no
`Content-Length`, with **411 Length Required**, *before* it looks at the
token. `HTTPClient::sendRequest()` only emits the header when there is a
payload:

```cpp
if(payload && size > 0) {
    addHeader(F("Content-Length"), String(size));
}
```

So `play` worked — it is the only command that sends a body (`{}`) — and
pause, next, previous, seek, volume, shuffle and repeat all got 411.
Verified against the live API:

| Request | Result |
|---|---|
| `PUT /v1/me/player/pause`, no `Content-Length` | **411 Length Required** |
| `PUT /v1/me/player/pause`, `Content-Length: 0` | 401 — shape accepted |

It also explained a second symptom. The 411 comes back as **HTTP/1.0 with no
`Connection: keep-alive`**, so the server closes the socket — meaning every
failed button press destroyed the kept-alive TLS session and the next poll
paid a fresh 1–2 s handshake. Broken controls and a sluggish display, one
root cause.

`apiSend()` now adds `Content-Length: 0` to every non-GET request without a
body, and a `411` case exists purely to say "firmware bug" if it ever
returns.

### A button flips back a moment after you press it

The UI updates optimistically the instant you press, because waiting for a
network round trip makes the panel feel broken. The poll that follows a
command is deliberately fast, to pick up what Spotify actually did. Those two
fight each other: Spotify often takes over a second to report a transport
change, so the fast poll arrives still saying *playing*, overwrites the
optimistic *paused*, and the button appears to have done nothing.

A successful command now records what it asked for, and for up to
`COMMAND_SETTLE_MS` (3 s) any polled value contradicting it is ignored. The
window closes the moment a poll agrees, so it is a ceiling rather than a
delay, and it is armed **only on success** — a rejected command must be
allowed to correct the optimistic value.

Raise `COMMAND_SETTLE_MS` if your playback device is slow to report; lower it
if a genuinely rejected command lingers on screen.

### Controls do nothing, message says "Not permitted"

Playback control requires Spotify Premium, or the device is restricted (TV
apps and some car head units reject remote control). The display half keeps
working either way.

### "No active device"

Spotify has nothing to control. Start playback on any device; it appears within
a poll interval. This is a normal state, not a fault.

### TLS handshake failures after months of working

A pinned root has probably rotated. Confirm:

```bash
openssl s_client -connect api.spotify.com:443 -servername api.spotify.com \
  </dev/null 2>/dev/null | grep -E '^ *[0-9] s:'
```

Regenerate by extracting the new root from your system trust store into
`src/SpotifyCerts.h`, keeping the existing `PROGMEM` string layout, and verify
before flashing:

```bash
openssl s_client -connect api.spotify.com:443 -servername api.spotify.com \
  -CAfile /etc/ssl/certs/<NewRoot>.pem </dev/null 2>&1 | grep 'Verify return code'
# want: Verify return code: 0 (ok)
```

As an emergency bypass, set `TLS_VERIFY_CERTIFICATES 0` in `config.h` — but
understand that anything on your network can then impersonate Spotify and
harvest the refresh token in flight.

### Artwork never appears

Check the serial log. `art: ... buffer reserved` should appear at boot; if the
48 KB allocation failed, artwork is disabled for the session. `artwork HTTP
404` or `image too large` indicates a specific cover, and that cover is retried
after 30 s. Reduce `ART_BOX_PX` or `ART_MAX_BYTES` in `config.h` if the heap is
tight.

### Reboots / `Guru Meditation` after a while

Watch the 30-second heap line in the serial log. A falling *min-ever* figure
means a leak. `monitor_filters = esp32_exception_decoder` is already enabled,
so backtraces come out symbolised.
# Spotify-remote-and-Now-Playing-display
