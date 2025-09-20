// GUITAR DRUM TRIGGER - ADVANCED ONSET DETECTION VERSION
// Combines your working drum synthesis with advanced onset detection

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

// Audio signal flow - YOUR EXACT SETUP
AudioInputI2S             audioInput;      // Guitar input
AudioSynthSimpleDrum      drumKick;        // Kick drum sound
AudioSynthSimpleDrum      drumSnare;       // Snare drum sound  
AudioSynthSimpleDrum      drumHihat;       // Hi-hat sound
AudioSynthSimpleDrum      drumRide;        // Ride sound
AudioSynthSimpleDrum      drumCrash;       // Crash sound
AudioMixer4               drumMixer;       // Mix all drums
AudioMixer4               mainMixer;       // Mix drums + guitar
AudioOutputI2S            audioOutput;     // Output to amp

// Analysis objects for detection
AudioAnalyzeFFT256        fft;             // For frequency and onset detection
AudioAnalyzePeak          peak;            // Peak detection
AudioFilterBiquad         highpass;        // Remove DC offset

// Audio connections - keeping your exact output routing!
AudioConnection patchCord1(audioInput, 0, highpass, 0);
AudioConnection patchCord2(highpass, 0, fft, 0);
AudioConnection patchCord3(highpass, 0, peak, 0);
AudioConnection patchCord4(audioInput, 0, mainMixer, 0);  // Dry guitar
AudioConnection patchCord5(drumKick, 0, drumMixer, 0);
AudioConnection patchCord6(drumSnare, 0, drumMixer, 1);
AudioConnection patchCord7(drumHihat, 0, drumMixer, 2);
AudioConnection patchCord8(drumRide, 0, drumMixer, 3);
AudioConnection patchCord9(drumCrash, 0, mainMixer, 2);   // Crash direct
AudioConnection patchCord10(drumMixer, 0, mainMixer, 1);   // All drums
AudioConnection patchCord11(mainMixer, 0, audioOutput, 0); // Left out
AudioConnection patchCord12(mainMixer, 0, audioOutput, 1); // Right out

AudioControlSGTL5000 audioShield;

// Advanced onset detection variables
const int ENERGY_HISTORY_SIZE = 8;
float energyHistory[ENERGY_HISTORY_SIZE] = {0};
int energyHistoryIndex = 0;
float lastEnergy = 0;
float fftData[128];  // Store FFT data for reuse

// Control variables - your existing ones
unsigned long lastTriggerTime[5] = {0};
const int retriggerDelay = 80;  // Minimum ms between same drum
float lastPeakLevel = 0;

// Onset detection thresholds - MADE MORE SENSITIVE
float adaptiveThreshold = 0.001;     // Lowered from 0.01
float noiseFloor = 0.0001;           // Much lower noise floor
float THRESHOLD_MULTIPLIER = 1.2;    // Lower = more sensitive (was 1.8)
float HFC_THRESHOLD = 0.8;           // Lower HFC requirement (was 1.0)
float ENERGY_RATIO_THRESHOLD = 1.2;  // Lower ratio needed (was 1.5)

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("====================================");
  Serial.println("  GUITAR DRUMS - ADVANCED TRIGGER  ");
  Serial.println("====================================");
  Serial.println("Initializing...");
  
  // Audio setup - keeping your exact configuration
  AudioMemory(50);
  audioShield.enable();
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);
  audioShield.lineInLevel(10);     // INCREASED from 0 to 10 for more sensitivity
  audioShield.micGain(40);         // Boost for guitar
  audioShield.volume(0.7);         // Output volume
  
  // Configure high-pass filter
  highpass.setHighpass(0, 40, 0.7);
  
  // Initialize energy history
  for (int i = 0; i < ENERGY_HISTORY_SIZE; i++) {
    energyHistory[i] = 0.001;
  }
  
  // Configure drum sounds - YOUR EXACT SETTINGS
  setupDrumSounds();
  
  // Setup mixer levels - YOUR EXACT LEVELS
  drumMixer.gain(0, 0.7);   // Kick volume
  drumMixer.gain(1, 0.7);   // Snare volume
  drumMixer.gain(2, 0.5);   // Hihat volume
  drumMixer.gain(3, 0.5);   // Ride volume
  
  mainMixer.gain(0, 0);     // Dry guitar volume (0%)
  mainMixer.gain(1, 0.8);   // Drums volume
  mainMixer.gain(2, 0.6);   // Crash volume
  
  // Play startup drum sequence
  delay(500);
  Serial.println("Playing test drums...");
  drumKick.noteOn();
  delay(200);
  drumSnare.noteOn();
  delay(200);
  drumHihat.noteOn();
  delay(200);
  drumCrash.noteOn();
  delay(500);
  
  Serial.println("\n♪ Ready! Play your guitar! ♪");
  Serial.println("====================================\n");
}

// Calculate energy from FFT data
float calculateEnergy() {
  float energy = 0;
  for (int i = 0; i < 128; i++) {
    energy += fftData[i] * fftData[i];
  }
  energy = sqrt(energy / 128.0);
  return energy;
}

// Calculate High Frequency Content for pluck detection
float calculateHFC() {
  float hfc = 0;
  for (int i = 64; i < 128; i++) {
    float weight = (float)i / 128.0;
    hfc += fftData[i] * fftData[i] * weight;
  }
  hfc = sqrt(hfc / 64.0);
  return hfc;
}

// Update energy history
float updateEnergyHistory(float currentEnergy) {
  energyHistory[energyHistoryIndex] = currentEnergy;
  energyHistoryIndex = (energyHistoryIndex + 1) % ENERGY_HISTORY_SIZE;
  
  float avgHistory = 0;
  for (int i = 0; i < ENERGY_HISTORY_SIZE; i++) {
    avgHistory += energyHistory[i];
  }
  avgHistory /= ENERGY_HISTORY_SIZE;
  return avgHistory;
}

// Advanced onset detection
bool detectOnset(float level, float &velocity) {
  if (level < noiseFloor) return false;
  
  float currentEnergy = calculateEnergy();
  if (currentEnergy < 0.001) currentEnergy = level;
  
  float hfc = calculateHFC();
  float avgHistory = updateEnergyHistory(currentEnergy);
  adaptiveThreshold = avgHistory * THRESHOLD_MULTIPLIER + noiseFloor;
  
  float energyRatio = currentEnergy / (avgHistory + 0.001);
  float hfcRatio = hfc / (avgHistory + 0.001);
  
  // Check all conditions
  bool energyCondition = currentEnergy > adaptiveThreshold;
  bool ratioCondition = energyRatio > ENERGY_RATIO_THRESHOLD;
  bool hfcCondition = hfcRatio > HFC_THRESHOLD || hfc > 0.01;
  bool risingEdge = currentEnergy > lastEnergy * 1.2;
  
  bool isOnset = false;
  if (energyCondition && ratioCondition && risingEdge) {
    if (hfcCondition || currentEnergy > adaptiveThreshold * 3) {
      isOnset = true;
      velocity = constrain(currentEnergy * 2.0, 0.0, 1.0);
    }
  }
  
  lastEnergy = currentEnergy;
  return isOnset;
}

// Detect pitch from FFT data
float detectPitch() {
  float maxLevel = 0;
  int maxBin = 0;
  
  for (int i = 2; i < 128; i++) {
    if (fftData[i] > maxLevel) {
      maxLevel = fftData[i];
      maxBin = i;
    }
  }
  
  if (maxLevel < 0.01) return -1;
  
  // Simple frequency calculation
  float frequency = maxBin * 44100.0 / 256.0;
  return frequency;
}

// YOUR EXACT DRUM TRIGGERING FUNCTION - with velocity support via mixer
void triggerDrumForFrequency(float freq, float velocity) {
  // Adjust drum velocity based on input velocity
  float drumGain = velocity * 0.8 + 0.2;  // Scale velocity (0.2 to 1.0)
  
  // KICK DRUM (60-110 Hz) - Low E string area
  if (freq >= 60 && freq < 110) {
    if (canRetrigger(0)) {
      drumMixer.gain(0, 0.7 * drumGain);  // Adjust kick volume by velocity
      drumKick.noteOn();  // Trigger drum (no parameter)
      Serial.print("🥁 KICK! ");
      Serial.print(freq, 1);
      Serial.print(" Hz, Vel: ");
      Serial.println(velocity, 2);
    }
  }
  // SNARE DRUM (110-165 Hz) - A string area
  else if (freq >= 110 && freq < 165) {
    if (canRetrigger(1)) {
      drumMixer.gain(1, 0.7 * drumGain);  // Adjust snare volume by velocity
      drumSnare.noteOn();
      Serial.print("🪘 SNARE! ");
      Serial.print(freq, 1);
      Serial.print(" Hz, Vel: ");
      Serial.println(velocity, 2);
    }
  }
  // HI-HAT (165-260 Hz) - D & G string area
  else if (freq >= 165 && freq < 260) {
    if (canRetrigger(2)) {
      drumMixer.gain(2, 0.5 * drumGain);  // Adjust hihat volume by velocity
      drumHihat.noteOn();
      Serial.print("🎩 HAT! ");
      Serial.print(freq, 1);
      Serial.print(" Hz, Vel: ");
      Serial.println(velocity, 2);
    }
  }
  // RIDE CYMBAL (260-400 Hz) - B string area
  else if (freq >= 260 && freq < 400) {
    if (canRetrigger(3)) {
      drumMixer.gain(3, 0.5 * drumGain);  // Adjust ride volume by velocity
      drumRide.noteOn();
      Serial.print("🔔 RIDE! ");
      Serial.print(freq, 1);
      Serial.print(" Hz, Vel: ");
      Serial.println(velocity, 2);
    }
  }
  // CRASH CYMBAL (400+ Hz) - High E string upper frets
  else if (freq >= 400) {
    if (canRetrigger(4)) {
      mainMixer.gain(2, 0.6 * drumGain);  // Adjust crash volume by velocity (on main mixer)
      drumCrash.noteOn();
      Serial.print("💥 CRASH! ");
      Serial.print(freq, 1);
      Serial.print(" Hz, Vel: ");
      Serial.println(velocity, 2);
    }
  }
}

// YOUR EXACT DRUM SOUND SETUP
void setupDrumSounds() {
  // KICK - Deep and punchy (60Hz fundamental)
  drumKick.frequency(60);
  drumKick.length(150);
  drumKick.secondMix(0.0);
  drumKick.pitchMod(0.5);
  
  // SNARE - Snappy with noise (200Hz + noise)
  drumSnare.frequency(200);
  drumSnare.length(100);
  drumSnare.secondMix(1.0);
  drumSnare.pitchMod(0.2);
  
  // HI-HAT - Short and crisp (800Hz)
  drumHihat.frequency(800);
  drumHihat.length(40);
  drumHihat.secondMix(1.0);
  drumHihat.pitchMod(0.0);
  
  // RIDE - Metallic ring (500Hz)
  drumRide.frequency(500);
  drumRide.length(300);
  drumRide.secondMix(0.5);
  drumRide.pitchMod(0.1);
  
  // CRASH - Long and bright (900Hz)
  drumCrash.frequency(900);
  drumCrash.length(500);
  drumCrash.secondMix(1.0);
  drumCrash.pitchMod(0.0);
}

// YOUR EXACT RETRIGGER FUNCTION
bool canRetrigger(int drumIndex) {
  if (millis() - lastTriggerTime[drumIndex] > retriggerDelay) {
    lastTriggerTime[drumIndex] = millis();
    return true;
  }
  return false;
}

void loop() {
  // NEW: Advanced onset detection replacing notefreq
  if (fft.available() && peak.available()) {
    // Read FFT data once
    for (int i = 0; i < 128; i++) {
      fftData[i] = fft.read(i);
    }
    
    // Read peak level
    float level = peak.read();
    
    // Debug: Show actual signal levels
    static int debugCounter = 0;
    debugCounter++;
    if (debugCounter % 50 == 0) {  // Every 50 loops
      if (level > 0.0001) {  // Very low threshold for any signal
        Serial.print("Level: ");
        Serial.print(level, 5);
        Serial.print(" Energy: ");
        Serial.print(calculateEnergy(), 5);
        Serial.print(" Threshold: ");
        Serial.println(adaptiveThreshold, 5);
      } else {
        Serial.print(".");  // No signal dot
      }
    }
    
    // Check for onset with velocity
    float velocity = 0;
    bool onsetDetected = detectOnset(level, velocity);
    
    if (onsetDetected) {
      // Detect pitch from FFT
      float freq = detectPitch();
      
      Serial.print("Onset detected! Freq: ");
      Serial.println(freq);
      
      if (freq > 50 && freq < 1000) {  // Valid frequency range
        // Trigger the appropriate drum with velocity
        triggerDrumForFrequency(freq, velocity);
      }
    }
  }
  
  // Simple peak test - bypass onset detection for testing
  static unsigned long lastSimpleTrigger = 0;
  if (peak.available()) {
    float level = peak.read();
    // Very simple trigger just to test if we're getting audio
    if (level > 0.01 && (millis() - lastSimpleTrigger > 100)) {
      Serial.print("SIMPLE TRIGGER! Level: ");
      Serial.println(level, 4);
      drumKick.noteOn();  // Test kick drum
      lastSimpleTrigger = millis();
    }
  }
}

// Optional: Adjust sensitivity on the fly
void adjustSensitivity(float newMultiplier) {
  THRESHOLD_MULTIPLIER = constrain(newMultiplier, 1.0, 3.0);
  Serial.print("Sensitivity adjusted to: ");
  Serial.println(THRESHOLD_MULTIPLIER);
}