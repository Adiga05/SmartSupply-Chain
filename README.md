# Smart Supply Chain Dashboard & Edge AI

This repository contains the software components for a next-generation Smart Supply Chain tracking system. It consists of a real-time React dashboard and an intelligent Edge AI architecture designed for ESP32 microcontrollers.

## 1. Web Dashboard
A Vite-based React application that serves as the **IoT Supply Chain Tracking Dashboard**. It pulls real-time telemetry data (temperature, humidity, shock, location, RFID) directly from the ThingSpeak API and visualizes it with beautiful, responsive charts.

### Getting Started (Dashboard)
```bash
npm install
npm run dev
```
The dashboard will run on `http://localhost:5173/`. Configure your ThingSpeak `CHANNEL_ID` and `READ_API_KEY` in `iot-supply-chain-dashboard.jsx`.

---

## 2. ESP32 Edge AI Architecture

Instead of the ESP32 acting as a "dumb" pipe that blindly sends raw sensor values, it analyzes data locally in real-time. It uses highly efficient Statistical Machine Learning, Feature Engineering, and Expert Systems (Rule-Based AI) instead of memory-heavy Neural Networks. 

### Tier 1: Statistical Anomaly Detection (Environmental)
* **File**: `anomaly_detector.h`
* **Technique**: Welford's Algorithm & Z-Score Calculation.
* **How it works**: The ESP32 constantly calculates the "running mean" and standard deviation of temperature and humidity (O(1) memory complexity). If a new reading has a Z-Score > 3.0, it instantly flags an anomaly.
* **Benefit**: Dynamically learns the environment. A package in winter has a different "normal" than one in summer. It only alerts on sudden, abnormal spikes (e.g., cold-chain break).

### Tier 2: Feature Extraction & Shock Classification
* **File**: `shock_classifier.h`
* **Technique**: Sliding Window Feature Extractor & Rule-Based Classifier.
* **How it works**: Samples the MPU6050 accelerometer at 50Hz into a 1-second window. It extracts statistical ML features:
  * **RMS (Root Mean Square)**: Overall energy.
  * **Peak-to-Peak**: Maximum amplitude swing.
  * **Kurtosis**: Measures how "spiky" or impulsive the signal is.
  * **Zero-Crossing Rate**: Estimates vibration frequency.
* **Classification Events**: `FREE_FALL`, `MAJOR_IMPACT`, `MINOR_BUMP`, `TILT_ALERT`, `VIBRATION`, or `NORMAL`.
* **Benefit**: ThingSpeak receives a clear semantic label explaining exactly what physical event happened, rather than just raw G-force data.

### Tier 3: Transport State Machine & Smart Alerting
* **File**: `transport_state.h`
* **Technique**: Finite State Machine (FSM) & Priority Alert Engine.
* **How it works**: Tracks the journey state (`STATIONARY`, `LOADING`, `IN_TRANSIT`) based on motion and RFID scans.
* **Benefit**: 
  * **CRITICAL**: If Tier 2 detects `FREE_FALL` or `MAJOR_IMPACT`, it transmits immediately to the cloud.
  * **NORMAL**: When `IN_TRANSIT`, transmits every 20 seconds.
  * **LOW**: When `STATIONARY`, transmission intervals are stretched to save massive amounts of battery and data.

### Why these techniques?
1. **Memory**: Uses only ~2.6 KB of RAM (compared to 20-50 KB for a Neural Network).
2. **No Training Data Needed**: Works right out of the box without needing to drop the hardware hundreds of times to train a model.
3. **Explainability**: Deterministic rules mean you know exactly *why* a "Tilt Alert" was flagged.
