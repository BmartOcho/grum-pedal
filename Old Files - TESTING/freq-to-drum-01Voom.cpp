// GUITAR DRUM TRIGGER - SIMPLIFIED VERSION
// Just guitar in, drums out. No extra hardware needed!

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

// Audio signal flow
AudioInputI2S             audioInput;      // Guitar input
AudioSynthSimpleDrum      drumKick;        // Kick drum sound
AudioSynthSimpleDrum      drumSnare;       // Snare drum sound  
AudioSynthSimpleDrum      drumHihat;       // Hi-hat sound
AudioSynthSimpleDrum      drumRide;        // Ride sound
AudioSynthSimpleDrum      drumCrash;       // Crash sound
AudioMixer4               drumMixer;       // Mix all drums
AudioMixer4               mainMixer;       // Mix drums + guitar
AudioOutputI2S            audioOutput;     // Output to amp
AudioAnalyzeNoteFrequency notefreq;        // Note detection
AudioAnalyzePeak          peak;            // Peak detection

// Audio connections (like patch cables!)
AudioConnection patchCord1(audioInput, 0, notefreq, 0);
AudioConnection patchCord2(audioInput, 0, peak, 0);
AudioConnection patchCord3(audioInput, 0, mainMixer, 0);  // Dry guitar
AudioConnection patchCord4(drumKick, 0, drumMixer, 0);
AudioConnection patchCord5(drumSnare, 0, drumMixer, 1);
AudioConnection patchCord6(drumHihat, 0, drumMixer, 2);
AudioConnection patchCord7(drumRide, 0, drumMixer, 3);
AudioConnection patchCord8(drumCrash, 0, mainMixer, 2);   // Crash direct
AudioConnection patchCord9(drumMixer, 0, mainMixer, 1);    // All drums
AudioConnection patchCord10(mainMixer, 0, audioOutput, 0); // Left out
AudioConnection patchCord11(mainMixer, 0, audioOutput, 1); // Right out

AudioControlSGTL5000 audioShield;

// Control variables
unsigned long lastTriggerTime[5] = {0};
// Different retrigger delays for each drum type
const int retriggerDelays[5] = {100, 80, 40, 120, 200}; // kick, snare, hihat, ride, crash
// Different level thresholds for each drum type  
const float drumThresholds[5] = {0.04, 0.03, 0.02, 0.025, 0.035}; // kick needs stronger hit
float lastPeakLevel = 0;
bool noteIsRinging = false;
unsigned long noteStartTime = 0;
// Store base gain levels for velocity scaling
const float baseGains[5] = {0.5, 0.5, 0.3, 0.3, 0.5};

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("====================================");
  Serial.println("   GUITAR DRUM MACHINE - SIMPLE    ");
  Serial.println("====================================");
  Serial.println("Initializing...");
  
  // Audio setup
  AudioMemory(50);
  audioShield.enable();
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);
  audioShield.lineInLevel(0);      // Most sensitive
  audioShield.micGain(40);         // Boost for guitar
  audioShield.volume(0.7);         // Output volume
  
  // Initialize note detection
  notefreq.begin(0.05);
  
  // Configure drum sounds
  setupDrumSounds();
  
  // Setup mixer levels
  drumMixer.gain(0, 0.5);   // Kick volume
  drumMixer.gain(1, 0.5);   // Snare volume
  drumMixer.gain(2, 0.3);   // Hihat volume
  drumMixer.gain(3, 0.3);   // Ride volume
  
  mainMixer.gain(0, 0.3);   // Dry guitar volume (30%)
  mainMixer.gain(1, 0.7);   // Drums volume (70%)
  mainMixer.gain(2, 0.5);   // Crash volume
  
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

void loop() {
  // Check for notes and trigger drums
  if (notefreq.available() && peak.available()) {
    float freq = notefreq.read();
    float probability = notefreq.probability();
    float level = peak.read();
    
    // Determine which drum this frequency maps to
    int drumType = -1;
    if (freq >= 60 && freq < 110) drumType = 0;      // Kick
    else if (freq >= 110 && freq < 165) drumType = 1; // Snare
    else if (freq >= 165 && freq < 260) drumType = 2; // Hihat
    else if (freq >= 260 && freq < 400) drumType = 3; // Ride
    else if (freq >= 400) drumType = 4;               // Crash
    
    // Detect NEW note attacks only (not sustained notes)
    bool isNewAttack = false;
    
    // Use drum-specific threshold for attack detection
    float attackThreshold = (drumType >= 0) ? drumThresholds[drumType] : 0.03;
    
    // If signal is stronger than before AND exceeds drum-specific threshold
    if (level > lastPeakLevel * 1.5 && level > attackThreshold) {
      isNewAttack = true;
      noteIsRinging = true;
      noteStartTime = millis();
    }
    
    // If we've been ringing for a while and level drops, reset
    if (noteIsRinging && level < 0.01) {
      noteIsRinging = false;
    }
    
    // Only trigger on NEW attacks with good confidence
    if (isNewAttack && probability > 0.7 && drumType >= 0) {
      triggerDrumForFrequency(freq, level);
    }
    
    lastPeakLevel = level;
  }
}

void triggerDrumForFrequency(float freq, float velocity) {
  // Calculate velocity multiplier (0.3 to 1.0 range)
  float velocityMultiplier = map(velocity * 100, 2, 20, 30, 100) / 100.0;
  velocityMultiplier = constrain(velocityMultiplier, 0.3, 1.0);
  
  // KICK DRUM (60-110 Hz) - Low E string area
  if (freq >= 60 && freq < 110) {
    if (canRetrigger(0)) {
      // Apply velocity to mixer gain before triggering
      drumMixer.gain(0, baseGains[0] * velocityMultiplier);
      drumKick.noteOn();
      Serial.print("🥁 KICK! ");
      Serial.print(freq, 1);
      Serial.print(" Hz [vel:");
      Serial.print(velocityMultiplier * 100, 0);
      Serial.println("%]");
    }
  }
  // SNARE DRUM (110-165 Hz) - A string area
  else if (freq >= 110 && freq < 165) {
    if (canRetrigger(1)) {
      drumMixer.gain(1, baseGains[1] * velocityMultiplier);
      drumSnare.noteOn();
      Serial.print("🪘 SNARE! ");
      Serial.print(freq, 1);
      Serial.print(" Hz [vel:");
      Serial.print(velocityMultiplier * 100, 0);
      Serial.println("%]");
    }
  }
  // HI-HAT (165-260 Hz) - D & G string area
  else if (freq >= 165 && freq < 260) {
    if (canRetrigger(2)) {
      drumMixer.gain(2, baseGains[2] * velocityMultiplier);
      drumHihat.noteOn();
      Serial.print("🎩 HAT! ");
      Serial.print(freq, 1);
      Serial.print(" Hz [vel:");
      Serial.print(velocityMultiplier * 100, 0);
      Serial.println("%]");
    }
  }
  // RIDE CYMBAL (260-400 Hz) - B string area
  else if (freq >= 260 && freq < 400) {
    if (canRetrigger(3)) {
      drumMixer.gain(3, baseGains[3] * velocityMultiplier);
      drumRide.noteOn();
      Serial.print("🔔 RIDE! ");
      Serial.print(freq, 1);
      Serial.print(" Hz [vel:");
      Serial.print(velocityMultiplier * 100, 0);
      Serial.println("%]");
    }
  }
  // CRASH CYMBAL (400+ Hz) - High E string upper frets
  else if (freq >= 400) {
    if (canRetrigger(4)) {
      mainMixer.gain(2, 0.5 * velocityMultiplier); // Crash on separate channel
      drumCrash.noteOn();
      Serial.print("💥 CRASH! ");
      Serial.print(freq, 1);
      Serial.print(" Hz [vel:");
      Serial.print(velocityMultiplier * 100, 0);
      Serial.println("%]");
    }
  }
}

void setupDrumSounds() {
  // KICK - Deep and punchy (60Hz fundamental)
  drumKick.frequency(60);
  drumKick.length(150);
  drumKick.secondMix(0.0);
  drumKick.pitchMod(0.5);
  
  // SNARE - Snappy with noise (200Hz + noise)
  drumSnare.frequency(200);
  drumSnare.length(100);
  drumSnare.secondMix(1.0);  // Add noise for "snare" sound
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

bool canRetrigger(int drumIndex) {
  // Use drum-specific retrigger delay
  if (millis() - lastTriggerTime[drumIndex] > retriggerDelays[drumIndex]) {
    lastTriggerTime[drumIndex] = millis();
    return true;
  }
  return false;
}