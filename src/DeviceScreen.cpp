#include "DeviceScreen.h"

#include "Log.h"

void DeviceScreen::enter() {
  _scroll        = 0;
  _dirty         = true;
  _lastCount     = 0xFF;
  _lastScroll    = 0xFF;
  _lastUpdatedAt = 0;
  _pressedRow    = -1;
  _display->clearScreen();
}

void DeviceScreen::clampScroll(uint8_t count) {
  const uint8_t maxScroll =
      (count > ListLayout::VISIBLE) ? (uint8_t)(count - ListLayout::VISIBLE) : 0;
  if (_scroll > maxScroll) _scroll = maxScroll;
}

/* Describe a device in one short line. The brief asks for six facts per
 * device: name, type, active, volume, whether volume can be controlled,
 * and whether it is restricted. Name is the row title and active is the
 * green dot, so the remaining four go here - including the volume reading
 * even when it cannot be changed, because "playing at 80% and I cannot
 * turn it down from here" is exactly what the user needs to know. */
static void describe(const SpotifyDevice &d, char *out, size_t len) {
  char vol[20];
  if (d.volumePercent >= 0) snprintf(vol, sizeof(vol), "vol %d%%", d.volumePercent);
  else                      strlcpy(vol, "vol --", sizeof(vol));

  const char *flag = d.isRestricted      ? "  -  restricted"
                     : !d.supportsVolume ? "  -  no remote volume"
                                         : "";

  snprintf(out, len, "%s  -  %s%s", d.type[0] ? d.type : "Device", vol, flag);
}

void DeviceScreen::drawRows(const DeviceList &devices) {
  clampScroll(devices.count);

  for (uint8_t slot = 0; slot < ListLayout::VISIBLE; ++slot) {
    const int16_t y = ListLayout::rowY(slot);
    const uint8_t idx = (uint8_t)(_scroll + slot);

    if (idx >= devices.count) {
      /* Blank the unused slot so a shrinking list does not leave ghosts. */
      _display->tft().fillRect(ListLayout::ROW_X, y, ListLayout::ROW_W,
                               ListLayout::ROW_H, Theme::BG);
      continue;
    }

    const SpotifyDevice &d = devices.items[idx];
    char sub[96];
    describe(d, sub, sizeof(sub));

    /* Green dot marks the device Spotify currently considers active. */
    const uint16_t badge = d.isActive ? Theme::ACCENT : Theme::BG;
    _display->drawListRow(y, d.name, sub, badge, _pressedRow == (int8_t)slot,
                          d.isRestricted);
  }

  const bool canUp   = _scroll > 0;
  const bool canDown = (uint16_t)(_scroll + ListLayout::VISIBLE) < devices.count;
  _display->drawScrollArrows(canUp, canDown);
}

void DeviceScreen::render(const DeviceList &devices, bool authed) {
  const bool changed = _dirty || devices.updatedAt != _lastUpdatedAt ||
                       devices.count != _lastCount || _scroll != _lastScroll ||
                       _keepPlaying != _lastKeep;
  if (!changed) return;

  const bool full = _dirty;
  _dirty         = false;
  _lastUpdatedAt = devices.updatedAt;
  _lastCount     = devices.count;
  _lastScroll    = _scroll;
  _lastKeep      = _keepPlaying;

  if (full) {
    _display->drawScreenHeader("Playback devices", "Back", "Refresh");
  }
  _display->drawToggle(ListLayout::ROW_X, ListLayout::OPT_Y,
                       ListLayout::ROW_W, ListLayout::OPT_H,
                       "Keep playing during transfer", _keepPlaying);

  if (!authed) {
    _display->drawCentredText("Not signed in to Spotify", 150, FONT_BODY,
                              Theme::ERR, Theme::BG);
    return;
  }

  if (devices.count == 0) {
    /* An empty list is the normal state when everything is closed, so it
     * gets the same wording as the Now Playing screen rather than looking
     * like a failure. */
    _display->drawCentredText("No Spotify devices found", 140, FONT_BODY,
                              Theme::TEXT_DIM, Theme::BG);
    _display->drawCentredText("Open Spotify on your phone, TV or computer.",
                              172, nullptr, Theme::TEXT_MUTE, Theme::BG);
    _display->drawScrollArrows(false, false);
    return;
  }

  drawRows(devices);
}

int8_t DeviceScreen::rowAt(int16_t x, int16_t y) const {
  if (x < ListLayout::ROW_X || x > ListLayout::ROW_X + ListLayout::ROW_W) {
    return -1;
  }
  for (uint8_t slot = 0; slot < ListLayout::VISIBLE; ++slot) {
    const int16_t ry = ListLayout::rowY(slot);
    if (y >= ry && y < ry + ListLayout::ROW_H) return (int8_t)slot;
  }
  return -1;
}

DeviceScreen::Result DeviceScreen::handleTouch(const TouchEvent &ev,
                                               const DeviceList &devices,
                                               char *outDeviceId,
                                               size_t outLen) {
  /* Press feedback: highlight the row under the finger straight away. */
  if (ev.phase == TouchPhase::Down) {
    const int8_t slot = rowAt(ev.x, ev.y);
    if (slot >= 0 && (uint16_t)(_scroll + slot) < devices.count) {
      _pressedRow = slot;
      _dirty      = true;
    }
    return Result::None;
  }

  if (ev.phase == TouchPhase::Up) {
    if (_pressedRow >= 0) {
      _pressedRow = -1;
      _dirty      = true;
    }
    return Result::None;
  }

  if (ev.phase != TouchPhase::Tap) return Result::None;

  if (_pressedRow >= 0) {
    _pressedRow = -1;
    _dirty      = true;
  }

  /* ---- header ---- */
  if (ev.y < ListLayout::HEADER_H) {
    if (ev.x >= ListLayout::BACK_X && ev.x < ListLayout::BACK_X + ListLayout::BACK_W) {
      return Result::Back;
    }
    if (ev.x >= ListLayout::ACTION_X) return Result::Refresh;
    return Result::None;
  }

  /* ---- keep-playing toggle ---- */
  if (ev.y >= ListLayout::OPT_Y && ev.y < ListLayout::OPT_Y + ListLayout::OPT_H) {
    _keepPlaying = !_keepPlaying;
    _dirty       = true;
    LOG_I("devices: keep-playing %s", _keepPlaying ? "on" : "off");
    return Result::None;
  }

  /* ---- scroll column ---- */
  if (ev.x >= ListLayout::SCROLL_X) {
    const bool canDown =
        (uint16_t)(_scroll + ListLayout::VISIBLE) < devices.count;
    if (ev.y >= ListLayout::SCROLL_UP_Y &&
        ev.y < ListLayout::SCROLL_UP_Y + ListLayout::SCROLL_BTN_H && _scroll > 0) {
      _scroll--;
      _dirty = true;
    } else if (ev.y >= ListLayout::SCROLL_DN_Y &&
               ev.y < ListLayout::SCROLL_DN_Y + ListLayout::SCROLL_BTN_H && canDown) {
      _scroll++;
      _dirty = true;
    }
    return Result::None;
  }

  /* ---- a device row ---- */
  const int8_t slot = rowAt(ev.x, ev.y);
  if (slot < 0) return Result::None;
  const uint8_t idx = (uint8_t)(_scroll + slot);
  if (idx >= devices.count) return Result::None;

  const SpotifyDevice &d = devices.items[idx];
  if (d.isRestricted) {
    /* Transferring to a restricted device produces a 403 and a confusing
     * dead end, so refuse up front and say why. */
    LOG_I("devices: %s is restricted, refusing transfer", d.name);
    return Result::None;
  }

  strlcpy(outDeviceId, d.id, outLen);
  return Result::Transfer;
}
