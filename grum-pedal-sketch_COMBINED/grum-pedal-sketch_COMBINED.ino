// Teensy 4.0 + Audio Shield
// Guitar -> Drum trigger with:
// - Dual-envelope onset (primary)
// - Spectral flux confirmation (FFT256)
// - String continuity hysteresis (lock drum for a short window)
// - Clipping detection & emergency mute/recover
//
// Route: I2S(line-in L/R) -> mix-> biquad(60Hz notch + HPF 70Hz) -> YIN/RMS/Peak/FFT
// Output: Drums to main mix, dry muted (enable if desired). 1s boot beep confirms output path.

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <CrashReport.h>

// ---------------- Audio objects ----------------
AudioInputI2S       in1;
AudioMixer4         inMix;
AudioFilterBiquad   biq;        // stage0 notch 60Hz, stage1 HPF 70Hz
AudioAnalyzeNoteFrequency yin;  // YIN pitch
AudioAnalyzeRMS     rms;        // RMS for onset envelope
AudioAnalyzePeak    peak;       // Peak for clipping/emergency
AudioAnalyzeFFT256  fft;        // For spectral flux
AudioSynthSimpleDrum kick, snare, hat, ride, crash;
AudioMixer4         drumMix;
AudioMixer4         mainMix;
AudioSynthWaveform  beep;
AudioOutputI2S      out1;

// ---------------- Connections ----------------
AudioConnection patchInL(in1, 0, inMix, 0);
AudioConnection patchInR(in1, 1, inMix, 1);
AudioConnection patchMixB(inMix, 0, biq, 0);
AudioConnection patchYIN(biq, 0, yin, 0);
AudioConnection patchRMS(biq, 0, rms, 0);
AudioConnection patchPEAK(biq, 0, peak, 0);
AudioConnection patchFFT(biq, 0, fft, 0);

AudioConnection patchKick(kick,   0, drumMix, 0);
AudioConnection patchSnare(snare, 0, drumMix, 1);
AudioConnection patchHat(hat,     0, drumMix, 2);
AudioConnection patchRide(ride,   0, drumMix, 3);

AudioConnection patchDrums(drumMix, 0, mainMix, 1);
AudioConnection patchCrash(crash,   0, mainMix, 2);
AudioConnection patchDry(inMix,     0, mainMix, 0);   // dry (muted by gain)
AudioConnection patchBeep(beep,     0, mainMix, 0);

AudioConnection patchOutL(mainMix, 0, out1, 0);
AudioConnection patchOutR(mainMix, 0, out1, 1);

AudioControlSGTL5000 sgtl;

// ---------------- Config ----------------
const uint16_t AUDIO_MEM       = 60;
const uint8_t  LINE_IN_LEVEL   = 14;     // 8..15; raise if RMS tiny
const float    VOL_DEFAULT     = 0.80f;
const float    HPF_HZ          = 70.0f;  // keeps E2 (82.4 Hz) intact
const float    NOTCH_HZ        = 60.0f;

// YIN pitch confidence
const float    YIN_CONF        = 0.80f;

// Per-drum debounce & mapping lock
const uint32_t RETRIGGER_MS    = 90;     // per-drum lockout
const float    HYST_PCT        = 0.06f;  // band hysteresis
const uint32_t LOCK_MS         = 150;    // string continuity: lock drum choice after onset

// Dual-EMA onset (primary)
const float    FAST_ALPHA      = 0.35f;
const float    SLOW_ALPHA      = 0.02f;
const float    RATIO_DELTA     = 0.06f;     // lower = more sensitive
const float    MIN_DELTA       = 0.00010f;
const uint32_t ONSET_MIN_GAP_MS= 90;
const float    MIN_RMS_FOR_PITCH = 0.0006f; // ignore frames below this

// Spectral flux confirmation
const float    FLUX_ALPHA      = 0.15f;     // EMA for flux baseline
const float    FLUX_K          = 3.0f;      // threshold = base + K*sigma (approx via MAD proxy)
const float    FLUX_MIN_ABS    = 0.02f;     // absolute floor to allow confirmation on very quiet
const float    FLUX_DECAY      = 0.90f;     // decay for variance proxy

// Clipping / Emergency mode
const float    PEAK_CLIP_LEVEL = 0.98f;     // near full-scale
const float    RMS_CLIP_LEVEL  = 0.60f;     // absurdly high; safety net
const int      CLIP_BLOCKS_ARM = 4;         // consecutive blocks to arm emergency
const uint32_t EMERGENCY_HOLD_MS = 600;     // mute time
const float    EMERGENCY_VOL    = 0.20f;    // reduced vol while held

// ---------------- State ----------------
uint32_t lastTrig[5] = {0,0,0,0,0};
float    fastEnv = 0.0f, slowEnv = 0.0f;
uint32_t lastOnsetMs = 0;

// FFT / spectral flux state
float prevSpec[128] = {0};   // FFT256 returns 0..127 bins
float fluxBase = 0.0f;       // EMA baseline
float fluxVar  = 0.0f;       // variance proxy (EMA of squared deviation, decayed)
bool  havePrevSpec = false;

// Emergency mode state
int       clipBlocks = 0;
bool      emergency  = false;
uint32_t  emergencyStart = 0;
float     userVolume = VOL_DEFAULT;  // to restore after emergency

// String continuity hysteresis (drum lock)
int       lockedDrum   = -1;
uint32_t  lockUntilMs  = 0;

// Bands for EADGBE open strings
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

bool canRetrigger(uint8_t i) {
  uint32_t now = millis();
  if (now - lastTrig[i] >= RETRIGGER_MS) { lastTrig[i] = now; return true; }
  return false;
}

int bandFor(float f) {
  for (uint8_t i=0; i<sizeof(bands)/sizeof(bands[0]); ++i) {
    float cmin = bands[i].fmin * (1.0f - HYST_PCT);
    float cmax = bands[i].fmax * (1.0f + HYST_PCT);
    if (f >= cmin && f <= cmax) return i;
  }
  return -1;
}

void trigger(uint8_t drumIdx, float vel, float freq) {
  float v = vel * 0.85f + 0.15f; // keep audible at low vel
  switch (drumIdx) {
    case 0: drumMix.gain(0, 0.7f * v); kick .noteOn(); Serial.printf("KICK  %.1fHz v=%.2f\n",  freq, vel); break;
    case 1: drumMix.gain(1, 0.7f * v); snare.noteOn(); Serial.printf("SNARE %.1fHz v=%.2f\n", freq, vel); break;
    case 2: drumMix.gain(2, 0.5f * v); hat  .noteOn(); Serial.printf("HAT   %.1fHz v=%.2f\n", freq, vel); break;
    case 3: drumMix.gain(3, 0.5f * v); ride .noteOn(); Serial.printf("RIDE  %.1fHz v=%.2f\n", freq, vel); break;
    case 4: mainMix.gain(2, 0.6f * v); crash.noteOn(); Serial.printf("CRASH %.1fHz v=%.2f\n", freq, vel); break;
  }
}

// ---------------- Setup ----------------
void setupDrums() {
  kick.frequency(60);   kick.length(150);  kick.secondMix(0.0f); kick.pitchMod(0.5f);
  snare.frequency(200); snare.length(100); snare.secondMix(1.0f); snare.pitchMod(0.2f);
  hat.frequency(800);   hat.length(40);    hat.secondMix(1.0f);   hat.pitchMod(0.0f);
  ride.frequency(500);  ride.length(300);  ride.secondMix(0.5f);  ride.pitchMod(0.1f);
  crash.frequency(900); crash.length(500); crash.secondMix(1.0f); crash.pitchMod(0.0f);

  drumMix.gain(0,0.7f); drumMix.gain(1,0.7f);
  drumMix.gain(2,0.5f); drumMix.gain(3,0.5f);

  mainMix.gain(0,0.00f); // dry muted by default (raise to 0.05..0.25 if you want monitor)
  mainMix.gain(1,0.80f); // drums bus
  mainMix.gain(2,0.60f); // crash direct
}

void setup() {
  Serial.begin(115200);
  delay(300);
  if (CrashReport) {
    Serial.println("=== CrashReport (previous boot) ===");
    Serial.print(CrashReport);
    Serial.println("===================================");
  }

  AudioMemory(AUDIO_MEM);
  sgtl.enable();
  sgtl.inputSelect(AUDIO_INPUT_LINEIN);
  sgtl.lineInLevel(LINE_IN_LEVEL);
  sgtl.volume(VOL_DEFAULT);
  sgtl.unmuteHeadphone();
  userVolume = VOL_DEFAULT;

  inMix.gain(0, 0.5f); // L
  inMix.gain(1, 0.5f); // R
  inMix.gain(2, 0.0f);
  inMix.gain(3, 0.0f);

  // Stage 0 notch 60Hz, Stage 1 HPF
  biq.setNotch(0, NOTCH_HZ, 8.0f);
  biq.setHighpass(1, HPF_HZ, 0.707f);

  yin.begin(0.15f);   // stable pitch
  fft.windowFunction(AudioWindowHanning256);

  setupDrums();

  // Boot beep (1s)
  beep.begin(WAVEFORM_SINE);
  beep.frequency(440);
  beep.amplitude(0.30f);
  delay(1000);
  beep.amplitude(0.0f);

  Serial.printf("AudioMemory now %u / %u (max %u)\n",
                AudioMemoryUsage(), AUDIO_MEM, AudioMemoryUsageMax());
  Serial.println("Guitar->Drums ready.");
}

// ---------------- Loop helpers ----------------
void updateEmergency(float pRms, float pPeak) {
  // Track clipping blocks
  if (pPeak >= PEAK_CLIP_LEVEL || pRms >= RMS_CLIP_LEVEL) {
    clipBlocks++;
  } else if (clipBlocks > 0) {
    clipBlocks--;
  }

  uint32_t now = millis();
  if (!emergency && clipBlocks >= CLIP_BLOCKS_ARM) {
    emergency = true;
    emergencyStart = now;
    sgtl.volume(EMERGENCY_VOL);
    Serial.println("⚠️  Emergency mode: clipping detected -> volume reduced & drums still active");
  }

  if (emergency && (now - emergencyStart >= EMERGENCY_HOLD_MS) && clipBlocks == 0) {
    emergency = false;
    sgtl.volume(userVolume);
    Serial.println("✅ Emergency cleared: volume restored");
  }
}

float computeSpectralFlux() {
  // Requires fft.available() to be true before calling
  float flux = 0.0f;
  for (int i = 0; i < 128; ++i) {
    float v = fft.read(i);                 // magnitude (0..~1)
    float d = v - prevSpec[i];
    if (d > 0) flux += d;                  // positive changes only
    prevSpec[i] = v;
  }
  // Normalize roughly by number of bins
  return flux / 128.0f;
}

// ---------------- Main loop ----------------
void loop() {
  // 1) Analyzer availability
  bool haveRms = rms.available();
  bool haveFft = fft.available(); // independent clocking
  bool havePeak= true;            // peak.read() is cheap; no avail() method

  float pRms  = haveRms ? rms.read()  : 0.0f;
  float pPeak = peak.read();

  // 2) Emergency mode handling
  updateEmergency(pRms, pPeak);

  // 3) Dual-envelope onset (primary) — only update envelopes on non-quiet frames
  bool quietFrame = (pRms < MIN_RMS_FOR_PITCH);
  if (haveRms && !quietFrame) {
    fastEnv = (1.0f - FAST_ALPHA) * fastEnv + FAST_ALPHA * pRms;
    slowEnv = (1.0f - SLOW_ALPHA) * slowEnv + SLOW_ALPHA * pRms;
  }
  float ratio = (slowEnv > 1e-6f) ? (fastEnv / slowEnv) : 0.0f;

  bool onsetDual = false;
  uint32_t now = millis();
  if (haveRms && !quietFrame && (now - lastOnsetMs > ONSET_MIN_GAP_MS)) {
    if ((ratio > (1.0f + RATIO_DELTA)) && ((fastEnv - slowEnv) > MIN_DELTA)) {
      onsetDual = true;
    }
  }

  // 4) Spectral flux confirmation (secondary)
  static float fluxEMA = 0.0f;
  static float fluxVarEMA = 0.0f; // variance proxy
  static bool  fluxInit = false;
  float flux = 0.0f;
  bool onsetFlux = false;

  if (haveFft) {
    float fluxRaw = computeSpectralFlux();
    if (!fluxInit) {
      fluxEMA = fluxRaw;
      fluxVarEMA = 0.0f;
      fluxInit = true;
      havePrevSpec = true;
    } else {
      float diff = fluxRaw - fluxEMA;
      fluxEMA = (1.0f - FLUX_ALPHA) * fluxEMA + FLUX_ALPHA * fluxRaw;
      // simple variance proxy
      fluxVarEMA = FLUX_DECAY * fluxVarEMA + (1.0f - FLUX_DECAY) * diff * diff;
    }
    flux = fluxRaw;

    float fluxStdApprox = sqrtf(fluxVarEMA);
    float fluxThresh = fluxEMA + FLUX_K * fluxStdApprox;
    if (fluxRaw > max(fluxThresh, FLUX_MIN_ABS)) {
      onsetFlux = true;
    }
  }

  // 5) Combined onset: require primary AND (flux confirmation OR no FFT yet)
  bool onset = onsetDual && (onsetFlux || !haveFft);
  if (onset) lastOnsetMs = now;

  // 6) Pitch + drum mapping with continuity lock & hum reject
  float f = -1.0f, q = 0.0f;
  if (yin.available()) {
    f = yin.read();
    q = yin.probability();
  }

  if (onset && f > 0 && q >= YIN_CONF) {
    // reject 60 Hz +/- 4 Hz
    if (!(f > 56.0f && f < 64.0f)) {
      int b = -1;

      // If we are in a lock window, reuse the locked drum
      if ((int32_t)(now - lockUntilMs) < 0 && lockedDrum >= 0) {
        b = lockedDrum; // use locked drum index directly
      } else {
        int bandIdx = bandFor(f);
        if (bandIdx >= 0) b = bands[bandIdx].idx;
      }

      if (b >= 0) {
        // Velocity: combine ratio (attack sharpness) with flux for extra brightness feel
        float velFromRatio = clamp01((ratio - 1.0f) * 6.0f); // tweak 4..8 gain to taste
        float velFromFlux  = clamp01(flux * 8.0f);           // scale flux a bit
        float vel = clamp01(0.65f * velFromRatio + 0.35f * velFromFlux);
        vel = 0.15f + 0.85f * vel; // floor so very soft hits still audible

        if (canRetrigger(b)) {
          trigger(b, vel, f);
          // lock drum choice for continuity
          lockedDrum  = b;
          lockUntilMs = now + LOCK_MS;
        }
      }
    }
  }

  // 7) Debug / heartbeat (throttled)
  static uint32_t tdbg = 0;
  if (millis() - tdbg > 220) {
    Serial.printf("RMS=%.4f fast=%.4f slow=%.4f ratio=%.2f | flux=%.4f base=%.4f | onset(D/F)=%d/%d | f=%.1fHz q=%.2f | E=%d\n",
                  pRms, fastEnv, slowEnv, ratio,
                  flux, fluxEMA, onsetDual?1:0, onsetFlux?1:0, f, q, emergency?1:0);
    tdbg = millis();
  }
}
