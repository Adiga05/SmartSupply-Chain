#include <Arduino.h>
#include "anomaly_detector.h"
#include "shock_classifier.h"
#include "transport_state.h"

// Instantiate Edge AI Objects
AnomalyDetector tempDetector(0.1);
AnomalyDetector humDetector(0.1);
ShockClassifier shockClassifier;
TransportFSM stateMachine(30000); // 30 second stationary timeout

// Timers for WiFi transmission
unsigned long lastTransmitTime = 0;
const unsigned long NORMAL_INTERVAL = 20000; // 20 seconds
const unsigned long LOW_INTERVAL = 60000;    // 60 seconds

// Dummy function to simulate reading temperature (replace with DHT/BME library)
float readTemperature() {
    return 25.0 + ((float)random(-10, 11) / 10.0);
}

// Dummy function to simulate reading humidity (replace with DHT/BME library)
float readHumidity() {
    return 50.0 + ((float)random(-20, 21) / 10.0);
}

// Dummy function to simulate reading accelerometer data at 50Hz (replace with MPU6050 library)
void readAccelerometer(float &x, float &y, float &z) {
    // Normal resting G-force (~1G on Z)
    x = ((float)random(-5, 6) / 100.0);
    y = ((float)random(-5, 6) / 100.0);
    z = 1.0 + ((float)random(-5, 6) / 100.0);
    
    // Occasionally simulate a major shock for testing
    if (random(0, 1000) > 995) {
        z = 5.0; // Major spike
    }
}

// Simulate WiFi transmission to ThingSpeak
void transmitToThingSpeak(float temp, float hum, ShockEvent shock, JourneyState state, float packedStatus) {
    Serial.println("\n>>> TRANSMITTING TO CLOUD <<<");
    Serial.print("Temp: "); Serial.print(temp);
    Serial.print(" | Hum: "); Serial.print(hum);
    Serial.print(" | Shock Event: "); Serial.print(ShockClassifier::getEventName(shock));
    Serial.print(" | Journey State: "); Serial.print(TransportFSM::getStateName(state));
    Serial.print(" | Packed AI: "); Serial.println(packedStatus, 2);
    Serial.println("-------------------------------------------------");
}

float packFeatures(float rms, float kurtosis, float zcr, float tilt) {
    int k = constrain((int)kurtosis, 0, 9);
    int z = constrain((int)zcr, 0, 99);
    int t = constrain((int)tilt, 0, 99);
    
    float integerPart = (k * 10000.0) + (z * 100.0) + t;
    float fractionPart = constrain(rms, 0.0f, 0.99f); 
    
    return integerPart + fractionPart;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); } // Wait for serial
    
    Serial.println("Starting Smart Supply Chain Edge AI node...");
    randomSeed(analogRead(0));
    
    // Pre-warm the Tier 1 anomaly detectors with a baseline
    Serial.println("Warming up environmental baseline...");
    for(int i=0; i<30; i++) {
        tempDetector.addReading(readTemperature());
        humDetector.addReading(readHumidity());
        delay(10);
    }
    Serial.println("Initialization complete.");
}

void loop() {
    unsigned long currentMillis = millis();
    static unsigned long lastSensorReadMillis = 0;
    const unsigned long SENSOR_INTERVAL = 20; // 50Hz sampling for accelerometer

    // 1. Read Accelerometer at high frequency (Tier 2 requirement)
    if (currentMillis - lastSensorReadMillis >= SENSOR_INTERVAL) {
        lastSensorReadMillis = currentMillis;

        // Evaluate temperature/humidity (Tier 1) at a lower frequency (e.g., 1 Hz)
        static int tempDivider = 0;
        float temp = tempDetector.getEMA();
        float hum = humDetector.getEMA();

        if (++tempDivider >= 50) { // 50 * 20ms = 1 second
            tempDivider = 0;
            temp = readTemperature();
            hum = readHumidity();
            
            tempDetector.addReading(temp);
            humDetector.addReading(hum);
            
            if (tempDetector.isAnomaly(temp)) {
                Serial.println("!!! TIER 1: TEMPERATURE ANOMALY DETECTED !!!");
            }
        }

        // Feed accelerometer data into Tier 2 Shock Classifier
        float ax, ay, az;
        readAccelerometer(ax, ay, az);
        
        bool windowFull = shockClassifier.addReading(ax, ay, az);
        
        // Once 1-second window is full, classify it
        if (windowFull) {
            ShockEvent currentEvent = shockClassifier.classify();
            
            if (currentEvent != NORMAL) {
                Serial.print("TIER 2: Event Detected: ");
                Serial.println(ShockClassifier::getEventName(currentEvent));
            }

            // 2. Update Tier 3 State Machine based on physical events
            stateMachine.updateWithEvent(currentEvent, currentMillis);
            
            // Allow simulated RFID scan via Serial input
            if (Serial.available() > 0) {
                char c = Serial.read();
                if (c == 'r' || c == 'R') {
                    Serial.println(">>> RFID SCANNED <<<");
                    stateMachine.triggerRFIDScan(currentMillis);
                }
            }

            // 3. Smart Alerting: Determine if we should transmit NOW based on State + Priority
            AlertPriority priority = stateMachine.getTransmissionPriority(currentEvent);
            JourneyState currentState = stateMachine.getState();
            bool shouldTransmit = false;

            if (priority == CRITICAL_PRIORITY) {
                shouldTransmit = true; // Break schedule immediately
            } 
            else if (priority == NORMAL_PRIORITY && (currentMillis - lastTransmitTime >= NORMAL_INTERVAL)) {
                shouldTransmit = true; // Transmit on normal schedule
            } 
            else if (priority == LOW_PRIORITY && (currentMillis - lastTransmitTime >= LOW_INTERVAL)) {
                shouldTransmit = true; // Transmit on slow schedule
            }

            if (shouldTransmit) {
                float rms, kurt, zcr, tilt;
                shockClassifier.extractFeatures(rms, kurt, zcr, tilt);
                float packedStatus = packFeatures(rms, kurt, zcr, tilt);
                
                transmitToThingSpeak(temp, hum, currentEvent, currentState, packedStatus);
                lastTransmitTime = currentMillis;
            }
        }
    }
}
