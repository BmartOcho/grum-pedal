// Use Teensy's DSP instructions for speed
#include <arm_math.h>

class OptimizedTrigger {
    // Use ARM DSP library for FFT
    arm_rfft_fast_instance_f32 fftInstance;
    
    void setupFFT() {
        arm_rfft_fast_init_f32(&fftInstance, 256);
    }
    
    // Fast correlation using DSP instructions
    float fastCorrelation(float32_t* srcA, float32_t* srcB, uint32_t length) {
        float32_t result;
        arm_dot_prod_f32(srcA, srcB, length, &result);
        return result;
    }
    
    // Use DMA for audio input to reduce CPU load
    void setupDMA() {
        // Configure DMA for automatic audio buffer filling
        // This runs in background without CPU intervention
    }
};