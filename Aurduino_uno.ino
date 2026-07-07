/*

 *  Screen 0 : Live Meter     (V, I, P + sine waves)
 *  Screen 1 : Power Analysis (V, I, P, S, VAR, PF, Angle)
 *  Screen 2 : Energy & Bill  (kWh, Rs, Rate)
 *  Screen 3 : Meter Status   (OK/FAULT, OVR, V level)
 *  Screen 4 : Overload History (EEPROM logged)
 *
 *  NAV button D7    → next screen (0→1→2→3→4→0)
 *                     hold 3s = reset energy
 *                     also used to confirm phase calibration at startup
 *  RESET button D8  → hold 6s = clear all + history
 *
 *  STARTUP CALIBRATION (runs once at power-on):
 *  Step 1 — Supply ON, NO load → noise floor measured automatically
 *  Step 2 — Connect resistive load (bulb), press D7 → phase offset measured
 *  Then normal meter operation begins.
 *
 *  EEPROM MAP:
 *  [0-3]   float   energy_kWh
 *  [4]     byte    ovrCount
 *  [5]     byte    ovrIndex
 *  [6-35]  3×10    OverloadEvent {hour, min, sec}
 *
 *  Serial CSV → ESP32 (9 fields, every 2s):
 *  V, I, P, S, PF, VAR, kWh, Bill, OvrFlag
 * =====================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <EEPROM.h>
#include <math.h>

#define DEBUG 0

#define OLED_ADDR  0x3C
Adafruit_SH1106G display(128, 64, &Wire);

/* ── PINS ── */
#define VOLT_PIN     A1
#define CURR_PIN     A0
#define BTN_NAV      7
#define BTN_RESET    8
#define LED_POWER    2
#define LED_OK       3
#define LED_OVERLOAD 4
#define LED_FAULT    5
#define BUZZER       6

/* ═════════════════════════════════════════════════════════
   MEASUREMENT CONSTANTS  
   ═════════════════════════════════════════════════════════*/
#define ADC_VREF     5.0f
#define ADC_MAX      1023.0f
#define VOLT_CAL     230.0f    // adjust with multimeter if needed
#define ACS712_SENS  0.264f    // V/A — calibrated for this specific module
#define VOLT_FLOOR   10.0f     // V  — no supply below this
#define CURR_MIN     0.020f    // A  — min detectable after noise subtraction
#define N            1000      // samples per pass (≈10 complete 50Hz cycles)

/* ── CALIBRATION STATE (measured at startup) ── */
float noiseIrms   = 0.0f;    // EMI phantom current, no load
float phaseOffset = 0.0f;    // ZMPT101B op-amp phase shift in degrees

/* ── MEASUREMENT RESULTS ── */
float Vrms, Irms;
float realPower, apparentPower, reactivePower, powerFactor, phaseAngleDeg;

/* ── ENERGY & BILL ── */
float energy_kWh       = 0.0f;
float lastSavedEnergy  = 0.0f;
float bill             = 0.0f;
const float tariff     = 8.0f;
unsigned long lastEnergyMillis = 0;

/* ── OVERLOAD ── */
#define POWER_LIMIT         2000.0f
#define VOLT_LOW_THRESHOLD  180.0f
#define VOLT_HIGH_THRESHOLD 260.0f
bool overloadFlag     = false;
bool lastOverloadFlag = false;

/* ── TIME ── */
unsigned long startMillis = 0;

/* ── SCREEN ── */
int  screenIndex  = 0;
#define TOTAL_SCREENS 5
bool navLastState = HIGH;
unsigned long lastBtnTime = 0;

/* ── RESET BUTTON ── */
bool resetHeld        = false;
unsigned long resetPressTime = 0;
bool resetActionDone  = false;

/* ── BLINK ── */
bool blinkState    = false;
unsigned long lastBlink = 0;

/* ── SINE ANIMATION ── */
float sinePhase = 0.0f;

/* ── EEPROM LAYOUT ── */
#define EE_ENERGY    0
#define EE_OVR_COUNT 4
#define EE_OVR_INDEX 5
#define EE_OVR_BASE  6

struct OverloadEvent { uint8_t hour, minute, second; };
OverloadEvent ovrHistory[10];
uint8_t ovrCount = 0;
uint8_t ovrIndex = 0;

/* ══════════════════════════════
   EEPROM FUNCTIONS 
   ═════════════════════════════ */
void saveEnergy() { EEPROM.put(EE_ENERGY, energy_kWh); }

void loadEnergy() {
  EEPROM.get(EE_ENERGY, energy_kWh);
  if (isnan(energy_kWh) || energy_kWh < 0.0f || energy_kWh > 99999.0f)
    energy_kWh = 0.0f;
  lastSavedEnergy = floorf(energy_kWh);
}

void saveOvrHistory() {
  EEPROM.put(EE_OVR_COUNT, ovrCount);
  EEPROM.put(EE_OVR_INDEX, ovrIndex);
  for (int i = 0; i < 10; i++) EEPROM.put(EE_OVR_BASE + i * 3, ovrHistory[i]);
}

void loadOvrHistory() {
  EEPROM.get(EE_OVR_COUNT, ovrCount);
  EEPROM.get(EE_OVR_INDEX, ovrIndex);
  if (ovrCount > 10) ovrCount = 0;
  if (ovrIndex > 9)  ovrIndex = 0;
  for (int i = 0; i < 10; i++) EEPROM.get(EE_OVR_BASE + i * 3, ovrHistory[i]);
}

void addOvrEvent(uint8_t h, uint8_t m, uint8_t s) {
  ovrHistory[ovrIndex].hour   = h;
  ovrHistory[ovrIndex].minute = m;
  ovrHistory[ovrIndex].second = s;
  ovrIndex = (ovrIndex + 1) % 10;
  if (ovrCount < 10) ovrCount++;
  saveOvrHistory();
}

void clearAllData() {
  energy_kWh = lastSavedEnergy = bill = 0.0f;
  ovrCount = ovrIndex = 0;
  memset(ovrHistory, 0, sizeof(ovrHistory));
  saveEnergy();
  saveOvrHistory();
}

/* ═══════════════════════════════════
   DISPLAY HELPERS \
   ═══════════════════════════════════ */
void drawSineWave(int x0, int y0, int w, int h, float phase) {
  int halfH = h / 2, yMid = y0 + halfH;
  float period = (float)w / 1.5f;
  for (int x = 0; x < w - 1; x++) {
    int y1 = constrain(yMid-(int)((halfH-1)*sinf(-phase+(float)x      *TWO_PI/period)),y0,y0+h-1);
    int y2 = constrain(yMid-(int)((halfH-1)*sinf(-phase+(float)(x+1)  *TWO_PI/period)),y0,y0+h-1);
    display.drawLine(x0+x, y1, x0+x+1, y2, SH110X_WHITE);
  }
}

void printTime(int x, int y, int hh, int mm, int ss) {
  display.setCursor(x, y); display.print(F("T:"));
  if (hh<10) display.print('0'); display.print(hh); display.print(':');
  if (mm<10) display.print('0'); display.print(mm); display.print(':');
  if (ss<10) display.print('0'); display.print(ss);
}

void showConfirmation(const __FlashStringHelper* msg) {
  display.clearDisplay();
  display.drawRect(5, 15, 118, 34, SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(12, 25); display.print(msg);
  display.setCursor(20, 36); display.print(F("Done!"));
  display.display();
  delay(1500);
}

/* ═══════════════════════════════════════════════════════════════════════════
   MEASUREMENT ENGINE V6
   ═══════════════════════════════════════════════════════════════════════════ */

/* measureRaw() — two-pass core measurement, no arrays, RAM-safe.
   Returns raw (uncalibrated for noise/phase) Vrms, Irms, real power.      */
void measureRaw(float &rawVrms, float &rawIrms, float &rawP) {
  /* Pass 1: DC offsets (= VCC/2 ≈ 512 counts per datasheet) */
  long sumV = 0, sumI = 0;
  for (int i = 0; i < N; i++) {
    sumV += analogRead(VOLT_PIN);
    sumI += analogRead(CURR_PIN);
  }
  float offsetV = (float)sumV / N;
  float offsetI = (float)sumI / N;

  /* Pass 2: subtract offset, accumulate RMS + power sums */
  double sumV2 = 0.0, sumI2 = 0.0, sumP = 0.0;
  for (int i = 0; i < N; i++) {
    float vc = (float)analogRead(VOLT_PIN) - offsetV;
    float ic = (float)analogRead(CURR_PIN) - offsetI;
    sumV2 += (double)vc * vc;
    sumI2 += (double)ic * ic;
    sumP  += (double)vc * ic;
  }

  float k = ADC_VREF / ADC_MAX;
  rawVrms = sqrtf((float)(sumV2 / N)) * k * VOLT_CAL;
  rawIrms = sqrtf((float)(sumI2 / N)) * k / ACS712_SENS;
  rawP    = fabsf((float)(sumP  / N)  * k * k * VOLT_CAL / ACS712_SENS);
}

/* calibrateNoise() — Step 1 of startup calibration.
   Supply must be ON. No load connected.
   Measures EMI phantom current induced by mains wiring.                    */
void calibrateNoise() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(8,  0); display.print(F("STEP 1: NOISE CAL"));
  display.drawLine(0, 10, 127, 10, SH110X_WHITE);
  display.setCursor(0, 14); display.print(F("Supply ON, no load."));
  display.setCursor(0, 24); display.print(F("Waiting for supply"));
  display.display();

  /* Wait until mains voltage is detected */
  float vtest = 0;
  while (vtest < 50.0f) {
    float d1, d2;
    measureRaw(vtest, d1, d2);
    display.print(F("."));
    display.display();
  }

  display.clearDisplay();
  display.setCursor(8,  0); display.print(F("STEP 1: NOISE CAL"));
  display.drawLine(0, 10, 127, 10, SH110X_WHITE);
  display.setCursor(0, 14); display.print(F("Supply detected!"));
  display.setCursor(0, 24); display.print(F("Measuring noise..."));
  display.display();

  /* Average 10 readings for stable noise floor */
  float sum = 0.0f;
  for (int i = 0; i < 10; i++) {
    float v, ri, p; measureRaw(v, ri, p);
    sum += ri;
  }
  noiseIrms = sum / 10.0f;

  display.setCursor(0, 36);
  display.print(F("Noise="));
  display.print(noiseIrms * 1000.0f, 1);
  display.print(F("mA Done!"));
  display.display();
  delay(1500);
}

/* calibratePhase() — Step 2 of startup calibration.
   Connect tungsten bulb (resistive load), press D7.
   Measures ZMPT101B op-amp phase shift.                                    */
void calibratePhase() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(8,  0); display.print(F("STEP 2: PHASE CAL"));
  display.drawLine(0, 10, 127, 10, SH110X_WHITE);
  display.setCursor(0, 14); display.print(F("Connect resistive"));
  display.setCursor(0, 24); display.print(F("load (bulb) now."));
  display.setCursor(0, 38); display.print(F("Press D7 when ready"));
  display.display();

  /* Wait for D7 button press */
  while (digitalRead(BTN_NAV) == HIGH) delay(50);
  delay(300); // debounce

  display.clearDisplay();
  display.setCursor(8,  0); display.print(F("STEP 2: PHASE CAL"));
  display.drawLine(0, 10, 127, 10, SH110X_WHITE);
  display.setCursor(0, 14); display.print(F("Measuring phase..."));
  display.display();

  /* Average 10 phase readings with bulb */
  float sumPhase = 0.0f;
  int   validCount = 0;
  for (int i = 0; i < 10; i++) {
    float rv, ri, rp;
    measureRaw(rv, ri, rp);
    float iReal2   = ri * ri - noiseIrms * noiseIrms;
    float corrIrms = (iReal2 > 0.0f) ? sqrtf(iReal2) : 0.0f;
    float S        = rv * corrIrms;
    if (S > 1.0f) {
      float pf    = constrain(rp / S, 0.0f, 1.0f);
      sumPhase   += degrees(acosf(pf));
      validCount++;
    }
  }
  phaseOffset = (validCount > 0) ? sumPhase / validCount : 0.0f;

  display.setCursor(0, 28);
  display.print(F("Phase offset="));
  display.print(phaseOffset, 1);
  display.print(F(" deg"));

  display.setCursor(0, 42);
  if      (phaseOffset < 5.0f)  display.print(F("Very low - great!"));
  else if (phaseOffset < 20.0f) display.print(F("Normal for ZMPT101B"));
  else                          display.print(F("High - still OK"));

  display.setCursor(0, 54);
  display.print(F("Starting meter..."));
  display.display();
  delay(2000);
}

/* measureAll() — called every loop. Applies noise + phase corrections.     */
void measureAll() {
  float rawVrms, rawIrms, rawP;
  measureRaw(rawVrms, rawIrms, rawP);

  /* Noise subtraction: I_real = sqrt(I_measured² - I_noise²)
     EMI phantom is capacitive (90° phase) → adds in quadrature to real I.  */
  float iReal2   = rawIrms * rawIrms - noiseIrms * noiseIrms;
  float corrIrms = (iReal2 > 0.0f) ? sqrtf(iReal2) : 0.0f;

  /* Floors */
  if (rawVrms < VOLT_FLOOR) { rawVrms = 0.0f; corrIrms = 0.0f; rawP = 0.0f; }
  if (corrIrms < CURR_MIN)  { corrIrms = 0.0f; rawP = 0.0f; }

  Vrms          = rawVrms;
  apparentPower = Vrms * corrIrms;

  /* Phase correction: subtract ZMPT101B op-amp phase shift.
     correctedPhase = measuredPhase - phaseOffset
     PF = cos(correctedPhase)                                                */
  float rawPF          = (apparentPower > 1.0f)
                         ? constrain(rawP / apparentPower, 0.0f, 1.0f)
                         : 0.0f;
  float rawPhase       = (rawPF > 0.0f) ? degrees(acosf(rawPF)) : 90.0f;
  float correctedPhase = constrain(rawPhase - phaseOffset, 0.0f, 90.0f);

  phaseAngleDeg = correctedPhase;
  powerFactor   = cosf(radians(phaseAngleDeg));

  /* Recalculate P and Q from corrected PF for consistent P+Q+S triangle */
  realPower     = apparentPower * powerFactor;
  reactivePower = apparentPower * sinf(radians(phaseAngleDeg));
  Irms          = corrIrms;
}

/* ═══════════════════════════════════
   SCREEN 0 : LIVE METER 
   ═══════════════════════════════════ */
void drawLiveMeter(int hh, int mm, int ss) {
  display.setTextSize(1);
  display.setCursor(10, 0); display.print(F("Smart Energy Meter"));
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  display.setCursor(0, 13);
  display.print(F("V:")); display.print(Vrms, 1); display.print(F("V"));
  drawSineWave(70, 10, 57, 11, sinePhase);
  display.drawLine(0, 22, 127, 22, SH110X_WHITE);

  display.setCursor(0, 26);
  display.print(F("I:")); display.print(Irms, 3); display.print(F("A"));
  drawSineWave(70, 23, 57, 11, sinePhase + PI);
  display.drawLine(0, 35, 127, 35, SH110X_WHITE);

  display.setCursor(0, 39);
  display.print(F("P: ")); display.print(realPower, 1); display.print(F(" W"));
  display.drawLine(0, 53, 127, 53, SH110X_WHITE);
  printTime(28, 56, hh, mm, ss);
}

/* ═══════════════════════════════════
   SCREEN 1 : POWER ANALYSIS
   ═══════════════════════════════════ */
void drawPowerAnalysis(int hh, int mm, int ss) {
  display.setTextSize(1);
  display.setCursor(18, 0); display.print(F("Power Analysis"));
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  /* Row 1: V and I */
  display.setCursor(0,  12); display.print(F("V:")); display.print(Vrms, 1);         display.print(F("V"));
  display.setCursor(68, 12); display.print(F("I:")); display.print(Irms, 3);         display.print(F("A"));

  /* Row 2: P and S */
  display.setCursor(0,  22); display.print(F("P:")); display.print(realPower, 1);    display.print(F("W"));
  display.setCursor(68, 22); display.print(F("S:")); display.print(apparentPower, 1);display.print(F("VA"));

  /* Row 3: VAR and PF */
  display.setCursor(0,  32); display.print(F("Q:"));  display.print(reactivePower, 1);
  display.setCursor(68, 32); display.print(F("PF:")); display.print(powerFactor, 2);

  /* Row 4: Phase angle — new, replaces old time row */
  display.setCursor(0,  42); display.print(F("Ang:")); display.print(phaseAngleDeg, 1); display.print(F("deg"));

  display.drawLine(0, 52, 127, 52, SH110X_WHITE);
  printTime(28, 55, hh, mm, ss);
}

/* ═══════════════════════════════════
   SCREEN 2 : ENERGY & BILL
   ═══════════════════════════════════ */
void drawEnergyBill(int hh, int mm, int ss) {
  display.setTextSize(1);
  display.setCursor(22, 0); display.print(F("Energy & Bill"));
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  display.setCursor(0, 13); display.print(F("Energy:")); display.print(energy_kWh, 4); display.print(F(" kWh"));
  display.setCursor(0, 25); display.print(F("Bill  :Rs.")); display.print(bill, 2);
  display.setCursor(0, 37); display.print(F("Rate  :Rs.8/unit"));
  display.drawLine(0, 48, 127, 48, SH110X_WHITE);
  printTime(28, 56, hh, mm, ss);
}

/* ═══════════════════════════════════
   SCREEN 3 : METER STATUS 
   ═══════════════════════════════════ */
void drawMeterStatus(int hh, int mm, int ss) {
  display.setTextSize(1);
  display.setCursor(22, 0); display.print(F("Meter Status"));
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  bool fault = (Vrms < 10.0f);
  display.setCursor(0, 12); display.print(F("Status  :")); display.print(fault ? F(" FAULT") : F("    OK"));
  display.setCursor(0, 23); display.print(F("Overload:")); display.print(overloadFlag ? F("   YES") : F("    NO"));
  display.setCursor(0, 34); display.print(F("Voltage :"));
  if      (fault)                        display.print(F("  NONE"));
  else if (Vrms < VOLT_LOW_THRESHOLD)    display.print(F("   LOW"));
  else if (Vrms > VOLT_HIGH_THRESHOLD)   display.print(F("  HIGH"));
  else                                   display.print(F("    OK"));
  display.setCursor(0, 45); display.print(F("Power   :")); display.print(realPower, 1); display.print(F("W"));
  display.drawLine(0, 54, 127, 54, SH110X_WHITE);
  printTime(28, 57, hh, mm, ss);
}

/* ═══════════════════════════════════
   SCREEN 4 : OVERLOAD HISTORY 
   ═══════════════════════════════════ */
void drawOvrHistory() {
  display.setTextSize(1);
  display.setCursor(20, 0); display.print(F("Meter History"));
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  if (ovrCount == 0) {
    display.setCursor(10, 28); display.print(F("No Events Logged"));
    display.setCursor(5,  42); display.print(F("System running OK"));
    return;
  }
  uint8_t shown = (ovrCount < 5) ? ovrCount : 5;
  for (int i = 0; i < shown; i++) {
    int slot = ((int)ovrIndex - 1 - i + 10) % 10;
    display.setCursor(0, 12 + i * 10);
    display.print(F("OVR")); display.print(i + 1); display.print(F(":"));
    if (ovrHistory[slot].hour   < 10) display.print('0'); display.print(ovrHistory[slot].hour);   display.print(':');
    if (ovrHistory[slot].minute < 10) display.print('0'); display.print(ovrHistory[slot].minute); display.print(':');
    if (ovrHistory[slot].second < 10) display.print('0'); display.print(ovrHistory[slot].second);
  }
  if (ovrCount > 5) {
    display.setCursor(72, 12); display.print(F("Total:")); display.print(ovrCount);
  }
}

/* ═══════════════════════════════════
   OVERLOAD ALERT 
   ═══════════════════════════════════ */
void drawOverloadAlert() {
  if (blinkState) {
    display.drawRect(0, 0, 128, 64, SH110X_WHITE);
    display.drawRect(2, 2, 124, 60, SH110X_WHITE);
    display.setTextSize(2); display.setCursor(8, 8);  display.print(F("OVERLOAD"));
    display.setTextSize(1); display.setCursor(12, 32);display.print(F("! Reduce Load !"));
    display.setCursor(20, 46);
    display.print(F("P:")); display.print(realPower, 1);
    display.print(F("W>")); display.print((int)POWER_LIMIT); display.print(F("W"));
  }
}

/* ═══════════════════════════════════
   LED & BUZZER 
   ═══════════════════════════════════ */
void updateIndicators() {
  digitalWrite(LED_POWER, HIGH);
  bool valid = (Vrms > 10.0f);
  if (!valid) {
    digitalWrite(LED_OK, LOW); digitalWrite(LED_OVERLOAD, LOW);
    digitalWrite(LED_FAULT, blinkState ? HIGH : LOW); digitalWrite(BUZZER, LOW);
    return;
  }
  digitalWrite(LED_FAULT, LOW);
  if (overloadFlag) {
    digitalWrite(LED_OK, LOW); digitalWrite(LED_OVERLOAD, HIGH);
    digitalWrite(BUZZER, blinkState ? HIGH : LOW);
  } else {
    digitalWrite(LED_OK, HIGH); digitalWrite(LED_OVERLOAD, LOW); digitalWrite(BUZZER, LOW);
  }
}

/* ═══════════════════════════════════
   SETUP
   ═══════════════════════════════════ */
void setup() {
  Serial.begin(9600);

  pinMode(BTN_NAV,      INPUT_PULLUP);
  pinMode(BTN_RESET,    INPUT_PULLUP);
  pinMode(LED_POWER,    OUTPUT);
  pinMode(LED_OK,       OUTPUT);
  pinMode(LED_OVERLOAD, OUTPUT);
  pinMode(LED_FAULT,    OUTPUT);
  pinMode(BUZZER,       OUTPUT);

  digitalWrite(LED_POWER,    LOW); digitalWrite(LED_OK,       LOW);
  digitalWrite(LED_OVERLOAD, LOW); digitalWrite(LED_FAULT,    LOW);
  digitalWrite(BUZZER,       LOW);

  display.begin(OLED_ADDR, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  /* Splash screen */
  display.setTextSize(2);
  display.setCursor(4, 2);  display.print(F("SMART"));
  display.setCursor(4, 20); display.print(F("ENERGY"));
  display.setCursor(4, 38); display.print(F("METER"));
  display.setTextSize(1);
  display.setCursor(0, 56); display.print(F("Team Energyflow-DMCE EXTC-B"));
  display.display();
  delay(3000);

  /* ── 2-STEP CALIBRATION ────────────────────────────────────────────────
     Step 1: noise (auto — supply ON, no load)
     Step 2: phase (press D7 with bulb connected)                          */
  calibrateNoise();
  calibratePhase();

  /* Load saved data */
  loadEnergy();
  loadOvrHistory();

  startMillis      = millis();
  lastBtnTime      = millis();
  lastEnergyMillis = millis();
  lastBlink        = millis();

#if DEBUG
  Serial.print(F("noiseIrms="));   Serial.print(noiseIrms * 1000, 1); Serial.println(F("mA"));
  Serial.print(F("phaseOffset=")); Serial.print(phaseOffset, 1);      Serial.println(F("deg"));
  Serial.print(F("energy="));      Serial.print(energy_kWh, 4);       Serial.println(F(" kWh"));
  Serial.print(F("ovrCount="));    Serial.println(ovrCount);
#endif
}

/* ═══════════════════════════════════
   LOOP
   ═══════════════════════════════════ */
void loop() {

  unsigned long now = millis();

  /* ── BLINK TIMER ── */
  if (now - lastBlink >= 500) { lastBlink = now; blinkState = !blinkState; }

  /* ── D7 NAV BUTTON ──────────────────────────────────
     Single tap (< 3s) → next screen
     Hold 3s           → reset energy                  */
  bool navNow = digitalRead(BTN_NAV);

  if (navNow == LOW && navLastState == HIGH) {
    lastBtnTime = now; resetActionDone = false;
  }
  if (navNow == LOW && !resetActionDone && (now - lastBtnTime) >= 3000) {
    energy_kWh = lastSavedEnergy = bill = 0.0f;
    saveEnergy();
    resetActionDone = true;
    showConfirmation(F("Energy Reset"));
  }
  if (navNow == HIGH && navLastState == LOW) {
    if (!resetActionDone && (now - lastBtnTime) < 3000)
      screenIndex = (screenIndex + 1) % TOTAL_SCREENS;
    resetActionDone = false;
  }
  navLastState = navNow;

  /* ── D8 RESET BUTTON ─────────────────────────────────
     Hold 6s → clear all data + history                 */
  bool resetNow = digitalRead(BTN_RESET);
  if (resetNow == LOW) {
    if (!resetHeld) { resetHeld = true; resetPressTime = now; resetActionDone = false; }
    else if (!resetActionDone && (now - resetPressTime) >= 6000) {
      clearAllData(); resetActionDone = true; showConfirmation(F("History Cleared"));
    }
  } else { resetHeld = false; resetActionDone = false; }

  /* ── MEASURE (v6 engine) ── */
  measureAll();

  /* ── OVERLOAD FLAG ── */
  overloadFlag = (realPower > POWER_LIMIT);

  /* ── LOG OVERLOAD EVENT (rising edge + 5s cooldown) ── */
  static unsigned long lastOvrSave = 0;
  if (overloadFlag && !lastOverloadFlag && (now - lastOvrSave) >= 5000UL) {
    unsigned long t = (now - startMillis) / 1000UL;
    addOvrEvent((uint8_t)(t/3600), (uint8_t)((t%3600)/60), (uint8_t)(t%60));
    lastOvrSave = now;
  }
  lastOverloadFlag = overloadFlag;

  /* ── ENERGY INTEGRATION ── */
  float elapsedHours  = (float)(now - lastEnergyMillis) / 3600000.0f;
  energy_kWh         += (realPower * elapsedHours) / 1000.0f;
  lastEnergyMillis    = now;
  bill                = energy_kWh * tariff;

  if ((energy_kWh - lastSavedEnergy) >= 1.0f) {
    lastSavedEnergy = floorf(energy_kWh);
    saveEnergy();
  }

  /* ── TIME ── */
  unsigned long totalSec = (now - startMillis) / 1000UL;
  int hh = totalSec / 3600, mm = (totalSec % 3600) / 60, ss = totalSec % 60;

  /* ── SINE ANIMATION ── */
  sinePhase += 0.3f;
  if (sinePhase > TWO_PI) sinePhase -= TWO_PI;

  /* ── INDICATORS ── */
  updateIndicators();

  /* ── DRAW (every 500ms) ── */
  static unsigned long lastDraw = 0;
  if (now - lastDraw >= 500) {
    lastDraw = now;
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    if (overloadFlag) {
      drawOverloadAlert();
    } else {
      switch (screenIndex) {
        case 0: drawLiveMeter(hh, mm, ss);     break;
        case 1: drawPowerAnalysis(hh, mm, ss); break;
        case 2: drawEnergyBill(hh, mm, ss);    break;
        case 3: drawMeterStatus(hh, mm, ss);   break;
        case 4: drawOvrHistory();              break;
      }
    }
    display.display();
  }

  /* ── SERIAL → ESP32 (every 2s // Can be changed as per requiremnt) ─────────────────────────────────────────
     9-field CSV: V, I, P, S, PF, VAR, kWh, Bill, OvrFlag
     ESP32 sscanf: "%f,%f,%f,%f,%f,%f,%f,%f,%d"                            */
  static unsigned long lastSend = 0;
  if (now - lastSend >= 2000) {
    lastSend = now;
    Serial.print(Vrms, 1);           Serial.print(',');
    Serial.print(Irms, 3);           Serial.print(',');
    Serial.print(realPower, 1);      Serial.print(',');
    Serial.print(apparentPower, 1);  Serial.print(',');
    Serial.print(powerFactor, 2);    Serial.print(',');
    Serial.print(reactivePower, 1);  Serial.print(',');
    Serial.print(energy_kWh, 5);     Serial.print(',');
    Serial.print(bill, 2);           Serial.print(',');
    Serial.println((int)overloadFlag);
  }
}
