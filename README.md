(The file `/home/phongpkf/Downloads/Do-An-3/README.md` exists, but is empty)
SafeVault — Smart Safe / IoT Access System
=========================================

Tổng quan
---------
SafeVault là hệ thống két an toàn IoT gồm 3 thành phần chính:
- `Ket_` (STM32F103): firmware điều khiển keypad, relay, cảm biến vân tay AS608, OLED, và logic PIN/OTP/Layer2.
- `Gateway` (ESP32): chương trình `Gateway.ino` làm cầu nối giữa STM32 (UART) và dashboard web (MQTT).
- `Web` (Flask + DeepFace): giao diện dashboard, API và engine nhận dạng khuôn mặt (DeepFace).

Tính năng chính
---------------
- Nhập PIN (Stealth PIN) + tùy chọn Layer 2: vân tay (AS608) tại chỗ hoặc xác thực khuôn mặt từ xa.
- Đăng ký vân tay từ dashboard (gửi lệnh xuống STM32 để enroll).
- Mở/khóa két qua MQTT từ web dashboard hoặc qua OTP (mã 1 lần).
- Cảnh báo xâm nhập từ cảm biến rung (SW-420) báo về dashboard.
- Giao diện web realtime sử dụng Server-Sent Events (SSE).

Thành phần phần cứng & chân nối (tóm tắt)
----------------------------------------
- STM32F103 (Ket_):
	- Keypad 4x3: Rows PA0..PA3, Cols PA4..PA6
	- OLED I2C: `hi2c1` (SSD1306)
	- Relay: PC13 (Active-LOW typical)
	- Fingerprint AS608: UART (sử dụng `huart3` trong firmware)
	- UART <-> ESP32: USART1 (PA9 = TX, PA10 = RX)

- ESP32 Gateway (`Gateway.ino`):
	- UART pins: RX=16, TX=17 (Serial2) connected to STM32 PA9/PA10
	- MQTT broker mặc định: `broker.hivemq.com:1883`
	- Cấu hình WiFi: chỉnh `WIFI_SSID` / `WIFI_PASSWORD` trong file

Chuẩn topic MQTT (mặc định)
---------------------------
- Command topic (subscribe trên ESP32): `smartlock/quang_1307/command`
- Status topic (publish từ ESP32): `smartlock/quang_1307/status`
- Heartbeat topic: `smartlock/quang_1307/heartbeat`

Hướng dẫn build & chạy
----------------------

1) Firmware STM32 (`Ket_`)
- Mở thư mục `Ket_` trong STM32CubeIDE hoặc Keil uVision (.ioc và MDK-ARM project có sẵn).
- Build và flash bằng ST-Link (hoặc phương tiện bạn sử dụng).
- Lưu ý: project dùng HAL, cấu hình I2C/USART/GPIO đã có trong mã nguồn.

2) ESP32 Gateway (`Gateway/Gateway.ino`)
- Mở `Gateway/Gateway.ino` trong Arduino IDE hoặc PlatformIO.
- Sửa `WIFI_SSID` và `WIFI_PASSWORD` cho mạng của bạn.
- Chọn board ESP32 tương ứng, compile và upload.

3) Web server + AI (`Web/`)
- Thư mục chứa `app.py` và `recognition_engine.py`.
- Trên Raspberry Pi / Linux, bạn có thể dùng `setup_pi.sh` để cài phụ thuộc: xem `Web/setup_pi.sh`.

Ví dụ cài thủ công (Linux):
```bash
python3 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install flask paho-mqtt opencv-python-headless deepface ultralytics numpy requests
```
- Chạy server:
```bash
source venv/bin/activate
python3 Web/app.py
```
- `recognition_engine.py` sử dụng DeepFace (Facenet) để tạo embedding và so sánh cosine distance.
- Lưu ý: tải model cần thiết và đảm bảo các thư viện ML tương thích (thời gian tải model và tài nguyên cao trên Pi).

Endpoints & quyền năng chính (tóm tắt)
-------------------------------------
- `GET /` → dashboard (static/index.html)
- `POST /mobile/scan` → gửi ảnh base64 để nhận dạng khuôn mặt
- `POST /fingerprint/enroll` → bắt đầu đăng ký vân tay (gửi lệnh xuống STM32 qua MQTT)
- `POST /otp/generate` → tạo OTP sau khi xác thực khuôn mặt
- `POST /unlock` hoặc `POST /unlock_door2` → mở két (gửi lệnh MQTT)

Lưu ý cấu hình
--------------
- MQTT broker mặc định đặt là `broker.hivemq.com`. Bạn có thể đổi broker trong `Web/app.py` và `Gateway/Gateway.ino`.
- Nếu muốn HTTPS cho mobile camera, đặt chứng chỉ vào `Web/ssl/cert.pem` và `Web/ssl/key.pem`.

Sử dụng nhanh
--------------
1. Flash firmware STM32.
2. Upload sketch ESP32, đảm bảo ESP32 kết nối WiFi và MQTT.
3. Trên server chạy `app.py` (và đảm bảo recognition engine sẵn sàng).
4. Mở dashboard trên trình duyệt: `http://<server_ip>:5000/` hoặc `/mobile` để quét mặt.

Ghi chú & Troubleshooting
--------------------------
- Nếu không thấy kết nối MQTT: kiểm tra WiFi trên ESP32 và broker reachable.
- Nếu STM32 không nhận lệnh từ ESP32: kiểm tra nối dây UART (PA9/PA10 ↔ RX16/TX17), và baud rate 115200.
- DeepFace có thể cần tải model ban đầu — quá trình này tốn thời gian và dung lượng.
- Để debug UART, dùng Serial prints trên ESP32 (Serial 115200) và log trên STM32 nếu có.

Tệp liên quan
--------------
- [Gateway/Gateway.ino](Gateway/Gateway.ino) — ESP32 gateway
- [Ket_/Core/Src/main.c](Ket_/Core/Src/main.c) — firmware chính STM32
- [Web/app.py](Web/app.py) — Flask app & API
- [Web/recognition_engine.py](Web/recognition_engine.py) — AI face recognition

Muốn tôi mở rộng README (sơ đồ nối dây, hình ảnh, `requirements.txt` hoặc scripts deploy), hay cần phiên bản README bằng tiếng Anh không?
