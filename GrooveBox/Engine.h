// The sequencer: a 96 PPQN clock (internal, or locked to an incoming 24 PPQN MIDI clock), the
// steps of every track scheduled one step ahead into an event queue (micro-timing, swing,
// ratchets, conditions, p-locks), pattern switching at the end of the pattern, chains.
#pragma once
#include "Model.h"
#include "Midi.h"
#include "Store.h"

static const uint32_t PPQN = 96;
static const uint32_t TICKS_PER_STEP = 24;          // a 1/16 at normal speed

static Pattern pattern;                             // playing (and edited)
static Pattern queued;                              // loaded, waiting for the end of pattern
static int8_t currentSlot = 0, queuedSlot = -1;
static int8_t chain[PATTERNS], chainLength, chainPos;
static bool patternDirty;

static bool playing;
static uint32_t tick;                               // since start
static uint32_t patternStartTick;
static uint32_t nextTickUs;
static uint16_t bpm = 120;
static bool fillHeld;

// external clock
static bool syncExternal;                           // follow 0xF8 instead of the own tempo
static bool clockOut = true;                        // send clock, start, stop (master)
static uint32_t extClocks, extLastUs, extIntervalUs = 20833;
static bool extAlive() { return syncExternal && micros() - extLastUs < 500000; }

// ---------------------------------------------------------------- the event queue

enum { EV_FREE = 0, EV_NOTE_ON, EV_NOTE_OFF, EV_CC };
struct Event { uint32_t at; uint8_t type, status, d1, d2, track; };
static void onTrackHit(int t);                      // the UI: flash the pad
static const int QUEUE = 96;
static Event events[QUEUE];

static void schedule(uint32_t at, uint8_t type, uint8_t status, uint8_t d1, uint8_t d2,
                     uint8_t track = 0xFF) {
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type == EV_FREE) {
      events[i] = Event{at, type, status, d1, d2, track};
      return;
    }
  }
  if (type == EV_NOTE_OFF) midiSend(status, d1, d2);   // full: never leave a note hanging
}

static void runEvents() {
  // note offs first, so a ratchet's next hit is not cut by its own previous note off
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < QUEUE; i++) {
      Event &e = events[i];
      if (e.type == EV_FREE || (int32_t)(tick - e.at) < 0) continue;
      if ((pass == 0) != (e.type == EV_NOTE_OFF)) continue;
      midiSend(e.status, e.d1, e.d2);
      if (e.type == EV_NOTE_ON && e.track < TRACKS) onTrackHit(e.track);
      e.type = EV_FREE;
    }
  }
}

static void flushEvents() {                         // stop: offs go out now, the rest is dropped
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type == EV_NOTE_OFF || events[i].type == EV_CC) midiSend(events[i].status, events[i].d1, events[i].d2);
    events[i].type = EV_FREE;
  }
}

// ---------------------------------------------------------------- FX (the FX page, while a pad is held)
// SHIFT + PLAY: the 16 pads are 16 effects, on while held; knobs 1-4 edit the held FX's
// parameters. They bend which step plays, how fast and how hard; the MIDI clock out is
// untouched, so a synced DAW keeps time. The last three are CC macros (see the UI).

enum {
  FX_STUT_4, FX_STUT_8, FX_STUT_16, FX_REVERSE,     // pad n (from the bottom-left) is FX n
  FX_HALF, FX_DOUBLE, FX_ROLL, FX_GATE,
  FX_ACCENT, FX_DUCK, FX_SCATTER, FX_GLITCH,
  FX_DROP, FX_MACRO_A, FX_MACRO_B, FX_MACRO_C,
  FX_COUNT
};

struct FxDef {
  uint8_t n;                                        // parameters (knobs 1..n)
  uint8_t max[4];                                   // each 0..max
  uint8_t def[4];
  uint32_t color;
};

static const FxDef fxDef[FX_COUNT] = {
  {1, {10, 0, 0, 0},   {0, 0, 0, 0},     0xFF3000},   // stutter 1/4:  decay per repeat (x5 %)
  {1, {10, 0, 0, 0},   {0, 0, 0, 0},     0xFF7000},   // stutter 1/8
  {1, {10, 0, 0, 0},   {0, 0, 0, 0},     0xFFB000},   // stutter 1/16
  {0, {0, 0, 0, 0},    {0, 0, 0, 0},     0x2060FF},   // reverse
  {0, {0, 0, 0, 0},    {0, 0, 0, 0},     0x8000FF},   // half speed
  {0, {0, 0, 0, 0},    {0, 0, 0, 0},     0x00FF80},   // double speed
  {1, {2, 0, 0, 0},    {1, 0, 0, 0},     0xFFE000},   // roll: every hit ratchets x2 / x3 / x4
  {1, {9, 0, 0, 0},    {2, 0, 0, 0},     0xFF0080},   // gate: note length 10 + 10 n %
  {1, {63, 0, 0, 0},   {30, 0, 0, 0},    0xE0E0E0},   // accent: velocity + n
  {1, {95, 0, 0, 0},   {45, 0, 0, 0},    0x4060A0},   // duck: velocity - n
  {2, {20, 20, 0, 0},  {8, 2, 0, 0},     0xFF00FF},   // scatter: drop (x5 %), add (x5 %)
  {1, {2, 0, 0, 0},    {0, 0, 0, 0},     0x00FFFF},   // glitch: jump every 1/4, 1/8, 1/16
  {1, {4, 0, 0, 0},    {0, 0, 0, 0},     0xFF0000},   // drop: mute tracks 1-4, 5-8, 9-12, 13-16, all but selected
  {4, {127, 127, 127, 1}, {74, 20, 127, 1}, 0xA0FF00},  // macro: CC, value held, value at rest, pressure depth
  {4, {127, 127, 127, 1}, {91, 110, 0, 1},  0x00FFA0},
  {4, {127, 127, 127, 1}, {12, 100, 0, 1},  0x00A0FF},
};

static uint8_t fxParam[FX_COUNT][4];
static uint16_t fxMask;                             // bit per FX held
static uint8_t fxSlice, fxSlicePad;                 // stutter loop in 1/16 steps (4, 2, 1), 0 = off
static uint32_t fxFrom;                             // pattern tick where the stutter loop starts
static uint16_t fxSeed;                             // glitch: a new jump pattern per press
static int8_t fxSolo;                               // drop: the track that stays

static void fxParamDefaults() {
  for (int i = 0; i < FX_COUNT; i++) for (int j = 0; j < 4; j++) fxParam[i][j] = fxDef[i].def[j];
}

static bool fxOn(int fx) { return fxMask & (1 << fx); }

static void fxRealign() { fxFrom = 0; }             // transport / pattern start: ticks count from 0 again

static void fxSet(int fx, bool on) {
  if (fx < 0 || fx >= FX_COUNT) return;
  if (on && !fxOn(fx) && fx == FX_GLITCH) fxSeed = random(65536);
  if (on) fxMask |= 1 << fx; else fxMask &= ~(1 << fx);
  static const uint8_t slices[3] = {4, 2, 1};
  uint8_t slice = 0, pad = 0;
  for (int i = 0; i < 3; i++) if (fxMask & (1 << i)) { slice = slices[i]; pad = i; }   // the shortest held wins
  fxSlicePad = pad;
  if (slice != fxSlice) {
    fxSlice = slice;
    uint32_t rel = tick - patternStartTick, len = slice * TICKS_PER_STEP;
    fxFrom = slice ? rel - rel % len : 0;           // the slice playing now
  }
}

static void fxReleaseAll() { for (int i = 0; i < FX_COUNT; i++) fxSet(i, false); }

// ---------------------------------------------------------------- steps

static uint32_t trackTicksPerStep(const Track &k) {
  uint32_t tps = k.speed == SPEED_HALF ? 48 : k.speed == SPEED_DOUBLE ? 12 : 24;
  if (fxOn(FX_HALF) && !fxOn(FX_DOUBLE)) tps *= 2;
  else if (fxOn(FX_DOUBLE) && !fxOn(FX_HALF)) tps /= 2;
  return tps;
}

// the step of a track that plays at stepCount, with the stutter, glitch and reverse FX applied
static int fxIndex(const Track &k, uint32_t stepCount, uint32_t tps) {
  uint32_t s = stepCount;
  if (fxSlice) {
    uint32_t base = fxFrom / tps, loop = max(fxSlice * TICKS_PER_STEP / tps, 1u);
    if (s >= base) s = base + (s - base) % loop;
  }
  if (fxOn(FX_GLITCH)) {                            // every n steps, jump to a pseudo-random step
    static const uint8_t every[3] = {4, 2, 1};
    uint32_t n = every[min(fxParam[FX_GLITCH][0], (uint8_t)2)];
    uint32_t h = (s / n + fxSeed) * 2654435761u;
    s = (h >> 8) % k.length + s % n;
  }
  int i = s % k.length;
  return fxOn(FX_REVERSE) ? k.length - 1 - i : i;
}

// stutter decay: how much softer this repeat is, in %
static int fxDecayPct(uint32_t stepCount, uint32_t tps) {
  if (!fxSlice) return 0;
  uint32_t base = fxFrom / tps, loop = max(fxSlice * TICKS_PER_STEP / tps, 1u);
  if (stepCount < base) return 0;
  uint32_t pass = (stepCount - base) / loop;
  uint32_t repeat = pass > 1 ? pass - 1 : 0;
  return min((int)(repeat * fxParam[fxSlicePad][0] * 5), 95);
}

static bool fxDropped(int t) {
  if (!fxOn(FX_DROP)) return false;
  uint8_t g = fxParam[FX_DROP][0];
  return g < 4 ? (t / 4 == g) : (t != fxSolo);          // a group of four, or everything but the selected track
}

static bool fxChance(uint8_t fiveP) { return (uint32_t)random(100) < (uint32_t)fiveP * 5; }

static bool conditionPasses(uint8_t cond, uint32_t loop) {
  if (cond == COND_ALWAYS) return true;
  if (cond <= COND_PROB_MAX) return (uint32_t)random(100) < cond;
  switch (cond) {
    case COND_1_2: return loop % 2 == 0;
    case COND_2_2: return loop % 2 == 1;
    case COND_1_3: return loop % 3 == 0;
    case COND_2_3: return loop % 3 == 1;
    case COND_3_3: return loop % 3 == 2;
    case COND_1_4: return loop % 4 == 0;
    case COND_2_4: return loop % 4 == 1;
    case COND_3_4: return loop % 4 == 2;
    case COND_4_4: return loop % 4 == 3;
    case COND_FILL: return fillHeld;
    case COND_NOT_FILL: return !fillHeld;
  }
  return true;
}

static int8_t skipStep[TRACKS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static uint8_t lastPlayedStep[TRACKS];              // for the playhead

static bool trackHasSteps(int t) {
  const Track &k = pattern.track[t];
  for (int i = 0; i < k.length; i++) if (pattern.step[t][i].flags & STEP_ON) return true;
  return false;
}

// the step that starts at stepCount (counted from the pattern start), scheduled from `start`
static void scheduleStep(int t, uint32_t stepCount, uint32_t start) {
  Track &k = pattern.track[t];
  uint32_t tps = trackTicksPerStep(k);
  int index = fxIndex(k, stepCount, tps);
  Step s = pattern.step[t][index];                  // a copy: the FX bend it
  if (skipStep[t] == (int)(stepCount % k.length)) { // just recorded live: already heard
    skipStep[t] = -1;
    return;
  }
  if (k.muted || fxDropped(t)) return;
  if (fxOn(FX_SCATTER)) {
    if ((s.flags & STEP_ON) && fxChance(fxParam[FX_SCATTER][0])) return;       // dropped
    if (!(s.flags & STEP_ON) && fxChance(fxParam[FX_SCATTER][1]) && trackHasSteps(t)) {
      stepClear(s);                                 // an added hit on a track that plays
      s.flags = STEP_ON;
      s.velocity = k.velocity;
    }
  }
  if (!(s.flags & STEP_ON)) return;
  if (!conditionPasses(s.cond, stepCount / k.length)) return;
  int32_t micro = fxOn(FX_REVERSE) ? -s.micro : s.micro;   // backwards: early becomes late
  int32_t at = (int32_t)start + micro * (int32_t)tps / 24;
  if (stepCount & 1) at += tps * pattern.swing * 8 / 100;
  if (at < (int32_t)tick) at = tick;
  uint8_t ch = k.channel;
  uint8_t ratchet = s.ratchet;
  if (fxOn(FX_ROLL)) ratchet = max(ratchet, (uint8_t)(2 + fxParam[FX_ROLL][0]));
  int vel = s.velocity;
  if (fxOn(FX_ACCENT)) vel += fxParam[FX_ACCENT][0];
  if (fxOn(FX_DUCK)) vel -= fxParam[FX_DUCK][0];
  vel = vel * (100 - fxDecayPct(stepCount, tps)) / 100;
  vel = constrain(vel, 1, 127);
  uint32_t gatePct = fxOn(FX_GATE) ? 10 + 10 * fxParam[FX_GATE][0] : min(k.gate, (uint8_t)100);
  uint32_t hitTicks = max(tps / ratchet, 1u);
  uint32_t gate = max(hitTicks * gatePct / 100, 1u);
  for (int i = 0; i < LOCKS; i++) {
    if (s.lock[i] != NO_LOCK) {
      schedule(at, EV_CC, 0xB0 | ch, k.lockCC[i], s.lock[i]);
      schedule(at + tps - 1, EV_CC, 0xB0 | ch, k.lockCC[i], k.lockBase[i]);
    }
  }
  for (uint32_t r = 0; r < ratchet; r++) {
    uint32_t hit = at + r * hitTicks;
    schedule(hit, EV_NOTE_ON, 0x90 | ch, k.note, vel, t);
    schedule(hit + gate, EV_NOTE_OFF, 0x80 | ch, k.note, 0);
  }
}

// ---------------------------------------------------------------- patterns

static void patternSave() {
  if (patternDirty && storeSave(currentSlot, pattern)) patternDirty = false;
}

static void queuePattern(int slot) {
  if (slot == currentSlot && queuedSlot < 0) return;
  storeLoad(slot, queued);
  queuedSlot = slot;
  if (!playing) {                                   // stopped: switch now
    patternSave();
    pattern = queued;
    currentSlot = slot;
    queuedSlot = -1;
  }
}

static void setChain(const int8_t *slots, int n) {
  memcpy(chain, slots, n);
  chainLength = n;
  chainPos = 0;
  if (n) queuePattern(chain[0]);
}

// a pattern switch: what the old pattern scheduled ahead does not play
static void dropFutureEvents() {
  for (int i = 0; i < QUEUE; i++) {
    if (events[i].type != EV_NOTE_OFF && (int32_t)(events[i].at - tick) >= 0) events[i].type = EV_FREE;
  }
}

// at the end of the pattern: take the queued one (or the next of the chain); true if switched
static bool patternBoundary() {
  if (queuedSlot < 0 && chainLength > 1) {
    chainPos = (chainPos + 1) % chainLength;
    if (chain[chainPos] != currentSlot) {
      storeLoad(chain[chainPos], queued);
      queuedSlot = chain[chainPos];
    }
  }
  if (queuedSlot >= 0) {
    patternSave();                                  // a few ms of flash: at the bar line
    pattern = queued;
    currentSlot = queuedSlot;
    queuedSlot = -1;
    patternStartTick = tick;
    dropFutureEvents();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the clock

static void noteRepeatClock();                     // the UI's note repeat, 24 PPQN

static void onTick() {
  uint32_t rel = tick - patternStartTick;
  if (rel && rel % (TICKS_PER_STEP * pattern.length) == 0 && patternBoundary()) { rel = 0; fxRealign(); }
  if (tick % 4 == 0) {
    if (clockOut) midiSend(0xF8);                   // clock out, 24 PPQN
    noteRepeatClock();
  }
  for (int t = 0; t < TRACKS; t++) {
    uint32_t tps = trackTicksPerStep(pattern.track[t]);
    if (rel % tps) continue;
    uint32_t stepCount = rel / tps;
    lastPlayedStep[t] = fxIndex(pattern.track[t], stepCount, tps);
    // one step ahead (micro-timing can be early); at the pattern start, step 0 now too
    if (rel == 0) scheduleStep(t, 0, tick);
    scheduleStep(t, stepCount + 1, tick + tps);
  }
  runEvents();
  tick++;
}

static void transportStart() {
  if (playing) return;
  playing = true;
  tick = patternStartTick = 0;
  fxRealign();
  nextTickUs = micros();
  if (clockOut) midiSend(0xFA);
}

static void transportStop() {
  if (!playing) return;
  playing = false;
  flushEvents();
  if (clockOut) midiSend(0xFC);
  for (int t = 0; t < TRACKS; t++) skipStep[t] = -1;
  patternSave();
}

static uint32_t tickIntervalUs() {
  return extAlive() ? extIntervalUs / 4 : 625000UL / bpm;
}

static void engineRun() {
  uint32_t now = micros();
  if (!playing) {
    // master clock keeps running while stopped, so a synced DAW knows the tempo
    static uint32_t nextClockUs;
    if (clockOut && (int32_t)(now - nextClockUs) >= 0) {
      uint32_t period = 4 * (625000UL / bpm);
      midiSend(0xF8);
      // from when it was due, not from now: the loop's delay must not slow the tempo down
      nextClockUs += period;
      if ((int32_t)(now - nextClockUs) >= 0) nextClockUs = now + period;   // after a stall
    }
    return;
  }
  if (extAlive()) {
    // interpolate between incoming clocks, never more than 3 ticks past the last one
    uint32_t limit = extClocks * 4 + 3;
    while ((int32_t)(now - nextTickUs) >= 0 && tick < limit) {
      onTick();
      nextTickUs += extIntervalUs / 4;
    }
    return;
  }
  if ((int32_t)(now - nextTickUs) > 200000) nextTickUs = now;    // after a long stall
  while ((int32_t)(now - nextTickUs) >= 0) {
    onTick();
    nextTickUs += tickIntervalUs();
  }
}

// incoming realtime messages (from midiIn)
static void engineClockIn() {
  if (!syncExternal) return;
  uint32_t now = micros(), dt = now - extLastUs;
  if (dt < 200000) extIntervalUs = (extIntervalUs * 3 + dt) / 4;
  extLastUs = now;
  if (!playing) return;
  extClocks++;
  while (tick < extClocks * 4) onTick();            // catch up to the clock
  nextTickUs = now + extIntervalUs / 4;
}

static void engineStartIn(bool fromZero) {
  if (!syncExternal) return;
  if (fromZero) {
    flushEvents();
    tick = patternStartTick = 0;
    fxRealign();
  }
  extClocks = tick / 4;                             // continue: carry on from where we are
  playing = true;
  nextTickUs = micros();
}

static void engineStopIn() {
  if (!syncExternal) return;
  transportStop();
}

// ---------------------------------------------------------------- live recording

// a pad played while recording: the nearest step of its track, micro-timing kept
static void recordHit(int t, uint8_t velocity) {
  Track &k = pattern.track[t];
  uint32_t tps = trackTicksPerStep(k);
  uint32_t rel = tick - patternStartTick;
  uint32_t stepCount = rel / tps, frac = rel % tps;
  int32_t micro = frac * 24 / tps;
  if (frac > tps / 2) {
    stepCount++;
    micro -= 24;
    skipStep[t] = stepCount % k.length;             // the sequencer would play it again
  }
  Step &s = pattern.step[t][stepCount % k.length];
  s.flags |= STEP_ON;
  s.velocity = velocity;
  s.micro = constrain(micro, -11, 11);
  patternDirty = true;
}

// the step a track is on now (playhead), or 0 when stopped
static int playheadStep(int t) {
  return playing ? lastPlayedStep[t] : 0;
}
