"""
YOLO Waste Detection Server
- Nhận ảnh từ phone camera qua web app
- Chạy YOLO model best.pt để nhận diện rác
- Gửi kết quả (loại rác) cho ESP32 qua HTTP
- Cung cấp API cho Dashboard (D:/Dashboard)

3 loại rác: Organic Waste, Recyclable Waste, Hazardous Waste
Map sang ID: 1=Organic, 2=Recyclable, 3=Hazardous

YOLO Waste Detection Server – cập nhật thêm endpoint /led và /led_status
Thêm so với bản cũ:
  POST /led          : Dashboard gọi để bật/tắt LED xanh {"state":"on"/"off"}
  GET  /led_status   : ESP32 poll mỗi 2s để biết cần bật/tắt LED GPIO
"""

from flask import Flask, request, jsonify, send_from_directory
from ultralytics import YOLO
import os
import time
from collections import deque

app = Flask(__name__)

MODEL_PATH = r"D:\YOLO\IOT\Src\best.pt"
print(f"Đang load model: {MODEL_PATH}")
model = YOLO(MODEL_PATH)
print("Model loaded thành công!")

CLASS_TO_ID = {
    "Organic Waste": 1,
    "Recyclable Waste": 2,
    "Hazardous Waste": 3,
}

last_detection = {
    "class_id": 0,
    "class_name": "Unknown",
    "confidence": 0.0,
    "timestamp": 0
}

detection_history = deque(maxlen=500)

battery_state = {
    "voltage": 12.0,
    "percent": 50,
    "timestamp": 0
}

# Trạng thái LED xanh – Dashboard ghi, ESP32 đọc
led_state = {
    "state": "off"   # "on" hoặc "off"
}

UPLOAD_FOLDER = os.path.join(os.path.dirname(__file__), "uploads")
os.makedirs(UPLOAD_FOLDER, exist_ok=True)


@app.route("/")
def index():
    return send_from_directory(os.path.dirname(__file__), "index.html")


@app.route("/detect", methods=["POST"])
def detect():
    global last_detection
    if "image" not in request.files:
        return jsonify({"error": "No image provided"}), 400
    file = request.files["image"]
    if file.filename == "":
        return jsonify({"error": "No image selected"}), 400

    timestamp = int(time.time() * 1000)
    img_path = os.path.join(UPLOAD_FOLDER, f"capture_{timestamp}.jpg")
    file.save(img_path)
    print(f"Nhận ảnh: {img_path}")

    try:
        results = model(img_path, conf=0.4)
        detected_class = "Unknown"
        detected_id = 0
        max_confidence = 0.0

        if results and len(results) > 0:
            result = results[0]
            if result.boxes is not None and len(result.boxes) > 0:
                boxes = result.boxes
                best_idx = boxes.conf.argmax().item()
                class_id = int(boxes.cls[best_idx].item())
                confidence = float(boxes.conf[best_idx].item())
                class_name = model.names[class_id]
                detected_class = class_name
                detected_id = CLASS_TO_ID.get(class_name, 0)
                max_confidence = confidence
                print(f"Phát hiện: {class_name} (ID={detected_id}), conf={confidence:.2f}")
            else:
                print("Không phát hiện rác")

        last_detection = {
            "class_id": detected_id,
            "class_name": detected_class,
            "confidence": max_confidence,
            "timestamp": timestamp
        }
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
    """ESP32 poll kết quả AI"""
    return jsonify(last_detection)


@app.route("/status", methods=["GET"])
def status():
    return jsonify({
        "status": "running",
        "model": MODEL_PATH,
        "last_detection": last_detection
    })


# ============================================================================
# API DASHBOARD
# ============================================================================

@app.route("/api/detections", methods=["GET"])
def api_detections():
    limit = request.args.get("limit", default=100, type=int)
    items = list(detection_history)[-limit:]
    return jsonify({
        "detections": items,
        "total": len(items),
        "last": items[-1] if items else None
    })


@app.route("/api/stats", methods=["GET"])
def api_stats():
    counts = {"organic": 0, "recyclable": 0, "hazardous": 0, "unknown": 0}
    for d in detection_history:
        cid = d.get("class_id", 0)
        if cid == 1: counts["organic"] += 1
        elif cid == 2: counts["recyclable"] += 1
        elif cid == 3: counts["hazardous"] += 1
        else: counts["unknown"] += 1

    return jsonify({
        "total_detections": len(detection_history),
        "counts": counts,
        "battery": battery_state,
        "last_detection": last_detection
    })


@app.route("/dashboard")
def dashboard():
    return send_from_directory(os.path.dirname(__file__), "dashboard.html")


@app.route("/battery", methods=["POST", "GET"])
def battery():
    global battery_state
    if request.method == "POST":
        data = request.get_json(silent=True) or {}
        battery_state["voltage"] = data.get("voltage", 0.0)
        battery_state["percent"] = data.get("percent", 0)
        battery_state["timestamp"] = int(time.time() * 1000)
        print(f"[BATTERY] {battery_state['voltage']}V, {battery_state['percent']}%")
        return jsonify({"success": True})
    return jsonify(battery_state)


# ============================================================================
# LED ENDPOINTS (MỚI)
# ============================================================================

@app.route("/led", methods=["POST"])
def led_control():
    """
    Dashboard gọi POST /led để bật/tắt LED xanh trên ESP32.
    Body JSON: {"state": "on"} hoặc {"state": "off"}
    """
    global led_state
    data = request.get_json(silent=True) or {}
    state = data.get("state", "off")
    if state not in ("on", "off"):
        return jsonify({"error": "state phải là 'on' hoặc 'off'"}), 400
    led_state["state"] = state
    print(f"[LED] Trạng thái mới: {state}")
    return jsonify({"success": True, "state": state})


@app.route("/led_status", methods=["GET"])
def led_status():
    """
    ESP32 poll GET /led_status mỗi 2 giây để biết cần bật/tắt LED GPIO.
    Trả về: {"state": "on"} hoặc {"state": "off"}
    """
    return jsonify(led_state)


# ============================================================================
# CORS
# ============================================================================
@app.after_request
def after_request(response):
    response.headers.set('Access-Control-Allow-Origin', '*')
    response.headers.set('Access-Control-Allow-Headers', 'Content-Type,Authorization')
    response.headers.set('Access-Control-Allow-Methods', 'GET,PUT,POST,DELETE,OPTIONS')
    return response


if __name__ == "__main__":
    import socket, threading

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
    print(f" Model  : {MODEL_PATH}")
    print(f" IP     : {LOCAL_IP}")
    print(f" ESP32  : http://{LOCAL_IP}:5001/result")
    print(f" LED    : http://{LOCAL_IP}:5001/led_status  (ESP32 poll)")
    print(f" Dashboard: https://{LOCAL_IP}:5000/dashboard")
    print("=" * 50)

    def run_http():
        print(" HTTP server cho ESP32: port 5001")
        app.run(host="0.0.0.0", port=5001, debug=False, use_reloader=False)

    http_thread = threading.Thread(target=run_http, daemon=True)
    http_thread.start()

    try:
        from OpenSSL import crypto
        print(" HTTPS trên port 5000...")
        app.run(host="0.0.0.0", port=5000, debug=False, ssl_context="adhoc", use_reloader=False)
    except (ImportError, Exception) as e:
        print(f" Không chạy được HTTPS: {e}")
        app.run(host="0.0.0.0", port=5000, debug=False, use_reloader=False)
