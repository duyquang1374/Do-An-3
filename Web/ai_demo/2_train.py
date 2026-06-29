import os
import json
from deepface import DeepFace

DATASET_DIR = "dataset"
EMB_FILE = "embeddings.json"

def main():

    # Kiểm tra xem thư mục dataset có tồn tại và có dữ liệu không
    if not os.path.exists(DATASET_DIR) or not os.listdir(DATASET_DIR):
        print(f"Thư mục '{DATASET_DIR}' không tồn tại hoặc trống! Vui lòng chạy bước 1 trước.")
        return

    print("Đang tải mô hình FaceNet và trích xuất vector đặc trưng...")
    new_embeddings = {}
    total_samples = 0

    for person_name in os.listdir(DATASET_DIR):
        person_dir = os.path.join(DATASET_DIR, person_name)

        if not os.path.isdir(person_dir):
            continue
            
        user_embs = []

        for filename in os.listdir(person_dir):
            if filename.lower().endswith(('.png', '.jpg', '.jpeg')):
                img_path = os.path.join(person_dir, filename)
                try:

                    objs = DeepFace.represent(
                        img_path=img_path, 
                        model_name="Facenet", 
                        detector_backend="opencv", 
                        enforce_detection=False
                    )
                    if len(objs) > 0:
                        user_embs.append(objs[0]["embedding"])
                        total_samples += 1
                        print(f"-> Đã trích xuất thành công: {person_name}/{filename}")
                except Exception as e:
                    print(f"-> Lỗi trích xuất ảnh {filename}: {e}")
        
        if user_embs:
            new_embeddings[person_name] = user_embs

    # Mã hóa bộ dữ liệu vector thành file embeddings.json
    with open(EMB_FILE, "w", encoding="utf-8") as f:
        json.dump(new_embeddings, f, indent=4)

    print(f"\n✅ QUÁ TRÌNH HOÀN TẤT!")


if __name__ == "__main__":
    main()
