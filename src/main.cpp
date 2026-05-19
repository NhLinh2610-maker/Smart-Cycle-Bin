#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============ CONFIG ============
const char* WIFI_SSID = "DL";
const char* WIFI_PASS = "26262626";
const char* SERVER_URL = "https://192.168.1.235:5000/result";

Servo myServo;

// == Servo Setup ==
const int servoPin = 4;

// == Stepper Setup ==
const int stepPin = 25;
const int dirPin = 26;
const int enablePin = 27;

// == Cảm biến siêu âm Setup ==
const int trigPin = 5;
const int echoPin = 18;

int wasteType = 0;
long long lastTimestamp = 0; // FIX: long long thay vì unsigned long để chứa Python timestamp ms

float readUltrasonicDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH);
  float distance = duration * 0.034 / 2;
  return distance;
}

// ============ Động cơ step chuyển động theo chiểu thuận ============
void stepperForward() {
  digitalWrite(dirPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(enablePin, LOW);
  for (int i = 0; i < 200; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);
  }
}

// =========== Động cơ step chuyển động theo chiểu nghịch ============
void stepperBackward() {
  digitalWrite(dirPin, LOW);
  delayMicroseconds(10);
  digitalWrite(enablePin, LOW);
  for (int i = 0; i < 200; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);
  }
}

// ============ Dừng động cơ step ============
void stepperStop() {
  digitalWrite(enablePin, HIGH);
  digitalWrite(stepPin, LOW);
}


// ============ Quay servo 90 độ rồi quay về 0 độ ============
void servoRotate90() {
  Serial.println("Servo quay 90 do");
  myServo.write(90);
  delay(1000);
  Serial.println("Servo quay ve 0 do");
  myServo.write(0);
  delay(1000);
}

// ============ Lấy kết quả AI từ server ============
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
      // Dùng long long để chứa timestamp ms (Python: int(time.time()*1000) > 4 billion)
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
        float conf = doc["confidence"] | 0.0;

        Serial.print(">>> PHÁT HIỆN RÁC: ");
        Serial.print(className);
        Serial.print(" (ID=");
        Serial.print(classId);
        Serial.print(", conf=");
        Serial.print(conf * 100, 1);
        Serial.println("%)");

        http.end();
        return classId;
      } else if (ts == lastTimestamp) {
        Serial.println("Không có phát hiện mới (cùng timestamp)");
      } else {
        Serial.println("Không phát hiện rác (class_id=0)");
      }
    } else {
      Serial.print("Lỗi parse JSON: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("Lỗi HTTP: ");
    Serial.println(httpCode);
  }

  http.end();
  return 0;
}

// =========== Chuyển waste type ID thành tên để hiển thị ============
const char* getWasteName(int type) {
  switch (type) {
    case 1: return "Organic Waste";
    case 2: return "Recyclable Waste";
    case 3: return "Inorganic Waste";
    case 4: return "Hazardous Waste";
    default: return "Unknown";
  }
}


// ================== MAIN CODE ==================
void setup() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);

  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enablePin, OUTPUT);
  digitalWrite(enablePin, HIGH);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  Serial.begin(115200);
  Serial.println("=== SMART BIN - AI WASTE DETECTION ===");
  Serial.println("Đang khởi động hệ thống...");
  Serial.println("-------------------");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Đang kết nối WiFi");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi đã kết nối! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("LỖI: Không kết nối được WiFi!");
  }

  myServo.write(0);
  delay(1000);
}

void loop() {
  Serial.println("===================");
  Serial.println("Chờ phát hiện rác từ AI...");

  wasteType = 0;
  while (wasteType == 0) {
    wasteType = getDetectionFromServer();
    if (wasteType == 0) {
      delay(2000);
    }
  }

  Serial.print("Phát hiện: ");
  Serial.print(getWasteName(wasteType));
  Serial.print(" -> Ngăn ");
  Serial.println(wasteType);
  Serial.println("-------------------");

  int stopDistanceForward = 0;
  int stopDistanceBackward = 4;

  switch (wasteType) {
    case 1: stopDistanceForward = 10; Serial.println("Khỏang cách dừng: 10 cm"); break;
    case 2: stopDistanceForward = 14; Serial.println("Khỏang cách dừng: 14 cm"); break;
    case 3: stopDistanceForward = 18; Serial.println("Khỏang cách dừng: 18 cm"); break;
    case 4: stopDistanceForward = 22; Serial.println("Khỏang cách dừng: 22 cm"); break;
    default: stopDistanceForward = 10; break;
  }

  Serial.println("PHASE 1: Động cơ step quay thuận");
  digitalWrite(dirPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(enablePin, LOW);

  float currentDistance = 0;
  while (true) {
    currentDistance = readUltrasonicDistance();
    Serial.print("Khỏang cách: ");
    Serial.print(currentDistance);
    Serial.println(" cm");
    if (currentDistance >= stopDistanceForward) {
      Serial.print("Đạt khoảng cách dừng: ");
      Serial.print(stopDistanceForward);
      Serial.println(" cm");
      break;
    }
    stepperForward();
    delay(50);
  }

  stepperStop();
  Serial.println("Động cơ step đã dừng");

  Serial.println("PHASE 2: Servo hoạt động");
  servoRotate90();

  Serial.println("PHASE 3: Chờ 2 giây");
  delay(2000);

  Serial.println("PHASE 4: Động cơ step quay nghịch");
  digitalWrite(dirPin, LOW);
  delayMicroseconds(10);
  digitalWrite(enablePin, LOW);

  while (true) {
    currentDistance = readUltrasonicDistance();
    Serial.print("Khỏang cách: ");
    Serial.print(currentDistance);
    Serial.println(" cm");
    if (currentDistance <= stopDistanceBackward) {
      Serial.print("Đạt khoảng cách dừng: ");
      Serial.print(stopDistanceBackward);
      Serial.println(" cm");
      break;
    }
    stepperBackward();
    delay(50);
  }

  stepperStop();
  Serial.println("Động cơ step đã dừng");

  Serial.println("===================");
  Serial.println("Hòan thành 1 chu kỳ!");
  Serial.println("Chuẩn bị cho chu kỳ tiếp theo...");
  Serial.println();

  wasteType = 0;
  delay(3000);
}
