// GUITAR -> DRUM SAMPLES WITH TRUE BYPASS TOGGLE
// Footswitch toggles between:
//   - BYPASS: Clean guitar only (for recording guitar to looper)
//   - ACTIVE: Drums only (for recording drums to looper)
//
// Hardware: Teensy 4.0 + Audio Shield Rev D
// Input: Audio Shield LINE IN
// Output: Audio Shield LINE OUT (to looper/amp)
// Control: Momentary footswitch + LED indicator

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Bounce2.h>  // For debouncing

// ====== BYPASS CONTROL PINS (NEW) ======
const int FOOTSWITCH_PIN = 2;   // Digital pin 2 for footswitch (momentary, normally open)
const int LED_PIN = 3;          // Digital pin 3 for LED indicator
// Note: Pins 2 and 3 are safe to use on Teensy 4.0 with Audio Shield

// ====== BYPASS STATE (NEW) ======
bool bypassMode = true;          // Start in bypass mode (guitar only)
Bounce footswitch = Bounce();   // Debouncer for footswitch
unsigned long lastToggleTime = 0;
const int toggleDebounceTime = 50;  // Additional debounce protection

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

// ====== Frequency Smoothing ======
const int FREQ_HISTORY_SIZE = 3;
float freqHistory[FREQ_HISTORY_SIZE];
int freqHistoryIndex = 0;
bool freqHistoryFilled = false;

// ====== AUDIO GRAPH ======
// Input / analysis
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
AudioMixer4               drumMixB;        // up to 4 sources
AudioMixer4               drumBus;         // sum A+B to one bus

// Final mix: guitar dry + drums
AudioMixer4               mainMixer;       
AudioOutputI2S            lineOutput;      // Line out ONLY (no headphone code)

AudioControlSGTL5000      audioShield;

// Patch cords
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

// Main to LINE OUTPUT only (removed headphone monitoring)
AudioConnection patchCord13(mainMixer, 0, lineOutput, 0);  // Left
AudioConnection patchCord14(mainMixer, 0, lineOutput, 1);  // Right

// ====== CONTROL ======
unsigned long lastTriggerTime[5] = {0};
const int retriggerDelay = 80;

// Round-robin across the 6 players
AudioPlaySdWav* players[6] = { &wav1, &wav2, &wav3, &wav4, &wav5, &wav6 };
int nextPlayer = 0;

bool playSample(const char* fname) {
  // Don't play if in bypass mode
  if (bypassMode) return false;
  
  // Try to find a free player first
  for (int i = 0; i < 6; ++i) {
    int idx = (nextPlayer + i) % 6;
    if (!players[idx]->isPlaying()) {
      players[idx]->play(fname);
      nextPlayer = (idx + 1) % 6;
      return true;
    }
  }
  // Steal the next slot if all busy
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

// ===== Smooth frequency readings =====
float getSmoothedFrequency(float newFreq) {
  freqHistory[freqHistoryIndex] = newFreq;
  freqHistoryIndex = (freqHistoryIndex + 1) % FREQ_HISTORY_SIZE;
  
  if (freqHistoryIndex == 0) {
    freqHistoryFilled = true;
  }
  
  float sum = 0;
  int count = freqHistoryFilled ? FREQ_HISTORY_SIZE : freqHistoryIndex;
  
  if (count == 0) return newFreq;
  
  for (int i = 0; i < count; i++) {
    sum += freqHistory[i];
  }
  
  return sum / count;
}

// ====== NEW FUNCTION: Update audio routing based on bypass state ======
void updateBypassState() {
  // Use AudioNoInterrupts to prevent pops/clicks
  AudioNoInterrupts();
  
  if (bypassMode) {
    // BYPASS MODE: Guitar only, no drums
    mainMixer.gain(0, 1.0f);  // Guitar at full volume
    mainMixer.gain(1, 0.0f);  // Drums completely muted
    digitalWrite(LED_PIN, LOW);  // LED off
    Serial.println(">>> BYPASS MODE: Guitar only");
  } else {
    // ACTIVE MODE: Drums only, no guitar
    mainMixer.gain(0, 0.0f);  // Guitar completely muted
    mainMixer.gain(1, 0.9f);  // Drums at normal level
    digitalWrite(LED_PIN, HIGH);  // LED on
    Serial.println(">>> ACTIVE MODE: Drums only");
  }
  
  AudioInterrupts();
}

// ====== NEW FUNCTION: Handle footswitch ======
void checkFootswitch() {
  footswitch.update();
  
  // Check for press (transition from HIGH to LOW for normally open switch)
  if (footswitch.fell()) {
    // Additional debounce protection
    if (millis() - lastToggleTime > toggleDebounceTime) {
      lastToggleTime = millis();
      bypassMode = !bypassMode;  // Toggle state
      updateBypassState();
    }
  }
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println("=========================================");
  Serial.println(" GUITAR DRUM MACHINE - TRUE BYPASS MODE ");
  Serial.println("=========================================");
  Serial.println("Footswitch: Press to toggle modes");
  Serial.println("BYPASS: Guitar only -> Looper");
  Serial.println("ACTIVE: Drums only -> Looper");
  Serial.println();

  // Setup footswitch and LED pins (NEW)
  pinMode(FOOTSWITCH_PIN, INPUT_PULLUP);  // Internal pullup resistor
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // Start with LED off
  
  // Initialize debouncer (NEW)
  footswitch.attach(FOOTSWITCH_PIN);
  footswitch.interval(25);  // 25ms debounce time

  AudioMemory(80);

  // Initialize frequency history
  for (int i = 0; i < FREQ_HISTORY_SIZE; i++) {
    freqHistory[i] = 0;
  }

  // Audio Shield setup
  audioShield.enable();
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);
  audioShield.lineInLevel(10);  // 0..15; 10 is a good start
  
  // LINE OUT configuration (removed headphone code)
  audioShield.lineOutLevel(13);  // 13 = 3.16V p-p (31 = 1.16V p-p)
  // Note: Lower values = higher output. 13 is good for line level gear
  
  // Disable headphone output to reduce noise
  audioShield.volume(0);  // Mute headphones completely

  // Analysis
  notefreq.begin(0.05);

  // Mix gains for drum submixers
  for (int i = 0; i < 4; ++i) drumMixA.gain(i, 0.8);
  for (int i = 0; i < 4; ++i) drumMixB.gain(i, 0.8);
  drumBus.gain(0, 0.9);
  drumBus.gain(1, 0.9);

  // Initial bypass state (starts in BYPASS mode)
  mainMixer.gain(0, 1.0f);  // Guitar ON
  mainMixer.gain(1, 0.0f);  // Drums OFF
  mainMixer.gain(2, 0.0f);  // Unused
  mainMixer.gain(3, 0.0f);  // Unused

  // SD init
  Serial.print("Mounting SD... ");
  if (!SD.begin(SD_CS)) {
    Serial.println("FAILED");
    while (1) { /* halt */ }
  }
  Serial.println("OK");

  // Check for drum sample files
  const char* files[] = { FILE_KICK, FILE_SNARE, FILE_HHCL, FILE_RIDE, FILE_CRASH };
  for (auto f : files) {
    if (!SD.exists(f)) {
      Serial.print("⚠️ Missing file on SD: ");
      Serial.println(f);
    }
  }

  // Test drums (only plays if you toggle to active mode)
  delay(300);
  Serial.println("System ready!");
  Serial.println("Currently in: BYPASS MODE (Guitar only)");
  Serial.println("Press footswitch to toggle between modes");
  Serial.println();
}

// ====== MAIN LOOP ======
void loop() {
  // Check footswitch for mode changes (NEW)
  checkFootswitch();
  
  // Only process drum triggers when in ACTIVE mode
  if (!bypassMode) {
    if (notefreq.available() && peak.available()) {
      float freq = notefreq.read();
      float probability = notefreq.probability();
      float level = peak.read();

      // Gate threshold
      if (probability > 0.6 && level > 0.02) {
        float smoothedFreq = getSmoothedFrequency(freq);
        triggerDrumForFrequency(smoothedFreq);
      }
    }
  }
}

// ====== DRUM TRIGGERING ======
void triggerDrumForFrequency(float freq) {
  // KICK (60-110 Hz)
  if (freq >= 60 && freq < 110) {
    if (canRetrigger(0)) {
      playSample(FILE_KICK);
      Serial.print("🥁 KICK! "); 
      Serial.print(freq, 1); 
      Serial.println(" Hz");
    }
  }
  // SNARE (110-165 Hz)
  else if (freq >= 110 && freq < 165) {
    if (canRetrigger(1)) {
      playSample(FILE_SNARE);
      Serial.print("🪘 SNARE! "); 
      Serial.print(freq, 1); 
      Serial.println(" Hz");
    }
  }
  // HI-HAT (165-260 Hz)
  else if (freq >= 165 && freq < 260) {
    if (canRetrigger(2)) {
      playSample(FILE_HHCL);
      Serial.print("🎩 HAT! "); 
      Serial.print(freq, 1); 
      Serial.println(" Hz");
    }
  }
  // RIDE (260-400 Hz)
  else if (freq >= 260 && freq < 400) {
    if (canRetrigger(3)) {
      playSample(FILE_RIDE);
      Serial.print("🔔 RIDE! "); 
      Serial.print(freq, 1); 
      Serial.println(" Hz");
    }
  }
  // CRASH (400+ Hz)
  else if (freq >= 400) {
    if (canRetrigger(4)) {
      playSample(FILE_CRASH);
      Serial.print("💥 CRASH! "); 
      Serial.print(freq, 1); 
      Serial.println(" Hz");
    }
  }
}

// ====== WIRING GUIDE ======
/*
 * FOOTSWITCH WIRING (9-pin using 3 pins for momentary switch):
 * - Pin 1: Connect to Teensy GND
 * - Pin 2: Connect to Teensy Digital Pin 2
 * - Pin 3: Not used (or second GND for stability)
 * - Switch type: Momentary, normally open
 * 
 * LED INDICATOR WIRING:
 * - LED Anode (+): Connect to Teensy Digital Pin 3 through 220-470 ohm resistor
 * - LED Cathode (-): Connect to Teensy GND
 * 
 * AUDIO CONNECTIONS:
 * - Guitar Input: Audio Shield LINE IN (not MIC in)
 * - Output: Audio Shield LINE OUT -> Looper -> Amp
 * - No headphone connection needed
 * 
 * POWER:
 * - Teensy powered via USB or 5V supply
 * - Audio Shield powered from Teensy
 */