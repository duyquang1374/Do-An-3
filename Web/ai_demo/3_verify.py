import cv2
import json
import os
import numpy as np
import base64
from deepface import DeepFace

EMB_FILE = "./embeddings.json"
COSINE_THRESHOLD = 0.40

def find_cosine_distance(source_representation, test_representation):
    a = np.matmul(np.transpose(source_representation), test_representation)
    b = np.sum(np.multiply(source_representation, source_representation))
    c = np.sum(np.multiply(test_representation, test_representation))
    return 1 - (a / (np.sqrt(b) * np.sqrt(c)))

def main():
    with open(EMB_FILE, "r", encoding="utf-8") as f:
        known_embeddings = json.load(f)

    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("Không thể mở Webcam!")
        return

    print("Webcam đã mở. Quá trình nhận diện đang chạy... Nhấn 'q' để THOÁT.")

    face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")

    process_frame_count = 0

    # Biến lưu kết quả tạm thời để hiển thị mượt hơn trên video
    current_name = "Unknown"
    current_conf = 0.0
    current_color = (0, 0, 255) # Đỏ
    
    while True:
        ret, frame = cap.read()
        if not ret: 
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        # Phát hiện khuôn mặt
        faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(60, 60))

        # Nếu tìm thấy mặt
        if len(faces) > 0:
            # Chọn khuôn mặt lớn nhất (người đứng gần cam nhất)
            (x, y, w, h) = max(faces, key=lambda r: r[2] * r[3])

            # Cắt khuôn mặt ra để gửi vào mô hình AI
            face_img = frame[y:y+h, x:x+w]
            
            # Cứ mỗi 3 khung hình mới gọi AI 1 lần để video không bị quá giật lag
            process_frame_count += 1
            if process_frame_count % 3 == 0:
                try:
                    # Truyền ảnh đã cắt vào FaceNet để lấy đặc trưng
                    objs = DeepFace.represent(img_path=face_img, model_name="Facenet", detector_backend="opencv", enforce_detection=False)
                    if len(objs) > 0:
                        target_emb = objs[0]["embedding"]
                        
                        best_match = "Unknown"
                        min_dist = 1.0

                        # So sánh với tất cả người trong cơ sở dữ liệu
                        for name, embs in known_embeddings.items():
                            for emb in embs:
                                dist = find_cosine_distance(target_emb, emb)
                                if dist < min_dist:
                                    min_dist = dist
                                    best_match = name

                        # Quyết định kết quả dựa trên ngưỡng
                        if min_dist <= COSINE_THRESHOLD:
                            current_name = best_match
                            # Quy đổi khoảng cách thành % tự tin (thấp nhất 70%, cao nhất 100%)
                            current_conf = round(max(70.0, 100.0 - (min_dist / COSINE_THRESHOLD) * 30.0), 1)
                            current_color = (0, 255, 0) # Xanh lá
                        else:
                            current_name = "Unknown"
                            current_conf = 0.0
                            current_color = (0, 0, 255) # Đỏ
                except Exception as e:
                    pass

            # Vẽ khung hình chữ nhật quanh khuôn mặt
            cv2.rectangle(frame, (x, y), (x+w, y+h), current_color, 2)
            
            # Ghi chữ lên trên khung
            if current_name != "Unknown":
                text = f"{current_name} ({current_conf}%)"
            else:
                text = "Unknown"
                
            cv2.putText(frame, text, (x, y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.7, current_color, 2)

        # Hiển thị cửa sổ
        cv2.imshow("Real-time Face Recognition", frame)
        
        # Nhấn q để thoát
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
