// ============================================================
//  ESP32 Edge AI Asset Tracker
//
//  An intelligent logistics monitoring system that runs edge AI
//  directly on the ESP32 to detect anomalies, classify impacts,
//  track transport state, and make smart alerting decisions —
//  all without cloud dependency.
//
//  Sensors: BME280, MPU6050, GPS (Serial2), MFRC522 RFID
//  Cloud:   ThingSpeak (8 fields + status string)
//
//  Edge AI Features:
//    Tier 1 — Statistical anomaly detection (temp/humidity/pressure)
//    Tier 2 — Accelerometer shock/impact classification
//    Tier 3 — Transport state machine + smart alerting
// ============================================================

// --- Edge AI Modules (must come first for macros used in conditional includes) ---
#include "edge_ai_config.h"

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <ThingSpeak.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <MPU6050_light.h>
#if LOCATION_SOURCE == LOC_SOURCE_GPS
#include <TinyGPS++.h>
#elif LOCATION_SOURCE == LOC_SOURCE_WIFI_IP
#include <HTTPClient.h>
#endif
#include <MFRC522.h>

#include "anomaly_detector.h"
#include "shock_classifier.h"
#include "transport_state.h"

// ========================== CREDENTIALS ==========================
const char* ssid           = "CMFpro";
const char* password       = "kiranskh";
unsigned long myChannelNumber = 3399994;
const char* myWriteAPIKey  = "P355FVMTNZJJHIJC";

WiFiClient client;

// ========================== HARDWARE =============================
Adafruit_BME280 bme;   // I2C BME280 (shares bus with MPU6050)

MPU6050    mpu(Wire);
#if LOCATION_SOURCE == LOC_SOURCE_GPS
TinyGPSPlus gps;
#endif
MFRC522    rfid(5, 4);   // SS = GPIO 5, RST = GPIO 4

// ========================== EDGE AI ==============================
AnomalyDetector tempDetector;     // Temperature anomaly detector
AnomalyDetector humDetector;      // Humidity anomaly detector
AnomalyDetector pressDetector;    // Pressure anomaly detector
ShockClassifier shockClassifier;  // Accelerometer shock classifier
SmartAlerter    alerter;          // Transport state + smart alerting

// ========================== TIMING ===============================
unsigned long lastTransmitTime    = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastClassifyTime    = 0;

// ========================== SENSOR DATA ==========================
float  tempC    = 0.0f;
float  humidity = 0.0f;
float  pressure = 0.0f;   // Pressure in hPa
double latitude = 0.0;
double longitude = 0.0;

#if LOCATION_SOURCE == LOC_SOURCE_WIFI_IP
bool fetchLocationByIP(double &lat, double &lon) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
    HTTPClient http;
    http.setTimeout(5000); // 5 second timeout
    http.begin("http://ip-api.com/json/");
    int httpCode = http.GET();
    
    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("[GeoIP] Response: " + payload);
        
        int latPos = payload.indexOf("\"lat\":");
        int lonPos = payload.indexOf("\"lon\":");
        
        if (latPos != -1 && lonPos != -1) {
            int latStart = latPos + 6;
            int latEnd = payload.indexOf(",", latStart);
            if (latEnd == -1) latEnd = payload.indexOf("}", latStart);
            
            int lonStart = lonPos + 6;
            int lonEnd = payload.indexOf(",", lonStart);
            if (lonEnd == -1) lonEnd = payload.indexOf("}", lonStart);
            
            if (latEnd != -1 && lonEnd != -1) {
                String latStr = payload.substring(latStart, latEnd);
                String lonStr = payload.substring(lonStart, lonEnd);
                latStr.trim();
                lonStr.trim();
                lat = latStr.toDouble();
                lon = lonStr.toDouble();
                success = true;
            }
        }
    } else {
        Serial.printf("[GeoIP] HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
    return success;
}
#endif

unsigned long lastScannedRFID = 0;
bool   newRFIDScanned  = false;

// ========================== AI STATE =============================
ShockClass currentShockClass = SHOCK_NORMAL;
bool  tempAnomaly  = false;
bool  humAnomaly   = false;
bool  pressAnomaly = false;
float peakG       = 0.0f;   // Peak G-force since last transmission

// =================================================================
//  SETUP
// =================================================================
void setup() {
    Serial.begin(115200);
#if LOCATION_SOURCE == LOC_SOURCE_GPS
    Serial2.begin(9600, SERIAL_8N1, 16, 17);  // GPS on UART2
#endif

    // --- WiFi ---
    WiFi.begin(ssid, password);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWi-Fi Connected!");

#if LOCATION_SOURCE == LOC_SOURCE_WIFI_IP
    Serial.println("[GeoIP] Fetching location from IP...");
    if (fetchLocationByIP(latitude, longitude)) {
        Serial.printf("[GeoIP] Successfully retrieved location: %.6f, %.6f\n", latitude, longitude);
    } else {
        Serial.println("[GeoIP] Failed to fetch location. Defaulting to 0.0, 0.0.");
    }
#endif

    // --- Peripherals ---
    ThingSpeak.begin(client);
    Wire.begin();
    SPI.begin();
    rfid.PCD_Init();
    delay(4);
    rfid.PCD_DumpVersionToSerial(); // Add diagnostic output for RFID hardware

    // --- BME280 ---
    if (!bme.begin(0x76)) {   // Default I2C address; use 0x77 if SDO is high
        Serial.println("BME280 NOT FOUND! Check wiring.");
    } else {
        Serial.println("BME280 Initialized.");
    }

    // --- MPU6050 ---
    byte mpuStatus = mpu.begin();
    if (mpuStatus != 0) {
        Serial.print("MPU6050 FAILED! Error: ");
        Serial.println(mpuStatus);
    } else {
        Serial.println("MPU6050 connected. Calculating offsets...");
        delay(1000);
        mpu.calcOffsets(true, true);  // KEEP DEVICE STILL DURING BOOT
        Serial.println("MPU6050 offsets calculated.");
    }

    // --- Edge AI Initialization ---
    tempDetector.init(ANOMALY_EMA_ALPHA, ANOMALY_Z_THRESHOLD);
    humDetector.init(ANOMALY_EMA_ALPHA, ANOMALY_Z_THRESHOLD);
    pressDetector.init(ANOMALY_EMA_ALPHA, ANOMALY_Z_THRESHOLD);
    shockClassifier.init();
    alerter.init();

    // --- Boot Banner ---
    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║     ESP32 Edge AI Asset Tracker v1.0     ║");
    Serial.println("╠══════════════════════════════════════════╣");
    Serial.println("║  [✓] Anomaly Detection     (Tier 1)     ║");
    Serial.println("║  [✓] Shock Classification  (Tier 2)     ║");
    Serial.println("║  [✓] Transport State FSM   (Tier 3)     ║");
    Serial.println("║  [✓] Smart Alerting        (Tier 3)     ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.printf("  Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("  Accel sample rate: %d Hz\n", 1000 / ACCEL_SAMPLE_INTERVAL_MS);
    Serial.printf("  Window size: %d samples (%d ms)\n",
                  ACCEL_WINDOW_SIZE,
                  ACCEL_WINDOW_SIZE * ACCEL_SAMPLE_INTERVAL_MS);
    Serial.println();
}

// =================================================================
//  MAIN LOOP
// =================================================================
void loop() {
    unsigned long now = millis();

    // ─────────────────────────────────────────────────────────────
    //  1. CONTINUOUS: Update MPU6050 internal state
    // ─────────────────────────────────────────────────────────────
    mpu.update();

    // ─────────────────────────────────────────────────────────────
    //  2. CONTROLLED 50Hz: Sample accelerometer into AI buffer
    // ─────────────────────────────────────────────────────────────
    if (now - lastAccelSampleTime >= ACCEL_SAMPLE_INTERVAL_MS) {
        float ax = mpu.getAccX();
        float ay = mpu.getAccY();
        float az = mpu.getAccZ();

        // Feed into shock classifier's ring buffer
        shockClassifier.addSample(ax, ay, az);

        // Track peak G-force for this transmission window
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        if (mag > peakG) peakG = mag;

        lastAccelSampleTime = now;
    }

    // ─────────────────────────────────────────────────────────────
    //  3. EVERY 1 SECOND: Run shock classification
    // ─────────────────────────────────────────────────────────────
    if (now - lastClassifyTime >= CLASSIFY_INTERVAL_MS) {
        currentShockClass = shockClassifier.classify();

        #if EDGE_AI_DEBUG
        if (currentShockClass != SHOCK_NORMAL) {
            AccelFeatures& f = shockClassifier.lastFeatures;
            Serial.printf("[SHOCK] %-12s | RMS=%.3f PkPk=%.3f Kurt=%.2f ZCR=%.2f Tilt=%.1f° MaxG=%.2f\n",
                shockClassifier.classToString(currentShockClass),
                f.rms, f.peakToPeak, f.kurtosis,
                f.zeroCrossingRate, f.tiltAngle, f.maxMagnitude);
        }
        #endif

        lastClassifyTime = now;
    }

#if LOCATION_SOURCE == LOC_SOURCE_GPS
    // ─────────────────────────────────────────────────────────────
    //  4. CONTINUOUS: Poll GPS buffer
    // ─────────────────────────────────────────────────────────────
    while (Serial2.available() > 0) {
        gps.encode(Serial2.read());
    }
#endif

    // ─────────────────────────────────────────────────────────────
    //  5. CONTINUOUS: Poll RFID scanner
    // ─────────────────────────────────────────────────────────────
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        lastScannedRFID = ((unsigned long)rfid.uid.uidByte[0] << 24) |
                          ((unsigned long)rfid.uid.uidByte[1] << 16) |
                          ((unsigned long)rfid.uid.uidByte[2] << 8)  |
                          ((unsigned long)rfid.uid.uidByte[3]);
        newRFIDScanned = true;
        Serial.printf("[RFID] Tag scanned: %lu\n", lastScannedRFID);
        rfid.PICC_HaltA();
    }

    // ─────────────────────────────────────────────────────────────
    //  6. EVERY LOOP: Update transport state machine
    //     The FSM processes shock class, RFID events, anomaly
    //     flags, and motion RMS to determine the current
    //     operational state and alert priority.
    // ─────────────────────────────────────────────────────────────
    alerter.updateState(
        currentShockClass,
        newRFIDScanned,
        tempAnomaly,
        humAnomaly,
        pressAnomaly,
        shockClassifier.getRMS(),
        now
    );
    newRFIDScanned = false;  // Consume the RFID event

    // ─────────────────────────────────────────────────────────────
    //  7. SMART TRANSMISSION: AI decides when to send telemetry
    //     - CRITICAL: send immediately (impact, free-fall)
    //     - HIGH: send on next normal cycle
    //     - NORMAL: every 20 seconds
    //     - LOW: every 60 seconds (stationary, save power)
    // ─────────────────────────────────────────────────────────────
    if (alerter.shouldTransmitNow(now, lastTransmitTime)) {
        transmitTelemetry(now);
    }
}

// =================================================================
//  TELEMETRY TRANSMISSION
//  Reads BME280, runs anomaly detection, packs all AI outputs
//  into ThingSpeak fields, and pushes to the cloud.
// =================================================================
void transmitTelemetry(unsigned long now) {

    // --- Read BME280 sensor ---
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0f;  // Pa → hPa

    // BME280 returns valid floats; update and analyze
    if (!isnan(t)) {
        tempC = t;
        tempAnomaly = tempDetector.update(tempC);
    }
    if (!isnan(h)) {
        humidity = h;
        humAnomaly = humDetector.update(humidity);
    }
    if (!isnan(p)) {
        pressure = p;
        pressAnomaly = pressDetector.update(pressure);
    }

    // --- Log anomalies ---
    #if EDGE_AI_DEBUG
    if (tempAnomaly) {
        Serial.printf("[ANOMALY] Temp: %.1f°C | Z=%.2f | mean=%.1f | std=%.2f | EMA=%.1f\n",
            tempC, tempDetector.getZScore(tempC),
            tempDetector.mean, tempDetector.getStdDev(),
            tempDetector.getEMA());
    }
    if (humAnomaly) {
        Serial.printf("[ANOMALY] Hum: %.1f%% | Z=%.2f | mean=%.1f | std=%.2f | EMA=%.1f\n",
            humidity, humDetector.getZScore(humidity),
            humDetector.mean, humDetector.getStdDev(),
            humDetector.getEMA());
    }
    if (pressAnomaly) {
        Serial.printf("[ANOMALY] Press: %.1f hPa | Z=%.2f | mean=%.1f | std=%.2f | EMA=%.1f\n",
            pressure, pressDetector.getZScore(pressure),
            pressDetector.mean, pressDetector.getStdDev(),
            pressDetector.getEMA());
    }
    // Log drift detection
    if (tempDetector.isDrifting(tempC)) {
        Serial.printf("[DRIFT]  Temperature drifting from EMA (%.1f vs EMA %.1f)\n",
            tempC, tempDetector.getEMA());
    }
    if (humDetector.isDrifting(humidity)) {
        Serial.printf("[DRIFT]  Humidity drifting from EMA (%.1f vs EMA %.1f)\n",
            humidity, humDetector.getEMA());
    }
    if (pressDetector.isDrifting(pressure)) {
        Serial.printf("[DRIFT]  Pressure drifting from EMA (%.1f vs EMA %.1f)\n",
            pressure, pressDetector.getEMA());
    }
    #endif

#if LOCATION_SOURCE == LOC_SOURCE_GPS
    // --- Update GPS ---
    if (gps.location.isValid()) {
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
    }
#endif

    // --- Compute composite anomaly score (3-factor average) ---
    float tempZ  = tempDetector.warmedUp  ? fabsf(tempDetector.getZScore(tempC))     : 0.0f;
    float humZ   = humDetector.warmedUp   ? fabsf(humDetector.getZScore(humidity))   : 0.0f;
    float pressZ = pressDetector.warmedUp ? fabsf(pressDetector.getZScore(pressure)) : 0.0f;
    float anomalyScore = (tempZ + humZ + pressZ) / 3.0f;

    // ─────────────────────────────────────────────────────────────
    //  ThingSpeak Field Mapping:
    //
    //  Field 1: Temperature (°C)              — raw sensor
    //  Field 2: Humidity (%)                   — raw sensor
    //  Field 3: Pressure (hPa)                — raw sensor
    //  Field 4: Latitude                       — GPS
    //  Field 5: Longitude                      — GPS
    //  Field 6: Packed Status (SSCCTAP)        — AI state encoding
    //  Field 7: Peak G-force                   — max G since last TX
    //  Field 8: Anomaly Score                  — avg |Z-score|
    // ─────────────────────────────────────────────────────────────
    ThingSpeak.setField(1, tempC);
    ThingSpeak.setField(2, humidity);
    ThingSpeak.setField(3, pressure);
    ThingSpeak.setField(4, String(latitude, 8));
    ThingSpeak.setField(5, String(longitude, 8));
    ThingSpeak.setField(6, alerter.packStatusField());
    ThingSpeak.setField(7, peakG);
    ThingSpeak.setField(8, anomalyScore);

    // --- Status string (human-readable for ThingSpeak dashboard) ---
    String status = String(alerter.stateToString(alerter.currentState)) + "|" +
                    String(shockClassifier.classToString(currentShockClass)) + "|" +
                    String(alerter.priorityToString(alerter.pendingPriority)) + "|" +
                    String(lastScannedRFID);
    ThingSpeak.setStatus(status);

    // --- Push to cloud ---
    int httpCode = ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);

    if (httpCode == 200) {
        Serial.println("────────────────────────────────────────");
        Serial.printf("[TX] SUCCESS | Priority: %s\n",
            alerter.priorityToString(alerter.pendingPriority));
        Serial.printf("     State:  %s\n",
            alerter.stateToString(alerter.currentState));
        Serial.printf("     Shock:  %s (class %d)\n",
            shockClassifier.classToString(currentShockClass),
            (int)currentShockClass);
        Serial.printf("     Temp:   %.1f°C  Hum: %.1f%%  Press: %.1f hPa\n", tempC, humidity, pressure);
        Serial.printf("     PeakG:  %.2f   AnomalyScore: %.2f\n", peakG, anomalyScore);
        Serial.printf("     GPS:    %.8f, %.8f\n", latitude, longitude);
        Serial.printf("     Status: %s\n", status.c_str());
        Serial.println("────────────────────────────────────────");
    } else {
        Serial.printf("[TX] FAILED | HTTP: %d\n", httpCode);
    }

    // --- Reset per-window metrics ---
    peakG = 0.0f;
    if (alerter.pendingPriority == PRIORITY_CRITICAL) {
        alerter.lastCriticalSend = now;
    }

    // --- Clear permanent latches after transmission ---
    // The event has been pushed to cloud; resume normal classification
    shockClassifier.permanentMajorImpact = false;
    shockClassifier.permanentMinorBump = false;
    shockClassifier.resetInstantPeaks();
    currentShockClass = SHOCK_NORMAL;

    lastTransmitTime = now;
}