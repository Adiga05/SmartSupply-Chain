#ifndef SHOCK_CLASSIFIER_H
#define SHOCK_CLASSIFIER_H

#include <Arduino.h>
#include <math.h>

enum ShockEvent { 
    NORMAL, 
    FREE_FALL, 
    MAJOR_IMPACT, 
    MINOR_BUMP, 
    TILT_ALERT, 
    VIBRATION 
};

class ShockClassifier {
private:
    static const int WINDOW_SIZE = 50; // 50Hz = 1 second window
    
    // We store the magnitude of the acceleration vector for most features
    float magBuffer[WINDOW_SIZE];
    
    // We also store Z-axis gravity to calculate simple tilt
    float zBuffer[WINDOW_SIZE];
    
    int index = 0;
    bool bufferFull = false;

    // Feature calculation methods
    float calculateRMS() const {
        float sumSq = 0;
        for (int i = 0; i < WINDOW_SIZE; i++) {
            sumSq += magBuffer[i] * magBuffer[i];
        }
        return sqrt(sumSq / WINDOW_SIZE);
    }

    float calculatePeakToPeak() const {
        float minVal = magBuffer[0];
        float maxVal = magBuffer[0];
        for (int i = 1; i < WINDOW_SIZE; i++) {
            if (magBuffer[i] < minVal) minVal = magBuffer[i];
            if (magBuffer[i] > maxVal) maxVal = magBuffer[i];
        }
        return maxVal - minVal;
    }

    float calculateKurtosisApproximation(float rms) const {
        if (rms == 0) return 0;
        float sumQuad = 0;
        float sum = 0;
        for (int i = 0; i < WINDOW_SIZE; i++) sum += magBuffer[i];
        float mean = sum / WINDOW_SIZE;
        
        for (int i = 0; i < WINDOW_SIZE; i++) {
            float diff = magBuffer[i] - mean;
            sumQuad += (diff * diff * diff * diff);
        }
        float m4 = sumQuad / WINDOW_SIZE;
        float variance = rms * rms - mean * mean; // E[X^2] - (E[X])^2
        if (variance <= 0) return 0;
        return (m4 / (variance * variance)) - 3.0; // Excess kurtosis
    }

    int calculateZeroCrossings(float meanVal) const {
        int zc = 0;
        bool lastState = magBuffer[0] > meanVal;
        for (int i = 1; i < WINDOW_SIZE; i++) {
            bool currentState = magBuffer[i] > meanVal;
            if (currentState != lastState) {
                zc++;
                lastState = currentState;
            }
        }
        return zc;
    }

    float calculateAverageTiltZ() const {
        float sumZ = 0;
        for(int i=0; i<WINDOW_SIZE; i++) sumZ += zBuffer[i];
        return sumZ / WINDOW_SIZE;
    }

public:
    ShockClassifier() {}

    // Add raw X, Y, Z readings (in G's)
    // Returns true if the window is full and ready for classification
    bool addReading(float ax, float ay, float az) {
        float magnitude = sqrt(ax*ax + ay*ay + az*az);
        magBuffer[index] = magnitude;
        zBuffer[index] = az;
        
        index++;
        if (index >= WINDOW_SIZE) {
            index = 0; // Overwrite in a rolling/sliding fashion or chunk-based
            bufferFull = true;
            return true;
        }
        // If we want a true sliding window that classifies on every tick after filling,
        // we would use a circular buffer. For simplicity, chunk-based (1 sec at a time) is often used,
        // but here we just signify it's full and wrap around.
        return false;
    }
    
    bool isBufferFull() const {
        return bufferFull;
    }
    
    // Rule-Based Decision Tree to classify the window
    ShockEvent classify() const {
        if (!bufferFull) return NORMAL;

        float rms = calculateRMS();
        float ptp = calculatePeakToPeak();
        float kurtosis = calculateKurtosisApproximation(rms);
        
        float sum = 0;
        for(int i=0; i<WINDOW_SIZE; i++) sum += magBuffer[i];
        float mean = sum / WINDOW_SIZE;
        
        int zc = calculateZeroCrossings(mean);
        float avgZ = calculateAverageTiltZ();

        // 1. FREE_FALL check (Magnitude near 0 for sustained time)
        if (mean < 0.3 && ptp < 0.4) {
            return FREE_FALL;
        }

        // 2. MAJOR_IMPACT check (Massive spike)
        if (ptp > 4.0 || rms > 2.5) {
            return MAJOR_IMPACT;
        }

        // 3. MINOR_BUMP check (Moderate spike with high impulsiveness/kurtosis)
        if (ptp > 1.5 && kurtosis > 1.5) {
            return MINOR_BUMP;
        }

        // 4. VIBRATION check (Sustained energy, high zero crossings)
        if (rms > 1.2 && zc > 15) {
            return VIBRATION;
        }

        // 5. TILT_ALERT check 
        // Assuming 1.0G = upright. If avgZ < 0.5 (more than ~60 deg tilt)
        if (fabs(avgZ) < 0.5 && mean > 0.8 && mean < 1.2) {
            return TILT_ALERT;
        }

        return NORMAL;
    }
    
    void extractFeatures(float &outRms, float &outKurtosis, float &outZcr, float &outTilt) {
        float mean = 0;
        for (int i = 0; i < WINDOW_SIZE; i++) mean += zBuffer[i];
        mean /= WINDOW_SIZE;

        float sqSum = 0;
        float fourthSum = 0;
        int zcCount = 0;
        
        for (int i = 0; i < WINDOW_SIZE; i++) {
            float diff = zBuffer[i] - mean;
            float sq = diff * diff;
            sqSum += sq;
            fourthSum += sq * sq;
            
            if (i > 0) {
                if ((zBuffer[i-1] - mean) * (zBuffer[i] - mean) < 0) {
                    zcCount++;
                }
            }
        }
        
        float variance = sqSum / WINDOW_SIZE;
        outRms = sqrt(variance);
        
        if (variance > 0.0001) {
            outKurtosis = (fourthSum / WINDOW_SIZE) / (variance * variance);
        } else {
            outKurtosis = 3.0; // Normal distribution kurtosis
        }
        
        outZcr = zcCount;
        
        // Tilt approximation from mean Z gravity (1.0 = 0 deg, 0.0 = 90 deg)
        float clampedMean = max(-1.0f, min(1.0f, mean));
        outTilt = acos(clampedMean) * 180.0 / PI;
    }

    // Helper to get string name of event
    static const char* getEventName(ShockEvent event) {
        switch(event) {
            case FREE_FALL: return "FREE_FALL";
            case MAJOR_IMPACT: return "MAJOR_IMPACT";
            case MINOR_BUMP: return "MINOR_BUMP";
            case TILT_ALERT: return "TILT_ALERT";
            case VIBRATION: return "VIBRATION";
            case NORMAL: return "NORMAL";
            default: return "UNKNOWN";
        }
    }
};

#endif // SHOCK_CLASSIFIER_H
