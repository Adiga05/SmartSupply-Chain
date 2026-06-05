#ifndef SHOCK_CLASSIFIER_H
#define SHOCK_CLASSIFIER_H

// ============================================================
//  Accelerometer-Based Shock & Impact Classifier
//
//  Collects accelerometer samples into a 1-second sliding
//  window, extracts time-domain features (RMS, peak-to-peak,
//  kurtosis, zero-crossing rate), and classifies the current
//  motion pattern into one of 6 categories.
//
//  Classification is rule-based with tuned thresholds — no ML
//  framework required. Can be replaced with a TFLite model
//  later (Tier 4) using the same feature vector.
//
//  Memory: ~616 bytes (50×3 floats buffer + features struct)
// ============================================================

#include <math.h>
#include <string.h>
#include "edge_ai_config.h"

// --- Shock Event Categories ---
enum ShockClass {
    SHOCK_NORMAL       = 0,   // Resting or gentle motion (~1G)
    SHOCK_MINOR_BUMP   = 1,   // Moderate jolt (1.5-3.0G)
    SHOCK_MAJOR_IMPACT = 2,   // Severe impact (>3.0G)
    SHOCK_FREE_FALL    = 3,   // Near-zero G detected (drop!)
    SHOCK_TILT_ALERT   = 4,   // Sustained tilt >45° from vertical
    SHOCK_VIBRATION    = 5    // High-frequency oscillation pattern
};

// --- Extracted Feature Vector ---
struct AccelFeatures {
    float rms;              // Root Mean Square — overall vibration energy
    float peakToPeak;       // Max minus min magnitude — amplitude range
    float kurtosis;         // Excess kurtosis — impulsiveness of signal
    float zeroCrossingRate; // Oscillation frequency estimate (0.0 - 1.0)
    float meanMagnitude;    // Average acceleration magnitude
    float maxMagnitude;     // Peak acceleration in window
    float tiltAngle;        // Current tilt from vertical (degrees)
    int   freeFallCount;    // Longest run of consecutive low-G samples
};

// --- Shock Classifier ---
struct ShockClassifier {
    // Ring buffer for accelerometer samples (3-axis)
    float bufferX[ACCEL_WINDOW_SIZE];
    float bufferY[ACCEL_WINDOW_SIZE];
    float bufferZ[ACCEL_WINDOW_SIZE];
    int   bufferIndex;
    bool  bufferFull;

    // Last computed results
    ShockClass   lastClass;
    AccelFeatures lastFeatures;

    // -------------------------------------------------------
    //  Initialize. Call once in setup().
    // -------------------------------------------------------
    void init() {
        bufferIndex = 0;
        bufferFull  = false;
        lastClass   = SHOCK_NORMAL;
        memset(bufferX, 0, sizeof(bufferX));
        memset(bufferY, 0, sizeof(bufferY));
        memset(bufferZ, 0, sizeof(bufferZ));
        memset(&lastFeatures, 0, sizeof(lastFeatures));
    }

    // -------------------------------------------------------
    //  Add a new accelerometer sample (in G units).
    //  Call at 50Hz (every 20ms) from the main loop.
    // -------------------------------------------------------
    void addSample(float x, float y, float z) {
        bufferX[bufferIndex] = x;
        bufferY[bufferIndex] = y;
        bufferZ[bufferIndex] = z;
        bufferIndex++;
        if (bufferIndex >= ACCEL_WINDOW_SIZE) {
            bufferIndex = 0;
            bufferFull = true;
        }
    }

    // -------------------------------------------------------
    //  Extract statistical features from the current window.
    //  This is the "feature engineering" step — the features
    //  computed here can also feed a neural network (Tier 4).
    // -------------------------------------------------------
    AccelFeatures extractFeatures() {
        AccelFeatures f;
        memset(&f, 0, sizeof(f));

        int n = bufferFull ? ACCEL_WINDOW_SIZE : bufferIndex;
        if (n < 2) return f;

        // --- Pass 1: Compute magnitudes, min, max, sums ---
        float magnitudes[ACCEL_WINDOW_SIZE];
        float sumMag   = 0.0f;
        float sumMagSq = 0.0f;
        float minMag   = 9999.0f;
        float maxMag   = -9999.0f;

        for (int i = 0; i < n; i++) {
            float mag = sqrtf(bufferX[i] * bufferX[i] +
                              bufferY[i] * bufferY[i] +
                              bufferZ[i] * bufferZ[i]);
            magnitudes[i] = mag;
            sumMag   += mag;
            sumMagSq += mag * mag;
            if (mag < minMag) minMag = mag;
            if (mag > maxMag) maxMag = mag;
        }

        f.meanMagnitude = sumMag / n;
        f.maxMagnitude  = maxMag;
        f.peakToPeak    = maxMag - minMag;

        // --- RMS (Root Mean Square) ---
        f.rms = sqrtf(sumMagSq / n);

        // --- Pass 2: Variance and 4th central moment (for kurtosis) ---
        float variance     = 0.0f;
        float fourthMoment = 0.0f;

        for (int i = 0; i < n; i++) {
            float diff = magnitudes[i] - f.meanMagnitude;
            float diffSq = diff * diff;
            variance     += diffSq;
            fourthMoment += diffSq * diffSq;
        }
        variance /= n;

        // Excess kurtosis: positive = spiky/impulsive, negative = flat
        if (variance > 0.0001f) {
            f.kurtosis = (fourthMoment / n) / (variance * variance) - 3.0f;
        } else {
            f.kurtosis = 0.0f;
        }

        // --- Zero-Crossing Rate (oscillation frequency estimate) ---
        int crossings = 0;
        for (int i = 1; i < n; i++) {
            float prev = magnitudes[i - 1] - f.meanMagnitude;
            float curr = magnitudes[i]     - f.meanMagnitude;
            if (prev * curr < 0.0f) crossings++;
        }
        f.zeroCrossingRate = (float)crossings / (float)(n - 1);

        // --- Free-fall detection: longest run of low-G samples ---
        f.freeFallCount     = 0;
        int consecutiveLowG = 0;
        for (int i = 0; i < n; i++) {
            if (magnitudes[i] < THRESHOLD_FREE_FALL) {
                consecutiveLowG++;
                if (consecutiveLowG > f.freeFallCount) {
                    f.freeFallCount = consecutiveLowG;
                }
            } else {
                consecutiveLowG = 0;
            }
        }

        // --- Tilt angle from vertical (using latest sample) ---
        // Assumes Z-axis is "up" when device is level.
        // acos(z / magnitude) gives angle from vertical.
        int latest = (bufferIndex == 0) ? (n - 1) : (bufferIndex - 1);
        float latestMag = magnitudes[latest];
        if (latestMag > 0.1f) {
            float cosAngle = bufferZ[latest] / latestMag;
            // Clamp to [-1, 1] to avoid NaN from floating-point drift
            if (cosAngle > 1.0f)  cosAngle = 1.0f;
            if (cosAngle < -1.0f) cosAngle = -1.0f;
            f.tiltAngle = acosf(cosAngle) * 180.0f / M_PI;
        } else {
            f.tiltAngle = 0.0f;
        }

        lastFeatures = f;
        return f;
    }

    // -------------------------------------------------------
    //  Classify the current motion pattern.
    //  Priority-ordered: most dangerous events checked first.
    //  Call every ~1 second after the window is populated.
    // -------------------------------------------------------
    ShockClass classify() {
        // Need minimum data to classify
        if (!bufferFull && bufferIndex < 10) {
            lastClass = SHOCK_NORMAL;
            return SHOCK_NORMAL;
        }

        AccelFeatures f = extractFeatures();

        // --- Priority 1: Free-fall (most urgent — package is falling!) ---
        if (f.freeFallCount >= FREE_FALL_MIN_SAMPLES) {
            lastClass = SHOCK_FREE_FALL;
            return SHOCK_FREE_FALL;
        }

        // --- Priority 2: Major impact (severe damage risk) ---
        if (f.maxMagnitude > THRESHOLD_MAJOR_IMPACT) {
            lastClass = SHOCK_MAJOR_IMPACT;
            return SHOCK_MAJOR_IMPACT;
        }

        // --- Priority 3: Minor bump (impulsive jolt, moderate G) ---
        if (f.maxMagnitude > THRESHOLD_MINOR_BUMP &&
            f.kurtosis > THRESHOLD_BUMP_KURTOSIS) {
            lastClass = SHOCK_MINOR_BUMP;
            return SHOCK_MINOR_BUMP;
        }

        // --- Priority 4: Tilt alert (package orientation wrong) ---
        if (f.tiltAngle > THRESHOLD_TILT_ANGLE) {
            lastClass = SHOCK_TILT_ALERT;
            return SHOCK_TILT_ALERT;
        }

        // --- Priority 5: Vibration (sustained oscillation) ---
        if (f.rms > THRESHOLD_VIBRATION_RMS &&
            f.zeroCrossingRate > 0.3f) {
            lastClass = SHOCK_VIBRATION;
            return SHOCK_VIBRATION;
        }

        // --- Default: Normal ---
        lastClass = SHOCK_NORMAL;
        return SHOCK_NORMAL;
    }

    // -------------------------------------------------------
    //  Human-readable class name for serial logging.
    // -------------------------------------------------------
    const char* classToString(ShockClass c) const {
        switch (c) {
            case SHOCK_NORMAL:       return "NORMAL";
            case SHOCK_MINOR_BUMP:   return "MINOR_BUMP";
            case SHOCK_MAJOR_IMPACT: return "MAJOR_IMPACT";
            case SHOCK_FREE_FALL:    return "FREE_FALL";
            case SHOCK_TILT_ALERT:   return "TILT_ALERT";
            case SHOCK_VIBRATION:    return "VIBRATION";
            default:                 return "UNKNOWN";
        }
    }

    // -------------------------------------------------------
    //  Get the RMS from the last classification run.
    //  Used by the transport state machine for motion sensing.
    // -------------------------------------------------------
    float getRMS() const {
        return lastFeatures.rms;
    }
};

#endif // SHOCK_CLASSIFIER_H
