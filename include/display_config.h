/* =========================================================================
 *  display_config.h - EVERY display and touch pin, the panel driver, the
 *  panel geometry and the touch calibration. Nothing else in the firmware
 *  hard-codes a pin number.
 *
 *  HOW THIS FILE IS USED
 *  ---------------------
 *  platformio.ini force-includes it with `-include`, so it is the first
 *  thing every translation unit sees - our code AND the TFT_eSPI library
 *  sources. USER_SETUP_LOADED below tells TFT_eSPI to configure itself from
 *  these macros instead of its own User_Setup.h.
 *
 *  Consequence: this file is compiled as part of C sources too (TJpg_Decoder
 *  ships tjpgd.c). Keep it to preprocessor macros. Anything C++-only must go
 *  inside the `#ifdef __cplusplus` block at the bottom.
 *
 *  >>> EVERY PIN BELOW IS A DEFAULT TAKEN FROM A KNOWN-WORKING BUILD OF   <<<
 *  >>> THIS EXACT PANEL. THEY ARE CONFIGURABLE - CHANGE THEM TO MATCH     <<<
 *  >>> YOUR WIRING. NOTHING IS AUTO-DETECTED.                             <<<
 * ====================================================================== */
#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

/* Tell TFT_eSPI that its setup is supplied here, so it does not pull in
 * User_Setup_Select.h. Must be defined before any TFT_eSPI header. */
#define USER_SETUP_LOADED 1

/* =========================================================================
 *  1. PANEL DRIVER                                             CONFIGURABLE
 *
 *  Exactly one driver must be enabled. ILI9488 and ST7796 are pin- and
 *  resolution-compatible on the common 4.0" 480x320 modules, so switching
 *  between them is a one-line change.
 *
 *  Not sure which you have? Build with ILI9488 first. A blank/white screen
 *  or heavily inverted colours usually means you have the other one.
 * ====================================================================== */
#define ILI9488_DRIVER 1
/* #define ST7796_DRIVER  1 */
/* #define ILI9341_DRIVER 1 */   /* 240x320 panels - also change geometry below */

/* =========================================================================
 *  2. PANEL GEOMETRY                                           CONFIGURABLE
 *
 *  These are the NATIVE (portrait) dimensions of the glass. The firmware
 *  calls setRotation(TFT_ROTATION) to get landscape, so the UI works in
 *  TFT_HEIGHT x TFT_WIDTH = 480 x 320.
 *
 *  For a 240x320 ILI9341: TFT_WIDTH 240 / TFT_HEIGHT 320. The UI layout in
 *  src/UiLayout.h scales off SCREEN_W/SCREEN_H but was designed for 480x320;
 *  smaller panels will need the layout constants revisited.
 * ====================================================================== */
#define TFT_WIDTH   320          /* native width  (portrait)               */
#define TFT_HEIGHT  480          /* native height (portrait)               */

/* 0/2 = portrait, 1/3 = landscape. 1 gives 480x320 with the ribbon left.  */
#define TFT_ROTATION 1

/* Landscape dimensions the UI actually draws in. Derived - do not edit.   */
#define SCREEN_W (TFT_HEIGHT)
#define SCREEN_H (TFT_WIDTH)

/* =========================================================================
 *  3. SPI PINS (VSPI bus, shared by panel and touch)           CONFIGURABLE
 *
 *  The TFT and the XPT2046 share SCLK/MOSI/MISO and are separated only by
 *  their chip selects. See src/InputManager.cpp for how the two chip
 *  selects are interlocked so only one slave ever drives MISO.
 *
 *  Avoid GPIO 6-11 (SPI flash), 34-39 (input-only, no pull-ups) and
 *  0/2/12/15 (boot strapping) when reassigning.
 * ====================================================================== */
#define TFT_SCLK  18             /* TFT SCK   + touch T_CLK  (shared)      */
#define TFT_MISO  19             /* TFT SDO   + touch T_DO   (shared)      */
#define TFT_MOSI  23             /* TFT SDI   + touch T_DIN  (shared)      */
#define TFT_CS     5             /* TFT chip select                        */
#define TFT_DC    27             /* TFT data/command (a.k.a. RS)           */
#define TFT_RST   33             /* TFT reset. -1 if wired to ESP32 EN     */

/* =========================================================================
 *  4. BACKLIGHT                                                CONFIGURABLE
 *
 *  NOTE: TFT_BL is deliberately NOT defined for TFT_eSPI. The backlight is
 *  owned by DisplayManager, which drives it through the LEDC peripheral so
 *  brightness can be varied. Two owners would fight over the pin.
 *
 *  Set PIN_TFT_BL to -1 when the module's LED pin is hard-wired to 3V3
 *  (the case on most 4.0" boards, including the reference hardware).
 * ====================================================================== */
#define PIN_TFT_BL        -1     /* -1 = not controllable, tied to 3V3     */
#define TFT_BL_ACTIVE_HIGH 1     /* 0 if the backlight sinks through the pin */
#define TFT_BL_PWM_CHANNEL 0     /* LEDC channel; must not clash elsewhere */
#define TFT_BL_PWM_FREQ    5000
#define TFT_BL_PWM_BITS    8     /* 8-bit -> duty 0..255                   */

/* =========================================================================
 *  5. TOUCH CONTROLLER                                         CONFIGURABLE
 *
 *  Set TOUCH_ENABLED to 0 for a panel with no touch layer. The firmware
 *  then compiles out all XPT2046 code and exposes the same playback
 *  actions through InputManager's GPIO button hooks (section 6).
 *
 *  TFT_eSPI's own TOUCH_CS is intentionally NOT defined: the XPT2046 is
 *  driven by PaulStoffregen's library instead, and the two must not both
 *  try to own the touch chip select.
 * ====================================================================== */
#define TOUCH_ENABLED   1
#define PIN_TOUCH_CS   26        /* XPT2046 chip select                    */
#define PIN_TOUCH_IRQ  25        /* XPT2046 pen-down IRQ. -1 = polled mode */

/* ------------------------------------------------------------------------
 *  Touch calibration - raw 12-bit XPT2046 readings at the extremes of the
 *  glass, expressed in SCREEN axes. TOUCH_SWAP_XY is applied BEFORE these
 *  are used, so numbers reported by the on-screen calibration aid paste
 *  straight in.
 *
 *  These defaults came off the reference panel. Yours will differ. If the
 *  touch lands in the wrong place, fix the three flags FIRST, reflash, and
 *  only then adjust the min/max:
 *      moves vertically when you slide sideways -> flip TOUCH_SWAP_XY
 *      moves left when you slide right          -> flip TOUCH_INVERT_X
 *      moves up when you slide down             -> flip TOUCH_INVERT_Y
 *  Set TOUCH_DEBUG_RAW to 1 to print raw readings over Serial.
 * --------------------------------------------------------------------- */
#define TOUCH_MIN_X    247       /* raw value at screen x = 0              */
#define TOUCH_MAX_X   3891       /* raw value at screen x = SCREEN_W-1     */
#define TOUCH_MIN_Y    369       /* raw value at screen y = 0              */
#define TOUCH_MAX_Y   3764       /* raw value at screen y = SCREEN_H-1     */

#define TOUCH_SWAP_XY   0        /* 1 = raw X drives screen Y and vice versa */
#define TOUCH_INVERT_X  1        /* 1 = mirror horizontally                */
#define TOUCH_INVERT_Y  1        /* 1 = mirror vertically                  */
#define TOUCH_DEBUG_RAW 0        /* 1 = print raw x/y/z on every press     */

/* Touch feel. Raise DEBOUNCE if one press registers twice; raise
 * RELEASE_SAMPLES if a held press flickers as separate taps.             */
#define TOUCH_POLL_MS         15
#define TOUCH_DEBOUNCE_MS     120
#define TOUCH_RELEASE_SAMPLES  3

/* =========================================================================
 *  6. OPTIONAL PHYSICAL BUTTONS                                CONFIGURABLE
 *
 *  Independent of touch - wire these and you get hardware transport keys
 *  whether or not the panel has a touch layer. -1 disables a button.
 *  Buttons are wired to GND and use the internal pull-up (active LOW).
 * ====================================================================== */
#define PIN_BTN_PREV       -1
#define PIN_BTN_PLAYPAUSE  -1
#define PIN_BTN_NEXT       -1
#define BTN_ACTIVE_LOW      1
#define BTN_DEBOUNCE_MS    40

/* =========================================================================
 *  7. SPI CLOCKS                                               CONFIGURABLE
 *
 *  27 MHz is the safe value for an ILI9488 on DuPont jumpers. Short,
 *  soldered wiring usually takes 40 MHz; drop to 20 MHz if you see noise,
 *  tearing or wrong colours.
 * ====================================================================== */
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY   6000000
#define SPI_TOUCH_FREQUENCY  2500000

/* Mandatory on ESP32 so the HAL mutex is toggled around every transfer -
 * this is what serialises the panel against the XPT2046 on the shared bus. */
#define SUPPORT_TRANSACTIONS

/* Do NOT define USE_HSPI_PORT. On the classic ESP32, TFT_eSPI builds its own
 * SPIClass(VSPI) while the touch library uses the global `SPI` object: two
 * wrappers over one peripheral sharing one HAL mutex. Moving the panel to
 * HSPI would split them onto different buses and break the CS interlock. */

/* =========================================================================
 *  8. FONTS
 *
 *  The UI draws with the GFX free fonts (LOAD_GFXFF) plus font 2 for small
 *  labels. FONT6/7/8 are large digit-only faces this UI never uses; leaving
 *  them out saves roughly 40 KB of flash.
 * ====================================================================== */
#define LOAD_GLCD   1            /* 8x8 fallback font, ~2 KB               */
#define LOAD_FONT2  1            /* 16px proportional - small labels       */
#define LOAD_FONT4  1            /* 26px proportional - fallback headings  */
#define LOAD_GFXFF  1            /* Adafruit GFX free fonts                */
#define SMOOTH_FONT 1            /* anti-aliased font support              */

/* =========================================================================
 *  9. COMPILE-TIME SANITY CHECKS (C++ only)
 * ====================================================================== */
#ifdef __cplusplus
static_assert(SCREEN_W > SCREEN_H,
              "This UI is landscape-only. TFT_ROTATION must be 1 or 3 and "
              "TFT_HEIGHT must exceed TFT_WIDTH.");
static_assert(TFT_ROTATION == 1 || TFT_ROTATION == 3,
              "TFT_ROTATION must be 1 or 3 for landscape.");
#if TOUCH_ENABLED
static_assert(PIN_TOUCH_CS != TFT_CS,
              "Touch and TFT cannot share a chip select.");
static_assert(TOUCH_MAX_X > TOUCH_MIN_X && TOUCH_MAX_Y > TOUCH_MIN_Y,
              "Touch calibration maxima must exceed the minima.");
#endif
#endif /* __cplusplus */

#endif /* DISPLAY_CONFIG_H */
