#include "encoder.h"
#include "main.h"
#include "tim.h"
#include "stm32_hal_legacy.h"

extern TIM_HandleTypeDef htim1;

int16_t encoder_get_value(void)
{
	return __HAL_TIM_GET_COUNTER(&htim1);
}




