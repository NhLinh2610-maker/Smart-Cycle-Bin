# 🗑️ Smart Bin - AI Waste Detection System

Hệ thống thùng rác thông minh sử dụng AI (YOLO) để phân loại rác tự động qua camera điện thoại.

## Kiến trúc hệ thống

```
┌─────────────┐    HTTP POST     ┌──────────────────┐    HTTP GET     ┌──────────┐
│  Phone Cam  │ ──── image ───>  │  Python Server   │ <─── poll ────  │  ESP32   │
│  (Web App)  │                  │  (Flask + YOLO)  │                 │  (Motor) │
└─────────────┘                  └──────────────────┘                 └──────────┘
                                       │
                                  YOLO Model
                                  (best.pt)
                                       │
                              ┌────────┴────────┐
                              │   4 loại rác:   │
                              │ 1=Organic       │
                              │ 2=Recyclable    │
                              │ 3=Inorganic     │
                              │ 4=Hazardous     │
                              └─────────────────┘
```

## Cấu trúc thư mục

```
cycle_bin/
├── src/
│   └── main.cpp          # Code ESP32 - WiFi, HTTP client, điều khiển motor
├── yolo_server/
│   ├── server.py         # Flask server - YOLO detection API
│   ├── index.html        # Web app - Phone camera UI
│   ├── requirements.txt  # Python dependencies
│   └── uploads/          # Thư mục lưu ảnh tạm
├── platformio.ini        # PlatformIO config (ESP32 + ArduinoJson)
└── README.md
```

---

## Chi tiết từng file

### 1. `yolo_server/server.py` - Python Flask Server

**Chức năng:**
- Load model YOLO (ví dụ: `best.pt`)
- Nhận ảnh từ phone camera, chạy YOLO detection
- Lưu kết quả và cung cấp API cho ESP32 poll

**API Endpoints:**

|    Endpoint    | Method |                  Mô tả                  |
|----------------|--------|-----------------------------------------|
| `/`            | GET    | Trả về web app (index.html)             |
| `/detect`      | POST   | Nhận ảnh → chạy YOLO → trả kết quả JSON |
| `/result`      | GET    | ESP32 poll kết quả detection gần nhất   |
| `/status`      | GET    | Check server status                     |
| `/open_door`   | POST   | Web app gọi để yêu cầu mở cửa thùng rác |
| `/door_status` | GET    | ESP32 poll trạng thái yêu cầu mở cửa    |

**Response format `/result`:**
```json example:
{
  "class_id": 2,
  "class_name": "Recyclable Waste",
  "confidence": 0.89,
  "timestamp": 1778841961206
}
```

**Class mapping:**

| class_id |      Tên lớp     |    Ngăn rác   |       Chức năng LED        |
|----------|------------------|---------------|----------------------------|
|    0     | Không phát hiện  |      -        | 🔴 LED Đỏ sáng (cảnh báo) |
|    1     | Organic Waste    | Ngăn 1 (10cm) |         Tắt LED            |
|    2     | Recyclable Waste | Ngăn 2 (14cm) |         Tắt LED            |
|    3     | Inorganic Waste  | Ngăn 3 (18cm) |         Tắt LED            |
|    4     | Hazardous Waste  | Ngăn 4 (22cm) |         Tắt LED            |

**⚠️ Tính năng mới: LED Cảnh báo đỏ**
- Khi AI không nhận diện được vật thể (`class_id = 0`):
  - 🔴 LED đỏ sẽ sáng trong 3 giây để cảnh báo
  - ESP32 không di chuyển motor, chờ detection mới
- Khi nhận diện thành công (class 1-4): LED đỏ tắt

**HTTPS:** Server tự động dùng SSL (adhoc) nếu có pyOpenSSL → Camera phone yêu cầu HTTPS.

---

### 2. `yolo_server/index.html` - Web App Phone Camera

**Chức năng:**
- Hiển thị camera phone (trước/sau)
- Chụp ảnh và gửi lên server `/detect`
- Hiển thị kết quả nhận diện
- Chế độ tự động chụp mỗi 3 giây
- 🚪 Nút "Mở cửa lấy rác" điều khiển servo từ xa

**Tính năng:**
- 🔄 Đổi camera trước/sau
- 📸 Chụp & Nhận diện (thủ công)
- ⏱️ Tự động chụp (mỗi 3s)
- 📊 Hiển thị loại rác, độ tin cậy, ngăn rác
- 🚪 **Nút "Mở cửa lấy rác"**: Gửi lệnh mở cửa qua ESP32
- ⚠️ Kiểm tra HTTPS/HTTP và polyfill getUserMedia

**Giao diện:**
- Dark theme, mobile-friendly
- Color-code: 🟢 Organic, 🔵 Recyclable, 🟠 Inorganic, 🔴 Hazardous

---

### 3. `src/main.cpp` - ESP32 Code

**Chức năng:**
- Kết nối WiFi
- Poll server `/result` mỗi 2 giây
- Khi phát hiện rác mới → điều khiển motor đến ngăn đúng
- 🔴 Khi AI không nhận diện được vật thể → Sáng LED đỏ cảnh báo
- 🚪 Kiểm tra `/door_status` mỗi 2 giây → Khi có yêu cầu mở cửa từ web → Servo Door quay 90° (giữ 5 giây rồi đóng lại)

**Luồng hoạt động:**
```
1. Kết nối WiFi
2. Loop: Poll server → chờ phát hiện rác mới
3. Khi phát hiện rác (class 1-4):
   ├── PHASE 1: Stepper quay THUẬN đến khoảng cách đúng (10/14/18/22 cm)
   ├── PHASE 2: Servo quay 90° → đổ rác vào ngăn → Trở về vị trí ban đầu
   ├── PHASE 3: Chờ 2 giây
   └── PHASE 4: Stepper quay NGHỊCH về vị trí gốc (4 cm)
   └── Trả về bước 2
4. Khi AI không nhận diện được (class 0):
   ├── 🔴 Sáng LED đỏ trong 3 giây (cảnh báo)
   ├── LED tắt
   └── Trả về bước 2
5. Khi có yêu cầu mở cửa từ Web App:
   ├── ESP32 poll `/door_status` → phát hiện flag `open=true`
   ├── Servo Door quay 90° (mở cửa)
   ├── Giữ cửa mở trong 5 giây
   └── Servo Door quay về 0° (đóng cửa)
```

**Hardware pins:**

|    Component    |   Pin   |        Chức năng         |
|-----------------|---------|--------------------------|
| Servo           | GPIO 4  | PWM điều khiển nắp thùng |
| Servo Door      | GPIO 16 | Điều khiển cửa lấy rác (mở/đóng) |
| Stepper Step    | GPIO 25 | Xung bước DRV8825        |
| Stepper Dir     | GPIO 26 | Hướng quay DRV8825       |
| Stepper Enable  | GPIO 27 | Enable/Disable DRV8825   |
| Ultrasonic Trig | GPIO 5  | Trigger cảm biến         |
| Ultrasonic Echo | GPIO 18 | Echo cảm biến            |
| LED Đỏ (Cảnh báo)| GPIO 2 | Báo AI không nhận diện được |

**Config cần đổi:**
```cpp
const char* WIFI_SSID = "YOUR_WIFI";      // WiFi name
const char* WIFI_PASS = "YOUR_PASSWORD";   // WiFi password
const char* SERVER_URL = "https://IP:5000/result"; // Server URL
const char* DOOR_URL = "https://IP:5000/door_status"; // Door status URL
const char* BATTERY_URL = "https://IP:5000/battery";  // Battery API URL
```

**⚡ Tính năng mới: Battery Dashboard**
- ESP32 đọc điện áp pin 3S 18650 qua ADC (GPIO 34)
- Gửi dữ liệu lên server mỗi 30 giây (voltage + %)
- Web app hiển thị dashboard pin với màu sắc:
  - 🟢 >50%: Xanh lá (good)
  - 🟠 20-50%: Cam (medium)
  - 🔴 <20%: Đỏ (low - cần sạc)

**Sơ đồ voltage divider (3S 18650 → ADC ESP32):**
```
   3S 18650 (+) ─── 30kΩ ───+─── 10kΩ ─── GND
                              │
                            ADC (GPIO34)
     Vbat ~9-12.6V → Vadc ~2.9-3.15V (an toàn cho ADC ESP32)
```
```

**Bug fix quan trọng:** `lastTimestamp` dùng `long long` (64-bit) vì Python `int(time.time()*1000)` tạo timestamp > 4 tỷ, vượt quá `unsigned long` 32-bit của ESP32.

---

### 4. `platformio.ini` - PlatformIO Config

```ini
[env:featheresp32]
platform = espressif32
board = featheresp32
framework = arduino
lib_deps =
    ESP32Servo
    bblanchon/ArduinoJson@^7.0.0
upload_port = COM6 #COM ESP32/ARDUINO
monitor_port = COM6 #COM ESP32/ARDUINO
monitor_speed = 115200
```

Thư viện: `ESP32Servo` (servo PWM) + `ArduinoJson` (parse JSON từ server).

---

## Hướng dẫn triển khai

### Bước 1: Cài đặt Python Server

```bash
cd yolo_server
pip install -r requirements.txt
# requirements: flask, ultralytics, opencv-python, pyOpenSSL
```

### Bước 2: Chạy Server

```bash
python server.py
```

Server chạy tại `https://<IP_PC>:5000`
- Kiểm tra IP: `ipconfig` (Windows)

### Bước 3: Cấu hình ESP32

Sửa 3 dòng trong `src/main.cpp`:
```cpp
const char* WIFI_SSID = "Tên_WiFi";
const char* WIFI_PASS = "Mật_khẩu";
const char* SERVER_URL = "https://<IP_PC>:5000/result";
```

### Bước 4: Upload ESP32

```bash
pio run -t upload
pio device monitor
```

### Bước 5: Sử dụng

1. Mở browser trên phone → truy cập `https://<IP_PC>:5000`
2. Chấp nhận certificate warning (SSL self-signed)
3. Cho phép truy cập camera
4. Chụp ảnh rác hoặc bật chế độ tự động
5. ESP32 tự động nhận kết quả và điều khiển motor/LED

---