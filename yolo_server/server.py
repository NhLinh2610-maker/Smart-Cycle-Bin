"""
YOLO Waste Detection Server
- Nhận ảnh từ phone camera qua web app
- Chạy YOLO model best.pt để nhận diện rác
- Gửi kết quả (loại rác) cho ESP32 qua HTTP
- Cung cấp API cho Dashboard (D:/Dashboard)

3 loại rác: Organic Waste, Recyclable Waste, Hazardous Waste
Map sang ID: 1=Organic, 2=Recyclable, 3=Hazardous
"""

from flask import Flask, request, jsonify, send_from_directory
from ultralytics import YOLO
import os
import time
from collections import deque

app = Flask(__name__)

# Load YOLO model
MODEL_PATH = r"D:\YOLO\IOT\Src\best.pt"
print(f"Đang load model: {MODEL_PATH}")
model = YOLO(MODEL_PATH)
print("Model loaded thành công!")

# Map class name -> ID cho ESP32 (3 loai rac)
CLASS_TO_ID = {
    "Organic Waste": 1,
    "Recyclable Waste": 2,
    "Hazardous Waste": 3,
}

ID_TO_CLASS = {v: k for k, v in CLASS_TO_ID.items()}

# Lưu kết quả phát hiện gần nhất
last_detection = {
    "class_id": 0,
    "class_name": "Unknown",
    "confidence": 0.0,
    "timestamp": 0
}

# Lịch sử nhận diện (cho Dashboard)
detection_history = deque(maxlen=500)

# Dữ liệu pin gần nhất (để Web app hiển thị dashboard)
battery_state = {
    "voltage": 12.0,
    "percent": 50,
    "timestamp": 0
}

# Thư mục lưu ảnh upload
UPLOAD_FOLDER = os.path.join(os.path.dirname(__file__), "uploads")
os.makedirs(UPLOAD_FOLDER, exist_ok=True)


@app.route("/")
def index():
    """Trả về web app cho phone camera"""
    return send_from_directory(os.path.dirname(__file__), "index.html")


@app.route("/detect", methods=["POST"])
def detect():
    """Nhận ảnh từ phone, chạy YOLO, trả về kết quả"""
    global last_detection

    if "image" not in request.files:
        return jsonify({"error": "No image provided"}), 400

    file = request.files["image"]
    if file.filename == "":
        return jsonify({"error": "No image selected"}), 400

    # Lưu ảnh tạm
    timestamp = int(time.time() * 1000)
    img_path = os.path.join(UPLOAD_FOLDER, f"capture_{timestamp}.jpg")
    file.save(img_path)
    print(f"Nhận ảnh: {img_path}")

    # Chạy YOLO detection
    try:
        results = model(img_path, conf=0.4)

        detected_class = "Unknown"
        detected_id = 0
        max_confidence = 0.0

        if results and len(results) > 0:
            result = results[0]
            if result.boxes is not None and len(result.boxes) > 0:
                # Lấy detection có confidence cao nhất
                boxes = result.boxes
                best_idx = boxes.conf.argmax().item()
                class_id = int(boxes.cls[best_idx].item())
                confidence = float(boxes.conf[best_idx].item())
                class_name = model.names[class_id]

                detected_class = class_name
                detected_id = CLASS_TO_ID.get(class_name, 0)
                max_confidence = confidence

                print(f"Phát hiện: {class_name} (ID={detected_id}), confidence={confidence:.2f}")
            else:
                print("Không phát hiện rác trong ảnh")
        else:
            print("Không phát hiện rác trong ảnh")

        # Cập nhật kết quả cuối cùng
        last_detection = {
            "class_id": detected_id,
            "class_name": detected_class,
            "confidence": max_confidence,
            "timestamp": timestamp
        }

        # Lưu lịch sử detection (cho Dashboard)
        detection_history.append(last_detection.copy())

        return jsonify({
            "success": True,
            "class_id": detected_id,
            "class_name": detected_class,
            "confidence": max_confidence
        })

    except Exception as e:
        print(f"Lỗi detection: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/result", methods=["GET"])
def get_result():
    """ESP32 gọi API này để lấy kết quả nhận diện gần nhất"""
    return jsonify(last_detection)


@app.route("/status", methods=["GET"])
def status():
    """Check server status"""
    return jsonify({
        "status": "running",
        "model": MODEL_PATH,
        "last_detection": last_detection
    })


# ========== API Dành cho Dashboard ==========

@app.route("/api/detections", methods=["GET"])
def api_detections():
    """API cho Dashboard: lấy lịch sử detections"""
    limit = request.args.get("limit", default=100, type=int)
    items = list(detection_history)[-limit:]
    return jsonify({
        "detections": items,
        "total": len(items),
        "last": items[-1] if items else None
    })


@app.route("/api/stats", methods=["GET"])
def api_stats():
    """API cho Dashboard: thống kê tổng quan"""
    counts = {
        "organic": 0,
        "recyclable": 0,
        "hazardous": 0,
        "unknown": 0
    }
    for d in detection_history:
        cid = d.get("class_id", 0)
        if cid == 1:
            counts["organic"] += 1
        elif cid == 2:
            counts["recyclable"] += 1
        elif cid == 3:
            counts["hazardous"] += 1
        else:
            counts["unknown"] += 1

    return jsonify({
        "total_detections": len(detection_history),
        "counts": counts,
        "battery": battery_state,
        "last_detection": last_detection
    })


@app.route("/dashboard")
def dashboard():
    """Trả về Dashboard HTML"""
    return send_from_directory(os.path.dirname(__file__), "dashboard.html")


@app.route("/battery", methods=["POST", "GET"])
def battery():
    """POST: ESP32 gửi dữ liệu pin | GET: Web app lấy dữ liệu pin"""
    global battery_state
    if request.method == "POST":
        data = request.get_json(silent=True) or {}
        battery_state["voltage"] = data.get("voltage", 0.0)
        battery_state["percent"] = data.get("percent", 0)
        battery_state["timestamp"] = int(time.time() * 1000)
        print(f"[BATTERY] Nhận: {battery_state['voltage']}V, {battery_state['percent']}%")
        return jsonify({"success": True})
    return jsonify(battery_state)


# ========== CORS cho Dashboard chạy ở file:/// hoặc domain khác ==========
@app.after_request
def after_request(response):
    response.headers.set('Access-Control-Allow-Origin', '*')
    response.headers.set('Access-Control-Allow-Headers', 'Content-Type,Authorization')
    response.headers.set('Access-Control-Allow-Methods', 'GET,PUT,POST,DELETE,OPTIONS')
    return response


if __name__ == "__main__":
    import socket

    # Lấy IP của máy
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        LOCAL_IP = s.getsockname()[0]
    except Exception:
        LOCAL_IP = "127.0.0.1"
    finally:
        s.close()

    print("=" * 50)
    print(" YOLO Waste Detection Server")
    print("=" * 50)
    print(f" Model: {MODEL_PATH}")
    print(f" Classes: {CLASS_TO_ID}")
    print(f" IP: {LOCAL_IP}")
    print(f" 📱 Web App (HTTPS): https://{LOCAL_IP}:5000/")
    print(f" 🔌 ESP32 API (HTTP): http://{LOCAL_IP}:5001/result")
    print(f" 📊 Dashboard: https://{LOCAL_IP}:5000/dashboard")
    print(f" 📡 API Stats: https://{LOCAL_IP}:5000/api/stats")
    print("=" * 50)

    # Chạy HTTP server cho ESP32 trên port 5001 (không SSL, tránh lỗi)
    import threading

    def run_http():
        print(" ✅ HTTP server cho ESP32: port 5001")
        app.run(host="0.0.0.0", port=5001, debug=False, use_reloader=False)

    http_thread = threading.Thread(target=run_http, daemon=True)
    http_thread.start()

    # Chạy HTTPS server cho Phone Camera trên port 5000
    try:
        from OpenSSL import crypto
        print(" ✅ pyOpenSSL found, khởi động HTTPS trên port 5000...")
        app.run(host="0.0.0.0", port=5000, debug=False, ssl_context="adhoc", use_reloader=False)
    except (ImportError, Exception) as e:
        print(f" ⚠️ Không chạy được HTTPS: {e}")
        print(" → Chạy HTTP trên port 5000 (camera phone có thể bị lỗi!)")
        print(" → Cài đặt: pip install pyOpenSSL")
        app.run(host="0.0.0.0", port=5000, debug=False, use_reloader=False)
