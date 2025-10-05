// ===============================================
//  GRUM Pedal - True Bypass Software Routing
//  Modified to include toggleable bypass system
//  Hardware: Teensy 4.0 + Audio Shield
// ===============================================

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Bounce.h>

// ============================================================
// == BYPASS SYSTEM CONFIGURATION ==
// ============================================================
#define FOOTSWITCH_PIN 2     // Footswitch connected here
#define LED_PIN 13           // Onboard LED (or external)
#define DEBOUNCE_TIME 15     // ms debounce delay

bool isActive = false;       // false = bypass, true = active
Bounce footswitch = Bounce(FOOTSWITCH_PIN, DEBOUNCE_TIME);

// ============================================================
// == AUDIO SYSTEM ==
// ============================================================
AudioInputI2S            i2s_in;           // Guitar input (line-in)
AudioMixer4              mixerGuitar;      // For routing clean guitar
AudioMixer4              mixerDrums;       // For routing drums
AudioOutputI2S           i2s_out;          // Line-out to amp/looper
AudioConnection*          patchCordGuitarToOut;
AudioConnection*          patchCordDrumsToOut;

// Existing audio objects (keep your onset detection, filters, and drum triggers here)
// For example:
// AudioFilterHighpass hp1;
// AudioAnalyzePeak peak1;
// AudioPlaySdWav drumKick;
// ...

AudioControlSGTL5000 audioShield;

// ============================================================
// == SETUP ==
// ============================================================
void setup() {
  pinMode(FOOTSWITCH_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  AudioMemory(60);
  audioShield.enable();
  audioShield.volume(0.8);

  // Initialize mix routing
  // Mixer 1: Clean Guitar Path
  mixerGuitar.gain(0, 1.0); // default pass-through
  // Mixer 2: Drum Path
  mixerDrums.gain(0, 0.0);  // muted by default

  // Routing
  patchCordGuitarToOut = new AudioConnection(mixerGuitar, 0, i2s_out, 0);
  patchCordDrumsToOut  = new AudioConnection(mixerDrums, 0, i2s_out, 1);

  // If you had headphone code, remove it completely
}

// ============================================================
// == LOOP ==
// ============================================================
void loop() {
  footswitch.update();
  if (footswitch.fallingEdge()) { // Toggle on press
    isActive = !isActive;
    digitalWrite(LED_PIN, isActive ? HIGH : LOW);

    if (isActive) {
      // ACTIVE MODE: Drums only
      fadeMixerGain(mixerGuitar, 0, 0.0);
      fadeMixerGain(mixerDrums, 0, 1.0);
    } else {
      // BYPASS MODE: Clean guitar only
      fadeMixerGain(mixerDrums, 0, 0.0);
      fadeMixerGain(mixerGuitar, 0, 1.0);
    }
  }

  // Keep all your existing onset detection + drum triggering code here
  // No change needed — just ensure the triggered drum sounds go to mixerDrums
}

// ============================================================
// == FADE FUNCTION TO PREVENT POPS ==
// ============================================================
void fadeMixerGain(AudioMixer4 &mixer, int channel, float targetGain) {
  float current = mixer.gain(channel);
  float step = (targetGain > current) ? 0.05 : -0.05;
  while (fabs(current - targetGain) > 0.01) {
    current += step;
    mixer.gain(channel, constrain(current, 0.0, 1.0));
    delay(5);
  }
  mixer.gain(channel, targetGain);
}

// ============================================================
// == NOTES ==
// ============================================================
// - When isActive == false (bypass): guitar path -> line out only
// - When isActive == true (active): drums -> line out only
// - LED ON in active mode
// - Smooth transitions to avoid clicks
// - You can adjust fade speed or debounce time if needed
// - Headphone code removed for cleaner routing
