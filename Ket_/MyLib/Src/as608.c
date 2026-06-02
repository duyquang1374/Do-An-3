/**
 * @file    as608.c
 * @brief   AS608 Fingerprint Sensor Library - Implementation
 * @note    Giao tiếp packet qua UART (HAL), baud 57600
 */

#include "as608.h"
#include <string.h>

/* ── Private ── */
static UART_HandleTypeDef *_huart = NULL;
static uint32_t _addr = AS608_DEFAULT_ADDR;

static uint8_t txBuf[32];
static uint8_t rxBuf[32];

/* ── Gửi/Nhận packet AS608 ── */

/**
 * @brief  Xây dựng và gửi command packet tới AS608
 * @param  cmd:  instruction code
 * @param  data: con trỏ dữ liệu phụ (hoặc NULL)
 * @param  dlen: kích thước dữ liệu phụ (byte)
 */
static void AS608_SendCmd(uint8_t cmd, const uint8_t *data, uint16_t dlen)
{
    uint16_t pktLen = 2 + 1 + dlen; /* length field = cmd(1) + checksum(2) + data */
    /* Thực ra: length = 1(PID) + 2(len trừ header+addr) ... Theo datasheet:
       Packet length = số byte từ sau Length đến hết Checksum
       = 1 (instruction) + dlen (data) + 2 (checksum) */
    /* Nhưng AS608 datasheet: Package Length = từ package identifier KHÔNG tính
       → length = 1 (PID included? No!)
       Thực tế format:
       [Header 2B][Addr 4B][PID 1B][Length 2B][Instruction 1B][Data...][Checksum 2B]
       Length = 1 (Instruction) + N (data) + 2 (checksum) = 3 + dlen
    */
    pktLen = 1 + dlen + 2; /* instruction + data + checksum */

    uint16_t idx = 0;
    /* Header */
    txBuf[idx++] = (AS608_HEADER >> 8) & 0xFF;  /* 0xEF */
    txBuf[idx++] = AS608_HEADER & 0xFF;          /* 0x01 */
    /* Address */
    txBuf[idx++] = (_addr >> 24) & 0xFF;
    txBuf[idx++] = (_addr >> 16) & 0xFF;
    txBuf[idx++] = (_addr >> 8)  & 0xFF;
    txBuf[idx++] = _addr & 0xFF;
    /* PID */
    txBuf[idx++] = AS608_PID_CMD;
    /* Length */
    txBuf[idx++] = (pktLen >> 8) & 0xFF;
    txBuf[idx++] = pktLen & 0xFF;
    /* Instruction */
    txBuf[idx++] = cmd;
    /* Data */
    if (data && dlen > 0) {
        memcpy(&txBuf[idx], data, dlen);
        idx += dlen;
    }
    /* Checksum = PID + Length(2B) + Instruction + Data */
    uint16_t cs = AS608_PID_CMD;
    cs += (pktLen >> 8) & 0xFF;
    cs += pktLen & 0xFF;
    cs += cmd;
    for (uint16_t i = 0; i < dlen; i++) cs += data[i];
    txBuf[idx++] = (cs >> 8) & 0xFF;
    txBuf[idx++] = cs & 0xFF;

    HAL_UART_Transmit(_huart, txBuf, idx, 500);
}

/**
 * @brief  Nhận ACK packet từ AS608
 * @param  ack:     con trỏ nhận byte xác nhận (confirmation code)
 * @param  payload: con trỏ nhận dữ liệu trả về (hoặc NULL)
 * @param  plen:    con trỏ nhận kích thước payload
 * @param  timeout: thời gian chờ tối đa (ms)
 * @retval AS608_OK nếu nhận thành công, AS608_TIMEOUT hoặc AS608_RX_ERR
 */
static uint8_t AS608_RecvAck(uint8_t *ack, uint8_t *payload, uint16_t *plen, uint32_t timeout)
{
    /* Nhận header + addr + PID + length = 9 byte trước */
    memset(rxBuf, 0, sizeof(rxBuf));

    if (HAL_UART_Receive(_huart, rxBuf, 9, timeout) != HAL_OK)
        return AS608_TIMEOUT;

    /* Kiểm tra header */
    if (rxBuf[0] != 0xEF || rxBuf[1] != 0x01)
        return AS608_RX_ERR;

    /* Kiểm tra PID = ACK (0x07) */
    if (rxBuf[6] != AS608_PID_ACK)
        return AS608_RX_ERR;

    /* Đọc length */
    uint16_t len = ((uint16_t)rxBuf[7] << 8) | rxBuf[8];
    /* len bao gồm: confirmation_code(1) + data(?) + checksum(2)
       → data_len = len - 3 */

    /* Nhận phần còn lại: confirmation_code + data + checksum = len byte */
    if (len > sizeof(rxBuf) - 9) len = sizeof(rxBuf) - 9;
    if (HAL_UART_Receive(_huart, &rxBuf[9], len, timeout) != HAL_OK)
        return AS608_TIMEOUT;

    /* Confirmation code */
    *ack = rxBuf[9];

    /* Payload (nếu có) */
    uint16_t dataLen = len - 3; /* trừ ack(1) + checksum(2) */
    if (payload && plen && dataLen > 0) {
        memcpy(payload, &rxBuf[10], dataLen);
        *plen = dataLen;
    } else if (plen) {
        *plen = 0;
    }

    return AS608_OK;
}

/* ══════════════════════════════════════ */
/*  PUBLIC API                           */
/* ══════════════════════════════════════ */

uint8_t AS608_Init(UART_HandleTypeDef *huart)
{
    _huart = huart;

    /* Handshake: VfyPwd (0x13) với password mặc định */
    uint8_t pwd[4] = {0, 0, 0, 0};
    AS608_SendCmd(0x13, pwd, 4);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 1000);
    if (ret != AS608_OK) return ret;
    return ack; /* 0x00 = OK, 0x13 = sai mật khẩu */
}

uint8_t AS608_GetImage(void)
{
    AS608_SendCmd(0x01, NULL, 0);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_GenChar(uint8_t slot)
{
    uint8_t data[1] = { slot };
    AS608_SendCmd(0x02, data, 1);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_Search(AS608_SearchResult_t *result)
{
    /* Search(0x04): CharBuffer1, StartPage=0, PageNum=AS608_LIBRARY_SIZE */
    uint8_t data[5] = {
        0x01,                            /* buffer 1 */
        0x00, 0x00,                      /* start page = 0 */
        0x00, AS608_LIBRARY_SIZE         /* page count */
    };
    AS608_SendCmd(0x04, data, 5);

    uint8_t ack;
    uint8_t payload[4];
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, payload, &plen, 3000);
    if (ret != AS608_OK) {
        result->status = ret;
        return ret;
    }

    result->status = ack;
    if (ack == AS608_OK && plen >= 4) {
        result->finger_id  = ((uint16_t)payload[0] << 8) | payload[1];
        result->confidence = ((uint16_t)payload[2] << 8) | payload[3];
    } else {
        result->finger_id  = 0;
        result->confidence = 0;
    }
    return ack;
}

uint8_t AS608_RegModel(void)
{
    AS608_SendCmd(0x05, NULL, 0);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_Store(uint16_t id)
{
    uint8_t data[3] = {
        0x01,                  /* CharBuffer1 */
        (id >> 8) & 0xFF,
        id & 0xFF
    };
    AS608_SendCmd(0x06, data, 3);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_Delete(uint16_t id, uint16_t count)
{
    uint8_t data[4] = {
        (id >> 8) & 0xFF, id & 0xFF,
        (count >> 8) & 0xFF, count & 0xFF
    };
    AS608_SendCmd(0x0C, data, 4);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_Empty(void)
{
    AS608_SendCmd(0x0D, NULL, 0);

    uint8_t ack;
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, NULL, &plen, 2000);
    if (ret != AS608_OK) return ret;
    return ack;
}

uint8_t AS608_GetTemplateCount(uint16_t *count)
{
    AS608_SendCmd(0x1D, NULL, 0);

    uint8_t ack;
    uint8_t payload[2];
    uint16_t plen;
    uint8_t ret = AS608_RecvAck(&ack, payload, &plen, 2000);
    if (ret != AS608_OK) return ret;
    if (ack == AS608_OK && plen >= 2) {
        *count = ((uint16_t)payload[0] << 8) | payload[1];
    } else {
        *count = 0;
    }
    return ack;
}
