// HYBRID GUITAR DRUM TRIGGER
// Combines YIN pitch detection with enhanced onset detection

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <CrashReport.h>

// ========== AUDIO GRAPH ==========
AudioInputI2S             audioInput;
AudioFilterBiquad         highpass;        // Remove DC/rumble
AudioAnalyzeNoteFrequency notefreq;        // YIN algorithm (better than FFT for mono pitch)
AudioAnalyzePeak          peak;            // For onset detection
AudioAnalyzeFFT256        fft;             // Keep FFT for spectral energy analysis

// Drum synths
AudioSynthSimpleDrum      drumKick;
AudioSynthSimpleDrum      drumSnare;
AudioSynthSimpleDrum      drumHihat;
AudioSynthSimpleDrum      drumRide;
AudioSynthSimpleDrum      drumCrash;

// Mixers
AudioMixer4               drumMixer;       // Mix drums
AudioMixer4               mainMixer;       // Mix everything
AudioOutputI2S            audioOutput;

// Audio routing
AudioConnection patchCord1(audioInput, 0, highpass, 0);
AudioConnection patchCord2(highpass, 0, notefreq, 0);
AudioConnection patchCord3(highpass, 0, peak, 0);
AudioConnection patchCord4(highpass, 0, fft, 0);
AudioConnection patchCord5(audioInput, 0, mainMixer, 0);  // Dry signal

// Drums to mixer
AudioConnection patchCord6(drumKick, 0, drumMixer, 0);
AudioConnection patchCord7(drumSnare, 0, drumMixer, 1);
AudioConnection patchCord8(drumHihat, 0, drumMixer, 2);
AudioConnection patchCord9(drumRide, 0, drumMixer, 3);

// Mixers to output
AudioConnection patchCord10(drumMixer, 0, mainMixer, 1);
AudioConnection patchCord11(drumCrash, 0, mainMixer, 2);
AudioConnection patchCord12(mainMixer, 0, audioOutput, 0);
AudioConnection patchCord13(mainMixer, 0, audioOutput, 1);

AudioControlSGTL5000 audioShield;

// ========== CONFIGURATION ==========
const int AUDIO_MEMORY = 60;
const int LINE_IN_LEVEL = 0;         // Try 0 first, then adjust guitar volume
const float HIGHPASS_FREQ = 35.0;    // Hz, remove rumble
const float YIN_THRESHOLD = 0.15;    // Lower = more sensitive pitch detection
const float INPUT_GAIN_ADJUST = 0.5; // Software gain reduction (0.1 to 1.0)

// Onset detection parameters  
const float EMA_ALPHA = 0.07;        // Envelope smoothing (0.03-0.15)
const float ONSET_THRESHOLD = 1.2;   // Even lower threshold for clipped signals
const float NOISE_FLOOR = 0.002;     // Minimum threshold
const int ONSET_HOLDOFF_MS = 50;     // Minimum time between onsets

// Per-drum retrigger times (ms)
const int RETRIGGER_TIME = 100;      // Increased from 70 to prevent spam
unsigned long lastTriggerTime[5] = {0, 0, 0, 0, 0};

// Frequency bands with hysteresis
struct FreqBand {
    float minFreq;
    float maxFreq;
    int drumIndex;
    const char* name;
};

// Tuned for standard guitar/bass
FreqBand frequencyBands[] = {
    {50.0,   95.0,  0, "KICK"},   // Low E and below
    {96.0,   140.0, 1, "SNARE"},  // A string area
    {141.0,  220.0, 2, "HAT"},    // D string area
    {221.0,  350.0, 3, "RIDE"},   // G and B strings
    {351.0,  800.0, 4, "CRASH"},  // High notes
};

// State variables
float envelopeEMA = 0.0;
unsigned long lastOnsetTime = 0;
float lastFrequency = 0;
float pitchConfidence = 0;

// Advanced onset detection variables
float recentPeaks[4] = {0};
int peakIndex = 0;
float spectralFlux = 0;
float lastSpectralEnergy = 0;

// ========== HELPER FUNCTIONS ==========

float calculateSpectralFlux() {
    if (!fft.available()) return 0;
    
    float energy = 0;
    // Focus on guitar frequency range (bins 2-30)
    for (int i = 2; i < 30; i++) {
        float mag = fft.read(i);
        energy += mag * mag;
    }
    energy = sqrt(energy / 28.0);
    
    // Calculate flux (positive difference)
    float flux = max(0, energy - lastSpectralEnergy);
    lastSpectralEnergy = energy;
    
    return flux;
}

bool detectOnset(float peakLevel, float& velocity) {
    // EMERGENCY MODE: If we're clipping, use pitch confidence as trigger
    if (peakLevel >= 0.99) {
        // Use pitch confidence change as onset detector when clipping
        static float lastConfidence = 0;
        static unsigned long lastConfTrigger = 0;
        float confDiff = pitchConfidence - lastConfidence;
        lastConfidence = pitchConfidence;
        
        bool timeOK = (millis() - lastConfTrigger) > 100;  // Increased from 50ms
        
        // Trigger on confidence jump (new note detected)
        if (timeOK && pitchConfidence > 0.75 && confDiff > 0.4) {  // Stricter requirements
            velocity = 0.6;  // Default velocity when clipping
            lastConfTrigger = millis();
            lastOnsetTime = millis();
            // Don't print here - causes too much serial traffic
            return true;
        }
        return false;
    }
    
    // NORMAL MODE: Original onset detection
    // Update recent peaks buffer
    recentPeaks[peakIndex] = peakLevel;
    peakIndex = (peakIndex + 1) % 4;
    
    // Calculate average of recent peaks
    float avgPeak = 0;
    for (int i = 0; i < 4; i++) {
        avgPeak += recentPeaks[i];
    }
    avgPeak /= 4.0;
    
    // Update envelope
    envelopeEMA = (1.0 - EMA_ALPHA) * envelopeEMA + EMA_ALPHA * peakLevel;
    
    // Calculate adaptive threshold
    float threshold = max(NOISE_FLOOR, envelopeEMA * ONSET_THRESHOLD);
    
    // Get spectral flux if available
    float flux = calculateSpectralFlux();
    
    // Multi-condition onset detection
    bool peakOnset = peakLevel > threshold;
    bool risingEdge = peakLevel > avgPeak * 1.3;
    bool spectralOnset = flux > 0.01;  // Spectral change detected
    bool timeOK = (millis() - lastOnsetTime) > ONSET_HOLDOFF_MS;
    
    // Combine conditions
    bool isOnset = timeOK && peakOnset && (risingEdge || spectralOnset);
    
    if (isOnset) {
        velocity = constrain(sqrt(peakLevel) * 1.5, 0.1, 1.0);
        lastOnsetTime = millis();
        return true;
    }
    
    return false;
}

int getFrequencyBand(float freq) {
    const float HYSTERESIS = 0.05;  // 5% overlap between bands
    
    for (int i = 0; i < 5; i++) {
        float minF = frequencyBands[i].minFreq * (1.0 - HYSTERESIS);
        float maxF = frequencyBands[i].maxFreq * (1.0 + HYSTERESIS);
        
        // Bias toward last frequency's band (string continuity)
        if (abs(freq - lastFrequency) < 20 && i == getFrequencyBand(lastFrequency)) {
            minF *= 0.9;  // Expand band for same string
            maxF *= 1.1;
        }
        
        if (freq >= minF && freq <= maxF) {
            return i;
        }
    }
    return -1;
}

bool canRetrigger(int drumIndex) {
    unsigned long now = millis();
    if (now - lastTriggerTime[drumIndex] > RETRIGGER_TIME) {
        lastTriggerTime[drumIndex] = now;
        return true;
    }
    return false;
}

void triggerDrum(int drumIndex, float velocity, float frequency) {
    float gain = velocity * 0.8 + 0.2;  // Always some minimum volume
    
    // Apply software gain reduction if needed
    gain *= INPUT_GAIN_ADJUST;
    
    // Simplified output - less serial spam
    const char* drumName = "";
    
    switch (drumIndex) {
        case 0:
            drumMixer.gain(0, 0.7 * gain);
            drumKick.noteOn();
            drumName = "KICK ";
            break;
        case 1:
            drumMixer.gain(1, 0.7 * gain);
            drumSnare.noteOn();
            drumName = "SNARE";
            break;
        case 2:
            drumMixer.gain(2, 0.5 * gain);
            drumHihat.noteOn();
            drumName = "HAT  ";
            break;
        case 3:
            drumMixer.gain(3, 0.5 * gain);
            drumRide.noteOn();
            drumName = "RIDE ";
            break;
        case 4:
            mainMixer.gain(2, 0.6 * gain);
            drumCrash.noteOn();
            drumName = "CRASH";
            break;
    }
    
    // Reduced serial output
    Serial.print(drumName);
    Serial.print(" ");
    Serial.print(frequency, 0);
    Serial.println("Hz");
}

void setupDrums() {
    // Your exact drum configurations
    drumKick.frequency(60);
    drumKick.length(150);
    drumKick.secondMix(0.0);
    drumKick.pitchMod(0.5);
    
    drumSnare.frequency(200);
    drumSnare.length(100);
    drumSnare.secondMix(1.0);
    drumSnare.pitchMod(0.2);
    
    drumHihat.frequency(800);
    drumHihat.length(40);
    drumHihat.secondMix(1.0);
    drumHihat.pitchMod(0.0);
    
    drumRide.frequency(500);
    drumRide.length(300);
    drumRide.secondMix(0.5);
    drumRide.pitchMod(0.1);
    
    drumCrash.frequency(900);
    drumCrash.length(500);
    drumCrash.secondMix(1.0);
    drumCrash.pitchMod(0.0);
    
    // Mixer levels
    drumMixer.gain(0, 0.7);   // Kick
    drumMixer.gain(1, 0.7);   // Snare
    drumMixer.gain(2, 0.5);   // Hihat
    drumMixer.gain(3, 0.5);   // Ride
    
    mainMixer.gain(0, 0.0);   // Dry signal (muted)
    mainMixer.gain(1, 0.8);   // Drums bus
    mainMixer.gain(2, 0.6);   // Crash direct
}

// ========== SETUP ==========
void setup() {
    Serial.begin(115200);
    delay(500);
    
    // Check for crashes
    if (CrashReport) {
        Serial.println("=== CRASH REPORT ===");
        Serial.print(CrashReport);
        Serial.println("====================");
    }
    
    Serial.println("====================================");
    Serial.println("  HYBRID YIN DRUM TRIGGER v2.0     ");
    Serial.println("====================================");
    
    // Audio initialization
    AudioMemory(AUDIO_MEMORY);
    audioShield.enable();
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.lineInLevel(LINE_IN_LEVEL);
    audioShield.lineOutLevel(29);  // Add line out configuration (13-31, 29=default)
    audioShield.volume(0.7);
    
    // IMPORTANT: Enable headphone output explicitly
    audioShield.unmuteHeadphone();
    audioShield.unmuteLineout();
    
    // Configure filters
    highpass.setHighpass(0, HIGHPASS_FREQ, 0.707);
    
    // Initialize YIN pitch detection
    notefreq.begin(YIN_THRESHOLD);
    
    // Setup drums
    setupDrums();
    
    // Initialize onset detection arrays
    for (int i = 0; i < 4; i++) {
        recentPeaks[i] = 0.001;
    }
    envelopeEMA = 0.001;
    
    // Startup sequence
    Serial.println("Playing test sequence...");
    delay(300);
    drumKick.noteOn();
    delay(200);
    drumSnare.noteOn();
    delay(200);
    drumHihat.noteOn();
    delay(200);
    drumCrash.noteOn();
    delay(500);
    
    Serial.println("\n🎸 Ready! Play your guitar!");
    Serial.println("====================================\n");
}

// ========== MAIN LOOP ==========
void loop() {
    // Read current peak level with software gain adjustment
    float currentPeak = peak.read() * INPUT_GAIN_ADJUST;
    
    // Clipping detection - less frequent warnings
    static unsigned long lastClipWarning = 0;
    static bool wasClipping = false;
    bool isClipping = currentPeak >= 0.99;
    
    if (isClipping && !wasClipping && millis() - lastClipWarning > 5000) {
        Serial.println("⚠️ CLIPPING! Turn down guitar volume!");
        lastClipWarning = millis();
    }
    wasClipping = isClipping;
    
    // Check for onset
    float velocity = 0;
    bool onsetDetected = detectOnset(currentPeak, velocity);
    
    // Check if we have pitch data
    if (notefreq.available()) {
        float frequency = notefreq.read();
        pitchConfidence = notefreq.probability();
        
        // Process normally if onset detected
        if (onsetDetected && frequency > 0 && pitchConfidence > 0.6) {
            int bandIndex = getFrequencyBand(frequency);
            
            if (bandIndex >= 0 && canRetrigger(bandIndex)) {
                triggerDrum(bandIndex, velocity, frequency);
                lastFrequency = frequency;
            }
        }
        // SIMPLIFIED FALLBACK: Only if clipping AND very confident
        else if (isClipping && frequency > 0 && pitchConfidence > 0.85) {
            static unsigned long lastFallbackTime = 0;
            static float lastFallbackFreq = 0;
            
            // Only trigger if frequency changed significantly or enough time passed
            bool freqChanged = abs(frequency - lastFallbackFreq) > 10;
            bool timeElapsed = (millis() - lastFallbackTime) > 150;
            
            if ((freqChanged || timeElapsed) && timeElapsed) {
                int bandIndex = getFrequencyBand(frequency);
                if (bandIndex >= 0 && canRetrigger(bandIndex)) {
                    Serial.print("FB: ");
                    triggerDrum(bandIndex, 0.5, frequency);
                    lastFallbackTime = millis();
                    lastFallbackFreq = frequency;
                }
            }
        }
    }
    
    // Minimal periodic debug
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 2000) {  // Every 2 seconds only
        if (!isClipping) {
            Serial.print("[OK] peak:");
            Serial.print(currentPeak, 2);
            Serial.print(" env:");
            Serial.println(envelopeEMA, 2);
        }
        lastDebugTime = millis();
    }
}