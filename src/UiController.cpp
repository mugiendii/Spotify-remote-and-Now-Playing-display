#include "UiController.h"

#include "Log.h"

void UiController::begin(DisplayManager *display, SpotifyClient *spotify,
                         NetManager *net, AlbumArt *art, PlaybackState *state,
                         AppStatus *status) {
  _display = display;
  _spotify = spotify;
  _net     = net;
  _art     = art;
  _state   = state;
  _status  = status;

  _devices.begin(display);
  _wifiList.begin(display);
  _keyboard.begin(display);
  _signIn.begin(display);
}

void UiController::go(Screen s) {
  if (_screen == s) return;
  _screen = s;

  /* The artwork decoder writes to fixed panel coordinates and knows nothing
   * about which screen is up, so an in-flight download must be stopped
   * before the art box is covered by anything else. */
  if (s != Screen::NowPlaying) _art->abort();

  switch (s) {
    case Screen::NowPlaying:
      _display->invalidateAll();
      /* The overlay screens painted over the art box, so the cached cover
       * is no longer on the glass. */
      _art->invalidate();
      break;
    case Screen::Devices:
      _devices.enter();
      /* Opening the picker is an explicit request for fresh information. */
      _spotify->refreshDevicesSoon();
      break;
    case Screen::WifiList:
      _wifiList.enter();
      _net->startScan();
      break;
    case Screen::WifiPassword:
      _keyboard.enter(_pendingSsid, true);
      break;
    case Screen::Setup:
      _signIn.enter();
      _authSettledAt = 0;
      break;
    case Screen::Boot:
      break;
  }
  LOG_D("ui: screen %u", (unsigned)s);
}

/* =========================================================================
 *  Now Playing
 * ====================================================================== */
void UiController::handleNowPlayingTouch(const TouchEvent &ev) {
  switch (ev.phase) {
    case TouchPhase::Down: {
      _downTarget = _display->hitTest(ev.x, ev.y);
      if (_downTarget != UiTarget::None && _downTarget != UiTarget::Volume &&
          _downTarget != UiTarget::Seek && _downTarget != UiTarget::DeviceSelect) {
        /* Light the button immediately - waiting for the API round trip
         * would make the panel feel broken. */
        _display->setButtonPressed(_downTarget);
      }
      if (_downTarget == UiTarget::Volume && _state->supportsVolume) {
        const int16_t pct = _display->volumeFromX(ev.x);
        _spotify->cmdVolume(pct);
        _lastVolSent = pct;
        _volThrottle = millis();
      }
      return;
    }

    case TouchPhase::Move: {
      /* Dragging the volume slider. Throttled, and only on a real change,
       * so a shaky finger cannot fire a hundred calls a second at the
       * Spotify rate limiter. */
      if (_downTarget != UiTarget::Volume || !_state->supportsVolume) return;
      const int16_t pct = _display->volumeFromX(ev.x);
      const uint32_t now = millis();
      if (now - _volThrottle >= 250 && abs(pct - _lastVolSent) >= VOLUME_STEP_PERCENT) {
        _spotify->cmdVolume(pct);
        _lastVolSent = pct;
        _volThrottle = now;
      }
      return;
    }

    case TouchPhase::Up:
      _display->setButtonPressed(UiTarget::None);
      _downTarget = UiTarget::None;
      return;

    case TouchPhase::Tap:
      break;

    default:
      return;
  }

  _display->setButtonPressed(UiTarget::None);

  /* Fire only if the finger lifted on the control it started on. */
  const UiTarget target = _display->hitTest(ev.x, ev.y);
  const UiTarget acted  = (target == _downTarget) ? target : UiTarget::None;
  _downTarget = UiTarget::None;

  if (acted == UiTarget::DeviceSelect) {
    go(Screen::Devices);
    return;
  }

  /* Volume and seek need a device but not a loaded track; the rest need
   * something to act on. Saying so beats firing a request that 404s. */
  if (acted != UiTarget::None && !_state->hasDevice) {
    _status->setMessage("No active device - pick one above", MESSAGE_TIMEOUT_MS);
    return;
  }

  switch (acted) {
    case UiTarget::Prev:      LOG_I("ui: previous");  _spotify->cmdPrevious(); break;
    case UiTarget::Next:      LOG_I("ui: next");      _spotify->cmdNext();     break;
    case UiTarget::PlayPause: LOG_I("ui: playpause"); _spotify->cmdPlayPause();break;
    case UiTarget::Shuffle:
      LOG_I("ui: shuffle -> %s", _state->shuffle ? "off" : "on");
      _spotify->cmdShuffle(!_state->shuffle);
      break;
    case UiTarget::Repeat: {
      const RepeatMode next = repeatNext(_state->repeat);
      LOG_I("ui: repeat -> %s", repeatToApi(next));
      _spotify->cmdRepeat(next);
      break;
    }
    case UiTarget::Seek:
      if (!_state->durationMs) {
        _status->setMessage("Nothing to seek", MESSAGE_TIMEOUT_MS);
      } else {
        const uint32_t pos = _display->seekFromX(ev.x, _state->durationMs);
        LOG_I("ui: seek to %lums", (unsigned long)pos);
        _spotify->cmdSeek(pos);
      }
      break;
    case UiTarget::Volume:
      if (!_state->supportsVolume) {
        _status->setMessage("This device has no remote volume", MESSAGE_TIMEOUT_MS);
      } else {
        const int16_t pct = _display->volumeFromX(ev.x);
        if (pct != _lastVolSent) {
          _spotify->cmdVolume(pct);
          _lastVolSent = pct;
        }
      }
      break;
    default:
      break;
  }
}

/* =========================================================================
 *  Device picker
 * ====================================================================== */
void UiController::handleDevicesTouch(const TouchEvent &ev) {
  char id[ID_LEN] = {0};
  const DeviceScreen::Result r =
      _devices.handleTouch(ev, _spotify->devices(), id, sizeof(id));

  switch (r) {
    case DeviceScreen::Result::Back:
      go(Screen::NowPlaying);
      break;

    case DeviceScreen::Result::Refresh:
      _spotify->refreshDevicesSoon();
      break;

    case DeviceScreen::Result::Transfer: {
      const SpotifyDevice *d = _spotify->devices().byId(id);
      LOG_I("ui: transfer to %s (keep playing: %s)", d ? d->name : id,
            _devices.keepPlaying() ? "yes" : "no");
      _spotify->cmdTransfer(id, _devices.keepPlaying());
      char msg[96];
      snprintf(msg, sizeof(msg), "Moving playback to %s", d ? d->name : "device");
      _status->setMessage(msg, MESSAGE_TIMEOUT_MS);
      go(Screen::NowPlaying);
      break;
    }

    default:
      break;
  }
}

/* =========================================================================
 *  Wi-Fi setup
 * ====================================================================== */
void UiController::handleWifiListTouch(const TouchEvent &ev) {
  char ssid[33] = {0};
  bool secured  = true;
  const WifiListScreen::Result r =
      _wifiList.handleTouch(ev, *_net, ssid, sizeof(ssid), &secured);

  switch (r) {
    case WifiListScreen::Result::Back:
      go(_net->isUp() ? Screen::NowPlaying : Screen::Setup);
      break;

    case WifiListScreen::Result::Rescan:
      _net->startScan();
      _wifiList.markDirty();
      break;

    case WifiListScreen::Result::Pick:
      strlcpy(_pendingSsid, ssid, sizeof(_pendingSsid));
      _pendingSecured = secured;
      if (!secured) {
        /* An open network needs no password, so skip the keyboard. */
        LOG_I("wifi: joining open network \"%s\"", ssid);
        _net->setCredentials(ssid, "");
        _status->setMessage("Connecting...", MESSAGE_TIMEOUT_MS);
        go(Screen::Setup);
      } else {
        go(Screen::WifiPassword);
      }
      break;

    default:
      break;
  }
}

void UiController::handleWifiPasswordTouch(const TouchEvent &ev) {
  switch (_keyboard.handleTouch(ev)) {
    case KeyboardScreen::Result::Cancel:
      go(Screen::WifiList);
      break;

    case KeyboardScreen::Result::Done: {
      LOG_I("wifi: saving credentials for \"%s\"", _pendingSsid);
      _net->setCredentials(_pendingSsid, _keyboard.text());
      _status->setMessage("Connecting...", MESSAGE_TIMEOUT_MS);
      go(Screen::Setup);
      break;
    }

    default:
      break;
  }
}

void UiController::handleSignInTouch(const TouchEvent &ev) {
  switch (_signIn.handleTouch(ev)) {
    case SignInScreen::Result::Back:
      go(Screen::NowPlaying);
      break;
    case SignInScreen::Result::WifiSetup:
      go(Screen::WifiList);
      break;
    default:
      break;
  }
}

/* =========================================================================
 *  Dispatch
 * ====================================================================== */
void UiController::handleTouch(const TouchEvent &ev) {
  if (ev.phase == TouchPhase::None) return;

  switch (_screen) {
    case Screen::NowPlaying:   handleNowPlayingTouch(ev);   break;
    case Screen::Devices:      handleDevicesTouch(ev);      break;
    case Screen::WifiList:     handleWifiListTouch(ev);     break;
    case Screen::WifiPassword: handleWifiPasswordTouch(ev); break;
    case Screen::Setup:        handleSignInTouch(ev);       break;
    case Screen::Boot:
      /* A tap during boot jumps straight to Wi-Fi setup, which is the way
       * out if the stored network no longer exists. */
      if (ev.phase == TouchPhase::Tap) go(Screen::WifiList);
      break;
  }
}

void UiController::handleButton(ButtonAction action) {
  if (action == ButtonAction::None) return;

  /* Hardware keys always mean transport, whatever is on screen - they are
   * physical controls on the case, not part of the UI. */
  if (!_state->hasDevice) {
    _status->setMessage("No active device", MESSAGE_TIMEOUT_MS);
    return;
  }
  switch (action) {
    case ButtonAction::Previous:  _spotify->cmdPrevious();  break;
    case ButtonAction::Next:      _spotify->cmdNext();      break;
    case ButtonAction::PlayPause: _spotify->cmdPlayPause(); break;
    default: break;
  }
}

/* =========================================================================
 *  Rendering
 * ====================================================================== */
void UiController::renderBoot() {
  /* Leave the splash up until there is something better to show, then hand
   * the screen to whichever state the device is actually in. */
  if (!_net->hasCredentials()) {
    go(Screen::WifiList);
    return;
  }
  if (_net->isUp()) {
    go(_spotify->authorised() ? Screen::NowPlaying : Screen::Setup);
    return;
  }
  if (millis() > 25000) {
    /* Credentials exist but will not associate - the network may be gone. */
    go(Screen::WifiList);
    return;
  }
  if (millis() - _bootTick > 500) {
    _bootTick = millis();
    _display->showBootScreen(_net->state() == NetState::Connecting
                                 ? "connecting to Wi-Fi..."
                                 : "waiting for Wi-Fi...");
  }
}

void UiController::render() {
  switch (_screen) {
    case Screen::Boot:
      renderBoot();
      break;

    case Screen::NowPlaying:
      /* Lost authorisation while running - send the user somewhere useful
       * rather than showing an empty player forever. */
      if (!_spotify->authorised() && _net->isUp()) {
        go(Screen::Setup);
        break;
      }
      _display->update(*_state, *_status);
      break;

    case Screen::Devices:
      _devices.render(_spotify->devices(), _spotify->authorised());
      break;

    case Screen::WifiList:
      if (_net->consumeScanDone()) _wifiList.markDirty();
      _wifiList.render(*_net);
      break;

    case Screen::WifiPassword:
      _keyboard.render();
      break;

    case Screen::Setup:
      _signIn.render(_net->ip(), _spotify->authorised());
      /* Once signed in from the browser there is nothing left to do here,
       * but hold the confirmation up for a moment so the sign-in visibly
       * succeeded rather than the screen just vanishing. */
      if (_spotify->authorised() && _net->isUp()) {
        if (!_authSettledAt) _authSettledAt = millis();
        if (millis() - _authSettledAt > 2000) go(Screen::NowPlaying);
      } else {
        _authSettledAt = 0;
      }
      break;
  }
}
