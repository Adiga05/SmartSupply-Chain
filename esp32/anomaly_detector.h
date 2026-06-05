#ifndef ANOMALY_DETECTOR_H
#define ANOMALY_DETECTOR_H

// ============================================================
//  Statistical Anomaly Detection using Welford's Online Algorithm
//  
//  Detects abnormal sensor readings (temperature spikes,
//  humidity excursions, cold-chain breaks) in real-time with
//  O(1) memory — no sliding window buffer required.
//
//  Two detection methods:
//    1. Z-Score: flags readings >N std deviations from running mean
//    2. EMA Drift: detects gradual drift away from moving average
//
//  Memory per instance: ~28 bytes (7 floats + 1 int + 1 bool)
// ============================================================

#include <math.h>
#include "edge_ai_config.h"

struct AnomalyDetector {
    // Welford's running statistics
    float mean;
    float m2;             // Sum of squared differences from mean
    
    // Exponential Moving Average
    float ema;
    float emaVariance;    // EMA of squared deviations
    
    // State
    int   count;
    float alpha;          // EMA smoothing factor
    float zThreshold;     // Z-score threshold for anomaly
    bool  warmedUp;       // True after enough samples collected

    // -------------------------------------------------------
    //  Initialize the detector. Call once in setup().
    // -------------------------------------------------------
    void init(float _alpha = ANOMALY_EMA_ALPHA,
              float _zThreshold = ANOMALY_Z_THRESHOLD) {
        mean        = 0.0f;
        m2          = 0.0f;
        ema         = 0.0f;
        emaVariance = 0.0f;
        count       = 0;
        alpha       = _alpha;
        zThreshold  = _zThreshold;
        warmedUp    = false;
    }

    // -------------------------------------------------------
    //  Feed a new sensor reading. Returns true if anomaly.
    //  Call this each time you get a valid sensor reading.
    // -------------------------------------------------------
    bool update(float value) {
        count++;

        // --- Welford's online mean & variance ---
        float delta  = value - mean;
        mean        += delta / count;
        float delta2 = value - mean;
        m2          += delta * delta2;

        // --- Exponential Moving Average ---
        if (count == 1) {
            ema         = value;
            emaVariance = 0.0f;
        } else {
            float emaDelta = value - ema;
            ema           += alpha * emaDelta;
            emaVariance    = (1.0f - alpha) * (emaVariance + alpha * emaDelta * emaDelta);
        }

        // --- Warmup check ---
        if (!warmedUp && count >= ANOMALY_WARMUP_SAMPLES) {
            warmedUp = true;
        }

        // Only flag anomalies after warmup period
        if (!warmedUp) return false;

        return (fabsf(getZScore(value)) > zThreshold);
    }

    // -------------------------------------------------------
    //  Get Z-score for a given value against running stats
    // -------------------------------------------------------
    float getZScore(float value) const {
        float sd = getStdDev();
        if (sd < 0.001f) return 0.0f;  // Avoid division by near-zero
        return (value - mean) / sd;
    }

    // -------------------------------------------------------
    //  Running standard deviation (sample variance)
    // -------------------------------------------------------
    float getStdDev() const {
        if (count < 2) return 0.0f;
        return sqrtf(m2 / (float)(count - 1));
    }

    // -------------------------------------------------------
    //  Current Exponential Moving Average
    // -------------------------------------------------------
    float getEMA() const {
        return ema;
    }

    // -------------------------------------------------------
    //  Standard deviation of the EMA
    // -------------------------------------------------------
    float getEMAStdDev() const {
        return sqrtf(emaVariance);
    }

    // -------------------------------------------------------
    //  Drift detection: is the value drifting away from EMA?
    //  Useful for detecting slow environmental changes.
    //  k = number of EMA-std-deviations for threshold
    // -------------------------------------------------------
    bool isDrifting(float value, float k = 2.0f) const {
        if (!warmedUp) return false;
        float emaStd = getEMAStdDev();
        if (emaStd < 0.001f) return false;
        return (fabsf(value - ema) > k * emaStd);
    }
};

#endif // ANOMALY_DETECTOR_H
