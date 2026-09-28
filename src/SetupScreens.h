/* =========================================================================
 *  SetupScreens.h - Wi-Fi network picker, on-screen keyboard, and the
 *  "finish signing in from a browser" screen.
 *
 *  The keyboard exists because the alternative - a captive-portal AP -
 *  needs a second device just to get the first one online. Typing a WPA2
 *  password on a resistive panel is not fun, but it is self-contained.
 *
 *  Spotify sign-in is deliberately NOT on the keyboard: the paste is a
 *  200-character URL, which no on-screen keyboard makes tolerable. That
 *  step happens in a browser against the device's own web server, which
 *  is reachable as soon as Wi-Fi is up.
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "DisplayManager.h"
#include "InputManager.h"
#include "NetManager.h"

/* ------------------------------------------------------------------------
 *  Wi-Fi network picker
 * --------------------------------------------------------------------- */
class WifiListScreen {
 public:
  enum class Result : uint8_t { None, Back, Rescan, Pick };

  void begin(DisplayManager *display) { _display = display; }
  void enter();
  void render(const NetManager &net);
  Result handleTouch(const TouchEvent &ev, const NetManager &net,
                     char *outSsid, size_t outLen, bool *outSecured);
  void markDirty() { _dirty = true; }

 private:
  int8_t rowAt(int16_t x, int16_t y) const;

  DisplayManager *_display = nullptr;
  uint8_t _scroll      = 0;
  bool    _dirty       = true;
  uint8_t _lastCount   = 0xFF;
  uint8_t _lastScroll  = 0xFF;
  bool    _lastScanning   = false;
  bool    _lastScanFailed = false;
  int8_t  _pressedRow  = -1;
};

/* ------------------------------------------------------------------------
 *  On-screen keyboard
 * --------------------------------------------------------------------- */
class KeyboardScreen {
 public:
  enum class Result : uint8_t { None, Cancel, Done };

  void begin(DisplayManager *display) { _display = display; }

  /* `title` labels what is being typed (e.g. the SSID). */
  void enter(const char *title, bool masked);
  void render();
  Result handleTouch(const TouchEvent &ev);

  const char *text() const { return _text; }

 private:
  enum class Layer : uint8_t { Lower, Upper, Symbols, Symbols2 };

  struct Key {
    int16_t     x, y, w, h;
    const char *label;
    char        emit;      /* 0 for function keys */
    uint8_t     fn;        /* 1 shift, 2 backspace, 3 layer, 4 space, 5 done, 6 cancel */
  };

  uint8_t buildKeys(Key *out, uint8_t maxKeys) const;
  void    drawKeys();
  int8_t  keyAt(int16_t x, int16_t y, const Key *keys, uint8_t n) const;

  DisplayManager *_display = nullptr;
  Layer   _layer   = Layer::Lower;
  bool    _masked  = true;
  bool    _dirty   = true;
  bool    _fieldDirty = true;
  int8_t  _pressed = -1;
  char    _title[40] = {0};
  char    _text[65]  = {0};   /* WPA2 max is 63 chars + NUL, plus slack */
};

/* ------------------------------------------------------------------------
 *  "Sign in from a browser" screen
 * --------------------------------------------------------------------- */
class SignInScreen {
 public:
  enum class Result : uint8_t { None, Back, WifiSetup };

  void begin(DisplayManager *display) { _display = display; }
  void enter();
  void render(const char *ip, bool authed);
  Result handleTouch(const TouchEvent &ev);

 private:
  DisplayManager *_display = nullptr;
  bool _dirty = true;
  char _lastIp[16] = {0};
  bool _lastAuthed = false;
};
