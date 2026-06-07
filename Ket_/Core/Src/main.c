/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "fonts.h"
#include "keypad_3x4_quang.h"
#include "ssd1306.h"
#include "as608.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PASSWORD "1234"
#define PASSWORD_LEN 4
#define MAX_INPUT 16
#define MAX_FAIL 3
#define LOCK_TIME_MS 5000
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
// ── Trạng thái hệ thống ──
typedef enum {
  STATE_MENU,           // Menu chọn chế độ
  STATE_INPUT_PIN,      // Đang nhập mật khẩu
  STATE_INPUT_OTP,      // Đang nhập mã OTP
  STATE_WAIT_FINGER,    // Chờ quét vân tay (Lớp 2 tại chỗ)
  STATE_WAIT_FACE,      // Chờ xác thực khuôn mặt từ web (mở từ xa)
  STATE_WAIT_OTP,       // Chờ server xác minh OTP
  STATE_VAULT_OPEN,     // Két đang mở
  STATE_LOCKED_TEMP,    // Tạm khóa do nhập sai 3 lần
  STATE_ENROLL_FINGER,  // Đang đăng ký vân tay mới
} SystemState_t;

SystemState_t sysState = STATE_MENU;

// ── OTP ──
char otpBuffer[8];             // Buffer nhập OTP
uint8_t otpIndex = 0;

char pinBuffer[MAX_INPUT + 1]; // Lưu mã PIN đang nhập
uint8_t pinIndex = 0;          // Vị trí ký tự hiện tại
uint8_t failCount = 0;         // Số lần nhập sai
uint32_t lastActionTime = 0;   // Timestamp cho timeout
uint8_t fingerFailCount = 0;   // Số lần quét vân tay sai

// ── Đăng ký vân tay ──
uint16_t enrollId = 0;         // ID vân tay đang đăng ký
uint8_t enrollStep = 0;        // Bước: 0=chờ lần1, 1=chờ nhấc tay, 2=chờ lần2

// ── Cấu hình Layer 2 ──
uint32_t layer2_enabled = 1;   // Trạng thái bật/tắt (1=Bật, 0=Tắt)
uint32_t lastPB0Press = 0;     // Debounce cho nút PB0

// ── Chế độ ngủ (Sleep / STOP Mode) ──
#define SLEEP_TIMEOUT_MS  15000  // 15 giây không thao tác → đi ngủ
volatile uint8_t isSleeping = 0;       // 1 = đang ngủ
volatile uint8_t wakeupSource = 0;     // 0=none, 1=touch, 2=uart

#define FLASH_USER_PAGE_ADDR   ((uint32_t)0x0800FC00) /* Page 63 */

uint32_t Read_Layer2_Status(void) {
    return *(__IO uint32_t*)FLASH_USER_PAGE_ADDR;
}

void Write_Layer2_Status(uint32_t status) {
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError = 0;
    EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.PageAddress = FLASH_USER_PAGE_ADDR;
    EraseInitStruct.NbPages     = 1;
    HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_USER_PAGE_ADDR, status);
    HAL_FLASH_Lock();
}

// ── UART nhận từ ESP32 ──
uint8_t uartRxByte;         // Nhận 1 byte
char uartRxBuffer[64];      // Buffer nhận dữ liệu ngắt
char uartProcessBuffer[64]; // Buffer để xử lý (tránh bị ghi đè)
uint8_t uartRxIndex = 0;
volatile uint8_t uartCmdReady = 0;   // Cờ có lệnh mới
volatile uint32_t uartByteCount = 0; // DEBUG: Đếm số byte nhận được

// ── Keypad cấu hình ──
Keypad_Cfg_t myKeypad;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
void OLED_ShowText(const char *line1, const char *line2, const char *line3);
void OLED_ShowPinEntry(void);
void OLED_ShowMenu(void);
void OLED_ShowOTPEntry(void);
void UART_SendString(const char *str);
void ProcessUARTCommand(void);
void Relay_Open(void);
void Relay_Close(void);
void ResetToMenu(void);
void EnterSleepMode(void);
void WakeupFromSleep(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// ── Hiển thị OLED (3 dòng) ──
void OLED_ShowText(const char *line1, const char *line2, const char *line3) {
  ssd1306_Fill(Black);
  if (line1) {
    ssd1306_SetCursor(0, 0);
    ssd1306_WriteString(line1, Font_7x10, White);
  }
  if (line2) {
    ssd1306_SetCursor(0, 20);
    ssd1306_WriteString(line2, Font_7x10, White);
  }
  if (line3) {
    ssd1306_SetCursor(0, 40);
    ssd1306_WriteString(line3, Font_7x10, White);
  }
  ssd1306_UpdateScreen(&hi2c1);
}

// ── Hiển thị PIN đang nhập (mã hóa *) ──
void OLED_ShowPinEntry(void) {
  char masked[MAX_INPUT + 1];
  for (uint8_t i = 0; i < pinIndex; i++) {
    masked[i] = '*';
  }
  masked[pinIndex] = '\0';

  char line2[24];
  snprintf(line2, sizeof(line2), "PIN: %s", masked);

  char line3[24];
  snprintf(line3, sizeof(line3), "Sai: %d/%d", failCount, MAX_FAIL);

  OLED_ShowText("== NHAP MAT KHAU ==", line2, line3);
}

// ── Hiển thị Menu chính ──
void OLED_ShowMenu(void) {
  OLED_ShowText("== SAFEVAULT ==", "1. Nhap mat khau", "2. Nhap ma OTP");
}

// ── Hiển thị OTP đang nhập ──
void OLED_ShowOTPEntry(void) {
  char masked[8];
  for (uint8_t i = 0; i < otpIndex; i++) masked[i] = '*';
  masked[otpIndex] = '\0';

  char line2[20];
  snprintf(line2, sizeof(line2), "OTP: %s", masked);
  OLED_ShowText("== NHAP MA OTP ==", line2, "Nhan # de xac nhan");
}

// ── Reset về menu chính ──
void ResetToMenu(void) {
  sysState = STATE_MENU;
  pinIndex = 0;
  otpIndex = 0;
  failCount = 0;
  lastActionTime = HAL_GetTick();
  memset(pinBuffer, 0, sizeof(pinBuffer));
  memset(otpBuffer, 0, sizeof(otpBuffer));
  OLED_ShowMenu();
}

// ── Gửi chuỗi qua UART đến ESP32 ──
void UART_SendString(const char *str) {
  HAL_UART_Transmit(&huart1, (uint8_t *)str, strlen(str), 200);
}

// ── Điều khiển Relay (PC13) ──
// Hầu hết module relay phổ biến là Active-LOW:
//   LOW  (RESET) = Relay BẬT (mở chốt)
//   HIGH (SET)   = Relay TẮT (đóng chốt)
void Relay_Open(void) { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET); }

void Relay_Close(void) { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); }

// ── UART Rx Complete callback ──
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    if (uartRxByte == '\n' || uartRxByte == '\r') {
      if (uartRxIndex > 0) {
        uartRxBuffer[uartRxIndex] = '\0';
        strcpy(uartProcessBuffer, uartRxBuffer);
        uartCmdReady = 1;
        uartRxIndex = 0;
      }
    } else {
      if (uartRxIndex < sizeof(uartRxBuffer) - 1) {
        uartRxBuffer[uartRxIndex++] = (char)uartRxByte;
      }
      uartByteCount++;
    }
    HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);
  }
}

// ── Xử lý lỗi UART (Overrun, Noise, Framing) ──
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);
  }
}

// ── Xử lý lệnh nhận từ ESP32 ──
void ProcessUARTCommand(void) {
  char *unlockPtr = strstr(uartProcessBuffer, "UNLOCK:");
  if (unlockPtr != NULL) {
    char *memberName = unlockPtr + 7;
    sysState = STATE_VAULT_OPEN;
    lastActionTime = HAL_GetTick();
    Relay_Open();

    char line2[22];
    snprintf(line2, sizeof(line2), "Nguoi: %s", memberName);
    OLED_ShowText("== KET DANG MO ==", line2, "Trang thai: OPEN");
    UART_SendString("ACK:UNLOCKED\n");

  } else if (strstr(uartProcessBuffer, "TEMP_LOCK") != NULL) {
    sysState = STATE_LOCKED_TEMP;
    lastActionTime = HAL_GetTick();
    OLED_ShowText("!! CANH BAO !!", "Tam khoa 5 giay", "Sai qua nhieu lan");
    HAL_Delay(LOCK_TIME_MS);
    ResetToMenu();

  } else if (strstr(uartProcessBuffer, "LOCK") != NULL) {
    Relay_Close();
    OLED_ShowText("== KET DA KHOA ==", "Trang thai: LOCKED", "");
    HAL_Delay(1500);
    ResetToMenu();
    UART_SendString("ACK:LOCKED\n");

  } else if (strstr(uartProcessBuffer, "FAIL") != NULL) {
    OLED_ShowText("== XAC THUC ==", "THAT BAI", "Thu lai...");
    HAL_Delay(2000);
    if (sysState == STATE_WAIT_FACE) {
      OLED_ShowText("== CHO XAC THUC ==", "Quet khuon mat",
                    "tren dien thoai...");
    }

  } else if (strstr(uartProcessBuffer, "ENROLL_START:") != NULL) {
    // Lệnh đăng ký vân tay mới từ Web
    char *idPtr = strstr(uartProcessBuffer, "ENROLL_START:") + 13;
    enrollId = (uint16_t)atoi(idPtr);
    enrollStep = 0;
    sysState = STATE_ENROLL_FINGER;
    lastActionTime = HAL_GetTick();
    char msg[24];
    snprintf(msg, sizeof(msg), "Slot ID: %d", enrollId);
    OLED_ShowText("== DANG KY VAN TAY ==", msg, "Dat ngon tay len...");
  }

  uartCmdReady = 0;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */

  // ── Khởi tạo OLED ──
  ssd1306_Init(&hi2c1);
  OLED_ShowText("== SafeVault ==", "Khoi dong...", "v3.0");
  HAL_Delay(1000);

  // ── Khởi tạo cảm biến vân tay AS608 ──
  if (AS608_Init(&huart3) == AS608_OK) {
    OLED_ShowText("AS608", "Ket noi OK!", "");
  } else {
    OLED_ShowText("AS608", "LOI KET NOI!", "Kiem tra day");
  }
  HAL_Delay(1000);

  // ── Đọc cấu hình Layer 2 từ Flash ──
  layer2_enabled = Read_Layer2_Status();
  if (layer2_enabled != 0 && layer2_enabled != 1) {
    // Chưa từng lưu (giá trị 0xFFFFFFFF), đặt mặc định là Bật (1)
    layer2_enabled = 1;
    Write_Layer2_Status(layer2_enabled);
  }

  // ── Cấu hình Keypad 4x3 ──
  // Rows: PA0, PA1, PA2, PA3 (Output)
  // Cols: PA4, PA5, PA6 (Input with Pull-up)
  myKeypad.R_Port[0] = GPIOA;
  myKeypad.R_Pin[0] = GPIO_PIN_0;
  myKeypad.R_Port[1] = GPIOA;
  myKeypad.R_Pin[1] = GPIO_PIN_1;
  myKeypad.R_Port[2] = GPIOA;
  myKeypad.R_Pin[2] = GPIO_PIN_2;
  myKeypad.R_Port[3] = GPIOA;
  myKeypad.R_Pin[3] = GPIO_PIN_3;

  myKeypad.C_Port[0] = GPIOA;
  myKeypad.C_Pin[0] = GPIO_PIN_4;
  myKeypad.C_Port[1] = GPIOA;
  myKeypad.C_Pin[1] = GPIO_PIN_5;
  myKeypad.C_Port[2] = GPIOA;
  myKeypad.C_Pin[2] = GPIO_PIN_6;

  // ── Cấu hình lại GPIO cho Keypad ──
  // Rows → Output Push-Pull
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // Cols → Input Pull-Up
  GPIO_InitStruct.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // ── Relay ban đầu: khóa ──
  Relay_Close();

  // ── Bắt đầu nhận UART interrupt ──
  HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);

  // ── Trạng thái ban đầu: Menu ──
  sysState = STATE_MENU;
  pinIndex = 0;
  otpIndex = 0;
  failCount = 0;
  uartCmdReady = 0;
  memset(pinBuffer, 0, sizeof(pinBuffer));
  memset(otpBuffer, 0, sizeof(otpBuffer));
  memset(uartProcessBuffer, 0, sizeof(uartProcessBuffer));
  OLED_ShowMenu();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    // ══════════════════════════════════════
    //  KIỂM TRA AUTO-SLEEP (15s không thao tác)
    // ══════════════════════════════════════
    if (!isSleeping && sysState == STATE_MENU) {
      if (HAL_GetTick() - lastActionTime > SLEEP_TIMEOUT_MS) {
        EnterSleepMode();
        // === CPU thức dậy tại đây sau khi có ngắt ===
        WakeupFromSleep();
        continue;
      }
    }

    // ══════════════════════════════════════
    //  TỰ ĐỘNG TẮT CÒI (SAU 5 GIÂY)
    // ══════════════════════════════════════
    if (HAL_GPIO_ReadPin(BUZZER_GPIO_Port, BUZZER_Pin) == GPIO_PIN_SET) {
      if (HAL_GetTick() - lastActionTime > 5000) {
        HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
      }
    }

    // ══════════════════════════════════════
    //  NÚT BẤM PB0: BẬT/TẮT LAYER 2
    // ══════════════════════════════════════
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_RESET) {
      if (HAL_GetTick() - lastPB0Press > 500) {
        layer2_enabled = !layer2_enabled;
        Write_Layer2_Status(layer2_enabled);

        if (layer2_enabled) {
          OLED_ShowText("== CAU HINH ==", "Lop 2: BAT", "(PIN + Van tay)");
        } else {
          OLED_ShowText("== CAU HINH ==", "Lop 2: TAT", "(Chi dung PIN)");
        }
        HAL_Delay(1500);

        lastActionTime = HAL_GetTick();
        if (sysState == STATE_MENU) OLED_ShowMenu();
        else if (sysState == STATE_INPUT_PIN) OLED_ShowPinEntry();
        else if (sysState == STATE_INPUT_OTP) OLED_ShowOTPEntry();
      }
      lastPB0Press = HAL_GetTick();
    }

    // ══════════════════════════════════════
    //  XỬ LÝ LỆNH UART TỪ ESP32 
    // ══════════════════════════════════════
    if (uartCmdReady) {
      ProcessUARTCommand();
    }

    // Tự động phục hồi ngắt UART
    if (huart1.RxState == HAL_UART_STATE_READY) {
      HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);
    }

    // ══════════════════════════════════════
    //  MENU CHÍNH
    // ══════════════════════════════════════
    if (sysState == STATE_MENU) {
      char key = Keypad_Read(&myKeypad);
      if (key == '1') {
        sysState = STATE_INPUT_PIN;
        lastActionTime = HAL_GetTick();
        OLED_ShowPinEntry();
      } else if (key == '2') {
        sysState = STATE_INPUT_OTP;
        lastActionTime = HAL_GetTick();
        OLED_ShowOTPEntry();
      }

    // ══════════════════════════════════════
    //  NHẬP MẬT KHẨU (Stealth PIN)
    // ══════════════════════════════════════
    } else if (sysState == STATE_INPUT_PIN) {
      // Timeout 10s không nhập gì → về menu
      if (pinIndex > 0 && (HAL_GetTick() - lastActionTime > 10000)) {
        ResetToMenu();
      }

      char key = Keypad_Read(&myKeypad);
      if (key != 0) {
        lastActionTime = HAL_GetTick();

        if (key == '*') {
          if (pinIndex > 0) {
            pinIndex--;
            pinBuffer[pinIndex] = '\0';
          } else {
            // Nhấn * khi trống → về menu
            ResetToMenu();
          }
          if (sysState == STATE_INPUT_PIN) OLED_ShowPinEntry();

        } else if (key == '#') {
          if (pinIndex == 0) {
            OLED_ShowText("== NHAP MAT KHAU ==", "Chua nhap PIN!", "Nhap roi nhan #");
            HAL_Delay(1000);
            OLED_ShowPinEntry();

          } else if (strstr(pinBuffer, PASSWORD) != NULL) {
            // ✅ Tìm thấy mật khẩu trong dãy (Stealth PIN)
            failCount = 0;
            pinIndex = 0;
            memset(pinBuffer, 0, sizeof(pinBuffer));

            if (layer2_enabled) {
                sysState = STATE_WAIT_FINGER;
                fingerFailCount = 0;
                lastActionTime = HAL_GetTick();

                OLED_ShowText("== PIN DUNG ==", "Chuyen Layer 2...", "Dat van tay len");
                HAL_Delay(500);
                OLED_ShowText("== QUET VAN TAY ==", "Dat ngon tay len", "cam bien...");
            } else {
                // 🔓 Bỏ qua Layer 2 (Bypass)
                OLED_ShowText("== MO KHOA ==", "Lop 2 da tat", "(Bypass L2)");
                UART_SendString("UNLOCK_BYPASS\n");
                Relay_Open();
                sysState = STATE_VAULT_OPEN;
                lastActionTime = HAL_GetTick();
            }

          } else {
            // ❌ Sai mật khẩu
            failCount++;
            pinIndex = 0;
            memset(pinBuffer, 0, sizeof(pinBuffer));

            if (failCount >= MAX_FAIL) {
              sysState = STATE_LOCKED_TEMP;
              OLED_ShowText("!! TAM KHOA !!", "Sai 3 lan lien tiep", "Cho 5 giay...");
              UART_SendString("LOCKED_TEMP\n");
              HAL_Delay(LOCK_TIME_MS);
              ResetToMenu();
            } else {
              char msg[24];
              snprintf(msg, sizeof(msg), "Sai! Con %d lan", MAX_FAIL - failCount);
              OLED_ShowText("== SAI MAT KHAU ==", msg, "Thu lai...");
              HAL_Delay(1500);
              OLED_ShowPinEntry();
            }
          }

        } else {
          // Ký tự số → thêm vào buffer
          if (pinIndex < MAX_INPUT) {
            pinBuffer[pinIndex++] = key;
            pinBuffer[pinIndex] = '\0';
            OLED_ShowPinEntry();
          }
        }
      }

    // ══════════════════════════════════════
    //  NHẬP MÃ OTP
    // ══════════════════════════════════════
    } else if (sysState == STATE_INPUT_OTP) {
      if (otpIndex > 0 && (HAL_GetTick() - lastActionTime > 10000)) {
        ResetToMenu();
      }

      char key = Keypad_Read(&myKeypad);
      if (key != 0) {
        lastActionTime = HAL_GetTick();

        if (key == '*') {
          if (otpIndex > 0) {
            otpIndex--;
            otpBuffer[otpIndex] = '\0';
          } else {
            ResetToMenu();
          }
          if (sysState == STATE_INPUT_OTP) OLED_ShowOTPEntry();

        } else if (key == '#') {
          if (otpIndex == 0) {
            OLED_ShowText("== NHAP MA OTP ==", "Chua nhap ma!", "Nhap roi nhan #");
            HAL_Delay(1000);
            OLED_ShowOTPEntry();
          } else {
            // Gửi OTP lên ESP32 để server xác minh
            char cmd[16];
            snprintf(cmd, sizeof(cmd), "OTP:%s\n", otpBuffer);
            UART_SendString(cmd);

            sysState = STATE_WAIT_OTP;
            lastActionTime = HAL_GetTick();
            OLED_ShowText("== XAC MINH OTP ==", "Dang kiem tra...", "Vui long cho");

            otpIndex = 0;
            memset(otpBuffer, 0, sizeof(otpBuffer));
          }

        } else {
          if (otpIndex < 6) {
            otpBuffer[otpIndex++] = key;
            otpBuffer[otpIndex] = '\0';
            OLED_ShowOTPEntry();
          }
        }
      }

    // ══════════════════════════════════════
    //  CHỜ SERVER XÁC MINH OTP
    // ══════════════════════════════════════
    } else if (sysState == STATE_WAIT_OTP) {
      if (HAL_GetTick() - lastActionTime > 10000) {
        OLED_ShowText("== HET HAN ==", "Server khong phan hoi", "");
        HAL_Delay(2000);
        ResetToMenu();
      }

    // ══════════════════════════════════════
    //  CHờ QUÉT VÂN TAY (LAYER 2 TẠI CHỖ)
    // ══════════════════════════════════════
    } else if (sysState == STATE_WAIT_FINGER) {
      // Timeout 15s
      if (HAL_GetTick() - lastActionTime > 15000) {
        OLED_ShowText("== HET HAN ==", "Khong quet van tay", "Vui long thu lai");
        HAL_Delay(2000);
        ResetToMenu();
      } else {
        // Thử đọc vân tay
        uint8_t imgRet = AS608_GetImage();
        if (imgRet == AS608_OK) {
          // Có ngón tay, tạo đặc trưng và tìm kiếm
          if (AS608_GenChar(1) == AS608_OK) {
            AS608_SearchResult_t result;
            uint8_t searchRet = AS608_Search(&result);
            if (searchRet == AS608_OK && result.status == AS608_OK) {
              // ✅ Vân tay khớp
              char line2[22];
              snprintf(line2, sizeof(line2), "ID:%d (%d%%)", result.finger_id, result.confidence / 100);
              OLED_ShowText("== MO KHOA ==", line2, "Van tay hop le!");
              Relay_Open();
              sysState = STATE_VAULT_OPEN;
              lastActionTime = HAL_GetTick();

              // Thông báo ESP32
              char cmd[32];
              snprintf(cmd, sizeof(cmd), "FINGER_OK:%d\n", result.finger_id);
              UART_SendString(cmd);
            } else {
              // ❌ Vân tay không khớp
              fingerFailCount++;
              if (fingerFailCount >= 3) {
                OLED_ShowText("!! THAT BAI !!", "3 lan sai lien tiep", "Quay ve menu");
                UART_SendString("FINGER_FAIL\n");
                HAL_Delay(2000);
                ResetToMenu();
              } else {
                char msg[24];
                snprintf(msg, sizeof(msg), "Con %d lan thu", 3 - fingerFailCount);
                OLED_ShowText("Van tay SAI!", msg, "Thu lai...");
                HAL_Delay(1500);
                OLED_ShowText("== QUET VAN TAY ==", "Dat ngon tay len", "cam bien...");
              }
            }
          } else {
            OLED_ShowText("Anh khong ro!", "Thu lai...", "");
            HAL_Delay(1000);
            OLED_ShowText("== QUET VAN TAY ==", "Dat ngon tay len", "cam bien...");
          }
        }
        // Nếu AS608_NO_FINGER → chưa đặt ngón tay, tiếp tục lặp
      }

    // ══════════════════════════════════════
    //  CHờ QUÉT MẶT TỪ XA (Web)
    // ══════════════════════════════════════
    } else if (sysState == STATE_WAIT_FACE) {
      if (HAL_GetTick() - lastActionTime > 15000) {
        OLED_ShowText("== HET HAN ==", "Qua 15s khong quet", "Vui long thu lai");
        HAL_Delay(2000);
        ResetToMenu();
      }

    // ══════════════════════════════════════
    //  KÉT ĐANG MỞ (Auto-lock 5s)
    // ══════════════════════════════════════
    } else if (sysState == STATE_VAULT_OPEN) {
      if (HAL_GetTick() - lastActionTime > 5000) {
        Relay_Close();
        OLED_ShowText("== KET DA KHOA ==", "Tu dong khoa", "");
        HAL_Delay(1500);
        ResetToMenu();
        UART_SendString("ACK:LOCKED\n");
      }

    // ══════════════════════════════════════
    //  ĐĂNG KÝ VÂN TAY MỚI (từ Web)
    // ══════════════════════════════════════
    } else if (sysState == STATE_ENROLL_FINGER) {
      // Timeout 30s
      if (HAL_GetTick() - lastActionTime > 30000) {
        OLED_ShowText("== HET HAN ==", "Dang ky that bai", "Timeout");
        UART_SendString("ENROLL_FAIL:TIMEOUT\n");
        HAL_Delay(2000);
        ResetToMenu();
      } else {
        if (enrollStep == 0) {
          // Bước 1: Thu ảnh vân tay lần 1
          uint8_t ret = AS608_GetImage();
          if (ret == AS608_OK) {
            if (AS608_GenChar(1) == AS608_OK) {
              enrollStep = 1;
              lastActionTime = HAL_GetTick();
              OLED_ShowText("== LAN 1 OK ==", "Nhac ngon tay ra", "roi dat lai...");
            } else {
              OLED_ShowText("Anh khong ro!", "Thu lai...", "");
              HAL_Delay(1000);
              OLED_ShowText("== DANG KY ==", "Dat ngon tay len", "cam bien...");
            }
          }
        } else if (enrollStep == 1) {
          // Bước 2: Chờ nhấc ngón tay ra
          if (AS608_GetImage() == AS608_NO_FINGER) {
            enrollStep = 2;
            lastActionTime = HAL_GetTick();
            OLED_ShowText("== LAN 2 ==", "Dat lai ngon tay", "len cam bien...");
          }
        } else if (enrollStep == 2) {
          // Bước 3: Thu ảnh vân tay lần 2
          uint8_t ret = AS608_GetImage();
          if (ret == AS608_OK) {
            if (AS608_GenChar(2) == AS608_OK) {
              // Ghép 2 mẫu thành template
              if (AS608_RegModel() == AS608_OK) {
                // Lưu vào slot
                if (AS608_Store(enrollId) == AS608_OK) {
                  char msg[24];
                  snprintf(msg, sizeof(msg), "ID: %d", enrollId);
                  OLED_ShowText("== THANH CONG ==", msg, "Da luu van tay!");
                  char cmd[32];
                  snprintf(cmd, sizeof(cmd), "ENROLL_OK:%d\n", enrollId);
                  UART_SendString(cmd);
                  HAL_Delay(2000);
                  ResetToMenu();
                } else {
                  OLED_ShowText("LOI!", "Khong luu duoc", "Thu lai...");
                  UART_SendString("ENROLL_FAIL:STORE\n");
                  HAL_Delay(2000);
                  ResetToMenu();
                }
              } else {
                OLED_ShowText("LOI!", "2 mau khong khop", "Thu lai tu dau");
                UART_SendString("ENROLL_FAIL:MISMATCH\n");
                HAL_Delay(2000);
                ResetToMenu();
              }
            } else {
              OLED_ShowText("Anh khong ro!", "Thu lai...", "");
              HAL_Delay(1000);
              OLED_ShowText("== LAN 2 ==", "Dat lai ngon tay", "len cam bien...");
            }
          }
        }
      }
    }

    HAL_Delay(50);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 57600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(relay_GPIO_Port, relay_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : relay_Pin */
  GPIO_InitStruct.Pin = relay_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(relay_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : key_Pin keyA1_Pin keyA2_Pin keyA3_Pin
                           keyA4_Pin keyA5_Pin keyA6_Pin */
  GPIO_InitStruct.Pin = key_Pin|keyA1_Pin|keyA2_Pin|keyA3_Pin
                          |keyA4_Pin|keyA5_Pin|keyA6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : touch_Pin */
  GPIO_InitStruct.Pin = touch_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(touch_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : button_Pin */
  GPIO_InitStruct.Pin = button_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(button_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SW420_Pin */
  GPIO_InitStruct.Pin = SW420_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SW420_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BUZZER_Pin */
  GPIO_InitStruct.Pin = BUZZER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BUZZER_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  // Override CubeMX: Keypad Rows PA0-PA3 = Output Push-Pull
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // Override CubeMX: Keypad Cols PA4-PA6 = Input Pull-Up
  GPIO_InitStruct.Pin = GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // Nút bấm PB0: Bật/Tắt Layer 2 (Input Pull-Up, nhấn = LOW)
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

// ══════════════════════════════════════
//  CHẾ ĐỘ NGỦ (STOP MODE)
// ══════════════════════════════════════
void EnterSleepMode(void) {
  isSleeping = 1;
  wakeupSource = 0;

  // 1. Tắt màn hình OLED
  ssd1306_Fill(Black);
  ssd1306_UpdateScreen(&hi2c1);
  ssd1306_SetDisplayOn(&hi2c1, 0);  // Display OFF

  // 2. Đảm bảo Relay đang khóa
  Relay_Close();

  // 3. Cấu hình PA10 (UART1_RX) thành ngắt EXTI để đánh thức từ ESP32
  //    (UART start bit = falling edge trên RX)
  HAL_UART_DeInit(&huart1);  // Tắt UART1 tạm thời
  GPIO_InitTypeDef gpioInit = {0};
  gpioInit.Pin = GPIO_PIN_10;
  gpioInit.Mode = GPIO_MODE_IT_FALLING;  // Start bit UART = falling edge
  gpioInit.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpioInit);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  // 4. Thông báo ESP32 rằng STM32 đang ngủ
  // (UART đã tắt nên không gửi được, ESP32 sẽ tự detect qua heartbeat)

  // 5. Đi vào STOP Mode - CPU dừng hoàn toàn tại đây
  HAL_SuspendTick();
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
  // ──── CPU DỪNG TẠI ĐÂY cho đến khi có ngắt EXTI ────
  HAL_ResumeTick();
}

void WakeupFromSleep(void) {
  // 1. Khôi phục xung nhịp hệ thống (STOP mode tắt HSE/PLL)
  SystemClock_Config();

  // 2. Tắt ngắt EXTI trên PA10, xóa pending bit, khôi phục UART1
  HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
  __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_10);  // Xóa EXTI pending bit
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_10);    // DeInit PA10 khỏi mode EXTI
  MX_USART1_UART_Init();                  // Reinit UART1 (PA9 TX + PA10 RX)
  HAL_Delay(10);                          // Chờ UART ổn định
  HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);

  // 3. Bật lại màn hình OLED
  ssd1306_SetDisplayOn(&hi2c1, 1);  // Display ON

  // 4. Reset trạng thái
  isSleeping = 0;
  lastActionTime = HAL_GetTick();
  sysState = STATE_MENU;
  pinIndex = 0;
  otpIndex = 0;
  memset(pinBuffer, 0, sizeof(pinBuffer));
  memset(otpBuffer, 0, sizeof(otpBuffer));

  // Reset UART buffers để đảm bảo không dính rác lúc thức dậy
  uartRxIndex = 0;
  uartCmdReady = 0;
  memset(uartRxBuffer, 0, sizeof(uartRxBuffer));
  memset(uartProcessBuffer, 0, sizeof(uartProcessBuffer));

  // 5. Hiển thị Menu
  OLED_ShowMenu();

  // 6. Gửi ACK:WAKEUP cho ESP32
  UART_SendString("ACK:WAKEUP\n");

  // 7. Nếu thức dậy do ESP32 (UART), chờ lệnh gửi lại và xử lý ngay
  if (wakeupSource == 2) {
    OLED_ShowText("== DANG XU LY ==", "Lenh tu xa...", "");
    uint32_t waitStart = HAL_GetTick();
    while (HAL_GetTick() - waitStart < 2000) {  // Chờ tối đa 2 giây
      // Khôi phục UART RX nếu bị lỗi
      if (huart1.RxState == HAL_UART_STATE_READY) {
        HAL_UART_Receive_IT(&huart1, &uartRxByte, 1);
      }
      if (uartCmdReady) {
        ProcessUARTCommand();
        break;  // Đã nhận và xử lý lệnh → thoát vòng chờ
      }
      HAL_Delay(10);
    }
  }
}

// ══════════════════════════════════════
//  EXTI CALLBACK (Cảm biến chạm + UART wakeup + RUNG)
// ══════════════════════════════════════
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
  if (GPIO_Pin == touch_Pin) {
    // Cảm biến chạm PA7 kích hoạt
    wakeupSource = 1;
    lastActionTime = HAL_GetTick();
  }
  else if (GPIO_Pin == GPIO_PIN_10) {
    // PA10 (UART1 RX) - ESP32 gửi dữ liệu → đánh thức
    wakeupSource = 2;
    lastActionTime = HAL_GetTick();
  }
  else if (GPIO_Pin == SW420_Pin) {
    // Cảm biến rung kích hoạt
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
    UART_SendString("WARNING:VIBRATION\n");
    wakeupSource = 3;
    lastActionTime = HAL_GetTick();
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
