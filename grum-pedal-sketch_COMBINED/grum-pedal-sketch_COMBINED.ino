// Teensy 4.0 + Audio Shield (SGTL5000)
// DIAGNOSTIC BUILD: get sound + reliable triggers first.
// - Stereo->mono sum
// - Notch 60 + HPF (analyzers can tap pre-filter for more level)
// - Dual-Envelope onset  + Simple peak onset (ORed)  [DIAG]
// - YIN pitch mapping (wide-open thresholds)
// - DRY MONITOR ON (so you can hear guitar path)
// - Emergency + Flux DISABLED for now (flip defines later to re-enable)
// - Boot beep

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <CrashReport.h>

// ===================== DIAGNOSTIC SWITCHES =====================
#define DIAG_BYPASS_FILTER   0   // 1: analyzers read from pre-filter mixer (more level)
#define DIAG_DRY_MONITOR     0   // 1: hear your guitar; set 0 later to mute dry
#define DIAG_USE_SIMPLE_ONSET 0  // 1: OR a simple peak-based onset with dual-envelope
#define USE_EMERGENCY         1  // 0: OFF for now
#define USE_FLUX_CONFIRM      1  // 0: OFF for now

// ===================== CONFIG =====================
const uint16_t AUDIO_MEM        = 60;
const uint8_t  LINE_IN_LEVEL    = 5;     // start hot so we see life; lower if obviously clipping
const float    VOL_DEFAULT      = 0.90f;
const float    NOTCH_HZ         = 60.0f;
const float    HPF_HZ           = 60.0f;

// YIN pitch (loosened for debug)
const float    YIN_CONF         = 0.75f;

// Debounce / band / continuity
const uint32_t RETRIGGER_MS     = 90;
const float    HYST_PCT         = 0.06f;
const uint32_t LOCK_MS          = 150;

// Dual-envelope onset (primary)
const float    FAST_ALPHA       = 0.35f;
const float    SLOW_ALPHA       = 0.02f;
const float    RATIO_DELTA      = 0.05f;     // slightly looser
const float    MIN_DELTA        = 0.00005f;  // lower absolute gate
const uint32_t ONSET_MIN_GAP_MS = 90;

// Simple onset (diagnostic) — peak threshold (pre-filter scale)
const float    SIMPLE_PEAK_THR  = 0.02f;     // if too chatty, raise to 0.03–0.05

// RMS gate
const float    MIN_RMS_FOR_PITCH= 0.0002f;   // much lower so YIN can engage

// ===================== AUDIO GRAPH =====================
AudioInputI2S       in1;
AudioMixer4         inMix;
AudioFilterBiquad   biq;
AudioAnalyzeNoteFrequency yin;
AudioAnalyzeRMS     rms;
AudioAnalyzePeak    peak;
AudioAnalyzeFFT256  fft;     // present but unused while USE_FLUX_CONFIRM=0

AudioSynthSimpleDrum kick, snare, hat, ride, crash;
AudioMixer4         drumMix;
AudioMixer4         mainMix;
AudioSynthWaveform  beep;
AudioOutputI2S      out1;

AudioConnection patchInL(in1, 0, inMix, 0);
AudioConnection patchInR(in1, 1, inMix, 1);
AudioConnection patchMixB(inMix, 0, biq, 0);

#if DIAG_BYPASS_FILTER
AudioConnection patchYIN(inMix, 0, yin, 0);
AudioConnection patchRMS(inMix, 0, rms, 0);
AudioConnection patchPEAK(inMix, 0, peak, 0);
#else
AudioConnection patchYIN(biq, 0, yin, 0);
AudioConnection patchRMS(biq, 0, rms, 0);
AudioConnection patchPEAK(biq, 0, peak, 0);
#endif

AudioConnection patchKick (kick,  0, drumMix, 0);
AudioConnection patchSnare(snare, 0, drumMix, 1);
AudioConnection patchHat  (hat,   0, drumMix, 2);
AudioConnection patchRide (ride,  0, drumMix, 3);

AudioConnection patchDrums(drumMix, 0, mainMix, 1);
AudioConnection patchCrash(crash,   0, mainMix, 2);
AudioConnection patchDry  (inMix,   0, mainMix, 0);
AudioConnection patchBeep (beep,    0, mainMix, 0);

AudioConnection patchOutL(mainMix, 0, out1, 0);
AudioConnection patchOutR(mainMix, 0, out1, 1);

AudioControlSGTL5000 sgtl;

// ===================== STATE =====================
uint32_t lastTrig[5] = {0,0,0,0,0};
float    fastEnv = 0.0f, slowEnv = 0.0f;
uint32_t lastOnsetMs = 0;

int      lockedDrum  = -1;
uint32_t lockUntilMs = 0;

struct Band { float fmin, fmax; uint8_t idx; };
Band bands[] = {
  {  74.0f,  92.0f, 0 }, // E2 -> Kick
  { 100.0f, 122.0f, 1 }, // A2 -> Snare
  { 135.0f, 160.0f, 2 }, // D3 -> Hat
  { 185.0f, 210.0f, 2 }, // G3 -> Hat
  { 235.0f, 260.0f, 3 }, // B3 -> Ride
  { 315.0f, 350.0f, 4 }, // E4 -> Crash
};

static inline float clamp01(float x){ return x < 0 ? 0 : (x > 1 ? 1 : x); }
bool canRetrigger(uint8_t i){ uint32_t n=millis(); if(n-lastTrig[i]>=RETRIGGER_MS){ lastTrig[i]=n; return true;} return false; }
int bandFor(float f){ for(uint8_t i=0;i<sizeof(bands)/sizeof(bands[0]);++i){ float cmin=bands[i].fmin*(1.0f-HYST_PCT), cmax=bands[i].fmax*(1.0f+HYST_PCT); if(f>=cmin && f<=cmax) return i; } return -1; }

void trigger(uint8_t drumIdx, float vel, float freq){
  float v = 0.2f + 0.8f*clamp01(vel); // keep audible
  switch (drumIdx) {
    case 0: drumMix.gain(0, 0.9f * v); kick .noteOn(); Serial.printf("KICK  %.1fHz v=%.2f\n",  freq, vel); break;
    case 1: drumMix.gain(1, 0.9f * v); snare.noteOn(); Serial.printf("SNARE %.1fHz v=%.2f\n", freq, vel); break;
    case 2: drumMix.gain(2, 0.7f * v); hat  .noteOn(); Serial.printf("HAT   %.1fHz v=%.2f\n", freq, vel); break;
    case 3: drumMix.gain(3, 0.7f * v); ride .noteOn(); Serial.printf("RIDE  %.1fHz v=%.2f\n", freq, vel); break;
    case 4: mainMix.gain(2, 0.8f * v); crash.noteOn(); Serial.printf("CRASH %.1fHz v=%.2f\n", freq, vel); break;
  }
}

void setupDrums(){
  kick.frequency(60);   kick.length(150);  kick.secondMix(0.0f); kick.pitchMod(0.5f);
  snare.frequency(200); snare.length(100); snare.secondMix(1.0f); snare.pitchMod(0.2f);
  hat.frequency(800);   hat.length(40);    hat.secondMix(1.0f);   hat.pitchMod(0.0f);
  ride.frequency(500);  ride.length(300);  ride.secondMix(0.5f);  ride.pitchMod(0.1f);
  crash.frequency(900); crash.length(500); crash.secondMix(1.0f); crash.pitchMod(0.0f);

  drumMix.gain(0,0.9f); drumMix.gain(1,0.9f);
  drumMix.gain(2,0.7f); drumMix.gain(3,0.7f);

#if DIAG_DRY_MONITOR
  mainMix.gain(0,0.40f); // DRY ON for testing
#else
  mainMix.gain(0,0.00f);
#endif
  mainMix.gain(1,0.90f); // DRUMS
  mainMix.gain(2,0.80f); // CRASH
}

void setup(){
  Serial.begin(115200);
  delay(200);
  if (CrashReport) { Serial.println("=== CrashReport ==="); Serial.print(CrashReport); Serial.println("===================="); }

  AudioMemory(AUDIO_MEM);

  sgtl.enable();
  sgtl.inputSelect(AUDIO_INPUT_LINEIN);
  sgtl.lineInLevel(LINE_IN_LEVEL);
  sgtl.volume(VOL_DEFAULT);
  sgtl.unmuteHeadphone();

  inMix.gain(0, 0.7f);   // L
  inMix.gain(1, 0.7f);   // R
  inMix.gain(2, 0.0f);
  inMix.gain(3, 0.0f);

  // Notch + HPF (only affect biq path; analyzers may bypass in DIAG)
  biq.setNotch(0, NOTCH_HZ, 6.0f);
  biq.setHighpass(1, HPF_HZ, 0.707f);

  yin.begin(0.12f);                  // slightly snappier for debug
  fft.windowFunction(AudioWindowHanning256);

  setupDrums();

  // Boot beep (audible)
  beep.begin(WAVEFORM_SINE);
  beep.frequency(440);
  beep.amplitude(0.70f);
  delay(900);
  beep.amplitude(0.0f);

  Serial.println("Guitar->Drums (DIAG) ready.");
}

void loop(){
  // --- 1) Read analyzers ---
  bool haveRms = rms.available();
  float pRms   = haveRms ? rms.read() : 0.0f;
  float pPeak  = peak.read();  // always available

  // --- 2) Dual-envelope update (no early returns) ---
  bool quietFrame = (pRms < MIN_RMS_FOR_PITCH);
  if (haveRms && !quietFrame) {
    fastEnv = (1.0f - FAST_ALPHA) * fastEnv + FAST_ALPHA * pRms;
    slowEnv = (1.0f - SLOW_ALPHA) * slowEnv + SLOW_ALPHA * pRms;
  }
  float ratio = (slowEnv > 1e-8f) ? (fastEnv / slowEnv) : 0.0f;

  bool onsetDual = false;
  uint32_t now = millis();
  if (haveRms && !quietFrame && (now - lastOnsetMs > ONSET_MIN_GAP_MS)) {
    if ((ratio > (1.0f + RATIO_DELTA)) && ((fastEnv - slowEnv) > MIN_DELTA)) {
      onsetDual = true;
    }
  }

  // --- 3) Simple peak onset (diagnostic OR) ---
  bool onsetSimple = false;
#if DIAG_USE_SIMPLE_ONSET
  if ((now - lastOnsetMs > ONSET_MIN_GAP_MS) && (pPeak > SIMPLE_PEAK_THR)) {
    onsetSimple = true;
  }
#endif

  bool onset = onsetDual || onsetSimple;
  if (onset) lastOnsetMs = now;

  // --- 4) Pitch read (don’t gate by RMS; we want to see if YIN locks) ---
  float f = -1.0f, q = 0.0f;
  if (yin.available()) { f = yin.read(); q = yin.probability(); }

  // --- 5) Map + trigger with continuity lock ---
  if (onset && f > 0 && q >= YIN_CONF) {
    if (!(f > 56.0f && f < 64.0f)) { // reject mains
      int drumIdx = -1;
      if ((int32_t)(now - lockUntilMs) < 0 && lockedDrum >= 0) {
        drumIdx = lockedDrum;
      } else {
        int b = bandFor(f);
        if (b >= 0) drumIdx = bands[b].idx;
      }
      if (drumIdx >= 0 && canRetrigger(drumIdx)) {
        // Velocity from peak (diagnostic-friendly) blended with ratio
        float vP = clamp01(pPeak * 6.0f);
        float vR = clamp01((ratio - 1.0f) * 6.0f);
        float vel = 0.6f * vP + 0.4f * vR;
        trigger(drumIdx, vel, f);
        lockedDrum  = drumIdx;
        lockUntilMs = now + LOCK_MS;
      }
    }
  }

  // --- 6) Debug ---
  static uint32_t tdbg = 0;
  if (millis() - tdbg > 220) {
    Serial.printf("RMS=%.4f fast=%.4f slow=%.4f ratio=%.2f  peak=%.3f  onset(D/S)=%d/%d  f=%.1fHz q=%.2f\n",
                  pRms, fastEnv, slowEnv, ratio, pPeak, onsetDual?1:0, onsetSimple?1:0, f, q);
    tdbg = millis();
  }
}
