/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define relay_Pin GPIO_PIN_13
#define relay_GPIO_Port GPIOC
#define key_Pin GPIO_PIN_0
#define key_GPIO_Port GPIOA
#define keyA1_Pin GPIO_PIN_1
#define keyA1_GPIO_Port GPIOA
#define keyA2_Pin GPIO_PIN_2
#define keyA2_GPIO_Port GPIOA
#define keyA3_Pin GPIO_PIN_3
#define keyA3_GPIO_Port GPIOA
#define keyA4_Pin GPIO_PIN_4
#define keyA4_GPIO_Port GPIOA
#define keyA5_Pin GPIO_PIN_5
#define keyA5_GPIO_Port GPIOA
#define keyA6_Pin GPIO_PIN_6
#define keyA6_GPIO_Port GPIOA
#define touch_Pin GPIO_PIN_7
#define touch_GPIO_Port GPIOA
#define touch_EXTI_IRQn EXTI9_5_IRQn
#define button_Pin GPIO_PIN_0
#define button_GPIO_Port GPIOB
#define SW420_Pin GPIO_PIN_1
#define SW420_GPIO_Port GPIOB
#define SW420_EXTI_IRQn EXTI1_IRQn
#define BUZZER_Pin GPIO_PIN_12
#define BUZZER_GPIO_Port GPIOB
#define CTHT_Pin GPIO_PIN_13
#define CTHT_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
