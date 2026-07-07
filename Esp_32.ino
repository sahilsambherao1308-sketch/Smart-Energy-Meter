/********** BLYNK TEMPLATE **********/
#define BLYNK_TEMPLATE_ID   "Enter template Id here"
#define BLYNK_TEMPLATE_NAME "Enter template name "

/********** BLYNK **********/
#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>         // Used for both ThingSpeak AND Firebase REST

/********** WIFI **********/
char ssid[] = "Enter your wifi name here"; // Do not erase double inverted commas 
char pass[] = "Enter password ";           // If there is problem connecting try to change the name and password of your wifi/hotspot
                                           // keep name and passowrd simple without any blank spaces and avoid special characters.   


/********** WIFI LED **********/
#define WIFI_LED 2

/********** BLYNK AUTH **********/
char blynkAuth[] = "Enter here Authentication token id";

/********** THINGSPEAK **********/
const char* tsServer = "Enter API Endpoint here";
const char* tsApiKey = "Enter API Key here";

/********** FIREBASE REST (no library needed) **********/
const char* fbHost  = "Enter Firebase Realtime Database Host (Database URL) here";
const char* fbAuth  = "Enter Firebase Database Secret (Legacy Authentication Token)  here ";

/********** UART **********/
#define RXD2 16
#define TXD2 17

/* ── ENERGY DATA ──────────────────────────────────────────
   Arduino CSV format (9 fields):
   V, I, P, S, PF, VAR, kWh, Bill, OvrFlag              */
float voltage     = 0.0;
float current     = 0.0;
float realPower   = 0.0;
float appPower    = 0.0;
float powerFactor = 0.0;
float reactivePow = 0.0;
float energy_kWh  = 0.0;
float billAmount  = 0.0;
int   overloadFlag = 0;

/********** OBJECTS **********/
BlynkTimer timer;

/********** STATE **********/
bool lastOverloadState = false;

/* ══════════════════════════════════════
   READ FROM ARDUINO UNO (UART2)
   ══════════════════════════════════════ */
void readArduino() {
  static String line = "";

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      int parsed = sscanf(line.c_str(),
                          "%f,%f,%f,%f,%f,%f,%f,%f,%d",
                          &voltage, &current, &realPower,
                          &appPower, &powerFactor, &reactivePow,
                          &energy_kWh, &billAmount, &overloadFlag);

      if (parsed != 9) {
        Serial.print("Malformed line (fields=");
        Serial.print(parsed);
        Serial.print("): ");
        Serial.println(line);
      }
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
}

/* ══════════════════════════════════════
   SEND TO BLYNK
   V0=Voltage  V1=Current    V2=RealPower
   V3=AppPower V4=PowerFactor V5=ReactivePow
   V6=kWh      V7=Bill       V8=Overload
   Overload alert via logEvent("overload_alert")
   ══════════════════════════════════════ */
void sendToBlynk() {
  Blynk.virtualWrite(V0, voltage);
  Blynk.virtualWrite(V1, current);
  Blynk.virtualWrite(V2, realPower);
  Blynk.virtualWrite(V3, appPower);
  Blynk.virtualWrite(V4, powerFactor);
  Blynk.virtualWrite(V5, reactivePow);
  Blynk.virtualWrite(V6, energy_kWh);
  Blynk.virtualWrite(V7, billAmount);
  Blynk.virtualWrite(V8, overloadFlag);
}

/* ══════════════════════════════════════
   SEND TO THINGSPEAK (15s minimum)
   field1=V  field2=I   field3=W
   field4=VA field5=PF  field6=kWh
   field7=Rs field8=Ovr
   ══════════════════════════════════════ */
void sendToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(tsServer)
             + "?api_key=" + tsApiKey
             + "&field1="  + String(voltage,     1)
             + "&field2="  + String(current,      3)
             + "&field3="  + String(realPower,    1)
             + "&field4="  + String(appPower,     1)
             + "&field5="  + String(powerFactor,  3)
             + "&field6="  + String(energy_kWh,   5)
             + "&field7="  + String(billAmount,   2)
             + "&field8="  + String(overloadFlag);

  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.print("ThingSpeak error: ");
    Serial.println(code);
  }
  http.end();
}

/* ══════════════════════════════════════
   SEND TO FIREBASE (REST PATCH — no library)
   Replaces FirebaseESP32 entirely.
   Saves ~150-200KB of flash.
   ══════════════════════════════════════ */
void sendToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  // Build JSON with all 9 fields
  String json = "{\"voltage\":"     + String(voltage,     2)
              + ",\"current\":"     + String(current,      3)
              + ",\"realPower\":"   + String(realPower,    1)
              + ",\"appPower\":"    + String(appPower,     1)
              + ",\"powerFactor\":" + String(powerFactor,  3)
              + ",\"reactivePow\":" + String(reactivePow,  1)
              + ",\"energy_kWh\":"  + String(energy_kWh,   5)
              + ",\"billAmount\":"  + String(billAmount,   2)
              + ",\"overload\":"    + String(overloadFlag)
              + "}";

  String url = "https://" + String(fbHost)
             + "/energy.json?auth=" + String(fbAuth);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PATCH(json);   // PATCH updates only listed fields
  if (code != 200) {
    Serial.print("Firebase error: ");
    Serial.println(code);
  }
  http.end();
}

/********** WIFI LED **********/
void updateWiFiLED() {
  digitalWrite(WIFI_LED, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
}

/* ══════════════════════════════════════
   OVERLOAD PUSH NOTIFICATION (Blynk)
   Rising edge only — fires once per event.
   ══════════════════════════════════════ */
void checkOverloadNotification() {
  bool currentState = (overloadFlag == 1);

  if (currentState && !lastOverloadState) {
    Blynk.logEvent("overload_alert",
                   "⚠ Overload detected! Please reduce load immediately.");
    Serial.println("Blynk overload_alert event sent.");
  }

  lastOverloadState = currentState;
}

/********** SETUP **********/
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(WIFI_LED, OUTPUT);
  digitalWrite(WIFI_LED, LOW);

  // WiFi with 10s timeout
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, pass);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
    digitalWrite(WIFI_LED, HIGH);
  } else {
    Serial.println("\nWiFi FAILED — continuing offline.");
  }

  Blynk.begin(blynkAuth, ssid, pass);

  timer.setInterval(2000L,  sendToBlynk);
  timer.setInterval(15000L, sendToThingSpeak);
  timer.setInterval(5000L,  sendToFirebase);
}

/********** LOOP **********/
void loop() {
  readArduino();
  updateWiFiLED();
  checkOverloadNotification();

  Blynk.run();
  timer.run();

  // WiFi reconnect every 30s if dropped
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck >= 30000UL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost — reconnecting...");
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
    }
  }
}