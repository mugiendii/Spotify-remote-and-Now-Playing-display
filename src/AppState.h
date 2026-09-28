/* =========================================================================
 *  AppState.h - the shared playback model.
 *
 *  Every field is a fixed-size buffer. Nothing here allocates: the whole
 *  struct is a single static instance owned by main.cpp, so the UI can hold
 *  a second copy as its "last drawn" snapshot and diff the two without any
 *  heap traffic on the render path.
 *
 *  Ownership: SpotifyClient writes it, DisplayManager reads it. Both run on
 *  the Arduino loop task, so no locking is needed. See README "Concurrency".
 * ====================================================================== */
#pragma once

#include <Arduino.h>

/* Field capacities. Spotify track names can be genuinely long; these are
 * sized so that realistic metadata is never truncated at the model layer
 * (the UI truncates or scrolls at the pixel level instead). */
#define ID_LEN        40      /* Spotify base-62 IDs are 22 chars          */
#define TITLE_LEN    160
#define ARTIST_LEN   160
#define ALBUM_LEN    128
#define DEVICE_LEN    64
#define URL_LEN      256
#define MESSAGE_LEN   96

/* What is loaded in the player. */
enum class ItemKind : uint8_t { None, Track, Episode };

/* Spotify's repeat_state, verbatim: "off" | "context" | "track". */
enum class RepeatMode : uint8_t { Off, Context, Track };

inline const char *repeatToApi(RepeatMode m) {
  switch (m) {
    case RepeatMode::Context: return "context";
    case RepeatMode::Track:   return "track";
    default:                  return "off";
  }
}

inline RepeatMode repeatFromApi(const char *s) {
  if (!s) return RepeatMode::Off;
  if (strcmp(s, "context") == 0) return RepeatMode::Context;
  if (strcmp(s, "track") == 0)   return RepeatMode::Track;
  return RepeatMode::Off;
}

/* Cycle order for the on-screen repeat button. */
inline RepeatMode repeatNext(RepeatMode m) {
  switch (m) {
    case RepeatMode::Off:     return RepeatMode::Context;
    case RepeatMode::Context: return RepeatMode::Track;
    default:                  return RepeatMode::Off;
  }
}

/* Which screen the UI is showing. */
enum class Screen : uint8_t {
  Boot,          /* splash while Wi-Fi comes up                           */
  NowPlaying,    /* the main display                                      */
  Devices,       /* pick a Spotify Connect device / transfer playback     */
  WifiList,      /* scanned networks                                      */
  WifiPassword,  /* on-screen keyboard                                    */
  Setup          /* "open http://<ip>/ to sign in"                        */
};

/* Wi-Fi link state, for the status bar indicator. */
enum class NetState : uint8_t { Down, Connecting, Up };

/* Our relationship with the Spotify API, for the status bar indicator. */
enum class ApiState : uint8_t {
  Init,          /* nothing attempted yet                                  */
  NoToken,       /* no access token and no way to get one                  */
  Refreshing,    /* token refresh in flight                                */
  Ok,            /* last request succeeded                                 */
  NoDevice,      /* authenticated, but no active Spotify device            */
  RateLimited,   /* 429 - backing off until retryAt                        */
  AuthFailed,    /* 401/400 on refresh - the refresh token is dead         */
  Error          /* transport or server error, will retry                  */
};

/* -------------------------------------------------------------------------
 *  PlaybackState - what is playing, as far as we last knew.
 * ---------------------------------------------------------------------- */
struct PlaybackState {
  ItemKind kind          = ItemKind::None;
  bool     hasItem       = false;   /* something is loaded in the player   */
  bool     hasDevice     = false;   /* an active device exists             */
  bool     isPlaying     = false;
  bool     supportsVolume = false;  /* device reports a volume we can set  */
  bool     isPrivate     = false;   /* private session: metadata withheld  */
  bool     shuffle       = false;
  RepeatMode repeat      = RepeatMode::Off;

  char itemId[ID_LEN]    = {0};     /* track/episode ID                    */
  char artKey[ID_LEN]    = {0};     /* album (or show) ID - artwork cache key */
  char title[TITLE_LEN]  = {0};
  char artist[ARTIST_LEN]= {0};     /* or the show's publisher             */
  char album[ALBUM_LEN]  = {0};     /* or the show name                    */
  char device[DEVICE_LEN]= {0};
  char deviceId[ID_LEN]  = {0};     /* the ACTIVE device, per Spotify      */
  char deviceType[24]    = {0};     /* "Computer", "Speaker", "TV", ...    */
  char artUrl[URL_LEN]   = {0};

  uint32_t durationMs    = 0;
  int16_t  volumePercent = -1;      /* -1 = unknown / not reportable       */

  /* Progress is stored as a sample plus the millis() at which it was taken,
   * so the UI can advance it locally between polls without ever drifting
   * away from the server's value - each poll re-anchors both fields. */
  uint32_t progressMs      = 0;
  uint32_t progressAnchor  = 0;

  /* Progress extrapolated to now. Playing tracks advance with the local
   * clock; paused ones sit still. Clamped to the track duration so a late
   * poll can never push the bar past the end. */
  uint32_t progressNow() const {
    if (!isPlaying) return progressMs;
    uint32_t elapsed = millis() - progressAnchor;
    uint32_t p = progressMs + elapsed;
    return (durationMs && p > durationMs) ? durationMs : p;
  }

  void anchorProgress(uint32_t ms) {
    progressMs     = ms;
    progressAnchor = millis();
  }

  /* Clears the ITEM only. isPrivate, device and volume belong to the
   * device and survive - a private session is precisely the case where
   * there is a device but no item, and wiping the flag here would throw
   * away the one piece of information that explains the blank screen. */
  void clearItem() {
    kind = ItemKind::None;
    hasItem = false;
    isPlaying = false;
    itemId[0] = artKey[0] = title[0] = artist[0] = album[0] = artUrl[0] = '\0';
    durationMs = 0;
    progressMs = 0;
    progressAnchor = millis();
  }
};

/* -------------------------------------------------------------------------
 *  SpotifyDevice / DeviceList - the result of GET /v1/me/player/devices.
 *
 *  Fixed-capacity, like everything else here: the list is refreshed on a
 *  timer and rendered straight from this array, so there is no allocation
 *  anywhere on the path from HTTP response to pixels.
 * ---------------------------------------------------------------------- */
struct SpotifyDevice {
  char    id[ID_LEN]        = {0};
  char    name[DEVICE_LEN]  = {0};
  char    type[24]          = {0};   /* Computer, Smartphone, Speaker, TV  */
  bool    isActive          = false;
  bool    isRestricted      = false; /* rejects every Web API command      */
  bool    supportsVolume    = false;
  int16_t volumePercent     = -1;
};

struct DeviceList {
  /* Eight covers a generous household. Extra devices are dropped with a
   * log line rather than silently truncating the user's choice. */
  static constexpr uint8_t MAX = 8;
  SpotifyDevice items[MAX];
  uint8_t       count     = 0;
  uint32_t      updatedAt = 0;     /* millis() of the last successful fetch */
  bool          valid     = false;

  const SpotifyDevice *byId(const char *id) const {
    if (!id || !*id) return nullptr;
    for (uint8_t i = 0; i < count; ++i) {
      if (strcmp(items[i].id, id) == 0) return &items[i];
    }
    return nullptr;
  }

  const SpotifyDevice *active() const {
    for (uint8_t i = 0; i < count; ++i) {
      if (items[i].isActive) return &items[i];
    }
    return nullptr;
  }
};

/* -------------------------------------------------------------------------
 *  Command-target policy.
 *
 *  Two requirements pull against each other:
 *
 *    - commands go to the explicitly selected device, when there is one;
 *    - the device must follow playback when the user switches devices in
 *      the Spotify app.
 *
 *  Holding a pin forever satisfies the first and breaks the second: press
 *  Next after moving playback to your phone and the old device wakes up
 *  instead. Releasing the pin the moment the active device differs breaks
 *  the first, because Spotify keeps reporting the OLD device for a second
 *  or two after our own transfer request.
 *
 *  So: the pin holds through a grace window after we ask for a transfer,
 *  and is released afterwards if Spotify says something else is active -
 *  the user has chosen a different device, just not through our UI.
 *
 *  An empty activeId does NOT release the pin: nothing is playing anywhere,
 *  and a pinned device is exactly what a Play press should wake.
 * ---------------------------------------------------------------------- */
inline bool shouldReleaseDevicePin(const char *pinned, const char *activeId,
                                   bool transferGraceActive) {
  if (!pinned || !*pinned)        return false;   /* nothing pinned        */
  if (transferGraceActive)        return false;   /* our transfer settling */
  if (!activeId || !*activeId)    return false;   /* nothing active at all */
  return strcmp(pinned, activeId) != 0;
}

/* -------------------------------------------------------------------------
 *  AppStatus - everything the status bar and message line need that is not
 *  playback metadata.
 * ---------------------------------------------------------------------- */
struct AppStatus {
  NetState net      = NetState::Down;
  ApiState api      = ApiState::Init;
  int8_t   rssiBars = 0;                  /* 0..4                          */
  bool     artBusy  = false;              /* artwork download in flight    */
  bool     authed   = false;              /* we hold a usable refresh token */
  char     ip[16]   = {0};                /* for the "open http://..." hint */

  char     message[MESSAGE_LEN] = {0};    /* transient user-facing notice  */
  uint32_t messageUntil = 0;              /* millis() when it should clear */

  void setMessage(const char *m, uint32_t holdMs) {
    strlcpy(message, m ? m : "", sizeof(message));
    messageUntil = millis() + holdMs;
  }

  void expireMessage() {
    if (message[0] && (int32_t)(millis() - messageUntil) >= 0) message[0] = '\0';
  }
};
