/*
  Smart Bin - AI Waste Detection + Remote Door + Battery Dashboard
  Phan cung:
    - ESP32
    - Servo (nap thung): GPIO 4
    - Servo (cua lay rac): GPIO 16
    - Stepper driver: GPIO 25 (step), 26 (dir), 27 (ena)
    - Ultrasonic: Trig=GPIO 5, Echo=GPIO 18
    - LED do canh bao: GPIO 2
    - Battery 3S 18650: GPIO 34 qua voltage divider
*/

#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================================================
// CONFIG
// ============================================================================
const char* WIFI_SSID = "DL";
const char* WIFI_PASS = "26262626";

const char* SERVER_URL   = "https://192.168.1.235:5000/result";
const char* DOOR_URL     = "https://192.168.1.235:5000/door_status";
const char* BATTERY_URL  = "https://192.168.1.235:5000/battery";

// ============================================================================
// PINS
// ============================================================================
const int servoPin     = 4;
const int servoDoorPin = 16;

const int stepPin = 25;
const int dirPin  = 26;
const int enaPin  = 27;

const int trigPin = 5;
const int echoPin = 18;

const int redLedPin = 2;

const int batteryPin = 34;   // GPIO34 - ADC1 channel

// ============================================================================
// SERVOS
// ============================================================================
Servo myServo;
Servo servoDoor;

// ============================================================================
// BATTERY ADC
// ============================================================================
const float ADC_MAX        = 4095.0f;
const float ADC_REF        = 3.3f;
const float BATT_FULL      = 12.6f;   // 3S max
const float BATT_EMPTY     = 9.0f;    // 3S min
const float DIVIDER_RATIO  = 4.0f;    // 30k/10k divider

float readBatteryVoltage() {
  int raw = analogRead(batteryPin);
  float v_adc = (raw / ADC_MAX) * ADC_REF;
  return v_adc * DIVIDER_RATIO;
}

int calcBatteryPercent(float v_bat) {
  if (v_bat >= BATT_FULL) return 100;
  if (v_bat <= BATT_EMPTY) return 0;
  return (int)(((v_bat - BATT_EMPTY) / (BATT_FULL - BATT_EMPTY)) * 100.0f);
}

// ============================================================================
// ULTRASONIC
// ============================================================================
float readUltrasonicDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH);
  return duration * 0.034f / 2.0f;
}

// ============================================================================
// STEPPER
// ============================================================================
void stepperForward() {
  digitalWrite(dirPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(enaPin, LOW);
  for (int i = 0; i < 200; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);
  }
}

void stepperBackward() {
  digitalWrite(dirPin, LOW);
  delayMicroseconds(10);
  digitalWrite(enaPin, LOW);
  for (int i = 0; i < 200; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);
  }
}

void stepperStop() {
  digitalWrite(enaPin, HIGH);
  digitalWrite(stepPin, LOW);
}

// ============================================================================
// SERVO (nap thung rac)
// ============================================================================
void servoRotate90() {
  Serial.println("Servo quay 90 do");
  myServo.write(90);
  delay(1000);
  Serial.println("Servo quay ve 0 do");
  myServo.write(0);
  delay(1000);
}

// ============================================================================
// SERVO CUA (remote door)
// ============================================================================
void openDoorServo() {
  Serial.println("[DOOR] Mo cua thung rac - Servo Door 90 do");
  servoDoor.write(90);
  delay(500);
  Serial.println("[DOOR] Giu cua mo trong 5 giay...");
  delay(5000);
  Serial.println("[DOOR] Dong cua thung rac - Servo Door 0 do");
  servoDoor.write(0);
  delay(500);
  Serial.println("[DOOR] Cua da dong lai.");
}

// ============================================================================
// DOOR STATUS POLL
// ============================================================================
bool checkDoorFromServer() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(DOOR_URL);
  http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, response)) {
      bool flag = doc["open"] | false;
      if (flag) {
        Serial.println(">>> NHAN YEU CAU MO CUA THUNG RAC <<<");
        http.end();
        return true;
      }
    }
  }
  http.end();
  return false;
}

// ============================================================================
// AI DETECTION POLL
// ============================================================================
int wasteType = 0;
long long lastTimestamp = 0;

int getDetectionFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi mat ket noi!");
    return 0;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(5000);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String response = http.getString();
    Serial.print("Server response: ");
    Serial.println(response);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      int classId = doc["class_id"] | 0;
      long long ts = doc["timestamp"].as<long long>();

      Serial.print("  class_id=");
      Serial.print(classId);
      Serial.print("  ts=");
      Serial.print((long)ts);
      Serial.print("  lastTs=");
      Serial.println((long)lastTimestamp);

      if (classId > 0 && ts != lastTimestamp) {
        lastTimestamp = ts;
        String className = doc["class_name"] | "Unknown";
        float conf = doc["confidence"] | 0.0f;

        Serial.print(">>> PHAT HIEN RAC: ");
        Serial.print(className);
        Serial.print(" (ID=");
        Serial.print(classId);
        Serial.print(", conf=");
        Serial.print(conf * 100, 1);
        Serial.println("%)");

        http.end();
        return classId;
      } else if (ts == lastTimestamp) {
        Serial.println("Khong co phat hien moi (cung timestamp)");
      } else if (classId == 0 && ts != lastTimestamp) {
        lastTimestamp = ts;
        Serial.println("AI khong nhan dien duoc vat the (class_id=0)");
        http.end();
        return -1;
      } else {
        Serial.println("Khong phat hien rac (class_id=0)");
      }
    } else {
      Serial.print("Loi parse JSON: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("Loi HTTP: ");
    Serial.println(httpCode);
  }

  http.end();
  return 0;
}

// ============================================================================
// BATTERY - GUI LEN SERVER
// ============================================================================
void sendBatteryToServer() {
  if (WiFi.status() != WL_CONNECTED) return;

  float v_bat = readBatteryVoltage();
  int pct = calcBatteryPercent(v_bat);

  Serial.print("[BATTERY] Voltage: ");
  Serial.print(v_bat, 2);
  Serial.print("V, Percent: ");
  Serial.print(pct);
  Serial.println("%");

  // Tao payload JSON (tranh loi escape string)
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"voltage\":%.2f,\"percent\":%d}", v_bat, pct);

  HTTPClient http;
  http.begin(BATTERY_URL);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    Serial.println("[BATTERY] Da gui du lieu pin len server.");
  } else {
    Serial.print("[BATTERY] Loi gui du lieu: ");
    Serial.println(httpCode);
  }
  http.end();
}

// ============================================================================
// WASTE NAME
// ============================================================================
const char* getWasteName(int type) {
  switch (type) {
    case 1: return "Organic Waste";
    case 2: return "Recyclable Waste";
    case 3: return "Inorganic Waste";
    case 4: return "Hazardous Waste";
    default: return "Unknown";
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // Servo timers
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);

  servoDoor.setPeriodHertz(50);
  servoDoor.attach(servoDoorPin, 500, 2400);

  // Stepper
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enaPin, OUTPUT);
  digitalWrite(enaPin, HIGH); // Disable at start

  // Ultrasonic
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  // LED
  pinMode(redLedPin, OUTPUT);
  digitalWrite(redLedPin, LOW);

  Serial.begin(115200);
  Serial.println("=== SMART BIN - AI + DOOR + BATTERY ===");

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi ok! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi LOI!");
  }

  // Init servos
  myServo.write(0);
  servoDoor.write(0);
  delay(1000);
}

// ============================================================================
// NON-BLOCKING TIMERS
// ============================================================================
unsigned long lastDoorPoll = 0;
const unsigned long DOOR_INTERVAL = 2000;     // ms

unsigned long lastBatterySend = 0;
const unsigned long BATTERY_INTERVAL = 30000;   // gui moi 30 giay

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  unsigned long now = millis();

  // ---- CHECK DOOR (non-blocking) ----
  if (now - lastDoorPoll >= DOOR_INTERVAL) {
    lastDoorPoll = now;
    if (checkDoorFromServer()) {
      openDoorServo();
    }
  }

  // ---- SEND BATTERY (non-blocking) ----
  if (now - lastBatterySend >= BATTERY_INTERVAL) {
    lastBatterySend = now;
    sendBatteryToServer();
  }

  // ---- AI WASTE DETECTION ----
  Serial.println("===================");
  Serial.println("Cho phat hien rac tu AI...");

  wasteType = 0;
  while (wasteType == 0) {
    // Check door request during wait
    if (checkDoorFromServer()) {
      openDoorServo();
    }

    // Check battery send
    unsigned long t = millis();
    if (t - lastBatterySend >= BATTERY_INTERVAL) {
      lastBatterySend = t;
      sendBatteryToServer();
    }

    wasteType = getDetectionFromServer();
    if (wasteType == 0) {
      delay(2000);
    }
  }

  // ---- KHONG NHAN DIEN DUOC ----
  if (wasteType == -1) {
    Serial.println("AI KHONG NHAN DIEN DUOC - Sang LED do!");
    digitalWrite(redLedPin, HIGH);
    delay(3000);
    digitalWrite(redLedPin, LOW);
    Serial.println("Chuan bi cho chu ky tiep theo...");
    wasteType = 0;
    delay(2000);
    return;
  }

  // ---- PHAT HIEN RAC ----
  Serial.print("Phat hien: ");
  Serial.print(getWasteName(wasteType));
  Serial.print(" -> Ngan ");
  Serial.println(wasteType);

  int stopDistFwd = 0;
  switch (wasteType) {
    case 1: stopDistFwd = 10; break;
    case 2: stopDistFwd = 14; break;
    case 3: stopDistFwd = 18; break;
    case 4: stopDistFwd = 22; break;
    default: stopDistFwd = 10; break;
  }

  // PHASE 1: Stepper quay thuan
  Serial.println("PHASE 1: Stepper quay thuan");
  digitalWrite(dirPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(enaPin, LOW);

  while (true) {
    float d = readUltrasonicDistance();
    if (d >= stopDistFwd) break;
    stepperForward();
    delay(50);
  }
  stepperStop();

  // PHASE 2: Servo hoat dong
  Serial.println("PHASE 2: Servo hoat dong");
  servoRotate90();

  // PHASE 3: Cho 2 giay
  Serial.println("PHASE 3: Cho 2 giay");
  delay(2000);

  // PHASE 4: Stepper quay nghich ve
  Serial.println("PHASE 4: Stepper quay nghich");
  digitalWrite(dirPin, LOW);
  delayMicroseconds(10);
  digitalWrite(enaPin, LOW);

  while (true) {
    float d = readUltrasonicDistance();
    if (d <= 4.0f) break;
    stepperBackward();
    delay(50);
  }
  stepperStop();

  Serial.println("=== Hoan thanh 1 chu ky ===");
  wasteType = 0;
  delay(3000);
}
