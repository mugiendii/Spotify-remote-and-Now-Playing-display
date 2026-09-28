#include "PlayerParser.h"

void PlayerParser::buildFilter(JsonDocument &filter) {
  filter["is_playing"]             = true;
  filter["progress_ms"]            = true;
  filter["currently_playing_type"] = true;

  filter["shuffle_state"]          = true;
  filter["repeat_state"]           = true;

  JsonObject dev = filter["device"].to<JsonObject>();
  dev["id"]                 = true;
  dev["type"]               = true;
  dev["name"]               = true;
  dev["volume_percent"]     = true;
  dev["supports_volume"]    = true;
  dev["is_private_session"] = true;
  dev["is_restricted"]      = true;

  JsonObject item = filter["item"].to<JsonObject>();
  item["id"]          = true;
  item["name"]        = true;
  item["duration_ms"] = true;
  item["type"]        = true;
  /* Index 0 in a filter means "every element of this array". */
  item["artists"][0]["name"] = true;
  item["images"][0]["url"]   = true;
  item["images"][0]["width"] = true;

  JsonObject album = item["album"].to<JsonObject>();
  album["id"]                 = true;
  album["name"]               = true;
  album["images"][0]["url"]   = true;
  album["images"][0]["width"] = true;

  JsonObject show = item["show"].to<JsonObject>();
  show["id"]        = true;
  show["name"]      = true;
  show["publisher"] = true;
}

const char *PlayerParser::apply(JsonDocument &doc, PlaybackState &st,
                                ArtChooser artChooser) {
  const char *note = nullptr;

  /* ---- device -------------------------------------------------------- */
  /* Shuffle and repeat live at the top level, not on the device. */
  st.shuffle = doc["shuffle_state"] | false;
  st.repeat  = repeatFromApi(doc["repeat_state"] | "off");

  JsonObject device = doc["device"];
  if (!device.isNull()) {
    st.hasDevice = true;
    strlcpy(st.device, device["name"] | "Unknown device", sizeof(st.device));
    strlcpy(st.deviceId, device["id"] | "", sizeof(st.deviceId));
    strlcpy(st.deviceType, device["type"] | "", sizeof(st.deviceType));
    st.isPrivate = device["is_private_session"] | false;

    /* A restricted device (a TV app, some car head units) rejects every
     * control call, so do not offer a slider that cannot work.
     * supports_volume is a newer field; when it is absent, fall back to
     * "a volume was reported at all". */
    const bool restricted = device["is_restricted"] | false;
    if (device["volume_percent"].isNull()) {
      st.volumePercent  = -1;
      st.supportsVolume = false;
    } else {
      st.volumePercent  = device["volume_percent"].as<int>();
      st.supportsVolume = (device["supports_volume"] | true) && !restricted;
    }
  } else {
    st.hasDevice      = false;
    st.isPrivate      = false;
    st.device[0]      = '\0';
    st.deviceId[0]    = '\0';
    st.deviceType[0]  = '\0';
    st.volumePercent  = -1;
    st.supportsVolume = false;
  }

  /* ---- item ---------------------------------------------------------- */
  const char *playingType = doc["currently_playing_type"] | "";
  JsonObject  it          = doc["item"];

  if (it.isNull()) {
    /* Item withheld. Three realistic causes, and the user deserves to know
     * which: a private session hides metadata, an advert has no track, and
     * Spotify's catch-all "unknown" covers local files and contexts the
     * API will not describe. */
    st.clearItem();
    if (st.isPrivate) {
      note = "Private session - details hidden";
    } else if (strcmp(playingType, "ad") == 0) {
      st.hasItem = true;
      strlcpy(st.title, "Advertisement", sizeof(st.title));
    } else if (st.hasDevice) {
      note = "No track information available";
    }
    st.isPlaying = doc["is_playing"] | false;
    st.anchorProgress(doc["progress_ms"] | 0UL);
    return note;
  }

  const char *type = it["type"] | "track";
  st.kind       = (strcmp(type, "episode") == 0) ? ItemKind::Episode : ItemKind::Track;
  st.hasItem    = true;
  st.isPlaying  = doc["is_playing"] | false;
  st.durationMs = it["duration_ms"] | 0UL;
  st.anchorProgress(doc["progress_ms"] | 0UL);
  strlcpy(st.itemId, it["id"] | "", sizeof(st.itemId));
  strlcpy(st.title,  it["name"] | "", sizeof(st.title));

  if (st.kind == ItemKind::Episode) {
    /* Podcasts: the show stands in for the album, the publisher for the
     * artist, and the artwork hangs off the episode rather than an album.
     * Cache by episode ID - unlike an album cover, episode art differs
     * from one episode to the next. */
    JsonObject sh = it["show"];
    strlcpy(st.artist, sh["publisher"] | "", sizeof(st.artist));
    strlcpy(st.album,  sh["name"] | "", sizeof(st.album));
    strlcpy(st.artKey, it["id"] | "", sizeof(st.artKey));
    artChooser(it["images"], st.artUrl, sizeof(st.artUrl));
  } else {
    /* Join every credited artist rather than taking the first - "feat."
     * collaborations read wrong otherwise. The band scrolls if it is long. */
    st.artist[0] = '\0';
    bool first = true;
    /* Bind the array to a named local: iterating directly over the
     * subscript expression leaves the proxy temporary dangling for the
     * duration of the loop (GCC 15 -Wdangling-reference catches it). */
    JsonArray artists = it["artists"].as<JsonArray>();
    for (JsonObject a : artists) {
      const char *n = a["name"] | "";
      if (!*n) continue;
      if (!first) strlcat(st.artist, ", ", sizeof(st.artist));
      strlcat(st.artist, n, sizeof(st.artist));
      first = false;
    }
    JsonObject al = it["album"];
    strlcpy(st.album, al["name"] | "", sizeof(st.album));
    /* Cache artwork by ALBUM id, not track id: skipping between tracks on
     * one album must not re-download the same cover. */
    strlcpy(st.artKey, al["id"] | "", sizeof(st.artKey));
    artChooser(al["images"], st.artUrl, sizeof(st.artUrl));
  }
  return note;
}

/* =========================================================================
 *  GET /v1/me/player/devices
 * ====================================================================== */
void PlayerParser::buildDevicesFilter(JsonDocument &filter) {
  JsonObject d = filter["devices"][0].to<JsonObject>();
  d["id"]              = true;
  d["name"]            = true;
  d["type"]            = true;
  d["is_active"]       = true;
  d["is_restricted"]   = true;
  d["supports_volume"] = true;
  d["volume_percent"]  = true;
}

uint8_t PlayerParser::applyDevices(JsonDocument &doc, DeviceList &list) {
  list.count = 0;

  JsonArray arr = doc["devices"].as<JsonArray>();
  if (arr.isNull()) {
    list.valid = false;
    return 0;
  }

  for (JsonObject d : arr) {
    if (list.count >= DeviceList::MAX) break;
    const char *id = d["id"] | "";
    /* A device with no ID cannot be a transfer target, so it is useless to
     * us even though Spotify will happily list it. */
    if (!*id) continue;

    SpotifyDevice &dev = list.items[list.count];
    strlcpy(dev.id,   id,                        sizeof(dev.id));
    strlcpy(dev.name, d["name"] | "Unknown",     sizeof(dev.name));
    strlcpy(dev.type, d["type"] | "",            sizeof(dev.type));
    dev.isActive     = d["is_active"] | false;
    dev.isRestricted = d["is_restricted"] | false;

    if (d["volume_percent"].isNull()) {
      dev.volumePercent  = -1;
      dev.supportsVolume = false;
    } else {
      dev.volumePercent = d["volume_percent"].as<int>();
      /* A restricted device rejects every command including volume, so it
       * must not advertise volume support whatever the field claims. */
      dev.supportsVolume = (d["supports_volume"] | true) && !dev.isRestricted;
    }
    list.count++;
  }

  list.valid     = true;
  list.updatedAt = millis();
  return list.count;
}
