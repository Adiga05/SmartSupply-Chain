#ifndef TRANSPORT_STATE_H
#define TRANSPORT_STATE_H

// ============================================================
//  Transport State Machine & Smart Alerting Engine
//
//  Tracks the operational state of the asset through a finite
//  state machine (Stationary → In-Transit → Impact, etc.) and
//  uses a priority-based alerting engine to decide WHEN and
//  HOW URGENTLY to transmit telemetry to the cloud.
//
//  States:
//    STATIONARY   — No motion detected for 30+ seconds
//    IN_TRANSIT   — Sustained motion for 5+ seconds
//    IMPACT_EVENT — Major shock/free-fall (auto-recovers in 5s)
//    LOADING      — RFID scanned while stationary
//    ENV_ALERT    — Temperature or humidity anomaly during transit
//
//  Alert Priorities:
//    CRITICAL — Immediate transmission (impact, free-fall, env anomaly)
//    HIGH     — Transmit on next normal cycle, flagged
//    NORMAL   — Regular 20-second interval
//    LOW      — Extended 60-second interval (power saving)
//
//  Memory: ~48 bytes
// ============================================================

#include "edge_ai_config.h"
#include "shock_classifier.h"

// --- Transport States ---
enum TransportState {
    STATE_STATIONARY    = 0,
    STATE_IN_TRANSIT    = 1,
    STATE_IMPACT_EVENT  = 2,
    STATE_LOADING       = 3,
    STATE_ENV_ALERT     = 4
};

// --- Alert Priority Levels ---
enum AlertPriority {
    PRIORITY_LOW      = 0,    // Extend interval to 60s (stationary)
    PRIORITY_NORMAL   = 1,    // Standard 20s interval
    PRIORITY_HIGH     = 2,    // Next cycle, flagged in status
    PRIORITY_CRITICAL  = 3    // Transmit ASAP (respecting rate limit)
};

// --- Snapshot of latest AI event (for telemetry packing) ---
struct AlertEvent {
    AlertPriority  priority;
    ShockClass     shockClass;
    bool           tempAnomaly;
    bool           humAnomaly;
    TransportState state;
};

// --- Smart Alerter (State Machine + Priority Engine) ---
struct SmartAlerter {
    TransportState currentState;
    TransportState previousState;
    AlertPriority  pendingPriority;
    
    unsigned long  stateEntryTime;     // When current state was entered
    unsigned long  lastCriticalSend;   // Last critical priority send time
    unsigned long  motionStartTime;    // When continuous motion began
    unsigned long  stillStartTime;     // When continuous stillness began
    bool           motionDetected;     // Is there motion right now?
    
    AlertEvent     lastEvent;          // Snapshot for telemetry

    // -------------------------------------------------------
    //  Initialize. Call once in setup().
    // -------------------------------------------------------
    void init() {
        currentState    = STATE_STATIONARY;
        previousState   = STATE_STATIONARY;
        pendingPriority = PRIORITY_LOW;
        stateEntryTime  = 0;
        lastCriticalSend = 0;
        motionStartTime = 0;
        stillStartTime  = 0;
        motionDetected  = false;
        memset(&lastEvent, 0, sizeof(lastEvent));
    }

    // -------------------------------------------------------
    //  Update state machine with latest sensor readings.
    //  Call this every loop iteration (or every classify cycle).
    //
    //  Parameters:
    //    shock       - Current shock classification
    //    rfidScanned - True if a new RFID tag was just scanned
    //    tempAnomaly - True if temperature Z-score exceeded threshold
    //    humAnomaly  - True if humidity Z-score exceeded threshold
    //    rms         - Current accelerometer RMS from shock classifier
    //    now         - Current millis() timestamp
    // -------------------------------------------------------
    void updateState(ShockClass shock, bool rfidScanned,
                     bool tempAnomaly, bool humAnomaly,
                     float rms, unsigned long now) {

        previousState = currentState;
        bool isMoving = (rms > MOTION_RMS_THRESHOLD);

        // --- Track motion/stillness duration ---
        if (isMoving) {
            if (!motionDetected) {
                motionStartTime = now;
                motionDetected = true;
            }
            stillStartTime = 0;
        } else {
            if (motionDetected) {
                stillStartTime = now;
                motionDetected = false;
            }
            motionStartTime = 0;
        }

        // --- State transition logic ---
        switch (currentState) {

            case STATE_STATIONARY:
                // RFID scan while stationary → loading/unloading
                if (rfidScanned) {
                    transitionTo(STATE_LOADING, now);
                }
                // Sustained motion → in transit
                else if (isMoving && motionStartTime > 0 &&
                         (now - motionStartTime >= MOTION_CONFIRM_MS)) {
                    transitionTo(STATE_IN_TRANSIT, now);
                }
                break;

            case STATE_IN_TRANSIT:
                // Major shock or free-fall → impact event
                if (shock == SHOCK_MAJOR_IMPACT || shock == SHOCK_FREE_FALL) {
                    transitionTo(STATE_IMPACT_EVENT, now);
                }
                // Environmental anomaly → alert state
                else if (tempAnomaly || humAnomaly) {
                    transitionTo(STATE_ENV_ALERT, now);
                }
                // Sustained stillness → stationary
                else if (!isMoving && stillStartTime > 0 &&
                         (now - stillStartTime >= STATIONARY_CONFIRM_MS)) {
                    transitionTo(STATE_STATIONARY, now);
                }
                break;

            case STATE_IMPACT_EVENT:
                // Auto-recover after timeout
                if (now - stateEntryTime >= IMPACT_RECOVER_MS) {
                    transitionTo(STATE_IN_TRANSIT, now);
                }
                break;

            case STATE_LOADING:
                // Motion detected → departing, now in transit
                if (isMoving && motionStartTime > 0 &&
                    (now - motionStartTime >= MOTION_CONFIRM_MS)) {
                    transitionTo(STATE_IN_TRANSIT, now);
                }
                // Timeout → return to stationary
                else if (now - stateEntryTime >= LOADING_TIMEOUT_MS) {
                    transitionTo(STATE_STATIONARY, now);
                }
                break;

            case STATE_ENV_ALERT:
                // Anomaly cleared → return to transit
                if (!tempAnomaly && !humAnomaly) {
                    transitionTo(STATE_IN_TRANSIT, now);
                }
                break;
        }

        // --- Evaluate alert priority ---
        evaluatePriority(shock, tempAnomaly, humAnomaly);

        // --- Store event snapshot ---
        lastEvent.priority    = pendingPriority;
        lastEvent.shockClass  = shock;
        lastEvent.tempAnomaly = tempAnomaly;
        lastEvent.humAnomaly  = humAnomaly;
        lastEvent.state       = currentState;
    }

    // -------------------------------------------------------
    //  Internal: transition to a new state with debug logging.
    // -------------------------------------------------------
    void transitionTo(TransportState newState, unsigned long now) {
        previousState  = currentState;
        currentState   = newState;
        stateEntryTime = now;

        #if EDGE_AI_DEBUG
        Serial.print("[STATE] ");
        Serial.print(stateToString(previousState));
        Serial.print(" -> ");
        Serial.println(stateToString(newState));
        #endif
    }

    // -------------------------------------------------------
    //  Internal: determine how urgently we need to transmit.
    // -------------------------------------------------------
    void evaluatePriority(ShockClass shock, bool tempAnomaly, bool humAnomaly) {
        // CRITICAL: major impact, free-fall, or combined env anomaly
        if (shock == SHOCK_MAJOR_IMPACT || shock == SHOCK_FREE_FALL ||
            (tempAnomaly && humAnomaly)) {
            pendingPriority = PRIORITY_CRITICAL;
            return;
        }

        // HIGH: minor bump, tilt, single env anomaly, or state change
        if (shock == SHOCK_MINOR_BUMP || shock == SHOCK_TILT_ALERT ||
            tempAnomaly || humAnomaly ||
            currentState != previousState) {
            pendingPriority = PRIORITY_HIGH;
            return;
        }

        // LOW: stationary and nothing happening
        if (currentState == STATE_STATIONARY && shock == SHOCK_NORMAL) {
            pendingPriority = PRIORITY_LOW;
            return;
        }

        // NORMAL: everything else (routine transit data)
        pendingPriority = PRIORITY_NORMAL;
    }

    // -------------------------------------------------------
    //  Should we transmit telemetry RIGHT NOW?
    //  Respects ThingSpeak rate limits while allowing critical
    //  events to bypass the normal 20s timer.
    //
    //  Parameters:
    //    now          - Current millis()
    //    lastTransmit - millis() of last successful transmission
    //
    //  Returns: true if telemetry should be sent now
    // -------------------------------------------------------
    bool shouldTransmitNow(unsigned long now, unsigned long lastTransmit) {
        unsigned long elapsed = now - lastTransmit;

        // Hard floor: never violate ThingSpeak rate limit
        if (elapsed < TRANSMIT_INTERVAL_MIN) return false;

        switch (pendingPriority) {
            case PRIORITY_CRITICAL:
                // Send ASAP, but respect cooldown between critical sends
                if (now - lastCriticalSend >= CRITICAL_COOLDOWN_MS) {
                    return true;
                }
                return false;

            case PRIORITY_HIGH:
                // Send on next normal cycle (no extended wait)
                return (elapsed >= TRANSMIT_INTERVAL_NORMAL);

            case PRIORITY_NORMAL:
                return (elapsed >= TRANSMIT_INTERVAL_NORMAL);

            case PRIORITY_LOW:
                // Extended interval to save power/bandwidth
                return (elapsed >= TRANSMIT_INTERVAL_LOW);
        }

        return (elapsed >= TRANSMIT_INTERVAL_NORMAL);
    }

    // -------------------------------------------------------
    //  Pack the current state, shock class, and anomaly flags
    //  into a single float for ThingSpeak Field 6.
    //
    //  Format: SSCCTA (decimal digits)
    //    SS = state (0-4)     × 10000
    //    CC = shockClass (0-5) × 100
    //    T  = tempAnomaly      × 10
    //    A  = humAnomaly       × 1
    //
    //  Example: 10201 = IN_TRANSIT, MAJOR_IMPACT, no temp, yes hum
    // -------------------------------------------------------
    float packStatusField() const {
        return (float)(
            (int)lastEvent.state      * 10000 +
            (int)lastEvent.shockClass * 100 +
            (lastEvent.tempAnomaly ? 10 : 0) +
            (lastEvent.humAnomaly  ?  1 : 0)
        );
    }

    // -------------------------------------------------------
    //  Human-readable string helpers for serial logging.
    // -------------------------------------------------------
    const char* stateToString(TransportState s) const {
        switch (s) {
            case STATE_STATIONARY:   return "STATIONARY";
            case STATE_IN_TRANSIT:   return "IN_TRANSIT";
            case STATE_IMPACT_EVENT: return "IMPACT_EVENT";
            case STATE_LOADING:      return "LOADING";
            case STATE_ENV_ALERT:    return "ENV_ALERT";
            default:                 return "UNKNOWN";
        }
    }

    const char* priorityToString(AlertPriority p) const {
        switch (p) {
            case PRIORITY_LOW:      return "LOW";
            case PRIORITY_NORMAL:   return "NORMAL";
            case PRIORITY_HIGH:     return "HIGH";
            case PRIORITY_CRITICAL: return "CRITICAL";
            default:                return "UNKNOWN";
        }
    }
};

#endif // TRANSPORT_STATE_H
