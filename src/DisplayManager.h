/* =========================================================================
 *  DisplayManager.h - all rendering. Nothing else in the firmware touches
 *  the TFT.
 *
 *  It owns two things:
 *
 *   1. The Now Playing screen, which is diff-rendered (see below).
 *   2. A set of drawing primitives - headers, list rows, key caps, text
 *      fields - that the other screens compose. Those screens hold no
 *      TFT_eSPI reference of their own, so there is exactly one place in
 *      the firmware that can put a pixel on the glass.
 *
 *  UPDATE STRATEGY
 *  ---------------
 *  update() is called every loop iteration but redraws almost nothing.
 *  It diffs the incoming PlaybackState/AppStatus against a snapshot of what
 *  is currently on the glass and repaints only the fields that differ. On a
 *  steady playing track that comes to one progress-bar delta of a few
 *  pixels per second plus a timecode every second - a few hundred bytes of
 *  SPI traffic instead of the 460 KB a full 480x320 repaint would cost.
 *
 *  MARQUEE
 *  -------
 *  Text wider than its band ping-pongs left and right with a pause at each
 *  end. All four text bands are the same size on purpose, so one reusable
 *  16 KB sprite renders any of them flicker-free. Four dedicated sprites
 *  would cost ~55 KB of heap that mbedTLS needs during the handshake.
 *  If the sprite cannot be allocated the code falls back to clipped direct
 *  drawing via setViewport, which flickers slightly but always works.
 * ====================================================================== */
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "AppState.h"
#include "UiLayout.h"
#include "config.h"

/* Which Now Playing control a press landed on. */
enum class UiTarget : uint8_t {
  None,
  Prev,
  PlayPause,
  Next,
  Shuffle,
  Repeat,
  Volume,
  Seek,
  DeviceSelect   /* the "Playing on: ..." strip in the status bar */
};

class DisplayManager {
 public:
  void begin();

  /* ---- Now Playing ---------------------------------------------------- */
  void update(const PlaybackState &st, const AppStatus &status);
  void invalidateAll();

  UiTarget hitTest(int16_t x, int16_t y) const;
  int16_t  volumeFromX(int16_t x) const;
  uint32_t seekFromX(int16_t x, uint32_t durationMs) const;
  void     setButtonPressed(UiTarget t);

  /* ---- boot / notices ------------------------------------------------- */
  void showBootScreen(const char *line);
  void showFatal(const char *title, const char *detail);

  /* ---- artwork --------------------------------------------------------
   *  AlbumArt owns decoding; DisplayManager owns the pixels. It hands back
   *  the TFT so TJpg_Decoder's callback can push blocks straight into the
   *  art box without an intermediate buffer.                             */
  TFT_eSPI &tft() { return _tft; }
  void artDrawPlaceholder(bool loading);
  void artClearBox();
  void artFrame();
  void artMarkPainted();
  bool consumeArtInvalidated() {
    const bool v = _artInvalidated;
    _artInvalidated = false;
    return v;
  }

  /* ---- primitives shared with the other screens ------------------------
   *  Deliberately dumb: they draw exactly what they are told, immediately,
   *  with no dirty tracking. The list and keyboard screens change wholesale
   *  when they change at all, so diffing them would cost more than it
   *  saves.                                                              */
  void clearScreen(uint16_t colour = Theme::BG);
  /* Header bar with optional left (back) and right (action) buttons. */
  void drawScreenHeader(const char *title, const char *leftLabel,
                        const char *rightLabel);
  /* A two-line list row. `badge` is drawn as a dot on the right when it is
   * not Theme::BG; `dim` greys the text for unusable entries. */
  void drawListRow(int16_t y, const char *line1, const char *line2,
                   uint16_t badge, bool highlight, bool dim);
  void drawScrollArrows(bool canUp, bool canDown);
  void drawKeyCap(int16_t x, int16_t y, int16_t w, int16_t h,
                  const char *label, bool pressed, bool accent);
  void drawTextField(int16_t x, int16_t y, int16_t w, int16_t h,
                     const char *text, bool masked, const char *placeholder);
  void drawToggle(int16_t x, int16_t y, int16_t w, int16_t h,
                  const char *label, bool on);
  void drawCentredText(const char *text, int16_t y, const GFXfont *font,
                       uint16_t colour, uint16_t bg);
  void drawNotice(const char *line1, const char *line2, const char *line3);

  /* Truncate `src` with an ellipsis so it fits `maxW` in the current font. */
  static void fitEllipsis(TFT_eSPI &t, const char *src, char *dst,
                          size_t dstLen, int16_t maxW);

  void setBacklight(uint8_t duty);

 private:
  /* ---- one scrolling text band -------------------------------------- */
  struct Band {
    int16_t            y        = 0;
    const GFXfont     *font     = nullptr;   /* nullptr = built-in font 2 */
    uint16_t           colour   = Theme::TEXT;
    char               text[TITLE_LEN] = {0};
    int16_t            textW    = 0;
    int16_t            offset   = 0;
    int8_t             dir      = 1;
    uint32_t           nextStep = 0;
    bool               scrolls  = false;
    bool               dirty    = true;
  };

  enum BandId : uint8_t { B_TITLE, B_ARTIST, B_ALBUM, B_MESSAGE, B_COUNT };

  void setBandText(BandId id, const char *text, uint16_t colour);
  void renderBand(Band &b);
  void stepMarquees();
  int16_t measure(const GFXfont *font, const char *text);
  void applyFont(TFT_eSPI &target, const GFXfont *font);

  void drawStaticChrome();
  void drawStatusBar(const AppStatus &s, const PlaybackState &st, bool force);
  void drawWifiIcon(int8_t bars, NetState net);
  void drawLinkDot(ApiState api);
  void drawProgress(const PlaybackState &st, bool force);
  void drawTimes(uint32_t posMs, uint32_t durMs, bool force);
  void drawTransport(const PlaybackState &st, bool force);
  void drawButton(int16_t x, int16_t w, UiTarget which,
                  const PlaybackState &st, bool pressed, bool enabled);
  void drawVolume(const PlaybackState &st, bool force);

  static void formatTime(uint32_t ms, char *out, size_t len);

  TFT_eSPI    _tft;
  TFT_eSprite _band{&_tft};
  bool        _bandSpriteOk = false;

  Band _bands[B_COUNT];

  /* ---- snapshot of what is currently on the glass -------------------- */
  bool       _forceAll      = true;
  int8_t     _lastBars      = -1;
  NetState   _lastNet       = NetState::Down;
  ApiState   _lastApi       = ApiState::Init;
  char       _lastDevice[DEVICE_LEN] = {0};
  int16_t    _lastVolume    = -2;
  int16_t    _lastVolSlider = -2;
  bool       _lastVolCtl    = false;
  int16_t    _lastFillW     = -1;
  uint32_t   _lastPosSec    = UINT32_MAX;
  uint32_t   _lastDurSec    = UINT32_MAX;
  bool       _lastPlaying   = false;
  bool       _lastShuffle   = false;
  RepeatMode _lastRepeat    = RepeatMode::Off;
  bool       _lastHasDev    = false;
  bool       _lastNoDevice  = false;   /* content area shows the notice   */
  UiTarget   _pressed       = UiTarget::None;
  UiTarget   _lastPressed   = UiTarget::None;
  bool       _artPainted    = false;
  bool       _artInvalidated = false;
  /* Last wording drawn by drawNotice(). Members, not function statics:
   * invalidateAll() has to be able to clear them, or a repaint triggered
   * from another screen finds them unchanged and draws nothing. */
  char       _notice1[96]  = {0};
  char       _notice2[128] = {0};
  char       _notice3[MESSAGE_LEN] = {0};
};
