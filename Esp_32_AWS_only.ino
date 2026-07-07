 // Note - Replace all the text with actual data wherever asked to enter before executing a program.
 

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

/* ================= WIFI ================= */
const char* ssid     = " Enter wifi name";
const char* password = "Enter wifi password";

/* ================= AWS IOT ================= */
const char* awsEndpoint  = "Enter AWS Endpoint Here";
const char* publishTopic = " Enter topic name here  Ex - smartmeter/energy/data ";  // MQTT Test client

/* ================= CERTIFICATES ================= */

// 1. Root CA
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
Enter Root CA Certificate here 
-----END CERTIFICATE-----
)EOF";

// 2. Device Certificate
static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
Enter Device certificate here 
-----END CERTIFICATE-----
)EOF";

// 3. Private Key
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
Enter Private key here 
-----END RSA PRIVATE KEY-----
)EOF";

/* ================= MQTT SETUP ================= */
WiFiClientSecure net;
PubSubClient client(net);

/* ================= WIFI ================= */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) {
      Serial.println("\nWiFi timeout, retrying...");
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(ssid, password);
      start = millis();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

/* ================= AWS IOT ================= */
void connectAWS() {
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);
  client.setServer(awsEndpoint, 8883);

  Serial.print("Connecting to AWS IoT...");
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  int retries = 0;
  while (!client.connected()) {
    if (client.connect("ESP32_SmartMeter")) {
      Serial.println("CONNECTED");
    } else {
      Serial.printf("FAILED rc=%d, retry %d/5\n", client.state(), ++retries);
      if (retries >= 5) {
        Serial.println("Too many retries, restarting WiFi...");
        WiFi.disconnect();
        delay(1000);
        connectWiFi();
        retries = 0;
      }
      delay(2000);
    }
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  Serial2.setTimeout(100);

  connectWiFi();
  connectAWS();
}

/* ================= LOOP ================= */
void loop() {
  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFi();
  }

  // Reconnect MQTT if dropped
  if (!client.connected()) {
    connectAWS();
  }

  client.loop();

  if (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();

    // FIX: All 9 variables — v, i, p, s, pf, var, e, b, ov
    // Previously 'var' (reactive power) was missing, causing fields 7-9
    // to shift left → energy read into bill, bill read into overload,
    // overload always 0 (float ~2.5 cast to int = 0)
    float v, i, p, s, pf, var, e, b;
    int ov;

    // FIX: sscanf now reads all 9 fields correctly
    if (sscanf(line.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%d",
               &v, &i, &p, &s, &pf, &var, &e, &b, &ov) == 9) {

      char payload[350];
      snprintf(payload, sizeof(payload),
        "{"
          "\"voltage\":%.1f,"
          "\"current\":%.3f,"
          "\"real_power\":%.1f,"
          "\"apparent_power\":%.1f,"
          "\"power_factor\":%.3f,"
          "\"reactive_power\":%.1f,"   // FIX: now included
          "\"energy_kwh\":%.5f,"
          "\"bill_rs\":%.2f,"
          "\"overload\":%d"            // FIX: now correctly reads 0 or 1
        "}",
        v, i, p, s, pf, var, e, b, ov);

      if (client.publish(publishTopic, payload)) {
        Serial.println("PUBLISHED TO AWS:");
        Serial.println(payload);
      } else {
        Serial.println("PUBLISH FAILED");
      }

    } else {
      Serial.println("PARSE ERROR: " + line);
    }
  }
}