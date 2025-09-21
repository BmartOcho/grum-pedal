// Teensy 4.0 + Audio Shield with SD Card Sample Playback
// Guitar -> Drum trigger with SD card samples
// Samples should be 16-bit, 44.1kHz WAV files named:
// KICK.WAV, SNARE.WAV, HAT.WAV, RIDE.WAV, CRASH.WAV

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <CrashReport.h>

// Use built-in SD card on Teensy 4.0 Audio Shield
#define SDCARD_CS_PIN    10
#define SDCARD_MOSI_PIN  11
#define SDCARD_SCK_PIN   13

// ---------------- Audio objects ----------------
AudioInputI2S       in1;
AudioMixer4         inMix;
AudioFilterBiquad   biq;        // stage0 notch 60Hz, stage1 HPF 70Hz
AudioAnalyzeNoteFrequency yin;  // YIN pitch
AudioAnalyzeRMS     rms;        // RMS for onset envelope
AudioAnalyzePeak    peak;       // Peak for clipping/emergency
AudioAnalyzeFFT256  fft;        // For spectral flux

// SD card sample players (5 drums)
AudioPlaySdWav      kickWav, snareWav, hatWav, rideWav, crashWav;
AudioMixer4         drumMix1;   // Mix first 4 drums
AudioMixer4         drumMix2;   // Mix crash + drumMix1
AudioMixer4         mainMix;    // Final mix with dry signal
AudioSynthWaveform  beep;       // Boot confirmation beep
AudioOutputI2S      out1;

// ---------------- Connections ----------------
// Input processing chain
AudioConnection patchInL(in1, 0, inMix, 0);
AudioConnection patchInR(in1, 1, inMix, 1);
AudioConnection patchMixB(inMix, 0, biq, 0);
AudioConnection patchYIN(biq, 0, yin, 0);
AudioConnection patchRMS(biq, 0, rms, 0);
AudioConnection patchPEAK(biq, 0, peak, 0);
AudioConnection patchFFT(biq, 0, fft, 0);

// SD WAV players to drum mixer
AudioConnection patchKickL(kickWav, 0, drumMix1, 0);
AudioConnection patchSnareL(snareWav, 0, drumMix1, 1);
AudioConnection patchHatL(hatWav, 0, drumMix1, 2);
AudioConnection patchRideL(rideWav, 0, drumMix1, 3);

// Cascade mixers (we need more than 4 inputs)
AudioConnection patchDrums1(drumMix1, 0, drumMix2, 0);
AudioConnection patchCrashL(crashWav, 0, drumMix2, 1);

// Final mix
AudioConnection patchDrums(drumMix2, 0, mainMix, 1);
AudioConnection patchDry(inMix, 0, mainMix, 0);   // dry signal (muted by default)
AudioConnection patchBeep(beep, 0, mainMix, 2);   // boot beep

// Output to headphones (both channels)
AudioConnection patchOutL(mainMix, 0, out1, 0);
AudioConnection patchOutR(mainMix, 0, out1, 1);

AudioControlSGTL5000 sgtl;

// ---------------- Config ----------------
const uint16_t AUDIO_MEM       = 100;    // Increased for SD playback
const uint8_t  LINE_IN_LEVEL   = 14;     // 8..15; raise if RMS tiny
const float    VOL_DEFAULT     = 0.75f;  // Headphone volume
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
const float    FLUX_K          = 3.0f;      // threshold = base + K*sigma
const float    FLUX_MIN_ABS    = 0.02f;     // absolute floor
const float    FLUX_DECAY      = 0.90f;     // decay for variance proxy

// Clipping / Emergency mode
const float    PEAK_CLIP_LEVEL = 0.98f;     // near full-scale
const float    RMS_CLIP_LEVEL  = 0.60f;     // absurdly high; safety net
const int      CLIP_BLOCKS_ARM = 4;         // consecutive blocks to arm emergency
const uint32_t EMERGENCY_HOLD_MS = 600;     // mute time
const float    EMERGENCY_VOL    = 0.20f;    // reduced vol while held

// Sample file names on SD card
const char* sampleFiles[] = {
  "KICK.WAV",
  "SNARE.WAV",
  "HAT.WAV",
  "RIDE.WAV",
  "CRASH.WAV"
};

// ---------------- State ----------------
uint32_t lastTrig[5] = {0,0,0,0,0};
float    fastEnv = 0.0f, slowEnv = 0.0f;
uint32_t lastOnsetMs = 0;

// FFT / spectral flux state
float prevSpec[128] = {0};
float fluxBase = 0.0f;
float fluxVar  = 0.0f;
bool  havePrevSpec = false;

// Emergency mode state
int       clipBlocks = 0;
bool      emergency  = false;
uint32_t  emergencyStart = 0;
float     userVolume = VOL_DEFAULT;

// String continuity hysteresis (drum lock)
int       lockedDrum   = -1;
uint32_t  lockUntilMs  = 0;

// SD card status
bool      sdCardReady = false;
bool      samplesFound[5] = {false, false, false, false, false};

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
  if (!sdCardReady || !samplesFound[drumIdx]) {
    Serial.printf("Cannot trigger drum %d - sample not available\n", drumIdx);
    return;
  }
  
  float v = vel * 0.85f + 0.15f; // keep audible at low vel
  
  switch (drumIdx) {
    case 0: 
      kickWav.play(sampleFiles[0]);
      drumMix1.gain(0, 0.9f * v);
      Serial.printf("KICK  %.1fHz v=%.2f\n", freq, vel);
      break;
    case 1:
      snareWav.play(sampleFiles[1]);
      drumMix1.gain(1, 0.9f * v);
      Serial.printf("SNARE %.1fHz v=%.2f\n", freq, vel);
      break;
    case 2:
      hatWav.play(sampleFiles[2]);
      drumMix1.gain(2, 0.7f * v);
      Serial.printf("HAT   %.1fHz v=%.2f\n", freq, vel);
      break;
    case 3:
      rideWav.play(sampleFiles[3]);
      drumMix1.gain(3, 0.7f * v);
      Serial.printf("RIDE  %.1fHz v=%.2f\n", freq, vel);
      break;
    case 4:
      crashWav.play(sampleFiles[4]);
      drumMix2.gain(1, 0.8f * v);
      Serial.printf("CRASH %.1fHz v=%.2f\n", freq, vel);
      break;
  }
}

// ---------------- Setup ----------------
bool initSDCard() {
  if (!(SD.begin(SDCARD_CS_PIN))) {
    Serial.println("Unable to access SD card!");
    return false;
  }
  Serial.println("SD card initialized successfully");
  
  // Check for each sample file
  for (int i = 0; i < 5; i++) {
    if (SD.exists(sampleFiles[i])) {
      samplesFound[i] = true;
      Serial.printf("Found: %s\n", sampleFiles[i]);
    } else {
      Serial.printf("Missing: %s\n", sampleFiles[i]);
    }
  }
  
  return true;
}

void setupMixers() {
  // Drum mixer 1 (kick, snare, hat, ride)
  drumMix1.gain(0, 0.9f);  // kick
  drumMix1.gain(1, 0.9f);  // snare
  drumMix1.gain(2, 0.7f);  // hat
  drumMix1.gain(3, 0.7f);  // ride
  
  // Drum mixer 2 (drumMix1 + crash)
  drumMix2.gain(0, 1.0f);  // drumMix1
  drumMix2.gain(1, 0.8f);  // crash
  drumMix2.gain(2, 0.0f);
  drumMix2.gain(3, 0.0f);
  
  // Main mix
  mainMix.gain(0, 0.00f);  // dry signal (muted by default, increase to 0.1-0.3 for monitoring)
  mainMix.gain(1, 0.85f);  // drums
  mainMix.gain(2, 0.30f);  // beep channel
  mainMix.gain(3, 0.0f);
}

void setup() {
  Serial.begin(115200);
  delay(500);  // Give serial time to initialize
  
  Serial.println("\n=== Teensy Guitar-to-Drums with SD Samples ===");
  
  if (CrashReport) {
    Serial.println("=== CrashReport (previous boot) ===");
    Serial.print(CrashReport);
    Serial.println("===================================");
  }

  // Initialize audio system
  AudioMemory(AUDIO_MEM);
  sgtl.enable();
  sgtl.inputSelect(AUDIO_INPUT_LINEIN);
  sgtl.lineInLevel(LINE_IN_LEVEL);
  sgtl.volume(VOL_DEFAULT);
  sgtl.unmuteHeadphone();
  sgtl.lineOutLevel(29);  // Set line out level for headphones
  userVolume = VOL_DEFAULT;

  // Configure input mixer
  inMix.gain(0, 0.5f); // L
  inMix.gain(1, 0.5f); // R
  inMix.gain(2, 0.0f);
  inMix.gain(3, 0.0f);

  // Configure filters
  biq.setNotch(0, NOTCH_HZ, 8.0f);
  biq.setHighpass(1, HPF_HZ, 0.707f);

  // Initialize analyzers
  yin.begin(0.15f);
  fft.windowFunction(AudioWindowHanning256);

  // Setup mixers
  setupMixers();

  // Initialize SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  sdCardReady = initSDCard();
  
  if (!sdCardReady) {
    Serial.println("WARNING: SD card not ready - samples will not play!");
  }

  // Boot confirmation beep (1 second)
  beep.begin(WAVEFORM_SINE);
  beep.frequency(440);
  beep.amplitude(0.30f);
  delay(1000);
  beep.amplitude(0.0f);
  mainMix.gain(2, 0.0f);  // Mute beep channel

  Serial.printf("AudioMemory: %u / %u (max %u)\n",
                AudioMemoryUsage(), AUDIO_MEM, AudioMemoryUsageMax());
  Serial.println("Guitar->Drums ready. Plug headphones into audio shield.");
  Serial.println("Ensure samples are on SD card: KICK.WAV, SNARE.WAV, HAT.WAV, RIDE.WAV, CRASH.WAV");
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
    Serial.println("⚠️  Emergency mode: clipping detected -> volume reduced");
  }

  if (emergency && (now - emergencyStart >= EMERGENCY_HOLD_MS) && clipBlocks == 0) {
    emergency = false;
    sgtl.volume(userVolume);
    Serial.println("✅ Emergency cleared: volume restored");
  }
}

float computeSpectralFlux() {
  float flux = 0.0f;
  for (int i = 0; i < 128; ++i) {
    float v = fft.read(i);
    float d = v - prevSpec[i];
    if (d > 0) flux += d;
    prevSpec[i] = v;
  }
  return flux / 128.0f;
}

// ---------------- Main loop ----------------
void loop() {
  // 1) Analyzer availability
  bool haveRms = rms.available();
  bool haveFft = fft.available();
  bool havePeak= true;

  float pRms  = haveRms ? rms.read()  : 0.0f;
  float pPeak = peak.read();

  // 2) Emergency mode handling
  updateEmergency(pRms, pPeak);

  // 3) Dual-envelope onset (primary)
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
  static float fluxVarEMA = 0.0f;
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
      fluxVarEMA = FLUX_DECAY * fluxVarEMA + (1.0f - FLUX_DECAY) * diff * diff;
    }
    flux = fluxRaw;

    float fluxStdApprox = sqrtf(fluxVarEMA);
    float fluxThresh = fluxEMA + FLUX_K * fluxStdApprox;
    if (fluxRaw > max(fluxThresh, FLUX_MIN_ABS)) {
      onsetFlux = true;
    }
  }

  // 5) Combined onset
  bool onset = onsetDual && (onsetFlux || !haveFft);
  if (onset) lastOnsetMs = now;

  // 6) Pitch + drum mapping with continuity lock
  float f = -1.0f, q = 0.0f;
  if (yin.available()) {
    f = yin.read();
    q = yin.probability();
  }

  if (onset && f > 0 && q >= YIN_CONF) {
    // reject 60 Hz hum
    if (!(f > 56.0f && f < 64.0f)) {
      int b = -1;

      // Check for locked drum
      if ((int32_t)(now - lockUntilMs) < 0 && lockedDrum >= 0) {
        b = lockedDrum;
      } else {
        int bandIdx = bandFor(f);
        if (bandIdx >= 0) b = bands[bandIdx].idx;
      }

      if (b >= 0) {
        // Calculate velocity
        float velFromRatio = clamp01((ratio - 1.0f) * 6.0f);
        float velFromFlux  = clamp01(flux * 8.0f);
        float vel = clamp01(0.65f * velFromRatio + 0.35f * velFromFlux);
        vel = 0.15f + 0.85f * vel;

        if (canRetrigger(b)) {
          trigger(b, vel, f);
          lockedDrum  = b;
          lockUntilMs = now + LOCK_MS;
        }
      }
    }
  }

  // 7) Debug output (throttled)
  static uint32_t tdbg = 0;
  if (millis() - tdbg > 500) {  // Reduced frequency
    Serial.printf("RMS=%.4f ratio=%.2f | f=%.1fHz q=%.2f | SD=%d\n",
                  pRms, ratio, f, q, sdCardReady?1:0);
    tdbg = millis();
  }
}