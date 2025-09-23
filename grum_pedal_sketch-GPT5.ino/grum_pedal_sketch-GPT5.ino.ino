// GUITAR -> DRUM SAMPLES (Teensy Audio)
// Plays SD WAV samples based on detected guitar frequency / band energy.
//
// Hardware: Teensy 4.x + Audio Shield (Rev D). Guitar into Line In or Mic In.
// SD wiring: use the Audio Shield SD slot. Set SD CS to BUILTIN_SDCARD on T4.1,
// or pin 10 for the Audio Shield (Rev D) on T4.0. See SD_CS below.

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// ---------- CONFIG ----------
#if defined(ARDUINO_TEENSY41)
  const int SD_CS = BUILTIN_SDCARD;     // Teensy 4.1 onboard SD
#else
  const int SD_CS = 10;                 // Audio Shield Rev D CS
#endif

// Thresholds & timing
float inputGain = 1.0f;                 // front-end gain into mixers
float onsetThreshold = 0.02f;           // peak amplitude to consider a hit
uint32_t retriggerMs = 120;             // prevent machine-gun retriggers

// Frequency band gates (Hz)
float kickMaxHz  = 120.0f;              // low thump
float snareMinHz = 140.0f, snareMaxHz = 320.0f;
float tomMinHz   = 80.0f,  tomMaxHz   = 240.0f;    // wide tom band
float rideMinHz  = 700.0f, rideMaxHz  = 1200.0f;
float crashMinHz = 900.0f;                          // bright top
float hatMinHz   = 4000.0f;                          // closed hats by brightness

// File names on SD (8.3 uppercase recommended)
const char* FILE_KICK  = "KICK.WAV";
const char* FILE_SNARE = "SNARE.WAV";
const char* FILE_TOM1  = "TOM1.WAV";
const char* FILE_TOM2  = "TOM2.WAV";
const char* FILE_RIDE  = "RIDE.WAV";
const char* FILE_CRASH = "CRASH.WAV";
const char* FILE_HHCL  = "HHCL.WAV";
const char* FILE_HHOP  = "HHOP.WAV";

// ---------- AUDIO GRAPH ----------
// Input
AudioInputI2S        in;            // from Audio Shield (line/mic)
AudioAmplifier       inGain;

// Analysis
AudioAnalyzePeak     peak;
AudioAnalyzeFFT1024  fft;

// Players (use a couple in round-robin so overlaps don’t cut off)
const int NPLAY = 6;
AudioPlaySdWav       play[NPLAY];

// Mixers
AudioMixer4          mixA;
AudioMixer4          mixB;
AudioMixer4          mainMix;       // final sum to outputs
AudioOutputI2S       out;

// Patchcords
AudioConnection pc1(in, 0, inGain, 0);
AudioConnection pc2(inGain, 0, peak, 0);
AudioConnection pc3(inGain, 0, fft, 0);

// Players into two subgroup mixers so we can sum >4 sources
AudioConnection pc4(play[0], 0, mixA, 0);
AudioConnection pc5(play[1], 0, mixA, 1);
AudioConnection pc6(play[2], 0, mixA, 2);
AudioConnection pc7(play[3], 0, mixA, 3);

AudioConnection pc8(play[4], 0, mixB, 0);
AudioConnection pc9(play[5], 0, mixB, 1);

AudioConnection pc10(mixA, 0, mainMix, 0);
AudioConnection pc11(mixB, 0, mainMix, 1);

// Optional: blend a touch of live guitar through
AudioConnection pc12(inGain, 0, mainMix, 2);

AudioConnection pc13(mainMix, 0, out, 0);
AudioConnection pc14(mainMix, 0, out, 1);

AudioControlSGTL5000 codec;

// ---------- STATE ----------
uint32_t lastTrig = 0;
int nextPlayer = 0;

// ---------- HELPERS ----------
bool canRetrigger() {
  return (millis() - lastTrig) > retriggerMs;
}

void triggerFile(const char* name) {
  // round-robin so multiple hits can overlap
  for (int i = 0; i < NPLAY; ++i) {
    int idx = (nextPlayer + i) % NPLAY;
    if (!play[idx].isPlaying()) {
      play[idx].play(name);
      nextPlayer = (idx + 1) % NPLAY;
      lastTrig = millis();
      return;
    }
  }
  // if all busy, steal the next slot
  play[nextPlayer].stop();
  play[nextPlayer].play(name);
  lastTrig = millis();
  nextPlayer = (nextPlayer + 1) % NPLAY;
}

float binHz(int bin) { return (44100.0f / 2.0f) * (float)bin / 1024.0f; }

// crude dominant-frequency estimator (magnitude max bin)
float dominantHz() {
  if (!fft.available()) return -1.0f;
  float maxMag = 0.0f;
  int maxBin = -1;
  for (int i = 2; i < 512; ++i) { // ignore DC/very low
    float m = fft.read(i);
    if (m > maxMag) { maxMag = m; maxBin = i; }
  }
  if (maxBin < 0) return -1.0f;
  return binHz(maxBin);
}

// ---------- SETUP ----------
void setup() {
  AudioMemory(80);

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);   // or AUDIO_INPUT_MIC
  codec.lineInLevel(10);                   // 0..15, adjust to avoid clipping
  codec.volume(0.6);                       // headphone/line out level

  // mixers
  for (int i = 0; i < 4; ++i) mixA.gain(i, 0.7);
  for (int i = 0; i < 4; ++i) mixB.gain(i, 0.7);
  mainMix.gain(0, 0.9);
  mainMix.gain(1, 0.9);
  mainMix.gain(2, 0.15);  // bleed some live guitar if you want it
  mainMix.gain(3, 0.0);

  inGain.gain(inputGain);

  // FFT config
  fft.windowFunction(AudioWindowHanning1024);

  // SD init
  if (!SD.begin(SD_CS)) {
    while (1) {
      // Stuck here = SD not found; check wiring/CS
      // (Leave as a tight loop so you notice quickly.)
    }
  }
}

// ---------- MAIN ----------
void loop() {
  // basic onset gate
  if (peak.available()) {
    float p = peak.read();
    if (p >= onsetThreshold && canRetrigger()) {
      // estimate frequency band
      float hz = dominantHz();

      // Decide which drum to fire.
      // You can tailor these to your current mapping (string/position/etc.).
      if (hz > 0) {
        if (hz <= kickMaxHz) {
          triggerFile(FILE_KICK);
        } else if (hz >= snareMinHz && hz <= snareMaxHz) {
          triggerFile(FILE_SNARE);
        } else if (hz >= tomMinHz && hz <= tomMaxHz) {
          // Pick tom1 or tom2 by sub-band
          triggerFile(hz < 150.0f ? FILE_TOM1 : FILE_TOM2);
        } else if (hz >= rideMinHz && hz <= rideMaxHz) {
          triggerFile(FILE_RIDE);
        } else if (hz >= crashMinHz && hz < hatMinHz) {
          triggerFile(FILE_CRASH);
        } else if (hz >= hatMinHz) {
          // very bright: closed vs open by overall level
          triggerFile(p > 0.12f ? FILE_HHOP : FILE_HHCL);
        } else {
          // Fallback if weird frequency: snare
          triggerFile(FILE_SNARE);
        }
      }
    }
  }
}
