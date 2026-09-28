#include "SetupScreens.h"

#include "Log.h"

/* =========================================================================
 *  Wi-Fi network picker
 * ====================================================================== */
void WifiListScreen::enter() {
  _scroll      = 0;
  _dirty       = true;
  _lastCount   = 0xFF;
  _lastScroll  = 0xFF;
  _pressedRow  = -1;
  _display->clearScreen();
}

void WifiListScreen::render(const NetManager &net) {
  const uint8_t count = net.networkCount();
  const bool changed = _dirty || count != _lastCount || _scroll != _lastScroll ||
                       net.scanning() != _lastScanning ||
                       net.scanFailed() != _lastScanFailed;
  if (!changed) return;

  const bool full = _dirty;
  _dirty        = false;
  _lastCount    = count;
  _lastScroll   = _scroll;
  _lastScanning   = net.scanning();
  _lastScanFailed = net.scanFailed();

  if (full) _display->drawScreenHeader("Wi-Fi networks", "Back", "Rescan");

  /* Status line where the device screen puts its toggle. */
  _display->tft().fillRect(ListLayout::ROW_X, ListLayout::OPT_Y,
                           ListLayout::ROW_W, ListLayout::OPT_H, Theme::BG);
  _display->tft().setTextFont(FONT_SMALL_N);
  _display->tft().setTextDatum(ML_DATUM);
  _display->tft().setTextColor(Theme::TEXT_MUTE, Theme::BG);
  char status[64];
  if (net.scanning()) {
    strlcpy(status, "Scanning...", sizeof(status));
  } else if (net.scanFailed()) {
    strlcpy(status, "Scan failed - retrying", sizeof(status));
  } else if (net.hasCredentials()) {
    snprintf(status, sizeof(status), "Saved: %s", net.ssid());
  } else {
    strlcpy(status, "No network saved", sizeof(status));
  }
  _display->tft().drawString(status, ListLayout::ROW_X,
                             ListLayout::OPT_Y + ListLayout::OPT_H / 2);

  if (count == 0) {
    /* Three very different situations, and telling them apart is the whole
     * point: "still working", "the radio refused", and "the radio looked
     * and there is genuinely nothing it can see". */
    if (net.scanning()) {
      _display->drawCentredText("Looking for networks...", 150, FONT_BODY,
                                Theme::TEXT_DIM, Theme::BG);
    } else if (net.scanFailed()) {
      _display->drawCentredText("Scan failed", 140, FONT_BODY, Theme::WARN,
                                Theme::BG);
      _display->drawCentredText("Retrying automatically - or tap Rescan", 172,
                                nullptr, Theme::TEXT_MUTE, Theme::BG);
    } else {
      _display->drawCentredText("No networks found", 134, FONT_BODY,
                                Theme::TEXT_DIM, Theme::BG);
      /* By far the most common cause, and invisible from the device's side
       * unless it is said out loud: this radio cannot see 5 GHz at all. */
      _display->drawCentredText("This board is 2.4 GHz only", 166, nullptr,
                                Theme::TEXT_MUTE, Theme::BG);
      _display->drawCentredText("5 GHz networks will never appear here", 190,
                                nullptr, Theme::TEXT_MUTE, Theme::BG);
    }
    _display->drawScrollArrows(false, false);
    return;
  }

  const uint8_t maxScroll =
      (count > ListLayout::VISIBLE) ? (uint8_t)(count - ListLayout::VISIBLE) : 0;
  if (_scroll > maxScroll) _scroll = maxScroll;

  for (uint8_t slot = 0; slot < ListLayout::VISIBLE; ++slot) {
    const int16_t y   = ListLayout::rowY(slot);
    const uint8_t idx = (uint8_t)(_scroll + slot);
    if (idx >= count) {
      _display->tft().fillRect(ListLayout::ROW_X, y, ListLayout::ROW_W,
                               ListLayout::ROW_H, Theme::BG);
      continue;
    }
    const ScannedNet &n = net.networks()[idx];
    char sub[48];
    snprintf(sub, sizeof(sub), "%s  -  signal %d/4",
             n.secured ? "secured" : "open", (int)n.bars);
    /* Green dot marks the network already saved. */
    const uint16_t badge =
        (strcmp(n.ssid, net.ssid()) == 0) ? Theme::ACCENT : Theme::BG;
    _display->drawListRow(y, n.ssid, sub, badge, _pressedRow == (int8_t)slot,
                          false);
  }

  _display->drawScrollArrows(_scroll > 0,
                             (uint16_t)(_scroll + ListLayout::VISIBLE) < count);
}

int8_t WifiListScreen::rowAt(int16_t x, int16_t y) const {
  if (x < ListLayout::ROW_X || x > ListLayout::ROW_X + ListLayout::ROW_W) return -1;
  for (uint8_t slot = 0; slot < ListLayout::VISIBLE; ++slot) {
    const int16_t ry = ListLayout::rowY(slot);
    if (y >= ry && y < ry + ListLayout::ROW_H) return (int8_t)slot;
  }
  return -1;
}

WifiListScreen::Result WifiListScreen::handleTouch(const TouchEvent &ev,
                                                   const NetManager &net,
                                                   char *outSsid, size_t outLen,
                                                   bool *outSecured) {
  if (ev.phase == TouchPhase::Down) {
    const int8_t slot = rowAt(ev.x, ev.y);
    if (slot >= 0 && (uint16_t)(_scroll + slot) < net.networkCount()) {
      _pressedRow = slot;
      _dirty      = true;
    }
    return Result::None;
  }
  if (ev.phase == TouchPhase::Up) {
    if (_pressedRow >= 0) { _pressedRow = -1; _dirty = true; }
    return Result::None;
  }
  if (ev.phase != TouchPhase::Tap) return Result::None;
  if (_pressedRow >= 0) { _pressedRow = -1; _dirty = true; }

  if (ev.y < ListLayout::HEADER_H) {
    if (ev.x >= ListLayout::BACK_X && ev.x < ListLayout::BACK_X + ListLayout::BACK_W) {
      return Result::Back;
    }
    if (ev.x >= ListLayout::ACTION_X) return Result::Rescan;
    return Result::None;
  }

  if (ev.x >= ListLayout::SCROLL_X) {
    const bool canDown =
        (uint16_t)(_scroll + ListLayout::VISIBLE) < net.networkCount();
    if (ev.y >= ListLayout::SCROLL_UP_Y &&
        ev.y < ListLayout::SCROLL_UP_Y + ListLayout::SCROLL_BTN_H && _scroll > 0) {
      _scroll--; _dirty = true;
    } else if (ev.y >= ListLayout::SCROLL_DN_Y &&
               ev.y < ListLayout::SCROLL_DN_Y + ListLayout::SCROLL_BTN_H && canDown) {
      _scroll++; _dirty = true;
    }
    return Result::None;
  }

  const int8_t slot = rowAt(ev.x, ev.y);
  if (slot < 0) return Result::None;
  const uint8_t idx = (uint8_t)(_scroll + slot);
  if (idx >= net.networkCount()) return Result::None;

  strlcpy(outSsid, net.networks()[idx].ssid, outLen);
  if (outSecured) *outSecured = net.networks()[idx].secured;
  return Result::Pick;
}

/* =========================================================================
 *  On-screen keyboard
 *
 *  The key grid is rebuilt from a table on every draw and every hit test,
 *  rather than stored. It is ~40 small structs on the stack; keeping one
 *  generator means the thing you see and the thing you tap can never drift
 *  apart, which is the classic on-screen-keyboard bug.
 * ====================================================================== */
namespace {

/* Four layers: lower, upper, and two pages of symbols. Two pages because a
 * Wi-Fi passphrase can legitimately contain any printable ASCII character,
 * and row 2 physically holds only MAX_ROW2_KEYS of them. */
const char *const kRows[4][3] = {
    {"qwertyuiop", "asdfghjkl",  "zxcvbnm"},
    {"QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM"},
    {"1234567890", "@#$_&-+()",  "*\"':;.,"},
    {"~`|^{}[]<>", "=%\\/!?",     ""},
};

/* A row that is one key too long centres to a negative x and runs off the
 * left edge with no warning, so the tables are checked here rather than
 * discovered on the glass. */
static_assert(sizeof("qwertyuiop") - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "kb row 0 too wide");
static_assert(sizeof("asdfghjkl")  - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "kb row 1 too wide");
static_assert(sizeof("zxcvbnm")    - 1 <= (size_t)KeyLayout::MAX_ROW2_KEYS, "kb row 2 too wide");
static_assert(sizeof("1234567890") - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "sym row 0 too wide");
static_assert(sizeof("@#$_&-+()")  - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "sym row 1 too wide");
static_assert(sizeof("*\"':;.,")    - 1 <= (size_t)KeyLayout::MAX_ROW2_KEYS, "sym row 2 too wide");
static_assert(sizeof("~`|^{}[]<>") - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "sym2 row 0 too wide");
static_assert(sizeof("=%\\/!?")     - 1 <= (size_t)KeyLayout::MAX_ROW_KEYS,  "sym2 row 1 too wide");

}  // namespace

void KeyboardScreen::enter(const char *title, bool masked) {
  strlcpy(_title, title ? title : "", sizeof(_title));
  _text[0]    = '\0';
  _layer      = Layer::Lower;
  _masked     = masked;
  _dirty      = true;
  _fieldDirty = true;
  _pressed    = -1;
  _display->clearScreen();
}

uint8_t KeyboardScreen::buildKeys(Key *out, uint8_t maxKeys) const {
  using namespace KeyLayout;
  uint8_t n = 0;
  const uint8_t layerIdx = (uint8_t)_layer;

  /* --- row 0: ten keys, centred --- */
  {
    const char *r = kRows[layerIdx][0];
    const int16_t count = (int16_t)strlen(r);
    const int16_t total = count * KEY_W + (count - 1) * GAP;
    int16_t x = (SCREEN_W - total) / 2;
    for (int16_t i = 0; i < count && n < maxKeys; ++i) {
      out[n++] = Key{x, rowY(0), KEY_W, KEY_H, nullptr, r[i], 0};
      x += PITCH_X;
    }
  }
  /* --- row 1 --- */
  {
    const char *r = kRows[layerIdx][1];
    const int16_t count = (int16_t)strlen(r);
    const int16_t total = count * KEY_W + (count - 1) * GAP;
    int16_t x = (SCREEN_W - total) / 2;
    for (int16_t i = 0; i < count && n < maxKeys; ++i) {
      out[n++] = Key{x, rowY(1), KEY_W, KEY_H, nullptr, r[i], 0};
      x += PITCH_X;
    }
  }
  /* --- row 2: shift + letters + backspace --- */
  {
    const char *r = kRows[layerIdx][2];
    const int16_t count = (int16_t)strlen(r);
    /* A layer may have no third-row characters at all (symbols page 2),
     * in which case only shift and backspace are placed. */
    const int16_t letters = count ? (count * KEY_W + (count - 1) * GAP) : 0;
    const int16_t total = WIDE_W + GAP + letters + (count ? GAP : 0) + WIDE_W;
    int16_t x = (SCREEN_W - total) / 2;

    const char *shiftLabel = (_layer == Layer::Upper)    ? "ABC"
                             : (_layer == Layer::Symbols)  ? "more"
                             : (_layer == Layer::Symbols2) ? "back"
                                                           : "shft";
    out[n++] = Key{x, rowY(2), WIDE_W, KEY_H, shiftLabel, 0, 1};
    x += WIDE_W + GAP;
    for (int16_t i = 0; i < count && n < maxKeys; ++i) {
      out[n++] = Key{x, rowY(2), KEY_W, KEY_H, nullptr, r[i], 0};
      x += PITCH_X;
    }
    if (n < maxKeys) out[n++] = Key{x, rowY(2), WIDE_W, KEY_H, "del", 0, 2};
  }
  /* --- row 3: layer, space, cancel, done --- */
  {
    const int16_t total = WIDE_W + GAP + SPACE_W + GAP + WIDE_W + GAP + DONE_W;
    int16_t x = (SCREEN_W - total) / 2;
    const bool symbolic = (_layer == Layer::Symbols || _layer == Layer::Symbols2);
    out[n++] = Key{x, rowY(3), WIDE_W, KEY_H, symbolic ? "abc" : "?123", 0, 3};
    x += WIDE_W + GAP;
    out[n++] = Key{x, rowY(3), SPACE_W, KEY_H, "space", ' ', 4};
    x += SPACE_W + GAP;
    out[n++] = Key{x, rowY(3), WIDE_W, KEY_H, "esc", 0, 6};
    x += WIDE_W + GAP;
    out[n++] = Key{x, rowY(3), DONE_W, KEY_H, "Connect", 0, 5};
  }
  return n;
}

/* Draws every cap. Only called on a layout change: individual presses are
 * repainted one cap at a time from handleTouch(), because redrawing ~35
 * rounded rects per keystroke is visibly laggy on a 27 MHz SPI panel. */
void KeyboardScreen::drawKeys() {
  Key keys[48];
  const uint8_t n = buildKeys(keys, sizeof(keys) / sizeof(keys[0]));

  for (uint8_t i = 0; i < n; ++i) {
    const Key &k = keys[i];
    char label[8];
    const char *text = k.label;
    if (!text) {
      label[0] = k.emit;
      label[1] = '\0';
      text = label;
    }
    _display->drawKeyCap(k.x, k.y, k.w, k.h, text, _pressed == (int8_t)i,
                         k.fn == 5);
  }
}

void KeyboardScreen::render() {
  if (!_dirty && !_fieldDirty) return;

  if (_dirty) {
    char header[64];
    snprintf(header, sizeof(header), "Password for %s", _title);
    _display->drawScreenHeader(header, nullptr, nullptr);
    _display->drawCentredText("Type the Wi-Fi password, then Connect",
                              KeyLayout::TITLE_Y + 8, nullptr,
                              Theme::TEXT_MUTE, Theme::BG);
  }

  _display->drawTextField(KeyLayout::FIELD_X, KeyLayout::FIELD_Y,
                          KeyLayout::FIELD_W, KeyLayout::FIELD_H, _text,
                          _masked, "(leave empty for an open network)");
  _fieldDirty = false;

  if (_dirty) drawKeys();
  _dirty = false;
}

int8_t KeyboardScreen::keyAt(int16_t x, int16_t y, const Key *keys,
                             uint8_t n) const {
  for (uint8_t i = 0; i < n; ++i) {
    const Key &k = keys[i];
    if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return (int8_t)i;
  }
  return -1;
}

KeyboardScreen::Result KeyboardScreen::handleTouch(const TouchEvent &ev) {
  Key keys[48];
  const uint8_t n = buildKeys(keys, sizeof(keys) / sizeof(keys[0]));

  if (ev.phase == TouchPhase::Down) {
    const int8_t i = keyAt(ev.x, ev.y, keys, n);
    if (i >= 0) {
      _pressed = i;
      /* Repaint just this cap - a full keyboard redraw per keystroke would
       * be ~90 rounded rects and visibly laggy. */
      const Key &k = keys[i];
      char label[8];
      const char *text = k.label;
      if (!text) { label[0] = k.emit; label[1] = '\0'; text = label; }
      _display->drawKeyCap(k.x, k.y, k.w, k.h, text, true, k.fn == 5);
    }
    return Result::None;
  }

  if (ev.phase != TouchPhase::Tap && ev.phase != TouchPhase::Up) {
    return Result::None;
  }

  /* Un-press whatever was lit. */
  if (_pressed >= 0) {
    const Key &k = keys[_pressed];
    char label[8];
    const char *text = k.label;
    if (!text) { label[0] = k.emit; label[1] = '\0'; text = label; }
    _display->drawKeyCap(k.x, k.y, k.w, k.h, text, false, k.fn == 5);
    _pressed = -1;
  }

  if (ev.phase != TouchPhase::Tap) return Result::None;

  const int8_t i = keyAt(ev.x, ev.y, keys, n);
  if (i < 0) return Result::None;
  const Key &k = keys[i];

  switch (k.fn) {
    case 1:   /* shift, or "more symbols" while in a symbol layer */
      switch (_layer) {
        case Layer::Lower:    _layer = Layer::Upper;    break;
        case Layer::Upper:    _layer = Layer::Lower;    break;
        case Layer::Symbols:  _layer = Layer::Symbols2; break;
        case Layer::Symbols2: _layer = Layer::Symbols;  break;
      }
      _dirty = true;
      return Result::None;

    case 2: {  /* backspace */
      const size_t len = strlen(_text);
      if (len) _text[len - 1] = '\0';
      _fieldDirty = true;
      return Result::None;
    }

    case 3:   /* letters <-> symbols */
      _layer = (_layer == Layer::Symbols || _layer == Layer::Symbols2)
                   ? Layer::Lower
                   : Layer::Symbols;
      _dirty = true;
      return Result::None;

    case 5:   /* Connect */
      return Result::Done;

    case 6:   /* cancel */
      return Result::Cancel;

    default:
      break;
  }

  /* A character key. */
  if (k.emit) {
    const size_t len = strlen(_text);
    if (len + 1 < sizeof(_text)) {
      _text[len]     = k.emit;
      _text[len + 1] = '\0';
      _fieldDirty    = true;
    } else {
      LOG_I("keyboard: password buffer full (%u chars)", (unsigned)len);
    }
    /* An upper-case layer reverts after one character, like a phone. */
    if (_layer == Layer::Upper) {
      _layer = Layer::Lower;
      _dirty = true;
    }
  }
  return Result::None;
}

/* =========================================================================
 *  Sign-in instructions
 * ====================================================================== */
void SignInScreen::enter() {
  _dirty      = true;
  _lastIp[0]  = '\0';
  _lastAuthed = false;
  _display->clearScreen();
}

void SignInScreen::render(const char *ip, bool authed) {
  const char *showIp = (ip && *ip) ? ip : "";
  if (!_dirty && strcmp(showIp, _lastIp) == 0 && authed == _lastAuthed) return;
  _dirty = false;
  strlcpy(_lastIp, showIp, sizeof(_lastIp));
  _lastAuthed = authed;

  _display->clearScreen();
  _display->drawScreenHeader("Spotify sign-in", "Back", "Wi-Fi");

  if (authed) {
    _display->drawCentredText("Signed in to Spotify", 110, FONT_TITLE,
                              Theme::ACCENT, Theme::BG);
    _display->drawCentredText("Manage or sign out from the same page:", 150,
                              FONT_BODY, Theme::TEXT_DIM, Theme::BG);
  } else {
    _display->drawCentredText("Finish setup in a browser", 100, FONT_TITLE,
                              Theme::TEXT, Theme::BG);
    _display->drawCentredText("On any phone or computer on this network, open:",
                              140, FONT_BODY, Theme::TEXT_DIM, Theme::BG);
  }

  if (*showIp) {
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", showIp);
    _display->drawCentredText(url, 190, FONT_TITLE, Theme::ACCENT, Theme::BG);
  } else {
    _display->drawCentredText("(waiting for Wi-Fi)", 190, FONT_BODY,
                              Theme::WARN, Theme::BG);
  }

  _display->drawCentredText(
      "The device never sees your Spotify password.", 240, nullptr,
      Theme::TEXT_MUTE, Theme::BG);
}

SignInScreen::Result SignInScreen::handleTouch(const TouchEvent &ev) {
  if (ev.phase != TouchPhase::Tap) return Result::None;
  if (ev.y < ListLayout::HEADER_H) {
    if (ev.x >= ListLayout::BACK_X && ev.x < ListLayout::BACK_X + ListLayout::BACK_W) {
      return Result::Back;
    }
    if (ev.x >= ListLayout::ACTION_X) return Result::WifiSetup;
  }
  return Result::None;
}
