#ifndef EDGE_AI_CONFIG_H
#define EDGE_AI_CONFIG_H

// ============================================================
//  Edge AI Configuration — All Tunable Parameters
//  Adjust these values to match your specific asset type
//  (e.g., fragile electronics vs industrial equipment)
// ============================================================

// --- Anomaly Detection (Welford's + Z-Score) ---
#define ANOMALY_Z_THRESHOLD       3.0f    // Std deviations for anomaly flag
#define ANOMALY_EMA_ALPHA         0.1f    // EMA smoothing (0.1=slow, 0.3=fast)
#define ANOMALY_WARMUP_SAMPLES    10      // Min readings before detection activates

// --- Accelerometer Sampling ---
#define ACCEL_WINDOW_SIZE         50      // Samples per window (50 @ 50Hz = 1s)
#define ACCEL_SAMPLE_INTERVAL_MS  20      // 20ms = 50Hz sampling rate
#define CLASSIFY_INTERVAL_MS      1000    // Run classifier every 1 second

// --- Shock Thresholds (G-force) ---
#define THRESHOLD_FREE_FALL       0.3f    // Below this magnitude = free fall
#define THRESHOLD_MINOR_BUMP      1.5f    // Above this = minor bump
#define THRESHOLD_MAJOR_IMPACT    3.0f    // Above this = major impact
#define THRESHOLD_TILT_ANGLE      45.0f   // Degrees from vertical for alert
#define THRESHOLD_VIBRATION_RMS   0.5f    // RMS above this = vibration pattern
#define THRESHOLD_BUMP_KURTOSIS   1.0f    // Kurtosis above this confirms impulsive bump

// Free-fall minimum consecutive samples
#define FREE_FALL_MIN_SAMPLES     3       // ~60ms at 50Hz

// --- Transport State Machine ---
#define MOTION_RMS_THRESHOLD      0.15f   // RMS above this = motion detected
#define MOTION_CONFIRM_MS         5000    // 5s sustained motion → IN_TRANSIT
#define STATIONARY_CONFIRM_MS     30000   // 30s no motion → STATIONARY
#define LOADING_TIMEOUT_MS        60000   // 60s loading timeout → STATIONARY
#define IMPACT_RECOVER_MS         20000   // 20s auto-recover from IMPACT_EVENT

// --- Smart Alerting / Transmission Intervals ---
#define TRANSMIT_INTERVAL_NORMAL  20000   // 20s normal telemetry cycle
#define TRANSMIT_INTERVAL_LOW     60000   // 60s when stationary (power saving)
#define TRANSMIT_INTERVAL_MIN     15000   // 15s ThingSpeak free-tier rate limit
#define CRITICAL_COOLDOWN_MS      15000   // Min gap between critical alert sends

// --- Debug Output ---
#define EDGE_AI_DEBUG             1       // 1 = verbose serial logging, 0 = silent

#endif // EDGE_AI_CONFIG_H
