// Teensy 4.0 Guitar/Bass Trigger with YIN Algorithm
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

class GuitarTrigger {
private:
    static const int SAMPLE_RATE = 44100;
    static const int BUFFER_SIZE = 2048;  // ~46ms at 44.1kHz
    static const int YIN_BUFFER_SIZE = 512;  // For YIN algorithm
    
    float audioBuffer[BUFFER_SIZE];
    float yinBuffer[YIN_BUFFER_SIZE];
    int bufferIndex = 0;
    
    // Frequency ranges for triggers (customize these)
    struct NoteRange {
        float minFreq;
        float maxFreq;
        int sampleIndex;
        int midiNote;
    };
    
    // Bass guitar ranges (E1 to G3)
    NoteRange bassRanges[12] = {
        {39.0, 43.0, 0, 28},   // E1
        {43.5, 47.5, 1, 29},   // F1
        {48.0, 52.0, 2, 31},   // G1
        {52.5, 57.5, 3, 33},   // A1
        {58.0, 63.0, 4, 35},   // B1
        {63.5, 69.5, 5, 36},   // C2
        {70.0, 76.0, 6, 38},   // D2
        {76.5, 83.5, 7, 40},   // E2
        {84.0, 91.0, 8, 41},   // F2
        {91.5, 99.5, 9, 43},   // G2
        {100.0, 108.0, 10, 45}, // A2
        {108.5, 118.5, 11, 47}  // B2
    };
    
    // State tracking
    float lastFrequency = 0;
    float lastAmplitude = 0;
    bool noteActive = false;
    unsigned long lastTriggerTime = 0;
    const unsigned int RETRIGGER_TIME = 50; // ms minimum between triggers
    
    // Onset detection parameters
    float energyHistory[8] = {0};
    int energyHistoryIndex = 0;
    float lastEnergy = 0;
    
public:
    // Optimized YIN implementation for Teensy
    float detectPitch(float* buffer, int bufferSize) {
        float threshold = 0.15;  // Confidence threshold
        
        // Step 1: Difference function
        for (int tau = 0; tau < YIN_BUFFER_SIZE; tau++) {
            float sum = 0;
            for (int i = 0; i < YIN_BUFFER_SIZE; i++) {
                float delta = buffer[i] - buffer[i + tau];
                sum += delta * delta;
            }
            yinBuffer[tau] = sum;
        }
        
        // Step 2: Cumulative mean normalized difference
        yinBuffer[0] = 1;
        float runningSum = 0;
        for (int tau = 1; tau < YIN_BUFFER_SIZE; tau++) {
            runningSum += yinBuffer[tau];
            yinBuffer[tau] *= tau / runningSum;
        }
        
        // Step 3: Absolute threshold
        int tau = -1;
        for (int i = 2; i < YIN_BUFFER_SIZE; i++) {
            if (yinBuffer[i] < threshold) {
                while (i + 1 < YIN_BUFFER_SIZE && yinBuffer[i + 1] < yinBuffer[i]) {
                    i++;
                }
                tau = i;
                break;
            }
        }
        
        // Step 4: Parabolic interpolation
        if (tau == -1 || tau == YIN_BUFFER_SIZE - 1) {
            return -1;  // No pitch found
        }
        
        float betterTau = tau;
        if (tau > 0 && tau < YIN_BUFFER_SIZE - 1) {
            float x0 = yinBuffer[tau - 1];
            float x1 = yinBuffer[tau];
            float x2 = yinBuffer[tau + 1];
            
            float a = (x2 - 2 * x1 + x0) / 2;
            float b = (x2 - x0) / 2;
            if (a != 0) {
                betterTau = tau - b / (2 * a);
            }
        }
        
        return SAMPLE_RATE / betterTau;
    }
    
    // Fast onset detection optimized for plucked strings
    bool detectPluck(float* buffer, int bufferSize, float& velocity) {
        // Calculate current frame energy
        float energy = 0;
        for (int i = bufferSize - 256; i < bufferSize; i++) {
            energy += buffer[i] * buffer[i];
        }
        energy = sqrt(energy / 256.0);
        
        // High-frequency content (important for pluck detection)
        float hfc = 0;
        for (int i = bufferSize - 128; i < bufferSize - 1; i++) {
            float diff = abs(buffer[i + 1] - buffer[i]);
            hfc += diff * diff;
        }
        hfc = sqrt(hfc / 128.0);
        
        // Calculate energy increase ratio
        float avgHistory = 0;
        for (int i = 0; i < 8; i++) {
            avgHistory += energyHistory[i];
        }
        avgHistory /= 8.0;
        
        // Adaptive threshold based on recent history
        float threshold = avgHistory * 2.5 + 0.01;  // Adjust multiplier for sensitivity
        float energyRatio = energy / (avgHistory + 0.001);
        
        // Store current energy in history
        energyHistory[energyHistoryIndex] = energy;
        energyHistoryIndex = (energyHistoryIndex + 1) % 8;
        
        // Detect onset with multiple conditions
        bool isOnset = false;
        if (energy > threshold && 
            energyRatio > 2.0 &&  // Sudden increase
            hfc > avgHistory * 1.5 &&  // High frequency content
            energy > lastEnergy * 1.3) {  // Rising edge
            
            isOnset = true;
            velocity = constrain(energy * 2.0, 0.0, 1.0);  // Normalize velocity
        }
        
        lastEnergy = energy;
        return isOnset;
    }
    
    // Zero-crossing rate for additional validation
    int calculateZCR(float* buffer, int start, int length) {
        int crossings = 0;
        for (int i = start; i < start + length - 1; i++) {
            if ((buffer[i] >= 0 && buffer[i + 1] < 0) || 
                (buffer[i] < 0 && buffer[i + 1] >= 0)) {
                crossings++;
            }
        }
        return crossings;
    }
    
    // Main processing function
    void process(float* inputBuffer, int inputSize) {
        // Copy to circular buffer
        for (int i = 0; i < inputSize; i++) {
            audioBuffer[bufferIndex] = inputBuffer[i];
            bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
        }
        
        // Check for pluck
        float velocity;
        bool pluckDetected = detectPluck(audioBuffer, BUFFER_SIZE, velocity);
        
        if (pluckDetected && 
            (millis() - lastTriggerTime) > RETRIGGER_TIME) {
            
            // Detect pitch using YIN
            float frequency = detectPitch(audioBuffer, BUFFER_SIZE);
            
            if (frequency > 0) {
                // Validate with zero-crossing rate
                int expectedZCR = (int)(frequency * 2 * 256 / SAMPLE_RATE);
                int actualZCR = calculateZCR(audioBuffer, BUFFER_SIZE - 256, 256);
                
                // Check if ZCR is within reasonable range
                if (abs(actualZCR - expectedZCR) < expectedZCR * 0.3) {
                    // Find matching range and trigger sample
                    triggerSample(frequency, velocity);
                    lastFrequency = frequency;
                    lastAmplitude = velocity;
                    lastTriggerTime = millis();
                    noteActive = true;
                }
            }
        }
        
        // Note off detection (optional)
        if (noteActive) {
            float currentEnergy = 0;
            for (int i = BUFFER_SIZE - 128; i < BUFFER_SIZE; i++) {
                currentEnergy += audioBuffer[i] * audioBuffer[i];
            }
            currentEnergy = sqrt(currentEnergy / 128.0);
            
            if (currentEnergy < lastAmplitude * 0.1) {
                noteActive = false;
                // Trigger note off if needed
            }
        }
    }
    
    void triggerSample(float frequency, float velocity) {
        // Find matching range
        for (int i = 0; i < 12; i++) {
            if (frequency >= bassRanges[i].minFreq && 
                frequency <= bassRanges[i].maxFreq) {
                
                // Trigger your sample here
                // sendMIDI(bassRanges[i].midiNote, velocity * 127);
                // or
                // playSample(bassRanges[i].sampleIndex, velocity);
                
                Serial.print("Triggered: ");
                Serial.print(frequency);
                Serial.print(" Hz, Sample: ");
                Serial.print(i);
                Serial.print(", Velocity: ");
                Serial.println(velocity);
                
                break;
            }
        }
    }
};

// Teensy Audio Library setup
AudioInputI2S            audioInput;
AudioAnalyzeFFT1024      fft;
AudioConnection          patchCord1(audioInput, 0, fft, 0);
AudioControlSGTL5000     audioShield;

GuitarTrigger trigger;
float audioBlock[128];

void setup() {
    Serial.begin(115200);
    AudioMemory(12);
    audioShield.enable();
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.volume(0.5);
    
    // High-pass filter to remove DC and low rumble
    audioShield.adcHighPassFilterEnable();
}

void loop() {
    // Process audio blocks
    if (fft.available()) {
        // Get audio data
        for (int i = 0; i < 128; i++) {
            audioBlock[i] = fft.read(i);
        }
        trigger.process(audioBlock, 128);
    }
}