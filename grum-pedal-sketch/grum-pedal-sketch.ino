#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <ResponsiveAnalogRead.h>

// Your existing audio setup - keeping it exactly as you have it
AudioInputI2S            audioInput;
AudioSynthSimpleDrum      drumKick;        // Kick drum sound
AudioSynthSimpleDrum      drumSnare;       // Snare drum sound  
AudioSynthSimpleDrum      drumHihat;       // Hi-hat sound
AudioSynthSimpleDrum      drumRide;        // Ride sound
AudioSynthSimpleDrum      drumCrash;       // Crash sound
AudioMixer4               drumMixer;       // Mix all drums
AudioMixer4               mainMixer;       // Mix drums + guitar
AudioAnalyzeFFT256        fft;
AudioAnalyzePeak          peak;
AudioFilterBiquad         highpass;
AudioOutputI2S            audioOutput;     // Output to amp

AudioConnection patchCord1(audioInput, 0, highpass, 0);
AudioConnection patchCord2(highpass, 0, fft, 0);
AudioConnection patchCord3(highpass, 0, peak, 0);
AudioConnection patchCord4(drumKick, 0, drumMixer, 0);
AudioConnection patchCord5(drumSnare, 0, drumMixer, 1);
AudioConnection patchCord8(drumCrash, 0, mainMixer, 2);   // Crash direct
AudioConnection patchCord9(drumMixer, 0, mainMixer, 1);    // All drums
AudioConnection patchCord10(mainMixer, 0, audioOutput, 0); // Left out
AudioConnection patchCord11(mainMixer, 0, audioOutput, 1); // Right out

AudioControlSGTL5000     audioShield;


// Advanced onset detection variables
const int ENERGY_HISTORY_SIZE = 8;
float energyHistory[ENERGY_HISTORY_SIZE] = {0};
int energyHistoryIndex = 0;
float lastEnergy = 0;
float fftData[128];  // Store FFT data for reuse


int noteSamples[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
float lastPeakLevel = 0;
unsigned long lastTriggerTime = 0;
const unsigned int RETRIGGER_TIME = 50;
bool noteActive = false;
int lastTriggeredNote = -1;

// New adaptive threshold variables
float adaptiveThreshold = 0.01;
float noiseFloor = 0.001;
float THRESHOLD_MULTIPLIER = 2.0;  // Lowered for better sensitivity
float HFC_THRESHOLD = 1.2;         // Lowered for better sensitivity
float ENERGY_RATIO_THRESHOLD = 1.5; // Lowered for better sensitivity

void setup() {
    Serial.begin(115200);
    delay(1000);  // Give serial time to initialize
    
    AudioMemory(12);
    
    audioShield.enable();
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.lineInLevel(5);  // Increased sensitivity (0-15, where 5 is default)
    audioShield.volume(0.5);
    
    // Configure high-pass filter to remove DC and low frequency noise
    highpass.setHighpass(0, 40, 0.7);  // 40 Hz cutoff
    
    // Initialize energy history with small values to avoid division by zero
    for (int i = 0; i < ENERGY_HISTORY_SIZE; i++) {
        energyHistory[i] = 0.001;
    }
    
    Serial.println("Hybrid Guitar/Bass Trigger Ready");
    Serial.println("Waiting for input...");
}

// Calculate current frame energy from stored FFT data
float calculateEnergy() {
    float energy = 0;
    
    // Use stored FFT data
    for (int i = 0; i < 128; i++) {
        energy += fftData[i] * fftData[i];
    }
    energy = sqrt(energy / 128.0);
    
    return energy;
}

// Calculate High Frequency Content (HFC) from stored FFT data
float calculateHFC() {
    float hfc = 0;
    
    // Weight higher frequency bins more heavily
    for (int i = 64; i < 128; i++) {  // Upper half of spectrum
        float weight = (float)i / 128.0;  // Linear weighting
        hfc += fftData[i] * fftData[i] * weight;
    }
    hfc = sqrt(hfc / 64.0);
    
    return hfc;
}

// Update energy history and calculate average
float updateEnergyHistory(float currentEnergy) {
    energyHistory[energyHistoryIndex] = currentEnergy;
    energyHistoryIndex = (energyHistoryIndex + 1) % ENERGY_HISTORY_SIZE;
    
    // Calculate average of history
    float avgHistory = 0;
    for (int i = 0; i < ENERGY_HISTORY_SIZE; i++) {
        avgHistory += energyHistory[i];
    }
    avgHistory /= ENERGY_HISTORY_SIZE;
    
    return avgHistory;
}

// Advanced onset detection
bool detectOnset(float level, float &velocity) {
    // First check if level is above noise floor
    if (level < noiseFloor) {
        return false;
    }
    
    // Calculate current energy from FFT data
    float currentEnergy = calculateEnergy();
    
    // If no FFT energy, fall back to peak level
    if (currentEnergy < 0.001) {
        currentEnergy = level;
    }
    
    // Calculate HFC for pluck detection
    float hfc = calculateHFC();
    
    // Update history and get average
    float avgHistory = updateEnergyHistory(currentEnergy);
    
    // Update adaptive threshold
    adaptiveThreshold = avgHistory * THRESHOLD_MULTIPLIER + noiseFloor;
    
    // Calculate energy increase ratio
    float energyRatio = currentEnergy / (avgHistory + 0.001);  // Avoid division by zero
    
    // Calculate HFC ratio
    float hfcRatio = hfc / (avgHistory + 0.001);
    
    // Check retrigger time first to avoid unnecessary processing
    if ((millis() - lastTriggerTime) < RETRIGGER_TIME) {
        lastEnergy = currentEnergy;
        return false;
    }
    
    // Multi-factor onset detection
    bool isOnset = false;
    
    // Check all conditions for onset - with some conditions being optional
    bool energyCondition = currentEnergy > adaptiveThreshold;
    bool ratioCondition = energyRatio > ENERGY_RATIO_THRESHOLD;
    bool hfcCondition = hfcRatio > HFC_THRESHOLD || hfc > 0.01;  // More lenient HFC
    bool risingEdge = currentEnergy > lastEnergy * 1.2;  // Lowered from 1.3
    
    // Combine conditions - make HFC optional for very strong signals
    if (energyCondition && ratioCondition && risingEdge) {
        if (hfcCondition || currentEnergy > adaptiveThreshold * 3) {  // Strong signal bypasses HFC
            isOnset = true;
            
            // Calculate velocity based on energy
            velocity = currentEnergy * 2.0;
            velocity = constrain(velocity, 0.0, 1.0);
            
            // Debug output
            Serial.print("ONSET! Energy: ");
            Serial.print(currentEnergy, 4);
            Serial.print(" Ratio: ");
            Serial.print(energyRatio, 2);
            Serial.print(" HFC: ");
            Serial.print(hfcRatio, 2);
            Serial.print(" Velocity: ");
            Serial.println(velocity, 2);
        }
    }
    
    lastEnergy = currentEnergy;
    return isOnset;
}

// Your existing pitch detection using stored FFT data
float detectPitch() {
    float maxLevel = 0;
    int maxBin = 0;
    
    // Find the peak frequency bin
    for (int i = 2; i < 128; i++) {  // Skip DC and very low frequencies
        if (fftData[i] > maxLevel) {
            maxLevel = fftData[i];
            maxBin = i;
        }
    }
    
    // Need minimum signal strength
    if (maxLevel < 0.01) return -1;
    
    // Parabolic interpolation for more accurate frequency
    if (maxBin > 0 && maxBin < 127) {
        float y1 = fftData[maxBin - 1];
        float y2 = fftData[maxBin];
        float y3 = fftData[maxBin + 1];
        
        if (y2 > y1 && y2 > y3) {  // Ensure we have a peak
            float x0 = (y3 - y1) / (2 * (2 * y2 - y1 - y3));
            float interpolatedBin = maxBin + x0;
            
            // Convert bin to frequency (44100 Hz sample rate, 256 point FFT)
            float frequency = interpolatedBin * 44100.0 / 256.0;
            return frequency;
        }
    }
    
    // Fallback to simple calculation
    float frequency = maxBin * 44100.0 / 256.0;
    return frequency;
}

// Find closest note to detected frequency
int findClosestNote(float frequency) {
    if (frequency < 0) return -1;
    
    int closestNote = -1;
    float minDifference = 1000;
    
    for (int i = 0; i < 16; i++) {
        float difference = abs(frequency - noteFreqs[i]);
        float cents = 1200 * log2(frequency / noteFreqs[i]);
        
        // Check if within ±50 cents (quarter tone)
        if (abs(cents) < 50 && difference < minDifference) {
            minDifference = difference;
            closestNote = i;
        }
    }
    
    return closestNote;
}

// Trigger sample with velocity
void triggerSample(int noteIndex, float velocity) {
    if (noteIndex < 0 || noteIndex >= 16) return;
    
    int sampleNumber = noteSamples[noteIndex];
    int velocityMIDI = (int)(velocity * 127);
    
    Serial.print("TRIGGER Note: ");
    Serial.print(noteIndex);
    Serial.print(" Sample: ");
    Serial.print(sampleNumber);
    Serial.print(" Freq: ");
    Serial.print(noteFreqs[noteIndex]);
    Serial.print(" Hz, Velocity: ");
    Serial.println(velocityMIDI);
    
    // Add your sample triggering code here
    // playSample(sampleNumber, velocity);
    // or
    // sendMIDI(noteToMIDI[noteIndex], velocityMIDI);
    
    lastTriggeredNote = noteIndex;
    noteActive = true;
}

// Note off detection (optional)
void checkNoteOff(float level) {
    if (noteActive && level < lastPeakLevel * 0.1) {
        noteActive = false;
        Serial.print("Note OFF: ");
        Serial.println(lastTriggeredNote);
        
        // Add your note-off handling here
        // stopSample(lastTriggeredNote);
        // or
        // sendMIDINoteOff(noteToMIDI[lastTriggeredNote]);
    }
}

void loop() {
    // Check if we have both FFT and peak data available
    if (fft.available() && peak.available()) {
        // Read FFT data ONCE and store it
        for (int i = 0; i < 128; i++) {
            fftData[i] = fft.read(i);
        }
        
        // Read peak level
        float level = peak.read();
        
        // For debugging - show we're getting signal
        static int loopCounter = 0;
        loopCounter++;
        if (loopCounter % 100 == 0) {  // Every 100 loops
            if (level > 0.001) {
                Serial.print("Signal detected - Level: ");
                Serial.print(level, 4);
                Serial.print(" Energy: ");
                Serial.println(calculateEnergy(), 4);
            }
        }
        
        // Advanced onset detection with velocity
        float velocity = 0;
        bool onsetDetected = detectOnset(level, velocity);
        
        if (onsetDetected) {
            // Detect pitch using stored FFT data
            float frequency = detectPitch();
            
            Serial.print("Frequency detected: ");
            Serial.println(frequency);
            
            if (frequency > 0) {
                // Find closest note
                int noteIndex = findClosestNote(frequency);
                
                if (noteIndex >= 0) {
                    // Adjust velocity based on detected frequency
                    // Lower notes get a boost since they naturally have less HF content
                    if (frequency < 100) {
                        velocity *= 1.2;
                    } else if (frequency > 300) {
                        velocity *= 0.9;
                    }
                    velocity = constrain(velocity, 0.0, 1.0);
                    
                    // Trigger the sample
                    triggerSample(noteIndex, velocity);
                    lastTriggerTime = millis();
                    lastPeakLevel = level;
                } else {
                    Serial.println("Note not in range");
                }
            }
        }
        
        // Check for note off
        checkNoteOff(level);
    }
    
    // Also handle case where only peak is available (fallback)
    else if (peak.available()) {
        float level = peak.read();
        
        // Simple fallback detection for when FFT isn't ready
        if (level > lastPeakLevel * 1.5 && 
            level > 0.01 && 
            (millis() - lastTriggerTime) > RETRIGGER_TIME) {
            
            Serial.println("Fallback trigger (FFT not available)");
            lastPeakLevel = level;
            lastTriggerTime = millis();
        }
    }
}

// Optional: Function to adjust sensitivity
void adjustSensitivity(float newMultiplier) {
    // Allow dynamic adjustment of threshold multiplier
    // Lower = more sensitive, Higher = less sensitive
    // Typical range: 1.5 (very sensitive) to 4.0 (less sensitive)
    THRESHOLD_MULTIPLIER = constrain(newMultiplier, 1.0, 4.0);
    
    Serial.print("Sensitivity adjusted to: ");
    Serial.println(THRESHOLD_MULTIPLIER);
}

// Optional: Function to switch between bass and guitar modes
void setInstrumentMode(bool isBass) {
    if (isBass) {
        // Bass mode - adjust parameters for lower frequencies
        highpass.setHighpass(0, 30, 0.7);  // Lower cutoff
        noiseFloor = 0.002;  // Higher noise floor for stronger signal
        THRESHOLD_MULTIPLIER = 2.2;  // Slightly higher threshold
        Serial.println("Mode set to: BASS");
    } else {
        // Guitar mode
        highpass.setHighpass(0, 40, 0.7);  // Standard cutoff
        noiseFloor = 0.001;
        THRESHOLD_MULTIPLIER = 2.0;
        Serial.println("Mode set to: GUITAR");
    }
}