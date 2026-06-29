import cv2

def main():

    cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
    face_cascade = cv2.CascadeClassifier(cascade_path)

    cap = cv2.VideoCapture(0)
    
    if not cap.isOpened():
        print("Không thể mở Webcam!")
        return
        
    print("Webcam đã mở. Đưa mặt vào camera để thử nghiệm. Nhấn 'q' để THOÁT.")
    
    while True:
        ret, frame = cap.read()
        if not ret:
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        

        faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(60, 60))

        for (x, y, w, h) in faces:
            # Vẽ hình chữ nhật màu Xanh dương (B=255, G=0, R=0), độ dày nét vẽ là 2
            cv2.rectangle(frame, (x, y), (x+w, y+h), (255, 0, 0), 2)
            
            # Viết chữ "Face" lên trên khung
            cv2.putText(frame, "Face", (x, y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)
            
        # Hiển thị cửa sổ
        cv2.imshow("Haar Cascade Face Detection", frame)

        # Nhấn phím 'q' để thoát
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
