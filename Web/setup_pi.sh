#!/bin/bash
# =============================================
#  SMART LOCK - Cài đặt trên Raspberry Pi 4
# =============================================

echo "=========================================="
echo "  Smart Lock - Cài đặt Raspberry Pi 4"
echo "=========================================="

# 1. Cập nhật hệ thống
echo "[1/4] Đang cập nhật hệ thống..."
sudo apt update && sudo apt upgrade -y

# 2. Cài đặt các gói hệ thống cần thiết
echo "[2/4] Đang cài đặt gói hệ thống..."
sudo apt install -y python3-pip python3-venv libatlas-base-dev \
    libhdf5-dev libopenblas-dev libjpeg-dev libpng-dev \
    libavcodec-dev libavformat-dev libswscale-dev

# 3. Tạo virtual environment
echo "[3/4] Đang tạo môi trường Python..."
python3 -m venv venv
source venv/bin/activate

# 4. Cài thư viện Python
echo "[4/4] Đang cài đặt thư viện Python..."
pip install --upgrade pip
pip install flask paho-mqtt opencv-python-headless ultralytics deepface requests numpy

echo ""
echo "=========================================="
echo "  ✅ CÀI ĐẶT HOÀN TẤT!"
echo "=========================================="
echo ""
echo "  Hướng dẫn chạy:"
echo "  1. Kích hoạt môi trường:  source venv/bin/activate"
echo "  2. Terminal 1:            python3 app.py"
echo "  3. Terminal 2:            python3 main_recognition.py"
echo ""
echo "  Lưu ý:"
echo "  - Đặt file yolov8n-face.pt vào thư mục này"
echo "  - Sửa MODEL_PATH trong main_recognition.py nếu cần"
echo "  - Webcam USB sẽ tự nhận là /dev/video0"
echo "=========================================="
