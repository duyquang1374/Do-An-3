from flask import Flask, request, jsonify, send_from_directory, Response
import json, time, os, threading, queue, random
import paho.mqtt.client as mqtt_client
import recognition_engine  # ← AI module mới

app = Flask(__name__, static_folder='static')

DB_FILE  = os.path.join(os.path.dirname(__file__), "db.json")
LOG_FILE = os.path.join(os.path.dirname(__file__), "log.json")
FP_FILE  = os.path.join(os.path.dirname(__file__), "fingerprints.json")

pending_enroll = {}  # {finger_id: label} - lưu label tạm khi chờ đăng ký

# ──────────────────────────────────────────
#  CẤU HÌNH MQTT
# ──────────────────────────────────────────
MQTT_BROKER  = "broker.hivemq.com"
MQTT_PORT    = 1883
MQTT_PREFIX  = "smartlock/quang_1307"
TOPIC_COMMAND   = f"{MQTT_PREFIX}/command"
TOPIC_STATUS    = f"{MQTT_PREFIX}/status"
TOPIC_HEARTBEAT = f"{MQTT_PREFIX}/heartbeat"

esp_online     = False
mqtt_client_   = None
mqtt_connected = False

# ── Battery state (từ ESP32 ADC) ──
battery_percent  = -1      # -1 = chưa nhận được dữ liệu
battery_voltage  = 0.0
battery_updated  = 0       # timestamp lần cập nhật cuối

# ── Trạng thái xác thực Layer 2 ──
waiting_for_face  = False
face_fail_count   = 0
_face_timeout_timer = None

def _reset_face_wait():
    global waiting_for_face, face_fail_count
    if waiting_for_face:
        waiting_for_face = False
        face_fail_count  = 0
        print("[APP] Timeout 10s: Hủy chờ quét khuôn mặt.")
        mqtt_publish_command("close_vault", "Timeout")

# ── Vault State Machine ──
# Các trạng thái két mở/đóng đã được loại bỏ, hiện được xử lý tự động trên phần cứng.

# ── OTP (Mã dùng 1 lần) ──
current_otp   = None    # Mã OTP hiện tại
otp_expiry    = 0       # Thời điểm hết hạn (timestamp)
otp_creator   = None    # Người tạo OTP

# ──────────────────────────────────────────
#  MQTT CALLBACKS
# ──────────────────────────────────────────
def on_connect(client, userdata, flags, reason_code, properties):
    global mqtt_connected
    if reason_code == 0:
        mqtt_connected = True
        client.subscribe(TOPIC_STATUS)
        client.subscribe(TOPIC_HEARTBEAT)
        print("[MQTT] Kết nối thành công")
    else:
        mqtt_connected = False
        print(f"[MQTT] Kết nối thất bại, code={reason_code}")

def on_message(client, userdata, msg):
    global esp_online
    try:
        payload = json.loads(msg.payload.decode())
        topic   = msg.topic
        print(f"[MQTT] Nhận [{topic}]: {payload}")

        if topic == TOPIC_HEARTBEAT:
            esp_online = True
            # Parse battery data từ heartbeat
            bat_pct = payload.get("battery_pct", None)
            bat_v   = payload.get("battery_v", None)
            if bat_pct is not None:
                global battery_percent, battery_voltage, battery_updated
                battery_percent = int(bat_pct)
                battery_voltage = float(bat_v) if bat_v else 0.0
                battery_updated = time.time()
                sse_push("battery_update", {
                    "percent": battery_percent,
                    "voltage": round(battery_voltage, 2),
                    "time": time.strftime("%H:%M:%S")
                })

        elif topic == TOPIC_STATUS:
            event  = payload.get("event", "")
            trigger = payload.get("trigger", "esp")
            esp_online = True

            # Layer 1 OK → chờ quét mặt từ điện thoại
            if event == "request_face":
                global waiting_for_face, face_fail_count, _face_timeout_timer
                waiting_for_face = True
                face_fail_count  = 0
                if _face_timeout_timer:
                    _face_timeout_timer.cancel()
                _face_timeout_timer = threading.Timer(10.0, _reset_face_wait)
                _face_timeout_timer.start()
                print("[APP] Layer 1 OK → Yêu cầu quét mặt từ điện thoại")
                # Push SSE để trang mobile tự động bật camera
                sse_push("face_request", {
                    "message": "Vui lòng quét khuôn mặt",
                    "time": time.strftime("%H:%M:%S")
                })

            # Cảnh báo xâm nhập từ ESP32 (SW-420)
            elif event == "intrusion":
                log_data = {
                    "user": "Hệ thống",
                    "status": "intrusion",
                    "type": "intrusion",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "timestamp": time.time()
                }
                threading.Thread(target=save_log, args=(log_data,), daemon=True).start()
                sse_push("intrusion_alert", {
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "message": "Phát hiện rung/va đập bất thường vào két sắt!"
                })
                print("[APP] ⚠️ CẢNH BÁO XÂM NHẬP!")

            elif event in ("unlocked", "locked"):
                user = payload.get("trigger", "Unknown")
                
                # Ánh xạ tên vân tay
                method_used = "không xác định"
                if "Finger" in user:
                    # Trích xuất số ID (hỗ trợ Finger_0 hoặc Fingerprint_0)
                    import re
                    match = re.search(r'\d+', user)
                    fp_id = match.group() if match else "0"
                    fps = load_fingerprints()
                    found_fp = next((f for f in fps if str(f["id"]) == str(fp_id)), None)
                    user = found_fp["label"] if found_fp else f"Vân tay {fp_id}"
                    method_used = "vân tay"
                elif user == "PIN_Bypass_L2":
                    user = "Người dùng"
                    method_used = "mật khẩu"
                elif user.startswith("OTP"):
                    user = "Người dùng"
                    method_used = "OTP"
                elif user == "stm32":
                    user = "Hệ thống"
                    method_used = "tự động"
                elif "Web" in user:
                    method_used = "Web"
                else:
                    method_used = "khuôn mặt"

                status_str = "granted" if event == "unlocked" else "locked"
                
                # Lưu thêm detail cho việc hiển thị log đẹp hơn
                detail_str = ""
                if event == "unlocked":
                    detail_str = f"mở khóa bằng {method_used}"

                log_data = {
                    "user": user,
                    "status": status_str,
                    "type": "esp_event", "event": event,
                    "detail": detail_str,
                    "method": method_used,
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "timestamp": time.time()
                }
                threading.Thread(target=save_log, args=(log_data,), daemon=True).start()
                
                # Báo cho frontend cập nhật giao diện
                if event == "unlocked":
                    sse_push("vault_update", {"open": True, "member": user})
                elif event == "locked":
                    sse_push("vault_update", {"open": False, "closed_by": user})

            elif event == "verify_otp":
                # Người dùng nhập OTP trên két → kiểm tra
                global current_otp, otp_expiry, otp_creator
                code = payload.get("code", "")
                print(f"[APP] Xác minh OTP: {code} (expected: {current_otp})")

                if current_otp and code == current_otp and time.time() < otp_expiry:
                    # OTP đúng + còn hạn
                    mqtt_publish_command("unlock_door2", f"OTP ({otp_creator})")
                    add_log("OTP", "Mở thành công", otp_creator or "Unknown", "Mở két bằng mã OTP")
                    sse_push("otp_used", {"status": "granted", "creator": otp_creator})
                    current_otp = None  # Xóa OTP (dùng 1 lần)
                    otp_expiry = 0
                    print(f"[APP] ✅ OTP đúng! Mở két.")
                else:
                    mqtt_publish_command("fail_door2", "OTP sai")
                    reason = "Mã sai" if current_otp != code else "Hết hạn"
                    add_log("OTP", "Thất bại", "Unknown", f"OTP {reason}")
                    sse_push("otp_used", {"status": "denied", "reason": reason})
                    print(f"[APP] ❌ OTP thất bại: {reason}")

            elif event == "enroll_ok":
                fp_id = payload.get("trigger", "0")
                label = pending_enroll.pop(str(fp_id), f"Fingerprint_{fp_id}")
                print(f"[APP] ✅ Đăng ký vân tay thành công! ID: {fp_id}, Label: {label}")
                sse_push("enroll_result", {"status": "ok", "finger_id": fp_id, "label": label})
                save_log({"user": label, "status": "enrolled", "type": "fingerprint", "detail": "Đã đăng ký thêm vân tay",
                          "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()})
                # Lưu vào fingerprints.json
                fps = load_fingerprints()
                fps = [f for f in fps if str(f.get("id")) != str(fp_id)]
                fps.append({"id": int(fp_id), "label": label,
                            "registered_at": time.strftime("%Y-%m-%d %H:%M:%S")})
                save_fingerprints(fps)

            elif event == "enroll_fail":
                reason = payload.get("trigger", "Unknown")
                print(f"[APP] ❌ Đăng ký vân tay thất bại: {reason}")
                sse_push("enroll_result", {"status": "fail", "reason": reason})

            elif event == "temp_lock_pin":
                # Nhập sai mật khẩu 3 lần → tạm khóa
                print("[APP] ⚠️ Nhập sai PIN 3 lần → Tạm khóa két!")
                save_log({
                    "user": "Không xác định", "status": "denied",
                    "type": "security_alert", "detail": "Nhập sai mật khẩu 3 lần - Tạm khóa",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()
                })
                sse_push("security_alert", {
                    "type": "pin_fail",
                    "message": "⚠️ Nhập sai mật khẩu 3 lần! Két tạm khóa.",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S")
                })

            elif event == "finger_fail":
                # Quét sai vân tay 3 lần
                print("[APP] ⚠️ Quét sai vân tay 3 lần!")
                save_log({
                    "user": "Không xác định", "status": "denied",
                    "type": "security_alert", "detail": "Quét sai vân tay 3 lần",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()
                })
                sse_push("security_alert", {
                    "type": "finger_fail",
                    "message": "⚠️ Quét sai vân tay 3 lần! Két tạm khóa.",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S")
                })

            elif event == "door_open_long":
                # Cửa két mở quá lâu (>20s)
                print("[APP] ⚠️ Cửa két mở quá lâu!")
                log_data = {
                    "user": "Hệ thống", "status": "warning",
                    "type": "security_alert", "detail": "Cửa két mở quá 20 giây",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()
                }
                threading.Thread(target=save_log, args=(log_data,), daemon=True).start()
                sse_push("security_alert", {
                    "type": "door_open_long",
                    "message": "🚪 Cửa két mở quá 20 giây! Vui lòng đóng cửa két.",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S")
                })

    except Exception as e:
        print(f"[MQTT] Lỗi xử lý message: {e}")

def on_disconnect(client, userdata, flags, reason_code, properties):
    global esp_online, mqtt_connected
    esp_online = mqtt_connected = False
    print(f"[MQTT] Ngắt kết nối, code={reason_code}")

# ──────────────────────────────────────────
#  KHỞI ĐỘNG MQTT
# ──────────────────────────────────────────
def start_mqtt():
    global mqtt_client_
    while True:
        try:
            cid    = f"flask-safevault-{os.getpid()}"
            client = mqtt_client.Client(mqtt_client.CallbackAPIVersion.VERSION2, client_id=cid)
            client.on_connect    = on_connect
            client.on_message    = on_message
            client.on_disconnect = on_disconnect
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            mqtt_client_ = client
            client.loop_forever()
        except Exception as e:
            print(f"[MQTT] Lỗi: {e}. Thử lại sau 5s...")
            time.sleep(5)

_mqtt_thread = threading.Thread(target=start_mqtt, daemon=True)
_mqtt_thread.start()

# Khởi động AI engine
recognition_engine.start_db_watcher()

# ──────────────────────────────────────────
#  MQTT PUBLISH
# ──────────────────────────────────────────
def mqtt_publish_command(action: str, user: str = "Unknown"):
    if mqtt_client_ is None: return False
    for _ in range(8):
        if mqtt_connected: break
        time.sleep(0.5)
    if not mqtt_connected: return False
    try:
        payload = json.dumps({"action": action, "user": user, "time": time.strftime("%H:%M")})
        result  = mqtt_client_.publish(TOPIC_COMMAND, payload, qos=1)
        result.wait_for_publish(timeout=5)
        ok = result.is_published()
        print(f"[MQTT] Publish {'OK' if ok else 'FAILED'}: {payload}")
        return ok
    except Exception as e:
        print(f"[MQTT] Publish error: {e}")
        return False

# ──────────────────────────────────────────
#  SSE — SERVER-SENT EVENTS
# ──────────────────────────────────────────
_sse_lock        = threading.Lock()
_sse_subscribers = []

def sse_push(event_type: str, data: dict):
    msg = f"event: {event_type}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"
    with _sse_lock:
        dead = []
        for q in _sse_subscribers:
            try:    q.put_nowait(msg)
            except: dead.append(q)
        for q in dead: _sse_subscribers.remove(q)

def add_log(log_type, status, user, detail=""):
    log_data = {
        "user": user, "status": status, "type": log_type,
        "detail": detail,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()
    }
    threading.Thread(target=save_log, args=(log_data,), daemon=True).start()

# ──────────────────────────────────────────
#  HELPERS
# ──────────────────────────────────────────
def load_db():
    try:
        with open(DB_FILE, "r", encoding="utf-8") as f: return json.load(f)
    except: return {}

def save_db(db):
    with open(DB_FILE, "w", encoding="utf-8") as f:
        json.dump(db, f, indent=4, ensure_ascii=False)

def load_logs():
    try:
        with open(LOG_FILE, "r", encoding="utf-8") as f: return json.load(f)
    except: return []

def save_log(data):
    logs = load_logs()
    logs.insert(0, data)
    with open(LOG_FILE, "w", encoding="utf-8") as f:
        json.dump(logs, f, indent=4, ensure_ascii=False)

def load_fingerprints():
    try:
        with open(FP_FILE, "r", encoding="utf-8") as f: return json.load(f)
    except: return []

def save_fingerprints(fps):
    with open(FP_FILE, "w", encoding="utf-8") as f:
        json.dump(fps, f, indent=4, ensure_ascii=False)

# ──────────────────────────────────────────
#  SERVE FRONTEND
# ──────────────────────────────────────────
@app.route("/")
def index():
    return send_from_directory("static", "index.html")

@app.route("/mobile")
def mobile():
    """Trang quét mặt tối ưu cho điện thoại."""
    return send_from_directory("static", "mobile_scan.html")

# ──────────────────────────────────────────
#  MOBILE SCAN — nhận ảnh từ điện thoại → AI
# ──────────────────────────────────────────
@app.route("/mobile/scan", methods=["POST"])
def mobile_scan():
    """
    Nhận ảnh base64 từ camera điện thoại.
    Chạy AI nhận dạng (LBPH + liveness).
    Nếu waiting_for_face=True → thực hiện mở két.
    """
    global waiting_for_face, face_fail_count
    try:
        data    = request.json or {}
        b64_img = data.get("image", "")
        if not b64_img:
            return jsonify({"status": "error", "message": "Không có ảnh"}), 400

        # ── AI nhận dạng ──
        result = recognition_engine.identify_face_from_b64(b64_img)
        user   = result["name"]
        status = result["status"]

        print(f"[SCAN] Kết quả AI: {result['reason']}")

        # ── Ghi log ──
        log_data = {
            "user": user, "status": status, "type": "access",
            "confidence": result.get("confidence", 0),
            "is_real": result.get("is_real", True),
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "timestamp": time.time()
        }
        threading.Thread(target=save_log, args=(log_data,), daemon=True).start()

        # ── Xử lý kết quả ──
        if waiting_for_face:
            if status == "granted":
                # Cập nhật access count
                db = load_db()
                if user in db:
                    db[user]["access_count"] = db[user].get("access_count", 0) + 1
                    db[user]["last_access"]  = time.strftime("%Y-%m-%d %H:%M:%S")
                    save_db(db)

                # Két mở thành công (gửi lệnh)
                mqtt_publish_command("unlock_door2", user)

                waiting_for_face = False
                face_fail_count  = 0
                if _face_timeout_timer:
                    _face_timeout_timer.cancel()

                print(f"[APP] ✅ Két mở bởi: {user} ({result['confidence']}%)")

                # Push SSE cập nhật dashboard
                sse_push("vault_update", {
                    "open": True, "member": user,
                    "start_time": time.strftime("%Y-%m-%d %H:%M:%S")
                })

            elif status == "denied":
                face_fail_count += 1
                print(f"[APP] ❌ Xác minh thất bại lần {face_fail_count}/3")
                if face_fail_count >= 3:
                    mqtt_publish_command("temp_lock")
                    waiting_for_face = False
                    face_fail_count  = 0
                    print("[APP] Khóa tạm 3 lần thất bại")
                else:
                    mqtt_publish_command("fail_door2", user)
        else:
            print("[APP] Nhận ảnh nhưng không đang chờ xác thực. Bỏ qua.")

        # Push SSE nhận dạng real-time cho dashboard
        sse_push("recognize", {
            "user": user, "status": status,
            "confidence": result.get("confidence", 0),
            "is_real": result.get("is_real", True),
            "reason": result.get("reason", ""),
            "time": time.strftime("%Y-%m-%d %H:%M:%S")
        })

        return jsonify({
            "status": status,
            "name": user,
            "confidence": result.get("confidence", 0),
            "is_real": result.get("is_real", True),
            "reason": result.get("reason", ""),
            "unlock": status == "granted"
        })

    except Exception as e:
        print(f"[SCAN] Lỗi: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ──────────────────────────────────────────
#  INTRUSION ALERT (SW-420 từ ESP32)
# ──────────────────────────────────────────
@app.route("/intrusion", methods=["POST"])
def intrusion():
    """Nhận cảnh báo xâm nhập từ ESP32 (cảm biến rung SW-420)."""
    log_data = {
        "user": "Hệ thống",
        "status": "intrusion",
        "type": "intrusion",
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "timestamp": time.time()
    }
    save_log(log_data)
    sse_push("intrusion_alert", {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "message": "Phát hiện rung/va đập bất thường!"
    })
    return jsonify({"status": "ok"})

# ──────────────────────────────────────────
#  VAULT STATUS
# ──────────────────────────────────────────
@app.route("/vault/status", methods=["GET"])
def vault_status():
    return jsonify({"open": False})

@app.route("/vault/close", methods=["POST"])
def vault_close():
    # Admin đóng két từ web dashboard
    sent = mqtt_publish_command("close_vault", "Admin")
    return jsonify({"status": "success", "message": "Đã gửi lệnh khóa két", "mqtt_sent": sent})

# ──────────────────────────────────────────
#  TRẠNG THÁI LAYER 2 + TEST
# ──────────────────────────────────────────
@app.route("/status", methods=["GET"])
def get_status():
    return jsonify({"waiting_for_face": waiting_for_face})

@app.route("/test_face", methods=["POST"])
def test_face():
    global waiting_for_face, face_fail_count, _face_timeout_timer
    waiting_for_face = True; face_fail_count = 0
    if _face_timeout_timer: _face_timeout_timer.cancel()
    _face_timeout_timer = threading.Timer(10.0, _reset_face_wait)
    _face_timeout_timer.start()
    sse_push("face_request", {"message": "Test: Vui lòng quét khuôn mặt", "time": time.strftime("%H:%M:%S")})
    return jsonify({"status": "ok", "message": "Đã kích hoạt Layer 2!"})

# ──────────────────────────────────────────
#  OTP — Mã dùng 1 lần
# ──────────────────────────────────────────
@app.route("/otp/generate", methods=["POST"])
def otp_generate():
    """Tạo mã OTP 6 số. Yêu cầu gửi kèm ảnh khuôn mặt để xác thực."""
    global current_otp, otp_expiry, otp_creator
    try:
        data = request.json or {}
        b64_img = data.get("image", "")
        if not b64_img:
            return jsonify({"status": "error", "message": "Cần ảnh khuôn mặt để xác thực"}), 400

        # Xác thực khuôn mặt
        result = recognition_engine.identify_face_from_b64(b64_img)
        if result["status"] != "granted":
            return jsonify({"status": "denied", "message": "Xác thực khuôn mặt thất bại", "reason": result.get("reason", "")})

        # Tạo OTP 6 số
        current_otp = str(random.randint(100000, 999999))
        otp_expiry = time.time() + 300  # 5 phút
        otp_creator = result["name"]

        # Gửi OTP xuống ESP32 (tùy chọn, để STM32 biết)
        mqtt_publish_command("set_otp", current_otp)

        save_log({"user": otp_creator, "status": "otp_created", "type": "otp", "detail": "Đã tạo mã OTP dùng 1 lần",
                  "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()})

        print(f"[APP] 🔑 OTP tạo bởi {otp_creator}: {current_otp} (hết hạn sau 5 phút)")
        return jsonify({"status": "ok", "otp": current_otp, "creator": otp_creator,
                        "expires_in": 300, "message": f"Mã OTP: {current_otp}"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route("/otp/status", methods=["GET"])
def otp_status():
    if current_otp and time.time() < otp_expiry:
        remaining = int(otp_expiry - time.time())
        return jsonify({"active": True, "remaining_sec": remaining, "creator": otp_creator})
    return jsonify({"active": False})

# ──────────────────────────────────────────
#  ĐĂNG KÝ THÀNH VIÊN
# ──────────────────────────────────────────
@app.route("/register", methods=["POST"])
def register():
    try:
        data      = request.json
        name      = data.get("name", "").strip()
        images    = data.get("images", [])
        img_b64   = data.get("image", "")
        if not name:
            return jsonify({"status": "error", "message": "Tên không được để trống"}), 400
        if not images and img_b64: images = [img_b64]
        if not images:
            return jsonify({"status": "error", "message": "Cần ít nhất 1 ảnh"}), 400

        db = load_db()
        if name not in db:
            db[name] = {"registered_at": time.strftime("%Y-%m-%d %H:%M:%S"), "access_count": 0,
                        "full_image": images[0], "face_images": images}
        else:
            db[name].update({"full_image": images[0], "face_images": images})
        save_db(db)

        save_log({"user": name, "status": "registered", "type": "register", "detail": "Đã đăng ký thêm khuôn mặt",
                  "image_count": len(images), "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()})
        return jsonify({"status": "success", "message": f"Đã đăng ký {name} với {len(images)} ảnh!"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ──────────────────────────────────────────
#  LOG (từ script AI cũ — giữ backward compat)
# ──────────────────────────────────────────
@app.route("/log", methods=["POST"])
def log():
    try:
        data   = request.json
        user   = data.get("user", "Unknown")
        status = data.get("status", "unknown")
        log_data = {"user": user, "status": status, "type": "access",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()}
        save_log(log_data)
        if status == "granted":
            db = load_db()
            if user in db:
                db[user]["access_count"] = db[user].get("access_count", 0) + 1
                db[user]["last_access"]  = time.strftime("%Y-%m-%d %H:%M:%S")
                save_db(db)
        return jsonify({"status": "logged"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ──────────────────────────────────────────
#  USERS / LOGS / STATS
# ──────────────────────────────────────────
@app.route("/users", methods=["GET"])
def users():
    db = load_db()
    return jsonify([{"name": n, "registered_at": i.get("registered_at","N/A"),
                     "access_count": i.get("access_count",0), "last_access": i.get("last_access","Chưa"),
                     "avatar": i.get("full_image","")} for n, i in db.items()])

@app.route("/users/<name>", methods=["DELETE"])
def delete_user(name):
    try:
        db = load_db()
        if name in db: del db[name]; save_db(db); return jsonify({"status": "success"})
        return jsonify({"status": "error", "message": "Không tìm thấy"}), 404
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route("/fingerprints", methods=["GET"])
def get_fingerprints():
    """Danh sách vân tay đã đăng ký."""
    return jsonify(load_fingerprints())

@app.route("/fingerprints/<int:fp_id>", methods=["DELETE"])
def delete_fingerprint(fp_id):
    """Xóa vân tay khỏi database web (không xóa trên chip AS608)."""
    fps = load_fingerprints()
    new_fps = [f for f in fps if f.get("id") != fp_id]
    if len(new_fps) == len(fps):
        return jsonify({"status": "error", "message": "Không tìm thấy"}), 404
    save_fingerprints(new_fps)
    return jsonify({"status": "success"})

@app.route("/logs", methods=["GET"])
def logs(): return jsonify(load_logs())

@app.route("/logs/clear", methods=["DELETE"])
def clear_logs():
    with open(LOG_FILE, "w") as f: json.dump([], f)
    return jsonify({"status": "success"})

@app.route("/stats", methods=["GET"])
def stats():
    db   = load_db(); logs = load_logs(); fps = load_fingerprints()
    return jsonify({
        "total_users":  len(db),
        "total_fingerprints": len(fps),
        "total_access": len([l for l in logs if l.get("type") == "access"]),
        "granted":      len([l for l in logs if l.get("status") == "granted"]),
        "denied":       len([l for l in logs if l.get("status") == "denied"]),
        "intrusions":   len([l for l in logs if l.get("type") == "intrusion"])
    })

# ──────────────────────────────────────────
#  EMERGENCY UNLOCK
# ──────────────────────────────────────────
@app.route("/unlock", methods=["POST"])
def unlock():
    sent = mqtt_publish_command("unlock", "Khẩn cấp Web")
    save_log({"user": "Khẩn cấp (Web)", "status": "granted", "type": "emergency",
              "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()})
    return jsonify({"status": "success", "message": "Đã gửi lệnh mở khẩn cấp", "mqtt_sent": sent})

@app.route("/unlock_door2", methods=["POST"])
def unlock_door2():
    data = request.json or {}
    user = data.get("user", "Khẩn cấp (Web)")
    sent = mqtt_publish_command("unlock_door2", user)
    save_log({"user": user, "status": "granted", "type": "emergency_vault",
              "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()})
    return jsonify({"status": "success", "message": "Đã mở két khẩn cấp", "mqtt_sent": sent})

# ──────────────────────────────────────────
#  REMOTE UNLOCK (Xác thực khuôn mặt từ xa)
# ──────────────────────────────────────────
@app.route("/remote/unlock", methods=["POST"])
def remote_unlock():
    """Mở két từ xa: nhận ảnh khuôn mặt → AI nhận dạng → nếu đúng → MQTT mở két."""
    try:
        data    = request.json or {}
        b64_img = data.get("image", "")
        if not b64_img:
            return jsonify({"status": "denied", "reason": "Không có ảnh"}), 400

        # AI nhận dạng
        result = recognition_engine.identify_face_from_b64(b64_img)
        user   = result["name"]
        status = result["status"]

        # Ghi log
        log_data = {
            "user": user, "status": status, "type": "remote_unlock",
            "confidence": result.get("confidence", 0),
            "time": time.strftime("%Y-%m-%d %H:%M:%S"), "timestamp": time.time()
        }
        threading.Thread(target=save_log, args=(log_data,), daemon=True).start()

        if status == "granted":
            # Cập nhật access count
            db = load_db()
            if user in db:
                db[user]["access_count"] = db[user].get("access_count", 0) + 1
                db[user]["last_access"]  = time.strftime("%Y-%m-%d %H:%M:%S")
                save_db(db)

            # Gửi MQTT mở két
            mqtt_publish_command("unlock_door2", user)
            print(f"[REMOTE] ✅ Két mở từ xa bởi: {user} ({result.get('confidence', 0)}%)")

            sse_push("vault_update", {
                "open": True, "member": user,
                "start_time": time.strftime("%Y-%m-%d %H:%M:%S")
            })

            return jsonify({
                "status": "granted", "name": user,
                "confidence": result.get("confidence", 0),
                "reason": f"Xác thực thành công: {user}"
            })
        else:
            print(f"[REMOTE] ❌ Từ chối: {result.get('reason', 'Unknown')}")
            return jsonify({
                "status": "denied", "name": user,
                "reason": result.get("reason", "Không nhận diện được")
            })
    except Exception as e:
        print(f"[REMOTE] Lỗi: {e}")
        return jsonify({"status": "denied", "reason": str(e)}), 500
# ──────────────────────────────────────────
#  FINGERPRINT ENROLLMENT
# ──────────────────────────────────────────
@app.route("/fingerprint/enroll", methods=["POST"])
def fingerprint_enroll():
    """Bắt đầu đăng ký vân tay mới. Gửi lệnh xuống STM32 qua MQTT."""
    data = request.json or {}
    label = data.get("label", "Unknown")

    # Tự động tìm ID còn trống (0-126)
    fps = load_fingerprints()
    used_ids = [int(f["id"]) for f in fps if "id" in f]
    finger_id = None
    for i in range(127):
        if i not in used_ids:
            finger_id = i
            break
            
    if finger_id is None:
        return jsonify({"status": "error", "message": "Số lượng vân tay đã đạt tối đa (127)"}), 400

    # Gửi lệnh qua MQTT
    payload = json.dumps({
        "action": "enroll_finger",
        "finger_id": finger_id,
        "user": label,
        "time": time.strftime("%H:%M")
    })
    if mqtt_client_ and mqtt_connected:
        mqtt_client_.publish(TOPIC_COMMAND, payload, qos=1)
        pending_enroll[str(finger_id)] = label  # Lưu label để dùng khi enroll_ok
        print(f"[APP] Gửi lệnh đăng ký vân tay ID={finger_id}, label={label}")
        return jsonify({"status": "ok", "message": f"Đang đăng ký vân tay ID {finger_id}. Vui lòng đặt ngón tay lên cảm biến."})
    else:
        return jsonify({"status": "error", "message": "MQTT chưa kết nối"}), 503

# ──────────────────────────────────────────
#  SSE ENDPOINT
# ──────────────────────────────────────────
@app.route("/events")
def events():
    def stream():
        q = queue.Queue(maxsize=20)
        with _sse_lock: _sse_subscribers.append(q)
        try:
            yield "event: connected\ndata: {}\n\n"
            while True:
                try:    yield q.get(timeout=25)
                except queue.Empty: yield ": heartbeat\n\n"
        except GeneratorExit: pass
        finally:
            with _sse_lock:
                if q in _sse_subscribers: _sse_subscribers.remove(q)
    return Response(stream(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

@app.route("/esp/status", methods=["GET"])
def esp_status():
    return jsonify({"online": esp_online, "mqtt_connected": mqtt_connected,
                    "mqtt_broker": MQTT_BROKER, "topic_command": TOPIC_COMMAND,
                    "battery_percent": battery_percent,
                    "battery_voltage": round(battery_voltage, 2)})

@app.route("/battery", methods=["GET"])
def get_battery():
    """Trả về thông tin pin hiện tại."""
    return jsonify({
        "percent": battery_percent,
        "voltage": round(battery_voltage, 2),
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(battery_updated)) if battery_updated else None,
        "status": "charging" if battery_voltage > 4.15 else ("low" if battery_percent < 20 else "normal")
    })

# ──────────────────────────────────────────
#  MAIN
# ──────────────────────────────────────────
if __name__ == "__main__":
    ssl_cert = os.path.join(os.path.dirname(__file__), "ssl", "cert.pem")
    ssl_key  = os.path.join(os.path.dirname(__file__), "ssl", "key.pem")
    if os.path.exists(ssl_cert) and os.path.exists(ssl_key):
        print("🔒 HTTPS MODE — Camera điện thoại sẽ hoạt động!")
        app.run(debug=True, host="0.0.0.0", port=5000, threaded=True,
                use_reloader=False, ssl_context=(ssl_cert, ssl_key))
    else:
        print("⚠️  HTTP MODE — Chạy: bash generate_ssl.sh để bật HTTPS")
        app.run(debug=True, host="0.0.0.0", port=5000, threaded=True, use_reloader=False)
