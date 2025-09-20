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

// Add these variables for improved pitch detection
float pitchHistory[3] = {0};  // Store last 3 pitch detections
int pitchHistoryIndex = 0;
float zcrBuffer[128];  // Buffer for zero-crossing rate calculation
int zcrBufferIndex = 0;

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
  
  // Configure high-pass filter - ADJUSTED for better bass response
  highpass.setHighpass(0, 30, 0.5);  // Lowered to 30Hz, less resonance
  
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

// Zero-crossing rate calculation for frequency validation
float calculateZCR(float* buffer, int length) {
  int crossings = 0;
  for (int i = 1; i < length; i++) {
    // Count zero crossings
    if ((buffer[i-1] >= 0 && buffer[i] < 0) || 
        (buffer[i-1] < 0 && buffer[i] >= 0)) {
      crossings++;
    }
  }
  // Convert to frequency estimate (crossings per sample * sample rate / 2)
  float zcrFreq = (crossings * 44100.0) / (length * 2.0);
  return zcrFreq;
}

// Find harmonic peaks in FFT data
bool findHarmonicPeaks(int* peaks, float* levels, int maxPeaks) {
  // Find local maxima in FFT data
  int peakCount = 0;
  
  for (int i = 1; i < 30 && peakCount < maxPeaks; i++) {
    // Check if this bin is a local maximum
    if (i < 127 && fftData[i] > fftData[i-1] && 
        fftData[i] > fftData[i+1] && 
        fftData[i] > 0.002) {
      peaks[peakCount] = i;
      levels[peakCount] = fftData[i];
      peakCount++;
    }
  }
  
  return peakCount > 0;
}

// Detect pitch with harmonic analysis and validation
float detectPitchWithHarmonics() {
  int peaks[8];
  float levels[8];
  
  // Find peaks in spectrum
  if (!findHarmonicPeaks(peaks, levels, 8)) {
    return -1;  // No significant peaks
  }
  
  // Find fundamental by looking for harmonic relationships
  float bestFundamental = -1;
  float bestScore = 0;
  
  // Test each peak as potential fundamental
  for (int f = 0; f < 3; f++) {  // Check first 3 peaks
    if (peaks[f] == 0) continue;
    
    float fundamental = peaks[f] * 172.27;  // Convert bin to Hz
    float score = levels[f];  // Start with peak strength
    
    // Look for harmonics of this fundamental
    for (int h = 2; h <= 4; h++) {  // Check 2nd, 3rd, 4th harmonics
      int harmonicBin = peaks[f] * h;
      if (harmonicBin < 30) {
        // Check if there's energy at the harmonic
        float harmonicEnergy = fftData[harmonicBin];
        if (harmonicBin > 0 && harmonicBin < 128) {
          // Also check neighboring bins (not perfect tuning)
          harmonicEnergy = max(harmonicEnergy, fftData[harmonicBin-1]);
          if (harmonicBin < 127) {
            harmonicEnergy = max(harmonicEnergy, fftData[harmonicBin+1]);
          }
        }
        
        // Add to score if harmonic is present
        if (harmonicEnergy > 0.001) {
          score += harmonicEnergy * (1.0 / h);  // Weight lower harmonics more
        }
      }
    }
    
    // Update best if this is better
    if (score > bestScore && fundamental >= 60 && fundamental <= 800) {
      bestScore = score;
      bestFundamental = fundamental;
    }
  }
  
  return bestFundamental;
}

// Multi-frame pitch averaging for stability
float getStablePitch() {
  float currentPitch = detectPitchWithHarmonics();
  
  // Store in history
  pitchHistory[pitchHistoryIndex] = currentPitch;
  pitchHistoryIndex = (pitchHistoryIndex + 1) % 3;
  
  // Check if we have consistent pitch over multiple frames
  int validCount = 0;
  float avgPitch = 0;
  
  for (int i = 0; i < 3; i++) {
    if (pitchHistory[i] > 0) {
      validCount++;
      avgPitch += pitchHistory[i];
    }
  }
  
  if (validCount < 2) {
    return -1;  // Need at least 2 valid readings
  }
  
  avgPitch /= validCount;
  
  // Check that all valid readings are within 10% of average
  for (int i = 0; i < 3; i++) {
    if (pitchHistory[i] > 0) {
      float deviation = abs(pitchHistory[i] - avgPitch) / avgPitch;
      if (deviation > 0.1) {  // More than 10% deviation
        return -1;  // Pitch not stable
      }
    }
  }
  
  // Debug output
  static int debugCount = 0;
  if (++debugCount % 5 == 0 && avgPitch > 0) {
    Serial.print("Stable pitch: ");
    Serial.print(avgPitch);
    Serial.print(" Hz (");
    Serial.print(validCount);
    Serial.println(" frames)");
  }
  
  return avgPitch;
}

void loop() {
  // NEW: Advanced onset detection with improved pitch detection
  if (fft.available() && peak.available()) {
    // Read FFT data once
    for (int i = 0; i < 128; i++) {
      fftData[i] = fft.read(i);
    }
    
    // Read peak level
    float level = peak.read();
    
    // Check for onset with velocity
    float velocity = 0;
    bool onsetDetected = detectOnset(level, velocity);
    
    if (onsetDetected) {
      // Use multi-frame averaged pitch detection with harmonic analysis
      float freq = getStablePitch();
      
      if (freq > 0) {  // Valid, stable frequency detected
        // Trigger the appropriate drum with velocity
        triggerDrumForFrequency(freq, velocity);
      } else {
        // Improved fallback using harmonic content analysis
        // Look for presence of harmonics to determine drum type
        float fundamental = detectPitchWithHarmonics();
        
        if (fundamental > 0) {
          // Got a fundamental but not stable over time
          Serial.print("Unstable pitch: ");
          Serial.print(fundamental);
          Serial.println(" Hz - using best guess");
          triggerDrumForFrequency(fundamental, velocity);
        } else {
          // No clear pitch - use energy distribution
          float lowEnergy = 0;   // Bins 1-3 (80-250 Hz)
          float midEnergy = 0;   // Bins 4-8 (250-500 Hz)
          float highEnergy = 0;  // Bins 9+ (500+ Hz)
          
          for (int i = 1; i <= 3; i++) lowEnergy += fftData[i];
          for (int i = 4; i <= 8; i++) midEnergy += fftData[i];
          for (int i = 9; i <= 20; i++) highEnergy += fftData[i];
          
          // Only show fallback if we have significant energy
          if (lowEnergy + midEnergy + highEnergy > 0.01) {
            Serial.print("Energy fallback - L:");
            Serial.print(lowEnergy, 3);
            Serial.print(" M:");
            Serial.print(midEnergy, 3);
            Serial.print(" H:");
            Serial.println(highEnergy, 3);
            
            // Trigger based on energy distribution
            if (lowEnergy > midEnergy * 1.5 && lowEnergy > highEnergy * 1.5) {
              drumMixer.gain(0, 0.7 * velocity);
              drumKick.noteOn();
              Serial.println("→ KICK");
            } else if (midEnergy > highEnergy * 1.2) {
              drumMixer.gain(1, 0.7 * velocity);
              drumSnare.noteOn();
              Serial.println("→ SNARE");
            } else {
              drumMixer.gain(2, 0.5 * velocity);
              drumHihat.noteOn();
              Serial.println("→ HAT");
            }
          }
        }
      }
    }
  }
  
  // Status indicator
  static unsigned long lastStatusTime = 0;
  static int dotCount = 0;
  if (millis() - lastStatusTime > 100) {
    if (peak.available()) {
      float level = peak.read();
      if (level > 0.001) {
        dotCount++;
        if (dotCount >= 10) {
          Serial.print("♪");
          dotCount = 0;
        }
      }
    }
    lastStatusTime = millis();
  }
}

// Optional: Adjust sensitivity on the fly
void adjustSensitivity(float newMultiplier) {
  THRESHOLD_MULTIPLIER = constrain(newMultiplier, 1.0, 3.0);
  Serial.print("Sensitivity adjusted to: ");
  Serial.println(THRESHOLD_MULTIPLIER);
}