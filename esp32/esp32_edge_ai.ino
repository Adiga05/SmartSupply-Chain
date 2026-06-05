// ============================================================
//  ESP32 Edge AI Asset Tracker
//
//  An intelligent logistics monitoring system that runs edge AI
//  directly on the ESP32 to detect anomalies, classify impacts,
//  track transport state, and make smart alerting decisions —
//  all without cloud dependency.
//
//  Sensors: DHT11, MPU6050, GPS (Serial2), MFRC522 RFID
//  Cloud:   ThingSpeak (8 fields + status string)
//
//  Edge AI Features:
//    Tier 1 — Statistical anomaly detection (temp/humidity)
//    Tier 2 — Accelerometer shock/impact classification
//    Tier 3 — Transport state machine + smart alerting
// ============================================================

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <ThingSpeak.h>
#include <DHT.h>
#include <MPU6050_light.h>
#include <TinyGPS++.h>
#include <MFRC522.h>

// --- Edge AI Modules ---
#include "edge_ai_config.h"
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
#define DHTPIN  32
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

MPU6050    mpu(Wire);
TinyGPSPlus gps;
MFRC522    rfid(5, 4);   // SS = GPIO 5, RST = GPIO 4

// ========================== EDGE AI ==============================
AnomalyDetector tempDetector;    // Temperature anomaly detector
AnomalyDetector humDetector;     // Humidity anomaly detector
ShockClassifier shockClassifier; // Accelerometer shock classifier
SmartAlerter    alerter;         // Transport state + smart alerting

// ========================== TIMING ===============================
unsigned long lastTransmitTime    = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastClassifyTime    = 0;

// ========================== SENSOR DATA ==========================
float  tempC    = 0.0f;
float  humidity = 0.0f;
double latitude = 0.0;
double longitude = 0.0;
unsigned long lastScannedRFID = 0;
bool   newRFIDScanned  = false;

// ========================== AI STATE =============================
ShockClass currentShockClass = SHOCK_NORMAL;
bool  tempAnomaly = false;
bool  humAnomaly  = false;
float peakG       = 0.0f;   // Peak G-force since last transmission

// =================================================================
//  SETUP
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial2.begin(9600, SERIAL_8N1, 16, 17);  // GPS on UART2

    // --- WiFi ---
    WiFi.begin(ssid, password);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWi-Fi Connected!");

    // --- Peripherals ---
    ThingSpeak.begin(client);
    Wire.begin();
    SPI.begin();
    rfid.PCD_Init();
    delay(4);
    rfid.PCD_DumpVersionToSerial(); // Add diagnostic output for RFID hardware

    // --- DHT11 ---
    dht.begin();
    Serial.println("DHT11 Initialized.");

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

    // ─────────────────────────────────────────────────────────────
    //  4. CONTINUOUS: Poll GPS buffer
    // ─────────────────────────────────────────────────────────────
    while (Serial2.available() > 0) {
        gps.encode(Serial2.read());
    }

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
//  Reads DHT11, runs anomaly detection, packs all AI outputs
//  into ThingSpeak fields, and pushes to the cloud.
// =================================================================
void transmitTelemetry(unsigned long now) {

    // --- Read DHT11 sensor ---
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    // DHT11 sometimes returns NaN — only update and analyze valid reads
    if (!isnan(t)) {
        tempC = t;
        tempAnomaly = tempDetector.update(tempC);
    }
    if (!isnan(h)) {
        humidity = h;
        humAnomaly = humDetector.update(humidity);
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
    // Log drift detection
    if (tempDetector.isDrifting(tempC)) {
        Serial.printf("[DRIFT]  Temperature drifting from EMA (%.1f vs EMA %.1f)\n",
            tempC, tempDetector.getEMA());
    }
    if (humDetector.isDrifting(humidity)) {
        Serial.printf("[DRIFT]  Humidity drifting from EMA (%.1f vs EMA %.1f)\n",
            humidity, humDetector.getEMA());
    }
    #endif

    // --- Update GPS ---
    if (gps.location.isValid()) {
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
    }

    // --- Compute composite anomaly score ---
    float tempZ = tempDetector.warmedUp ? fabsf(tempDetector.getZScore(tempC)) : 0.0f;
    float humZ  = humDetector.warmedUp  ? fabsf(humDetector.getZScore(humidity)) : 0.0f;
    float anomalyScore = (tempZ + humZ) / 2.0f;

    // ─────────────────────────────────────────────────────────────
    //  ThingSpeak Field Mapping:
    //
    //  Field 1: Temperature (°C)              — raw sensor
    //  Field 2: Humidity (%)                   — raw sensor
    //  Field 3: Shock Class (0-5 enum)         — AI classification
    //  Field 4: Latitude                       — GPS
    //  Field 5: Longitude                      — GPS
    //  Field 6: Packed Status (SSCCTA)         — AI state encoding
    //  Field 7: Peak G-force                   — max G since last TX
    //  Field 8: Anomaly Score                  — avg |Z-score|
    // ─────────────────────────────────────────────────────────────
    ThingSpeak.setField(1, tempC);
    ThingSpeak.setField(2, humidity);
    ThingSpeak.setField(3, (int)currentShockClass);
    ThingSpeak.setField(4, (float)latitude);
    ThingSpeak.setField(5, (float)longitude);
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
        Serial.printf("     Temp:   %.1f°C  Hum: %.1f%%\n", tempC, humidity);
        Serial.printf("     PeakG:  %.2f   AnomalyScore: %.2f\n", peakG, anomalyScore);
        Serial.printf("     GPS:    %.6f, %.6f\n", latitude, longitude);
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
    lastTransmitTime = now;
}
