/* =========================================================================
 *  InputManager.h - touch and optional physical buttons.
 *
 *  SPI SHARING
 *  -----------
 *  The panel and the XPT2046 sit on one VSPI bus, separated only by their
 *  chip selects. TFT_eSPI keeps its own SPIClass(VSPI) while the touch
 *  library uses the global `SPI` object: two wrappers over one peripheral,
 *  sharing one HAL mutex. On top of that this module forces the panel's CS
 *  high around every touch read, so only one slave can ever drive MISO.
 *  Skipping that interlock produces the classic symptom of a display that
 *  works until you touch it and then fills with garbage.
 *
 *  WHAT IT EMITS
 *  -------------
 *  Raw, debounced touch phases with screen coordinates - NOT playback
 *  actions. Hit-testing belongs to whichever screen is showing, because
 *  the same tap means "next track", "pick this device" or "type a Q"
 *  depending on where the UI is.
 *
 *  Physical buttons are different: they are wired to transport functions
 *  and have no screen context, so they still emit a direct ButtonAction.
 *  That is what lets a build with TOUCH_ENABLED 0 keep working unchanged.
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "display_config.h"

/* Touch phases. Tap is emitted on release, and only when the release
 * lands close enough to the press for it to count as a tap rather than a
 * drag that wandered off. */
enum class TouchPhase : uint8_t { None, Down, Move, Up, Tap };

struct TouchEvent {
  TouchPhase phase = TouchPhase::None;
  int16_t    x     = 0;
  int16_t    y     = 0;
  /* Where the gesture began - lets a screen confirm a tap started on the
   * same control it ended on. */
  int16_t    downX = 0;
  int16_t    downY = 0;
};

/* Hardware keys map straight onto transport actions. */
enum class ButtonAction : uint8_t { None, Previous, PlayPause, Next };

class InputManager {
 public:
  void begin();

  /* Sample touch. At most one event per call. */
  TouchEvent pollTouch();

  /* Sample the GPIO buttons. At most one action per call. */
  ButtonAction pollButtons();

  bool isDown() const { return _down; }

 private:
  bool readTouchPoint(int16_t &sx, int16_t &sy);
  void mapRaw(uint16_t rawX, uint16_t rawY, int16_t &sx, int16_t &sy);

  /* ---- touch ---- */
  bool     _down        = false;
  int16_t  _x = 0, _y = 0;
  int16_t  _downX = 0, _downY = 0;
  uint8_t  _upStreak    = 0;
  uint32_t _lastPoll    = 0;
  uint32_t _lastRelease = 0;

  /* ---- physical buttons ---- */
  struct Button {
    int8_t       pin     = -1;
    ButtonAction action  = ButtonAction::None;
    bool         stable  = false;   /* debounced logical "pressed" */
    bool         raw     = false;
    uint32_t     changed = 0;
  };
  Button _buttons[3];
};
