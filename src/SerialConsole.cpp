#include "SerialConsole.h"

#include <WiFi.h>

#include "InputManager.h"
#include "Log.h"
#include "NetManager.h"
#include "SpotifyClient.h"

void SerialConsole::begin(NetManager *net, SpotifyClient *spotify,
                          InputManager *input, PlaybackState *state,
                          AppStatus *status) {
  _net     = net;
  _spotify = spotify;
  _input   = input;
  _state   = state;
  _status  = status;
  Serial.println();
  Serial.println("Type 'help' for the recovery console.");
}

void SerialConsole::printHelp() const {
  Serial.println();
  Serial.println("  Recovery console");
  Serial.println("  ----------------");
  Serial.println("  status            what the device thinks is going on");
  Serial.println("  scan              list visible Wi-Fi networks");
  Serial.println("  ssid <name>       stage an SSID    (spaces are fine)");
  Serial.println("  pass <password>   stage a password (spaces are fine)");
  Serial.println("  join              save the staged credentials and connect");
  Serial.println("  forget            erase the stored Wi-Fi credentials");
  Serial.println("  touch             re-run the touch controller probe");
  Serial.println("  signout           erase the Spotify refresh token");
  Serial.println("  reboot            restart the device");
  Serial.println();
  Serial.println("  Example:  ssid MyHotspot");
  Serial.println("            pass hunter2");
  Serial.println("            join");
  Serial.println();
}

void SerialConsole::printStatus() const {
  Serial.println();
  Serial.printf("  wifi      : %s\n",
                _net->isUp() ? "connected"
                             : (_net->state() == NetState::Connecting
                                    ? "connecting" : "down"));
  Serial.printf("  ssid      : \"%s\"%s\n", _net->ssid(),
                _net->hasCredentials() ? "" : "  (none stored)");
  Serial.printf("  ip        : %s\n", _net->ip()[0] ? _net->ip() : "-");
  Serial.printf("  signal    : %d/4\n", (int)_net->bars());
  Serial.printf("  spotify   : %s\n",
                _spotify->authorised() ? "signed in" : "NOT signed in");
  Serial.printf("  device    : %s\n",
                _state->hasDevice ? _state->device : "none active");
  Serial.printf("  playing   : %s\n", _state->isPlaying ? "yes" : "no");
  Serial.printf("  heap      : %u free, %u min-ever\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  if (_status->message[0]) Serial.printf("  message   : %s\n", _status->message);
  Serial.println();
}

void SerialConsole::handleLine(char *line) {
  /* Trim both ends - a trailing CR from a Windows terminal would otherwise
   * become part of the SSID, which is a miserable thing to debug. */
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
               line[n - 1] == '\r' || line[n - 1] == '\n')) {
    line[--n] = '\0';
  }
  if (!*line) return;

  /* Split the verb from its argument; the argument keeps its spaces. */
  char *arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  }

  if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
    printHelp();

  } else if (strcasecmp(line, "status") == 0) {
    printStatus();

  } else if (strcasecmp(line, "scan") == 0) {
    Serial.println("  scanning...");
    _net->startScan();

  } else if (strcasecmp(line, "ssid") == 0) {
    if (!arg || !*arg) {
      Serial.println("  usage: ssid <network name>");
      return;
    }
    strlcpy(_ssid, arg, sizeof(_ssid));
    _haveSsid = true;
    Serial.printf("  staged ssid \"%s\" - now 'pass <password>' then 'join'\n",
                  _ssid);

  } else if (strcasecmp(line, "pass") == 0) {
    strlcpy(_pass, arg ? arg : "", sizeof(_pass));
    /* Never echo it back. */
    Serial.printf("  staged password %s - now 'join'\n", redact(_pass));

  } else if (strcasecmp(line, "join") == 0) {
    if (!_haveSsid) {
      Serial.println("  nothing staged: 'ssid <name>' first");
      return;
    }
    Serial.printf("  saving \"%s\" and connecting...\n", _ssid);
    _net->setCredentials(_ssid, _pass);
    memset(_pass, 0, sizeof(_pass));
    _haveSsid = false;

  } else if (strcasecmp(line, "forget") == 0) {
    _net->forgetCredentials();
    Serial.println("  wi-fi credentials erased");

  } else if (strcasecmp(line, "touch") == 0) {
    _input->selfTest();

  } else if (strcasecmp(line, "signout") == 0) {
    _spotify->signOut();
    Serial.println("  spotify refresh token erased");

  } else if (strcasecmp(line, "reboot") == 0) {
    Serial.println("  rebooting...");
    Serial.flush();
    ESP.restart();

  } else {
    Serial.printf("  unknown command '%s' - try 'help'\n", line);
  }
}

void SerialConsole::service() {
  /* Bounded per call: a paste of a long line must not stall the loop any
   * more than the characters already sitting in the UART buffer. */
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;

    if (c == '\n' || c == '\r') {
      if (_len) {
        _line[_len] = '\0';
        handleLine(_line);
        _len = 0;
      }
      continue;
    }
    if (c == 8 || c == 127) {          /* backspace */
      if (_len) _len--;
      continue;
    }
    if (_len + 1 < sizeof(_line)) _line[_len++] = (char)c;
  }
}
