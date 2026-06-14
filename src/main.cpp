#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

const char* ssid = "POLWEC_2.4G_EXT";
const char* password = "Polco@101";

#define ONE_WIRE_BUS 4

#ifndef LED_BUILTIN
#define LED_BUILTIN 48
#endif

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

WebServer server(80);

bool ledState = false;
float waterTemp = -999.0;
int sensorCount = 0;

void readTemperature() {
  sensors.requestTemperatures();
  waterTemp = sensors.getTempCByIndex(0);
}

void handleRoot() {
  String html = "<h1>Hydroponics Dashboard</h1>";
  html += "<p>ESP32-S3 is online</p>";

  html += "<p>Water Temp: ";
  if (waterTemp == DEVICE_DISCONNECTED_C || waterTemp < -100) {
    html += "Sensor not detected";
  } else {
    html += String(waterTemp, 2);
    html += " &deg;C";
  }
  html += "</p>";

  html += "<p>Sensors found: ";
  html += String(sensorCount);
  html += "</p>";

  html += "<p>LED is: ";
  html += ledState ? "ON" : "OFF";
  html += "</p>";

  html += "<a href='/on'><button>LED ON</button></a> ";
  html += "<a href='/off'><button>LED OFF</button></a> ";
  html += "<a href='/'><button>Refresh</button></a>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);

  sensors.begin();
  sensorCount = sensors.getDeviceCount();

  Serial.println("DS18B20 + WiFi Dashboard starting...");
  Serial.print("Sensors found: ");
  Serial.println(sensorCount);

  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Dashboard IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);

  server.on("/on", []() {
    ledState = true;
    digitalWrite(LED_BUILTIN, HIGH);
    handleRoot();
  });

  server.on("/off", []() {
    ledState = false;
    digitalWrite(LED_BUILTIN, LOW);
    handleRoot();
  });

  server.begin();
}

void loop() {
  readTemperature();

  Serial.print("Water Temp: ");
  if (waterTemp == DEVICE_DISCONNECTED_C || waterTemp < -100) {
    Serial.println("Sensor not detected");
  } else {
    Serial.print(waterTemp);
    Serial.println(" C");
  }

  server.handleClient();
  delay(1000);
}