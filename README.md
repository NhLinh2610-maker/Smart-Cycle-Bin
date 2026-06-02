# 🗑️ Smart Bin - AI Waste Detection System

Hệ thống thùng rác thông minh sử dụng AI (YOLO) để phân loại rác tự động qua camera điện thoại, tích hợp dashboard realtime và điều khiển từ xa qua web.

---

## Kiến trúc hệ thống

```
┌──────────────┐  POST /detect  ┌──────────────────────┐  GET /result     ┌──────────────┐
│  Phone Cam   │ ─── ảnh ────→  │   Python Server      │ ←─── poll ─────  │    ESP32     │
│  (Web App)   │                │  (Flask + YOLO)      │                  │  (Motor/LED) │
└──────────────┘                └──────────────────────┘                  └──────────────┘
                                         │          ↑
                                    YOLO Model   POST /battery
                                    (best.pt)    GET /led_status
                                         │          │
                               ┌─────────┴──────────┴────────┐
                               │       API Endpoints          │
                               │  /api/stats  /api/detections │
                               │  /led        /led_status     │
                               └────────────────────────────-─┘
                                         │
                               ┌─────────┴──────────┐
                               │    Dashboard        │
                               │  (dashboard.html)   │
                               └─────────────────────┘
```

---

## Tính năng chính

- **Phân loại rác bằng AI (YOLO):** Nhận diện 3 loại rác qua camera điện thoại
- **Điều khiển cơ học tự động:** Stepper motor đưa khay rác đến đúng ngăn, servo mở/đóng nắp
- **Dashboard realtime:** Theo dõi số lần phát hiện, pin, trạng thái hệ thống
- **Điều khiển LED từ xa:** Dashboard bật/tắt LED xanh GPIO 12 qua API
- **Giám sát pin:** ESP32 đọc điện áp pin 3S 18650, gửi lên server mỗi 30 giây
- **Zoom camera + Flash:** Web app hỗ trợ hardware zoom (nếu có), CSS zoom fallback, torch API

---

## Phân loại rác

| Class ID | Tên | Ngăn rác | Khoảng cách |
|----------|-----|----------|-------------|
| 0 | Không phát hiện | — | — |
| 1 | Organic Waste | Ngăn 1 | 7 cm |
| 2 | Recyclable Waste | Ngăn 2 | 19 cm |
| 3 | Hazardous Waste | Ngăn 3 | 32 cm |

---

## Cấu trúc thư mục

```
cycle_bin/
├── src/
│   └── main.cpp              # Code ESP32
├── yolo_server/
│   ├── server.py             # Flask server – YOLO detection + LED + Battery API
│   ├── index.html            # Web app – camera điện thoại
│   ├── dashboard.html        # Dashboard realtime
│   ├── requirements.txt      # Python dependencies
│   └── uploads/              # Ảnh tạm (tự tạo khi chạy)
├── platformio.ini            # PlatformIO config
└── README.md
```

---

## Chi tiết từng thành phần

### 1. `server.py` — Python Flask Server

**Chức năng:** Load YOLO model, nhận ảnh từ phone, detect rác, cung cấp API cho ESP32 và Dashboard.

#### API Endpoints

| Endpoint | Method | Mô tả |
|----------|--------|-------|
| `/` | GET | Trả về web app (`index.html`) |
| `/detect` | POST | Nhận ảnh → chạy YOLO → lưu kết quả |
| `/result` | GET | ESP32 poll kết quả detect gần nhất |
| `/status` | GET | Kiểm tra trạng thái server |
| `/battery` | POST/GET | ESP32 gửi / Dashboard đọc thông tin pin |
| `/led` | POST | Dashboard gọi để bật/tắt LED xanh |
| `/led_status` | GET | ESP32 poll trạng thái LED mỗi 2 giây |
| `/api/stats` | GET | Dashboard lấy thống kê tổng hợp |
| `/api/detections` | GET | Dashboard lấy lịch sử detect (limit tùy chọn) |
| `/dashboard` | GET | Trả về trang dashboard |

#### Response format `/result`

```json
{
  "class_id": 2,
  "class_name": "Recyclable Waste",
  "confidence": 0.89,
  "timestamp": 1778841961206
}
```

#### Response format `/api/stats`

```json
{
  "total_detections": 42,
  "counts": {
    "organic": 15,
    "recyclable": 20,
    "hazardous": 5,
    "unknown": 2
  },
  "battery": { "voltage": 11.8, "percent": 72, "timestamp": 1778841961000 },
  "last_detection": { ... }
}
```

#### Cấu hình server

```python
MODEL_PATH = r"D:\YOLO\IOT\Src\best.pt"   # Đường dẫn đến model YOLO
```

Server tự động dùng HTTPS (port 5000, yêu cầu `pyOpenSSL`) cho phone camera và HTTP (port 5001) cho ESP32.

---

### 2. `index.html` — Web App Camera (Phone)

**Chức năng:** Giao diện chụp ảnh trên điện thoại, gửi lên server để nhận diện.

**Tính năng:**

- 🔄 Đổi camera trước/sau
- 📸 Chụp thủ công và gửi detect
- ⏱️ Chế độ tự động chụp mỗi 3 giây
- 🔍 Zoom slider + preset buttons (hardware zoom ưu tiên, fallback CSS zoom)
- ⚡ Torch/Flash (Android Chrome + camera sau)
- 🔋 Hiển thị dashboard pin theo thời gian thực
- 📊 Hiển thị kết quả: loại rác, độ tin cậy, ngăn rác

**Color coding kết quả:**

| Loại rác | Màu |
|----------|-----|
| Organic Waste | 🟢 Xanh lá |
| Recyclable Waste | 🔵 Xanh dương |
| Hazardous Waste | 🔴 Đỏ |
| Không phát hiện | ⚫ Xám |

**Yêu cầu:** Phải truy cập qua HTTPS (trình duyệt chặn camera trên HTTP). Dùng ngrok nếu cần tunnel:

```bash
ngrok http 5000
```

---

### 3. `dashboard.html` — Dashboard Realtime

**Chức năng:** Giao diện giám sát và điều khiển hệ thống, chạy trên máy tính hoặc tablet.

**Widget chính:**

| Widget | Nguồn dữ liệu |
|--------|--------------|
| Fill Level | Tính từ `total_detections` (50 lần = 100%) |
| Battery | `POST /battery` từ ESP32 |
| Detected Waste | `GET /api/stats` |
| Temperature | Fake data (demo) |
| AI Prediction | Fake data (demo) |
| Waste Analytics (chart) | Một phần từ server, một phần fake |
| Recent Detections table | `GET /api/detections` |

**Điều khiển từ xa:**

- **Open Bin:** Yêu cầu nhập mật khẩu → gọi `POST /led {"state":"on"}` → ESP32 bật LED xanh GPIO 12
- **Close Bin:** Gọi `POST /led {"state":"off"}` → ESP32 tắt LED xanh
- **Welcome 🌸:** Hiển thị overlay animation (fireworks + tên giảng viên)

**LED status indicator:** Hiển thị trạng thái LED xanh trực quan (đồng bộ 3 giây/lần).

**Auto-refresh:** `pollData()` mỗi 3 giây, fetch `/api/stats` và `/api/detections`.

---

### 4. `main.cpp` — ESP32 Firmware

**Chức năng:** Kết nối WiFi, poll server, điều khiển cơ học (stepper + servo), quản lý LED và pin.

#### Sơ đồ chân (Pin Map)

| Component | GPIO | Ghi chú |
|-----------|------|---------|
| Servo nắp (MG996R) | 4 | PWM 500–2400 µs |
| Stepper Step (DRV8825) | 25 | Xung bước |
| Stepper Dir | 26 | Hướng quay |
| Stepper Enable | 27 | Active LOW |
| Ultrasonic Trig | 5 | |
| Ultrasonic Echo | 18 | |
| LED Đỏ (cảnh báo) | 2 | Bật khi AI không nhận diện |
| LED Xanh (remote) | 12 | Điều khiển từ Dashboard |
| Battery ADC | 34 | Qua voltage divider |
| LCD SDA | 21 | I2C |
| LCD SCL | 22 | I2C |

#### Servo Timing (MG996R Standard 180°)

| Hành động | Microseconds |
|-----------|-------------|
| Dừng (neutral) | 1500 µs |
| Mở nắp (CW) | 1700 µs |
| Đóng nắp (CCW) | 1300 µs |
| Thời gian quay 90° | 900 ms |

#### Luồng hoạt động chính

```
SETUP:
  → Khởi tạo LCD, stepper, servo, LED, ADC
  → Kết nối WiFi
  → Hiển thị IP trên LCD

LOOP:
  ├── Mỗi 30 giây: đọc pin ADC → POST /battery
  ├── Mỗi 2 giây:  GET /led_status → bật/tắt LED GPIO 12
  └── Chờ AI phát hiện rác (GET /result mỗi 2 giây):
        ├── class_id = 0 (chưa có)      → tiếp tục chờ
        ├── class_id = -1 (không detect) → LED đỏ sáng 3 giây → chờ tiếp
        └── class_id = 1/2/3            → chạy chu kỳ phân loại:
              Phase 1: Stepper FORWARD đến đúng khoảng cách (7/19/32 cm)
              Phase 2: Servo mở nắp 90° (900 ms)
              Phase 3: Giữ nắp mở 2 giây (rác rơi xuống)
              Phase 4: Servo đóng nắp về 0° (900 ms)
              Phase 5: Stepper BACKWARD về vị trí gốc (≤2 cm)
```

#### LCD 1602 — Trạng thái hiển thị

| Trạng thái | Dòng 1 | Dòng 2 |
|-----------|--------|--------|
| Khởi động | `Smart Bin` | `Starting...` |
| WiFi OK | `WiFi Connected!` | `<IP address>` |
| WiFi lỗi | `WiFi ERROR!` | `Check SSID/PW` |
| Chờ detect | `Waiting for` | `AI detection...` |
| Không detect được | `Not Detected!` | `!! Warning !!` |
| Phát hiện rác | `Detected:` | `Organic/Recyclable/Hazardous` |
| Đang di chuyển | `Moving...` | `<tên loại rác>` |
| Đang mở nắp | `Opening lid...` | `<tên loại rác>` |
| Đang đổ rác | `Dropping...` | `<tên loại rác>` |
| Đang đóng nắp | `Closing lid...` | `<tên loại rác>` |
| Đang về nhà | `Returning...` | _(trống)_ |
| Hoàn thành | `Done! Bin:` | `<tên loại rác>` |

#### Sơ đồ voltage divider (Pin 3S 18650 → ADC ESP32)

```
3S 18650 (+) ─── 30kΩ ───┬─── 10kΩ ─── GND
                          │
                    ADC (GPIO 34)

Vbat 9.0V–12.6V → Vadc ~2.25V–3.15V  (an toàn cho ESP32 max 3.3V)
```

Tỉ lệ đọc: `Vbat = (analogRead / 4095.0) × 3.3 × 4.0`

---

### 5. `platformio.ini` — PlatformIO Config

```ini
[env:featheresp32]
platform = espressif32
board = featheresp32
framework = arduino
lib_deps =
    ESP32Servo
    bblanchon/ArduinoJson@^7.0.0
    arduino-libraries/LiquidCrystal@^1.0.7
upload_port = COM6
monitor_port = COM6
monitor_speed = 115200
```

---

## Hướng dẫn triển khai

### Bước 1: Cài Python dependencies

```bash
cd yolo_server
pip install flask ultralytics opencv-python pyOpenSSL
```

### Bước 2: Chạy Flask server

```bash
python server.py
```

Đầu ra terminal sẽ hiển thị IP máy tính, ví dụ:

```
 IP     : 192.168.1.100
 ESP32  : http://192.168.1.100:5001/result
 LED    : http://192.168.1.100:5001/led_status
 Dashboard: https://192.168.1.100:5000/dashboard
```

### Bước 3: Cấu hình ESP32

Sửa 4 dòng đầu trong `src/main.cpp`:

```cpp
const char* WIFI_SSID   = "Ten_WiFi";
const char* WIFI_PASS   = "Mat_khau";
const char* SERVER_URL  = "http://<IP_PC>:5001/result";
const char* BATTERY_URL = "http://<IP_PC>:5001/battery";
const char* LED_URL     = "http://<IP_PC>:5001/led_status";
```

> **Lưu ý:** ESP32 dùng HTTP (port 5001), không cần HTTPS.

### Bước 4: Upload firmware ESP32

```bash
pio run -t upload
pio device monitor
```

### Bước 5: Mở Web App trên điện thoại

1. Truy cập `https://<IP_PC>:5000` trên browser điện thoại
2. Chấp nhận cảnh báo SSL (self-signed certificate)
3. Cho phép quyền truy cập camera
4. Chụp ảnh hoặc bật chế độ Auto (3 giây/lần)

### Bước 6: Mở Dashboard

Truy cập `https://<IP_PC>:5000/dashboard` trên máy tính hoặc tablet.

Mật khẩu mặc định để mở bin: `123456`

---

## Các vấn đề đã biết & lưu ý

**Camera web app yêu cầu HTTPS:** Trình duyệt hiện đại chặn `getUserMedia()` trên HTTP. Dùng `pyOpenSSL` để server tự tạo cert, hoặc tunnel qua ngrok:

```bash
ngrok http 5000
```

**Timestamp 64-bit:** Python tạo timestamp dạng `int(time.time()*1000)` vượt quá 32-bit. ESP32 dùng `long long` (64-bit) để parse đúng — không được đổi sang `unsigned long`.

**Motor và servo không chạy cùng lúc:** Code thêm `delay(500)` sau `stepperStop()` để từ trường DRV8825 tan trước khi servo nhận lệnh, tránh nhiễu điện.

**Flash torch chỉ hoạt động trên:** Android Chrome + camera sau (environment). iOS Safari không hỗ trợ torch API.

**CSS zoom vs Hardware zoom:** Nếu thiết bị không hỗ trợ hardware zoom qua `track.getCapabilities()`, web app tự fallback sang CSS `scale()`. Ảnh gửi lên server khi CSS zoom được crop đúng để AI nhận diện chính xác vùng được zoom.

---
