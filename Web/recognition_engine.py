"""
SafeVault — Recognition Engine
================================
AI module: LBPH Face Recognition + Liveness Detection (Anti-Spoofing)
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

# ──────────────────────────────────────────
#  CONFIG
# ──────────────────────────────────────────
DB_FILE              = os.path.join(os.path.dirname(__file__), "db.json")
FACE_SIZE            = (200, 200)
DB_WATCH_INTERVAL    = 10      # Giây giữa 2 lần kiểm tra db.json thay đổi

# ── Ngưỡng nhận diện (dual-threshold security) ──────────────────────────────
# LBPH distance: 0 = perfect match, cao hơn = khác nhau hơn
#
# CONFIDENCE_THRESHOLD (78): Ngưỡng phát hiện — dist < 78 thì "có thể là người này"
# STRICT_ACCEPT_THRESHOLD (62): Ngưỡng chấp nhận — dist < 62 thì mới GRANT
#   → Vùng 62-78: phát hiện nhưng từ chối (yêu cầu chụp lại)
#   → Dưới 62: chắc chắn đủ để grant
#
# Tại sao không chỉ dùng 1 ngưỡng?
#   Nếu threshold=78: nhạy nhưng dễ nhầm người khác (FAR cao)
#   Nếu threshold=55: không nhầm nhưng thường xuyên từ chối người thật (FRR cao)
#   Dual-threshold: nhận diện rộng, nhưng grant chặt → cân bằng tốt nhất
CONFIDENCE_THRESHOLD    = 78   # Phát hiện: nếu dist < giá trị này → tiếp tục xử lý
STRICT_ACCEPT_THRESHOLD = 50   # Chấp nhận: chỉ grant nếu dist < giá trị này (đòi hỏi độ tin cậy ~75% trở lên)

# Hạ xuống 20 — ảnh JPEG từ điện thoại nén qua base64 thường blur_score chỉ 25-50
BLUR_THRESHOLD          = 20.0

# ──────────────────────────────────────────
#  HAAR CASCADE + LBPH
# ──────────────────────────────────────────
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)
recognizer = cv2.face.LBPHFaceRecognizer_create()

# ──────────────────────────────────────────
#  STATE (thread-safe)
# ──────────────────────────────────────────
label_map     = {}     # {0: "Nguyen Van An", 1: "Tran Thi Bich", ...}
last_db_mtime = 0.0
is_trained    = False
_lock         = threading.Lock()


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
#  HELPER: Phát hiện khuôn mặt lớn nhất
# ──────────────────────────────────────────
def _extract_face(img):
    """
    Phát hiện khuôn mặt lớn nhất trong ảnh.
    Trả về (face_roi_gray_resized, (x,y,w,h)) hoặc (None, None).
    
    Cải tiến:
    - scaleFactor=1.05 (mịn hơn, phát hiện nhiều góc hơn)
    - minNeighbors=3   (ít chặt hơn, giảm false-negative)
    - minSize=(40,40)  (chấp nhận mặt nhỏ hơn/xa camera hơn)
    - CLAHE trước khi detect (cân bằng sáng tốt hơn)
    """
    if img is None:
        return None, None
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    # CLAHE: tăng tương phản cục bộ, giúp nhận khuôn mặt trong điều kiện ánh sáng kém
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    gray  = clahe.apply(gray)
    detected = face_cascade.detectMultiScale(
        gray, scaleFactor=1.05, minNeighbors=3, minSize=(40, 40)
    )
    if len(detected) == 0:
        return None, None
    (x, y, w, h) = max(detected, key=lambda r: r[2] * r[3])
    face_roi     = gray[y : y + h, x : x + w]
    face_resized = cv2.resize(face_roi, FACE_SIZE)
    return face_resized, (x, y, w, h)


# ──────────────────────────────────────────
#  HELPER: Data augmentation khi train
# ──────────────────────────────────────────
def _augment_face(face):
    """Tạo biến thể từ 1 khuôn mặt để tăng độ chính xác LBPH.
    
    Mở rộng từ 6 → 12 biến thể:
    - Mô phỏng đa dạng ánh sáng, góc, chất lượng camera
    - Giúp model robust hơn khi nhận diện thực tế
    """
    flipped = cv2.flip(face, 1)
    clahe   = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    return [
        face,                                                        # gốc
        flipped,                                                     # lật ngang
        cv2.convertScaleAbs(face,    alpha=1.3, beta=30),           # rất sáng
        cv2.convertScaleAbs(face,    alpha=1.1, beta=10),           # sáng nhẹ
        cv2.convertScaleAbs(face,    alpha=0.8, beta=-20),          # tối hơn
        cv2.convertScaleAbs(face,    alpha=0.6, beta=-40),          # rất tối
        cv2.GaussianBlur(face,   (3, 3), 0),                        # mờ nhẹ (camera thấp)
        cv2.GaussianBlur(face,   (5, 5), 0),                        # mờ vừa
        cv2.equalizeHist(face),                                      # cân bằng histogram
        clahe.apply(face),                                           # CLAHE
        cv2.convertScaleAbs(flipped, alpha=1.2, beta=15),           # lật + sáng
        cv2.convertScaleAbs(flipped, alpha=0.8, beta=-15),          # lật + tối
    ]


# ──────────────────────────────────────────
#  LIVENESS DETECTION (Anti-Spoofing)
# ──────────────────────────────────────────
def check_liveness(b64_str: str) -> tuple:
    """
    Phát hiện mặt thật vs ảnh giả bằng phân tích độ nét (Laplacian Variance).
    
    Nguyên lý:
    - Mặt người thật chụp bằng camera điện thoại → ảnh sắc nét (variance cao)
    - Ảnh in ra giấy hoặc hiện trên màn hình → thường mờ hơn (variance thấp)
    
    Returns: (is_real: bool, blur_score: float)
    """
    img = _decode_image(b64_str)
    if img is None:
        return False, 0.0

    face, _ = _extract_face(img)
    if face is None:
        return False, 0.0

    blur_score = cv2.Laplacian(face, cv2.CV_64F).var()
    is_real    = blur_score >= BLUR_THRESHOLD
    return is_real, round(blur_score, 1)


# ──────────────────────────────────────────
#  LOAD DATABASE & TRAIN LBPH
# ──────────────────────────────────────────
def load_database() -> bool:
    """
    Đọc db.json, crop khuôn mặt từ ảnh đăng ký, train LBPH.
    Chỉ retrain nếu db.json thay đổi (kiểm tra mtime).
    Thread-safe.
    """
    global label_map, last_db_mtime, is_trained

    if not os.path.exists(DB_FILE):
        print("[AI] Không tìm thấy db.json!")
        return False

    try:
        current_mtime = os.path.getmtime(DB_FILE)
        if current_mtime <= last_db_mtime:
            return is_trained  # Chưa thay đổi

        print("[AI] Đang cập nhật cơ sở dữ liệu khuôn mặt...")
        with open(DB_FILE, "r", encoding="utf-8") as f:
            raw_db = json.load(f)

        faces, labels, new_label_map = [], [], {}
        label_id = 0

        for name, info in raw_db.items():
            if not isinstance(info, dict):
                continue
            # Ưu tiên face_images (nhiều ảnh), fallback full_image (1 ảnh)
            image_list = info.get("face_images", [])
            if not image_list:
                single = info.get("full_image", "")
                if single:
                    image_list = [single]
            if not image_list:
                continue

            person_count = 0
            for b64 in image_list:
                if not b64:
                    continue
                img      = _decode_image(b64)
                face, _  = _extract_face(img)
                if face is None:
                    continue
                for variant in _augment_face(face):
                    faces.append(variant)
                    labels.append(label_id)
                    person_count += 1

            if person_count > 0:
                new_label_map[label_id] = name
                label_id += 1
                print(f"[AI] Đã học: {name} ({person_count} mẫu)")
            else:
                print(f"[AI] Không có ảnh hợp lệ: {name}")

        if faces:
            with _lock:
                recognizer.train(faces, np.array(labels))
                label_map     = new_label_map
                is_trained    = True
                last_db_mtime = current_mtime
            print(f"[AI] ✅ Sẵn sàng: {len(label_map)} thành viên, {len(faces)} mẫu tổng")
            return True
        else:
            print("[AI] Không có khuôn mặt nào để học!")
            is_trained = False
            return False

    except Exception as e:
        print(f"[AI] Lỗi load database: {e}")
        return False


# ──────────────────────────────────────────
#  NHẬN DẠNG KHUÔN MẶT CHÍNH
# ──────────────────────────────────────────
def identify_face_from_b64(b64_str: str) -> dict:
    """
    Nhận dạng khuôn mặt từ ảnh base64 (gửi từ điện thoại).
    
    Quy trình 3 bước:
    1. Liveness check — loại ảnh giả
    2. Phát hiện khuôn mặt — tìm ROI
    3. LBPH predict — so sánh với database
    
    Returns:
        dict với các trường:
        - status:     "granted" | "denied"
        - name:       tên thành viên hoặc "Unknown"
        - confidence: độ tin cậy 0–100%
        - is_real:    True/False (liveness)
        - blur_score: điểm nét ảnh
        - reason:     mô tả kết quả
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
    load_database()  # Nạp lại nếu db.json thay đổi

    if not is_trained:
        return {
            "status":     "denied",
            "name":       "Unknown",
            "confidence": 0.0,
            "is_real":    True,
            "blur_score": blur_score,
            "reason":     "Chưa có thành viên nào được đăng ký"
        }

    # ── Bước 3: Phát hiện và nhận dạng khuôn mặt ──
    img = _decode_image(b64_str)
    if img is None:
        return {
            "status": "denied", "name": "Unknown", "confidence": 0.0,
            "is_real": True, "blur_score": blur_score, "reason": "Ảnh không hợp lệ"
        }

    face, box = _extract_face(img)
    if face is None:
        return {
            "status": "denied", "name": "Unknown", "confidence": 0.0,
            "is_real": True, "blur_score": blur_score, "reason": "Không phát hiện khuôn mặt trong ảnh"
        }

    try:
        with _lock:
            label, lbph_dist = recognizer.predict(face)

        print(f"[AI] LBPH: dist={lbph_dist:.1f}, label={label} "
              f"| detect<{CONFIDENCE_THRESHOLD}, accept<{STRICT_ACCEPT_THRESHOLD}")

        if label not in label_map:
            # Label không tồn tại trong map → bỏ qua
            pass

        elif lbph_dist < STRICT_ACCEPT_THRESHOLD:
            # ✅ Vùng CHẮC CHẮN: dist thấp, khó nhầm sang người khác
            name       = label_map[label]
            confidence = round(min(99.9, 100.0 - (lbph_dist / 2.0)), 1)
            print(f"[AI] ✅ GRANTED: {name}, dist={lbph_dist:.1f} < {STRICT_ACCEPT_THRESHOLD}")
            return {
                "status":     "granted",
                "name":       name,
                "confidence": confidence,
                "is_real":    True,
                "blur_score": blur_score,
                "reason":     f"Nhận dạng thành công: {name} ({confidence}%)"
            }

        elif lbph_dist < CONFIDENCE_THRESHOLD:
            # ⚠️ Vùng KHÔNG CHẮC: nhận dạng được nhưng confidence chưa đủ cao
            # → Từ chối nhưng gợi ý chụp lại (không phải người lạ hoàn toàn)
            name = label_map[label]
            print(f"[AI] ⚠️ UNCERTAIN: {name}, dist={lbph_dist:.1f} "
                  f"(cần < {STRICT_ACCEPT_THRESHOLD} để grant)")
            return {
                "status":     "denied",
                "name":       name,          # Trả tên để UI có thể gợi ý
                "confidence": 0.0,
                "is_real":    True,
                "blur_score": blur_score,
                "reason":     f"Nhận dạng chưa đủ tin cậy — vui lòng chụp lại rõ hơn (dist={lbph_dist:.0f})"
            }

    except Exception as e:
        print(f"[AI] Lỗi predict: {e}")

    return {
        "status":     "denied",
        "name":       "Unknown",
        "confidence": 0.0,
        "is_real":    True,
        "blur_score": blur_score,
        "reason":     "Không nhận ra khuôn mặt trong database"
    }


# ──────────────────────────────────────────
#  BACKGROUND WATCHER — Tự động retrain
# ──────────────────────────────────────────
def start_db_watcher():
    """
    Khởi động thread nền: theo dõi db.json và retrain LBPH khi có thay đổi.
    Gọi hàm này 1 lần khi khởi động Flask.
    """
    def _watch():
        load_database()  # Load ngay khi start
        while True:
            time.sleep(DB_WATCH_INTERVAL)
            load_database()

    t = threading.Thread(target=_watch, daemon=True, name="AI-DBWatcher")
    t.start()
    print("[AI] 🔍 Database watcher đã khởi động (interval={}s)".format(DB_WATCH_INTERVAL))
