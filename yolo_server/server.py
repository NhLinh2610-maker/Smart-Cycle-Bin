"""
YOLO Waste Detection Server
- Nhận ảnh từ phone camera qua web app
- Chạy YOLO model best.pt để nhận diện rác
- Gửi kết quả (loại rác) cho ESP32 qua HTTP

4 loại rác: Organic Waste, Recyclable Waste, Inorganic Waste, Hazardous Waste
Map sang ID: 1=Organic, 2=Recyclable, 3=Inorganic, 4=Hazardous
"""

from flask import Flask, request, jsonify, send_from_directory
from ultralytics import YOLO
import os
import time

app = Flask(__name__)

# Load YOLO model
MODEL_PATH = r"D:\YOLO\IOT\Src\best.pt"
print(f"Đang load model: {MODEL_PATH}")
model = YOLO(MODEL_PATH)
print("Model loaded thành công!")

# Map class name -> ID cho ESP32
CLASS_TO_ID = {
    "Organic Waste": 1,
    "Recyclable Waste": 2,
    "Inorganic Waste": 3,
    "Hazardous Waste": 4,
}

# Lưu kết quả phát hiện gần nhất
last_detection = {
    "class_id": 0,
    "class_name": "Unknown",
    "confidence": 0.0,
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

if __name__ == "__main__":
    print("=" * 50)
    print("  YOLO Waste Detection Server")
    print("=" * 50)
    print(f"  Model: {MODEL_PATH}")
    print(f"  Classes: {CLASS_TO_ID}")
    print(f"  Web App: http://<IP>:5000/")
    print(f"  ESP32 API: http://<IP>:5000/result")
    print("=" * 50)
    
    # Chạy server trên tất cả interface, port 5000
    # Sử dụng SSL tự động (cần pyOpenSSL: pip install pyOpenSSL)
    # Camera phone yêu cầu HTTPS mới hoạt động
    try:
        import ssl
        print("\n  Đang khởi động HTTPS...")
        app.run(host="0.0.0.0", port=5000, debug=False, ssl_context="adhoc")
    except ImportError:
        print("\n  ⚠️ Không có pyOpenSSL, chạy HTTP (camera phone sẽ bị lỗi!)")
        print("  → Cài đặt: pip install pyOpenSSL")
        print("  → Hoặc dùng ngrok: ngrok http 5000")
        app.run(host="0.0.0.0", port=5000, debug=False)
