/* =========================================================================
 *  PlayerParser.h - turn a /v1/me/player response into a PlaybackState.
 *
 *  Deliberately free of any networking dependency. Parsing is the part of
 *  the Spotify integration most likely to be wrong and least likely to
 *  announce it - a mistyped field name yields a blank label, not a crash -
 *  so it is separated from the transport and exercised by host-side tests
 *  in test/test_player_parser.cpp. Run them with tools/run_parser_tests.sh.
 * ====================================================================== */
#pragma once

#include <ArduinoJson.h>

#include "AppState.h"

namespace PlayerParser {

/* Populate the ArduinoJson filter that keeps the parsed document to the
 * fields the UI actually uses. The raw payload runs to several kilobytes,
 * most of it available-market lists we have no use for. */
void buildFilter(JsonDocument &filter);

/* Apply a filtered /v1/me/player document to `st`.
 *
 * Returns a user-facing note to display, or nullptr when there is nothing
 * to say. `artChooser` selects an artwork URL from a Spotify images array;
 * it is injected so this module stays independent of the display. */
using ArtChooser = void (*)(JsonVariantConst images, char *out, size_t outLen);

const char *apply(JsonDocument &doc, PlaybackState &st, ArtChooser artChooser);

/* Populate the filter for GET /v1/me/player/devices. */
void buildDevicesFilter(JsonDocument &filter);

/* Apply a filtered devices document to `list`. Returns the number of
 * devices stored; anything past DeviceList::MAX is dropped. */
uint8_t applyDevices(JsonDocument &doc, DeviceList &list);

}  // namespace PlayerParser
