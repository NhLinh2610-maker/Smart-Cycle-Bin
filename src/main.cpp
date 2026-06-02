/*
  Smart Bin - AI Waste Detection + Battery Dashboard + LED xanh
  VERSION FINAL – thêm LED xanh GPIO 12 điều khiển từ Dashboard

  Thay đổi so với v6:
    - Thêm LED_GREEN_PIN = GPIO 12
    - Thêm hàm checkLedStatus(): poll GET /led_status mỗi 2 giây
    - Nếu server trả {"state":"on"}  → digitalWrite(LED_GREEN_PIN, HIGH)
    - Nếu server trả {"state":"off"} → digitalWrite(LED_GREEN_PIN, LOW)
    - Tích hợp vào loop() cùng với poll pin và poll AI

  Phần cứng:
    Servo nắp   : GPIO 4  (MG996R standard 180°)
    Stepper     : GPIO 25 (step), 26 (dir), 27 (ena)
    Ultrasonic  : Trig=GPIO 5, Echo=GPIO 18
    LED đỏ      : GPIO 2
    LED xanh    : GPIO 12  ← MỚI
    Battery 3S  : GPIO 34
    LCD 1602 I2C: SDA=21, SCL=22, addr=0x27
*/

#include <Arduino.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

// ============================================================================
// CẤU HÌNH MẠNG
// ============================================================================
const char* WIFI_SSID   = "NGO KIM THANH";
const char* WIFI_PASS   = "11445555";
const char* SERVER_URL  = "http://10.51.184.239:5001/result";
const char* BATTERY_URL = "http://10.51.184.239:5001/battery";
const char* LED_URL     = "http://10.51.184.239:5001/led_status";  // MỚI

// ============================================================================
// CHÂN KẾT NỐI
// ============================================================================
#define SERVO_PIN       4
#define STEP_PIN        25
#define DIR_PIN         26
#define ENA_PIN         27
#define TRIG_PIN        5
#define ECHO_PIN        18
#define LED_RED_PIN     2
#define LED_GREEN_PIN   12    // MỚI: LED xanh điều khiển từ Dashboard
#define BAT_PIN         34

// ============================================================================
// SERVO (MG996R standard 180°)
//   writeMicroseconds(500)  = 0°  → đóng nắp
//   writeMicroseconds(1450) = 90° → mở nắp
// ============================================================================
#define SERVO_STOP    1500
#define SERVO_CW      1700   // mở nắp
#define SERVO_CCW     1300   // đóng nắp
#define SERVO_90_MS    900   // thời gian quay 90°

// ============================================================================
// ĐỐI TƯỢNG
// ============================================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servoLid;

unsigned long lastBatterySend = 0;
unsigned long lastLedPoll     = 0;    // MỚI
const unsigned long BATTERY_INTERVAL = 30000;
const unsigned long LED_INTERVAL     = 2000;  // poll LED mỗi 2 giây

int       wasteType     = 0;
long long lastTimestamp = -1;

// ============================================================================
// LCD
// ============================================================================
void lcdShow(const char* line1, const char* line2 = "") {
  lcd.setCursor(0, 0);
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", line1 ? line1 : "");
  lcd.print(buf);
  lcd.setCursor(0, 1);
  snprintf(buf, sizeof(buf), "%-16s", line2 ? line2 : "");
  lcd.print(buf);
}

// ============================================================================
// BATTERY
// ============================================================================
float readBatteryVoltage() {
  return (analogRead(BAT_PIN) / 4095.0f) * 3.3f * 4.0f;
}
int calcBatteryPercent(float v) {
  if (v >= 12.6f) return 100;
  if (v <= 9.0f)  return 0;
  return (int)(((v - 9.0f) / 3.6f) * 100.0f);
}

// ============================================================================
// ULTRASONIC
// ============================================================================
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
  return (dur == 0) ? 999.0f : dur * 0.034f / 2.0f;
}

// ============================================================================
// STEPPER
// ============================================================================
void stepperMove(int steps) {
  digitalWrite(ENA_PIN, LOW);
  delayMicroseconds(10);
  for (int i = 0; i < steps; i++) {
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(1000);
    digitalWrite(STEP_PIN, LOW);  delayMicroseconds(1000);
  }
}
void stepperStop() {
  digitalWrite(ENA_PIN,  HIGH);
  digitalWrite(STEP_PIN, LOW);
}

// ============================================================================
// SERVO
// ============================================================================
void servoRotate(int speedUS, int ms) {
  servoLid.setPeriodHertz(50);
  servoLid.attach(SERVO_PIN, 500, 2400);
  servoLid.writeMicroseconds(SERVO_STOP);
  delay(50);
  servoLid.writeMicroseconds(speedUS);
  delay(ms);
  servoLid.writeMicroseconds(SERVO_STOP);
  delay(100);
  servoLid.detach();
}
void servoOpen()  { servoRotate(SERVO_CW,  SERVO_90_MS); }
void servoClose() { servoRotate(SERVO_CCW, SERVO_90_MS); }

// ============================================================================
// WIFI / HTTP
// ============================================================================
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.reconnect();
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

int doGET(const char* url, String& resp) {
  WiFiClient c; HTTPClient h;
  h.begin(c, url);
  h.setConnectTimeout(5000);
  h.setTimeout(8000);
  int code = h.GET();
  if (code == 200) resp = h.getString();
  h.end();
  return code;
}

int doPOST(const char* url, const char* payload) {
  WiFiClient c; HTTPClient h;
  h.begin(c, url);
  h.setConnectTimeout(5000);
  h.setTimeout(8000);
  h.addHeader("Content-Type", "application/json");
  int code = h.POST((uint8_t*)payload, strlen(payload));
  h.end();
  return code;
}

// ============================================================================
// LED XANH – poll trạng thái từ server (MỚI)
// Dashboard gọi POST /led → server lưu state
// ESP32 poll GET /led_status → bật/tắt GPIO 12
// ============================================================================
void checkLedStatus() {
  if (!ensureWiFi()) return;
  String resp;
  if (doGET(LED_URL, resp) == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, resp)) {
      const char* state = doc["state"] | "off";
      if (strcmp(state, "on") == 0) {
        digitalWrite(LED_GREEN_PIN, HIGH);
        Serial.println("[LED] Xanh: BẬT");
      } else {
        digitalWrite(LED_GREEN_PIN, LOW);
        Serial.println("[LED] Xanh: TẮT");
      }
    }
  }
}

// ============================================================================
// AI DETECTION
// ============================================================================
int getDetection() {
  if (!ensureWiFi()) return 0;
  String resp;
  if (doGET(SERVER_URL, resp) != 200) return 0;
  Serial.print("[AI] "); Serial.println(resp);
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  long long ts = doc["timestamp"].as<long long>();
  if (ts == 0 || ts == lastTimestamp) return 0;
  lastTimestamp = ts;
  int cid = doc["class_id"] | 0;
  if (cid > 0) {
    Serial.printf("[AI] %s (ID=%d, conf=%.1f%%)\n",
      (const char*)(doc["class_name"] | "?"), cid,
      (float)(doc["confidence"] | 0.0f) * 100.0f);
    return cid;
  }
  return -1;
}

// ============================================================================
// BATTERY
// ============================================================================
void sendBattery() {
  if (!ensureWiFi()) return;
  float v   = readBatteryVoltage();
  int   pct = calcBatteryPercent(v);
  char  buf[64];
  snprintf(buf, sizeof(buf), "{\"voltage\":%.2f,\"percent\":%d}", v, pct);
  Serial.printf("[BAT] %.2fV %d%%\n", v, pct);
  doPOST(BATTERY_URL, buf);
}

// ============================================================================
// TÊN RÁC
// ============================================================================
const char* wasteName(int t) {
  switch (t) { case 1: return "Organic Waste"; case 2: return "Recyclable Waste"; case 3: return "Hazardous Waste"; default: return "Unknown"; }
}
const char* wasteShort(int t) {
  switch (t) { case 1: return "Organic"; case 2: return "Recyclable"; case 3: return "Hazardous"; default: return "Unknown"; }
}

// ============================================================================
// CHU KỲ PHÂN LOẠI
// ============================================================================
void runSortingCycle(int type) {
  Serial.printf("\n[CYCLE] === %s ===\n", wasteName(type));
  lcdShow("Detected:", wasteShort(type));
  delay(1000);

  int target = 7;
  switch (type) { case 1: target = 7; break; case 2: target = 19; break; case 3: target = 32; break; }

  // GĐ1: Tiến
  lcdShow("Moving...", wasteShort(type));
  digitalWrite(DIR_PIN, HIGH); delayMicroseconds(10);
  int safety = 0;
  while (safety < 10000) {
    float d = readDistance();
    Serial.printf("  dist=%.1f cm\n", d);
    if (d >= (float)target) break;
    stepperMove(10); safety += 10; delay(10);
  }
  stepperStop();
  delay(500); // chờ từ trường stepper tan

  // GĐ2: Mở nắp
  lcdShow("Opening lid...", wasteShort(type));
  servoOpen();

  // GĐ3: Giữ 2 giây
  lcdShow("Dropping...", wasteShort(type));
  delay(2000);

  // GĐ4: Đóng nắp
  lcdShow("Closing lid...", wasteShort(type));
  servoClose();
  delay(300);

  // GĐ5: Về home
  lcdShow("Returning...", "");
  digitalWrite(DIR_PIN, LOW); delayMicroseconds(10);
  safety = 0;
  while (safety < 10000) {
    float d = readDistance();
    Serial.printf("  dist=%.1f cm\n", d);
    if (d <= 2.0f) break;
    stepperMove(10); safety += 10; delay(10);
  }
  stepperStop();

  lcdShow("Done! Bin:", wasteShort(type));
  delay(2000);
  lcdShow("Waiting for", "AI detection...");
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== SMART BIN FINAL (LED xanh) ===");

  // I2C + LCD
  Wire.begin(21, 22);
  delay(200);
  lcd.init(); lcd.backlight();
  lcdShow("Smart Bin", "Starting...");

  // Stepper
  pinMode(STEP_PIN, OUTPUT); pinMode(DIR_PIN, OUTPUT); pinMode(ENA_PIN, OUTPUT);
  digitalWrite(ENA_PIN, HIGH); digitalWrite(STEP_PIN, LOW); digitalWrite(DIR_PIN, LOW);

  // Ultrasonic
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // LED đỏ
  pinMode(LED_RED_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, LOW);

  // LED xanh (MỚI)
  pinMode(LED_GREEN_PIN, OUTPUT);
  digitalWrite(LED_GREEN_PIN, LOW);
  Serial.println("[LED] Xanh GPIO 12 sẵn sàng.");

  // Servo timer
  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  Serial.println("[SERVO] Timers allocated.");

  // WiFi
  lcdShow("Connecting WiFi", "Please wait...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) {
    delay(500); Serial.print("."); timeout++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.printf("[WiFi] OK IP=%s\n", ip.c_str());
    lcdShow("WiFi Connected!", ip.c_str());
  } else {
    Serial.println("[WiFi] FAIL!");
    lcdShow("WiFi ERROR!", "Check SSID/PW");
  }

  delay(2000);
  lcdShow("Waiting for", "AI detection...");
  Serial.println("[SETUP] Done.");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  unsigned long now = millis();

  // Gửi pin mỗi 30 giây
  if (now - lastBatterySend >= BATTERY_INTERVAL) {
    lastBatterySend = now;
    sendBattery();
  }

  // Poll LED mỗi 2 giây (MỚI)
  if (now - lastLedPoll >= LED_INTERVAL) {
    lastLedPoll = now;
    checkLedStatus();
  }

  // Chờ AI phát hiện rác
  wasteType = 0;
  while (wasteType == 0) {
    unsigned long t = millis();

    if (t - lastBatterySend >= BATTERY_INTERVAL) {
      lastBatterySend = t;
      sendBattery();
    }
    // Tiếp tục poll LED ngay cả khi đang chờ AI
    if (t - lastLedPoll >= LED_INTERVAL) {
      lastLedPoll = t;
      checkLedStatus();
    }

    wasteType = getDetection();
    if (wasteType == 0) delay(2000);
  }

  // AI không nhận diện được
  if (wasteType == -1) {
    digitalWrite(LED_RED_PIN, HIGH);
    lcdShow("Not Detected!", "!! Warning !!");
    delay(3000);
    digitalWrite(LED_RED_PIN, LOW);
    lcdShow("Waiting for", "AI detection...");
    wasteType = 0;
    delay(1000);
    return;
  }

  // Chạy chu kỳ phân loại
  runSortingCycle(wasteType);
  wasteType = 0;
  delay(500);
}
