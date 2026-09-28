/* =========================================================================
 *  UiController.h - screen routing and the translation of taps into
 *  Spotify commands.
 *
 *  main.cpp stays a scheduler: it services the network, the Spotify client
 *  and the artwork downloader, then hands one touch event to this class.
 *  Everything about "which screen is showing and what does a tap there
 *  mean" lives here, so adding a screen touches one file.
 * ====================================================================== */
#pragma once

#include <Arduino.h>

#include "AlbumArt.h"
#include "AppState.h"
#include "DeviceScreen.h"
#include "DisplayManager.h"
#include "InputManager.h"
#include "NetManager.h"
#include "SetupScreens.h"
#include "SpotifyClient.h"

class UiController {
 public:
  void begin(DisplayManager *display, SpotifyClient *spotify, NetManager *net,
             AlbumArt *art, PlaybackState *state, AppStatus *status);

  /* Feed one touch event and one hardware-button action, then repaint. */
  void handleTouch(const TouchEvent &ev);
  void handleButton(ButtonAction action);
  void render();

  Screen screen() const { return _screen; }
  void   go(Screen s);

  /* True while a screen other than Now Playing is up, so the caller can
   * hold off artwork downloads that would repaint an invisible box. */
  bool overlayActive() const { return _screen != Screen::NowPlaying; }

 private:
  void handleNowPlayingTouch(const TouchEvent &ev);
  void handleDevicesTouch(const TouchEvent &ev);
  void handleWifiListTouch(const TouchEvent &ev);
  void handleWifiPasswordTouch(const TouchEvent &ev);
  void handleSignInTouch(const TouchEvent &ev);
  void renderBoot();

  DisplayManager *_display = nullptr;
  SpotifyClient  *_spotify = nullptr;
  NetManager     *_net     = nullptr;
  AlbumArt       *_art     = nullptr;
  PlaybackState  *_state   = nullptr;
  AppStatus      *_status  = nullptr;

  DeviceScreen   _devices;
  WifiListScreen _wifiList;
  KeyboardScreen _keyboard;
  SignInScreen   _signIn;

  Screen _screen = Screen::Boot;

  /* Now Playing drag state: which control the finger went down on, so a
   * slide off it cancels rather than firing. */
  UiTarget _downTarget  = UiTarget::None;
  int16_t  _lastVolSent = -1;
  uint32_t _volThrottle = 0;

  char _pendingSsid[33] = {0};
  bool _pendingSecured  = true;
  uint32_t _bootTick    = 0;
  /* When the browser sign-in completed, so the Setup screen can show the
   * confirmation briefly before handing over to Now Playing. */
  uint32_t _authSettledAt = 0;
};
