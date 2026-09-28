/* =========================================================================
 *  SerialConsole.h - recovery and diagnostics over the USB serial port.
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  Every other way into this device needs something that can itself fail:
 *  the touchscreen needs working touch hardware, and the web page needs the
 *  device to already be on your network. Store one bad SSID on a unit whose
 *  touch layer is not wired and there is no way back in short of erasing
 *  flash and redoing the Spotify sign-in.
 *
 *  A serial console needs neither. The USB cable you flash with is enough.
 *
 *  Non-blocking: it consumes whatever characters have arrived and returns.
 *  Credentials are staged and then committed, rather than parsed out of one
 *  line, because SSIDs and passwords both routinely contain spaces and any
 *  single-line syntax would have to guess where one ends.
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "AppState.h"

class NetManager;
class SpotifyClient;
class InputManager;

class SerialConsole {
 public:
  void begin(NetManager *net, SpotifyClient *spotify, InputManager *input,
             PlaybackState *state, AppStatus *status);
  /* Call every loop iteration. Never blocks. */
  void service();

 private:
  void handleLine(char *line);
  void printHelp() const;
  void printStatus() const;

  NetManager    *_net     = nullptr;
  SpotifyClient *_spotify = nullptr;
  InputManager  *_input   = nullptr;
  PlaybackState *_state   = nullptr;
  AppStatus     *_status  = nullptr;

  char    _line[160] = {0};
  uint8_t _len       = 0;

  /* Staged credentials, committed by "join". */
  char _ssid[33] = {0};
  char _pass[65] = {0};
  bool _haveSsid = false;
};
