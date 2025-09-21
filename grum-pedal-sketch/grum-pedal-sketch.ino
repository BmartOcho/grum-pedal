#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <CrashReport.h>

// ---------- Graph (simple & robust) ----------
AudioInputI2S             in1;        // audio shield line-in (stereo -> we use left)
AudioFilterBiquad         hpf;        // DC/rumble removal
AudioAnalyzeNoteFrequency yin;        // robust pitch (YIN)
AudioAnalyzePeak          peak;       // time-domain peak for onset + velocity

AudioSynthSimpleDrum      kick, snare, hat, ride, crash;
AudioMixer4               drumMix, mainMix;
AudioOutputI2S            out1;
AudioSynthWaveform        beep;

AudioMixer4 inMix;


AudioConnection patchInHPF(inMix, 0, hpf, 0);  // summed -> HPF -> YIN/Peak
AudioConnection patchCord2(hpf, 0, yin, 0);
AudioConnection patchCord3(hpf, 0, peak, 0);
AudioConnection patchCord4(kick,  0, drumMix, 0);
AudioConnection patchCord5(snare, 0, drumMix, 1);
AudioConnection patchCord6(hat,   0, drumMix, 2);
AudioConnection patchCord7(ride,  0, drumMix, 3);
AudioConnection patchCord8(drumMix, 0, mainMix, 1);
AudioConnection patchCord9(mainMix, 0, out1, 0);
AudioConnection patchCord10(mainMix, 0, out1, 1);
AudioConnection patchInL(in1, 0, inMix, 0);  // left
AudioConnection patchInR(in1, 1, inMix, 1); // right
AudioConnection patchDry(inMix, 0, mainMix, 0);  //  summed dry -> main mix ch0 

AudioControlSGTL5000 sgtl;

// ---------- Config ----------
const uint16_t AUDIO_MEM = 60;
const uint8_t  LINE_IN_LEVEL = 15;     // adjust 8..15 to taste
const float    HPF_HZ = 30.0f;

// YIN confidence
const float    YIN_CONF = 0.80f;       // 0.80..0.90

// Debounce/hysteresis
const uint32_t RETRIGGER_MS = 90;
const float    HYST_PCT = 0.06f;       // ±6% around band edges

// Onset parameters
const float    EMA_ALPHA = 0.05f;      // 0.03..0.10 (smoothing). Larger = quicker adaptation
const float    THRESH_MULT = 1.5f;     // 1.4..2.2 (sensitivity). Lower = more sensitive
const float    NOISE_FLOOR = 0.0025f;   // small floor so idle noise doesn’t trigger
const uint32_t ONSET_MIN_GAP_MS = 70;  // global onset guard (in addition to per-drum debounce)

// Bands for standard tuning
struct Band { float fmin, fmax; uint8_t idx; };
Band bands[] = {
  {  0.0f,  100.0f, 0 }, // -> Kick
  { 110.0f, 200.0f, 1 }, //  -> Snare
  { 210.0f, 300.0f, 2 }, //  -> Hat
  { 310.0f, 400.0f, 2 }, //  -> Hat
  { 410.0f, 500.0f, 3 }, //  -> Ride
  { 510.0f, 600.0f, 4 }, //  -> Crash
};

// State
uint32_t lastTrig[5] = {0,0,0,0,0};
uint32_t lastOnsetMs = 0;

float emaEnv = 0.0f;     // adaptive envelope for onset thresholding

// ---------- Helpers ----------
float clamp01(float x){ return x<0?0:(x>1?1:x); }

float readVelocity() {
  float p = peak.read();                 // peak is already 0..~1
  return clamp01(p);
}

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
  float v = vel * 0.85f + 0.15f;       // keep audible at low vel
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

  mainMix.gain(0, 0.0f); // dry muted for now
  mainMix.gain(1, 0.8f); // drums bus
  mainMix.gain(2, 0.6f); // crash direct
}

void setup() {
  Serial.begin(115200);
  delay(300);
  beep.begin(WAVEFORM_SINE);
  beep.frequency(440);
  beep.amplitude(0.3f);
  delay(1000);
  beep.amplitude(0.0f);



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
  sgtl.unmuteHeadphone(); // belt-and-suspenders: ensure HP out is enabled
  
  inMix.gain(0, 0.5f);  // L
  inMix.gain(1, 0.5f);  // R
  inMix.gain(2, 0.0f);
  inMix.gain(3, 0.0f);


  hpf.setHighpass(0, HPF_HZ, 0.707f);
  yin.begin(0.15f);  // stable pitch

  setupDrums();

  // Boot beep
  kick.noteOn(); delay(110);
  snare.noteOn(); delay(110);
  hat.noteOn();   delay(110);
  crash.noteOn(); delay(160);

  Serial.printf("AudioMemory now %u / %u (max %u)\n",
                AudioMemoryUsage(), AUDIO_MEM, AudioMemoryUsageMax());
  Serial.println("Stage 2: Onset-gated drums ready.");
}

void loop() {
  // --- Adaptive onset tracking from peak ---
  float p = peak.read();                        // recent peak estimate
  emaEnv = (1.0f - EMA_ALPHA) * emaEnv + EMA_ALPHA * p;
  float thresh = max(NOISE_FLOOR, emaEnv * THRESH_MULT);

  bool onset = (p > thresh) && ((millis() - lastOnsetMs) > ONSET_MIN_GAP_MS);

  // --- Pitch ---
  if (yin.available()) {
    float f = yin.read();
    float q = yin.probability();

    if (onset && f > 0 && q >= YIN_CONF) {
      int b = bandFor(f);
      if (b >= 0) {
        float vel = clamp01(p);                 // velocity at the spike
        uint8_t drumIdx = bands[b].idx;
        if (canRetrigger(drumIdx)) {
          trigger(drumIdx, vel, f);
          lastOnsetMs = millis();
        }
      } else {
        // Uncomment for debugging “between-band” notes
        // Serial.printf("Onset %.3f > %.3f but freq %.1fHz not mapped (q=%.2f)\n", p, thresh, f, q);
        lastOnsetMs = millis(); // still consume this onset to avoid rapid re-hits
      }
    }
  }

  // Heartbeat/memory every ~500ms
  static uint32_t tdbg=0;
  if (millis() - tdbg > 150) {
    float f_dbg = yin.read();            // safe: returns last computed value
    float q_dbg = yin.probability();
    Serial.printf("p=%.3f  env=%.3f  thr=%.3f  f=%.1fHz  q=%.2f\n", p, emaEnv, thresh, f_dbg, q_dbg);
    tdbg = millis();
  }
 }
