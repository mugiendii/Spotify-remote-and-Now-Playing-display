#include "InputManager.h"

#include <SPI.h>

#include "Log.h"
#include "config.h"

#if TOUCH_ENABLED
#include <XPT2046_Touchscreen.h>

/* Passing the IRQ pin lets the library short-circuit a read when the panel
 * is not being touched, which keeps the shared bus quiet. If your module's
 * T_IRQ is not wired through, set PIN_TOUCH_IRQ to -1 in display_config.h
 * and the library falls back to polling the pressure reading. */
#if PIN_TOUCH_IRQ >= 0
static XPT2046_Touchscreen ts(PIN_TOUCH_CS, PIN_TOUCH_IRQ);
#else
static XPT2046_Touchscreen ts(PIN_TOUCH_CS);
#endif

/* How far a finger may travel between press and release and still count as
 * a tap. Resistive panels are noisy, and a fingertip rolls a few pixels on
 * the way up; anything under this is still the same button press. */
static constexpr int16_t TAP_SLOP_PX = 18;
#endif  /* TOUCH_ENABLED */

#if TOUCH_ENABLED
/* One 12-bit conversion, driven directly rather than through the library,
 * so the probe works regardless of whether the panel is being touched.
 * Same CS interlock as every other touch access. */
static uint16_t xptRaw(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(SPI_TOUCH_FREQUENCY, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, HIGH);          /* park the panel */
  digitalWrite(PIN_TOUCH_CS, LOW);
  SPI.transfer(cmd);
  const uint16_t hi = SPI.transfer(0);
  const uint16_t lo = SPI.transfer(0);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  SPI.endTransaction();
  return (uint16_t)(((hi << 8) | lo) >> 3);   /* 12 significant bits */
}
#endif

void InputManager::selfTest() {
#if !TOUCH_ENABLED
  _touchHealthy = false;
  LOG_I("touch: disabled at compile time (TOUCH_ENABLED 0)");
#else
  /* Sample the X plate repeatedly. An untouched panel floats, so a live
   * controller returns slightly different values each time. A bus that is
   * not wired through returns the same value every time - all zeroes when
   * MISO is low, all ones when it floats high. That difference is the
   * whole test. */
  uint16_t lo = 0xFFFF, hi = 0;
  uint16_t first = 0;
  bool     identical = true;

  for (int i = 0; i < 12; ++i) {
    const uint16_t v = xptRaw(0xD1);        /* X position, 12-bit */
    if (i == 0) first = v;
    else if (v != first) identical = false;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    delayMicroseconds(200);
  }
  const uint16_t z1 = xptRaw(0xB1);

  LOG_I("touch: probe cs=%d irq=%d -> x %u..%u (%s), z1 %u",
        PIN_TOUCH_CS, PIN_TOUCH_IRQ, lo, hi,
        identical ? "IDENTICAL" : "varying", z1);

  _touchHealthy = !identical;
  if (identical && (first == 0 || first == 4095)) {
    LOG_E("touch: controller is NOT responding (every read was %u). Check "
          "T_CS=%d, T_CLK=%d, T_DIN=%d, T_DO=%d and that the panel's touch "
          "header is actually connected.",
          first, PIN_TOUCH_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);
  } else if (identical) {
    LOG_E("touch: readings never change (%u) - suspect a wiring fault", first);
  } else {
    LOG_I("touch: controller responding normally");
  }

#if PIN_TOUCH_IRQ >= 0
  /* T_IRQ idles HIGH and pulls LOW on contact. Stuck LOW with nothing
   * touching it usually means the pin is not really connected. */
  pinMode(PIN_TOUCH_IRQ, INPUT);
  LOG_I("touch: T_IRQ reads %s at rest (expected HIGH)",
        digitalRead(PIN_TOUCH_IRQ) ? "HIGH" : "LOW - suspect");
#endif
#endif
}

void InputManager::begin() {
#if TOUCH_ENABLED
  /* The touch CS must be an output and parked high before anything else
   * drives the bus. */
  pinMode(PIN_TOUCH_CS, OUTPUT);
  digitalWrite(PIN_TOUCH_CS, HIGH);

  /* ts.begin() calls SPI.begin() internally. TFT_eSPI has already brought
   * the bus up with our pin map, so that call is a no-op and the mapping
   * stands. */
  ts.begin();
  /* Rotation 1 is the library's identity mapping: raw 12-bit ADC values
   * reach mapRaw() untouched, so the constants in display_config.h are the
   * only calibration anywhere in the path. */
  ts.setRotation(1);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  LOG_I("input: touch enabled (cs=%d irq=%d)", PIN_TOUCH_CS, PIN_TOUCH_IRQ);
#else
  LOG_I("input: touch disabled at compile time");
#endif

  _buttons[0].pin    = PIN_BTN_PREV;
  _buttons[0].action = ButtonAction::Previous;
  _buttons[1].pin    = PIN_BTN_PLAYPAUSE;
  _buttons[1].action = ButtonAction::PlayPause;
  _buttons[2].pin    = PIN_BTN_NEXT;
  _buttons[2].action = ButtonAction::Next;

  for (Button &b : _buttons) {
    if (b.pin < 0) continue;
    pinMode(b.pin, BTN_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
    LOG_I("input: button on GPIO %d", (int)b.pin);
  }
}

/* =========================================================================
 *  Touch
 * ====================================================================== */
#if TOUCH_ENABLED

void InputManager::mapRaw(uint16_t rawX, uint16_t rawY, int16_t &sx, int16_t &sy) {
  long ax = rawX, ay = rawY;
#if TOUCH_SWAP_XY
  /* Swap FIRST - the calibration constants are expressed on screen axes. */
  const long t = ax;
  ax = ay;
  ay = t;
#endif
  long x = (ax - (long)TOUCH_MIN_X) * (SCREEN_W - 1) /
           ((long)TOUCH_MAX_X - (long)TOUCH_MIN_X);
  long y = (ay - (long)TOUCH_MIN_Y) * (SCREEN_H - 1) /
           ((long)TOUCH_MAX_Y - (long)TOUCH_MIN_Y);
#if TOUCH_INVERT_X
  x = (SCREEN_W - 1) - x;
#endif
#if TOUCH_INVERT_Y
  y = (SCREEN_H - 1) - y;
#endif
  if (x < 0) x = 0;
  if (x > SCREEN_W - 1) x = SCREEN_W - 1;
  if (y < 0) y = 0;
  if (y > SCREEN_H - 1) y = SCREEN_H - 1;
  sx = (int16_t)x;
  sy = (int16_t)y;
}

bool InputManager::readTouchPoint(int16_t &sx, int16_t &sy) {
  /* Deselect the panel before talking to the touch controller. */
  digitalWrite(TFT_CS, HIGH);
  const bool touched = ts.touched();
  uint16_t rawX = 0, rawY = 0, rawZ = 0;
  if (touched) {
    const TS_Point p = ts.getPoint();
    rawX = (uint16_t)p.x;
    rawY = (uint16_t)p.y;
    rawZ = (uint16_t)p.z;
  }
  /* Release the bus again so the next panel write is not fighting the
   * XPT2046 for MISO. */
  digitalWrite(PIN_TOUCH_CS, HIGH);

  if (!touched) return false;
#if TOUCH_DEBUG_RAW
  LOG_I("touch raw x=%u y=%u z=%u", rawX, rawY, rawZ);
#else
  (void)rawZ;
#endif
  mapRaw(rawX, rawY, sx, sy);
  return true;
}

TouchEvent InputManager::pollTouch() {
  TouchEvent ev;
  const uint32_t now = millis();
  if (now - _lastPoll < TOUCH_POLL_MS) return ev;
  _lastPoll = now;

  int16_t sx = 0, sy = 0;
  const bool pressed = readTouchPoint(sx, sy);

  if (pressed) {
    _upStreak = 0;
    if (!_down) {
      /* Contact bounce after a release reads as a fresh press; ignore
       * anything arriving inside the debounce window. */
      if (now - _lastRelease < TOUCH_DEBOUNCE_MS) return ev;
      _down  = true;
      _x = _downX = sx;
      _y = _downY = sy;
      ev.phase = TouchPhase::Down;
    } else if (abs(sx - _x) > 2 || abs(sy - _y) > 2) {
      _x = sx;
      _y = sy;
      ev.phase = TouchPhase::Move;
    } else {
      return ev;                       /* holding still: nothing to report */
    }
    ev.x = _x;
    ev.y = _y;
    ev.downX = _downX;
    ev.downY = _downY;
    return ev;
  }

  if (_down) {
    /* Require several consecutive empty reads before calling it a release,
     * so jitter mid-press is not read as one press ending and another
     * beginning. This is the "one press, one action" rule. */
    if (++_upStreak < TOUCH_RELEASE_SAMPLES) return ev;

    _down        = false;
    _upStreak    = 0;
    _lastRelease = now;

    const bool isTap = (abs(_x - _downX) <= TAP_SLOP_PX) &&
                       (abs(_y - _downY) <= TAP_SLOP_PX);
    ev.phase = isTap ? TouchPhase::Tap : TouchPhase::Up;
    ev.x = _x;
    ev.y = _y;
    ev.downX = _downX;
    ev.downY = _downY;
  }
  return ev;
}

#else  /* !TOUCH_ENABLED */

TouchEvent InputManager::pollTouch() { return TouchEvent{}; }
bool InputManager::readTouchPoint(int16_t &, int16_t &) { return false; }
void InputManager::mapRaw(uint16_t, uint16_t, int16_t &, int16_t &) {}

#endif /* TOUCH_ENABLED */

/* =========================================================================
 *  Physical buttons
 *
 *  Standard time-based debounce: a reading must hold steady for
 *  BTN_DEBOUNCE_MS before it is believed. Events fire on the press edge,
 *  which suits transport keys - unlike the touch buttons, there is no
 *  "slide off to cancel" gesture to wait for.
 * ====================================================================== */
ButtonAction InputManager::pollButtons() {
  ButtonAction action = ButtonAction::None;
  const uint32_t now = millis();

  for (Button &b : _buttons) {
    if (b.pin < 0) continue;

    const bool level   = (digitalRead(b.pin) == HIGH);
    const bool pressed = BTN_ACTIVE_LOW ? !level : level;

    if (pressed != b.raw) {
      b.raw     = pressed;
      b.changed = now;
      continue;
    }
    if (now - b.changed < BTN_DEBOUNCE_MS) continue;

    if (b.stable != b.raw) {
      b.stable = b.raw;
      if (b.stable && action == ButtonAction::None) {
        action = b.action;
        LOG_D("input: button GPIO %d pressed", (int)b.pin);
      }
    }
  }
  return action;
}
