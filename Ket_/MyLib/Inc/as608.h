/**
 * @file    as608.h
 * @brief   AS608 Fingerprint Sensor Library for STM32 (HAL UART)
 * @note    Giao tiếp qua UART, baud rate mặc định 57600
 */

#ifndef AS608_H
#define AS608_H

#include "main.h"
#include <stdint.h>

/* ── Mã trả về ── */
#define AS608_OK            0x00  // Thành công
#define AS608_RX_ERR        0x01  // Lỗi nhận gói tin
#define AS608_NO_FINGER     0x02  // Không phát hiện ngón tay
#define AS608_ENROLL_FAIL   0x03  // Lỗi thu ảnh
#define AS608_IMG_MESSY     0x06  // Ảnh quá mờ
#define AS608_IMG_SMALL     0x07  // Ảnh quá nhỏ
#define AS608_NOT_FOUND     0x09  // Không tìm thấy vân tay
#define AS608_MERGE_ERR     0x0A  // Lỗi ghép template
#define AS608_BAD_LOC       0x0B  // ID vượt quá phạm vi
#define AS608_DB_ERR        0x0C  // Lỗi đọc DB
#define AS608_UPLOAD_ERR    0x0D  // Lỗi upload
#define AS608_NO_PKT        0x0E  // Không nhận được gói tiếp theo
#define AS608_DELETE_ERR    0x10  // Lỗi xóa
#define AS608_EMPTY_ERR     0x11  // Lỗi xóa toàn bộ
#define AS608_PWD_ERR       0x13  // Sai mật khẩu
#define AS608_TIMEOUT       0xFE  // Timeout
#define AS608_ERR           0xFF  // Lỗi chung

/* ── Cấu hình ── */
#define AS608_DEFAULT_ADDR  0xFFFFFFFF
#define AS608_DEFAULT_PWD   0x00000000
#define AS608_HEADER        0xEF01
#define AS608_PID_CMD       0x01  // Command packet
#define AS608_PID_DATA      0x02  // Data packet
#define AS608_PID_ACK       0x07  // Acknowledge packet
#define AS608_PID_END       0x08  // End of data packet

#define AS608_LIBRARY_SIZE  127   // Số vân tay tối đa (0-126)

/* ── Kết quả tìm kiếm ── */
typedef struct {
    uint8_t  status;     // AS608_OK nếu tìm thấy
    uint16_t finger_id;  // ID vân tay khớp
    uint16_t confidence; // Độ tin cậy (0-65535)
} AS608_SearchResult_t;

/* ── Hàm API ── */

/**
 * @brief  Khởi tạo AS608, kiểm tra kết nối (handshake)
 * @param  huart: con trỏ tới UART handle (vd: &huart3)
 * @retval AS608_OK nếu kết nối thành công
 */
uint8_t AS608_Init(UART_HandleTypeDef *huart);

/**
 * @brief  Thu ảnh vân tay từ cảm biến
 * @retval AS608_OK, AS608_NO_FINGER, hoặc mã lỗi
 */
uint8_t AS608_GetImage(void);

/**
 * @brief  Tạo đặc trưng (character file) từ ảnh vân tay
 * @param  slot: 1 hoặc 2 (CharBuffer1 hoặc CharBuffer2)
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_GenChar(uint8_t slot);

/**
 * @brief  Tìm vân tay trong cơ sở dữ liệu chip
 * @param  result: con trỏ nhận kết quả (ID, confidence)
 * @retval AS608_OK nếu tìm thấy, AS608_NOT_FOUND nếu không
 */
uint8_t AS608_Search(AS608_SearchResult_t *result);

/**
 * @brief  Ghép 2 mẫu CharBuffer1 và CharBuffer2 thành template
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_RegModel(void);

/**
 * @brief  Lưu template vào cơ sở dữ liệu với ID chỉ định
 * @param  id: vị trí lưu (0 - AS608_LIBRARY_SIZE-1)
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_Store(uint16_t id);

/**
 * @brief  Xóa một vân tay khỏi cơ sở dữ liệu
 * @param  id: vị trí cần xóa
 * @param  count: số lượng cần xóa (thường = 1)
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_Delete(uint16_t id, uint16_t count);

/**
 * @brief  Xóa toàn bộ cơ sở dữ liệu vân tay
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_Empty(void);

/**
 * @brief  Lấy số lượng vân tay đã lưu
 * @param  count: con trỏ nhận số lượng
 * @retval AS608_OK hoặc mã lỗi
 */
uint8_t AS608_GetTemplateCount(uint16_t *count);

#endif /* AS608_H */
