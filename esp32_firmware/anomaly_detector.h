#ifndef ANOMALY_DETECTOR_H
#define ANOMALY_DETECTOR_H

#include <math.h>

class AnomalyDetector {
private:
    unsigned long count;
    float mean;
    float m2; // Sum of squares of differences from the current mean
    float ema; // Exponential Moving Average for tracking slow drift
    float alpha; // EMA alpha coefficient

public:
    AnomalyDetector(float emaAlpha = 0.1) : count(0), mean(0.0), m2(0.0), ema(0.0), alpha(emaAlpha) {}

    // Welford's Online Algorithm to update running mean and variance
    void addReading(float value) {
        count++;
        
        // Update EMA
        if (count == 1) {
            ema = value;
        } else {
            ema = (alpha * value) + ((1.0 - alpha) * ema);
        }

        // Update Welford's statistics
        float delta = value - mean;
        mean += delta / count;
        float delta2 = value - mean;
        m2 += delta * delta2;
    }

    float getVariance() const {
        if (count < 2) return 0.0;
        return m2 / (count - 1); // Sample variance
    }

    float getStandardDeviation() const {
        return sqrt(getVariance());
    }

    float getMean() const {
        return mean;
    }
    
    float getEMA() const {
        return ema;
    }

    // Calculate the Z-score for a given value based on current statistics
    float getZScore(float value) const {
        if (count < 2) return 0.0;
        float stddev = getStandardDeviation();
        if (stddev == 0.0) return 0.0; // Avoid division by zero
        return fabs(value - mean) / stddev;
    }

    // Check if a value is an anomaly based on Z-score threshold
    bool isAnomaly(float value, float zThreshold = 3.0) const {
        // Need a baseline (e.g., at least 10 readings) before calling true anomalies
        if (count < 10) return false; 
        return getZScore(value) > zThreshold;
    }
    
    // Reset the detector if the environment drastically changes
    void reset() {
        count = 0;
        mean = 0.0;
        m2 = 0.0;
        ema = 0.0;
    }
};

#endif // ANOMALY_DETECTOR_H
