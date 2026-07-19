#include <Arduino.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- Firebase ---
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- WI-FI & FIREBASE CONFIGURATION ---
// Gitignored — copy include/secrets.h.example to include/secrets.h and fill in real values.
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
#define API_KEY        FIREBASE_API_KEY
#define DATABASE_URL   FIREBASE_DATABASE_URL

FirebaseData   fbdo;       // Dedicated stream handle
FirebaseData   fbdoUpload; // Dedicated upload handle
FirebaseAuth   auth;
FirebaseConfig config;

bool firebaseReady = false;
unsigned long lastFbUpload = 0;
const unsigned long FB_UPLOAD_INTERVAL = 3000; // Upload data every 3s

unsigned long lastWifiRetry = 0;
const unsigned long WIFI_RETRY_INTERVAL = 30000; // Retry Wi-Fi (and Firebase init) every 30s while disconnected

// Pump override from cloud: -1 = AUTO, 0 = force OFF, 1 = force ON
volatile int pumpOverride = -1;

// --- PIN CONFIGURATION ---
#define ONE_WIRE_BUS 4
#define TDS_PIN 5
#define PH_PIN 1
#define RELAY_1 6
#define WATER_LOW_PIN 2
#define WATER_HIGH_PIN 3

// NOTE: Ensure your hardware matches this! 
// ADC_2_5db attenuation maxes out at ~1.5V. If your sensor outputs up to 3.3V, use ADC_11db and change this to 3.3
#define ANALOG_MAX_VOLTAGE 1.5 

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float currentTemp = 25.0;
float tdsValue = 0.0;
float phValue = 7.0;
bool isFilling = false;
String waterStatusStr = "NORMAL";

unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_INTERVAL = 1000; // Read sensors every 1s

unsigned long fillStartTime = 0;
const unsigned long MAX_FILL_DURATION = 180000; // 3 min safety cutoff if a float switch never reports "full"
bool fillTimeoutFault = false; // Latched until the tank is actually full or the operator force-stops the pump

void initFirebase() {
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    
    // Fix: Increase response timeout for unstable/extender Wi-Fi connections
    config.timeout.rtdbKeepAlive = 10000;

    if (Firebase.signUp(&config, &auth, "", "")) {
        Serial.println("Firebase anonymous sign-up OK");
        firebaseReady = true;
    } else {
        Serial.printf("Firebase sign-up failed: %s\n", config.signer.signupError.message.c_str());
    }

    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // Fix: Increase buffer breathing room for SSL/TLS handshakes
    fbdo.setBSSLBufferSize(3072, 1024);
    fbdoUpload.setBSSLBufferSize(3072, 1024);
    
    // Fix: Cap the maximum response buffer length to prevent payload read timeouts
    fbdoUpload.setResponseSize(512);

    if (!Firebase.RTDB.beginStream(&fbdo, "/control/pumpOverride")) {
        Serial.printf("Stream begin failed: %s\n", fbdo.errorReason().c_str());
    }
}

void pollFirebaseStream() {
    if (!firebaseReady) return;
    
    // Non-blocking background stream listener
    if (Firebase.RTDB.readStream(&fbdo)) {
        if (fbdo.streamAvailable()) {
            if (fbdo.dataType() == "int" || fbdo.dataType() == "number") {
                pumpOverride = fbdo.intData();
                Serial.printf("\n[CLOUD] Pump override changed -> %d\n", pumpOverride);
            }
        }
    }
}

void uploadToFirebase() {
    if (!firebaseReady) return;

    FirebaseJson json;
    json.set("temperature", currentTemp);
    json.set("tds", tdsValue);
    json.set("ph", phValue);
    json.set("pump", isFilling ? "RUNNING" : "IDLE");
    json.set("water_status", waterStatusStr);
    json.set("override", pumpOverride);
    json.set("ts", (int)(millis() / 1000));

    // Fix: Using updateNode minimizes transmitted packet size, significantly stopping payload timeouts
    if (!Firebase.RTDB.updateNode(&fbdoUpload, "/sensor", &json)) {
        Serial.printf("\n[ERROR] Upload failed: %s\n", fbdoUpload.errorReason().c_str());
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n========================================");
    Serial.println("--- Hydroponics System: Cloud Boot   ---");
    Serial.println("========================================");

    pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
    sensors.begin();
    pinMode(TDS_PIN, INPUT);
    pinMode(PH_PIN, INPUT);
    pinMode(WATER_LOW_PIN, INPUT_PULLUP);
    pinMode(WATER_HIGH_PIN, INPUT_PULLUP);
    
    // 2.5dB attenuation = Max ADC voltage measurable is ~1.5V
    analogSetAttenuation(ADC_2_5db); 

    pinMode(RELAY_1, OUTPUT);
    digitalWrite(RELAY_1, HIGH); // Default OFF

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWi-Fi Connected Successfully!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        initFirebase();
    } else {
        Serial.println("\nWi-Fi Failed. Running sensors offline (pump AUTO only).");
    }
}

void loop() {
    // 0. Recover from a failed/dropped Wi-Fi connection instead of staying offline forever.
    // setup() only tries once; without this, a bad connection at boot freezes Firebase
    // permanently since initFirebase() would never get called.
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL) {
            lastWifiRetry = millis();
            Serial.println("\n[WiFi] Not connected — retrying...");
            WiFi.reconnect();
        }
    } else if (!firebaseReady) {
        Serial.println("\n[WiFi] Connected — initializing Firebase...");
        initFirebase();
    }

    // 1. Keep stream connection open continuously
    pollFirebaseStream();

    // 2. Independent Sensor & Automation Loop (Runs every 1 second)
    if (millis() - lastSensorUpdate >= SENSOR_INTERVAL) {
        lastSensorUpdate = millis();

        // Sensors Reading
        sensors.requestTemperatures();
        currentTemp = sensors.getTempCByIndex(0);
        if (currentTemp == DEVICE_DISCONNECTED_C) currentTemp = 25.0;

        int rawTDS = analogRead(TDS_PIN);
        float voltageTDS = (float)rawTDS * ANALOG_MAX_VOLTAGE / 4095.0;
        float compensationCoefficient = 1.0 + 0.02 * (currentTemp - 25.0);
        float compensatedVoltageTDS = voltageTDS / compensationCoefficient;
        tdsValue = (133.42 * pow(compensatedVoltageTDS, 3) - 255.86 * pow(compensatedVoltageTDS, 2) + 857.39 * compensatedVoltageTDS);
        if (tdsValue < 0) tdsValue = 0;

        int rawPH = analogRead(PH_PIN);
        float voltagePH = (float)rawPH * ANALOG_MAX_VOLTAGE / 4095.0;
        phValue = 7.0 + ((1.50 - voltagePH) / 0.18);

        // Float switches
        bool lowSwitchOpen = (digitalRead(WATER_LOW_PIN) == HIGH);
        bool topSwitchOpen = (digitalRead(WATER_HIGH_PIN) == HIGH);
        bool topSwitchClosed = !topSwitchOpen;

        // A latched fill-timeout fault only clears once the tank is verifiably full or the
        // operator explicitly stops the pump — otherwise a stuck switch would just quietly
        // retry (and time out) forever instead of surfacing a persistent error.
        if (fillTimeoutFault && (topSwitchClosed || pumpOverride == 0)) {
            fillTimeoutFault = false;
        }

        // Water automation with cloud override + float-switch safety limits
        if (lowSwitchOpen && topSwitchClosed) {
            isFilling = false;
            digitalWrite(RELAY_1, HIGH);
            waterStatusStr = "SENSOR ERROR";
        }
        else if (fillTimeoutFault) {
            isFilling = false;
            digitalWrite(RELAY_1, HIGH);
            waterStatusStr = "FILL TIMEOUT ERROR";
        }
        else if (pumpOverride == 1) {
            if (topSwitchClosed) {
                isFilling = false;
                digitalWrite(RELAY_1, HIGH);
                waterStatusStr = "TANK FULL";
            } else {
                if (!isFilling) fillStartTime = millis();
                isFilling = true;
                if (millis() - fillStartTime >= MAX_FILL_DURATION) {
                    fillTimeoutFault = true;
                    isFilling = false;
                    digitalWrite(RELAY_1, HIGH);
                    waterStatusStr = "FILL TIMEOUT ERROR";
                } else {
                    digitalWrite(RELAY_1, LOW);
                    waterStatusStr = "FORCE FILLING";
                }
            }
        }
        else if (pumpOverride == 0) {
            isFilling = false;
            digitalWrite(RELAY_1, HIGH);
            waterStatusStr = topSwitchOpen ? "LOW (PUMP OFF)" : "PUMP OFF";
        }
        else { // AUTO MODE
            if (isFilling) {
                if (topSwitchClosed) {
                    isFilling = false;
                    digitalWrite(RELAY_1, HIGH);
                    waterStatusStr = "TANK FULL";
                } else if (millis() - fillStartTime >= MAX_FILL_DURATION) {
                    fillTimeoutFault = true;
                    isFilling = false;
                    digitalWrite(RELAY_1, HIGH);
                    waterStatusStr = "FILL TIMEOUT ERROR";
                } else {
                    digitalWrite(RELAY_1, LOW);
                    waterStatusStr = "REFILLING...";
                }
            } else {
                if (lowSwitchOpen) {
                    fillStartTime = millis();
                    isFilling = true;
                    digitalWrite(RELAY_1, LOW);
                    waterStatusStr = "REFILLING...";
                }
                else if (topSwitchClosed) { waterStatusStr = "TANK FULL"; }
                else if (topSwitchOpen)   { waterStatusStr = "LOW WARNING"; }
            }
        }

        // Output system health parameters to debug terminal
        Serial.print("Heap: "); Serial.print(ESP.getFreeHeap());
        Serial.print(" | RSSI: "); Serial.print(WiFi.RSSI()); // Monitor network strength
        Serial.print("dBm | Temp: "); Serial.print(currentTemp, 1);
        Serial.print("C | Status: "); Serial.print(waterStatusStr);
        Serial.print(" | Pump: "); Serial.print(isFilling ? "RUNNING" : "IDLE");
        Serial.print(" | Ovr: "); Serial.println(pumpOverride);
    }

    // 3. Staggered Firebase Upload Loop (Runs every 3 seconds)
    if (millis() - lastFbUpload >= FB_UPLOAD_INTERVAL) {
        lastFbUpload = millis();
        uploadToFirebase();
    }
}