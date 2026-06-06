"""
SafeVault — Recognition Engine (DeepFace Edition)
================================
AI module: DeepFace (FaceNet) + Liveness Detection
Import trong Flask app.py thay vì chạy script riêng.

Tác giả: SafeVault IoT
"""

import base64
import json
import os
import threading
import time

import cv2
import numpy as np
from deepface import DeepFace

# ──────────────────────────────────────────
#  CONFIG
# ──────────────────────────────────────────
DB_FILE              = os.path.join(os.path.dirname(__file__), "db.json")
DB_WATCH_INTERVAL    = 10      # Giây giữa 2 lần kiểm tra db.json thay đổi

# Ngưỡng nhận diện cho FaceNet (Cosine Distance)
# Dưới 0.40 được xem là cùng một người.
COSINE_THRESHOLD     = 0.40

# Hạ xuống 20 — ảnh JPEG từ điện thoại nén qua base64 thường blur_score chỉ 25-50
BLUR_THRESHOLD          = 20.0

# ──────────────────────────────────────────
#  STATE (thread-safe)
# ──────────────────────────────────────────
# Dạng: {"Kien": [emb1, emb2, ...], "An": [emb1, ...]}
known_embeddings = {}
last_db_mtime    = 0.0
is_trained       = False
_lock            = threading.Lock()

# ──────────────────────────────────────────
#  HELPER: Decode base64 → OpenCV image
# ──────────────────────────────────────────
def _decode_image(b64_str: str):
    """Giải mã chuỗi base64 thành ảnh OpenCV. Trả về None nếu lỗi."""
    try:
        if "," in b64_str:
            b64_str = b64_str.split(",")[-1]
        img_data = base64.b64decode(b64_str)
        nparr    = np.frombuffer(img_data, np.uint8)
        img      = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
        return img
    except Exception:
        return None

# ──────────────────────────────────────────
#  LIVENESS DETECTION (Anti-Spoofing)
# ──────────────────────────────────────────
def check_liveness(b64_str: str) -> tuple:
    """
    Phát hiện mặt thật vs ảnh giả bằng phân tích độ nét (Laplacian Variance).
    Returns: (is_real: bool, blur_score: float)
    """
    img = _decode_image(b64_str)
    if img is None:
        return False, 0.0

    # Sử dụng Haar Cascade để tìm ROI kiểm tra độ mờ cho nhẹ
    face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    detected = face_cascade.detectMultiScale(gray, scaleFactor=1.05, minNeighbors=3, minSize=(40, 40))
    
    if len(detected) == 0:
        return False, 0.0
    
    (x, y, w, h) = max(detected, key=lambda r: r[2] * r[3])
    face = gray[y : y + h, x : x + w]

    blur_score = cv2.Laplacian(face, cv2.CV_64F).var()
    is_real    = blur_score >= BLUR_THRESHOLD
    return is_real, round(blur_score, 1)

# ──────────────────────────────────────────
#  COSINE DISTANCE
# ──────────────────────────────────────────
def find_cosine_distance(source_representation, test_representation):
    a = np.matmul(np.transpose(source_representation), test_representation)
    b = np.sum(np.multiply(source_representation, source_representation))
    c = np.sum(np.multiply(test_representation, test_representation))
    return 1 - (a / (np.sqrt(b) * np.sqrt(c)))

# ──────────────────────────────────────────
#  LOAD DATABASE & EXTRACT EMBEDDINGS
# ──────────────────────────────────────────
def load_database() -> bool:
    """
    Đọc db.json, trích xuất embedding bằng FaceNet và lưu vào bộ nhớ.
    Chỉ chạy lại nếu db.json thay đổi (kiểm tra mtime).
    """
    global known_embeddings, last_db_mtime, is_trained

    if not os.path.exists(DB_FILE):
        print("[AI] Không tìm thấy db.json!")
        return False

    try:
        current_mtime = os.path.getmtime(DB_FILE)
        if current_mtime <= last_db_mtime:
            return is_trained  # Chưa thay đổi

        print("[AI] Đang cập nhật cơ sở dữ liệu (DeepFace)...")
        with open(DB_FILE, "r", encoding="utf-8") as f:
            raw_db = json.load(f)

        new_embeddings = {}
        person_count = 0
        total_samples = 0

        for name, info in raw_db.items():
            if not isinstance(info, dict): continue
            
            image_list = info.get("face_images", [])
            if not image_list:
                single = info.get("full_image", "")
                if single: image_list = [single]
            if not image_list: continue

            user_embs = []
            for b64 in image_list:
                if not b64: continue
                img = _decode_image(b64)
                if img is None: continue
                
                try:
                    # Trích xuất embedding (vector 512D)
                    # enforce_detection=False để không crash nếu không thấy mặt rõ
                    objs = DeepFace.represent(img_path=img, model_name="Facenet", detector_backend="opencv", enforce_detection=False)
                    if len(objs) > 0:
                        user_embs.append(objs[0]["embedding"])
                        total_samples += 1
                except Exception as e:
                    pass
            
            if user_embs:
                new_embeddings[name] = user_embs
                person_count += 1
                print(f"[AI] Đã học: {name} ({len(user_embs)} mẫu FaceNet)")

        with _lock:
            known_embeddings = new_embeddings
            is_trained       = person_count > 0
            last_db_mtime    = current_mtime
        
        print(f"[AI] ✅ Sẵn sàng: {person_count} thành viên, {total_samples} mẫu FaceNet")
        return is_trained

    except Exception as e:
        print(f"[AI] Lỗi load database: {e}")
        return False

# ──────────────────────────────────────────
#  NHẬN DẠNG KHUÔN MẶT CHÍNH
# ──────────────────────────────────────────
def identify_face_from_b64(b64_str: str) -> dict:
    """
    Nhận dạng khuôn mặt từ ảnh base64 (gửi từ điện thoại).
    """
    # ── Bước 1: Liveness check ──
    is_real, blur_score = check_liveness(b64_str)
    print(f"[AI] Liveness: is_real={is_real}, blur_score={blur_score:.1f} (threshold={BLUR_THRESHOLD})")

    if not is_real:
        return {
            "status":     "denied",
            "name":       "Unknown",
            "confidence": 0.0,
            "is_real":    False,
            "blur_score": blur_score,
            "reason":     f"Ảnh không hợp lệ hoặc có thể là ảnh giả (độ nét={blur_score:.1f})"
        }

    # ── Bước 2: Kiểm tra database ──
    load_database()

    if not is_trained or not known_embeddings:
        return {
            "status":     "denied",
            "name":       "Unknown",
            "confidence": 0.0,
            "is_real":    True,
            "blur_score": blur_score,
            "reason":     "Chưa có thành viên nào được đăng ký"
        }

    # ── Bước 3: Trích xuất embedding của ảnh gửi lên ──
    img = _decode_image(b64_str)
    if img is None:
        return {
            "status": "denied", "name": "Unknown", "confidence": 0.0,
            "is_real": True, "blur_score": blur_score, "reason": "Ảnh không hợp lệ"
        }

    try:
        objs = DeepFace.represent(img_path=img, model_name="Facenet", detector_backend="opencv", enforce_detection=False)
        if len(objs) == 0:
            raise ValueError("Không tìm thấy khuôn mặt")
        target_emb = objs[0]["embedding"]
    except Exception as e:
        return {
            "status": "denied", "name": "Unknown", "confidence": 0.0,
            "is_real": True, "blur_score": blur_score, "reason": "Không phát hiện khuôn mặt trong ảnh"
        }

    # ── Bước 4: So sánh Cosine Distance ──
    best_match_name = "Unknown"
    min_distance = 1.0 # Giá trị càng nhỏ càng giống

    with _lock:
        for name, embs in known_embeddings.items():
            for emb in embs:
                dist = find_cosine_distance(target_emb, emb)
                if dist < min_distance:
                    min_distance = dist
                    best_match_name = name

    print(f"[AI] FaceNet: dist={min_distance:.3f}, best_match={best_match_name} | threshold={COSINE_THRESHOLD}")

    if min_distance <= COSINE_THRESHOLD:
        # ✅ GRANTED
        # Tính confidence: 0.40 -> 70%, 0.0 -> 100%
        # Công thức quy đổi: Confidence = 100 - (dist / 0.40) * 30
        confidence = round(max(70.0, 100.0 - (min_distance / COSINE_THRESHOLD) * 30.0), 1)
        print(f"[AI] ✅ GRANTED: {best_match_name}, dist={min_distance:.3f}")
        return {
            "status":     "granted",
            "name":       best_match_name,
            "confidence": confidence,
            "is_real":    True,
            "blur_score": blur_score,
            "reason":     f"Nhận dạng thành công: {best_match_name} ({confidence}%)"
        }
    else:
        # ❌ DENIED
        print(f"[AI] ❌ DENIED: {best_match_name}, dist={min_distance:.3f} > {COSINE_THRESHOLD}")
        return {
            "status":     "denied",
            "name":       best_match_name,
            "confidence": 0.0,
            "is_real":    True,
            "blur_score": blur_score,
            "reason":     f"Nhận dạng chưa đủ tin cậy — vui lòng chụp lại rõ hơn (sai lệch {min_distance:.2f})"
        }

# ──────────────────────────────────────────
#  BACKGROUND WATCHER — Tự động retrain
# ──────────────────────────────────────────
def start_db_watcher():
    """
    Khởi động thread nền: theo dõi db.json và retrain khi có thay đổi.
    """
    def _watch():
        load_database()  # Load ngay khi start
        while True:
            time.sleep(DB_WATCH_INTERVAL)
            load_database()

    t = threading.Thread(target=_watch, daemon=True, name="AI-DBWatcher")
    t.start()
    print("[AI] 🔍 Database watcher đã khởi động (interval={}s)".format(DB_WATCH_INTERVAL))

