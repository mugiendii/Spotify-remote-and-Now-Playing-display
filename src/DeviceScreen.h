/* =========================================================================
 *  DeviceScreen.h - the Spotify Connect device picker.
 *
 *  Lists what GET /v1/me/player/devices returned and turns a tap into a
 *  transfer request. It owns no network code and no TFT handle: it renders
 *  through DisplayManager's primitives and reports back what the user
 *  chose, which keeps the transfer policy in one place (UiController).
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "AppState.h"
#include "DisplayManager.h"
#include "InputManager.h"

class DeviceScreen {
 public:
  /* What a tap asked for. */
  enum class Result : uint8_t { None, Back, Refresh, Transfer };

  void begin(DisplayManager *display) { _display = display; }

  /* Called when the screen becomes visible. */
  void enter();

  /* Repaint if anything changed. Cheap to call every iteration. */
  void render(const DeviceList &devices, bool authed);

  /* Feed one touch event. `outDeviceId` is filled on Result::Transfer. */
  Result handleTouch(const TouchEvent &ev, const DeviceList &devices,
                     char *outDeviceId, size_t outLen);

  /* Whether a transfer should keep audio running. Spec calls for this to
   * be offered explicitly; it defaults to on because pausing on handover
   * is almost never what anyone wants. */
  bool keepPlaying() const { return _keepPlaying; }

 private:
  void drawRows(const DeviceList &devices);
  int8_t rowAt(int16_t x, int16_t y) const;   /* -1 = none */
  void clampScroll(uint8_t count);

  DisplayManager *_display = nullptr;
  bool     _keepPlaying = true;
  uint8_t  _scroll      = 0;
  bool     _dirty       = true;

  /* Snapshot used to decide whether a repaint is needed. */
  uint32_t _lastUpdatedAt = 0;
  uint8_t  _lastCount     = 0xFF;
  uint8_t  _lastScroll    = 0xFF;
  bool     _lastKeep      = true;
  int8_t   _pressedRow    = -1;
};
