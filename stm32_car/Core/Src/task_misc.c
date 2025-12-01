#include "task_misc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "tim.h"

extern QueueHandle_t Queue_car_recv;
extern QueueHandle_t Queue_car_send;

static car_status_t car_status_recv = {0};

//左轮前进
void car_left_advance(void)
{
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);
}
//右轮前进
void car_right_advance(void)
{
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
}
//左轮后退
void car_left_retreat(void)
{
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
}
//右轮后退
void car_right_retreat(void)
{
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
}
//小车开始移动
void car_move_start(void)
{
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
}
//小车停止移动
void car_move_stop(void)
{
	HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
	HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_2);
}

void car_move(car_status_t *car_status)
{
	uint32_t newSpeed = __HAL_TIM_GetAutoreload(&htim3) * car_status->speed_max / 127;
	__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed);
	__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed);
	
	uint32_t newDirctSpeed = __HAL_TIM_GetAutoreload(&htim3) * car_status->dirct_speed / 127;
	
	if(car_status->current_speed > 0)	//前进
	{
		car_left_advance();
		car_right_advance();
		
		if(car_status->direction > 0)	//左转
		{
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed + newDirctSpeed / 2);
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed - newDirctSpeed / 2);
			if(newSpeed <= newDirctSpeed)
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, 0);
			}
			if(newSpeed + newDirctSpeed >= __HAL_TIM_GetAutoreload(&htim3))
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed);
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed - newDirctSpeed);
				if(newSpeed <= newDirctSpeed * 2)
				{
					__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, 0);
				}
			}
		}
		else if(car_status->direction < 0)	//右转
		{
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed + newDirctSpeed / 2);
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed - newDirctSpeed / 2);
			if(newSpeed <= newDirctSpeed)
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, 0);
			}
			if(newSpeed + newDirctSpeed >= __HAL_TIM_GetAutoreload(&htim3))
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed);
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed - newDirctSpeed);
				if(newSpeed <= newDirctSpeed * 2)
				{
					__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, 0);
				}
			}
		}
		
		car_move_start();
	}
	else if(car_status->current_speed < 0)	//后退
	{
		car_left_retreat();
		car_right_retreat();
		
				if(car_status->direction > 0)	//左转
		{
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed + newDirctSpeed);
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed - newDirctSpeed);
			if(newSpeed <= newDirctSpeed)
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, 0);
			}
			if(newSpeed + newDirctSpeed >= __HAL_TIM_GetAutoreload(&htim3))
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed);
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed - newDirctSpeed * 2);
				if(newSpeed <= newDirctSpeed * 2)
				{
					__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, 0);
				}
			}
		}
		else if(car_status->direction < 0)	//右转
		{
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed + newDirctSpeed);
			__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed - newDirctSpeed);
			if(newSpeed <= newDirctSpeed)
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, 0);
			}
			if(newSpeed + newDirctSpeed >= __HAL_TIM_GetAutoreload(&htim3))
			{
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_1, newSpeed);
				__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, newSpeed - newDirctSpeed * 2);
				if(newSpeed <= newDirctSpeed * 2)
				{
					__HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_2, 0);
				}
			}
		}
		
		car_move_start();
	}
	else	//停止
	{
		car_move_stop();
	}
}




void Task_Misc( void * argv)
{
	
	
	while(1)
	{		
		xQueueReceive(Queue_car_recv, &car_status_recv, portMAX_DELAY);
		car_move(&car_status_recv);
		//vTaskDelay(100);
	}
}
