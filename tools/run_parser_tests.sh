#!/usr/bin/env bash
# Compile and run the host-side tests (AuthUtil + PlayerParser).
#
# Uses the ArduinoJson copy PlatformIO already downloaded, so run a device
# build first (`pio run`) if .pio/libdeps is not there yet.
set -euo pipefail
cd "$(dirname "$0")/.."

AJ=.pio/libdeps/esp32dev/ArduinoJson/src
if [ ! -d "$AJ" ]; then
  echo "ArduinoJson not found at $AJ - run 'pio run' once first." >&2
  exit 1
fi

TMP=$(mktemp -d)
fail=0

echo "--- AuthUtil ---"
g++ -std=c++17 -Wall -Wextra -O1 \
    -I src -I test \
    test/test_auth_util.cpp \
    -o "$TMP/auth_tests"
"$TMP/auth_tests" || fail=1

echo
echo "--- PlayerParser ---"
g++ -std=c++17 -Wall -Wextra -O1 \
    -I test/host -I "$AJ" -I include -I src -I test \
    test/test_player_parser.cpp src/PlayerParser.cpp \
    -o "$TMP/parser_tests"
"$TMP/parser_tests" || fail=1

exit $fail
