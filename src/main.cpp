/*
  Smart Bin - AI Waste Detection + Battery Dashboard
  VERSION v6 — Servo Standard (không phải 360°)

  Servo MG996R — Standard Servo (KHÔNG phải continuous rotation):
    writeMicroseconds(500)  = 0°  → đóng nắp
    writeMicroseconds(1450) = 90° → mở nắp

  Nguyên nhân lỗi v5.4 "quay 2 vòng":
    SERVO_TRAVEL_MS=1500ms không đủ cho servo chịu tải cơ học.
    Servo vật lý chưa về 0° nhưng currentServoUS đã cập nhật=500.
    Re-anchor lần sau dùng điểm tham chiếu sai → servo tìm lại vị trí
    bằng cách quay vượt qua 0° rồi quay lại → "2 vòng".

  Fix v6:
    1. Bỏ hoàn toàn re-anchor + currentServoUS tracking
    2. attach() → writeMicroseconds(target) → delay đủ lớn → detach()
    3. Mỗi thao tác servo là độc lập, không phụ thuộc lịch sử
    4. detach() sau mỗi lần dùng → servo giữ vị trí bằng cơ học, không jitter

  Phần cứng:
    Servo nắp     : GPIO 4  (MG996R, standard servo)
    Stepper       : GPIO 25 (step), 26 (dir), 27 (ena)
    Ultrasonic    : Trig=GPIO 5, Echo=GPIO 18
    LED đỏ        : GPIO 2
    Battery 3S    : GPIO 34
    LCD 1602 I2C  : SDA=21, SCL=22, addr=0x27
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
const char* WIFI_SSID   = "Start Coffee Tea";
const char* WIFI_PASS   = "xincamon";
const char* SERVER_URL  = "http://192.168.1.52:5001/result";
const char* BATTERY_URL = "http://192.168.1.52:5001/battery";

// ============================================================================
// CHÂN KẾT NỐI
// ============================================================================
#define SERVO_PIN    4
#define STEP_PIN     25
#define DIR_PIN      26
#define ENA_PIN      27
#define TRIG_PIN     5
#define ECHO_PIN     18
#define LED_RED_PIN  2
#define BAT_PIN      34

// ============================================================================
// SERVO 360° (Continuous Rotation) — đã calibrate
//
// Servo 360° KHÔNG định vị theo góc, chỉ quay theo tốc độ + thời gian:
//   1500µs = DỪNG (deadband)
//   1300µs = quay CW  (thuận kim đồng hồ)
//   1700µs = quay CCW (ngược kim đồng hồ)
//
// Calibration kết quả:
//   CW  900ms @ 1700µs = 90° → mở nắp
//   CCW 900ms @ 1300µs = 90° → đóng nắp về vị trí ban đầu
// ============================================================================
#define SERVO_STOP    1500   // µs — dừng hoàn toàn
#define SERVO_CW      1700   // µs — quay CW (mở nắp)
#define SERVO_CCW     1300   // µs — quay CCW (đóng nắp)
#define SERVO_90_MS    900   // ms — thời gian quay 90° (đã calibrate)

// ============================================================================
// ĐỐI TƯỢNG
// ============================================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servoLid;

unsigned long lastBatterySend    = 0;
const unsigned long BATTERY_INTERVAL = 30000;
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
// SERVO 360° — hàm điều khiển
//
// servoRotate(speedUS, ms):
//   1. attach()
//   2. writeMicroseconds(STOP) → đảm bảo servo đứng yên trước khi ra lệnh
//   3. delay(50ms)             → settle
//   4. writeMicroseconds(speedUS) → servo bắt đầu quay
//   5. delay(ms)               → chờ đúng góc cần quay
//   6. writeMicroseconds(STOP) → dừng servo
//   7. delay(100ms)            → servo dừng hẳn
//   8. detach()                → cắt PWM, không còn xung nào
//
// Tại sao write(STOP) trước lệnh quay:
//   Khi attach(), LEDC channel có thể xuất xung không xác định trong ~20ms
//   Write(STOP) ngay sau attach đảm bảo servo không giật trước khi quay đúng hướng
// ============================================================================
void servoRotate(int speedUS, int ms) {
  Serial.printf("[SERVO] Rotate: speed=%dus, time=%dms\n", speedUS, ms);

  servoLid.setPeriodHertz(50);
  servoLid.attach(SERVO_PIN, 500, 2400);

  // Đảm bảo servo đứng yên trước khi ra lệnh
  servoLid.writeMicroseconds(SERVO_STOP);
  delay(50);

  // Quay trong thời gian ms
  servoLid.writeMicroseconds(speedUS);
  delay(ms);

  // Dừng
  servoLid.writeMicroseconds(SERVO_STOP);
  delay(100);

  // Detach — không còn xung PWM nào
  servoLid.detach();

  Serial.println("[SERVO] Done.");
}

// Mở nắp: quay CW 900ms = 90°
void servoOpen() {
  Serial.println("[LID] Opening (CW 900ms)...");
  servoRotate(SERVO_CW, SERVO_90_MS);
  Serial.println("[LID] Open done.");
}

// Đóng nắp: quay CCW 900ms = 90° (về vị trí ban đầu)
void servoClose() {
  Serial.println("[LID] Closing (CCW 900ms)...");
  servoRotate(SERVO_CCW, SERVO_90_MS);
  Serial.println("[LID] Close done.");
}

// ============================================================================
// WIFI / HTTP
// ============================================================================
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] Reconnecting...");
  WiFi.reconnect();
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] OK.");
      return true;
    }
    delay(500);
  }
  Serial.println("[WiFi] FAIL.");
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
  if (ts == 0 || ts == lastTimestamp) {
    Serial.println("[AI] No new detection.");
    return 0;
  }
  lastTimestamp = ts;

  int cid = doc["class_id"] | 0;
  if (cid > 0) {
    const char* cname = doc["class_name"] | "?";
    float conf        = doc["confidence"]  | 0.0f;
    Serial.printf("[AI] Detected: %s (ID=%d, conf=%.1f%%)\n",
                  cname, cid, conf * 100.0f);
    return cid;
  }
  Serial.println("[AI] Not detected (class_id=0).");
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
  switch (t) {
    case 1:  return "Organic Waste";
    case 2:  return "Recyclable Waste";
    case 3:  return "Hazardous Waste";
    default: return "Unknown";
  }
}
const char* wasteShort(int t) {
  switch (t) {
    case 1:  return "Organic";
    case 2:  return "Recyclable";
    case 3:  return "Hazardous";
    default: return "Unknown";
  }
}

// ============================================================================
// CHU KỲ PHÂN LOẠI
// ============================================================================
void runSortingCycle(int type) {
  Serial.printf("\n[CYCLE] === START: %s ===\n", wasteName(type));
  lcdShow("Detected:", wasteShort(type));
  delay(1000);

  // Khoảng cách cảm biến khi đến đúng ngăn
  int target = 7;
  switch (type) {
    case 1: target = 7;  break;  // Organic
    case 2: target = 19; break;  // Recyclable
    case 3: target = 32; break;  // Hazardous
  }
  Serial.printf("[CYCLE] Target dist: %d cm\n", target);

  // --- GĐ1: Tiến đến ngăn ---
  Serial.println("[CYCLE] GD1: Moving forward...");
  lcdShow("Moving...", wasteShort(type));
  digitalWrite(DIR_PIN, HIGH);
  delayMicroseconds(10);

  int safety = 0;
  while (safety < 10000) {
    float d = readDistance();
    Serial.printf("  dist=%.1f cm (target=%d)\n", d, target);
    if (d >= (float)target) {
      Serial.println("  Reached target!");
      break;
    }
    stepperMove(10);
    safety += 10;
    delay(10);
  }
  stepperStop();
  if (safety >= 10000) Serial.println("[CYCLE] WARNING: safety limit hit!");

  // Chờ từ trường stepper tan hết trước khi servo hoạt động
  // Tránh nhiễu điện từ ảnh hưởng PWM servo
  delay(500);

  // --- GĐ2: Mở nắp 90° ---
  Serial.println("[CYCLE] GD2: Open lid");
  lcdShow("Opening lid...", wasteShort(type));
  servoOpen();

  // --- GĐ3: Giữ mở 2 giây cho rác rơi vào ---
  Serial.println("[CYCLE] GD3: Holding open 2s...");
  lcdShow("Dropping...", wasteShort(type));
  delay(2000);

  // --- GĐ4: Đóng nắp về 0° ---
  // servoClose() hoàn toàn độc lập với servoOpen()
  // attach mới → writeMicroseconds(500) → delay(2000ms) → detach
  // Servo đi từ bất kỳ vị trí nào về 0° trong 2000ms
  Serial.println("[CYCLE] GD4: Close lid");
  lcdShow("Closing lid...", wasteShort(type));
  servoClose();

  // Chờ thêm để servo lock cơ học hoàn toàn
  delay(300);

  // --- GĐ5: Về home ---
  Serial.println("[CYCLE] GD5: Returning home...");
  lcdShow("Returning...", "");
  digitalWrite(DIR_PIN, LOW);
  delayMicroseconds(10);

  safety = 0;
  while (safety < 10000) {
    float d = readDistance();
    Serial.printf("  dist=%.1f cm\n", d);
    if (d <= 2.0f) {
      Serial.println("  Home reached!");
      break;
    }
    stepperMove(10);
    safety += 10;
    delay(10);
  }
  stepperStop();

  Serial.printf("[CYCLE] === DONE: %s ===\n", wasteName(type));
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
  Serial.println("\n=== SMART BIN v6 ===");

  // I2C + LCD
  Wire.begin(21, 22);
  delay(200);
  lcd.init();
  lcd.backlight();
  lcdShow("Smart Bin v6", "Starting...");
  Serial.println("[LCD] OK.");

  // Stepper — disable ngay
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(ENA_PIN,  OUTPUT);
  digitalWrite(ENA_PIN,  HIGH); // HIGH = disable
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN,  LOW);

  // Ultrasonic
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // LED
  pinMode(LED_RED_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, LOW);

  // Servo 360° — chỉ allocate timer, KHÔNG attach, KHÔNG gọi servoClose()
  // Lý do: servo 360° không có vị trí home tuyệt đối.
  // Gọi servoClose() khi boot sẽ làm servo quay CCW 900ms không cần thiết
  // nếu nắp đang đóng sẵn → nắp bị lệch.
  // Người dùng tự đặt nắp về vị trí đóng trước khi bật nguồn.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  Serial.println("[SERVO] Timers allocated. Servo idle (not attached).");
  Serial.println("[SERVO] NOTE: Dam bao nap dang dong truoc khi bat nguon!");

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
    Serial.printf("[WiFi] Connected! IP=%s\n", ip.c_str());
    lcdShow("WiFi Connected!", ip.c_str());
  } else {
    Serial.println("[WiFi] FAIL!");
    lcdShow("WiFi ERROR!", "Check SSID/PW");
  }

  delay(2000);
  lcdShow("Waiting for", "AI detection...");
  Serial.println("[SETUP] Done. Entering loop().");
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

  // Chờ AI phát hiện rác
  Serial.println("[LOOP] Waiting for AI...");
  wasteType = 0;

  while (wasteType == 0) {
    unsigned long t = millis();
    if (t - lastBatterySend >= BATTERY_INTERVAL) {
      lastBatterySend = t;
      sendBattery();
    }
    wasteType = getDetection();
    if (wasteType == 0) delay(2000);
  }

  // AI không nhận diện được → LED đỏ
  if (wasteType == -1) {
    Serial.println("[LOOP] Not detected → LED warning!");
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