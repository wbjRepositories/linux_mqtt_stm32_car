#include "task_oled.h"
#include "OLED.h"
#include "encoder.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <stdio.h>

extern TaskHandle_t OLED_Task_Handle;
extern car_status_t car_status;
extern SemaphoreHandle_t mqtt_resond;

void Task_OLED( void * argv)
{
	uint8_t new = encoder_get_value() / 4;
	uint8_t old = new;
	uint8_t select = 0;
	uint32_t ulNotifiedValue;
	uint8_t primary_menu;				//一级菜单
	uint8_t new_speed_max = 0;			//新设置的最大速度
	int8_t speed_increase = 0;	//最大速度增加值
	
	
	OLED_Init();
	OLED_ShowString(0,0,"test1", OLED_8X16);
	OLED_ShowString(0,16,"test2", OLED_8X16);
	OLED_ShowString(0,32,"test3", OLED_8X16);
	OLED_ShowString(0,48,"test4", OLED_8X16);
	OLED_Update();
	while(1)
	{
		const TickType_t xBlockTime = pdMS_TO_TICKS(0);
		//ulNotifiedValue = ulTaskNotifyTake(pdTRUE, xBlockTime);
		
		uint32_t notifyCount = 0;
		xTaskNotifyAndQuery(OLED_Task_Handle, 0, eNoAction,  &notifyCount);
		OLED_Clear();
		
		
		if(notifyCount == 0)
		{
			new = encoder_get_value() / 4;
			//printf("%d\n",new);
			if(new - old >= 1)
			{
				select++;
			}
			else if(old - new >= 1)
			{
				select = select + 3;
			}
			old = new;
			primary_menu  = select % 4;	
			OLED_ShowString(0,0,"行进速度调节    ", OLED_8X16);
			OLED_ShowString(0,16,"转向速度调节    ", OLED_8X16);
			OLED_ShowString(0,32,"自动前进        ", OLED_8X16);
			OLED_ShowString(0,48,"test4           ", OLED_8X16);
			OLED_ReverseArea(0, 16 * primary_menu, 128, 16);
		}
		else if(notifyCount == 1)
		{
			switch(primary_menu){
				case 0:
					new = encoder_get_value() / 4;
					if(new - old >= 1)
					{
						speed_increase = speed_increase + 4;
					}
					else if(old - new >= 1)
					{
						speed_increase = speed_increase - 4;
					}
					old = new;
					new_speed_max = car_status.speed_max + speed_increase;
					OLED_DrawLine(0, 32, 127, 32);
					if(new_speed_max <= 0)
					{
						new_speed_max = 0;
					}
					else if(new_speed_max > 127)
					{
						new_speed_max = 127;
					}
					OLED_DrawCircle(new_speed_max, 32, 4, OLED_FILLED);
					break;
				case 1:
					//·····
					break;
				case 2:
					break;
				case 3:
					break;
			}
		}
		else if(notifyCount == 2)
		{
			speed_increase = 0;
			car_status.speed_max = new_speed_max;
			ulNotifiedValue = ulTaskNotifyTake(pdTRUE, xBlockTime);
			xSemaphoreGive(mqtt_resond);
		}
		
		OLED_Update();
		vTaskDelay(100);
		
		
		
	}
}


