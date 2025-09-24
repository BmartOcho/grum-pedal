// GUITAR -> DRUM SAMPLES (based on your working sketch)
// Replaces synth drums with SD WAV playback but keeps your exact band mapping.
//
// Hardware: Teensy 4.x + Audio Shield (Rev D) OR Teensy 4.1 (onboard SD)
// Input: Audio Shield LINE IN (same as your sketch)

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// ====== SD CONFIG ======
#if defined(ARDUINO_TEENSY41)
  const int SD_CS = BUILTIN_SDCARD;   // Teensy 4.1 onboard SD
#else
  const int SD_CS = 10;               // Audio Shield Rev D CS
#endif

// ====== FILE NAMES ======
const char* FILE_KICK  = "KICK.WAV";
const char* FILE_SNARE = "SNARE.WAV";
const char* FILE_HHCL  = "HHCL.WAV";
const char* FILE_RIDE  = "RIDE.WAV";
const char* FILE_CRASH = "CRASH.WAV";

// ======Frequency Smoothing (NEW) =====
const int FREQ_HISTORY_SIZE = 3; // Number of samples to average
float freqHistory[FREQ_HISTORY_SIZE];
int freqHistoryIndex = 0;
bool freqHistoryFilled = false;

// ====== AUDIO GRAPH ======
// Input / analysis (kept from your sketch)
AudioInputI2S             audioInput;      // Guitar input (Audio Shield)
AudioAnalyzeNoteFrequency notefreq;        // Note detection
AudioAnalyzePeak          peak;            // Peak detection

// SD WAV players (multiple so hits can overlap)
AudioPlaySdWav            wav1;
AudioPlaySdWav            wav2;
AudioPlaySdWav            wav3;
AudioPlaySdWav            wav4;
AudioPlaySdWav            wav5;
AudioPlaySdWav            wav6;

// Mixers for WAVs -> drum bus
AudioMixer4               drumMixA;        // up to 4 sources
AudioMixer4               drumMixB;        // up to 4 sources (we'll use 2 of them)
AudioMixer4               drumBus;         // sum A+B to one bus

// Final mix (kept from your sketch idea): guitar dry + drums + (extra slot if needed)
AudioMixer4               mainMixer;       
AudioOutputI2S            audioOutput;

AudioControlSGTL5000      audioShield;

// Patch cords (like your originals, adjusted for new graph)
AudioConnection patchCord1(audioInput, 0, notefreq, 0);
AudioConnection patchCord2(audioInput, 0, peak, 0);
AudioConnection patchCord3(audioInput, 0, mainMixer, 0);    // Dry guitar to main

// WAV players into the two submixers
AudioConnection patchCord4(wav1, 0, drumMixA, 0);
AudioConnection patchCord5(wav2, 0, drumMixA, 1);
AudioConnection patchCord6(wav3, 0, drumMixA, 2);
AudioConnection patchCord7(wav4, 0, drumMixA, 3);
AudioConnection patchCord8(wav5, 0, drumMixB, 0);
AudioConnection patchCord9(wav6, 0, drumMixB, 1);

// Submixers into drumBus
AudioConnection patchCord10(drumMixA, 0, drumBus, 0);
AudioConnection patchCord11(drumMixB, 0, drumBus, 1);

// Drum bus into main mix
AudioConnection patchCord12(drumBus, 0, mainMixer, 1);

// Main to outputs (stereo)
AudioConnection patchCord13(mainMixer, 0, audioOutput, 0);  // Left
AudioConnection patchCord14(mainMixer, 0, audioOutput, 1);  // Right

// ====== CONTROL ======
unsigned long lastTriggerTime[5] = {0};
const int retriggerDelay = 80;  // Minimum ms between same drum

// simple round-robin across the 6 players
AudioPlaySdWav* players[6] = { &wav1, &wav2, &wav3, &wav4, &wav5, &wav6 };
int nextPlayer = 0;

bool playSample(const char* fname) {
  // try to find a free player first
  for (int i = 0; i < 6; ++i) {
    int idx = (nextPlayer + i) % 6;
    if (!players[idx]->isPlaying()) {
      players[idx]->play(fname);
      nextPlayer = (idx + 1) % 6;
      return true;
    }
  }
  // steal the next slot if all busy
  players[nextPlayer]->stop();
  bool ok = players[nextPlayer]->play(fname);
  nextPlayer = (nextPlayer + 1) % 6;
  return ok;
}

bool canRetrigger(int drumIndex) {
  if (millis() - lastTriggerTime[drumIndex] > retriggerDelay) {
    lastTriggerTime[drumIndex] = millis();
    return true;
  }
  return false;
}

// ===== NEW FUNCTION: Smooth frequency readings =====
float getSmoothedFrequency(float newFreq) {
  // Add new frequency to history
  freqHistory[freqHistoryIndex] = newFreq;
  freqHistoryIndex = (freqHistoryIndex + 1) % FREQ_HISTORY_SIZE;

  // Mark history as filled once we've gone through it
  if (freqHistoryIndex == 0) {
    freqHistoryFilled = true;
  }

  // Calculate average
  float sum = 0;
  int count = freqHistoryFilled ? FREQ_HISTORY_SIZE : freqHistoryIndex;

  // Return raw value if we don't have enough samples yet
  if (count == 0) return newFreq;

  for (int i = 0; i < count; i++) {
    sum += freqHistory[i];
  }

  return sum / count;
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println("====================================");
  Serial.println("   GUITAR DRUM MACHINE - SD SAMPLES ");
  Serial.println("====================================");

  AudioMemory(80); // more buffers for SD playback

  // Initialize frequency history
  for (int i = 0; i < FREQ_HISTORY_SIZE; i++) {
    freqHistory[i] = 0;
  }

  // Codec / input (same approach as yours)
  audioShield.enable();
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);  // using line-in on Audio Shield
  audioShield.lineInLevel(10);                  // 0..15; 10 is a good start
  audioShield.volume(0.7);

  // Analysis
  notefreq.begin(0.05);

  // Mix gains
  for (int i = 0; i < 4; ++i) drumMixA.gain(i, 0.8);
  for (int i = 0; i < 4; ++i) drumMixB.gain(i, 0.8);
  drumBus.gain(0, 0.9);
  drumBus.gain(1, 0.9);

  mainMixer.gain(0, 0.0f);  // Dry guitar (set to taste; you had 0 before)
  mainMixer.gain(1, 0.9f);  // Drums
  mainMixer.gain(2, 0.0f);
  mainMixer.gain(3, 0.0f);

  // SD init
  Serial.print("Mounting SD... ");
  if (!SD.begin(SD_CS)) {
    Serial.println("FAILED");
    while (1) { /* halt so the issue is obvious */ }
  }
  Serial.println("OK");

  // Quick existence check (helps catch typos/format issues)
  const char* files[] = { FILE_KICK, FILE_SNARE, FILE_HHCL, FILE_RIDE, FILE_CRASH };
  for (auto f : files) {
    if (!SD.exists(f)) {
      Serial.print("⚠️ Missing file on SD: ");
      Serial.println(f);
    }
  }

  // Tiny demo to prove audio path
  delay(300);
  Serial.println("Playing short test (KICK->SNARE->HAT)...");
  playSample(FILE_KICK); delay(180);
  playSample(FILE_SNARE); delay(180);
  playSample(FILE_HHCL); delay(250);
  Serial.println("Ready! Play your guitar.");
}

// ====== MAIN ======
void loop() {
  if (notefreq.available() && peak.available()) {
    float freq = notefreq.read();
    float probability = notefreq.probability();
    float level = peak.read();

    // Same gate you used
    if (probability > 0.6 && level > 0.02) {
      // NEW: Use smoothed frequency instead of raw
      float smoothedFreq = getSmoothedFrequency(freq);
      triggerDrumForFrequency(smoothedFreq);

      // Optional debug: show both raw and smooth frequency
      if (false) { // Set to true to see the difference }
        Serial.print("Raw: ");
        Serial.print(freq, 1);
        Serial.print(" Hz, Smoothed: ");
        Serial.print(smoothedFreq, 1);
        Serial.print( "Hz");
      }
    }
  }
}

// ====== YOUR MAPPING, NOW TRIGGERING SAMPLES ======
void triggerDrumForFrequency(float freq) {
  // KICK (60-110 Hz)
  if (freq >= 60 && freq < 110) {
    if (canRetrigger(0)) {
      playSample(FILE_KICK);
      Serial.print("🥁 KICK! "); Serial.print(freq, 1); Serial.println(" Hz");
    }
  }
  // SNARE (110-165 Hz)
  else if (freq >= 110 && freq < 165) {
    if (canRetrigger(1)) {
      playSample(FILE_SNARE);
      Serial.print("🪘 SNARE! "); Serial.print(freq, 1); Serial.println(" Hz");
    }
  }
  // HI-HAT (165-260 Hz) -> closed hat sample
  else if (freq >= 165 && freq < 260) {
    if (canRetrigger(2)) {
      playSample(FILE_HHCL);
      Serial.print("🎩 HAT! "); Serial.print(freq, 1); Serial.println(" Hz");
    }
  }
  // RIDE (260-400 Hz)
  else if (freq >= 260 && freq < 400) {
    if (canRetrigger(3)) {
      playSample(FILE_RIDE);
      Serial.print("🔔 RIDE! "); Serial.print(freq, 1); Serial.println(" Hz");
    }
  }
  // CRASH (400+ Hz)
  else if (freq >= 400) {
    if (canRetrigger(4)) {
      playSample(FILE_CRASH);
      Serial.print("💥 CRASH! "); Serial.print(freq, 1); Serial.println(" Hz");
    }
  }
}
