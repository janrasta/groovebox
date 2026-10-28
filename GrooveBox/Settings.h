// What survives a power cycle besides the patterns: tempo, PLAY mode options, note repeat.
#pragma once
#include <EEPROM.h>

enum { CURVE_LINEAR = 0, CURVE_SOFT, CURVE_HARD, CURVE_FULL };

// MIDI clock: OUT the pad is the master (sends clock, start, stop), IN it follows the incoming
// clock and sends none, OFF its own tempo, sending and following none. (Was a bool "follow":
// false = OUT, true = IN, so saved settings keep their meaning.)
enum { CLOCK_OUT = 0, CLOCK_IN, CLOCK_OFF, CLOCK_MODES };

struct Settings {
  uint32_t magic;
  uint16_t bpm;           // 30..300
  uint8_t clock;          // CLOCK_OUT / CLOCK_IN / CLOCK_OFF
  uint8_t slot;           // the pattern loaded at power up
  uint8_t curve;          // pad velocity curve
  int8_t transpose;       // PLAY mode pads, semitones -12..12
  int8_t octave;          // PLAY mode pads, -3..3
  uint8_t rate;           // note repeat 0..7 = 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T
  uint8_t swing;          // note repeat swing 0..4
  bool latch;
  bool aftertouch;
  bool bluetooth;
};

static const uint32_t SETTINGS_MAGIC = 0x47420001;   // "GB" v1

static Settings settings;
static uint32_t settingsChangedAt;

static void settingsDefaults() {
  settings = Settings{SETTINGS_MAGIC, 120, CLOCK_OUT, 0, CURVE_LINEAR, 0, 0, 4, 0, false, true, false};
}

static void settingsLoad() {
  EEPROM.get(0, settings);
  if (settings.magic != SETTINGS_MAGIC) settingsDefaults();
  if (settings.clock >= CLOCK_MODES) settings.clock = CLOCK_OUT;
}

static void settingsChanged() { settingsChangedAt = millis() | 1; }

static void settingsSaveWhenIdle() {
  if (settingsChangedAt && millis() - settingsChangedAt > 2000) {
    EEPROM.put(0, settings);
    EEPROM.commit();
    settingsChangedAt = 0;
  }
}
