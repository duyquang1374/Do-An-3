#include "keypad_3x4_quang.h"

char key_map[4][3] = {
				  {'1', '2', '3'},
	        {'4', '5', '6'},
				  {'7', '8', '9'},
				  {'*', '0', '#'}
				  };
char Keypad_Read(Keypad_Cfg_t *Keypad){
	for(int r=0; r<4; r++){
		for(int i=0; i<4; i++){
			HAL_GPIO_WritePin(Keypad->R_Port[i], Keypad->R_Pin[i], GPIO_PIN_SET);
		}
		HAL_GPIO_WritePin(Keypad->R_Port[r], Keypad->R_Pin[r], GPIO_PIN_RESET);
		
	for(int c=0; c<3; c++){	
		if(HAL_GPIO_ReadPin(Keypad->C_Port[c],Keypad->C_Pin[c])==GPIO_PIN_RESET){
			HAL_Delay(20);
			while(HAL_GPIO_ReadPin(Keypad->C_Port[c], Keypad->C_Pin[c])==GPIO_PIN_RESET);
			return key_map[r][c];
			}
		}
	}
	return 0;
}