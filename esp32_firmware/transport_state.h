#ifndef TRANSPORT_STATE_H
#define TRANSPORT_STATE_H

#include "shock_classifier.h"

enum JourneyState {
    STATIONARY,
    LOADING,
    IN_TRANSIT
};

enum AlertPriority {
    LOW_PRIORITY,    // Send infrequently to save battery (e.g., every 60s)
    NORMAL_PRIORITY, // Send standard tracking updates (e.g., every 20s)
    CRITICAL_PRIORITY// Send IMMEDIATELY, breaking the schedule loop
};

class TransportFSM {
private:
    JourneyState currentState;
    unsigned long lastMotionTime;
    unsigned long stationaryTimeout; // e.g., 30000ms (30 seconds)

public:
    TransportFSM(unsigned long timeoutMs = 30000) 
        : currentState(STATIONARY), lastMotionTime(0), stationaryTimeout(timeoutMs) {}

    // Process a new shock event to update the state machine
    void updateWithEvent(ShockEvent event, unsigned long currentTimeMs) {
        if (event != NORMAL && event != FREE_FALL) {
            // Any physical motion or impact resets the stationary timer
            lastMotionTime = currentTimeMs;
            
            if (currentState == STATIONARY) {
                // If we were stationary and start moving, default to in transit.
                // A true supply chain system might wait for an RFID scan to enter LOADING.
                currentState = IN_TRANSIT; 
            }
        }

        // Check for timeout to return to STATIONARY
        if (currentState == IN_TRANSIT || currentState == LOADING) {
            if ((currentTimeMs - lastMotionTime) > stationaryTimeout) {
                currentState = STATIONARY;
            }
        }
    }

    // Explicitly trigger a loading state (e.g., via RFID scan)
    void triggerRFIDScan(unsigned long currentTimeMs) {
        currentState = LOADING;
        lastMotionTime = currentTimeMs; // Reset timeout
    }

    JourneyState getState() const {
        return currentState;
    }

    // Smart Alerting Logic based on Tier 2 events and Tier 3 state
    AlertPriority getTransmissionPriority(ShockEvent currentEvent) const {
        // 1. CRITICAL: Immediate transmit needed
        if (currentEvent == FREE_FALL || currentEvent == MAJOR_IMPACT || currentEvent == TILT_ALERT) {
            return CRITICAL_PRIORITY;
        }

        // 2. NORMAL: Regular tracking while actively moving
        if (currentState == IN_TRANSIT || currentState == LOADING) {
            return NORMAL_PRIORITY;
        }

        // 3. LOW: Not moving, nothing happening, save battery
        return LOW_PRIORITY;
    }

    // Helper to get string name of state
    static const char* getStateName(JourneyState state) {
        switch(state) {
            case STATIONARY: return "STATIONARY";
            case LOADING: return "LOADING";
            case IN_TRANSIT: return "IN_TRANSIT";
            default: return "UNKNOWN";
        }
    }
};

#endif // TRANSPORT_STATE_H
