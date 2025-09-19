class StringInstrumentTrigger {
private:
    // Adaptive noise gate
    float noiseFloor = 0.001;
    float gateThreshold = 0.005;
    
    // String-specific parameters
    struct StringProfile {
        float expectedDecayRate;
        float pluckThreshold;
        float harmonicRatios[4];  // Expected harmonic ratios
    };
    
    StringProfile bassProfile = {
        0.95,  // Slower decay for bass
        0.02,  // Higher threshold (stronger signal)
        {2.0, 3.0, 4.0, 5.0}  // Harmonic series
    };
    
    StringProfile guitarProfile = {
        0.92,  // Faster decay than bass
        0.015, // Lower threshold
        {2.0, 3.0, 4.0, 5.0}
    };
    
    StringProfile* currentProfile = &guitarProfile;
    
public:
    // Comb filter approach for very fast pitch detection
    float combFilterPitch(float* buffer, int bufferSize, float minFreq, float maxFreq) {
        int minPeriod = SAMPLE_RATE / maxFreq;
        int maxPeriod = SAMPLE_RATE / minFreq;
        
        float maxCorr = 0;
        int bestPeriod = 0;
        
        // Only check periods in expected range
        for (int period = minPeriod; period <= maxPeriod && period < bufferSize/2; period++) {
            float correlation = 0;
            float normA = 0;
            float normB = 0;
            
            // Normalized autocorrelation
            for (int i = 0; i < bufferSize - period; i++) {
                correlation += buffer[i] * buffer[i + period];
                normA += buffer[i] * buffer[i];
                normB += buffer[i + period] * buffer[i + period];
            }
            
            if (normA > 0 && normB > 0) {
                correlation = correlation / sqrt(normA * normB);
                
                if (correlation > maxCorr) {
                    maxCorr = correlation;
                    bestPeriod = period;
                }
            }
        }
        
        if (maxCorr > 0.7 && bestPeriod > 0) {  // Confidence threshold
            return (float)SAMPLE_RATE / bestPeriod;
        }
        return -1;
    }
    
    // Harmonic product spectrum for robust pitch detection
    float harmonicProductSpectrum(float* fftMagnitude, int fftSize, int sampleRate) {
        float hps[fftSize/2];
        memcpy(hps, fftMagnitude, (fftSize/2) * sizeof(float));
        
        // Downsample and multiply
        for (int h = 2; h <= 4; h++) {
            for (int i = 0; i < fftSize/(2*h); i++) {
                hps[i] *= fftMagnitude[i * h];
            }
        }
        
        // Find peak
        int maxBin = 0;
        float maxValue = 0;
        
        int minBin = 20 * fftSize / sampleRate;  // Start from 20Hz
        int maxBin = 2000 * fftSize / sampleRate; // Up to 2kHz
        
        for (int i = minBin; i < maxBin && i < fftSize/8; i++) {
            if (hps[i] > maxValue) {
                maxValue = hps[i];
                maxBin = i;
            }
        }
        
        // Parabolic interpolation for precise frequency
        if (maxBin > 0 && maxBin < fftSize/8 - 1) {
            float y1 = hps[maxBin - 1];
            float y2 = hps[maxBin];
            float y3 = hps[maxBin + 1];
            
            float x0 = (y3 - y1) / (2 * (2 * y2 - y1 - y3));
            float freq = (maxBin + x0) * sampleRate / (float)fftSize;
            
            return freq;
        }
        
        return -1;
    }
    
    // Velocity calculation with frequency weighting
    float calculateVelocity(float* buffer, int bufferSize, float frequency) {
        // RMS with frequency-dependent weighting
        float rms = 0;
        for (int i = bufferSize - 256; i < bufferSize; i++) {
            rms += buffer[i] * buffer[i];
        }
        rms = sqrt(rms / 256.0);
        
        // Adjust for frequency (lower notes naturally have more energy)
        float freqCompensation = 1.0;
        if (frequency > 0) {
            freqCompensation = sqrt(100.0 / frequency);  // Compensation curve
            freqCompensation = constrain(freqCompensation, 0.5, 2.0);
        }
        
        float velocity = rms * freqCompensation * 4.0;  // Scale factor
        return constrain(velocity, 0.0, 1.0);
    }
};