#include "DisplayManager.h"

#include "Log.h"

/* =========================================================================
 *  Icon primitives - drawn with geometry rather than bitmaps.
 *
 *  Vector glyphs cost a few hundred bytes of code instead of a few
 *  kilobytes of flash per bitmap, scale if you change the button size, and
 *  sidestep any question of shipping someone else's artwork.
 * ====================================================================== */
namespace {

void iconPrev(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.fillRect(cx - 9, cy - 8, 3, 16, c);
  t.fillTriangle(cx + 9, cy - 8, cx + 9, cy + 8, cx - 4, cy, c);
}

void iconNext(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.fillTriangle(cx - 9, cy - 8, cx - 9, cy + 8, cx + 4, cy, c);
  t.fillRect(cx + 6, cy - 8, 3, 16, c);
}

void iconPlay(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.fillTriangle(cx - 6, cy - 9, cx - 6, cy + 9, cx + 9, cy, c);
}

void iconPause(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.fillRect(cx - 7, cy - 9, 5, 18, c);
  t.fillRect(cx + 2, cy - 9, 5, 18, c);
}

/* Two crossing arrows. Simplified to fit a 44 px key without turning to
 * mush: two diagonals plus arrowheads on the right. */
void iconShuffle(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.drawLine(cx - 10, cy - 5, cx + 6, cy + 5, c);
  t.drawLine(cx - 10, cy - 4, cx + 6, cy + 6, c);
  t.drawLine(cx - 10, cy + 5, cx + 6, cy - 5, c);
  t.drawLine(cx - 10, cy + 6, cx + 6, cy - 4, c);
  t.fillTriangle(cx + 4, cy - 9, cx + 4, cy - 1, cx + 10, cy - 5, c);
  t.fillTriangle(cx + 4, cy + 1, cx + 4, cy + 9, cx + 10, cy + 5, c);
}

/* A loop. `one` adds the "1" that marks repeat-one. */
void iconRepeat(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c, bool one) {
  t.drawRoundRect(cx - 10, cy - 7, 20, 14, 5, c);
  t.drawRoundRect(cx - 10, cy - 6, 20, 14, 5, c);
  /* Break the loop and add an arrowhead so it reads as a cycle. */
  t.fillRect(cx - 3, cy - 9, 8, 4, Theme::BTN);
  t.fillTriangle(cx + 2, cy - 11, cx + 2, cy - 3, cx + 8, cy - 7, c);
  if (one) {
    t.fillRect(cx - 1, cy - 3, 2, 8, c);
    t.fillRect(cx - 3, cy - 1, 2, 2, c);
  }
}

void iconSpeaker(TFT_eSPI &t, int16_t x, int16_t y, uint16_t c) {
  t.fillRect(x, y + 4, 4, 6, c);
  t.fillTriangle(x + 4, y + 7, x + 10, y, x + 10, y + 14, c);
}

void iconNote(TFT_eSPI &t, int16_t cx, int16_t cy, uint16_t c) {
  t.fillRect(cx + 6, cy - 22, 3, 26, c);
  t.fillRect(cx + 9, cy - 22, 10, 3, c);
  t.fillCircle(cx + 2, cy + 4, 7, c);
  t.fillCircle(cx + 2, cy + 4, 3, Theme::PANEL);
}

/* A right-pointing chevron, marking the device strip as tappable. */
void iconChevron(TFT_eSPI &t, int16_t x, int16_t cy, uint16_t c) {
  t.drawLine(x, cy - 4, x + 4, cy, c);
  t.drawLine(x, cy + 4, x + 4, cy, c);
  t.drawLine(x + 1, cy - 4, x + 5, cy, c);
  t.drawLine(x + 1, cy + 4, x + 5, cy, c);
}

}  // namespace

/* =========================================================================
 *  Lifecycle
 * ====================================================================== */
void DisplayManager::begin() {
  _tft.init();
  _tft.setRotation(TFT_ROTATION);
  _tft.fillScreen(Theme::BG);
  _tft.setTextWrap(false);            /* bands clip; wrapping would corrupt */
  setBacklight(BACKLIGHT_LEVEL);

  /* One sprite, reused by every text band. 282 x 28 x 2 B = ~15.8 KB.
   * Allocated once at boot, before the TLS stack has fragmented the heap. */
  _band.setColorDepth(16);
  _bandSpriteOk = (_band.createSprite(Layout::TEXT_W, Layout::BAND_H) != nullptr);
  if (!_bandSpriteOk) {
    LOG_E("band sprite alloc failed (%d x %d); falling back to clipped draw",
          Layout::TEXT_W, Layout::BAND_H);
  } else {
    LOG_I("band sprite %dx%d ok, free heap %u",
          Layout::TEXT_W, Layout::BAND_H, (unsigned)ESP.getFreeHeap());
  }

  /* Band carries default member initialisers, so under gnu++11 it is not an
   * aggregate and cannot be brace-initialised. This plain table is, and it
   * keeps the four band definitions readable in one block. */
  struct BandInit {
    BandId         id;
    int16_t        y;
    const GFXfont *font;      /* nullptr = built-in font 2 */
    uint16_t       colour;
  };
  static const BandInit kBands[] = {
    {B_TITLE,   Layout::BAND_TITLE_Y,  FONT_TITLE, Theme::TEXT},
    {B_ARTIST,  Layout::BAND_ARTIST_Y, FONT_BODY,  Theme::TEXT_DIM},
    {B_ALBUM,   Layout::BAND_ALBUM_Y,  nullptr,    Theme::TEXT_MUTE},
    {B_MESSAGE, Layout::BAND_MSG_Y,    nullptr,    Theme::WARN},
  };
  for (const BandInit &i : kBands) {
    _bands[i.id].y      = i.y;
    _bands[i.id].font   = i.font;
    _bands[i.id].colour = i.colour;
  }
}

void DisplayManager::setBacklight(uint8_t duty) {
#if PIN_TFT_BL >= 0
  static bool attached = false;
  if (!attached) {
    ledcSetup(TFT_BL_PWM_CHANNEL, TFT_BL_PWM_FREQ, TFT_BL_PWM_BITS);
    ledcAttachPin(PIN_TFT_BL, TFT_BL_PWM_CHANNEL);
    attached = true;
  }
  const uint8_t v = TFT_BL_ACTIVE_HIGH ? duty : (uint8_t)(255 - duty);
  ledcWrite(TFT_BL_PWM_CHANNEL, v);
#else
  (void)duty;   /* backlight hard-wired to 3V3 - nothing to drive */
#endif
}

void DisplayManager::invalidateAll() {
  _forceAll      = true;
  _lastBars      = -1;
  _lastApi       = ApiState::Init;
  _lastVolume    = -2;
  _lastVolSlider = -2;
  _lastFillW     = -1;
  _lastPosSec    = UINT32_MAX;
  _lastDurSec    = UINT32_MAX;
  _lastDevice[0] = '\0';
  _notice1[0] = _notice2[0] = _notice3[0] = '\0';
  for (auto &b : _bands) b.dirty = true;
}

/* =========================================================================
 *  Boot / fatal screens
 * ====================================================================== */
void DisplayManager::showBootScreen(const char *line) {
  _tft.fillScreen(Theme::BG);
  _tft.setTextDatum(MC_DATUM);
  _tft.setFreeFont(FONT_TITLE);
  _tft.setTextColor(Theme::ACCENT, Theme::BG);
  _tft.drawString("Now Playing", SCREEN_W / 2, SCREEN_H / 2 - 24);
  _tft.setFreeFont(FONT_BODY);
  _tft.setTextColor(Theme::TEXT_DIM, Theme::BG);
  _tft.setTextPadding(SCREEN_W - 40);
  _tft.drawString(line ? line : "", SCREEN_W / 2, SCREEN_H / 2 + 16);
  _tft.setTextPadding(0);
  invalidateAll();
}

void DisplayManager::showFatal(const char *title, const char *detail) {
  _tft.fillScreen(Theme::BG);
  _tft.setTextDatum(MC_DATUM);
  _tft.setFreeFont(FONT_TITLE);
  _tft.setTextColor(Theme::ERR, Theme::BG);
  _tft.drawString(title ? title : "Error", SCREEN_W / 2, SCREEN_H / 2 - 20);
  _tft.setFreeFont(FONT_BODY);
  _tft.setTextColor(Theme::TEXT_DIM, Theme::BG);
  _tft.drawString(detail ? detail : "", SCREEN_W / 2, SCREEN_H / 2 + 16);
  invalidateAll();
}

/* =========================================================================
 *  Static chrome - drawn once, then only patched
 * ====================================================================== */
void DisplayManager::drawStaticChrome() {
  /* fillScreen erases the album art along with everything else. Flag it so
   * the caller can drop the artwork cache and re-fetch, rather than leaving
   * an empty frame until the next album change. */
  if (_artPainted) _artInvalidated = true;
  _artPainted = false;

  _tft.fillScreen(Theme::BG);
  _tft.fillRect(0, Layout::STATUS_Y, SCREEN_W, Layout::STATUS_H, Theme::PANEL);
  _tft.drawFastHLine(0, Layout::STATUS_H, SCREEN_W, Theme::TROUGH);

  /* Progress trough. */
  _tft.fillRoundRect(Layout::PROG_X, Layout::PROG_Y, Layout::PROG_W,
                     Layout::PROG_H, Layout::PROG_H / 2, Theme::TROUGH);

  iconSpeaker(_tft, Layout::VOL_ICON_X, Layout::VOL_ICON_Y, Theme::TEXT_MUTE);
}

void DisplayManager::artFrame() {
  _tft.drawRoundRect(Layout::ART_X - 1, Layout::ART_Y - 1,
                     Layout::ART_W + 2, Layout::ART_H + 2, 5, Theme::TROUGH);
}

void DisplayManager::artClearBox() {
  _tft.fillRoundRect(Layout::ART_X, Layout::ART_Y,
                     Layout::ART_W, Layout::ART_H, 4, Theme::PANEL);
  _artPainted = false;
}

void DisplayManager::artMarkPainted() { _artPainted = true; }

void DisplayManager::artDrawPlaceholder(bool loading) {
  artClearBox();
  iconNote(_tft, Layout::ART_X + Layout::ART_W / 2 - 4,
           Layout::ART_Y + Layout::ART_H / 2 - 10,
           loading ? Theme::TEXT_MUTE : Theme::TROUGH);
  _tft.setTextDatum(MC_DATUM);
  _tft.setTextFont(FONT_SMALL_N);
  _tft.setTextColor(Theme::TEXT_MUTE, Theme::PANEL);
  _tft.drawString(loading ? "loading" : "no artwork",
                  Layout::ART_X + Layout::ART_W / 2,
                  Layout::ART_Y + Layout::ART_H - 26);
  artFrame();
}

/* =========================================================================
 *  Text bands
 * ====================================================================== */
void DisplayManager::applyFont(TFT_eSPI &target, const GFXfont *font) {
  if (font) target.setFreeFont(font);
  else      target.setTextFont(FONT_SMALL_N);
}

int16_t DisplayManager::measure(const GFXfont *font, const char *text) {
  applyFont(_tft, font);
  return (int16_t)_tft.textWidth(text);
}

void DisplayManager::setBandText(BandId id, const char *text, uint16_t colour) {
  Band &b = _bands[id];
  const char *t = text ? text : "";
  if (strcmp(b.text, t) == 0 && b.colour == colour) return;

  strlcpy(b.text, t, sizeof(b.text));
  b.colour   = colour;
  b.textW    = measure(b.font, b.text);
  b.scrolls  = b.textW > Layout::TEXT_W;
  b.offset   = 0;
  b.dir      = 1;
  /* Hold at the start before the first scroll, so the beginning of a new
   * title is readable rather than immediately sliding away. */
  b.nextStep = millis() + MARQUEE_PAUSE_MS;
  b.dirty    = true;
}

void DisplayManager::renderBand(Band &b) {
  const int16_t x = Layout::TEXT_X;
  const int16_t midY = Layout::BAND_H / 2;

  if (_bandSpriteOk) {
    _band.fillSprite(Theme::BG);
    applyFont(_band, b.font);
    _band.setTextDatum(ML_DATUM);
    _band.setTextColor(b.colour, Theme::BG);
    _band.drawString(b.text, -b.offset, midY);
    _band.pushSprite(x, b.y);
  } else {
    /* Fallback: clip with a viewport. Coordinates inside a viewport are
     * relative to its origin, and fillScreen fills only the viewport. */
    _tft.setViewport(x, b.y, Layout::TEXT_W, Layout::BAND_H);
    _tft.fillScreen(Theme::BG);
    applyFont(_tft, b.font);
    _tft.setTextDatum(ML_DATUM);
    _tft.setTextColor(b.colour, Theme::BG);
    _tft.drawString(b.text, -b.offset, midY);
    _tft.resetViewport();
  }
  b.dirty = false;
}

void DisplayManager::stepMarquees() {
  const uint32_t now = millis();
  for (auto &b : _bands) {
    if (!b.scrolls || !b.text[0]) continue;
    if ((int32_t)(now - b.nextStep) < 0) continue;

    /* Scroll until the tail is flush with the right edge, then reverse.
     * Ping-pong rather than wrap-around: no gap to size, no second copy of
     * the string to draw, and the start of the text is never mid-slide. */
    const int16_t maxOffset = b.textW - Layout::TEXT_W;
    b.offset += b.dir;

    if (b.offset >= maxOffset) {
      b.offset   = maxOffset;
      b.dir      = -1;
      b.nextStep = now + MARQUEE_PAUSE_MS;
    } else if (b.offset <= 0) {
      b.offset   = 0;
      b.dir      = 1;
      b.nextStep = now + MARQUEE_PAUSE_MS;
    } else {
      b.nextStep = now + MARQUEE_STEP_MS;
    }
    b.dirty = true;
  }
}

/* =========================================================================
 *  Status bar
 * ====================================================================== */
void DisplayManager::drawWifiIcon(int8_t bars, NetState net) {
  const int16_t x = Layout::WIFI_X, y = Layout::WIFI_Y;
  _tft.fillRect(x, y, Layout::WIFI_W, Layout::WIFI_H, Theme::PANEL);

  if (net == NetState::Down) {
    /* A cross, so "no Wi-Fi" is unmistakable rather than just "zero bars". */
    _tft.drawLine(x + 3, y + 3, x + 14, y + 12, Theme::ERR);
    _tft.drawLine(x + 14, y + 3, x + 3, y + 12, Theme::ERR);
    return;
  }

  for (int i = 0; i < 4; ++i) {
    const int16_t h  = 4 + i * 3;
    const int16_t bx = x + i * 5;
    const int16_t by = y + Layout::WIFI_H - h;
    uint16_t c;
    if (net == NetState::Connecting) {
      /* Sweep one lit bar left to right while associating. */
      c = ((millis() / 250) % 4 == (uint32_t)i) ? Theme::WARN : Theme::TROUGH;
    } else {
      c = (i < bars) ? Theme::ACCENT : Theme::TROUGH;
    }
    _tft.fillRect(bx, by, 4, h, c);
  }
}

void DisplayManager::drawLinkDot(ApiState api) {
  uint16_t c;
  switch (api) {
    case ApiState::Ok:
    case ApiState::NoDevice:    c = Theme::ACCENT;   break;
    case ApiState::Refreshing:
    case ApiState::RateLimited: c = Theme::WARN;     break;
    case ApiState::AuthFailed:
    case ApiState::NoToken:
    case ApiState::Error:       c = Theme::ERR;      break;
    default:                    c = Theme::TEXT_MUTE; break;
  }
  _tft.fillCircle(Layout::LINK_DOT_X, Layout::LINK_DOT_Y, Layout::LINK_DOT_R, c);
  /* A ring around a hollow centre when there is no active device: the link
   * is healthy but nothing is listening. */
  if (api == ApiState::NoDevice) {
    _tft.fillCircle(Layout::LINK_DOT_X, Layout::LINK_DOT_Y,
                    Layout::LINK_DOT_R - 2, Theme::PANEL);
  }
}

void DisplayManager::fitEllipsis(TFT_eSPI &t, const char *src, char *dst,
                                 size_t dstLen, int16_t maxW) {
  strlcpy(dst, src ? src : "", dstLen);
  if (t.textWidth(dst) <= maxW) return;

  /* Trim one character at a time until the string plus an ellipsis fits.
   * Linear, but it runs only when the text changes, not per frame. */
  size_t n = strlen(dst);
  while (n > 1) {
    dst[--n] = '\0';
    char probe[192];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (t.textWidth(probe) <= maxW) {
      strlcpy(dst, probe, dstLen);
      return;
    }
  }
}

void DisplayManager::drawStatusBar(const AppStatus &s, const PlaybackState &st,
                                   bool force) {
  /* The connecting animation and the RSSI both move, so this repaints on a
   * change of bars, of state, or continuously while associating. */
  if (force || s.rssiBars != _lastBars || s.net != _lastNet ||
      s.net == NetState::Connecting) {
    drawWifiIcon(s.rssiBars, s.net);
    _lastBars = s.rssiBars;
    _lastNet  = s.net;
  }

  if (force || s.api != _lastApi) {
    drawLinkDot(s.api);
    _lastApi = s.api;
  }

  if (force || strcmp(_lastDevice, st.device) != 0) {
    strlcpy(_lastDevice, st.device, sizeof(_lastDevice));
    _tft.setTextFont(FONT_SMALL_N);

    /* "Playing on: <device>" is the phrasing the spec calls for, and it
     * doubles as the button that opens the device picker. */
    char line[DEVICE_LEN + 24];
    if (st.device[0]) snprintf(line, sizeof(line), "Playing on: %s", st.device);
    else              strlcpy(line, "No active device", sizeof(line));

    char shown[sizeof(line) + 4];
    fitEllipsis(_tft, line, shown, sizeof(shown), Layout::DEVICE_W - 14);

    _tft.fillRect(Layout::DEVICE_X, Layout::DEVICE_Y - 2,
                  Layout::DEVICE_W, 20, Theme::PANEL);
    _tft.setTextDatum(TL_DATUM);
    _tft.setTextColor(st.device[0] ? Theme::TEXT_DIM : Theme::TEXT_MUTE,
                      Theme::PANEL);
    _tft.drawString(shown, Layout::DEVICE_X, Layout::DEVICE_Y);
    iconChevron(_tft, Layout::DEVICE_X + _tft.textWidth(shown) + 6,
                Layout::DEVICE_Y + 8, Theme::TEXT_MUTE);
  }

  if (force || st.volumePercent != _lastVolume) {
    _lastVolume = st.volumePercent;
    char buf[16];
    if (st.volumePercent >= 0) snprintf(buf, sizeof(buf), "%d%%", st.volumePercent);
    else                       strlcpy(buf, "--", sizeof(buf));
    _tft.setTextFont(FONT_SMALL_N);
    _tft.setTextDatum(TR_DATUM);
    _tft.setTextColor(Theme::TEXT_DIM, Theme::PANEL);
    _tft.setTextPadding(60);
    _tft.drawString(buf, Layout::VOL_TEXT_R, Layout::VOL_TEXT_Y);
    _tft.setTextPadding(0);
  }
}

/* =========================================================================
 *  Progress bar and timecodes
 * ====================================================================== */
void DisplayManager::drawProgress(const PlaybackState &st, bool force) {
  const uint32_t pos = st.progressNow();
  int16_t fillW = 0;
  if (st.durationMs > 0) {
    /* 64-bit intermediate: pos can exceed 9 million ms on a long podcast,
     * and pos * PROG_W would overflow 32 bits well before that. */
    const uint64_t num = (uint64_t)pos * (uint64_t)Layout::PROG_W;
    fillW = (int16_t)(num / st.durationMs);
    if (fillW > Layout::PROG_W) fillW = Layout::PROG_W;
  }

  if (!force && fillW == _lastFillW) return;

  if (force || fillW < _lastFillW || _lastFillW < 0) {
    /* Seek backwards or first draw: repaint the whole bar. */
    _tft.fillRoundRect(Layout::PROG_X, Layout::PROG_Y, Layout::PROG_W,
                       Layout::PROG_H, Layout::PROG_H / 2, Theme::TROUGH);
    if (fillW > 0) {
      _tft.fillRoundRect(Layout::PROG_X, Layout::PROG_Y, fillW,
                         Layout::PROG_H, Layout::PROG_H / 2, Theme::ACCENT);
    }
  } else {
    /* Forward progress: paint only the newly covered strip. A second of
     * playback on a 3-minute track is about 2 px. */
    _tft.fillRect(Layout::PROG_X + _lastFillW, Layout::PROG_Y,
                  fillW - _lastFillW, Layout::PROG_H, Theme::ACCENT);
  }
  _lastFillW = fillW;
}

void DisplayManager::formatTime(uint32_t ms, char *out, size_t len) {
  const uint32_t total = ms / 1000;
  const uint32_t h = total / 3600;
  const uint32_t m = (total % 3600) / 60;
  const uint32_t s = total % 60;
  if (h) snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)h,
                  (unsigned long)m, (unsigned long)s);
  else   snprintf(out, len, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

void DisplayManager::drawTimes(uint32_t posMs, uint32_t durMs, bool force) {
  const uint32_t posSec = posMs / 1000;
  const uint32_t durSec = durMs / 1000;
  char buf[16];

  _tft.setTextFont(FONT_SMALL_N);
  _tft.setTextColor(Theme::TEXT_MUTE, Theme::BG);
  _tft.setTextPadding(70);

  if (force || posSec != _lastPosSec) {
    _lastPosSec = posSec;
    formatTime(posMs, buf, sizeof(buf));
    _tft.setTextDatum(TL_DATUM);
    _tft.drawString(buf, Layout::PROG_X, Layout::TIME_Y);
  }
  if (force || durSec != _lastDurSec) {
    _lastDurSec = durSec;
    if (durMs) formatTime(durMs, buf, sizeof(buf));
    else       strlcpy(buf, "--:--", sizeof(buf));
    _tft.setTextDatum(TR_DATUM);
    _tft.drawString(buf, Layout::PROG_X + Layout::PROG_W, Layout::TIME_Y);
  }
  _tft.setTextPadding(0);
}

/* =========================================================================
 *  Transport controls
 * ====================================================================== */
void DisplayManager::drawButton(int16_t x, int16_t w, UiTarget which,
                                const PlaybackState &st, bool pressed,
                                bool enabled) {
  const int16_t y  = Layout::BTN_Y;
  const int16_t cx = x + w / 2;
  const int16_t cy = y + Layout::BTN_H / 2;

  _tft.fillRoundRect(x, y, w, Layout::BTN_H, 8,
                     pressed ? Theme::BTN_DOWN : Theme::BTN);
  _tft.drawRoundRect(x, y, w, Layout::BTN_H, 8,
                     pressed ? Theme::ACCENT : Theme::TROUGH);

  uint16_t fg = enabled ? Theme::TEXT : Theme::TEXT_MUTE;

  switch (which) {
    case UiTarget::Prev: iconPrev(_tft, cx, cy, fg); break;
    case UiTarget::Next: iconNext(_tft, cx, cy, fg); break;
    case UiTarget::PlayPause:
      if (enabled) fg = Theme::ACCENT;
      if (st.isPlaying) iconPause(_tft, cx, cy, fg);
      else              iconPlay(_tft, cx, cy, fg);
      break;
    case UiTarget::Shuffle:
      /* Lit green when on, grey when off - the state IS the icon colour,
       * so there is no separate indicator to keep in sync. */
      iconShuffle(_tft, cx, cy,
                  !enabled ? Theme::TEXT_MUTE
                           : (st.shuffle ? Theme::ACCENT : Theme::TEXT_MUTE));
      break;
    case UiTarget::Repeat:
      iconRepeat(_tft, cx, cy,
                 !enabled ? Theme::TEXT_MUTE
                          : (st.repeat == RepeatMode::Off ? Theme::TEXT_MUTE
                                                          : Theme::ACCENT),
                 st.repeat == RepeatMode::Track);
      break;
    default: break;
  }
}

void DisplayManager::drawTransport(const PlaybackState &st, bool force) {
  const bool enabled = st.hasDevice;
  const bool changed = (st.isPlaying != _lastPlaying) ||
                       (st.hasDevice != _lastHasDev) ||
                       (st.shuffle != _lastShuffle) ||
                       (st.repeat != _lastRepeat) ||
                       (_pressed != _lastPressed);

  if (!force && !changed) return;

  drawButton(Layout::BTN_SHUF_X, Layout::SMALL_W, UiTarget::Shuffle, st,
             _pressed == UiTarget::Shuffle, enabled);
  drawButton(Layout::BTN_PREV_X, Layout::BTN_W, UiTarget::Prev, st,
             _pressed == UiTarget::Prev, enabled);
  drawButton(Layout::BTN_PLAY_X, Layout::PLAY_W, UiTarget::PlayPause, st,
             _pressed == UiTarget::PlayPause, enabled);
  drawButton(Layout::BTN_NEXT_X, Layout::BTN_W, UiTarget::Next, st,
             _pressed == UiTarget::Next, enabled);
  drawButton(Layout::BTN_REPT_X, Layout::SMALL_W, UiTarget::Repeat, st,
             _pressed == UiTarget::Repeat, enabled);

  _lastPlaying = st.isPlaying;
  _lastHasDev  = st.hasDevice;
  _lastShuffle = st.shuffle;
  _lastRepeat  = st.repeat;
  _lastPressed = _pressed;
}

void DisplayManager::drawVolume(const PlaybackState &st, bool force) {
  const bool ctl = st.supportsVolume && st.volumePercent >= 0;
  if (!force && st.volumePercent == _lastVolSlider && ctl == _lastVolCtl) return;
  _lastVolSlider = st.volumePercent;
  _lastVolCtl    = ctl;

  const int16_t x = Layout::VOL_TRACK_X;
  const int16_t w = Layout::VOL_TRACK_W;
  const int16_t y = Layout::VOL_Y;

  _tft.fillRect(x - 2, y - Layout::VOL_KNOB_R - 2, w + 4,
                Layout::VOL_KNOB_R * 2 + 6, Theme::BG);
  _tft.fillRoundRect(x, y, w, Layout::VOL_H, Layout::VOL_H / 2, Theme::TROUGH);

  if (ctl) {
    const int16_t fw = (int16_t)((int32_t)w * st.volumePercent / 100);
    if (fw > 0) {
      _tft.fillRoundRect(x, y, fw, Layout::VOL_H, Layout::VOL_H / 2, Theme::ACCENT);
    }
    _tft.fillCircle(x + fw, y + Layout::VOL_H / 2, Layout::VOL_KNOB_R, Theme::TEXT);
  }
}

/* =========================================================================
 *  Hit testing
 * ====================================================================== */
UiTarget DisplayManager::hitTest(int16_t x, int16_t y) const {
  /* The "Playing on: ..." strip opens the device picker. */
  if (y >= Layout::STATUS_Y && y <= Layout::STATUS_Y + Layout::STATUS_H &&
      x >= Layout::DEVICE_X && x <= Layout::DEVICE_X + Layout::DEVICE_W) {
    return UiTarget::DeviceSelect;
  }

  if (y >= Layout::BTN_Y && y <= Layout::BTN_Y + Layout::BTN_H) {
    if (x >= Layout::BTN_SHUF_X && x < Layout::BTN_SHUF_X + Layout::SMALL_W)
      return UiTarget::Shuffle;
    if (x >= Layout::BTN_PREV_X && x < Layout::BTN_PREV_X + Layout::BTN_W)
      return UiTarget::Prev;
    if (x >= Layout::BTN_PLAY_X && x < Layout::BTN_PLAY_X + Layout::PLAY_W)
      return UiTarget::PlayPause;
    if (x >= Layout::BTN_NEXT_X && x < Layout::BTN_NEXT_X + Layout::BTN_W)
      return UiTarget::Next;
    if (x >= Layout::BTN_REPT_X && x < Layout::BTN_REPT_X + Layout::SMALL_W)
      return UiTarget::Repeat;
  }

  /* The volume band is deliberately much taller than the 8 px track - a
   * fingertip on a resistive panel cannot reliably hit 8 px. */
  if (y >= Layout::VOL_TOUCH_Y && y <= Layout::VOL_TOUCH_Y + Layout::VOL_TOUCH_H &&
      x >= Layout::VOL_X && x <= Layout::VOL_X + Layout::VOL_W) {
    return UiTarget::Volume;
  }

  /* Same reasoning for the 6 px progress bar. */
  if (y >= Layout::SEEK_TOUCH_Y && y <= Layout::SEEK_TOUCH_Y + Layout::SEEK_TOUCH_H &&
      x >= Layout::PROG_X && x <= Layout::PROG_X + Layout::PROG_W) {
    return UiTarget::Seek;
  }
  return UiTarget::None;
}

int16_t DisplayManager::volumeFromX(int16_t x) const {
  const int16_t x0 = Layout::VOL_TRACK_X;
  const int16_t w  = Layout::VOL_TRACK_W;
  int32_t pct = (int32_t)(x - x0) * 100 / (w ? w : 1);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (int16_t)pct;
}

uint32_t DisplayManager::seekFromX(int16_t x, uint32_t durationMs) const {
  if (!durationMs) return 0;
  int32_t dx = x - Layout::PROG_X;
  if (dx < 0) dx = 0;
  if (dx > Layout::PROG_W) dx = Layout::PROG_W;
  /* 64-bit: a 3-hour podcast in ms times 456 overflows 32 bits. */
  return (uint32_t)(((uint64_t)durationMs * (uint64_t)dx) / Layout::PROG_W);
}

void DisplayManager::setButtonPressed(UiTarget t) { _pressed = t; }

/* =========================================================================
 *  The Now Playing entry point
 * ====================================================================== */
void DisplayManager::update(const PlaybackState &st, const AppStatus &status) {
  bool force = _forceAll;
  if (force) {
    drawStaticChrome();
    _forceAll = false;
    /* Set to the opposite of the incoming state so the content-area
     * branch below always fires and repaints it. */
    _lastNoDevice = st.hasDevice;
  }

  /* The content area is either artwork + metadata, or the "nothing is
   * listening" notice. Switching between them repaints that whole region,
   * because the two layouts share no elements. */
  const bool noDevice = !st.hasDevice;
  if (noDevice != _lastNoDevice || force) {
    _lastNoDevice = noDevice;
    _tft.fillRect(0, Layout::STATUS_H + 1, SCREEN_W,
                  Layout::SEEK_TOUCH_Y - Layout::STATUS_H - 1, Theme::BG);
    if (noDevice) {
      if (_artPainted) _artInvalidated = true;
      _artPainted = false;
    } else {
      artDrawPlaceholder(status.artBusy);
      for (auto &b : _bands) b.dirty = true;
    }
    force = true;
  }

  if (noDevice) {
    /* The exact wording the brief asks for, plus whatever transient
     * message is live underneath it. */
    drawNotice("No active Spotify device.",
               "Open Spotify on your phone, TV or computer.",
               status.message[0] ? status.message : nullptr);
  } else {
    if (st.hasItem) {
      setBandText(B_TITLE, st.title, Theme::TEXT);
      setBandText(B_ARTIST, st.artist, Theme::TEXT_DIM);
      setBandText(B_ALBUM, st.album, Theme::TEXT_MUTE);
    } else {
      setBandText(B_TITLE, "Nothing playing", Theme::TEXT_DIM);
      setBandText(B_ARTIST, "", Theme::TEXT_DIM);
      setBandText(B_ALBUM, "", Theme::TEXT_MUTE);
    }

    uint16_t msgColour = Theme::WARN;
    if (status.api == ApiState::AuthFailed || status.api == ApiState::Error ||
        status.api == ApiState::NoToken) {
      msgColour = Theme::ERR;
    }
    setBandText(B_MESSAGE, status.message, msgColour);

    stepMarquees();
    for (auto &b : _bands) {
      if (b.dirty) renderBand(b);
    }
  }

  drawStatusBar(status, st, force);
  drawProgress(st, force);
  drawTimes(st.progressNow(), st.durationMs, force);
  drawTransport(st, force);
  drawVolume(st, force);
}

/* =========================================================================
 *  Shared primitives for the other screens
 * ====================================================================== */
void DisplayManager::clearScreen(uint16_t colour) {
  if (_artPainted) _artInvalidated = true;
  _artPainted = false;
  _tft.fillScreen(colour);
  /* Anything cached about the Now Playing screen is now a lie. */
  invalidateAll();
}

void DisplayManager::drawScreenHeader(const char *title, const char *leftLabel,
                                      const char *rightLabel) {
  _tft.fillRect(0, 0, SCREEN_W, ListLayout::HEADER_H, Theme::PANEL);
  _tft.drawFastHLine(0, ListLayout::HEADER_H, SCREEN_W, Theme::TROUGH);

  if (leftLabel && *leftLabel) {
    drawKeyCap(ListLayout::BACK_X, 3, ListLayout::BACK_W,
               ListLayout::HEADER_H - 6, leftLabel, false, false);
  }
  if (rightLabel && *rightLabel) {
    drawKeyCap(ListLayout::ACTION_X, 3, ListLayout::ACTION_W,
               ListLayout::HEADER_H - 6, rightLabel, false, false);
  }

  _tft.setFreeFont(FONT_BODY);
  _tft.setTextDatum(MC_DATUM);
  _tft.setTextColor(Theme::TEXT, Theme::PANEL);
  _tft.drawString(title ? title : "", SCREEN_W / 2, ListLayout::HEADER_H / 2);
}

void DisplayManager::drawListRow(int16_t y, const char *line1,
                                 const char *line2, uint16_t badge,
                                 bool highlight, bool dim) {
  const int16_t x = ListLayout::ROW_X;
  const int16_t w = ListLayout::ROW_W;
  const int16_t h = ListLayout::ROW_H;

  _tft.fillRoundRect(x, y, w, h, 6, highlight ? Theme::BTN_DOWN : Theme::PANEL);
  _tft.drawRoundRect(x, y, w, h, 6, highlight ? Theme::ACCENT : Theme::TROUGH);

  /* Reserve the right-hand strip for the badge so long names cannot run
   * underneath it. */
  const int16_t textW = w - 20 - (badge != Theme::BG ? 22 : 0);

  _tft.setFreeFont(FONT_BODY);
  _tft.setTextDatum(TL_DATUM);
  _tft.setTextColor(dim ? Theme::TEXT_MUTE : Theme::TEXT,
                    highlight ? Theme::BTN_DOWN : Theme::PANEL);
  char shown[160];
  fitEllipsis(_tft, line1, shown, sizeof(shown), textW);
  _tft.drawString(shown, x + 10, y + 5);

  if (line2 && *line2) {
    _tft.setTextFont(FONT_SMALL_N);
    _tft.setTextColor(Theme::TEXT_MUTE, highlight ? Theme::BTN_DOWN : Theme::PANEL);
    fitEllipsis(_tft, line2, shown, sizeof(shown), textW);
    _tft.drawString(shown, x + 10, y + 26);
  }

  if (badge != Theme::BG) {
    _tft.fillCircle(x + w - 16, y + h / 2, 6, badge);
  }
}

void DisplayManager::drawScrollArrows(bool canUp, bool canDown) {
  const int16_t x = ListLayout::SCROLL_X;
  const int16_t w = ListLayout::SCROLL_W;

  _tft.fillRect(x, ListLayout::LIST_Y, w,
                SCREEN_H - ListLayout::LIST_Y, Theme::BG);

  const int16_t cx = x + w / 2;
  if (canUp) {
    const int16_t cy = ListLayout::SCROLL_UP_Y + ListLayout::SCROLL_BTN_H / 2;
    _tft.fillRoundRect(x, ListLayout::SCROLL_UP_Y, w,
                       ListLayout::SCROLL_BTN_H, 5, Theme::BTN);
    _tft.fillTriangle(cx, cy - 7, cx - 8, cy + 5, cx + 8, cy + 5, Theme::TEXT);
  }
  if (canDown) {
    const int16_t cy = ListLayout::SCROLL_DN_Y + ListLayout::SCROLL_BTN_H / 2;
    _tft.fillRoundRect(x, ListLayout::SCROLL_DN_Y, w,
                       ListLayout::SCROLL_BTN_H, 5, Theme::BTN);
    _tft.fillTriangle(cx, cy + 7, cx - 8, cy - 5, cx + 8, cy - 5, Theme::TEXT);
  }
}

void DisplayManager::drawKeyCap(int16_t x, int16_t y, int16_t w, int16_t h,
                                const char *label, bool pressed, bool accent) {
  const uint16_t face = pressed ? Theme::BTN_DOWN
                                : (accent ? Theme::ACCENT_DK : Theme::BTN);
  _tft.fillRoundRect(x, y, w, h, 6, face);
  _tft.drawRoundRect(x, y, w, h, 6,
                     pressed || accent ? Theme::ACCENT : Theme::TROUGH);

  if (!label || !*label) return;
  /* Single characters get the larger face; word labels get the small font
   * so "Cancel" or "Rescan" still fit a narrow cap. */
  if (strlen(label) <= 2) _tft.setFreeFont(FONT_BODY);
  else                    _tft.setTextFont(FONT_SMALL_N);
  _tft.setTextDatum(MC_DATUM);
  _tft.setTextColor(Theme::TEXT, face);
  _tft.drawString(label, x + w / 2, y + h / 2);
}

void DisplayManager::drawTextField(int16_t x, int16_t y, int16_t w, int16_t h,
                                   const char *text, bool masked,
                                   const char *placeholder) {
  _tft.fillRoundRect(x, y, w, h, 6, Theme::PANEL);
  _tft.drawRoundRect(x, y, w, h, 6, Theme::ACCENT);

  char shown[128];
  const bool empty = (!text || !*text);
  if (empty) {
    strlcpy(shown, placeholder ? placeholder : "", sizeof(shown));
  } else if (masked) {
    /* Show the length, not the characters. The last character stays
     * visible for a moment in the caller's flow if it wants that; here it
     * is fully masked so a shoulder-surfer gets nothing. */
    const size_t n = strnlen(text, sizeof(shown) - 1);
    memset(shown, '*', n);
    shown[n] = '\0';
  } else {
    strlcpy(shown, text, sizeof(shown));
  }

  _tft.setFreeFont(FONT_BODY);
  _tft.setTextDatum(ML_DATUM);
  _tft.setTextColor(empty ? Theme::TEXT_MUTE : Theme::TEXT, Theme::PANEL);

  /* Keep the TAIL of a long entry visible - that is where the cursor is. */
  char fitted[128];
  strlcpy(fitted, shown, sizeof(fitted));
  const int16_t maxW = w - 20;
  size_t skip = 0;
  while (_tft.textWidth(fitted + skip) > maxW && fitted[skip]) skip++;
  _tft.drawString(fitted + skip, x + 10, y + h / 2);
}

void DisplayManager::drawToggle(int16_t x, int16_t y, int16_t w, int16_t h,
                                const char *label, bool on) {
  _tft.fillRect(x, y, w, h, Theme::BG);

  const int16_t boxW = 40, boxH = 20;
  const int16_t bx = x + w - boxW;
  const int16_t by = y + (h - boxH) / 2;

  _tft.setTextFont(FONT_SMALL_N);
  _tft.setTextDatum(ML_DATUM);
  _tft.setTextColor(Theme::TEXT_DIM, Theme::BG);
  _tft.drawString(label ? label : "", x, y + h / 2);

  _tft.fillRoundRect(bx, by, boxW, boxH, boxH / 2,
                     on ? Theme::ACCENT : Theme::TROUGH);
  _tft.fillCircle(on ? bx + boxW - boxH / 2 : bx + boxH / 2, by + boxH / 2,
                  boxH / 2 - 3, Theme::TEXT);
}

void DisplayManager::drawCentredText(const char *text, int16_t y,
                                     const GFXfont *font, uint16_t colour,
                                     uint16_t bg) {
  applyFont(_tft, font);
  _tft.setTextDatum(MC_DATUM);
  _tft.setTextColor(colour, bg);
  _tft.drawString(text ? text : "", SCREEN_W / 2, y);
}

void DisplayManager::drawNotice(const char *line1, const char *line2,
                                const char *line3) {
  const char *l3 = line3 ? line3 : "";

  /* Only repaint when the wording actually changes; this is called every
   * loop iteration while no device is active. */
  if (strcmp(_notice1, line1) == 0 && strcmp(_notice2, line2) == 0 &&
      strcmp(_notice3, l3) == 0) {
    return;
  }
  strlcpy(_notice1, line1, sizeof(_notice1));
  strlcpy(_notice2, line2, sizeof(_notice2));
  strlcpy(_notice3, l3, sizeof(_notice3));

  _tft.fillRect(0, Layout::STATUS_H + 1, SCREEN_W,
                Layout::SEEK_TOUCH_Y - Layout::STATUS_H - 1, Theme::BG);
  drawCentredText(line1, 90, FONT_TITLE, Theme::TEXT, Theme::BG);
  drawCentredText(line2, 128, FONT_BODY, Theme::TEXT_DIM, Theme::BG);
  if (*l3) drawCentredText(l3, 162, nullptr, Theme::WARN, Theme::BG);
}
