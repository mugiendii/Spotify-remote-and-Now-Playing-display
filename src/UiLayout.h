/* =========================================================================
 *  UiLayout.h - every rectangle, colour and font used by the interface.
 *
 *  Kept apart from display_config.h because that file is force-included
 *  into C sources; this one is C++ and only DisplayManager needs it.
 *
 *  Designed for 480x320 landscape:
 *
 *    +--------------------------------------------------------------+ 0
 *    | [wifi] (o)  Living Room speaker                      vol 62% | status
 *    +--------------------------------------------------------------+ 28
 *    |  +------------+   Track title that scrolls if too long       |
 *    |  |            |   Artist name                                |
 *    |  |  album art |   Album name                                 |
 *    |  |  160 x 160 |   transient message line                     |
 *    |  +------------+                                              |
 *    |                                                              |
 *    |  1:07 =========================------------------------ 3:42 | progress
 *    |                                                              |
 *    |        [ |< ]     [  > /|| ]     [ >| ]      [-] vol [+]     | controls
 *    +--------------------------------------------------------------+ 320
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "config.h"          /* ART_BOX_PX, MARQUEE_*, ... */

/* =========================================================================
 *  PALETTE - dark, with a single green accent.
 *
 *  Deliberately NOT Spotify's brand green (#1DB954). This is an unofficial
 *  device; using a generic green keeps it clearly a hobby build rather than
 *  something passing itself off as Spotify hardware. Change ACCENT if you
 *  prefer - nothing else depends on the exact hue.
 * ====================================================================== */
namespace Theme {
  constexpr uint16_t BG        = 0x0861;   /* #0b0c0b near-black           */
  constexpr uint16_t PANEL     = 0x18E3;   /* #1c1c1c raised surface       */
  constexpr uint16_t ACCENT    = 0x2E8B;   /* #2ed15c green accent         */
  constexpr uint16_t ACCENT_DK = 0x1424;   /* dim accent, progress trough  */
  constexpr uint16_t TEXT      = 0xFFFF;   /* primary text                 */
  constexpr uint16_t TEXT_DIM  = 0xA534;   /* secondary text               */
  constexpr uint16_t TEXT_MUTE = 0x6B4D;   /* tertiary / disabled          */
  constexpr uint16_t TROUGH    = 0x3186;   /* progress + volume trough     */
  constexpr uint16_t WARN      = 0xFD20;   /* amber - transient problems   */
  constexpr uint16_t ERR       = 0xF9A6;   /* red - hard failures          */
  constexpr uint16_t BTN       = 0x2124;   /* button face                  */
  constexpr uint16_t BTN_DOWN  = 0x4A69;   /* button face while pressed    */
}

/* =========================================================================
 *  FONTS
 *
 *  GFX free fonts bundled with TFT_eSPI. FONT_SMALL is the built-in font 2,
 *  which is cheaper to render and legible at 16 px - used for the status
 *  bar and timecodes where text changes often.
 * ====================================================================== */
namespace Fonts {
  #define FONT_TITLE   &FreeSansBold12pt7b   /* ~24 px line                */
  #define FONT_BODY    &FreeSans9pt7b        /* ~18 px line                */
  #define FONT_SMALL_N 2                     /* built-in font 2, 16 px     */
}

/* =========================================================================
 *  GEOMETRY
 *
 *  All bands in the text column share one width and one height so a single
 *  reusable sprite can render any of them - see DisplayManager's marquee.
 *  That is a memory decision, not a cosmetic one: four separately sized
 *  sprites would cost ~55 KB of heap that TLS needs.
 * ====================================================================== */
namespace Layout {
  /* ---- status bar ---- */
  constexpr int16_t STATUS_H      = 28;
  constexpr int16_t STATUS_Y      = 0;
  constexpr int16_t WIFI_X        = 10;    /* signal bars                  */
  constexpr int16_t WIFI_Y        = 7;
  constexpr int16_t WIFI_W        = 20;
  constexpr int16_t WIFI_H        = 14;
  constexpr int16_t LINK_DOT_X    = 42;    /* Spotify link indicator       */
  constexpr int16_t LINK_DOT_Y    = 14;
  constexpr int16_t LINK_DOT_R    = 5;
  /* "Playing on: <device>" - tapping it opens the device picker, so it is
   * both the label and the affordance. */
  constexpr int16_t DEVICE_X      = 56;
  constexpr int16_t DEVICE_Y      = 5;
  constexpr int16_t DEVICE_W      = 316;
  constexpr int16_t VOL_TEXT_R    = 474;
  constexpr int16_t VOL_TEXT_Y    = 5;

  /* ---- album art ---- */
  constexpr int16_t ART_X         = 12;
  constexpr int16_t ART_Y         = 38;
  constexpr int16_t ART_W         = ART_BOX_PX;
  constexpr int16_t ART_H         = ART_BOX_PX;

  /* ---- text column: four bands of identical size ---- */
  constexpr int16_t TEXT_X        = 186;
  constexpr int16_t TEXT_W        = SCREEN_W - TEXT_X - 12;   /* 282       */
  constexpr int16_t BAND_H        = 28;
  constexpr int16_t BAND_TITLE_Y  = 44;
  constexpr int16_t BAND_ARTIST_Y = 76;
  constexpr int16_t BAND_ALBUM_Y  = 104;
  constexpr int16_t BAND_MSG_Y    = 136;

  /* ---- progress + seek ---- */
  constexpr int16_t PROG_X        = 12;
  constexpr int16_t PROG_W        = SCREEN_W - 24;            /* 456       */
  constexpr int16_t PROG_Y        = 212;
  constexpr int16_t PROG_H        = 6;
  /* The visible bar is 6 px tall, which no fingertip can hit on a resistive
   * panel. The seek target is a much taller invisible band around it. */
  constexpr int16_t SEEK_TOUCH_Y  = 200;
  constexpr int16_t SEEK_TOUCH_H  = 28;
  constexpr int16_t TIME_Y        = 228;

  /* ---- transport row ----
   *  shuffle | prev | play/pause | next | repeat |      volume
   *  Shuffle and repeat are narrower than the transport keys: they are
   *  toggles you set occasionally, not controls you jab at. */
  constexpr int16_t BTN_Y         = 252;
  constexpr int16_t BTN_H         = 54;
  constexpr int16_t SMALL_W       = 44;
  constexpr int16_t BTN_W         = 60;
  constexpr int16_t PLAY_W        = 68;

  constexpr int16_t BTN_SHUF_X    = 10;
  constexpr int16_t BTN_PREV_X    = 62;
  constexpr int16_t BTN_PLAY_X    = 130;
  constexpr int16_t BTN_NEXT_X    = 206;
  constexpr int16_t BTN_REPT_X    = 274;

  /* ---- volume slider ---- */
  constexpr int16_t VOL_X         = 330;
  constexpr int16_t VOL_W         = 138;
  constexpr int16_t VOL_Y         = 276;
  constexpr int16_t VOL_H         = 8;
  constexpr int16_t VOL_KNOB_R    = 7;
  constexpr int16_t VOL_ICON_X    = VOL_X;
  constexpr int16_t VOL_ICON_Y    = 272;
  constexpr int16_t VOL_TRACK_X   = VOL_X + 18;
  constexpr int16_t VOL_TRACK_W   = VOL_W - 18;
  constexpr int16_t VOL_TOUCH_Y   = 256;
  constexpr int16_t VOL_TOUCH_H   = 46;

  /* ---- "no active device" notice, drawn across the content area ---- */
  constexpr int16_t NOTICE_X      = 12;
  constexpr int16_t NOTICE_Y      = 60;
  constexpr int16_t NOTICE_W      = SCREEN_W - 24;
  constexpr int16_t NOTICE_H      = 120;

  static_assert(BTN_SHUF_X + SMALL_W < BTN_PREV_X, "shuffle overlaps previous");
  static_assert(BTN_PREV_X + BTN_W   < BTN_PLAY_X, "previous overlaps play");
  static_assert(BTN_PLAY_X + PLAY_W  < BTN_NEXT_X, "play overlaps next");
  static_assert(BTN_NEXT_X + BTN_W   < BTN_REPT_X, "next overlaps repeat");
  static_assert(BTN_REPT_X + SMALL_W < VOL_X,      "repeat overlaps the volume slider");
  static_assert(VOL_TRACK_X + VOL_TRACK_W <= SCREEN_W, "volume slider runs off-screen");
  static_assert(TEXT_X > ART_X + ART_W,     "text column overlaps the album art");
  static_assert(BAND_MSG_Y + BAND_H < SEEK_TOUCH_Y, "message band overlaps the seek strip");
  static_assert(SEEK_TOUCH_Y + SEEK_TOUCH_H <= TIME_Y, "seek strip overlaps the timecodes");
  static_assert(BTN_Y + BTN_H <= SCREEN_H,  "transport buttons fall off the bottom");
  static_assert(ART_Y + ART_H < SEEK_TOUCH_Y, "album art overlaps the seek strip");
}

/* =========================================================================
 *  LIST SCREENS (device picker, Wi-Fi networks)
 *
 *  One geometry for both: the two screens differ only in what a row says,
 *  so sharing the layout keeps them visually identical and halves the
 *  hit-testing code.
 * ====================================================================== */
namespace ListLayout {
  constexpr int16_t HEADER_H   = 30;
  constexpr int16_t BACK_X     = 6;
  constexpr int16_t BACK_W     = 74;
  constexpr int16_t ACTION_W   = 84;                      /* Refresh/Rescan */
  constexpr int16_t ACTION_X   = SCREEN_W - ACTION_W - 6;

  /* Optional second header row, used by the device screen for the
   * "keep playing" toggle. */
  constexpr int16_t OPT_Y      = 34;
  constexpr int16_t OPT_H      = 28;

  constexpr int16_t LIST_Y     = 66;
  constexpr int16_t ROW_H      = 46;
  constexpr int16_t ROW_GAP    = 2;
  constexpr int16_t ROW_X      = 8;
  constexpr int16_t ROW_W      = 432;
  constexpr uint8_t VISIBLE    = 5;

  /* Scroll column, drawn only when the list is longer than VISIBLE. */
  constexpr int16_t SCROLL_X   = 444;
  constexpr int16_t SCROLL_W   = 30;
  constexpr int16_t SCROLL_UP_Y   = LIST_Y;
  constexpr int16_t SCROLL_DN_Y   = LIST_Y + 4 * (ROW_H + ROW_GAP);
  constexpr int16_t SCROLL_BTN_H  = ROW_H;

  constexpr int16_t rowY(uint8_t i) { return LIST_Y + i * (ROW_H + ROW_GAP); }

  static_assert(ROW_X + ROW_W <= SCROLL_X, "list rows overlap the scroll column");
  static_assert(SCROLL_X + SCROLL_W <= SCREEN_W, "scroll column runs off-screen");
  static_assert(LIST_Y + VISIBLE * (ROW_H + ROW_GAP) <= SCREEN_H,
                "list rows fall off the bottom");
  static_assert(OPT_Y + OPT_H <= LIST_Y, "options row overlaps the list");
}

/* =========================================================================
 *  ON-SCREEN KEYBOARD
 *
 *  Four rows, sized so the whole thing fits under a password field without
 *  scrolling. Key faces are defined in SetupScreens.cpp; only the grid
 *  lives here.
 * ====================================================================== */
namespace KeyLayout {
  constexpr int16_t KEY_W    = 43;
  constexpr int16_t KEY_H    = 46;
  constexpr int16_t GAP      = 4;
  constexpr int16_t PITCH_X  = KEY_W + GAP;
  constexpr int16_t PITCH_Y  = KEY_H + GAP;
  constexpr int16_t ROW0_Y   = 108;
  constexpr int16_t WIDE_W   = 64;     /* shift, backspace, ?123           */
  constexpr int16_t DONE_W   = 118;
  constexpr int16_t SPACE_W  = 232;

  constexpr int16_t TITLE_Y  = 36;
  constexpr int16_t FIELD_X  = 12;
  constexpr int16_t FIELD_Y  = 58;
  constexpr int16_t FIELD_W  = SCREEN_W - 24;
  constexpr int16_t FIELD_H  = 40;

  constexpr int16_t rowY(uint8_t r) { return ROW0_Y + r * PITCH_Y; }

  /* How many character keys each row can hold. Row 2 loses two wide caps
   * (shift and backspace) plus their gaps. The key tables in
   * SetupScreens.cpp static_assert against these, because a row that is
   * one character too long does not wrap or clip - it centres to a
   * negative x and silently runs off the left edge. */
  constexpr int16_t MAX_ROW_KEYS  = (SCREEN_W + GAP) / PITCH_X;
  constexpr int16_t MAX_ROW2_KEYS =
      (SCREEN_W - 2 * WIDE_W - 3 * GAP + GAP) / PITCH_X;

  /* Ten keys plus their gaps must fit the panel. */
  static_assert(10 * KEY_W + 9 * GAP <= SCREEN_W, "keyboard row 0 is wider than the screen");
  static_assert(ROW0_Y + 4 * PITCH_Y <= SCREEN_H, "keyboard rows fall off the bottom");
  static_assert(FIELD_Y + FIELD_H < ROW0_Y, "password field overlaps the keys");
}
