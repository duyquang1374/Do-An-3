import cv2
import os
import time

DATASET_DIR = "dataset"

def main():
    name = input("Nhập tên thành viên đăng ký: ").strip()
    if not name:
        print("Tên không được để trống!")
        return

    user_dir = os.path.join(DATASET_DIR, name)
    os.makedirs(user_dir, exist_ok=True)

    cap = cv2.VideoCapture(0)
    
    if not cap.isOpened():
        print("Không thể mở Webcam!")
        return

    print(f"\n- Nhấn 's' CHỤP ẢNH"
          f"\n- Nhấn 'q' để THOÁT ")
    
    count = 0
    while True:
        ret, frame = cap.read()
        if not ret:
            print("Lỗi đọc webcam.")
            break
            
        cv2.imshow(f"Dang ky: {name} ", frame)
        key = cv2.waitKey(1) & 0xFF
        
        if key == ord('s'):
            count += 1
            filename = f"{name}_{int(time.time())}.jpg"
            filepath = os.path.join(user_dir, filename)
            cv2.imwrite(filepath, frame)
            print(f"Đã lưu ảnh {count}: {filepath}")
        elif key == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()

    if count > 0:
        print(f"\n✅ Hoàn tất! .")

    else:
        print("\nChưa có ảnh nào được lưu.")

if __name__ == "__main__":
    main()
