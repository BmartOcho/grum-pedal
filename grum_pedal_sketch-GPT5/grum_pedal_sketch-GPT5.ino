// Teensy 4.0 + Audio Shield: Guitar -> Drum Trigger
// Reliable pitch (YIN) + dual-envelope onset + per-string bands + dry monitor

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <CrashReport.h>

// ---------------- Audio Objects ----------------
AudioInputI2S       in1;       // Audio Shield line-in (stereo)
AudioMixer4         inMix;     // sum L+R to mono
AudioFilterBiquad   hpf;       // remove DC/rumble
AudioAnalyzeNoteFrequency yin; // robust pitch (YIN)
AudioAnalyzePeak    peak;      // time-domain peak (for onset/velocity)
AudioAnalyzeRMS     rms;  // Ben added this

AudioSynthSimpleDrum kick, snare, hat, ride, crash;
AudioMixer4         drumMix;
AudioMixer4         mainMix;
AudioSynthWaveform  beep;      // boot beep / path check
AudioOutputI2S      out1;

AudioConnection patchInL(in1, 0, inMix, 0);        // L -> inMix ch0
AudioConnection patchInR(in1, 1, inMix, 1);        // R -> inMix ch1
AudioConnection patchInHPF(inMix, 0, hpf, 0);      // mono -> HPF
AudioConnection patchYIN(hpf, 0, yin, 0);
AudioConnection patchPeak(hpf, 0, peak, 0);
AudioConnection patchRMS(hpf, 0, rms, 0);  //  Ben added this too

AudioConnection patchKick(kick,   0, drumMix, 0);
AudioConnection patchSnare(snare, 0, drumMix, 1);
AudioConnection patchHat(hat,     0, drumMix, 2);
AudioConnection patchRide(ride,   0, drumMix, 3);

AudioConnection patchDrums(drumMix, 0, mainMix, 1); // drums bus -> main ch1
AudioConnection patchCrash(crash,   0, mainMix, 2); // crash direct -> main ch2
AudioConnection patchDry(inMix,     0, mainMix, 0); // dry mono -> main ch0
AudioConnection patchBeep(beep,     0, mainMix, 0); // beep shares dry channel

AudioConnection patchOutL(mainMix, 0, out1, 0);
AudioConnection patchOutR(mainMix, 0, out1, 1);

AudioControlSGTL5000 sgtl;

// ---------------- Config ----------------
const uint16_t AUDIO_MEM     = 60;
const uint8_t  LINE_IN_LEVEL = 14;       // was 8 or 15 earlier, trying 14 now...
const float    HPF_HZ        = 30.0f;
const float MIN_RMS_FOR_PITCH = 0.0006f;  // ignore junk below this

// YIN pitch confidence (0..1)
const float    YIN_CONF      = 0.80f;    // 0.80–0.85 recommended

// Per-drum debounce
const uint32_t RETRIGGER_MS  = 90;       // per-drum lockout

// Band hysteresis
const float    HYST_PCT      = 0.06f;    // ±6%

// Dual-EMA onset parameters (scale-free)
const float    FAST_ALPHA    = 0.35f;    // attack envelope
const float    SLOW_ALPHA    = 0.02f;    // baseline envelope
const float    RATIO_DELTA   = 0.06f;    // was 0.15
const float    MIN_DELTA     = 0.00010f; // was 0.02
const uint32_t ONSET_MIN_GAP_MS = 90;    // a touch more gaurd

// ---------------- State ----------------
float fastEnv = 0.0f, slowEnv = 0.0f;
uint32_t lastOnsetMs = 0;
uint32_t lastTrig[5] = {0,0,0,0,0};

// EADGBE open-string target bands (Hz) -> drum index
struct Band { float fmin, fmax; uint8_t idx; };
Band bands[] = {
  {  74.0f,  92.0f, 0 }, // E2  -> Kick
  { 100.0f, 122.0f, 1 }, // A2  -> Snare
  { 135.0f, 160.0f, 2 }, // D3  -> Hat
  { 185.0f, 210.0f, 2 }, // G3  -> Hat
  { 235.0f, 260.0f, 3 }, // B3  -> Ride
  { 315.0f, 350.0f, 4 }, // E4  -> Crash
};

// ---------------- Helpers ----------------
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

void setupDrums() {
  kick.frequency(60);   kick.length(150);  kick.secondMix(0.0f); kick.pitchMod(0.5f);
  snare.frequency(200); snare.length(100); snare.secondMix(1.0f); snare.pitchMod(0.2f);
  hat.frequency(800);   hat.length(40);    hat.secondMix(1.0f);   hat.pitchMod(0.0f);
  ride.frequency(500);  ride.length(300);  ride.secondMix(0.5f);  ride.pitchMod(0.1f);
  crash.frequency(900); crash.length(500); crash.secondMix(1.0f); crash.pitchMod(0.0f);

  drumMix.gain(0,0.7f); drumMix.gain(1,0.7f);
  drumMix.gain(2,0.5f); drumMix.gain(3,0.5f);

  mainMix.gain(0, 0.0f); // dry
  mainMix.gain(1, 0.8f); // drums bus
  mainMix.gain(2, 0.60f); // crash direct
}

// ---------------- Setup ----------------
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
  sgtl.volume(0.80f);
  sgtl.unmuteHeadphone();

  inMix.gain(0, 0.5f); // L
  inMix.gain(1, 0.5f); // R
  inMix.gain(2, 0.0f);
  inMix.gain(3, 0.0f);

  hpf.setNotch(0, 60.0f, 8.0f);  // stage 0: notch out main hums
  hpf.setHighpass(1, 70.0f, 0.707f);  // stage 1: gentle HPF; E2 still passes

  hpf.setHighpass(0, HPF_HZ, 0.707f);
  yin.begin(0.15f); // stable pitch

  setupDrums();

  // Boot beep (1s @ 440 Hz) to confirm output path
  beep.begin(WAVEFORM_SINE);
  beep.frequency(440);
  beep.amplitude(0.30f);
  delay(1000);
  beep.amplitude(0.0f);

  Serial.printf("AudioMemory now %u / %u (max %u)\n",
                AudioMemoryUsage(), AUDIO_MEM, AudioMemoryUsageMax());
  Serial.println("Guitar->Drums ready.");
}

// ---------------- Loop ----------------
void loop() {
  // ---- 1) Read analyzers with availability gating (no early returns) ----
  bool haveRms = rms.available();
  float p = haveRms ? rms.read() : 0.0f;

  // Reject ultra-quiet frames from influencing envelopes, but don't return
  const bool quietFrame = (p < MIN_RMS_FOR_PITCH);

  if (!quietFrame && haveRms) {
    // Dual-envelope (scale-free onset)
    fastEnv = (1.0f - FAST_ALPHA) * fastEnv + FAST_ALPHA * p;
    slowEnv = (1.0f - SLOW_ALPHA) * slowEnv + SLOW_ALPHA * p;
  }

  // Compute onset from current envelopes
  bool onset = false;
  uint32_t now = millis();
  float ratio = (slowEnv > 1e-6f) ? (fastEnv / slowEnv) : 0.0f;

  if (!quietFrame && haveRms && (now - lastOnsetMs > ONSET_MIN_GAP_MS)) {
    if ((ratio > (1.0f + RATIO_DELTA)) && ((fastEnv - slowEnv) > MIN_DELTA)) {
      onset = true;
      lastOnsetMs = now;
    }
  }

  // ---- 2) Pitch + trigger (no early returns; we just skip mapping) ----
  float f = -1.0f, q = 0.0f;
  if (yin.available()) {
    f = yin.read();
    q = yin.probability();
  }

  // Only consider valid, confident pitch & a detected onset
  if (onset && f > 0 && q >= YIN_CONF) {
    // Skip mains hum region
    if (f < 56.0f || f > 64.0f) {
      int b = bandFor(f);
      if (b >= 0) {
        // Velocity from onset sharpness (ratio-1 scaled), with a small floor
        const float VEL_GAIN = 6.0f;                 // adjust 4..8 to taste
        float vel = clamp01((ratio - 1.0f) * VEL_GAIN);
        vel = 0.15f + vel * 0.85f;                   // floor so soft hits still audible

        uint8_t drumIdx = bands[b].idx;
        if (canRetrigger(drumIdx)) {
          trigger(drumIdx, vel, f);
        }
      }
    }
  }

  // ---- 3) Debug / heartbeat (always runs; throttled) ----
  static uint32_t tdbg = 0;
  if (millis() - tdbg > 200) {
    Serial.printf("RMS=%.4f  fast=%.4f  slow=%.4f  ratio=%.2f  f=%.1fHz  q=%.2f  onset=%d\n",
                  p, fastEnv, slowEnv, ratio, f, q, onset ? 1 : 0);
    tdbg = millis();
  }
}

  
