#ifndef __KEYPAD_H
#define __KEYPAD_H

#include "main.h"
typedef struct{
	GPIO_TypeDef* R_Port[4];
	uint16_t R_Pin[4];
	GPIO_TypeDef* C_Port[3];
	uint16_t C_Pin[3];
}Keypad_Cfg_t;

char Keypad_Read(Keypad_Cfg_t *Keypad);

#endif