/* =========================================================================
 *  Host-side tests for PlayerParser against realistic /v1/me/player
 *  payloads. Build and run with tools/run_parser_tests.sh.
 *
 *  These cover the cases that are awkward to reach on the device: a
 *  podcast episode, a private session, an advert, a device that cannot
 *  change its own volume, and artwork selection. Each payload is trimmed
 *  from the real API response shape, keeping the nesting intact.
 * ====================================================================== */
#include <Arduino.h>          /* the host stub in test/host */
#include <ArduinoJson.h>

#include "../src/PlayerParser.h"
#include "config.h"        /* ART_BOX_PX - the box the chooser targets */

#include <cstdlib>
#include <iostream>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, what)                                                  \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::cout << "  FAIL: " << (what) << "  [" << #cond << "]\n";        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_STR(actual, expected, what)                                  \
  do {                                                                     \
    ++g_checks;                                                            \
    if (std::strcmp((actual), (expected)) != 0) {                          \
      std::cout << "  FAIL: " << (what) << "\n      expected: \""          \
                << (expected) << "\"\n      actual:   \"" << (actual)      \
                << "\"\n";                                                 \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

/* The real chooser lives in AlbumArt.cpp, which pulls in the TFT stack.
 * This is a faithful copy of its selection rule so the test exercises the
 * same decision the firmware makes. */
static void testArtChooser(JsonVariantConst images, char *out, size_t outLen) {
  out[0] = '\0';
  JsonArrayConst arr = images.as<JsonArrayConst>();
  if (arr.isNull()) return;
  const char *best = nullptr;
  int bestW = INT32_MAX;
  const char *fallback = nullptr;
  int fallbackW = -1;
  for (JsonObjectConst img : arr) {
    const char *u = img["url"] | "";
    if (!*u) continue;
    const int w = img["width"] | 0;
    if (w >= ART_BOX_PX && w < bestW) { bestW = w; best = u; }
    if (w > fallbackW) { fallbackW = w; fallback = u; }
  }
  const char *chosen = best ? best : fallback;
  if (chosen) strlcpy(out, chosen, outLen);
}

static const char *run(const char *json, PlaybackState &st) {
  JsonDocument filter;
  PlayerParser::buildFilter(filter);
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    std::cout << "  FAIL: deserialize: " << err.c_str() << "\n";
    ++g_failures;
    return nullptr;
  }
  return PlayerParser::apply(doc, st, testArtChooser);
}

/* ---------------------------------------------------------------------- */
static void testTrack() {
  std::cout << "track with multiple artists\n";
  static const char kJson[] = R"({
    "device": {"id":"abc","is_active":true,"is_private_session":false,
               "is_restricted":false,"name":"Living Room","type":"Speaker",
               "volume_percent":62,"supports_volume":true},
    "repeat_state":"off","shuffle_state":false,
    "progress_ms": 67000, "is_playing": true,
    "currently_playing_type":"track",
    "item": {
      "album": {"album_type":"album","id":"ALBUM123","name":"Random Access Memories",
        "images":[{"height":640,"url":"https://i.scdn.co/image/big","width":640},
                  {"height":300,"url":"https://i.scdn.co/image/mid","width":300},
                  {"height":64,"url":"https://i.scdn.co/image/tiny","width":64}],
        "available_markets":["US","GB","SE","DE","FR"]},
      "artists":[{"id":"a1","name":"Daft Punk"},{"id":"a2","name":"Pharrell Williams"}],
      "duration_ms": 224000, "id":"TRACK99", "name":"Get Lucky", "type":"track",
      "available_markets":["US","GB"]
    }
  })";
  PlaybackState st;
  const char *note = run(kJson, st);

  CHECK(note == nullptr, "no note for a normal track");
  CHECK(st.hasItem, "hasItem");
  CHECK(st.hasDevice, "hasDevice");
  CHECK(st.isPlaying, "isPlaying");
  CHECK(st.kind == ItemKind::Track, "kind is Track");
  CHECK_STR(st.title, "Get Lucky", "title");
  CHECK_STR(st.artist, "Daft Punk, Pharrell Williams", "artists joined");
  CHECK_STR(st.album, "Random Access Memories", "album");
  CHECK_STR(st.device, "Living Room", "device");
  CHECK_STR(st.artKey, "ALBUM123", "art cache key is the ALBUM id");
  CHECK_STR(st.artUrl, "https://i.scdn.co/image/mid",
            "smallest image that still covers the 160px box");
  CHECK(st.durationMs == 224000, "duration");
  CHECK(st.progressMs == 67000, "progress");
  CHECK(st.volumePercent == 62, "volume");
  CHECK(st.supportsVolume, "supportsVolume");
}

static void testEpisode() {
  std::cout << "podcast episode\n";
  static const char kJson[] = R"({
    "device":{"name":"Phone","volume_percent":30,"supports_volume":true,
              "is_private_session":false,"is_restricted":false},
    "progress_ms": 512000, "is_playing": true,
    "currently_playing_type":"episode",
    "item":{
      "id":"EP42","name":"The One About Embedded Systems","type":"episode",
      "duration_ms": 3600000,
      "images":[{"height":640,"url":"https://i.scdn.co/image/ep-big","width":640},
                {"height":300,"url":"https://i.scdn.co/image/ep-mid","width":300}],
      "show":{"id":"SHOW7","name":"Hardware Hour","publisher":"Some Network"}
    }
  })";
  PlaybackState st;
  const char *note = run(kJson, st);

  CHECK(note == nullptr, "no note for an episode");
  CHECK(st.kind == ItemKind::Episode, "kind is Episode");
  CHECK_STR(st.title, "The One About Embedded Systems", "episode title");
  CHECK_STR(st.artist, "Some Network", "publisher stands in for artist");
  CHECK_STR(st.album, "Hardware Hour", "show stands in for album");
  CHECK_STR(st.artKey, "EP42", "episode art keyed by EPISODE id");
  CHECK_STR(st.artUrl, "https://i.scdn.co/image/ep-mid", "episode art chosen");
  CHECK(st.durationMs == 3600000, "long duration survives");
}

static void testPrivateSession() {
  std::cout << "private session\n";
  static const char kJson[] = R"({
    "device":{"name":"Laptop","volume_percent":100,"is_private_session":true,
              "is_restricted":false,"supports_volume":true},
    "progress_ms":null,"is_playing":true,
    "currently_playing_type":"unknown","item":null
  })";
  PlaybackState st;
  const char *note = run(kJson, st);

  CHECK(note != nullptr, "a private session must explain itself");
  if (note) CHECK_STR(note, "Private session - details hidden", "private note");
  CHECK(!st.hasItem, "no item");
  CHECK(st.hasDevice, "device still known");
  CHECK(st.isPrivate, "isPrivate survives clearItem()");
  CHECK_STR(st.device, "Laptop", "device name still shown");
}

static void testAdvert() {
  std::cout << "advert\n";
  static const char kJson[] = R"({
    "device":{"name":"Phone","volume_percent":45,"is_private_session":false,
              "is_restricted":false},
    "progress_ms":5000,"is_playing":true,
    "currently_playing_type":"ad","item":null
  })";
  PlaybackState st;
  const char *note = run(kJson, st);

  CHECK(note == nullptr, "an advert needs no warning note");
  CHECK(st.hasItem, "advert counts as something playing");
  CHECK_STR(st.title, "Advertisement", "advert title");
  CHECK_STR(st.artUrl, "", "no artwork for an advert");
  CHECK(st.isPlaying, "advert is playing");
}

static void testRestrictedDevice() {
  std::cout << "restricted device / no remote volume\n";
  static const char kJson[] = R"({
    "device":{"name":"Living Room TV","volume_percent":50,
              "is_private_session":false,"is_restricted":true,
              "supports_volume":true},
    "progress_ms":1000,"is_playing":false,"currently_playing_type":"track",
    "item":{"id":"T1","name":"Song","type":"track","duration_ms":1000,
            "artists":[{"name":"A"}],
            "album":{"id":"AL1","name":"Alb","images":[]}}
  })";
  PlaybackState st;
  run(kJson, st);

  CHECK(!st.supportsVolume, "restricted device must not offer a slider");
  CHECK(st.volumePercent == 50, "reported volume is still displayed");
  CHECK(!st.isPlaying, "paused");
  CHECK_STR(st.artUrl, "", "empty images array yields no url");
}

static void testNoDeviceKey() {
  std::cout << "response with no device object\n";
  static const char kJson[] = R"({"progress_ms":0,"is_playing":false,"item":null})";
  PlaybackState st;
  strlcpy(st.device, "stale", sizeof(st.device));
  st.volumePercent = 99;
  run(kJson, st);

  CHECK(!st.hasDevice, "hasDevice cleared");
  CHECK_STR(st.device, "", "stale device name cleared");
  CHECK(st.volumePercent == -1, "volume marked unknown");
}

static void testSmallArtOnly() {
  std::cout << "artwork smaller than the box\n";
  static const char kJson[] = R"({
    "device":{"name":"D","volume_percent":10},
    "progress_ms":0,"is_playing":true,"currently_playing_type":"track",
    "item":{"id":"T","name":"N","type":"track","duration_ms":1,
      "artists":[{"name":"A"}],
      "album":{"id":"AL","name":"Al","images":[
        {"url":"https://i.scdn.co/image/s64","width":64,"height":64}]}}
  })";
  PlaybackState st;
  run(kJson, st);
  CHECK_STR(st.artUrl, "https://i.scdn.co/image/s64",
            "falls back to the largest available when none covers the box");
}

static void testShuffleRepeat() {
  std::cout << "shuffle and repeat state\n";
  static const char kJson[] = R"({
    "device":{"id":"DEV1","name":"Desk","type":"Computer","volume_percent":40,
              "supports_volume":true,"is_restricted":false},
    "shuffle_state":true,"repeat_state":"track",
    "progress_ms":10,"is_playing":true,"currently_playing_type":"track",
    "item":{"id":"T","name":"N","type":"track","duration_ms":100,
      "artists":[{"name":"A"}],"album":{"id":"AL","name":"Al","images":[]}}
  })";
  PlaybackState st;
  run(kJson, st);
  CHECK(st.shuffle, "shuffle_state true");
  CHECK(st.repeat == RepeatMode::Track, "repeat_state track");
  CHECK_STR(st.deviceId, "DEV1", "active device id captured for targeting");
  CHECK_STR(st.deviceType, "Computer", "device type captured");
}

static void testRepeatRoundTrip() {
  std::cout << "repeat mode mapping\n";
  CHECK(repeatFromApi("off") == RepeatMode::Off, "off");
  CHECK(repeatFromApi("context") == RepeatMode::Context, "context");
  CHECK(repeatFromApi("track") == RepeatMode::Track, "track");
  CHECK(repeatFromApi(nullptr) == RepeatMode::Off, "null defaults to off");
  CHECK_STR(repeatToApi(RepeatMode::Context), "context", "to api context");
  CHECK_STR(repeatToApi(RepeatMode::Track), "track", "to api track");
  CHECK_STR(repeatToApi(RepeatMode::Off), "off", "to api off");
  /* The button cycles off -> context -> track -> off. */
  CHECK(repeatNext(RepeatMode::Off) == RepeatMode::Context, "cycle 1");
  CHECK(repeatNext(RepeatMode::Context) == RepeatMode::Track, "cycle 2");
  CHECK(repeatNext(RepeatMode::Track) == RepeatMode::Off, "cycle wraps");
}

static void testDeviceList() {
  std::cout << "device list\n";
  static const char kJson[] = R"({"devices":[
    {"id":"D1","is_active":true,"is_private_session":false,"is_restricted":false,
     "name":"Living Room TV","type":"TV","volume_percent":35,"supports_volume":true},
    {"id":"D2","is_active":false,"is_restricted":true,
     "name":"Car","type":"Automobile","volume_percent":80,"supports_volume":true},
    {"id":"D3","is_active":false,"is_restricted":false,
     "name":"Phone","type":"Smartphone","volume_percent":null,"supports_volume":false},
    {"id":"","name":"Ghost","type":"Unknown","is_active":false}
  ]})";
  JsonDocument filter;
  PlayerParser::buildDevicesFilter(filter);
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, kJson, DeserializationOption::Filter(filter));
  CHECK(!err, "devices deserialize");
  DeviceList list;
  const uint8_t n = PlayerParser::applyDevices(doc, list);

  CHECK(n == 3, "the id-less device is skipped");
  CHECK(list.valid, "list marked valid");
  CHECK_STR(list.items[0].name, "Living Room TV", "first device name");
  CHECK_STR(list.items[0].type, "TV", "first device type");
  CHECK(list.items[0].isActive, "first device is active");
  CHECK(list.items[0].volumePercent == 35, "first device volume");
  CHECK(list.items[0].supportsVolume, "first device volume controllable");

  CHECK(list.items[1].isRestricted, "car is restricted");
  CHECK(!list.items[1].supportsVolume,
        "a restricted device must not advertise volume control");

  CHECK(list.items[2].volumePercent == -1, "null volume becomes unknown");
  CHECK(!list.items[2].supportsVolume, "no volume support reported");

  const SpotifyDevice *a = list.active();
  CHECK(a != nullptr && strcmp(a->id, "D1") == 0, "active() finds D1");
  CHECK(list.byId("D2") != nullptr, "byId finds D2");
  CHECK(list.byId("nope") == nullptr, "byId misses unknown");
  CHECK(list.byId("") == nullptr, "byId rejects empty");
}

static void testDeviceListOverflow() {
  std::cout << "device list overflow is bounded\n";
  std::string j = "{\"devices\":[";
  for (int i = 0; i < 20; ++i) {
    if (i) j += ",";
    j += "{\"id\":\"D" + std::to_string(i) +
         "\",\"name\":\"Dev\",\"type\":\"Speaker\",\"is_active\":false}";
  }
  j += "]}";
  JsonDocument filter;
  PlayerParser::buildDevicesFilter(filter);
  JsonDocument doc;
  deserializeJson(doc, j, DeserializationOption::Filter(filter));
  DeviceList list;
  const uint8_t n = PlayerParser::applyDevices(doc, list);
  CHECK(n == DeviceList::MAX, "clamped to MAX");
  CHECK(list.count <= DeviceList::MAX, "count never exceeds capacity");
}

static void testEmptyDeviceList() {
  std::cout << "no devices available\n";
  static const char kJson[] = R"({"devices":[]})";
  JsonDocument filter;
  PlayerParser::buildDevicesFilter(filter);
  JsonDocument doc;
  deserializeJson(doc, kJson, DeserializationOption::Filter(filter));
  DeviceList list;
  const uint8_t n = PlayerParser::applyDevices(doc, list);
  CHECK(n == 0, "zero devices");
  CHECK(list.valid, "an empty list is still a valid answer");
  CHECK(list.active() == nullptr, "no active device");
}

static void testDevicePinPolicy() {
  std::cout << "command-target pin: following Spotify Connect\n";

  /* No pin: commands already go to whatever is active. */
  CHECK(!shouldReleaseDevicePin("", "D2", false), "nothing pinned");
  CHECK(!shouldReleaseDevicePin(nullptr, "D2", false), "null pin");

  /* Pinned and still the active device - hold it. */
  CHECK(!shouldReleaseDevicePin("D1", "D1", false), "pin matches active");

  /* The case the whole policy exists for: the user moved playback in the
   * Spotify app, so our selection is stale and must be released. */
  CHECK(shouldReleaseDevicePin("D1", "D2", false),
        "playback moved elsewhere - release the pin and follow");

  /* ...but NOT while our own transfer is still settling, or the very next
   * command would go back to the device we just moved away from. */
  CHECK(!shouldReleaseDevicePin("D1", "D2", true),
        "transfer grace window protects a fresh selection");

  /* Nothing active anywhere: keep the pin, because a Play press should
   * wake the device the user chose. */
  CHECK(!shouldReleaseDevicePin("D1", "", false), "no active device keeps the pin");
  CHECK(!shouldReleaseDevicePin("D1", nullptr, false), "null active keeps the pin");
}

static void testPlayerIntent() {
  std::cout << "command intent: surviving a poll that has not caught up\n";
  const uint32_t NOW = 100000;          /* the shim's fixed clock */

  /* The exact failure this exists for: tap pause, poll arrives 700 ms
   * later still reporting playing, and the button looks dead. */
  {
    PlaybackState st; st.isPlaying = true;          /* what the poll said */
    PlayerIntent in; in.hasPlaying = true; in.playing = false;
    in.arm(NOW, 3000);
    const bool guarding = in.reconcile(st, NOW + 700);
    CHECK(!st.isPlaying, "optimistic pause survives a stale poll");
    CHECK(guarding, "still guarding while Spotify disagrees");
  }

  /* Once Spotify agrees, stop overriding - the guard is not a fixed delay. */
  {
    PlaybackState st; st.isPlaying = false;
    PlayerIntent in; in.hasPlaying = true; in.playing = false;
    in.arm(NOW, 3000);
    CHECK(!in.reconcile(st, NOW + 700), "guard released once the poll agrees");
  }

  /* After the window, the server wins even if it still disagrees -
   * otherwise a silently-ignored command would be invisible forever. */
  {
    PlaybackState st; st.isPlaying = true;
    PlayerIntent in; in.hasPlaying = true; in.playing = false;
    in.arm(NOW, 3000);
    CHECK(!in.reconcile(st, NOW + 3001), "window expires");
    CHECK(st.isPlaying, "expired guard does not touch the polled value");
  }

  /* Unarmed intent must never override anything. */
  {
    PlaybackState st; st.isPlaying = true;
    PlayerIntent in; in.hasPlaying = true; in.playing = false;   /* not armed */
    CHECK(!in.reconcile(st, NOW), "unarmed intent is inert");
    CHECK(st.isPlaying, "unarmed intent leaves the polled value alone");
  }

  /* Shuffle and repeat have the same race. */
  {
    PlaybackState st; st.shuffle = false; st.repeat = RepeatMode::Off;
    PlayerIntent in;
    in.hasShuffle = true; in.shuffle = true;
    in.hasRepeat  = true; in.repeat  = RepeatMode::Track;
    in.arm(NOW, 3000);
    in.reconcile(st, NOW + 700);
    CHECK(st.shuffle, "shuffle intent survives");
    CHECK(st.repeat == RepeatMode::Track, "repeat intent survives");
  }

  /* Volume: some speakers quantise, so near-misses count as agreement
   * rather than fighting the hardware for the whole window. */
  {
    PlaybackState st; st.volumePercent = 49;
    PlayerIntent in; in.hasVolume = true; in.volume = 50;
    in.arm(NOW, 3000);
    CHECK(!in.reconcile(st, NOW + 700), "1% off counts as agreement");
    CHECK(st.volumePercent == 49, "device's own rounding is respected");
  }
  {
    PlaybackState st; st.volumePercent = 20;
    PlayerIntent in; in.hasVolume = true; in.volume = 50;
    in.arm(NOW, 3000);
    CHECK(in.reconcile(st, NOW + 700), "a real disagreement is guarded");
    CHECK(st.volumePercent == 50, "requested volume restored");
  }

  /* An unknown volume (-1) must not be overwritten - that is "the device
   * does not report one", not "the device disagrees". */
  {
    PlaybackState st; st.volumePercent = -1;
    PlayerIntent in; in.hasVolume = true; in.volume = 50;
    in.arm(NOW, 3000);
    in.reconcile(st, NOW + 700);
    CHECK(st.volumePercent == -1, "unknown volume left as unknown");
  }

  /* clear() really disarms. */
  {
    PlaybackState st; st.isPlaying = true;
    PlayerIntent in; in.hasPlaying = true; in.playing = false;
    in.arm(NOW, 3000); in.clear();
    CHECK(!in.armed(), "clear() disarms");
    CHECK(!in.reconcile(st, NOW + 100), "cleared intent is inert");
  }
}

int main() {
  std::cout << "PlayerParser host tests (ART_BOX_PX=" << ART_BOX_PX << ")\n\n";
  testTrack();
  testEpisode();
  testPrivateSession();
  testAdvert();
  testRestrictedDevice();
  testNoDeviceKey();
  testSmallArtOnly();
  testShuffleRepeat();
  testRepeatRoundTrip();
  testDeviceList();
  testDeviceListOverflow();
  testEmptyDeviceList();
  testDevicePinPolicy();
  testPlayerIntent();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
            << " checks passed\n";
  if (g_failures) std::cout << g_failures << " FAILURE(S)\n";
  return g_failures ? 1 : 0;
}
