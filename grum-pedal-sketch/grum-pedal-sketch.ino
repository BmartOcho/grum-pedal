#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <ResponsiveAnalogRead.h>

// Your existing audio setup - keeping it exactly as you have it
AudioInputI2S            audioInput;
AudioAnalyzeFFT256       fft;
AudioAnalyzePeak         peak;
AudioFilterBiquad        highpass;
AudioConnection          patchCord1(audioInput, 0, highpass, 0);
AudioConnection          patchCord2(highpass, 0, fft, 0);
AudioConnection          patchCord3(highpass, 0, peak, 0);
AudioControlSGTL5000     audioShield;

// Advanced onset detection variables
const int ENERGY_HISTORY_SIZE = 8;
float energyHistory[ENERGY_HISTORY_SIZE] = {0};
int energyHistoryIndex = 0;
float lastEnergy = 0;
float audioBuffer[256];  // Buffer for HFC calculation
int audioBufferIndex = 0;

// Your existing variables
float noteFreqs[16] = {
    82.41, 87.31, 92.50, 98.00, 103.83, 110.00, 116.54, 123.47,
    130.81, 138.59, 146.83, 155.56, 164.81, 174.61, 185.00, 196.00
};
int noteSamples[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
float lastPeakLevel = 0;
unsigned long lastTriggerTime = 0;
const unsigned int RETRIGGER_TIME = 50;
bool noteActive = false;
int lastTriggeredNote = -1;

// New adaptive threshold variables
float adaptiveThreshold = 0.01;
float noiseFloor = 0.001;
const float THRESHOLD_MULTIPLIER = 2.5;  // Adjust for sensitivity
const float HFC_THRESHOLD = 1.5;         // High frequency content threshold
const float ENERGY_RATIO_THRESHOLD = 2.0; // Energy increase ratio threshold

void setup() {
    Serial.begin(115200);
    AudioMemory(12);
    
    audioShield.enable();
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.lineInLevel(0);  // Adjust input sensitivity (0-15)
    audioShield.volume(0.5);
    
    // Configure high-pass filter to remove DC and low frequency noise
    highpass.setHighpass(0, 40, 0.7);  // 40 Hz cutoff
    
    Serial.println("Hybrid Guitar/Bass Trigger Ready");
}

// Calculate current frame energy
float calculateEnergy(float level, int fftBins = 128) {
    float energy = 0;
    
    // Use FFT bins for more accurate energy calculation
    if (fft.available()) {
        for (int i = 0; i < fftBins; i++) {
            float binValue = fft.read(i);
            energy += binValue * binValue;
        }
        energy = sqrt(energy / fftBins);
    } else {
        // Fallback to peak level if FFT not available
        energy = level;
    }
    
    return energy;
}

// Calculate High Frequency Content (HFC)
float calculateHFC() {
    float hfc = 0;
    
    if (fft.available()) {
        // Weight higher frequency bins more heavily
        for (int i = 64; i < 128; i++) {  // Upper half of spectrum
            float binValue = fft.read(i);
            float weight = (float)i / 128.0;  // Linear weighting
            hfc += binValue * binValue * weight;
        }
        hfc = sqrt(hfc / 64.0);
    }
    
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

// Advanced onset detection replacing simple peak comparison
bool detectOnset(float level, float &velocity) {
    // Calculate current energy
    float currentEnergy = calculateEnergy(level);
    
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
    
    // Multi-factor onset detection
    bool isOnset = false;
    
    // Check all conditions for onset
    if (currentEnergy > adaptiveThreshold &&           // Above adaptive threshold
        energyRatio > ENERGY_RATIO_THRESHOLD &&         // Sudden energy increase
        hfcRatio > HFC_THRESHOLD &&                     // High frequency content present
        currentEnergy > lastEnergy * 1.3 &&             // Rising edge
        (millis() - lastTriggerTime) > RETRIGGER_TIME) { // Retrigger time passed
        
        isOnset = true;
        
        // Calculate velocity based on energy and HFC
        velocity = currentEnergy * 2.0;
        
        // Frequency compensation (optional - lower notes naturally have more energy)
        // This will be refined after pitch detection
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
    
    lastEnergy = currentEnergy;
    return isOnset;
}

// Your existing pitch detection (can be enhanced with YIN later)
float detectPitch() {
    if (!fft.available()) return -1;
    
    float maxLevel = 0;
    int maxBin = 0;
    
    // Find the peak frequency bin
    for (int i = 2; i < 128; i++) {  // Skip DC and very low frequencies
        float level = fft.read(i);
        if (level > maxLevel) {
            maxLevel = level;
            maxBin = i;
        }
    }
    
    // Parabolic interpolation for more accurate frequency
    if (maxBin > 0 && maxBin < 127) {
        float y1 = fft.read(maxBin - 1);
        float y2 = fft.read(maxBin);
        float y3 = fft.read(maxBin + 1);
        
        float x0 = (y3 - y1) / (2 * (2 * y2 - y1 - y3));
        float interpolatedBin = maxBin + x0;
        
        // Convert bin to frequency (44100 Hz sample rate, 256 point FFT)
        float frequency = interpolatedBin * 44100.0 / 256.0;
        return frequency;
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
    // Check peak level
    if (peak.available()) {
        float level = peak.read();
        
        // Advanced onset detection with velocity
        float velocity = 0;
        bool onsetDetected = detectOnset(level, velocity);
        
        if (onsetDetected) {
            // Detect pitch
            float frequency = detectPitch();
            
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
                }
            }
        }
        
        // Check for note off
        checkNoteOff(level);
    }
    
    // Optional: Print debug info periodically
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 1000) {  // Every second
        if (peak.available()) {
            float level = peak.read();
            float energy = calculateEnergy(level);
            
            Serial.print("Level: ");
            Serial.print(level, 4);
            Serial.print(" Energy: ");
            Serial.print(energy, 4);
            Serial.print(" Threshold: ");
            Serial.println(adaptiveThreshold, 4);
        }
        lastDebugTime = millis();
    }
}

// Optional: Function to adjust sensitivity
void adjustSensitivity(float newMultiplier) {
    // Allow dynamic adjustment of threshold multiplier
    // Lower = more sensitive, Higher = less sensitive
    // Typical range: 1.5 (very sensitive) to 4.0 (less sensitive)
    const float MIN_MULTIPLIER = 1.5;
    const float MAX_MULTIPLIER = 4.0;
    
    float constrainedMultiplier = constrain(newMultiplier, MIN_MULTIPLIER, MAX_MULTIPLIER);
    
    // Note: You'd need to make THRESHOLD_MULTIPLIER non-const for this
    // THRESHOLD_MULTIPLIER = constrainedMultiplier;
    
    Serial.print("Sensitivity adjusted to: ");
    Serial.println(constrainedMultiplier);
}

// Optional: Function to switch between bass and guitar modes
void setInstrumentMode(bool isBass) {
    if (isBass) {
        // Bass mode - adjust parameters for lower frequencies
        highpass.setHighpass(0, 30, 0.7);  // Lower cutoff
        noiseFloor = 0.002;  // Higher noise floor for stronger signal
        // THRESHOLD_MULTIPLIER = 2.8;  // Slightly higher threshold
    } else {
        // Guitar mode
        highpass.setHighpass(0, 40, 0.7);  // Standard cutoff
        noiseFloor = 0.001;
        // THRESHOLD_MULTIPLIER = 2.5;
    }
    
    Serial.print("Mode set to: ");
    Serial.println(isBass ? "BASS" : "GUITAR");
}